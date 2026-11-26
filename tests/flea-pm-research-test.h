/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Actual ledger core; only its host lock, immutable parameters and clock are
 * replaced. The enclosing fixture executes actual selector/down/up/FLL paths. */
static unsigned pm_research_attempts, pm_research_ms;
static u64 research_time;
static bool research_locked;
static u64 pm_now(void) { return research_time; }
static void pm_lock(unsigned long *flags)
{
    check(!research_locked, "ledger lock is not recursive");
    research_locked = true;
    *flags = 0;
}
static void pm_unlock(unsigned long flags)
{
    check(research_locked && !flags, "ledger lock balances");
    research_locked = false;
}
#include "flea-pm-research-core.h"
