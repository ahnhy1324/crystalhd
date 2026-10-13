/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef CRYSTALHD_PM_CYCLE_H
#define CRYSTALHD_PM_CYCLE_H

enum chd_pm_cycle_phase {
	CHD_CYCLE_DOWN_CLAIMED = 1,
	CHD_CYCLE_DOWN_REFUSED,
	CHD_CYCLE_ENTERED,
	CHD_CYCLE_UP_CLAIMED,
	CHD_CYCLE_WAKE_CONFIRMED,
	CHD_CYCLE_RESUME_CONFIRMED,
	CHD_CYCLE_FAULT,
};

struct chd_pm_cycle_report {
	u64 session, down_begin_ns, down_end_ns, up_begin_ns, up_end_ns;
	u32 phase, down_attempted, down_helper_result, down_result;
	u32 up_attempted, up_status, fll_attempted, fll_status, last_state;
	u32 fll_attempts, fll_confirmed;
	u32 up_helper_status;
	u32 entry_pause_fll, entry_snapshot_valid, down_snapshot_valid;
	u32 up_snapshot_valid, wake_snapshot_valid, saved_cke;
	u32 tx_owner, irq_owner, raw_owner, fw_transaction, fw_active, fw_count;
	u32 fw_poisoned, tx0, tx1, rx0, rx1, empty, single;
	u32 saved[FLEA_PR_COUNT];
};

#endif
