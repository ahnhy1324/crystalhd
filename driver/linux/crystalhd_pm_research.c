/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <linux/debugfs.h>
#include <linux/err.h>
#include <linux/ktime.h>
#include <linux/module.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include "crystalhd_hw.h"
#include "crystalhd_lnx.h"
#include "crystalhd_pm_research.h"

/* Both finite limits are required. Zero/zero is the matched OFF control. */
static unsigned int pm_research_attempts;
static unsigned int pm_research_ms;
module_param(pm_research_attempts, uint, 0400);
MODULE_PARM_DESC(pm_research_attempts, "Isolated PM research attempts, 1..8; requires explicit milliseconds; default OFF");
module_param(pm_research_ms, uint, 0400);
MODULE_PARM_DESC(pm_research_ms, "New-down admission window, 1..5000 milliseconds; not a forced-wake deadline");

static DEFINE_SPINLOCK(research_lock);
static struct dentry *research_dir;
static u64 pm_now(void) { return ktime_get_mono_fast_ns(); }
static void pm_lock(unsigned long *flags) { spin_lock_irqsave(&research_lock, *flags); }
static void pm_unlock(unsigned long flags) { spin_unlock_irqrestore(&research_lock, flags); }

/* PM research core begin: exercised verbatim by the device-free harness. */
static struct chd_pm_ledger pm_ledger;

static bool pm_limits_valid(u32 attempts, u32 milliseconds)
{
	return (!attempts && !milliseconds) ||
		(attempts && attempts <= CHD_PM_MAX_ATTEMPTS &&
		 milliseconds && milliseconds <= CHD_PM_MAX_MS);
}

static bool pm_owned(const struct crystalhd_hw *hw)
{
	return hw->FleaResearchSession &&
	       hw->FleaResearchSession == pm_ledger.session && !pm_ledger.retired;
}

/* Caller holds the ledger lock, after hw->lock whenever the latter is needed.
 * Each admitted attempt reserves six entries plus stop/fault/retirement space.
 * IRQ/selector busy decisions never reach this ledger. */
static void pm_record(const struct crystalhd_hw *hw, u32 event, u32 status, u32 fll)
{
	struct chd_pm_record *r;
	u32 i;

	if (pm_ledger.count >= CHD_PM_RECORDS) {
		pm_ledger.dropped++;
		pm_ledger.stop = CHD_PM_CAPACITY;
		return;
	}
	r = &pm_ledger.records[pm_ledger.count++];
	*r = (struct chd_pm_record) {
		.time_ns = pm_now(), .session = hw->FleaResearchSession,
		.event = event, .phase = pm_ledger.phase, .state = hw->FleaPowerState,
		.snapshot = hw->FleaPowerSaved.valid, .status = status, .fll = fll,
		.cleanup = hw->FleaCleanupActive, .transition = hw->FleaPowerTransition,
		.tx = hw->FleaTxActive, .irq = hw->FleaIrqActive, .raw = hw->FleaRawActive,
		.fw_transaction = hw->fwcmd_trans_active, .fw_active = hw->fwcmd_active,
		.fw_count = hw->FwCmdCnt, .fw_poisoned = hw->fwcmd_poisoned,
		.tx0 = hw->TxList0Sts, .tx1 = hw->TxList1Sts,
		.rx0 = hw->rx_list_sts[0], .rx1 = hw->rx_list_sts[1],
		.empty = hw->EmptyCnt, .single = hw->SingleThreadAppFIFOEmpty,
		.saved_cke = hw->FleaPowerSaved.cke,
	};
	for (i = 0; i < FLEA_PR_COUNT; i++)
		r->saved[i] = hw->FleaPowerSaved.value[i];
}

static void pm_stop(const struct crystalhd_hw *hw, u32 reason)
{
	if (pm_ledger.stop != CHD_PM_RUNNING)
		return;
	pm_ledger.stop = reason;
	pm_ledger.stop_ns = pm_now();
	pm_record(hw, CHD_PM_STOP, reason, ~0U);
}

/* Binding is deferred until a real down admission; temporary probe/open/close
 * contexts can arm the host fences without consuming the module-wide claim.
 * A pre-entry failure binds only a failure record, with no admission window. */
static bool pm_bind(struct crystalhd_hw *hw, bool admission)
{
	u64 now = pm_now();
	if (pm_owned(hw))
		return true;
	if (pm_ledger.claimed || !hw->FleaEnablePWM ||
	    !pm_limits_valid(pm_research_attempts, pm_research_ms) || !pm_research_attempts ||
	    (admission && now > ~(u64)0 - (u64)pm_research_ms * 1000000U))
		return false;
	pm_ledger.claimed = true;
	pm_ledger.session = 1;
	pm_ledger.max_attempts = pm_research_attempts;
	pm_ledger.window_ms = pm_research_ms;
	if (admission) {
		pm_ledger.start_ns = now;
		pm_ledger.deadline_ns = now + (u64)pm_research_ms * 1000000U;
	}
	hw->FleaResearchSession = 1;
	pm_record(hw, CHD_PM_SESSION, admission ? BC_STS_SUCCESS : BC_STS_IO_ERROR, ~0U);
	return true;
}

bool crystalhd_pm_research_session(struct crystalhd_hw *hw)
{
	unsigned long flags;
	bool enabled;
	pm_lock(&flags);
	enabled = pm_limits_valid(pm_research_attempts, pm_research_ms) &&
		pm_research_attempts && (!pm_ledger.claimed || pm_owned(hw));
	pm_unlock(flags);
	return enabled;
}

bool crystalhd_pm_research_down_begin(struct crystalhd_hw *hw)
{
	unsigned long flags;
	bool admitted = false;

	pm_lock(&flags);
	/* Called only after the setter's successful zero FLL and final owner
	 * check, under its reservation and hw lock. Do not spend an attempt
	 * for a moving/busy selection or expose physical IO on a bad claim. */
	if (pm_ledger.phase != CHD_PM_IDLE || !hw->FleaPowerTransition ||
	    hw->FleaPowerState != FLEA_PS_LP_PENDING || hw->FleaPowerSaved.valid ||
	    hw->FleaCleanupActive || hw->dma_fault || !hw->adp->present || !hw->dev_started ||
	    hw->FleaTxActive || hw->FleaIrqActive || hw->FleaRawActive ||
	    hw->fwcmd_trans_active || hw->fwcmd_active || hw->FwCmdCnt || hw->fwcmd_poisoned ||
	    hw->TxList0Sts != ListStsFree || hw->TxList1Sts != ListStsFree ||
	    hw->rx_list_sts[0] != sts_free || hw->rx_list_sts[1] != sts_free ||
	    hw->EmptyCnt || hw->SingleThreadAppFIFOEmpty)
		goto out;
	if (!pm_bind(hw, true) || pm_ledger.stop != CHD_PM_RUNNING)
		goto out;
	if (pm_now() >= pm_ledger.deadline_ns) {
		pm_stop(hw, CHD_PM_DEADLINE);
		goto out;
	}
	if (pm_ledger.attempts == pm_ledger.max_attempts) {
		pm_stop(hw, CHD_PM_LIMIT);
		goto out;
	}
	if (pm_ledger.count > CHD_PM_RECORDS - 9U) {
		pm_stop(hw, CHD_PM_CAPACITY);
		goto out;
	}
	pm_ledger.attempts++;
	pm_ledger.phase = CHD_PM_DOWN;
	pm_record(hw, CHD_PM_DOWN_BEGIN, BC_STS_SUCCESS, 0);
	admitted = true;
out:
	pm_unlock(flags);
	return admitted;
}

enum FLEA_POWER_DOWN_RESULT crystalhd_pm_research_down_end(
	struct crystalhd_hw *hw, enum FLEA_POWER_DOWN_RESULT result)
{
	unsigned long flags;

	pm_lock(&flags);
	if (!pm_owned(hw)) {
		pm_unlock(flags);
		return FLEA_PD_FAULT;
	}
	if (pm_ledger.phase != CHD_PM_DOWN ||
	    (result != FLEA_PD_ENTERED && result != FLEA_PD_REFUSED && result != FLEA_PD_FAULT) ||
	    (result == FLEA_PD_ENTERED && !hw->FleaPowerSaved.valid) ||
	    (result == FLEA_PD_REFUSED && hw->FleaPowerSaved.valid))
		result = FLEA_PD_FAULT;
	if (result == FLEA_PD_ENTERED) {
		pm_ledger.entered++;
		pm_ledger.phase = CHD_PM_ASLEEP;
	} else if (result == FLEA_PD_REFUSED) {
		pm_ledger.refused++;
		pm_ledger.phase = CHD_PM_IDLE;
	} else {
		pm_ledger.failed++;
		pm_ledger.phase = CHD_PM_UNCERTAIN;
	}
	pm_record(hw, CHD_PM_DOWN_END, result, ~0U);
	if (result != FLEA_PD_ENTERED)
		pm_stop(hw, result == FLEA_PD_REFUSED ? CHD_PM_REFUSAL : CHD_PM_FAILURE);
	pm_unlock(flags);
	return result;
}

bool crystalhd_pm_research_up_begin(struct crystalhd_hw *hw)
{
	unsigned long flags;
	bool admitted;

	pm_lock(&flags);
	/* An outstanding wake is permitted after either admission limit. */
	admitted = pm_owned(hw) && pm_ledger.phase == CHD_PM_ASLEEP &&
		hw->FleaPowerTransition && hw->FleaPowerState == FLEA_PS_LP_COMPLETE &&
		hw->FleaPowerSaved.valid && !hw->dma_fault && !hw->FleaCleanupActive &&
		hw->adp->present && hw->dev_started;
	if (admitted) {
		pm_ledger.phase = CHD_PM_UP;
		pm_ledger.up_attempts++;
		pm_record(hw, CHD_PM_UP_BEGIN, BC_STS_SUCCESS, ~0U);
	}
	pm_unlock(flags);
	return admitted;
}

BC_STATUS crystalhd_pm_research_up_end(struct crystalhd_hw *hw, BC_STATUS status)
{
	unsigned long flags;

	pm_lock(&flags);
	if (!pm_owned(hw)) {
		pm_unlock(flags);
		return BC_STS_IO_ERROR;
	}
	if (pm_ledger.phase != CHD_PM_UP ||
	    (status == BC_STS_SUCCESS && hw->FleaPowerSaved.valid))
		status = BC_STS_IO_ERROR;
	if (status == BC_STS_SUCCESS) {
		pm_ledger.up_completed++;
		pm_ledger.phase = CHD_PM_NEEDS_FLL;
	} else {
		pm_ledger.up_failed++;
		pm_ledger.phase = CHD_PM_UNCERTAIN;
		pm_stop(hw, CHD_PM_FAILURE);
	}
	pm_record(hw, CHD_PM_UP_END, status, ~0U);
	pm_unlock(flags);
	return status;
}

void crystalhd_pm_research_fll(struct crystalhd_hw *hw, BC_STATUS status, u32 count)
{
	unsigned long flags;

	pm_lock(&flags);
	if (status != BC_STS_SUCCESS)
		pm_bind(hw, false);
	if (pm_owned(hw) && (pm_ledger.phase == CHD_PM_NEEDS_FLL || status != BC_STS_SUCCESS)) {
		if (status == BC_STS_SUCCESS) {
			pm_ledger.fll_completed++;
			pm_ledger.phase = CHD_PM_IDLE;
		} else {
			pm_ledger.fll_failed++;
			pm_ledger.phase = CHD_PM_UNCERTAIN;
			pm_stop(hw, CHD_PM_FAILURE);
		}
		pm_record(hw, CHD_PM_FLL, status, count);
	}
	pm_unlock(flags);
}

void crystalhd_pm_research_fault(struct crystalhd_hw *hw)
{
	unsigned long flags;

	pm_lock(&flags);
	pm_bind(hw, false);
	if (pm_owned(hw) && !pm_ledger.fault_recorded) {
		pm_stop(hw, CHD_PM_FAILURE);
		/* The actual outstanding phase is retained in the fault record. */
		pm_record(hw, CHD_PM_FAULT, BC_STS_IO_ERROR, ~0U);
		pm_ledger.fault_recorded = true;
	}
	pm_unlock(flags);
}

void crystalhd_pm_research_retire(struct crystalhd_hw *hw)
{
	unsigned long flags;

	pm_lock(&flags);
	if (pm_owned(hw)) {
		pm_stop(hw, CHD_PM_RETIRED);
		pm_record(hw, CHD_PM_RETIRE, BC_STS_SUCCESS, ~0U);
		pm_ledger.retired = true;
	}
	pm_unlock(flags);
}
/* PM research core end. */

static int research_show(struct seq_file *seq, void *unused)
{
	struct chd_pm_ledger *snapshot;
	unsigned long flags;
	u32 i, j;
	(void)unused;

	snapshot = kmalloc(sizeof(*snapshot), GFP_KERNEL);
	if (!snapshot)
		return -ENOMEM;
	pm_lock(&flags);
	*snapshot = pm_ledger;
	pm_unlock(flags);
	seq_printf(seq, "pm-research=finite1 attempts_limit=%u window_ms=%u capacity=%u claimed=%u retired=%u phase=%u stop=%u count=%u dropped=%u start_ns=%llu deadline_ns=%llu stop_ns=%llu read_ns=%llu attempts=%u entered=%u refused=%u failed=%u up_attempts=%u up_completed=%u up_failed=%u fll_completed=%u fll_failed=%u\n",
		pm_research_attempts, pm_research_ms, CHD_PM_RECORDS,
		snapshot->claimed, snapshot->retired, snapshot->phase, snapshot->stop,
		snapshot->count, snapshot->dropped, snapshot->start_ns,
		snapshot->deadline_ns, snapshot->stop_ns, pm_now(), snapshot->attempts,
		snapshot->entered, snapshot->refused, snapshot->failed,
		snapshot->up_attempts, snapshot->up_completed, snapshot->up_failed,
		snapshot->fll_completed, snapshot->fll_failed);
	for (i = 0; i < snapshot->count; i++) {
		const struct chd_pm_record *r = &snapshot->records[i];
		seq_printf(seq, "record=%u time_ns=%llu session=%llu event=%u phase=%u state=%u snapshot=%u status=%u fll=%u cleanup=%u transition=%u tx=%u irq=%u raw=%u fw_transaction=%u fw_active=%u fw_count=%u fw_poisoned=%u tx0=%u tx1=%u rx0=%u rx1=%u empty=%u single=%u cke=%u saved=",
			i, r->time_ns, r->session, r->event, r->phase, r->state,
			r->snapshot, r->status, r->fll, r->cleanup, r->transition,
			r->tx, r->irq, r->raw, r->fw_transaction, r->fw_active,
			r->fw_count, r->fw_poisoned, r->tx0, r->tx1, r->rx0, r->rx1,
			r->empty, r->single, r->saved_cke);
		for (j = 0; j < FLEA_PR_COUNT; j++)
			seq_printf(seq, "%s%08x", j ? "," : "", r->saved[j]);
		seq_putc(seq, '\n');
	}
	kfree(snapshot);
	return 0;
}

static int research_open(struct inode *inode, struct file *file)
{
	return single_open(file, research_show, inode->i_private);
}
static const struct file_operations research_fops = {
	.owner = THIS_MODULE, .open = research_open, .read = seq_read,
	.llseek = seq_lseek, .release = single_release,
};

int crystalhd_pm_research_init(void)
{
	struct dentry *file;
	if (!pm_limits_valid(pm_research_attempts, pm_research_ms))
		return -EINVAL;
	research_dir = debugfs_create_dir("crystalhd-pm-research", NULL);
	if (IS_ERR_OR_NULL(research_dir))
		return research_dir ? PTR_ERR(research_dir) : -ENOMEM;
	file = debugfs_create_file("trace", 0400, research_dir, NULL, &research_fops);
	if (IS_ERR_OR_NULL(file)) {
		int status = file ? PTR_ERR(file) : -ENOMEM;
		debugfs_remove_recursive(research_dir);
		research_dir = NULL;
		return status;
	}
	return 0;
}
void crystalhd_pm_research_cleanup(void)
{
	debugfs_remove_recursive(research_dir);
	research_dir = NULL;
}
