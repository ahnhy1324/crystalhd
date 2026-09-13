// SPDX-License-Identifier: GPL-2.0-or-later
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef enum {
	BC_STS_SUCCESS, BC_STS_INV_ARG, BC_STS_ERR_USAGE,
	BC_STS_IO_ERROR, BC_STS_NO_DATA, BC_STS_IO_USER_ABORT
} BC_STATUS;
#define MAX_VALID_POLL_CNT 2
#define BCHP_INTR_INTR_STATUS 0
#define BC_EVENT_START_CAPTURE 6
#define BC_LINK_CAP_EN 1
#define BC_LINK_FMT_CHG 2
#define READ_ONCE(value) (value)
#define spin_lock_irqsave(lock, flags) ((void)(lock), (flags) = 0)
#define spin_unlock_irqrestore(lock, flags) ((void)(lock), (void)(flags))
#define dev_err(dev, ...) ((void)(dev))
#define dev_dbg(dev, ...) ((void)(dev))
#define printk(...) ((void)0)
#include "dma-stop-types.h"

struct device { int unused; };
struct pci_dev { int irq; };
struct crystalhd_adp { struct pci_dev *pdev; };
struct queue { unsigned int count; };
struct crystalhd_rx_dma_pkt { unsigned int pkt_tag; };
struct crystalhd_hw {
	struct crystalhd_adp *adp;
	unsigned int RxCaptureState, RxSeqNum, rx_list_post_index;
	enum list_sts rx_list_sts[2];
	int rx_lock, fetch_sem;
	bool dma_fault;
	struct queue *rx_actq, *rx_rdyq, *rx_freeq;
	uint32_t (*pfnReadDevRegister)(struct crystalhd_adp *, uint32_t);
	void (*pfnStopRXDMAEngines)(struct crystalhd_hw *);
	bool (*pfnNotifyHardware)(struct crystalhd_hw *, int);
};
struct crystalhd_cmd { struct crystalhd_hw *hw_ctx; unsigned int state; };
typedef struct {
	struct { union { struct { uint32_t bDiscardOnly; } FlushRxCap; } u; } udata;
} crystalhd_ioctl_data;

static unsigned int irq_depth, reads, clears, unmaps, stops, notifications, starts;
static uint32_t interrupt_bits;
static BC_STATUS start_result;
static bool notify_result;
static struct device device;

static struct device *chddev(void) { return &device; }
static void down(int *sem) { assert(!*sem); *sem = 1; }
static int down_interruptible(int *sem) { down(sem); return 0; }
static void up(int *sem) { assert(*sem == 1); *sem = 0; }
static void disable_irq(int irq) { (void)irq; irq_depth++; }
static void enable_irq(int irq) { (void)irq; assert(irq_depth); irq_depth--; }
static void msleep_interruptible(unsigned int delay) { (void)delay; }
static uint32_t read_register(struct crystalhd_adp *adp, uint32_t reg)
{
	(void)adp; (void)reg;
	assert(irq_depth);
	reads++;
	return interrupt_bits;
}
static void crystalhd_flea_clear_rx_errs_intrs(struct crystalhd_hw *hw)
{
	(void)hw; clears++;
}
static void crystalhd_hw_dma_fatal_stop(struct crystalhd_hw *hw)
{
	hw->dma_fault = true;
}
static struct crystalhd_rx_dma_pkt *crystalhd_dioq_fetch(struct queue *queue)
{
	static struct crystalhd_rx_dma_pkt packet;
	assert(queue);
	if (!queue->count)
		return NULL;
	queue->count--;
	return &packet;
}
static BC_STATUS crystalhd_dioq_add(struct queue *queue,
		struct crystalhd_rx_dma_pkt *packet, bool wake, unsigned int tag)
{
	(void)packet; (void)wake; (void)tag;
	queue->count++;
	return BC_STS_SUCCESS;
}
static void crystalhd_rx_pkt_rel_call_back(struct crystalhd_hw *hw, void *packet)
{
	(void)packet;
	assert(hw->fetch_sem == 1);
	assert(irq_depth);
	unmaps++;
}
static bool notify_hardware(struct crystalhd_hw *hw, int event)
{
	assert(hw->fetch_sem == 1);
	assert(event == BC_EVENT_START_CAPTURE);
	notifications++;
	return notify_result;
}
static BC_STATUS crystalhd_hw_start_capture(struct crystalhd_hw *hw)
{
	assert(hw->fetch_sem == 1);
	assert(!hw->rx_actq->count && !hw->rx_rdyq->count);
	assert(notifications);
	starts++;
	return start_result;
}
static void stop_success(struct crystalhd_hw *hw)
{ assert(hw->fetch_sem == 1); stops++; }
static void stop_failure(struct crystalhd_hw *hw)
{ assert(hw->fetch_sem == 1); hw->dma_fault = true; stops++; }
#include "dma-stop-functions.h"

int main(void)
{
	struct pci_dev pci = { .irq = 1 };
	struct crystalhd_adp adp = { .pdev = &pci };
	struct crystalhd_hw hw = { .adp = &adp,
		.pfnReadDevRegister = read_register,
		.pfnStopRXDMAEngines = stop_success,
		.pfnNotifyHardware = notify_hardware };
	struct queue active = {1}, ready = {1}, freeq = {0};
	struct crystalhd_cmd ctx = { .hw_ctx = &hw, .state = BC_LINK_CAP_EN };
	crystalhd_ioctl_data data = {0};
	union FLEA_INTR_BITS_COMMON done = { .WholeReg = 0 };

	/* A monitor-only close must not touch nonexistent DMA queues/IRQs. */
	assert(crystalhd_hw_stop_capture(&hw, true) == BC_STS_SUCCESS);
	assert(!irq_depth && !stops && !unmaps);
	irq_depth = 1;
	hw.rx_list_post_index = 1;
	crystalhd_flea_stop_rx_dma_engine(&hw);
	assert(!reads && !hw.rx_list_post_index);
	done.L0YRxDMADone = 1;
	done.L0UVRxDMADone = 1;
	done.L1YRxDMADone = 1;
	interrupt_bits = done.WholeReg;
	hw.rx_list_sts[0] = rx_sts_waiting;
	hw.rx_list_sts[1] = rx_waiting_y_intr;
	hw.rx_list_post_index = 1;
	crystalhd_flea_stop_rx_dma_engine(&hw);
	assert(!hw.rx_list_sts[0] && !hw.rx_list_sts[1]);
	assert(!hw.rx_list_post_index && !hw.dma_fault && clears == 1);
	/* A missing UV completion must fail without advertising a free list. */
	hw.rx_list_sts[0] = rx_sts_waiting;
	done.L0UVRxDMADone = 0;
	interrupt_bits = done.WholeReg;
	crystalhd_flea_stop_rx_dma_engine(&hw);
	assert(hw.dma_fault && hw.rx_list_sts[0] == rx_sts_waiting);
	irq_depth = 0;
	hw.dma_fault = false;
	hw.rx_actq = &active; hw.rx_rdyq = &ready; hw.rx_freeq = &freeq;
	data.udata.u.FlushRxCap.bDiscardOnly = 1;
	notify_result = true;
	start_result = BC_STS_NO_DATA;
	assert(bc_cproc_flush_cap_buffs(&ctx, &data) == BC_STS_SUCCESS);
	assert(!active.count && !ready.count && freeq.count == 2);
	assert(notifications == 1 && starts == 1 && !unmaps && !irq_depth);
	start_result = BC_STS_IO_ERROR;
	assert(bc_cproc_flush_cap_buffs(&ctx, &data) == BC_STS_IO_ERROR);
	notify_result = false;
	assert(bc_cproc_flush_cap_buffs(&ctx, &data) == BC_STS_IO_ERROR);
	assert(starts == 2 && !hw.fetch_sem && !irq_depth);
	notify_result = true;
	hw.pfnStopRXDMAEngines = stop_failure;
	active.count = 1;
	assert(bc_cproc_flush_cap_buffs(&ctx, &data) == BC_STS_IO_ERROR);
	assert(active.count == 1 && freeq.count == 2 && !unmaps && !irq_depth);
	assert(!hw.fetch_sem);
	data.udata.u.FlushRxCap.bDiscardOnly = 0;
	assert(bc_cproc_flush_cap_buffs(&ctx, &data) == BC_STS_IO_ERROR);
	assert(active.count == 1 && freeq.count == 2 && !unmaps && !irq_depth);
	/* A later successful teardown releases every retained registration. */
	hw.dma_fault = false;
	hw.pfnStopRXDMAEngines = stop_success;
	ctx.state = BC_LINK_CAP_EN;
	assert(bc_cproc_flush_cap_buffs(&ctx, &data) == BC_STS_SUCCESS);
	assert(!active.count && !ready.count && !freeq.count && unmaps == 3);
	assert(!ctx.state && !hw.fetch_sem && !irq_depth);
	puts("DMA stop/restart tests passed (ASan/UBSan)");
	return 0;
}
