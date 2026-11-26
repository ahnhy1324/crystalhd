/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef CRYSTALHD_PM_RESEARCH_H
#define CRYSTALHD_PM_RESEARCH_H

/* Research-only derivative of e38a2f8's one-cycle numeric ledger. No target
 * register access, allocation, scheduling or hardware pointers in records. */
#define CHD_PM_MAX_ATTEMPTS 8U
#define CHD_PM_MAX_MS 5000U
#define CHD_PM_RECORDS 64U

enum chd_pm_phase {
	CHD_PM_IDLE, CHD_PM_DOWN, CHD_PM_ASLEEP, CHD_PM_UP,
	CHD_PM_NEEDS_FLL, CHD_PM_UNCERTAIN,
};
enum chd_pm_stop {
	CHD_PM_RUNNING, CHD_PM_LIMIT, CHD_PM_DEADLINE, CHD_PM_REFUSAL,
	CHD_PM_FAILURE, CHD_PM_CAPACITY, CHD_PM_RETIRED,
};
enum chd_pm_event {
	CHD_PM_SESSION, CHD_PM_DOWN_BEGIN, CHD_PM_DOWN_END,
	CHD_PM_UP_BEGIN, CHD_PM_UP_END, CHD_PM_FLL,
	CHD_PM_STOP, CHD_PM_FAULT, CHD_PM_RETIRE,
};
struct chd_pm_record {
	u64 time_ns, session;
	u32 event, phase, state, snapshot, status, fll, cleanup, transition;
	u32 tx, irq, raw, fw_transaction, fw_active, fw_count, fw_poisoned;
	u32 tx0, tx1, rx0, rx1, empty, single, saved_cke;
	u32 saved[FLEA_PR_COUNT];
};
struct chd_pm_ledger {
	u64 session, start_ns, deadline_ns, stop_ns;
	u32 max_attempts, window_ms, attempts, entered, refused, failed;
	u32 up_attempts, up_completed, up_failed, fll_completed, fll_failed;
	u32 phase, stop, count, dropped;
	bool claimed, fault_recorded, retired;
	struct chd_pm_record records[CHD_PM_RECORDS];
};

#ifdef CRYSTALHD_ENABLE_PM_RESEARCH
struct crystalhd_hw;
int crystalhd_pm_research_init(void);
void crystalhd_pm_research_cleanup(void);
bool crystalhd_pm_research_session(struct crystalhd_hw *hw);
bool crystalhd_pm_research_down_begin(struct crystalhd_hw *hw);
enum FLEA_POWER_DOWN_RESULT crystalhd_pm_research_down_end(
	struct crystalhd_hw *hw, enum FLEA_POWER_DOWN_RESULT result);
bool crystalhd_pm_research_up_begin(struct crystalhd_hw *hw);
BC_STATUS crystalhd_pm_research_up_end(struct crystalhd_hw *hw, BC_STATUS status);
void crystalhd_pm_research_fll(struct crystalhd_hw *hw, BC_STATUS status, u32 count);
void crystalhd_pm_research_fault(struct crystalhd_hw *hw);
void crystalhd_pm_research_retire(struct crystalhd_hw *hw);
#endif
#endif
