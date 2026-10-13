/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Source-extracted Flea power decisions, with no hardware power operation.
 * This guards conservative RX status handling; it does not prove that the
 * remaining production guards establish hardware/firmware quiescence.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "flea-power-types.h"

#define READ_ONCE(value) (value)
#define printk(...) ((void)0)

struct crystalhd_hw {
    uint32_t FleaEnablePWM;
    enum FLEA_POWER_STATES FleaPowerState;
    enum LIST_STATUS TxList0Sts, TxList1Sts;
    uint32_t EmptyCnt, SingleThreadAppFIFOEmpty, FwCmdCnt;
    enum list_sts rx_list_sts[2];
};

enum { RX_CASES = 16 + 32 };
static unsigned transitions, cases;
static enum FLEA_POWER_STATES requested;

/* Only record the selector's request. The real setter performs DRAM writes
 * and runtime power-down; its hardware side effects are outside this test.
 */
static bool crystalhd_flea_set_power_state(struct crystalhd_hw *hw,
                                         enum FLEA_POWER_STATES state)
{
    transitions++;
    requested = state;
    hw->FleaPowerState = state;
    return true;
}

#include "flea-power-functions.h"

static void require(bool condition, const char *what,
                    const struct crystalhd_hw *before,
                    enum FLEA_STATE_CH_EVENT event)
{
    if (condition) return;
    fprintf(stderr, "%s: state=%u event=%u pwm=%u rx=%#x/%#x "
            "tx=%u/%u empty=%u single=%u fw=%u\n", what,
            (unsigned)before->FleaPowerState, (unsigned)event,
            before->FleaEnablePWM, (unsigned)before->rx_list_sts[0],
            (unsigned)before->rx_list_sts[1], (unsigned)before->TxList0Sts,
            (unsigned)before->TxList1Sts, before->EmptyCnt,
            before->SingleThreadAppFIFOEmpty, before->FwCmdCnt);
    exit(EXIT_FAILURE);
}

/* Every combination of waiting/error bits, plus every individual status bit.
 * Unknown/error status must not become permission to power down.
 */
static enum list_sts rx_status(unsigned index)
{
    uint32_t bits[] = {rx_waiting_y_intr, rx_waiting_uv_intr,
                       rx_y_error, rx_uv_error};
    uint32_t status = 0;
    if (index >= 16) return (enum list_sts)(UINT32_C(1) << (index - 16));
    for (unsigned bit = 0; bit < 4; bit++)
        if (index & (1U << bit)) status |= bits[bit];
    return (enum list_sts)status;
}

static struct crystalhd_hw setup(enum FLEA_POWER_STATES state,
                                 unsigned rx0, unsigned rx1, unsigned busy)
{
    return (struct crystalhd_hw){
        .FleaEnablePWM = true, .FleaPowerState = state,
        .TxList0Sts = busy & 1U ? TxListWaitingForIntr : ListStsFree,
        .TxList1Sts = busy & 2U ? TxListWaitingForIntr : ListStsFree,
        .EmptyCnt = busy & 4U ? 2U : 0U,
        .SingleThreadAppFIFOEmpty = !!(busy & 8U),
        .FwCmdCnt = busy & 16U ? 2U : 0U,
        .rx_list_sts = {rx_status(rx0), rx_status(rx1)}
    };
}

static void step(struct crystalhd_hw *hw, enum FLEA_STATE_CH_EVENT event,
                 enum FLEA_POWER_STATES expected, unsigned expected_calls)
{
    const struct crystalhd_hw before = *hw;
    transitions = 0;
    requested = FLEA_PS_NONE;
    crystalhd_flea_set_next_power_state(hw, event);
    require(hw->FleaPowerState == expected, "wrong next state", &before, event);
    require(transitions == expected_calls, "wrong setter count", &before, event);
    require(!transitions || requested == expected, "wrong power request", &before, event);
    require(hw->FleaEnablePWM == before.FleaEnablePWM &&
            hw->TxList0Sts == before.TxList0Sts &&
            hw->TxList1Sts == before.TxList1Sts &&
            hw->EmptyCnt == before.EmptyCnt &&
            hw->SingleThreadAppFIFOEmpty == before.SingleThreadAppFIFOEmpty &&
            hw->FwCmdCnt == before.FwCmdCnt &&
            hw->rx_list_sts[0] == before.rx_list_sts[0] &&
            hw->rx_list_sts[1] == before.rx_list_sts[1],
            "selector changed ownership or PWM", &before, event);
    cases++;
}

static void idle_matrix(void)
{
    for (unsigned branch = 0; branch < 2; branch++) {
        enum FLEA_POWER_STATES from = branch ? FLEA_PS_LP_PENDING : FLEA_PS_ACTIVE;
        enum FLEA_STATE_CH_EVENT event = branch ? FLEA_EVT_CMD_COMP : FLEA_EVT_FLL_CHANGE;
        for (unsigned rx0 = 0; rx0 < RX_CASES; rx0++)
            for (unsigned rx1 = 0; rx1 < RX_CASES; rx1++)
                for (unsigned busy = 0; busy < 32; busy++) {
                    struct crystalhd_hw hw = setup(from, rx0, rx1, busy);
                    enum FLEA_POWER_STATES to = !rx0 && !rx1 && !busy ?
                        FLEA_PS_LP_COMPLETE : FLEA_PS_LP_PENDING;
                    step(&hw, event, to, from != to);
                }
    }
}

static void retired_owners(void)
{
    for (unsigned rx0 = 0; rx0 < RX_CASES; rx0++)
        for (unsigned rx1 = 0; rx1 < RX_CASES; rx1++)
            for (unsigned busy = 0; busy < 32; busy++) {
                struct crystalhd_hw hw = setup(FLEA_PS_ACTIVE, rx0, rx1, busy);
                if (!rx0 && !rx1 && !busy) continue;
                step(&hw, FLEA_EVT_FLL_CHANGE, FLEA_PS_LP_PENDING, 1);
                step(&hw, FLEA_EVT_CMD_COMP, FLEA_PS_LP_PENDING, 0);
                /* External owners retire/clear their own status. The selector
                 * must not erase errors to manufacture an idle condition.
                 */
                hw = setup(FLEA_PS_LP_PENDING, 0, 0, 0);
                step(&hw, FLEA_EVT_CMD_COMP, FLEA_PS_LP_COMPLETE, 1);
            }
}

static void disabled_pwm(void)
{
    struct crystalhd_hw hw = setup(FLEA_PS_LP_COMPLETE, 15, 16, 31);
    const struct crystalhd_hw before = hw;
    crystalhd_flea_init_power_state(&hw);
    require(hw.FleaEnablePWM == false && hw.FleaPowerState == FLEA_PS_NONE,
            "PWM unexpectedly enabled at init", &before, FLEA_EVT_NONE);
    require(hw.TxList0Sts == before.TxList0Sts &&
            hw.TxList1Sts == before.TxList1Sts &&
            hw.EmptyCnt == before.EmptyCnt &&
            hw.SingleThreadAppFIFOEmpty == before.SingleThreadAppFIFOEmpty &&
            hw.FwCmdCnt == before.FwCmdCnt &&
            hw.rx_list_sts[0] == before.rx_list_sts[0] &&
            hw.rx_list_sts[1] == before.rx_list_sts[1],
            "initializer changed ownership", &before, FLEA_EVT_NONE);
    cases++;
    for (unsigned state = FLEA_PS_NONE; state <= FLEA_PS_LP_COMPLETE; state++)
        for (unsigned event = FLEA_EVT_NONE; event <= FLEA_EVT_CMD_COMP; event++)
            for (unsigned rx0 = 0; rx0 < RX_CASES; rx0++)
                for (unsigned rx1 = 0; rx1 < RX_CASES; rx1++)
                    for (unsigned busy = 0; busy < 32; busy++) {
                        hw = setup(state, rx0, rx1, busy);
                        hw.FleaEnablePWM = false;
                        step(&hw, event, FLEA_PS_ACTIVE, 0);
                    }
}

static void events(void)
{
    for (unsigned state = FLEA_PS_ACTIVE; state <= FLEA_PS_LP_COMPLETE; state++)
        for (unsigned event = FLEA_EVT_NONE; event <= FLEA_EVT_CMD_COMP; event++) {
            struct crystalhd_hw hw = setup(state, 0, 0, 0);
            enum FLEA_POWER_STATES to = state;
            unsigned calls;
            if (event == FLEA_EVT_STOP_DEVICE) to = FLEA_PS_STOPPED;
            else if (event == FLEA_EVT_START_DEVICE) to = FLEA_PS_ACTIVE;
            else if ((state == FLEA_PS_ACTIVE && event == FLEA_EVT_FLL_CHANGE) ||
                     (state == FLEA_PS_LP_PENDING && event == FLEA_EVT_CMD_COMP))
                to = FLEA_PS_LP_COMPLETE;
            else if (state != FLEA_PS_ACTIVE &&
                     (event == FLEA_EVT_FW_CMD_POST || event == FLEA_EVT_FLL_CHANGE))
                to = FLEA_PS_ACTIVE;
            calls = to != state && event != FLEA_EVT_START_DEVICE &&
                    event != FLEA_EVT_STOP_DEVICE;
            step(&hw, event, to, calls);
        }
}

int main(void)
{
    idle_matrix();
    retired_owners();
    disabled_pwm();
    events();
    printf("PASS: %u Flea power selector cases; no device access\n", cases);
    return 0;
}
