// SPDX-License-Identifier: GPL-2.0-or-later
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum {
	BC_STS_SUCCESS, BC_STS_INV_ARG, BC_STS_ERR_USAGE,
	BC_STS_IO_ERROR, BC_STS_NO_DATA, BC_STS_IO_USER_ABORT
} BC_STATUS;
#define MAX_VALID_POLL_CNT 2
#define BCHP_INTR_INTR_STATUS 0
/* Distinct register-shim tokens; these tests check accesses, not bus addresses. */
#define BCHP_INTR_INTR_CLR_REG 1
#define BCHP_INTR_EOI_CTRL 2
#define INTR_INTR_STATUS 3
#define Stream2Host_Intr_Sts 4
#define INTR_INTR_CLR_REG 5
#define INTR_EOI_CTRL 6
#define BC_PCI_DEVID_FLEA 0x1615
#define BC_PCI_DEVID_LINK 0x1612
#define BC_EVENT_START_CAPTURE 6
#define BC_LINK_CAP_EN 1
#define BC_LINK_FMT_CHG 2
#define BC_RX_LIST_CNT 16
#define BC_TX_LIST_CNT 2
#define ARRAY_SIZE(array) (sizeof(array) / sizeof((array)[0]))
#define READ_ONCE(value) (value)
#define WRITE_ONCE(value, update) ((value) = (update))
#define spin_lock_irqsave(lock, flags) ((void)(lock), (flags) = 0)
#define spin_unlock_irqrestore(lock, flags) ((void)(lock), (void)(flags))
#define dev_err(dev, ...) ((void)(dev))
#define dev_dbg(dev, ...) ((void)(dev))
#define printk(...) ((void)0)
#include "dma-stop-types.h"

struct device { int unused; };
struct pci_dev { struct device dev; int irq; uint32_t device; };
struct crystalhd_adp {
	struct pci_dev *pdev;
	bool present;
	bool dma_terminal_quiesced;
	struct { unsigned int cin_wait_exit; } cmds;
};
struct crystalhd_rx_buffer {
	unsigned int id;
	bool released;
};
struct crystalhd_rx_dma_pkt {
	unsigned int pkt_tag;
	struct crystalhd_rx_buffer *buffer;
	struct crystalhd_rx_dma_pkt *next;
	bool detached;
	struct { void *pdma_desc_start; unsigned int sz; uint64_t phy_addr; } desc_mem;
};
struct tx_dma_pkt {
	struct { void *pdma_desc_start; unsigned int sz; uint64_t phy_addr; } desc_mem;
	const void *buffer, *retained_buffer;
	void *call_back, *cb_context;
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
	struct crystalhd_dioq *tx_actq, *tx_freeq;
	struct tx_dma_pkt tx_pkt_pool[BC_TX_LIST_CNT];
	struct crystalhd_rx_dma_pkt *rx_pkt_pool_head;
	struct crystalhd_rx_dma_pkt *rx_fallback_head;
	uint32_t (*pfnReadDevRegister)(struct crystalhd_adp *, uint32_t);
	uint32_t (*pfnReadFPGARegister)(struct crystalhd_adp *, uint32_t);
	void (*pfnWriteDevRegister)(struct crystalhd_adp *, uint32_t, uint32_t);
	void (*pfnWriteFPGARegister)(struct crystalhd_adp *, uint32_t, uint32_t);
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
static bool local_pending, master_enabled;
static unsigned int master_clears, pending_waits;
static unsigned int flea_masks, link_masks;
static bool checking_ack;
static uint32_t ack_dma_status, ack_decoder_status;
static struct { bool fpga; uint32_t reg, value; } ack_writes[4];
static unsigned int ack_write_count;
static unsigned int ioq_deletes, tx_retires;
static bool checking_combined_retirement;
static unsigned int retirement_step, tx_puts;
static const void *expected_retained_tx;
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
	assert(current_hw && adp == current_hw->adp);
	assert(irq_depth);
	reads++;
	if (checking_ack) {
		assert(reg == BCHP_INTR_INTR_STATUS || reg == Stream2Host_Intr_Sts);
		return reg == BCHP_INTR_INTR_STATUS ? ack_dma_status : ack_decoder_status;
	}
	return interrupt_bits;
}
static uint32_t read_fpga(struct crystalhd_adp *adp, uint32_t reg)
{
	assert(checking_ack && current_hw && adp == current_hw->adp && irq_depth);
	assert(reg == INTR_INTR_STATUS);
	reads++;
	return ack_dma_status;
}
static void write_ack(struct crystalhd_adp *adp, uint32_t reg, uint32_t value, bool fpga)
{
	assert(checking_ack && current_hw && adp == current_hw->adp && irq_depth);
	assert(ack_write_count < ARRAY_SIZE(ack_writes));
	assert(flea_masks + link_masks == 1);
	ack_writes[ack_write_count].fpga = fpga;
	ack_writes[ack_write_count].reg = reg;
	ack_writes[ack_write_count++].value = value;
}
static void write_register(struct crystalhd_adp *adp, uint32_t reg, uint32_t value)
{ write_ack(adp, reg, value, false); }
static void write_fpga(struct crystalhd_adp *adp, uint32_t reg, uint32_t value)
{ write_ack(adp, reg, value, true); }
static void crystalhd_flea_disable_interrupts(struct crystalhd_hw *hw)
{
	assert(hw == current_hw && hw->adp->pdev->device == BC_PCI_DEVID_FLEA);
	assert(hw->dma_fault && !hw->adp->present && hw->adp->cmds.cin_wait_exit);
	flea_masks++;
}
static void crystalhd_link_disable_interrupts(struct crystalhd_hw *hw)
{
	assert(hw == current_hw && hw->adp->pdev->device == BC_PCI_DEVID_LINK);
	assert(hw->dma_fault && !hw->adp->present && hw->adp->cmds.cin_wait_exit);
	link_masks++;
}
static void crystalhd_flea_clear_rx_errs_intrs(struct crystalhd_hw *hw)
{
	(void)hw; clears++;
}
void crystalhd_hw_dma_fatal_stop(struct crystalhd_hw *hw);
static void pci_clear_master(struct pci_dev *pdev)
{
	assert(current_hw && pdev == current_hw->adp->pdev);
	master_enabled = false;
	master_clears++;
}
static int pci_wait_for_pending_transaction(struct pci_dev *pdev)
{
	assert(current_hw && pdev == current_hw->adp->pdev);
	pending_waits++;
	return local_pending;
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
	if (checking_combined_retirement)
		assert(retirement_step++ == 0);
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
	if (checking_combined_retirement) {
		for (unsigned int i = 0; i < ARRAY_SIZE(current_hw->tx_pkt_pool); i++) {
			const struct tx_dma_pkt *packet = &current_hw->tx_pkt_pool[i];

			assert(!packet->buffer && !packet->retained_buffer &&
				!packet->call_back && !packet->cb_context);
		}
		assert(tx_puts == 1 && retirement_step++ == 2);
	}
	assert(buffer && !buffer->released);
	buffer->released = true;
	releases++;
}
static void crystalhd_hw_delete_ioqs(struct crystalhd_hw *hw)
{
	assert(hw == current_hw && !hw->rx_actq && !hw->rx_rdyq && !hw->rx_freeq &&
		!hw->tx_actq && !hw->tx_freeq && !hw->rx_fallback_head && !hw->rx_pkt_pool_head);
	ioq_deletes++;
}
static void crystalhd_hw_retire_tx_quiesced(struct crystalhd_hw *hw)
{
	assert(hw == current_hw && ioq_deletes == tx_retires + 1);
	if (checking_combined_retirement) {
		assert(hw->dma_fault && !hw->adp->present &&
			hw->adp->dma_terminal_quiesced);
		assert(!attached_owners && !hw->rx_actq && !hw->rx_rdyq &&
			!hw->rx_freeq && !hw->rx_pkt_pool_head && !hw->rx_fallback_head);
		for (unsigned int i = 0; i < ARRAY_SIZE(packets); i++)
			assert(!packets[i].buffer);
		/* Model only the TX retire boundary; the actual shared ring cleanup
		 * must have detached every RX identity before reaching any TX put.
		 */
		for (unsigned int i = 0; i < ARRAY_SIZE(hw->tx_pkt_pool); i++) {
			struct tx_dma_pkt *packet = &hw->tx_pkt_pool[i];

			if (packet->retained_buffer) {
				assert(packet->retained_buffer == expected_retained_tx);
				assert(!tx_puts && !releases && retirement_step++ == 1);
				packet->retained_buffer = NULL;
				tx_puts++;
			}
		}
	}
	for (unsigned int i = 0; i < ARRAY_SIZE(hw->tx_pkt_pool); i++) {
		const struct tx_dma_pkt *packet = &hw->tx_pkt_pool[i];

		assert(!packet->buffer && !packet->retained_buffer && !packet->call_back && !packet->cb_context);
	}
	tx_retires++;
}
static struct crystalhd_rx_dma_pkt *crystalhd_hw_alloc_rx_pkt(struct crystalhd_hw *hw)
{
	assert(hw == current_hw && !hw->rx_pkt_pool_head);
	return NULL;
}
static void bc_kern_dma_free(struct crystalhd_adp *adp, unsigned int size,
		void *memory, uint64_t address)
{
	(void)adp; (void)size; (void)memory; (void)address;
	abort(); /* No positive empty-inventory case owns DMA allocation storage. */
}
static void kfree(void *memory)
{
	(void)memory;
	abort();
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
{ assert(hw->fetch_sem == 1); crystalhd_hw_dma_fatal_stop(hw); stops++; }
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
	adp->present = true;
	adp->cmds.cin_wait_exit = 0;
	local_pending = master_enabled = true;
	master_clears = pending_waits = 0;
	flea_masks = link_masks = 0;
	checking_ack = false;
	checking_combined_retirement = false;
	retirement_step = tx_puts = 0;
	expected_retained_tx = NULL;
	ioq_deletes = tx_retires = 0;
	irq_depth = reads = clears = releases = detaches = stops = 0;
	notifications = starts = 0;
	interrupt_sem = false;
	notify_result = true;
	start_result = BC_STS_SUCCESS;
}

static void test_flush_frontend(bool direct)
{
	struct pci_dev pci = { .irq = 1, .device = BC_PCI_DEVID_FLEA };
	struct crystalhd_adp adp = { .pdev = &pci, .present = true };
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

static void test_fatal_pending_is_not_proof(void)
{
	for (unsigned int pending = 0; pending < 2; pending++) {
		struct pci_dev pci = { .irq = 1, .device = BC_PCI_DEVID_FLEA };
		struct crystalhd_adp adp = { .pdev = &pci };
		struct crystalhd_hw hw;
		struct crystalhd_cmd ctx;
		struct crystalhd_dioq active, ready, freeq;

		reset_flush_test(&hw, &ctx, &adp, &active, &ready, &freeq,
			1, 1, 1, BC_LINK_CAP_EN);
		local_pending = pending;
		hw.pfnStopRXDMAEngines = stop_failure;
		assert(crystalhd_hw_stop_capture(&hw, true) == BC_STS_IO_ERROR);
		assert(hw.dma_fault && !adp.present && adp.cmds.cin_wait_exit);
		assert(flea_masks == 1 && !link_masks && master_clears == 1 &&
			pending_waits == 1 && !master_enabled);
		assert(active.count == 1 && ready.count == 1 && freeq.count == 1);
		assert(!releases && !detaches && attached_owners == 3);
		/* Even a later successful engine callback cannot revoke uncertainty.
		 * End this isolated model retained; there is no fabricated reset/free.
		 */
		hw.pfnStopRXDMAEngines = stop_success;
		assert(crystalhd_hw_stop_capture(&hw, true) == BC_STS_IO_ERROR);
		assert(hw.dma_fault && !adp.present && !irq_depth && !hw.fetch_sem);
		assert(active.count == 1 && ready.count == 1 && freeq.count == 1);
		assert(!releases && !detaches && attached_owners == 3);
	}
}

static void test_fault_interrupt_acknowledgement(void)
{
	const uint32_t devices[] = { BC_PCI_DEVID_FLEA, BC_PCI_DEVID_LINK, 0xffff };
	const uint32_t statuses[] = { 0, ~0U, 1, 0xdeaddead };

	for (unsigned int chip = 0; chip < ARRAY_SIZE(devices); chip++) {
		for (unsigned int dma = 0; dma < ARRAY_SIZE(statuses); dma++) {
			for (unsigned int decoder = 0; decoder < ARRAY_SIZE(statuses); decoder++) {
				struct pci_dev pci = { .irq = 1, .device = devices[chip] };
				struct crystalhd_adp adp = { .pdev = &pci };
				struct crystalhd_hw hw;
				struct crystalhd_cmd ctx;
				struct crystalhd_dioq active, ready, freeq;
				bool dma_valid = statuses[dma] && statuses[dma] != ~0U;
				bool decoder_valid = statuses[decoder] && statuses[decoder] != ~0U &&
					statuses[decoder] != 0xdeaddead;
				bool handled = chip == 0 ? dma_valid : chip == 1 && (dma_valid || decoder_valid);
				unsigned int n = 0;

				reset_flush_test(&hw, &ctx, &adp, &active, &ready, &freeq,
					1, 1, 1, BC_LINK_CAP_EN);
				crystalhd_hw_dma_fatal_stop(&hw);
				flea_masks = link_masks = 0;
				checking_ack = true;
				ack_dma_status = statuses[dma];
				ack_decoder_status = statuses[decoder];
				ack_write_count = 0;
				memset(ack_writes, 0, sizeof(ack_writes));
				hw.pfnReadDevRegister = read_register;
				hw.pfnReadFPGARegister = read_fpga;
				hw.pfnWriteDevRegister = write_register;
				hw.pfnWriteFPGARegister = write_fpga;
				irq_depth = 1; /* Model an already-dispatched interrupt callback. */
				assert(crystalhd_hw_ack_fault_interrupt(&hw) == handled);
				assert(reads == (chip == 0 ? 1U : chip == 1 ? 2U : 0U));
				assert(flea_masks == (unsigned int)(handled && chip == 0));
				assert(link_masks == (unsigned int)(handled && chip == 1));
				if (handled && chip == 0) {
					assert(!ack_writes[n].fpga && ack_writes[n].reg == BCHP_INTR_INTR_CLR_REG &&
						ack_writes[n++].value == statuses[dma]);
					assert(!ack_writes[n].fpga && ack_writes[n].reg == BCHP_INTR_EOI_CTRL &&
						ack_writes[n++].value == 1);
				} else if (handled) {
					if (decoder_valid) {
						assert(!ack_writes[n].fpga && ack_writes[n].reg == Stream2Host_Intr_Sts &&
							ack_writes[n++].value == statuses[decoder]);
						assert(!ack_writes[n].fpga && ack_writes[n].reg == Stream2Host_Intr_Sts &&
							!ack_writes[n++].value);
					}
					if (dma_valid)
						assert(ack_writes[n].fpga && ack_writes[n].reg == INTR_INTR_CLR_REG &&
							ack_writes[n++].value == statuses[dma]);
					assert(ack_writes[n].fpga && ack_writes[n].reg == INTR_EOI_CTRL &&
						ack_writes[n++].value == 1);
				}
				assert(ack_write_count == n && !detaches && !releases && attached_owners == 3);
				assert(active.count == 1 && ready.count == 1 && freeq.count == 1 && !hw.rx_cancel_epoch);
				assert(hw.dma_fault && !adp.present && adp.cmds.cin_wait_exit &&
					!master_enabled && master_clears == 1 && pending_waits == 1);
				assert(irq_depth == 1); /* Ack never changes Linux IRQ lifetime. */
			}
		}
	}
}

static void test_empty_faulted_ring_inventory(void)
{
	for (unsigned int field = 0; field <= 17; field++) {
		struct pci_dev pci = { .irq = 1, .device = BC_PCI_DEVID_FLEA };
		struct crystalhd_adp adp = { .pdev = &pci };
		struct crystalhd_hw hw = { .adp = &adp, .dma_fault = true };
		unsigned int token = field;
		void *identity = &token;

		current_hw = &hw;
		ioq_deletes = tx_retires = 0;
		/* field zero is the exact empty monitor inventory. Every other case
		 * injects one owner without dereferencing that rejected identity.
		 */
		switch (field) {
		case 0: break;
		case 1: hw.rx_pkt_pool_head = identity; break;
		case 2: hw.rx_fallback_head = identity; break;
		case 3: hw.rx_actq = identity; break;
		case 4: hw.rx_rdyq = identity; break;
		case 5: hw.rx_freeq = identity; break;
		case 6: hw.tx_actq = identity; break;
		case 7: hw.tx_freeq = identity; break;
		default: {
			struct tx_dma_pkt *packet = &hw.tx_pkt_pool[(field - 8) / 5];

			switch ((field - 8) % 5) {
			case 0: packet->desc_mem.pdma_desc_start = identity; break;
			case 1: packet->buffer = identity; break;
			case 2: packet->retained_buffer = identity; break;
			case 3: packet->call_back = identity; break;
			case 4: packet->cb_context = identity; break;
			}
		}
		}
		assert(crystalhd_hw_dma_inventory_empty(&hw) == !field);
		if (field) {
			struct crystalhd_hw before = hw;

			assert(crystalhd_hw_free_dma_rings(&hw) == BC_STS_IO_ERROR);
			assert(!memcmp(&before, &hw, sizeof(hw)) && !ioq_deletes && !tx_retires);
		} else {
			assert(crystalhd_hw_free_dma_rings(&hw) == BC_STS_SUCCESS);
			assert(ioq_deletes == 1 && tx_retires == 1);
		}
		assert(hw.dma_fault && !adp.dma_terminal_quiesced);
	}
}

static void test_combined_terminal_ring_retirement(void)
{
	struct pci_dev pci = { .irq = 1, .device = BC_PCI_DEVID_FLEA };
	struct crystalhd_adp adp = { .pdev = &pci };
	struct crystalhd_hw hw;
	struct crystalhd_cmd ctx;
	struct crystalhd_dioq active, ready, freeq;
	unsigned int tx_identity;

	reset_flush_test(&hw, &ctx, &adp, &active, &ready, &freeq,
		0, 0, 0, 0);
	hw.rx_actq = hw.rx_rdyq = hw.rx_freeq = NULL;
	packets[0].buffer = &buffers[0];
	hw.rx_fallback_head = &packets[0];
	attached_owners = 1;
	hw.tx_pkt_pool[1].retained_buffer = &tx_identity;
	expected_retained_tx = &tx_identity;
	/* Independent caller-proven terminal state, not a successful local retry.
	 * The fixture does not erase the fatal flag or resume hardware accesses.
	 */
	hw.dma_fault = true;
	adp.present = false;
	adp.cmds.cin_wait_exit = 1;
	adp.dma_terminal_quiesced = true;
	master_enabled = false;
	irq_depth = 1;
	hw.fetch_sem = 1;
	checking_combined_retirement = true;
	assert(!crystalhd_hw_dma_inventory_empty(&hw));
	assert(crystalhd_hw_free_dma_rings(&hw) == BC_STS_SUCCESS);
	assert(detaches == 1 && tx_puts == 1 && releases == 1 &&
		retirement_step == 3 && buffers[0].released && packets[0].detached);
	assert(ioq_deletes == 1 && tx_retires == 1 &&
		crystalhd_hw_dma_inventory_empty(&hw));
	assert(crystalhd_hw_free_dma_rings(&hw) == BC_STS_SUCCESS);
	assert(detaches == 1 && tx_puts == 1 && releases == 1 && retirement_step == 3);
	assert(ioq_deletes == 2 && tx_retires == 2 &&
		crystalhd_hw_dma_inventory_empty(&hw));
	assert(hw.dma_fault && !adp.present && adp.dma_terminal_quiesced &&
		adp.cmds.cin_wait_exit && !master_enabled && irq_depth == 1 && hw.fetch_sem == 1);
	assert(!stops && !reads && !master_clears && !pending_waits &&
		!flea_masks && !link_masks && !notifications && !starts);
	checking_combined_retirement = false;
}

int main(void)
{
	struct pci_dev pci = { .irq = 1, .device = BC_PCI_DEVID_FLEA };
	struct crystalhd_adp adp = { .pdev = &pci, .present = true };
	struct crystalhd_hw hw = { .adp = &adp,
		.pfnReadDevRegister = read_register,
		.pfnStopRXDMAEngines = stop_success,
		.pfnNotifyHardware = notify_hardware };
	struct crystalhd_dioq active = {0}, ready = {0}, freeq = {0};
	struct crystalhd_cmd ctx = { .hw_ctx = &hw, .state = BC_LINK_CAP_EN };
	crystalhd_ioctl_data data = {0};
	union FLEA_INTR_BITS_COMMON done = { .WholeReg = 0 };
	current_hw = &hw;
	local_pending = master_enabled = true;

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
		!hw.fetch_sem && !hw.rx_cancel_epoch);
	/* A later backend stop does not establish terminal PCI/IRQ proof. */
	hw.pfnStopRXDMAEngines = stop_success;
	ctx.state = BC_LINK_CAP_EN;
	assert(bc_cproc_flush_cap_buffs(&ctx, &data) == BC_STS_IO_ERROR);
	assert(active.count == 1 && !ready.count && freeq.count == 2);
	assert(!releases && !detaches && attached_owners == 3);
	assert(!buffers[0].released && !buffers[1].released && !buffers[2].released);
	assert(hw.dma_fault && !adp.present && adp.cmds.cin_wait_exit);
	assert(!ctx.state && !hw.fetch_sem && !irq_depth &&
		!hw.rx_cancel_epoch);
	test_flush_frontend(false);
	test_flush_frontend(true);
	test_fatal_pending_is_not_proof();
	test_fault_interrupt_acknowledgement();
	test_empty_faulted_ring_inventory();
	test_combined_terminal_ring_retirement();
	puts("DMA stop/restart and fatal ownership tests passed");
	return 0;
}
