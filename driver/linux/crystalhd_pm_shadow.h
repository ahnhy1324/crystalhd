/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef CRYSTALHD_PM_SHADOW_H
#define CRYSTALHD_PM_SHADOW_H

#include "crystalhd_pm_arbiter.h"
#include "crystalhd_pm_shadow_ring.h"

struct crystalhd_hw;
int crystalhd_pm_shadow_init(void);
void crystalhd_pm_shadow_cleanup(void);
void crystalhd_pm_shadow_session(struct crystalhd_hw *hw);
void crystalhd_pm_shadow_retire(struct crystalhd_hw *hw);
bool crystalhd_pm_arbiter_claim(void);
void crystalhd_pm_arbiter_record(struct crystalhd_hw *hw,
			       const struct chd_pm_arbiter_report *report);
void crystalhd_pm_shadow_trace(struct crystalhd_hw *hw, u32 stage,
			      u32 request, u32 old_state, u32 selected,
			      u32 status, u32 fll);

#endif
