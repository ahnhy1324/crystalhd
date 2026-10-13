/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef CRYSTALHD_PM_ARBITER_H
#define CRYSTALHD_PM_ARBITER_H

enum chd_pm_arbiter_result {
	CHD_ARB_NOT_RUN,
	CHD_ARB_BASELINE_INVALID,
	CHD_ARB_DISABLE_UNCONFIRMED,
	CHD_ARB_MEMC_INVALID,
	CHD_ARB_TIMEOUT,
	CHD_ARB_GUARDS_REACHED,
};

enum chd_pm_arbiter_restore {
	CHD_ARB_RESTORE_NOT_ATTEMPTED,
	CHD_ARB_RESTORE_CONFIRMED,
	CHD_ARB_RESTORE_UNCONFIRMED,
};

struct chd_pm_arbiter_report {
	u64 begin_ns, end_ns, session;
	u32 result, restore, down_result, touched;
	u32 baseline, disable_readback, restore_live, restore_readback;
	u32 memc_first, memc_last, polls, delay_us;
};

#endif
