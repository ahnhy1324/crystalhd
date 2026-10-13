/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Actual immutable self-refresh-only protocol, shared by kernel and mocks. */
#ifndef CRYSTALHD_PM_RETENTION_OPS_H
#define CRYSTALHD_PM_RETENTION_OPS_H

static inline bool chd_retention_read(struct crystalhd_hw *hw, u32 address,
				      u32 reserved, u32 *value)
{
	*value = hw->pfnReadDevRegister(hw->adp, address);
	return *value != ~0U && !(*value & reserved);
}

static inline bool chd_retention_update(struct crystalhd_hw *hw, u32 address,
		u32 reserved, u32 owned, u32 desired, u32 *live, u32 *readback)
{
	if (!chd_retention_read(hw, address, reserved, live))
		return false;
	hw->pfnWriteDevRegister(hw->adp, address, (*live & ~owned) | (desired & owned));
	return chd_retention_read(hw, address, reserved, readback) &&
	       (*readback & owned) == (desired & owned);
}

static inline bool chd_retention_poll(struct crystalhd_hw *hw, u32 mask,
				      u32 desired, struct chd_pm_retention_poll *poll)
{
	const u32 known = BCHP_DDR23_CTL_REGS_0_CTL_STATUS_idle_MASK |
		BCHP_DDR23_CTL_REGS_0_CTL_STATUS_rd_fifo1_empty_MASK |
		BCHP_DDR23_CTL_REGS_0_CTL_STATUS_rd_fifo2_empty_MASK |
		BCHP_DDR23_CTL_REGS_0_CTL_STATUS_rd_fifo3_empty_MASK |
		BCHP_DDR23_CTL_REGS_0_CTL_STATUS_clke_MASK;
	u32 value, i;

	*poll = (struct chd_pm_retention_poll) {
		.first = ~0U, .last = ~0U, .result = CHD_RET_POLL_TIMEOUT,
	};
	for (i = 0; i < 21; i++) {
		value = hw->pfnReadDevRegister(hw->adp, BCHP_DDR23_CTL_REGS_0_CTL_STATUS);
		if (!i)
			poll->first = value;
		poll->last = value;
		poll->polls++;
		if (value & ~known) {
			poll->result = CHD_RET_POLL_INVALID;
			return false;
		}
		if ((value & mask) == desired) {
			poll->result = CHD_RET_POLL_REACHED;
			return true;
		}
		if (i < 20) {
			udelay(10);
			poll->delay_us += 10;
		}
	}
	return false;
}

static inline enum FLEA_POWER_DOWN_RESULT crystalhd_flea_retention_cycle(
	struct crystalhd_hw *hw, struct chd_pm_retention_report *r)
{
	const u32 arb = BCHP_PRI_ARB_CONTROL_REGS_MASTER_CTL_arb_disable_MASK;
	const u32 arb_reserved = BCHP_PRI_ARB_CONTROL_REGS_MASTER_CTL_reserved0_MASK;
	const u32 params = BCHP_DDR23_CTL_REGS_0_PARAMS2_clke_MASK;
	const u32 params_reserved = BCHP_DDR23_CTL_REGS_0_PARAMS2_reserved0_MASK;
	const u32 refresh = BCHP_PRI_ARB_CONTROL_REGS_REFRESH_CTL_0_enable_MASK;
	const u32 refresh_reserved = BCHP_PRI_ARB_CONTROL_REGS_REFRESH_CTL_0_reserved0_MASK;
	const u32 cke = BCHP_DDR23_CTL_REGS_0_CTL_STATUS_clke_MASK;
	const u32 known = BCHP_DDR23_CTL_REGS_0_CTL_STATUS_idle_MASK |
		BCHP_DDR23_CTL_REGS_0_CTL_STATUS_rd_fifo1_empty_MASK |
		BCHP_DDR23_CTL_REGS_0_CTL_STATUS_rd_fifo2_empty_MASK |
		BCHP_DDR23_CTL_REGS_0_CTL_STATUS_rd_fifo3_empty_MASK | cke;

	lockdep_assert_held(&hw->lock);
	*r = (struct chd_pm_retention_report) {
		.session = hw->FleaShadowSession, .begin_ns = ktime_get_mono_fast_ns(),
		.phase = CHD_RET_BASELINE, .result = CHD_RET_BASELINE_REFUSED,
		.down_result = FLEA_PD_REFUSED, .master_baseline = ~0U,
		.params_baseline = ~0U, .refresh_baseline = ~0U, .memc_baseline = ~0U,
		.disable_readback = ~0U, .master_restore_live = ~0U,
		.master_restore_readback = ~0U, .params_entry_live = ~0U,
		.params_entry_readback = ~0U, .refresh_entry_live = ~0U,
		.refresh_entry_readback = ~0U, .refresh_restore_live = ~0U,
		.refresh_restore_readback = ~0U, .params_restore_live = ~0U,
		.params_restore_readback = ~0U,
		.readiness = {.first = ~0U, .last = ~0U},
		.entry = {.first = ~0U, .last = ~0U},
		.wake = {.first = ~0U, .last = ~0U},
	};
	if (!chd_retention_read(hw, BCHP_PRI_ARB_CONTROL_REGS_MASTER_CTL,
		arb_reserved, &r->master_baseline) || r->master_baseline != 0 ||
	    !chd_retention_read(hw, BCHP_DDR23_CTL_REGS_0_PARAMS2,
		params_reserved, &r->params_baseline) || (r->params_baseline & params) ||
	    !chd_retention_read(hw, BCHP_PRI_ARB_CONTROL_REGS_REFRESH_CTL_0,
		refresh_reserved, &r->refresh_baseline) || !(r->refresh_baseline & refresh) ||
	    !chd_retention_read(hw, BCHP_DDR23_CTL_REGS_0_CTL_STATUS,
		~known, &r->memc_baseline) || !(r->memc_baseline & cke)) {
		r->failure = CHD_RET_BASELINE;
		goto done;
	}
	r->phase = CHD_RET_ARBITER_DISABLE;
	r->arbiter_touched = 1;
	hw->pfnWriteDevRegister(hw->adp, BCHP_PRI_ARB_CONTROL_REGS_MASTER_CTL,
		(r->master_baseline & ~arb) | arb);
	if (!chd_retention_read(hw, BCHP_PRI_ARB_CONTROL_REGS_MASTER_CTL,
		arb_reserved, &r->disable_readback) || !(r->disable_readback & arb))
		goto recover;
	r->phase = CHD_RET_READINESS;
	if (!chd_retention_poll(hw, known, known, &r->readiness))
		goto recover;
	r->phase = CHD_RET_PARAMS_REQUEST;
	/* Even ambiguous publication owns rollback until verified CKE-high. */
	r->ddr_requested = 1;
	if (!chd_retention_update(hw, BCHP_DDR23_CTL_REGS_0_PARAMS2,
		params_reserved, params, params, &r->params_entry_live, &r->params_entry_readback))
		goto recover;
	r->phase = CHD_RET_ENTRY_COMMAND;
	hw->pfnWriteDevRegister(hw->adp, BCHP_DDR23_CTL_REGS_0_REFRESH_CMD, 0x60);
	r->phase = CHD_RET_REFRESH_DISABLE;
	if (!chd_retention_update(hw, BCHP_PRI_ARB_CONTROL_REGS_REFRESH_CTL_0,
		refresh_reserved, refresh, 0, &r->refresh_entry_live, &r->refresh_entry_readback))
		goto recover;
	r->phase = CHD_RET_CKE_LOW;
	if (!chd_retention_poll(hw, cke, 0, &r->entry))
		goto recover;
	r->result = CHD_RET_COMPLETED_CYCLE;
	goto restore;

recover:
	r->failure = r->phase;
	r->result = CHD_RET_CHECKED_ROLLBACK;
restore:
	if (r->ddr_requested) {
		r->phase = CHD_RET_REFRESH_RESTORE;
		if (!chd_retention_update(hw, BCHP_PRI_ARB_CONTROL_REGS_REFRESH_CTL_0,
			refresh_reserved, refresh, r->refresh_baseline,
			&r->refresh_restore_live, &r->refresh_restore_readback))
			goto fault;
		r->phase = CHD_RET_PARAMS_RESTORE;
		if (!chd_retention_update(hw, BCHP_DDR23_CTL_REGS_0_PARAMS2,
			params_reserved, params, r->params_baseline,
			&r->params_restore_live, &r->params_restore_readback))
			goto fault;
		udelay(5);
		r->settle_us = 5;
		r->phase = CHD_RET_WAKE_COMMAND;
		hw->pfnWriteDevRegister(hw->adp, BCHP_DDR23_CTL_REGS_0_REFRESH_CMD, 0x60);
	}
	r->phase = CHD_RET_CKE_HIGH;
	if (!chd_retention_poll(hw, cke, cke, &r->wake))
		goto fault;
	r->phase = CHD_RET_ARBITER_RESTORE;
	if (!chd_retention_update(hw, BCHP_PRI_ARB_CONTROL_REGS_MASTER_CTL,
		arb_reserved, arb, r->master_baseline,
		&r->master_restore_live, &r->master_restore_readback))
		goto fault;
	r->phase = CHD_RET_FINISHED;
	goto done;
fault:
	r->rollback_failure = r->phase;
	r->result = CHD_RET_UNCONFIRMED;
	r->down_result = FLEA_PD_FAULT;
done:
	r->end_ns = ktime_get_mono_fast_ns();
	return r->down_result;
}

#endif
