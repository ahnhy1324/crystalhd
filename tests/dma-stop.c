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
#define BC_RX_LIST_CNT 16
#define ARRAY_SIZE(array) (sizeof(array) / sizeof((array)[0]))
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
struct crystalhd_rx_buffer {
	unsigned int id;
	bool released;
};
struct crystalhd_rx_dma_pkt {
	unsigned int pkt_tag;
	struct crystalhd_rx_buffer *buffer;
	struct crystalhd_rx_dma_pkt *next;
	bool detached;
};
struct crystalhd_dioq {
	struct crystalhd_rx_dma_pkt *items[BC_RX_LIST_CNT];
	unsigned int count;
};
struct crystalhd_hw {
	struct crystalhd_adp *adp;
	unsigned int RxCaptureState, RxSeqNum, rx_list_post_index;
	uint64_t rx_cancel_epoch;
	enum list_sts rx_list_sts[2];
	int rx_lock, fetch_sem;
	bool dma_fault;
	struct crystalhd_dioq *rx_actq, *rx_rdyq, *rx_freeq;
	struct crystalhd_rx_dma_pkt *rx_fallback_head;
	uint32_t (*pfnReadDevRegister)(struct crystalhd_adp *, uint32_t);
	void (*pfnStopRXDMAEngines)(struct crystalhd_hw *);
	bool (*pfnNotifyHardware)(struct crystalhd_hw *, int);
};
struct crystalhd_cmd { struct crystalhd_hw *hw_ctx; unsigned int state; };
typedef struct {
	struct { union { struct { uint32_t bDiscardOnly; } FlushRxCap; } u; } udata;
} crystalhd_ioctl_data;

static unsigned int irq_depth, reads, clears, releases, detaches;
static unsigned int attached_owners, stops, notifications, starts;
static uint32_t interrupt_bits;
static BC_STATUS start_result;
static bool notify_result, interrupt_sem;
static struct device device;
static struct crystalhd_hw *current_hw;
static struct crystalhd_rx_buffer buffers[BC_RX_LIST_CNT];
static struct crystalhd_rx_dma_pkt packets[BC_RX_LIST_CNT];

static struct device *chddev(void) { return &device; }
static void down(int *sem) { assert(!*sem); *sem = 1; }
static int down_interruptible(int *sem)
{
	if (interrupt_sem) {
		interrupt_sem = false;
		return -1;
	}
	down(sem);
	return 0;
}
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
static struct crystalhd_rx_dma_pkt *
crystalhd_dioq_fetch(struct crystalhd_dioq *queue)
{
	struct crystalhd_rx_dma_pkt *packet;

	assert(queue);
	if (!queue->count)
		return NULL;
	packet = queue->items[--queue->count];
	queue->items[queue->count] = NULL;
	assert(packet);
	return packet;
}
static BC_STATUS crystalhd_dioq_add(struct crystalhd_dioq *queue,
		struct crystalhd_rx_dma_pkt *packet, bool wake, unsigned int tag)
{
	(void)wake;
	(void)tag;
	assert(queue && packet && queue->count < ARRAY_SIZE(queue->items));
	queue->items[queue->count++] = packet;
	return BC_STS_SUCCESS;
}
static void crystalhd_hw_free_rx_pkt(struct crystalhd_hw *hw,
		struct crystalhd_rx_dma_pkt *packet)
{
	assert(packet && packet->buffer && !packet->detached);
	assert(hw->fetch_sem == 1);
	assert(irq_depth);
	packet->buffer = NULL;
	packet->detached = true;
	assert(attached_owners);
	attached_owners--;
	detaches++;
}
static void crystalhd_hw_retain_rx_pkt(struct crystalhd_hw *hw,
		struct crystalhd_rx_dma_pkt *packet)
{
	assert(hw && packet);
	packet->next = hw->rx_fallback_head;
	hw->rx_fallback_head = packet;
}
static struct crystalhd_rx_dma_pkt *
crystalhd_hw_fetch_retained_rx_pkt(struct crystalhd_hw *hw)
{
	struct crystalhd_rx_dma_pkt *packet = hw->rx_fallback_head;

	if (packet) {
		hw->rx_fallback_head = packet->next;
		packet->next = NULL;
	}
	return packet;
}
static void crystalhd_rx_buffer_release(struct crystalhd_adp *adp,
		struct crystalhd_rx_buffer *buffer)
{
	assert(adp && current_hw && current_hw->fetch_sem == 1);
	assert(irq_depth);
	/* stop_capture_locked must detach every packet before the first release. */
	assert(!attached_owners);
	assert(buffer && !buffer->released);
	buffer->released = true;
	releases++;
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

static BC_STATUS flush_capture(struct crystalhd_cmd *ctx, bool direct,
		uint32_t discard_only)
{
	crystalhd_ioctl_data data = {0};

	if (direct)
		return crystalhd_capture_flush(ctx, discard_only != 0);
	data.udata.u.FlushRxCap.bDiscardOnly = discard_only;
	return bc_cproc_flush_cap_buffs(ctx, &data);
}

static void seed_queue(struct crystalhd_dioq *queue, unsigned int count,
		unsigned int *next_owner)
{
	unsigned int i;

	for (i = 0; i < count; i++) {
		unsigned int owner = (*next_owner)++;

		assert(owner < BC_RX_LIST_CNT);
		buffers[owner].id = owner + 1;
		packets[owner].pkt_tag = 0x100 + owner;
		packets[owner].buffer = &buffers[owner];
		queue->items[queue->count++] = &packets[owner];
		attached_owners++;
	}
}

static void reset_flush_test(struct crystalhd_hw *hw, struct crystalhd_cmd *ctx,
		struct crystalhd_adp *adp, struct crystalhd_dioq *active,
		struct crystalhd_dioq *ready, struct crystalhd_dioq *freeq,
		unsigned int active_count, unsigned int ready_count,
		unsigned int free_count, unsigned int state)
{
	unsigned int next_owner = 0;

	memset(buffers, 0, sizeof(buffers));
	memset(packets, 0, sizeof(packets));
	*active = (struct crystalhd_dioq){0};
	*ready = (struct crystalhd_dioq){0};
	*freeq = (struct crystalhd_dioq){0};
	attached_owners = 0;
	seed_queue(active, active_count, &next_owner);
	seed_queue(ready, ready_count, &next_owner);
	seed_queue(freeq, free_count, &next_owner);
	*hw = (struct crystalhd_hw){
		.adp = adp,
		.rx_actq = active,
		.rx_rdyq = ready,
		.rx_freeq = freeq,
		.pfnStopRXDMAEngines = stop_success,
		.pfnNotifyHardware = notify_hardware,
	};
	*ctx = (struct crystalhd_cmd){ .hw_ctx = hw, .state = state };
	current_hw = hw;
	irq_depth = reads = clears = releases = detaches = stops = 0;
	notifications = starts = 0;
	interrupt_sem = false;
	notify_result = true;
	start_result = BC_STS_SUCCESS;
}

static void test_flush_frontend(bool direct)
{
	struct pci_dev pci = { .irq = 1 };
	struct crystalhd_adp adp = { .pdev = &pci };
	struct crystalhd_hw hw;
	struct crystalhd_cmd ctx;
	struct crystalhd_dioq active, ready, freeq;
	const unsigned int running = BC_LINK_CAP_EN | BC_LINK_FMT_CHG | 0x80;

	reset_flush_test(&hw, &ctx, &adp, &active, &ready, &freeq,
		0, 0, 0, running);
	assert(flush_capture(NULL, direct, 0) == BC_STS_INV_ARG);
	ctx.hw_ctx = NULL;
	assert(flush_capture(&ctx, direct, 1) == BC_STS_INV_ARG);
	assert(!hw.fetch_sem && !stops && !notifications && !starts);

	reset_flush_test(&hw, &ctx, &adp, &active, &ready, &freeq,
		1, 1, 1, running);
	interrupt_sem = true;
	assert(flush_capture(&ctx, direct, 0) == BC_STS_IO_USER_ABORT);
	assert(active.count == 1 && ready.count == 1 && freeq.count == 1);
	assert(ctx.state == running && !hw.fetch_sem && !stops && !releases &&
		!detaches && attached_owners == 3 &&
		!notifications && !starts && !irq_depth);

	reset_flush_test(&hw, &ctx, &adp, &active, &ready, &freeq,
		1, 1, 1, BC_LINK_FMT_CHG | 0x80);
	assert(flush_capture(&ctx, direct, 1) == BC_STS_ERR_USAGE);
	assert(active.count == 1 && ready.count == 1 && freeq.count == 1);
	assert(ctx.state == (BC_LINK_FMT_CHG | 0x80) && !hw.fetch_sem &&
		!stops && !releases && !detaches && attached_owners == 3 &&
		!notifications && !starts && !irq_depth);

	reset_flush_test(&hw, &ctx, &adp, &active, &ready, &freeq,
		1, 1, 0, running);
	start_result = BC_STS_NO_DATA;
	assert(flush_capture(&ctx, direct, direct ? 1 : UINT32_MAX) == BC_STS_SUCCESS);
	assert(!active.count && !ready.count && freeq.count == 2);
	assert(ctx.state == running && !hw.fetch_sem && stops == 1 &&
		!releases && !detaches && attached_owners == 2 &&
		notifications == 1 && starts == 1 && !irq_depth &&
		!hw.rx_cancel_epoch);

	reset_flush_test(&hw, &ctx, &adp, &active, &ready, &freeq,
		1, 1, 0, running);
	notify_result = false;
	assert(flush_capture(&ctx, direct, 1) == BC_STS_IO_ERROR);
	assert(!active.count && !ready.count && freeq.count == 2);
	assert(ctx.state == running && !hw.fetch_sem && stops == 1 &&
		!releases && !detaches && attached_owners == 2 &&
		notifications == 1 && !starts && !irq_depth);

	reset_flush_test(&hw, &ctx, &adp, &active, &ready, &freeq,
		1, 1, 0, running);
	start_result = BC_STS_IO_ERROR;
	assert(flush_capture(&ctx, direct, 1) == BC_STS_IO_ERROR);
	assert(!active.count && !ready.count && freeq.count == 2);
	assert(ctx.state == running && !hw.fetch_sem && stops == 1 &&
		!releases && !detaches && attached_owners == 2 &&
		notifications == 1 && starts == 1 && !irq_depth);

	reset_flush_test(&hw, &ctx, &adp, &active, &ready, &freeq,
		1, 1, 1, running);
	hw.pfnStopRXDMAEngines = stop_failure;
	assert(flush_capture(&ctx, direct, 1) == BC_STS_IO_ERROR);
	assert(active.count == 1 && ready.count == 1 && freeq.count == 1);
	assert(ctx.state == running && hw.dma_fault && !hw.fetch_sem &&
		stops == 1 && !releases && !detaches && attached_owners == 3 &&
		!notifications && !starts && !irq_depth);

	reset_flush_test(&hw, &ctx, &adp, &active, &ready, &freeq,
		1, 1, 1, running);
	assert(flush_capture(&ctx, direct, 0) == BC_STS_SUCCESS);
	assert(!active.count && !ready.count && !freeq.count);
	assert(releases == 3 && detaches == 3 && !attached_owners);
	assert(buffers[0].released && buffers[1].released && buffers[2].released);
	assert(ctx.state == 0x80 && !hw.fetch_sem && stops == 1 &&
		!notifications && !starts && !irq_depth &&
		hw.rx_cancel_epoch == 1);

	reset_flush_test(&hw, &ctx, &adp, &active, &ready, &freeq,
		1, 1, 1, running);
	hw.pfnStopRXDMAEngines = stop_failure;
	assert(flush_capture(&ctx, direct, 0) == BC_STS_IO_ERROR);
	assert(active.count == 1 && ready.count == 1 && freeq.count == 1);
	assert(!releases && !detaches && attached_owners == 3);
	assert(ctx.state == 0x80 && hw.dma_fault && !hw.fetch_sem && stops == 1 &&
		!notifications && !starts && !irq_depth &&
		hw.rx_cancel_epoch == 1);
}

int main(void)
{
	struct pci_dev pci = { .irq = 1 };
	struct crystalhd_adp adp = { .pdev = &pci };
	struct crystalhd_hw hw = { .adp = &adp,
		.pfnReadDevRegister = read_register,
		.pfnStopRXDMAEngines = stop_success,
		.pfnNotifyHardware = notify_hardware };
	struct crystalhd_dioq active = {0}, ready = {0}, freeq = {0};
	struct crystalhd_cmd ctx = { .hw_ctx = &hw, .state = BC_LINK_CAP_EN };
	crystalhd_ioctl_data data = {0};
	union FLEA_INTR_BITS_COMMON done = { .WholeReg = 0 };

	/* A monitor-only close must not touch nonexistent DMA queues/IRQs. */
	assert(crystalhd_hw_stop_capture(&hw, true) == BC_STS_SUCCESS);
	assert(!irq_depth && !stops && !releases && !hw.rx_cancel_epoch);
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
	reset_flush_test(&hw, &ctx, &adp, &active, &ready, &freeq,
		1, 1, 0, BC_LINK_CAP_EN);
	data.udata.u.FlushRxCap.bDiscardOnly = 1;
	start_result = BC_STS_NO_DATA;
	assert(bc_cproc_flush_cap_buffs(&ctx, &data) == BC_STS_SUCCESS);
	assert(!active.count && !ready.count && freeq.count == 2);
	assert(notifications == 1 && starts == 1 && !releases && !detaches &&
		attached_owners == 2 && !irq_depth);
	start_result = BC_STS_IO_ERROR;
	assert(bc_cproc_flush_cap_buffs(&ctx, &data) == BC_STS_IO_ERROR);
	notify_result = false;
	assert(bc_cproc_flush_cap_buffs(&ctx, &data) == BC_STS_IO_ERROR);
	assert(starts == 2 && !hw.fetch_sem && !irq_depth);

	reset_flush_test(&hw, &ctx, &adp, &active, &ready, &freeq,
		1, 0, 2, BC_LINK_CAP_EN);
	hw.pfnStopRXDMAEngines = stop_failure;
	assert(bc_cproc_flush_cap_buffs(&ctx, &data) == BC_STS_IO_ERROR);
	assert(active.count == 1 && !ready.count && freeq.count == 2);
	assert(!releases && !detaches && attached_owners == 3 && !irq_depth);
	assert(!hw.fetch_sem && !hw.rx_cancel_epoch);
	data.udata.u.FlushRxCap.bDiscardOnly = 0;
	assert(bc_cproc_flush_cap_buffs(&ctx, &data) == BC_STS_IO_ERROR);
	assert(active.count == 1 && !ready.count && freeq.count == 2);
	assert(!releases && !detaches && attached_owners == 3 && !irq_depth &&
		!hw.fetch_sem && hw.rx_cancel_epoch == 1);
	/* A later successful teardown releases every retained registration. */
	hw.dma_fault = false;
	hw.pfnStopRXDMAEngines = stop_success;
	ctx.state = BC_LINK_CAP_EN;
	assert(bc_cproc_flush_cap_buffs(&ctx, &data) == BC_STS_SUCCESS);
	assert(!active.count && !ready.count && !freeq.count);
	assert(releases == 3 && detaches == 3 && !attached_owners);
	assert(buffers[0].released && buffers[1].released && buffers[2].released);
	assert(!ctx.state && !hw.fetch_sem && !irq_depth &&
		hw.rx_cancel_epoch == 2);
	test_flush_frontend(false);
	test_flush_frontend(true);
	puts("DMA stop/restart tests passed (ASan/UBSan)");
	return 0;
}
