/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Device-free execution of the artifact's actual trace reader and stubs. */
#include <assert.h>
#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef uint32_t u32;
typedef unsigned long long u64;
typedef unsigned int BC_STATUS;
#define BC_STS_SUCCESS 0U
#define BC_STS_IO_ERROR 7U
#define ListStsFree 0U
#define sts_free 0U
#define READ_ONCE(v) (v)
#define GFP_KERNEL 0
#define __printf(a, b) __attribute__((format(printf, a, b)))

#include "pwm-shadow-types.h"
#include "crystalhd_pm_shadow_ring.h"

struct crystalhd_hw {
	u64 FleaShadowSession;
	u32 FleaPowerState, FleaPowerTransition, FleaTxActive, FleaIrqActive;
	u32 FleaRawActive, fwcmd_trans_active, fwcmd_active, FwCmdCnt;
	u32 fwcmd_poisoned, TxList0Sts, TxList1Sts, rx_list_sts[2];
	u32 EmptyCnt, SingleThreadAppFIFOEmpty, hw_pause_issued, dma_fault;
	u32 FleaEnablePWM;
	int lock;
};
struct inode { int unused; };
struct seq_file { void *private; };
struct file { void *private_data; };
static struct chd_pm_shadow_ring shadow_ring;
static u64 shadow_session, now;
static int shadow_lock, lock_depth, hw_lock_depth;
static unsigned int fault_publications, allocations, checks;
static bool fail_alloc, fail_open;
static char output[131072];
static size_t output_size;

#define CHECK(expression) do { assert(expression); checks++; } while (0)
#define spin_lock_irqsave(lock, flags) do { \
	(flags) = 0; \
	if ((void *)(lock) == (void *)&shadow_lock) { CHECK(lock_depth == 0); lock_depth++; } \
	else { CHECK(lock_depth == 0 && hw_lock_depth == 0); hw_lock_depth++; } \
} while (0)
#define spin_unlock_irqrestore(lock, flags) do { \
	(void)(flags); \
	if ((void *)(lock) == (void *)&shadow_lock) { CHECK(lock_depth == 1); lock_depth--; } \
	else { CHECK(lock_depth == 0 && hw_lock_depth == 1); hw_lock_depth--; } \
} while (0)

static u64 ktime_get_mono_fast_ns(void) { return ++now; }
static void *kmalloc(size_t size, int flags)
{
	void *result;
	(void)flags;
	if (fail_alloc)
		return NULL;
	result = malloc(size);
	if (result)
		allocations++;
	return result;
}
static void kfree(void *address)
{
	if (address) {
		CHECK(allocations > 0);
		allocations--;
		free(address);
	}
}
static int single_open(struct file *file, int (*show)(struct seq_file *, void *),
		       void *data)
{
	struct seq_file *seq;
	(void)show;
	if (fail_open)
		return -EIO;
	seq = kmalloc(sizeof(*seq), GFP_KERNEL);
	if (!seq)
		return -ENOMEM;
	seq->private = data;
	file->private_data = seq;
	return 0;
}
static int single_release(struct inode *inode, struct file *file)
{
	(void)inode;
	kfree(file->private_data);
	file->private_data = NULL;
	return 0;
}
static void seq_printf(struct seq_file *seq, const char *format, ...) __printf(2, 3);
static void seq_printf(struct seq_file *seq, const char *format, ...)
{
	va_list args;
	int written;
	(void)seq;
	CHECK(lock_depth == 0 && hw_lock_depth == 0);
	va_start(args, format);
	written = vsnprintf(output + output_size, sizeof(output) - output_size,
			    format, args);
	va_end(args);
	CHECK(written >= 0 && (size_t)written < sizeof(output) - output_size);
	output_size += (size_t)written;
}
static void seq_puts(struct seq_file *seq, const char *text)
{
	seq_printf(seq, "%s", text);
}
static void crystalhd_hw_publish_fault(struct crystalhd_hw *hw)
{
	fault_publications++;
	hw->dma_fault = true;
}

#include "pwm-shadow-functions.h"
#include "pwm-shadow-stubs.h"

static void trace(struct crystalhd_hw *hw, u32 stage)
{
	crystalhd_pm_shadow_trace(hw, stage, FLEA_EVT_NONE, hw->FleaPowerState,
				 hw->FleaPowerState, BC_STS_SUCCESS,
				 CHD_PM_SHADOW_UNKNOWN);
}

int main(void)
{
	struct crystalhd_hw *hw = calloc(1, sizeof(*hw));
	struct file file = {0};
	struct inode inode = {0};
	struct chd_pm_shadow_record record = {0};
	struct chd_pm_shadow_ring *snapshot;
	u32 i;
	u32 *owners[] = {
		&hw->FleaTxActive, &hw->FleaIrqActive, &hw->FleaRawActive,
		&hw->fwcmd_trans_active, &hw->fwcmd_active, &hw->FwCmdCnt,
		&hw->fwcmd_poisoned, &hw->TxList0Sts, &hw->TxList1Sts,
		&hw->rx_list_sts[0], &hw->rx_list_sts[1], &hw->EmptyCnt,
		&hw->SingleThreadAppFIFOEmpty,
	};

	CHECK(hw != NULL);
	hw->FleaEnablePWM = true;
	hw->FleaPowerState = FLEA_PS_ACTIVE;
	crystalhd_pm_shadow_session(hw);
	CHECK(hw->FleaShadowSession == 1);
	CHECK(crystalhd_flea_runtime_power_dn(hw) == FLEA_PD_REFUSED);
	CHECK(hw->FleaPowerState == FLEA_PS_ACTIVE && !hw->dma_fault);
	CHECK(shadow_ring.down_refused == 1 && !shadow_ring.violations);
	hw->FleaTxActive = 1;
	CHECK(crystalhd_flea_runtime_power_dn(hw) == FLEA_PD_REFUSED);
	CHECK(shadow_ring.record[1].violations == CHD_PM_DOWN_OWNER);
	hw->FleaTxActive = 0;
	hw->FleaPowerState = FLEA_PS_LP_PENDING;
	trace(hw, CHD_PM_RX_ADMIT);
	CHECK(shadow_ring.record[2].violations == CHD_PM_RX_NOT_ACTIVE);
	trace(hw, CHD_PM_FLL_RESTORE_BEGIN);
	CHECK(shadow_ring.record[3].violations == 0);
	hw->FleaPowerState = FLEA_PS_ACTIVE;
	hw->FleaPowerTransition = 1;
	trace(hw, CHD_PM_RX_ADMIT);
	CHECK(shadow_ring.record[4].violations == CHD_PM_RX_NOT_ACTIVE);
	hw->FleaPowerTransition = 0;
	trace(hw, CHD_PM_FLL_RESTORE_BEGIN);
	CHECK(shadow_ring.record[5].violations == CHD_PM_FLL_EARLY_ACTIVE);
	hw->FleaPowerState = FLEA_PS_LP_COMPLETE;
	CHECK(crystalhd_flea_runtime_power_up(hw) == BC_STS_IO_ERROR);
	CHECK(hw->FleaPowerState == FLEA_PS_FAULT && hw->dma_fault);
	CHECK(fault_publications == 1 && shadow_ring.up_entry == 1);
	CHECK(shadow_ring.lp_complete == 1);
	/* Each independently owned producer/list must trigger the same fuse. */
	hw->FleaPowerState = FLEA_PS_LP_PENDING;
	for (i = 0; i < sizeof(owners) / sizeof(owners[0]); i++) {
		u32 slot = shadow_ring.next;
		*owners[i] = 1;
		CHECK(crystalhd_flea_runtime_power_dn(hw) == FLEA_PD_REFUSED);
		CHECK(shadow_ring.record[slot].violations == CHD_PM_DOWN_OWNER);
		*owners[i] = 0;
	}
	/* PENDING + failed FLL is not ACTIVE admission. ACTIVE + failure is. */
	crystalhd_pm_shadow_trace(hw, CHD_PM_FLL_RESTORE, FLEA_EVT_NONE,
		FLEA_PS_LP_PENDING, FLEA_PS_ACTIVE, BC_STS_IO_ERROR, 3);
	CHECK(shadow_ring.record[20].violations == 0);
	hw->FleaPowerState = FLEA_PS_ACTIVE;
	crystalhd_pm_shadow_trace(hw, CHD_PM_FLL_RESTORE, FLEA_EVT_NONE,
		FLEA_PS_LP_PENDING, FLEA_PS_ACTIVE, BC_STS_IO_ERROR, 3);
	CHECK(shadow_ring.record[21].violations == CHD_PM_FLL_EARLY_ACTIVE);
	crystalhd_pm_shadow_retire(hw);
	CHECK(shadow_ring.retire_valid && shadow_ring.retire.sequence == 23 &&
	      shadow_ring.retire.stage == CHD_PM_RETIRE &&
	      shadow_ring.retire.session == 1 && shadow_ring.retire.new_state == FLEA_PS_ACTIVE);

	/* A reader owns a copy, unaffected by future writes or the freed session. */
	CHECK(shadow_open(&inode, &file) == 0);
	snapshot = ((struct seq_file *)file.private_data)->private;
	CHECK(snapshot->count == 23 && snapshot->total == 23);
	free(hw);
	for (i = 0; i < CHD_PM_SHADOW_RECORDS + 7; i++)
		chd_pm_shadow_store(&shadow_ring, &record, 0);
	CHECK(shadow_ring.count == CHD_PM_SHADOW_RECORDS);
	CHECK(shadow_ring.total == CHD_PM_SHADOW_RECORDS + 30);
	CHECK(shadow_ring.overwritten == 30);
	CHECK(snapshot->count == 23 && snapshot->total == 23);
	CHECK(shadow_show(file.private_data, NULL) == 0);
	CHECK(strstr(output, "total=23 overwritten=0 down_refused=15 up_entry=1") != NULL);
	CHECK(shadow_release(&inode, &file) == 0 && allocations == 0);
	output_size = 0;
	CHECK(shadow_open(&inode, &file) == 0);
	CHECK(shadow_show(file.private_data, NULL) == 0);
	snapshot = ((struct seq_file *)file.private_data)->private;
	CHECK(snapshot->receipt_count == 23 && snapshot->receipt[0].sequence == 1 &&
	      snapshot->receipt[1].sequence == 2 && snapshot->retire.sequence == 23 &&
	      snapshot->first_fault.sequence == 2);
	CHECK(strstr(output, "receipt 1 ") && strstr(output, "retire 23 ") &&
	      strstr(output, "first-fault 2 "));
	for (i = 0; i < snapshot->count; i++) {
		u32 first = (snapshot->next + CHD_PM_SHADOW_RECORDS - snapshot->count) %
			    CHD_PM_SHADOW_RECORDS;
		CHECK(snapshot->record[(first + i) % CHD_PM_SHADOW_RECORDS].sequence ==
		      (u64)snapshot->overwritten + i + 1);
	}
	CHECK(shadow_release(&inode, &file) == 0 && allocations == 0);
	fail_alloc = true;
	CHECK(shadow_open(&inode, &file) == -ENOMEM && allocations == 0);
	fail_alloc = false;
	fail_open = true;
	CHECK(shadow_open(&inode, &file) == -EIO && allocations == 0);
	for (i = 0; i < CHD_PM_SHADOW_RECEIPTS + 3; i++)
		chd_pm_shadow_store(&shadow_ring, &record, 1);
	CHECK(shadow_ring.receipt_count == CHD_PM_SHADOW_RECEIPTS &&
	      shadow_ring.receipt_dropped == 26 && shadow_ring.receipt[0].sequence == 1 &&
	      shadow_ring.retire.sequence == 23 && shadow_ring.first_fault.sequence == 2);
	shadow_ring.total = shadow_ring.overwritten = shadow_ring.down_refused =
		shadow_ring.up_entry = shadow_ring.lp_complete = shadow_ring.violations =
		shadow_ring.receipt_total = shadow_ring.receipt_dropped = ~(u64)0;
	shadow_ring.stage_count[CHD_PM_DOWN_REFUSED] = ~(u64)0;
	record.stage = CHD_PM_DOWN_REFUSED;
	record.violations = CHD_PM_LP_COMPLETE;
	chd_pm_shadow_store(&shadow_ring, &record, 1);
	CHECK(shadow_ring.total == ~(u64)0 && shadow_ring.down_refused == ~(u64)0 &&
	      shadow_ring.overwritten == ~(u64)0 && shadow_ring.lp_complete == ~(u64)0 &&
	      shadow_ring.violations == ~(u64)0 && shadow_ring.receipt_total == ~(u64)0 &&
	      shadow_ring.receipt_dropped == ~(u64)0 &&
	      shadow_ring.stage_count[CHD_PM_DOWN_REFUSED] == ~(u64)0);
	record.stage = CHD_PM_UP_ENTRY;
	chd_pm_shadow_store(&shadow_ring, &record, 0);
	CHECK(shadow_ring.up_entry == ~(u64)0);
	/* A first terminal DMA/firmware fault need not violate a PM assertion. */
	for (i = 0; i < 3; i++) {
		struct crystalhd_hw fault_hw = {0};
		fault_hw.FleaShadowSession = 2;
		fault_hw.FleaPowerState = i == 1 ? FLEA_PS_FAULT : FLEA_PS_ACTIVE;
		fault_hw.dma_fault = i == 0;
		fault_hw.fwcmd_poisoned = i == 2;
		shadow_ring.first_fault_valid = 0;
		crystalhd_pm_shadow_trace(&fault_hw, CHD_PM_FAULT, FLEA_EVT_NONE,
			fault_hw.FleaPowerState, fault_hw.FleaPowerState,
			BC_STS_IO_ERROR, CHD_PM_SHADOW_UNKNOWN);
		CHECK(shadow_ring.first_fault_valid &&
		      shadow_ring.first_fault.dma_fault == (i == 0) &&
		      shadow_ring.first_fault.new_state == fault_hw.FleaPowerState &&
		      shadow_ring.first_fault.fw_poisoned == (i == 2) &&
		      shadow_ring.first_fault.violations == 0);
	}
	CHECK(lock_depth == 0);
	printf("pwm-shadow: %u checks PASS; snapshot-bytes=%zu; no device access\n",
	       checks, sizeof(shadow_ring));
	return 0;
}
