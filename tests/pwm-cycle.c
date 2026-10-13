/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Actual one-cycle ledger/wrappers/setter; physical helpers are device-free stubs. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef uint32_t u32;
typedef unsigned long long u64;
typedef unsigned int BC_STATUS;
#define BC_STS_SUCCESS 0U
#define BC_STS_IO_ERROR 7U
#define BC_STS_ERR_USAGE 2U
#define READ_ONCE(v) (v)
#define ListStsFree 0U
#define sts_free 0U
#define CHECK(e) do { assert(e); checks++; } while (0)
#include "pwm-cycle-types.h"
#include "crystalhd_pm_arbiter.h"
#include "crystalhd_pm_retention.h"
#include "crystalhd_pm_cycle.h"
#include "crystalhd_pm_shadow_ring.h"

struct mock_adp { u32 present; };
struct mock_saved { u32 value[FLEA_PR_COUNT], cke; bool valid; };
struct crystalhd_hw {
    struct mock_adp *adp;
    int lock;
    u64 FleaShadowSession;
    u32 FleaPowerState, FleaPowerTransition, FleaCyclePauseFll;
    u32 FleaEnablePWM, dma_fault, dev_started;
    struct mock_saved FleaPowerSaved;
    u32 FleaTxActive, FleaIrqActive, FleaRawActive, fwcmd_trans_active;
    u32 fwcmd_active, FwCmdCnt, fwcmd_poisoned, TxList0Sts, TxList1Sts;
    u32 rx_list_sts[2], EmptyCnt, SingleThreadAppFIFOEmpty, hw_pause_issued;
    u32 FleaFLLUpdateAddr, PwrDwnPiQIntr, PwrDwnTxIntr;
    BC_STATUS (*pfnDevDRAMWrite)(struct crystalhd_hw *, u32, u32, u32 *);
};

static struct chd_pm_shadow_ring shadow_ring;
static int shadow_lock, shadow_depth, hw_depth;
static u64 now;
static unsigned checks, physical_downs, physical_ups, faults;
static unsigned fll_writes, picture_replays, tx_replays;
static u32 down_result, up_result, fll_result;
static bool down_without_snapshot, up_leaves_snapshot;

#define spin_lock_irqsave(lockp, flags) do { \
    (flags) = 0; \
    if ((void *)(lockp) == (void *)&shadow_lock) { CHECK(shadow_depth == 0); shadow_depth++; } \
    else { CHECK(shadow_depth == 0 && hw_depth == 0); hw_depth++; } \
} while (0)
#define spin_unlock_irqrestore(lockp, flags) do { \
    (void)(flags); \
    if ((void *)(lockp) == (void *)&shadow_lock) { CHECK(shadow_depth == 1); shadow_depth--; } \
    else { CHECK(shadow_depth == 0 && hw_depth == 1); hw_depth--; } \
} while (0)

static u64 ktime_get_mono_fast_ns(void) { return ++now; }
void crystalhd_pm_shadow_trace(struct crystalhd_hw *, u32, u32, u32, u32, u32, u32);
static void crystalhd_hw_publish_fault(struct crystalhd_hw *hw)
{
    CHECK(!shadow_depth && hw_depth == 1);
    hw->dma_fault = 1;
    hw->adp->present = 0;
    faults++;
}

#include "pwm-cycle-ledger.h"
#include "pwm-cycle-observer.h"

static enum FLEA_POWER_DOWN_RESULT crystalhd_flea_physical_power_dn(struct crystalhd_hw *hw)
{
    CHECK(hw_depth == 1 && !shadow_depth && hw->FleaPowerTransition);
    CHECK(shadow_ring.cycle.phase == CHD_CYCLE_DOWN_CLAIMED);
    physical_downs++;
    hw->FleaPowerSaved.valid = down_result == FLEA_PD_ENTERED && !down_without_snapshot;
    if (hw->FleaPowerSaved.valid) {
        hw->FleaPowerSaved.cke = 2;
        for (unsigned i = 0; i < FLEA_PR_COUNT; i++)
            hw->FleaPowerSaved.value[i] = 0x1200U + i; /* Synthetic ledger words, not MMIO. */
    }
    if (down_result == FLEA_PD_FAULT) {
        hw->FleaPowerState = FLEA_PS_FAULT;
        crystalhd_hw_publish_fault(hw);
    }
    return down_result;
}
static BC_STATUS crystalhd_flea_physical_power_up(struct crystalhd_hw *hw)
{
    CHECK(hw_depth == 1 && !shadow_depth && hw->FleaPowerTransition);
    CHECK(shadow_ring.cycle.phase == CHD_CYCLE_UP_CLAIMED);
    physical_ups++;
    if (up_result == BC_STS_SUCCESS && !up_leaves_snapshot)
        hw->FleaPowerSaved.valid = false;
    if (up_result != BC_STS_SUCCESS) {
        hw->FleaPowerState = FLEA_PS_FAULT;
        crystalhd_hw_publish_fault(hw);
    }
    return up_result;
}
static u32 crystalhd_hw_count_free_rx_pkts(struct crystalhd_hw *hw)
{
    (void)hw;
    CHECK(hw_depth == 0 && !shadow_depth);
    return 8;
}
static BC_STATUS fll_write(struct crystalhd_hw *hw, u32 address, u32 words, u32 *value)
{
    CHECK(hw_depth == 1 && !shadow_depth && address == hw->FleaFLLUpdateAddr);
    CHECK(words == 1 && value && (*value == 0 || *value == 8));
    if (*value) CHECK(!hw->FleaPowerSaved.valid && hw->FleaPowerState != FLEA_PS_LP_COMPLETE);
    fll_writes++;
    return fll_result;
}
static void crystalhd_flea_handle_PicQSts_intr(struct crystalhd_hw *hw)
{
    CHECK(!hw_depth && !shadow_depth && hw->FleaPowerState == FLEA_PS_ACTIVE);
    CHECK(!hw->FleaPowerSaved.valid && shadow_ring.cycle.fll_confirmed);
    picture_replays++;
}
static void crystalhd_flea_update_tx_buff_info(struct crystalhd_hw *hw)
{
    CHECK(!hw_depth && !shadow_depth && hw->FleaPowerState == FLEA_PS_ACTIVE);
    CHECK(!hw->FleaPowerSaved.valid && shadow_ring.cycle.fll_confirmed);
    tx_replays++;
}

#include "pwm-cycle-wrappers.h"

static struct crystalhd_hw prepare(struct mock_adp *adp)
{
    struct crystalhd_hw hw = {0};
    CHECK(!hw_depth && !shadow_depth);
    memset(&shadow_ring, 0, sizeof(shadow_ring)); /* Test-only fresh module instance. */
    adp->present = 1;
    hw.adp = adp;
    hw.FleaShadowSession = 4;
    hw.FleaPowerState = FLEA_PS_LP_PENDING;
    hw.FleaPowerTransition = hw.FleaCyclePauseFll = 1;
    hw.FleaEnablePWM = hw.dev_started = 1;
    hw.FleaFLLUpdateAddr = 0x2000;
    hw.pfnDevDRAMWrite = fll_write;
    physical_downs = physical_ups = faults = fll_writes = 0;
    picture_replays = tx_replays = 0;
    down_result = FLEA_PD_ENTERED;
    up_result = fll_result = BC_STS_SUCCESS;
    down_without_snapshot = up_leaves_snapshot = false;
    return hw;
}
static void lock_hw(void) { CHECK(!hw_depth && !shadow_depth); hw_depth = 1; }
static void unlock_hw(void) { CHECK(hw_depth == 1 && !shadow_depth); hw_depth = 0; }
static void enter(struct crystalhd_hw *hw)
{
    CHECK(crystalhd_flea_set_power_state(hw, FLEA_PS_LP_COMPLETE) == BC_STS_SUCCESS);
    CHECK(hw->FleaPowerState == FLEA_PS_LP_COMPLETE && hw->FleaPowerSaved.valid);
    CHECK(physical_downs == 1 && shadow_ring.cycle.phase == CHD_CYCLE_ENTERED);
}
static struct chd_pm_shadow_record *last_record(void)
{
    return &shadow_ring.record[(shadow_ring.next + CHD_PM_SHADOW_RECORDS - 1) % CHD_PM_SHADOW_RECORDS];
}

int main(void)
{
    struct mock_adp adp;
    struct crystalhd_hw hw;
    unsigned i;

    /* One real setter-level round trip, with saved identity and linked replay. */
    hw = prepare(&adp);
    enter(&hw);
    CHECK(shadow_ring.cycle.session == 4 && shadow_ring.cycle.down_attempted == 1);
    CHECK(shadow_ring.cycle.entry_pause_fll && !shadow_ring.cycle.entry_snapshot_valid);
    CHECK(shadow_ring.cycle.down_snapshot_valid && shadow_ring.cycle.saved_cke == 2);
    for (i = 0; i < FLEA_PR_COUNT; i++) CHECK(shadow_ring.cycle.saved[i] == 0x1200U + i);
    CHECK(!last_record()->violations && last_record()->snapshot_valid);
    hw.PwrDwnPiQIntr = hw.PwrDwnTxIntr = 1;
    CHECK(crystalhd_flea_set_power_state(&hw, FLEA_PS_ACTIVE) == BC_STS_SUCCESS);
    CHECK(hw.FleaPowerState == FLEA_PS_ACTIVE && !hw.FleaPowerSaved.valid && !hw.FleaCyclePauseFll);
    CHECK(physical_ups == 1 && picture_replays == 1 && tx_replays == 1 && !faults);
    CHECK(shadow_ring.cycle.phase == CHD_CYCLE_RESUME_CONFIRMED &&
          shadow_ring.cycle.up_attempted == 1 && shadow_ring.cycle.fll_attempted &&
          shadow_ring.cycle.fll_attempts == 1 && shadow_ring.cycle.fll_confirmed);
    CHECK(!last_record()->violations && !crystalhd_pm_cycle_reject_asleep(&hw));
    lock_hw();
    CHECK(crystalhd_flea_runtime_power_dn(&hw) == FLEA_PD_REFUSED && physical_downs == 1);
    crystalhd_pm_cycle_fll_record(&hw, BC_STS_IO_ERROR);
    CHECK(shadow_ring.cycle.fll_status == BC_STS_SUCCESS && shadow_ring.cycle.fll_attempts == 1);
    unlock_hw();

    /* Refused capture consumes the immutable attempt and admits no wake. */
    hw = prepare(&adp); down_result = FLEA_PD_REFUSED;
    lock_hw();
    CHECK(crystalhd_flea_runtime_power_dn(&hw) == FLEA_PD_REFUSED && physical_downs == 1);
    CHECK(shadow_ring.cycle.phase == CHD_CYCLE_DOWN_REFUSED && !hw.FleaPowerSaved.valid);
    CHECK(crystalhd_flea_runtime_power_dn(&hw) == FLEA_PD_REFUSED && physical_downs == 1);
    hw.FleaPowerState = FLEA_PS_LP_COMPLETE; hw.FleaPowerSaved.valid = true;
    CHECK(crystalhd_flea_runtime_power_up(&hw) == BC_STS_IO_ERROR && !physical_ups && faults == 1);
    unlock_hw();

    /* Admission refusals occur before helpers, but still consume one attempt. */
    for (i = 0; i < 19; i++) {
        hw = prepare(&adp);
        u32 *owners[] = {&hw.FleaTxActive, &hw.FleaIrqActive, &hw.FleaRawActive,
            &hw.fwcmd_trans_active, &hw.fwcmd_active, &hw.FwCmdCnt, &hw.fwcmd_poisoned,
            &hw.TxList0Sts, &hw.TxList1Sts, &hw.rx_list_sts[0], &hw.rx_list_sts[1],
            &hw.EmptyCnt, &hw.SingleThreadAppFIFOEmpty};
        if (i < 13) *owners[i] = 1;
        else if (i == 13) hw.FleaPowerTransition = 0;
        else if (i == 14) hw.FleaCyclePauseFll = 0;
        else if (i == 15) hw.FleaPowerState = FLEA_PS_ACTIVE;
        else if (i == 16) adp.present = 0;
        else if (i == 17) hw.dev_started = 0;
        else hw.FleaPowerSaved.valid = true;
        lock_hw();
        CHECK(crystalhd_flea_runtime_power_dn(&hw) == FLEA_PD_REFUSED && !physical_downs);
        CHECK(shadow_ring.cycle.down_attempted && shadow_ring.cycle.phase == CHD_CYCLE_DOWN_REFUSED);
        CHECK(crystalhd_flea_runtime_power_dn(&hw) == FLEA_PD_REFUSED && !physical_downs);
        unlock_hw();
    }

    /* A helper cannot report entry without its newly captured snapshot. */
    hw = prepare(&adp); down_without_snapshot = true;
    lock_hw();
    CHECK(crystalhd_flea_runtime_power_dn(&hw) == FLEA_PD_FAULT && hw.dma_fault && !adp.present);
    CHECK(shadow_ring.cycle.down_helper_result == FLEA_PD_ENTERED &&
          shadow_ring.cycle.down_result == FLEA_PD_FAULT && shadow_ring.cycle.phase == CHD_CYCLE_FAULT);
    unlock_hw();

    /* A physical failure is terminal, without a duplicate fault publication. */
    hw = prepare(&adp); down_result = FLEA_PD_FAULT;
    lock_hw();
    CHECK(crystalhd_flea_runtime_power_dn(&hw) == FLEA_PD_FAULT && physical_downs == 1);
    CHECK(shadow_ring.cycle.phase == CHD_CYCLE_FAULT && faults == 1 && !adp.present && hw.dma_fault);
    unlock_hw();

    /* No recorded ENTERED result can replace a missing or foreign claim. */
    for (i = 0; i < 3; i++) {
        hw = prepare(&adp);
        lock_hw();
        if (i != 0) CHECK(crystalhd_pm_cycle_down_claim(&hw));
        if (i == 1) hw.FleaShadowSession++;
        if (i == 2) shadow_ring.cycle.entry_pause_fll = 0;
        hw.FleaPowerSaved.valid = true;
        CHECK(!crystalhd_pm_cycle_down_record(&hw, FLEA_PD_ENTERED));
        CHECK(shadow_ring.cycle.down_helper_result == FLEA_PD_ENTERED &&
              shadow_ring.cycle.down_result == FLEA_PD_FAULT && shadow_ring.cycle.phase == CHD_CYCLE_FAULT);
        unlock_hw();
    }

    /* Every unlinking/replay/missing-lifetime case avoids physical wake I/O. */
    for (i = 0; i < 9; i++) {
        hw = prepare(&adp); enter(&hw);
        if (i == 0) hw.FleaShadowSession++;
        if (i == 1) hw.FleaPowerSaved.valid = false;
        if (i == 2) hw.FleaPowerState = FLEA_PS_LP_PENDING;
        if (i == 3) hw.FleaPowerTransition = 0;
        if (i == 4) hw.dma_fault = 1;
        if (i == 5) adp.present = 0;
        if (i == 6) hw.dev_started = 0;
        if (i == 7) shadow_ring.cycle.up_attempted = 1;
        if (i == 8) shadow_ring.cycle.phase = CHD_CYCLE_FAULT;
        lock_hw();
        CHECK(crystalhd_flea_runtime_power_up(&hw) == BC_STS_IO_ERROR && !physical_ups);
        CHECK(hw.FleaPowerState == FLEA_PS_FAULT && hw.dma_fault && !adp.present);
        unlock_hw();
    }

    /* FW/TX wake owners are allowed after a linked, previously quiescent entry. */
    for (i = 0; i < 4; i++) {
        hw = prepare(&adp); enter(&hw);
        if (i == 0) hw.FleaTxActive = 1;
        if (i == 1) hw.fwcmd_trans_active = 1;
        if (i == 2) hw.fwcmd_active = 1;
        if (i == 3) hw.FwCmdCnt = 1;
        CHECK(crystalhd_flea_set_power_state(&hw, FLEA_PS_ACTIVE) == BC_STS_SUCCESS && physical_ups == 1);
        CHECK(!faults && hw.FleaPowerState == FLEA_PS_ACTIVE && !last_record()->violations);
    }

    /* Failed or incomplete physical wake never reaches FLL or replay. */
    for (i = 0; i < 2; i++) {
        hw = prepare(&adp); enter(&hw);
        unsigned before_fll = fll_writes;
        if (i == 0) up_result = BC_STS_IO_ERROR;
        else up_leaves_snapshot = true;
        hw.PwrDwnPiQIntr = hw.PwrDwnTxIntr = 1;
        CHECK(crystalhd_flea_set_power_state(&hw, FLEA_PS_ACTIVE) == BC_STS_IO_ERROR);
        CHECK(hw.FleaPowerState == FLEA_PS_FAULT && hw.dma_fault && !adp.present);
        CHECK(physical_ups == 1 && fll_writes == before_fll && !picture_replays && !tx_replays);
        CHECK(shadow_ring.cycle.phase == CHD_CYCLE_FAULT && shadow_ring.cycle.up_status == BC_STS_IO_ERROR);
        CHECK(shadow_ring.cycle.up_helper_status == (i ? BC_STS_SUCCESS : BC_STS_IO_ERROR));
        CHECK(crystalhd_flea_set_power_state(&hw, FLEA_PS_ACTIVE) == BC_STS_IO_ERROR && physical_ups == 1);
    }

    /* Physical wake alone is not ACTIVE: failed FLL stays pending, then retry is host-only. */
    hw = prepare(&adp); enter(&hw); fll_result = BC_STS_IO_ERROR;
    hw.PwrDwnPiQIntr = hw.PwrDwnTxIntr = 1;
    CHECK(crystalhd_flea_set_power_state(&hw, FLEA_PS_ACTIVE) == BC_STS_IO_ERROR);
    CHECK(hw.FleaPowerState == FLEA_PS_LP_PENDING && !hw.FleaPowerSaved.valid && !faults);
    CHECK(shadow_ring.cycle.phase == CHD_CYCLE_WAKE_CONFIRMED && physical_ups == 1 &&
          shadow_ring.cycle.fll_status == BC_STS_IO_ERROR && !shadow_ring.cycle.fll_confirmed);
    CHECK(!picture_replays && !tx_replays && !last_record()->violations);
    fll_result = BC_STS_SUCCESS;
    CHECK(crystalhd_flea_set_power_state(&hw, FLEA_PS_ACTIVE) == BC_STS_SUCCESS);
    CHECK(physical_ups == 1 && shadow_ring.cycle.phase == CHD_CYCLE_RESUME_CONFIRMED &&
          shadow_ring.cycle.fll_status == BC_STS_IO_ERROR && shadow_ring.cycle.fll_attempts == 2 &&
          shadow_ring.cycle.fll_confirmed && picture_replays == 1 && tx_replays == 1);

    /* Client death at sleep retains fault and metadata; guard adds no helper work. */
    hw = prepare(&adp); enter(&hw);
    CHECK(crystalhd_pm_cycle_reject_asleep(&hw) && hw.FleaPowerState == FLEA_PS_FAULT);
    CHECK(hw.FleaPowerSaved.valid && hw.dma_fault && !adp.present && faults == 1 && !physical_ups);
    CHECK(crystalhd_pm_cycle_reject_asleep(&hw) && faults == 1);
    CHECK(shadow_ring.cycle.phase == CHD_CYCLE_FAULT);

    /* An unlinked COMPLETE is a violation, not evidence of legitimate entry. */
    hw = prepare(&adp); hw.FleaPowerState = FLEA_PS_LP_COMPLETE;
    lock_hw();
    crystalhd_pm_shadow_trace(&hw, CHD_PM_SET, FLEA_EVT_NONE, FLEA_PS_LP_PENDING,
        FLEA_PS_LP_COMPLETE, BC_STS_SUCCESS, CHD_PM_SHADOW_UNKNOWN);
    CHECK(last_record()->violations & CHD_PM_LP_COMPLETE);
    unlock_hw();
    CHECK(!shadow_depth && !hw_depth);
    printf("pwm-cycle: %u checks PASS; actual ledger/wrappers/FLL/observer, no device access\n", checks);
    return 0;
}
