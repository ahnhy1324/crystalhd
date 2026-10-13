/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef CRYSTALHD_PM_RETENTION_H
#define CRYSTALHD_PM_RETENTION_H

enum chd_pm_retention_phase {
	CHD_RET_BASELINE = 1,
	CHD_RET_ARBITER_DISABLE,
	CHD_RET_READINESS,
	CHD_RET_PARAMS_REQUEST,
	CHD_RET_ENTRY_COMMAND,
	CHD_RET_REFRESH_DISABLE,
	CHD_RET_CKE_LOW,
	CHD_RET_REFRESH_RESTORE,
	CHD_RET_PARAMS_RESTORE,
	CHD_RET_WAKE_COMMAND,
	CHD_RET_CKE_HIGH,
	CHD_RET_ARBITER_RESTORE,
	CHD_RET_FINISHED,
};

enum chd_pm_retention_result {
	CHD_RET_BASELINE_REFUSED = 1,
	CHD_RET_COMPLETED_CYCLE,
	CHD_RET_CHECKED_ROLLBACK,
	CHD_RET_UNCONFIRMED,
};

enum chd_pm_retention_poll_result {
	CHD_RET_POLL_REACHED = 1,
	CHD_RET_POLL_TIMEOUT,
	CHD_RET_POLL_INVALID,
};

struct chd_pm_retention_poll {
	u32 first, last, polls, delay_us, result;
};

struct chd_pm_retention_report {
	u64 session, begin_ns, end_ns;
	u32 phase, failure, rollback_failure, result, down_result;
	u32 arbiter_touched, ddr_requested, settle_us;
	u32 master_baseline, params_baseline, refresh_baseline, memc_baseline;
	u32 disable_readback, master_restore_live, master_restore_readback;
	u32 params_entry_live, params_entry_readback;
	u32 refresh_entry_live, refresh_entry_readback;
	u32 refresh_restore_live, refresh_restore_readback;
	u32 params_restore_live, params_restore_readback;
	struct chd_pm_retention_poll readiness, entry, wake;
};

#endif
