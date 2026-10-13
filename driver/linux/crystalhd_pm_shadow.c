/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Numeric observers issue no target MMIO. Asleep rejection uses existing faults. */
#include <linux/debugfs.h>
#include <linux/err.h>
#include <linux/ktime.h>
#include <linux/module.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include "crystalhd_hw.h"
#include "crystalhd_lnx.h"
#include "crystalhd_pm_shadow.h"

static DEFINE_SPINLOCK(shadow_lock);
static struct chd_pm_shadow_ring shadow_ring;
static u64 shadow_session;
static struct dentry *shadow_debugfs;
static bool arbiter_attempted;

bool crystalhd_pm_cycle_down_claim(struct crystalhd_hw *hw)
{
	struct chd_pm_cycle_report *r = &shadow_ring.cycle;
	unsigned long flags;
	u32 i;
	bool claimed = false;

	spin_lock_irqsave(&shadow_lock, flags);
	if (!r->down_attempted) {
		*r = (struct chd_pm_cycle_report) {
			.session = hw->FleaShadowSession,
			.down_attempted = 1, .phase = CHD_CYCLE_DOWN_CLAIMED,
			.down_begin_ns = ktime_get_mono_fast_ns(),
			.down_helper_result = ~0U, .down_result = ~0U,
			.up_status = ~0U, .fll_status = ~0U, .saved_cke = ~0U,
			.up_helper_status = ~0U,
			.entry_pause_fll = hw->FleaCyclePauseFll,
			.entry_snapshot_valid = hw->FleaPowerSaved.valid,
			.tx_owner = hw->FleaTxActive, .irq_owner = hw->FleaIrqActive,
			.raw_owner = hw->FleaRawActive, .fw_transaction = hw->fwcmd_trans_active,
			.fw_active = hw->fwcmd_active, .fw_count = hw->FwCmdCnt,
			.fw_poisoned = hw->fwcmd_poisoned,
			.tx0 = hw->TxList0Sts, .tx1 = hw->TxList1Sts,
			.rx0 = hw->rx_list_sts[0], .rx1 = hw->rx_list_sts[1],
			.empty = hw->EmptyCnt, .single = hw->SingleThreadAppFIFOEmpty,
		};
		for (i = 0; i < FLEA_PR_COUNT; i++)
			r->saved[i] = ~0U;
		claimed = true;
	}
	spin_unlock_irqrestore(&shadow_lock, flags);
	return claimed;
}

bool crystalhd_pm_cycle_down_record(struct crystalhd_hw *hw, u32 result)
{
	struct chd_pm_cycle_report *r = &shadow_ring.cycle;
	unsigned long flags;
	u32 i;
	bool valid;

	spin_lock_irqsave(&shadow_lock, flags);
	valid = r->down_attempted && r->session == hw->FleaShadowSession &&
		(result == FLEA_PD_REFUSED || result == FLEA_PD_ENTERED || result == FLEA_PD_FAULT);
	if (result == FLEA_PD_ENTERED &&
	    (!hw->FleaPowerSaved.valid || !r->entry_pause_fll || r->entry_snapshot_valid ||
	     r->tx_owner || r->irq_owner || r->raw_owner || r->fw_transaction || r->fw_active ||
	     r->fw_count || r->fw_poisoned || r->tx0 != ListStsFree || r->tx1 != ListStsFree ||
	     r->rx0 != sts_free || r->rx1 != sts_free || r->empty || r->single))
		valid = false;
	r->down_helper_result = result;
	r->down_result = valid ? result : FLEA_PD_FAULT;
	r->down_end_ns = ktime_get_mono_fast_ns();
	r->down_snapshot_valid = hw->FleaPowerSaved.valid;
	r->phase = r->down_result == FLEA_PD_ENTERED ? CHD_CYCLE_ENTERED :
		r->down_result == FLEA_PD_REFUSED ? CHD_CYCLE_DOWN_REFUSED : CHD_CYCLE_FAULT;
	if (hw->FleaPowerSaved.valid) {
		r->saved_cke = hw->FleaPowerSaved.cke;
		for (i = 0; i < FLEA_PR_COUNT; i++)
			r->saved[i] = hw->FleaPowerSaved.value[i];
	}
	spin_unlock_irqrestore(&shadow_lock, flags);
	return valid;
}

bool crystalhd_pm_cycle_up_claim(struct crystalhd_hw *hw)
{
	struct chd_pm_cycle_report *r = &shadow_ring.cycle;
	unsigned long flags;
	bool claimed = false;

	spin_lock_irqsave(&shadow_lock, flags);
	if (r->phase == CHD_CYCLE_ENTERED && r->down_result == FLEA_PD_ENTERED &&
	    r->session == hw->FleaShadowSession && !r->up_attempted &&
	    hw->FleaPowerTransition && hw->FleaPowerState == FLEA_PS_LP_COMPLETE &&
	    hw->FleaPowerSaved.valid && !READ_ONCE(hw->dma_fault) &&
	    READ_ONCE(hw->adp->present) && READ_ONCE(hw->dev_started)) {
		r->up_attempted = 1;
		r->phase = CHD_CYCLE_UP_CLAIMED;
		r->up_begin_ns = ktime_get_mono_fast_ns();
		r->up_snapshot_valid = hw->FleaPowerSaved.valid;
		claimed = true;
	}
	spin_unlock_irqrestore(&shadow_lock, flags);
	return claimed;
}

void crystalhd_pm_cycle_up_record(struct crystalhd_hw *hw, u32 status)
{
	struct chd_pm_cycle_report *r = &shadow_ring.cycle;
	unsigned long flags;

	spin_lock_irqsave(&shadow_lock, flags);
	r->up_helper_status = status;
	r->up_status = status == BC_STS_SUCCESS && !hw->FleaPowerSaved.valid ?
		BC_STS_SUCCESS : BC_STS_IO_ERROR;
	r->up_end_ns = ktime_get_mono_fast_ns();
	r->wake_snapshot_valid = hw->FleaPowerSaved.valid;
	r->phase = status == BC_STS_SUCCESS && !hw->FleaPowerSaved.valid ?
		CHD_CYCLE_WAKE_CONFIRMED : CHD_CYCLE_FAULT;
	spin_unlock_irqrestore(&shadow_lock, flags);
}

void crystalhd_pm_cycle_fll_record(struct crystalhd_hw *hw, u32 status)
{
	struct chd_pm_cycle_report *r = &shadow_ring.cycle;
	unsigned long flags;

	spin_lock_irqsave(&shadow_lock, flags);
	if (r->session == hw->FleaShadowSession && r->up_attempted &&
	    r->up_status == BC_STS_SUCCESS && r->phase == CHD_CYCLE_WAKE_CONFIRMED) {
		if (!r->fll_attempted) {
			r->fll_attempted = 1;
			r->fll_status = status;
		}
		if (r->fll_attempts != ~0U)
			r->fll_attempts++;
		if (status == BC_STS_SUCCESS)
			r->fll_confirmed = 1;
		r->phase = status == BC_STS_SUCCESS ? CHD_CYCLE_RESUME_CONFIRMED : CHD_CYCLE_WAKE_CONFIRMED;
	}
	spin_unlock_irqrestore(&shadow_lock, flags);
}

bool crystalhd_pm_cycle_reject_asleep(struct crystalhd_hw *hw)
{
	unsigned long flags;
	bool asleep;

	spin_lock_irqsave(&hw->lock, flags);
	asleep = hw->FleaPowerState == FLEA_PS_LP_COMPLETE || hw->FleaPowerSaved.valid;
	if (asleep && !READ_ONCE(hw->dma_fault)) {
		hw->FleaPowerState = FLEA_PS_FAULT;
		crystalhd_hw_publish_fault(hw);
		crystalhd_pm_shadow_trace(hw, CHD_PM_ASLEEP_CLOSE, FLEA_EVT_STOP_DEVICE,
			FLEA_PS_LP_COMPLETE, FLEA_PS_FAULT, BC_STS_IO_ERROR, CHD_PM_SHADOW_UNKNOWN);
	}
	spin_unlock_irqrestore(&hw->lock, flags);
	return asleep;
}

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
		.snapshot_valid = READ_ONCE(hw->FleaPowerSaved.valid),
	};
	unsigned long flags;
	u32 critical;

	if ((stage == CHD_PM_DOWN_REFUSED || stage == CHD_PM_DOWN_ENTRY || stage == CHD_PM_DOWN_ENTERED) &&
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
	critical = record.violations || record.dma_fault || record.fw_poisoned ||
		record.new_state == FLEA_PS_FAULT || stage == CHD_PM_INIT || stage == CHD_PM_RETIRE ||
		stage == CHD_PM_DOWN_REFUSED || stage == CHD_PM_UP_ENTRY ||
		stage == CHD_PM_ARBITER ||
		stage == CHD_PM_RETENTION ||
		stage == CHD_PM_DOWN_ENTRY || stage == CHD_PM_DOWN_ENTERED ||
		stage == CHD_PM_UP_RESULT || stage == CHD_PM_ASLEEP_CLOSE ||
		stage == CHD_PM_FLL_RESTORE_BEGIN || stage == CHD_PM_FLL_RESTORE ||
		record.old_state != record.new_state ||
		(stage == CHD_PM_SELECT && record.old_state != record.selected_state) ||
		(stage == CHD_PM_FLL && fll == 0);
	spin_lock_irqsave(&shadow_lock, flags);
	if (shadow_ring.cycle.session == record.session) {
		const struct chd_pm_cycle_report *r = &shadow_ring.cycle;

		record.cycle_phase = r->phase;
		if (record.new_state == FLEA_PS_LP_COMPLETE &&
		    !(r->down_result == FLEA_PD_ENTERED &&
		      (((r->phase == CHD_CYCLE_ENTERED || r->phase == CHD_CYCLE_UP_CLAIMED) && record.snapshot_valid) ||
		       r->phase == CHD_CYCLE_WAKE_CONFIRMED)))
			record.violations |= CHD_PM_LP_COMPLETE;
		if (record.new_state == FLEA_PS_ACTIVE && r->up_attempted &&
		    !(r->up_status == BC_STS_SUCCESS && r->fll_confirmed))
			record.violations |= CHD_PM_FLL_EARLY_ACTIVE;
		shadow_ring.cycle.last_state = record.new_state;
		if (record.dma_fault || record.new_state == FLEA_PS_FAULT)
			shadow_ring.cycle.phase = CHD_CYCLE_FAULT;
	} else if (record.new_state == FLEA_PS_LP_COMPLETE) {
		record.violations |= CHD_PM_LP_COMPLETE;
	}
	critical |= record.violations != 0;
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
		   "%u %u %u %u %u %u %u %u %u %u %u %u %u %u %u %u\n",
		   kind, r->sequence, r->time_ns, r->session, r->stage, r->request,
		   r->old_state, r->selected_state, r->new_state,
		   r->reservation, r->status, r->fll, r->violations, r->dma_fault,
		   r->tx_owner, r->irq_owner, r->raw_owner,
		   r->fw_transaction, r->fw_active, r->fw_count,
		   r->fw_poisoned, r->tx_list0, r->tx_list1,
		   r->rx_list0, r->rx_list1, r->empty_count,
		   r->single_fifo_empty, r->rx_pause, r->snapshot_valid, r->cycle_phase);
}

static int shadow_show(struct seq_file *seq, void *unused)
{
	const struct chd_pm_shadow_ring *snapshot = seq->private;
	u32 i, first = (snapshot->next + CHD_PM_SHADOW_RECORDS -
			snapshot->count) % CHD_PM_SHADOW_RECORDS;
	(void)unused;

	seq_printf(seq, "artifact=3.10.3-pm-cycle physical_runtime=one-shot "
		   "normal_stop_device_shutdown=reject-asleep "
		   "capacity=%u total=%llu overwritten=%llu down_refused=%llu "
		   "up_entry=%llu lp_complete=%llu violations=%llu "
		   "receipt_capacity=%u receipt_total=%llu receipt_dropped=%llu\n",
		   CHD_PM_SHADOW_RECORDS, snapshot->total, snapshot->overwritten,
		   snapshot->down_refused, snapshot->up_entry,
		   snapshot->lp_complete, snapshot->violations,
		   CHD_PM_SHADOW_RECEIPTS, snapshot->receipt_total, snapshot->receipt_dropped);
	for (i = 0; i < CHD_PM_STAGE_COUNT; i++)
		seq_printf(seq, "stage-count stage=%u count=%llu\n", i, snapshot->stage_count[i]);
	if (snapshot->cycle.down_attempted) {
		const struct chd_pm_cycle_report *r = &snapshot->cycle;

		seq_printf(seq, "cycle session=%llu down_begin_ns=%llu down_end_ns=%llu "
			   "up_begin_ns=%llu up_end_ns=%llu phase=%u down_attempted=%u "
			   "down_helper_result=%u down_result=%u up_attempted=%u up_helper_status=%u up_status=%u "
			   "fll_attempted=%u fll_status=%u last_state=%u fll_attempts=%u fll_confirmed=%u entry_pause_fll=%u "
			   "entry_snapshot_valid=%u down_snapshot_valid=%u up_snapshot_valid=%u "
			   "wake_snapshot_valid=%u saved_cke=%u tx=%u irq=%u raw=%u "
			   "fw_transaction=%u fw_active=%u fw_count=%u fw_poisoned=%u "
			   "tx0=%u tx1=%u rx0=%u rx1=%u empty=%u single=%u\n",
			   r->session, r->down_begin_ns, r->down_end_ns, r->up_begin_ns, r->up_end_ns,
			   r->phase, r->down_attempted, r->down_helper_result, r->down_result,
			   r->up_attempted, r->up_helper_status, r->up_status,
			   r->fll_attempted, r->fll_status, r->last_state,
			   r->fll_attempts, r->fll_confirmed,
			   r->entry_pause_fll, r->entry_snapshot_valid, r->down_snapshot_valid,
			   r->up_snapshot_valid, r->wake_snapshot_valid, r->saved_cke,
			   r->tx_owner, r->irq_owner, r->raw_owner, r->fw_transaction,
			   r->fw_active, r->fw_count, r->fw_poisoned, r->tx0, r->tx1, r->rx0,
			   r->rx1, r->empty, r->single);
		for (i = 0; i < FLEA_PR_COUNT; i++)
			seq_printf(seq, "cycle-owned index=%u value=%u\n", i, r->saved[i]);
	}
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
		 "fw_active fw_count fw_poisoned tx0 tx1 rx0 rx1 empty single rx_pause snapshot_valid cycle_phase\n");
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
