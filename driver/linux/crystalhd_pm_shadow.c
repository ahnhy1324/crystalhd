/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Isolated host-PM shadow artifact. No target MMIO or firmware operations. */
#include <linux/debugfs.h>
#include <linux/err.h>
#include <linux/ktime.h>
#include <linux/module.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include "crystalhd_hw.h"
#include "crystalhd_pm_shadow.h"

static DEFINE_SPINLOCK(shadow_lock);
static struct chd_pm_shadow_ring shadow_ring;
static u64 shadow_session;
static struct dentry *shadow_debugfs;
static bool arbiter_attempted;

bool crystalhd_pm_arbiter_claim(void)
{
	unsigned long flags;
	bool claimed;

	spin_lock_irqsave(&shadow_lock, flags);
	claimed = !arbiter_attempted;
	arbiter_attempted = true;
	spin_unlock_irqrestore(&shadow_lock, flags);
	return claimed;
}

void crystalhd_pm_arbiter_record(struct crystalhd_hw *hw,
			       const struct chd_pm_arbiter_report *report)
{
	unsigned long flags;

	spin_lock_irqsave(&shadow_lock, flags);
	shadow_ring.arbiter = *report;
	shadow_ring.arbiter_valid = 1;
	spin_unlock_irqrestore(&shadow_lock, flags);
	crystalhd_pm_shadow_trace(hw, CHD_PM_ARBITER, FLEA_EVT_NONE,
				 hw->FleaPowerState, hw->FleaPowerState,
				 report->down_result, CHD_PM_SHADOW_UNKNOWN);
}

void crystalhd_pm_retention_record(struct crystalhd_hw *hw,
			          const struct chd_pm_retention_report *report)
{
	unsigned long flags;

	spin_lock_irqsave(&shadow_lock, flags);
	shadow_ring.retention = *report;
	shadow_ring.retention_valid = 1;
	spin_unlock_irqrestore(&shadow_lock, flags);
	crystalhd_pm_shadow_trace(hw, CHD_PM_RETENTION, FLEA_EVT_NONE,
				 hw->FleaPowerState, hw->FleaPowerState,
				 report->down_result, CHD_PM_SHADOW_UNKNOWN);
}

void crystalhd_pm_shadow_session(struct crystalhd_hw *hw)
{
	unsigned long flags;

	spin_lock_irqsave(&shadow_lock, flags);
	chd_pm_shadow_increment(&shadow_session);
	hw->FleaShadowSession = shadow_session;
	spin_unlock_irqrestore(&shadow_lock, flags);
}

/* Existing caller lifetime and hw->lock protect normal input snapshots.
 * Terminal fault publication can be unlocked: its owner fields are best-effort
 * READ_ONCE samples, not a new coherent DMA/firmware ownership fence.
 * Lock order is hw->lock -> shadow_lock. Readers acquire shadow_lock only.
 */
void crystalhd_pm_shadow_trace(struct crystalhd_hw *hw, u32 stage,
			      u32 request, u32 old_state, u32 selected,
			      u32 status, u32 fll)
{
	struct chd_pm_shadow_record record = {
		.time_ns = ktime_get_mono_fast_ns(),
		.session = READ_ONCE(hw->FleaShadowSession),
		.stage = stage, .request = request, .old_state = old_state,
		.selected_state = selected, .new_state = READ_ONCE(hw->FleaPowerState),
		.reservation = READ_ONCE(hw->FleaPowerTransition),
		.status = status, .fll = fll,
		.dma_fault = READ_ONCE(hw->dma_fault),
		.tx_owner = READ_ONCE(hw->FleaTxActive), .irq_owner = READ_ONCE(hw->FleaIrqActive),
		.raw_owner = READ_ONCE(hw->FleaRawActive),
		.fw_transaction = READ_ONCE(hw->fwcmd_trans_active),
		.fw_active = READ_ONCE(hw->fwcmd_active), .fw_count = READ_ONCE(hw->FwCmdCnt),
		.fw_poisoned = READ_ONCE(hw->fwcmd_poisoned),
		.tx_list0 = READ_ONCE(hw->TxList0Sts), .tx_list1 = READ_ONCE(hw->TxList1Sts),
		.rx_list0 = READ_ONCE(hw->rx_list_sts[0]),
		.rx_list1 = READ_ONCE(hw->rx_list_sts[1]),
		.empty_count = READ_ONCE(hw->EmptyCnt),
		.single_fifo_empty = READ_ONCE(hw->SingleThreadAppFIFOEmpty),
		.rx_pause = READ_ONCE(hw->hw_pause_issued),
	};
	unsigned long flags;
	u32 critical;

	if (stage == CHD_PM_DOWN_REFUSED &&
	    (record.tx_owner || record.irq_owner || record.raw_owner ||
	     record.fw_transaction || record.fw_active || record.fw_count ||
	     record.fw_poisoned || record.tx_list0 != ListStsFree ||
	     record.tx_list1 != ListStsFree || record.rx_list0 != sts_free ||
	     record.rx_list1 != sts_free || record.empty_count ||
	     record.single_fifo_empty))
		record.violations |= CHD_PM_DOWN_OWNER;
	if (stage == CHD_PM_RX_ADMIT &&
	    (record.new_state != FLEA_PS_ACTIVE || record.reservation))
		record.violations |= CHD_PM_RX_NOT_ACTIVE;
	/* Nonzero FLL is written while PENDING/reserved, then ACTIVE is
	 * published only on success. The intermediate PENDING state is valid.
	 */
	if ((stage == CHD_PM_FLL_RESTORE_BEGIN ||
	     (stage == CHD_PM_FLL_RESTORE && status != BC_STS_SUCCESS)) &&
	    record.new_state == FLEA_PS_ACTIVE)
		record.violations |= CHD_PM_FLL_EARLY_ACTIVE;
	if (record.new_state == FLEA_PS_LP_COMPLETE)
		record.violations |= CHD_PM_LP_COMPLETE;
	critical = record.violations || record.dma_fault || record.fw_poisoned ||
		record.new_state == FLEA_PS_FAULT || stage == CHD_PM_INIT || stage == CHD_PM_RETIRE ||
		stage == CHD_PM_DOWN_REFUSED || stage == CHD_PM_UP_ENTRY ||
		stage == CHD_PM_ARBITER ||
		stage == CHD_PM_RETENTION ||
		stage == CHD_PM_FLL_RESTORE_BEGIN || stage == CHD_PM_FLL_RESTORE ||
		record.old_state != record.new_state ||
		(stage == CHD_PM_SELECT && record.old_state != record.selected_state) ||
		(stage == CHD_PM_FLL && fll == 0);
	spin_lock_irqsave(&shadow_lock, flags);
	chd_pm_shadow_store(&shadow_ring, &record, critical);
	spin_unlock_irqrestore(&shadow_lock, flags);
}

void crystalhd_pm_shadow_retire(struct crystalhd_hw *hw)
{
	unsigned long flags;

	if (!READ_ONCE(hw->FleaEnablePWM))
		return;
	/* Process-context final receipt before the caller frees the hw object. */
	spin_lock_irqsave(&hw->lock, flags);
	crystalhd_pm_shadow_trace(hw, CHD_PM_RETIRE, FLEA_EVT_STOP_DEVICE,
				 hw->FleaPowerState, hw->FleaPowerState,
				 BC_STS_SUCCESS, CHD_PM_SHADOW_UNKNOWN);
	spin_unlock_irqrestore(&hw->lock, flags);
}

static void shadow_print_record(struct seq_file *seq, const char *kind,
				const struct chd_pm_shadow_record *r)
{
	seq_printf(seq, "%s %llu %llu %llu %u %u %u %u %u %u %u %u %u %u "
		   "%u %u %u %u %u %u %u %u %u %u %u %u %u %u\n",
		   kind, r->sequence, r->time_ns, r->session, r->stage, r->request,
		   r->old_state, r->selected_state, r->new_state,
		   r->reservation, r->status, r->fll, r->violations, r->dma_fault,
		   r->tx_owner, r->irq_owner, r->raw_owner,
		   r->fw_transaction, r->fw_active, r->fw_count,
		   r->fw_poisoned, r->tx_list0, r->tx_list1,
		   r->rx_list0, r->rx_list1, r->empty_count,
		   r->single_fifo_empty, r->rx_pause);
}

static int shadow_show(struct seq_file *seq, void *unused)
{
	const struct chd_pm_shadow_ring *snapshot = seq->private;
	u32 i, first = (snapshot->next + CHD_PM_SHADOW_RECORDS -
			snapshot->count) % CHD_PM_SHADOW_RECORDS;
	(void)unused;

	seq_printf(seq, "artifact=3.10.3-pm-retention clock_gating=compiled-out "
		   "self_refresh_only=one-shot "
		   "normal_stop_device_shutdown=unchanged "
		   "capacity=%u total=%llu overwritten=%llu down_refused=%llu "
		   "up_entry=%llu lp_complete=%llu violations=%llu "
		   "receipt_capacity=%u receipt_total=%llu receipt_dropped=%llu\n",
		   CHD_PM_SHADOW_RECORDS, snapshot->total, snapshot->overwritten,
		   snapshot->down_refused, snapshot->up_entry,
		   snapshot->lp_complete, snapshot->violations,
		   CHD_PM_SHADOW_RECEIPTS, snapshot->receipt_total, snapshot->receipt_dropped);
	for (i = 0; i < CHD_PM_STAGE_COUNT; i++)
		seq_printf(seq, "stage-count stage=%u count=%llu\n", i, snapshot->stage_count[i]);
	if (snapshot->arbiter_valid) {
		const struct chd_pm_arbiter_report *a = &snapshot->arbiter;

		seq_printf(seq, "arbiter session=%llu begin_ns=%llu end_ns=%llu result=%u restore=%u "
			   "down_result=%u touched=%u baseline=%u disable_readback=%u "
			   "restore_live=%u restore_readback=%u memc_first=%u memc_last=%u "
			   "polls=%u delay_us=%u\n", a->session, a->begin_ns, a->end_ns,
			   a->result, a->restore, a->down_result, a->touched, a->baseline,
			   a->disable_readback, a->restore_live, a->restore_readback,
			   a->memc_first, a->memc_last, a->polls, a->delay_us);
	}
	if (snapshot->retention_valid) {
		const struct chd_pm_retention_report *r = &snapshot->retention;
		const struct chd_pm_retention_poll *p[] = {&r->readiness, &r->entry, &r->wake};
		const u32 phase[] = {CHD_RET_READINESS, CHD_RET_CKE_LOW, CHD_RET_CKE_HIGH};

		seq_printf(seq, "retention session=%llu begin_ns=%llu end_ns=%llu "
			   "phase=%u failure=%u rollback_failure=%u result=%u down_result=%u "
			   "arbiter_touched=%u ddr_requested=%u settle_us=%u "
			   "master_baseline=%u params_baseline=%u refresh_baseline=%u "
			   "memc_baseline=%u disable_readback=%u master_restore_live=%u "
			   "master_restore_readback=%u params_entry_live=%u params_entry_readback=%u "
			   "refresh_entry_live=%u refresh_entry_readback=%u refresh_restore_live=%u "
			   "refresh_restore_readback=%u params_restore_live=%u params_restore_readback=%u\n",
			   r->session, r->begin_ns, r->end_ns, r->phase, r->failure,
			   r->rollback_failure, r->result, r->down_result,
			   r->arbiter_touched, r->ddr_requested, r->settle_us,
			   r->master_baseline, r->params_baseline, r->refresh_baseline,
			   r->memc_baseline, r->disable_readback, r->master_restore_live,
			   r->master_restore_readback, r->params_entry_live, r->params_entry_readback,
			   r->refresh_entry_live, r->refresh_entry_readback, r->refresh_restore_live,
			   r->refresh_restore_readback, r->params_restore_live, r->params_restore_readback);
		for (i = 0; i < 3; i++)
			seq_printf(seq, "retention-poll phase=%u first=%u last=%u polls=%u "
				   "delay_us=%u result=%u\n", phase[i], p[i]->first,
				   p[i]->last, p[i]->polls, p[i]->delay_us, p[i]->result);
	}
	seq_puts(seq, "kind sequence time_ns session stage request old selected new "
		 "reserved status fll violations dma_fault tx irq raw fw_transaction "
		 "fw_active fw_count fw_poisoned tx0 tx1 rx0 rx1 empty single rx_pause\n");
	for (i = 0; i < snapshot->receipt_count; i++)
		shadow_print_record(seq, "receipt", &snapshot->receipt[i]);
	if (snapshot->retire_valid)
		shadow_print_record(seq, "retire", &snapshot->retire);
	if (snapshot->first_fault_valid)
		shadow_print_record(seq, "first-fault", &snapshot->first_fault);
	for (i = 0; i < snapshot->count; i++) {
		const struct chd_pm_shadow_record *r =
			&snapshot->record[(first + i) % CHD_PM_SHADOW_RECORDS];

		shadow_print_record(seq, "recent", r);
	}
	return 0;
}

static int shadow_open(struct inode *inode, struct file *file)
{
	struct chd_pm_shadow_ring *snapshot;
	unsigned long flags;
	int ret;
	(void)inode;

	snapshot = kmalloc(sizeof(*snapshot), GFP_KERNEL);
	if (!snapshot)
		return -ENOMEM;
	spin_lock_irqsave(&shadow_lock, flags);
	*snapshot = shadow_ring;
	spin_unlock_irqrestore(&shadow_lock, flags);
	ret = single_open(file, shadow_show, snapshot);
	if (ret)
		kfree(snapshot);
	return ret;
}

static int shadow_release(struct inode *inode, struct file *file)
{
	struct seq_file *seq = file->private_data;

	kfree(seq->private);
	return single_release(inode, file);
}

static const struct file_operations shadow_fops = {
	.owner = THIS_MODULE,
	.open = shadow_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = shadow_release,
};

int crystalhd_pm_shadow_init(void)
{
	struct dentry *file;

	shadow_debugfs = debugfs_create_dir("crystalhd-pm-shadow", NULL);
	if (IS_ERR_OR_NULL(shadow_debugfs))
		return shadow_debugfs ? PTR_ERR(shadow_debugfs) : -ENOMEM;
	file = debugfs_create_file("trace", 0400, shadow_debugfs, NULL, &shadow_fops);
	if (IS_ERR_OR_NULL(file)) {
		int ret = file ? PTR_ERR(file) : -ENOMEM;

		debugfs_remove_recursive(shadow_debugfs);
		shadow_debugfs = NULL;
		return ret;
	}
	return 0;
}

void crystalhd_pm_shadow_cleanup(void)
{
	debugfs_remove_recursive(shadow_debugfs);
	shadow_debugfs = NULL;
}
