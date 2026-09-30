/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Exercise the production Link RX ISR at deterministic lock, MMIO and
 * ownership boundaries. Device handlers and MMIO are controlled stubs.
 */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "bc_dts_types.h"
struct _BC_DTS_PROC_OUT;
#include "bc_dts_defs.h"
#include "bcm_70012_regs.h"
#include "rx-isr-types.h"

#define KERN_ERR ""
#define printk(...) ((void)0)
#define dev_info(dev, ...) ((void)(dev))
#define MAYBE_UNUSED __attribute__((unused))

#define GET_RX_INTR_MASK (INTR_INTR_STATUS_L1_UV_RX_DMA_ERR_INTR_MASK | \
	INTR_INTR_STATUS_L1_UV_RX_DMA_DONE_INTR_MASK | \
	INTR_INTR_STATUS_L1_Y_RX_DMA_ERR_INTR_MASK | \
	INTR_INTR_STATUS_L1_Y_RX_DMA_DONE_INTR_MASK | \
	INTR_INTR_STATUS_L0_UV_RX_DMA_ERR_INTR_MASK | \
	INTR_INTR_STATUS_L0_UV_RX_DMA_DONE_INTR_MASK | \
	INTR_INTR_STATUS_L0_Y_RX_DMA_ERR_INTR_MASK | \
	INTR_INTR_STATUS_L0_Y_RX_DMA_DONE_INTR_MASK)

struct device { int unused; };
struct pci_dev { struct device dev; };
struct crystalhd_adp { struct pci_dev *pdev; };
struct crystalhd_rx_dma_pkt { unsigned id; };
struct crystalhd_hw_stats { uint32_t rx_success, rx_errors; };
struct crystalhd_hw {
	struct crystalhd_adp *adp;
	int rx_lock;
	enum list_sts rx_list_sts[DMA_ENGINE_CNT];
	struct crystalhd_hw_stats stats;
	uint32_t stop_pending;
	bool hw_pause_issued;
	uint32_t (*pfnReadFPGARegister)(struct crystalhd_adp *, uint32_t);
	void (*pfnWriteFPGARegister)(struct crystalhd_adp *, uint32_t, uint32_t);
	void (*pfnHWGetDoneSize)(struct crystalhd_hw *, uint32_t,
				     uint32_t *, uint32_t *);
};

static struct pci_dev endpoint;
static struct crystalhd_adp adapter = { .pdev = &endpoint };
static struct crystalhd_hw hardware;
static struct crystalhd_rx_dma_pkt old_packets[DMA_ENGINE_CNT] = {
	{ .id = 1 }, { .id = 2 }
};
static struct crystalhd_rx_dma_pkt fresh_packets[DMA_ENGINE_CNT] = {
	{ .id = 11 }, { .id = 12 }
};
static BC_STATUS planned[DMA_ENGINE_CNT];
static bool old_active[DMA_ENGINE_CNT], fresh_active[DMA_ENGINE_CNT];
static bool old_detached[DMA_ENGINE_CNT], old_ready[DMA_ENGINE_CNT];
static bool old_free[DMA_ENGINE_CNT], inject_fresh[DMA_ENGINE_CNT];
static uint32_t live_y[DMA_ENGINE_CNT], live_uv[DMA_ENGINE_CNT];
static uint32_t sampled_y[DMA_ENGINE_CNT], sampled_uv[DMA_ENGINE_CNT];
static unsigned checks, failures, lock_recursions, duplicate_tags;
static unsigned posts_before_both_handlers;
static unsigned detach_calls, explicit_complete_calls, legacy_complete_calls;
static unsigned detach_under_lock, complete_under_lock, complete_outside_lock;
static unsigned handler_calls[DMA_ENGINE_CNT], fresh_posts[DMA_ENGINE_CNT];
static unsigned done_size_under_lock[DMA_ENGINE_CNT], fresh_detaches;
static unsigned start_calls, finalize_calls, error_complete_calls;
static bool both_errors_detached_before_retry;

static void check(bool condition, const char *message)
{
	checks++;
	if (!condition) {
		failures++;
		fprintf(stderr, "FAIL: %s\n", message);
	}
}

static void post_fresh_at_unlock(void);

static void test_spin_lock(int *lock, unsigned long *flags)
{
	*flags = 0;
	if (*lock) {
		lock_recursions++;
		return;
	}
	*lock = 1;
}

static void test_spin_unlock(int *lock, unsigned long flags)
{
	(void)flags;
	assert(*lock == 1);
	*lock = 0;
	post_fresh_at_unlock();
}

#define spin_lock_irqsave(lock, flags) test_spin_lock((lock), &(flags))
#define spin_unlock_irqrestore(lock, flags) test_spin_unlock((lock), (flags))

static void post_fresh_at_unlock(void)
{
	unsigned long flags;

	for (unsigned i = 0; i < DMA_ENGINE_CNT; i++) {
		if (!inject_fresh[i] || fresh_posts[i] ||
		    hardware.rx_list_sts[i] != sts_free)
			continue;
		fresh_posts[i]++;
		posts_before_both_handlers += handler_calls[1] == 0;
		spin_lock_irqsave(&hardware.rx_lock, flags);
		if (old_active[i])
			duplicate_tags++;
		fresh_active[i] = true;
		hardware.rx_list_sts[i] = rx_waiting_y_intr;
		live_y[i] = 640 + i;
		live_uv[i] = 320 + i;
		spin_unlock_irqrestore(&hardware.rx_lock, flags);
	}
}

static uint32_t read_register(struct crystalhd_adp *adp, uint32_t reg)
{
	assert(adp == &adapter);
	if (reg == MISC1_Y_RX_ERROR_STATUS || reg == MISC1_UV_RX_ERROR_STATUS)
		return 0;
	assert(false);
	return 0;
}

static void write_register(struct crystalhd_adp *adp, uint32_t reg,
			   uint32_t value)
{
	(void)reg;
	(void)value;
	assert(adp == &adapter);
}

static void get_done_size(struct crystalhd_hw *hw, uint32_t index,
			  uint32_t *y_size, uint32_t *uv_size)
{
	assert(hw == &hardware && index < DMA_ENGINE_CNT);
	done_size_under_lock[index] += hw->rx_lock == 1;
	*y_size = live_y[index];
	*uv_size = live_uv[index];
}

static bool handle_list(struct crystalhd_hw *hw, unsigned index)
{
	assert(hw == &hardware && hw->rx_lock == 1);
	handler_calls[index]++;
	if (planned[index] == BC_STS_NO_DATA)
		return false;
	hw->rx_list_sts[index] = planned[index] == BC_STS_SUCCESS ?
		sts_free : rx_y_error;
	return true;
}

static bool crystalhd_link_rx_list0_handler(
	struct crystalhd_hw *hw, uint32_t intr_sts,
	uint32_t y_err_sts, uint32_t uv_err_sts)
{
	(void)intr_sts;
	(void)y_err_sts;
	(void)uv_err_sts;
	return handle_list(hw, 0);
}

static bool crystalhd_link_rx_list1_handler(
	struct crystalhd_hw *hw, uint32_t intr_sts,
	uint32_t y_err_sts, uint32_t uv_err_sts)
{
	(void)intr_sts;
	(void)y_err_sts;
	(void)uv_err_sts;
	return handle_list(hw, 1);
}

static MAYBE_UNUSED struct crystalhd_rx_dma_pkt *crystalhd_rx_pkt_detach(
	struct crystalhd_hw *hw, uint32_t list_index, BC_STATUS status)
{
	struct crystalhd_rx_dma_pkt *packet = NULL;

	detach_calls++;
	assert(hw == &hardware && list_index < DMA_ENGINE_CNT);
	detach_under_lock += hw->rx_lock == 1;
	check(status == planned[list_index],
	      "completion detaches the owner with its list status");
	if (old_active[list_index]) {
		old_active[list_index] = false;
		old_detached[list_index] = true;
		packet = &old_packets[list_index];
	} else if (fresh_active[list_index]) {
		fresh_active[list_index] = false;
		fresh_detaches++;
		packet = &fresh_packets[list_index];
	}
	return packet;
}

static MAYBE_UNUSED BC_STATUS crystalhd_rx_pkt_complete(
	struct crystalhd_hw *hw, struct crystalhd_rx_dma_pkt *packet,
	uint32_t list_index, BC_STATUS status)
{
	unsigned long flags;
	uint32_t y_size, uv_size;

	explicit_complete_calls++;
	assert(hw == &hardware && list_index < DMA_ENGINE_CNT && packet);
	if (hw->rx_lock)
		complete_under_lock++;
	else
		complete_outside_lock++;

	if (status == BC_STS_SUCCESS) {
		hw->pfnHWGetDoneSize(hw, list_index, &y_size, &uv_size);
		sampled_y[list_index] = y_size;
		sampled_uv[list_index] = uv_size;
		if (packet == &old_packets[list_index]) {
			old_detached[list_index] = false;
			old_ready[list_index] = true;
		}
		return BC_STS_SUCCESS;
	}

	check(packet == &old_packets[list_index] && old_detached[list_index],
	      "error completion retains the packet detached for this list");
	if (!error_complete_calls++ && planned[0] == BC_STS_ERROR &&
	    planned[1] == BC_STS_ERROR)
		both_errors_detached_before_retry =
			old_detached[0] && old_detached[1];
	spin_lock_irqsave(&hw->rx_lock, flags);
	old_detached[list_index] = false;
	old_free[list_index] = true;
	spin_unlock_irqrestore(&hw->rx_lock, flags);
	return BC_STS_BUSY;
}

static MAYBE_UNUSED BC_STATUS crystalhd_rx_pkt_done(
	struct crystalhd_hw *hw, uint32_t list_index, BC_STATUS status)
{
	struct crystalhd_rx_dma_pkt *packet;

	legacy_complete_calls++;
	packet = crystalhd_rx_pkt_detach(hw, list_index, status);
	if (!packet)
		return BC_STS_INV_ARG;
	return crystalhd_rx_pkt_complete(hw, packet, list_index, status);
}

static BC_STATUS crystalhd_hw_start_capture(struct crystalhd_hw *hw)
{
	assert(hw == &hardware && !hw->rx_lock);
	start_calls++;
	return BC_STS_SUCCESS;
}

static void crystalhd_link_hw_finalize_pause(struct crystalhd_hw *hw)
{
	assert(hw == &hardware && !hw->rx_lock);
	finalize_calls++;
}

#include "rx-link-isr.h"

static void reset_scenario(BC_STATUS list0, BC_STATUS list1,
			   unsigned missing_mask, unsigned inject_mask)
{
	memset(&hardware, 0, sizeof(hardware));
	hardware.adp = &adapter;
	hardware.pfnReadFPGARegister = read_register;
	hardware.pfnWriteFPGARegister = write_register;
	hardware.pfnHWGetDoneSize = get_done_size;
	planned[0] = list0;
	planned[1] = list1;
	for (unsigned i = 0; i < DMA_ENGINE_CNT; i++) {
		hardware.rx_list_sts[i] = rx_waiting_y_intr;
		old_active[i] = !(missing_mask & (1U << i));
		fresh_active[i] = old_detached[i] = old_ready[i] = false;
		old_free[i] = false;
		inject_fresh[i] = inject_mask & (1U << i);
		live_y[i] = 128 + i;
		live_uv[i] = 64 + i;
		sampled_y[i] = sampled_uv[i] = 0;
		handler_calls[i] = fresh_posts[i] = 0;
		done_size_under_lock[i] = 0;
	}
	lock_recursions = duplicate_tags = posts_before_both_handlers = 0;
	detach_calls = 0;
	explicit_complete_calls = legacy_complete_calls = 0;
	detach_under_lock = complete_under_lock = complete_outside_lock = 0;
	fresh_detaches = start_calls = finalize_calls = error_complete_calls = 0;
	both_errors_detached_before_retry = false;
}

static void invoke_isr(void)
{
	uint32_t interrupt = INTR_INTR_STATUS_L0_Y_RX_DMA_DONE_INTR_MASK |
		INTR_INTR_STATUS_L1_Y_RX_DMA_DONE_INTR_MASK;

	crystalhd_link_rx_isr(&hardware, interrupt);
}

static void run_success_scenario(void)
{
	reset_scenario(BC_STS_SUCCESS, BC_STS_NO_DATA, 0, 1);
	invoke_isr();

	check(!hardware.rx_lock && !lock_recursions && !duplicate_tags &&
	      !posts_before_both_handlers,
	      "Link success never exposes a reusable list before detaching its owner");
	check(legacy_complete_calls == 1 && detach_calls == 1 &&
	      explicit_complete_calls == 1 && detach_under_lock == 1 &&
	      complete_under_lock == 1 && !complete_outside_lock,
	      "successful completion remains wholly inside rx_lock");
	check(sampled_y[0] == 128 && sampled_uv[0] == 64 &&
	      done_size_under_lock[0] == 1,
	      "successful completion samples the old list's live sizes under lock");
	check(!old_active[0] && old_ready[0] && fresh_active[0] &&
	      fresh_posts[0] == 1 && !fresh_detaches,
	      "the old success reaches ready before a fresh owner reuses its tag");
	check(hardware.stats.rx_success == 1 && !hardware.stats.rx_errors &&
	      handler_calls[0] == 1 && handler_calls[1] == 1 && start_calls == 1,
	      "success accounting visits both lists and restarts after unlocking");
}

static void run_error_scenario(void)
{
	reset_scenario(BC_STS_ERROR, BC_STS_NO_DATA, 0, 1);
	invoke_isr();

	check(!hardware.rx_lock && !lock_recursions && !duplicate_tags &&
	      !posts_before_both_handlers,
	      "Link error retry never exposes a duplicate tag or recurses on rx_lock");
	check(!legacy_complete_calls && detach_calls == 1 &&
	      explicit_complete_calls == 1 && detach_under_lock == 1 &&
	      !complete_under_lock && complete_outside_lock == 1,
	      "error completion detaches under rx_lock and retries after unlocking");
	check(!old_active[0] && old_free[0] && fresh_active[0] &&
	      fresh_posts[0] == 1 && !fresh_detaches,
	      "a fresh owner safely reuses the failed packet's detached tag");
	check(hardware.stats.rx_errors == 1 && !hardware.stats.rx_success &&
	      handler_calls[0] == 1 && handler_calls[1] == 1 && !start_calls,
	      "error accounting visits both lists without a success restart");
}

static void run_dual_scenario(BC_STATUS list0, BC_STATUS list1)
{
	unsigned successes = (list0 == BC_STS_SUCCESS) +
		(list1 == BC_STS_SUCCESS);
	unsigned errors = (list0 == BC_STS_ERROR) +
		(list1 == BC_STS_ERROR);

	reset_scenario(list0, list1, 0, 3);
	invoke_isr();

	check(!hardware.rx_lock && !lock_recursions && !duplicate_tags &&
	      !fresh_detaches && !posts_before_both_handlers,
	      "dual Link completions stay atomic until both handlers finish");
	check(legacy_complete_calls == successes && detach_calls == 2 &&
	      explicit_complete_calls == 2 && detach_under_lock == 2 &&
	      complete_under_lock == successes && complete_outside_lock == errors,
	      "dual completion keeps successes inside and error retries outside rx_lock");
	check(old_ready[0] == (list0 == BC_STS_SUCCESS) &&
	      old_ready[1] == (list1 == BC_STS_SUCCESS) &&
	      old_free[0] == (list0 == BC_STS_ERROR) &&
	      old_free[1] == (list1 == BC_STS_ERROR) &&
	      fresh_active[0] && fresh_active[1],
	      "dual completion preserves both old results and both fresh owners");
	check((list0 != BC_STS_SUCCESS ||
	       (sampled_y[0] == 128 && sampled_uv[0] == 64)) &&
	      (list1 != BC_STS_SUCCESS ||
	       (sampled_y[1] == 129 && sampled_uv[1] == 65)),
	      "every dual success samples its original done-size generation");
	check(hardware.stats.rx_success == successes &&
	      hardware.stats.rx_errors == errors && handler_calls[0] == 1 &&
	      handler_calls[1] == 1 && start_calls == (successes != 0),
	      "dual completion accounts for both lists and restarts at most once");
	if (errors == DMA_ENGINE_CNT)
		check(both_errors_detached_before_retry,
		      "both failed owners detach before the first retry can repost");
}

static void run_missing_owner_scenario(void)
{
	reset_scenario(BC_STS_ERROR, BC_STS_NO_DATA, 1, 1);
	invoke_isr();

	check(!hardware.rx_lock && !lock_recursions && detach_calls == 1 &&
	      detach_under_lock == 1 && !explicit_complete_calls &&
	      !legacy_complete_calls && !posts_before_both_handlers,
	      "a missing error owner is checked under rx_lock and not completed");
	check(fresh_active[0] && fresh_posts[0] == 1 && !fresh_detaches &&
	      !old_detached[0] && !old_free[0],
	      "a fresh owner posted after a missing completion cannot be stolen");
}

static void run_no_change_scenario(void)
{
	reset_scenario(BC_STS_NO_DATA, BC_STS_NO_DATA, 0, 0);
	invoke_isr();

	check(!hardware.rx_lock && !lock_recursions && !detach_calls &&
	      !explicit_complete_calls && !legacy_complete_calls,
	      "an interrupt without list transitions balances the lock without completion");
	check(old_active[0] && old_active[1] &&
	      handler_calls[0] == 1 && handler_calls[1] == 1 &&
	      !hardware.stats.rx_success && !hardware.stats.rx_errors &&
	      !start_calls && !finalize_calls,
	      "an unchanged interrupt preserves both owners and control state");
}

static void run_control_scenarios(void)
{
	reset_scenario(BC_STS_SUCCESS, BC_STS_SUCCESS, 0, 0);
	hardware.stop_pending = 1;
	invoke_isr();
	check(finalize_calls == 1 && !start_calls && !hardware.rx_lock,
	      "stop-pending finalizes once after both lists become free and unlock");

	reset_scenario(BC_STS_SUCCESS, BC_STS_NO_DATA, 0, 0);
	hardware.stop_pending = 1;
	invoke_isr();
	check(!finalize_calls && !start_calls && !hardware.rx_lock,
	      "stop-pending waits when one list remains active");

	reset_scenario(BC_STS_SUCCESS, BC_STS_NO_DATA, 0, 0);
	hardware.hw_pause_issued = true;
	invoke_isr();
	check(!finalize_calls && !start_calls && !hardware.rx_lock,
	      "an issued hardware pause suppresses restart after unlocking");
}

int main(void)
{
	run_success_scenario();
	run_error_scenario();
	run_dual_scenario(BC_STS_SUCCESS, BC_STS_SUCCESS);
	run_dual_scenario(BC_STS_SUCCESS, BC_STS_ERROR);
	run_dual_scenario(BC_STS_ERROR, BC_STS_SUCCESS);
	run_dual_scenario(BC_STS_ERROR, BC_STS_ERROR);
	run_missing_owner_scenario();
	run_no_change_scenario();
	run_control_scenarios();

	printf("Link RX ISR ownership: %u checks, %u failures\n", checks, failures);
	return failures ? 1 : 0;
}
