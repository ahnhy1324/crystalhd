/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Source-extracted Flea power decisions/transitions with mocked MMIO.
 * This guards conservative RX status handling; it does not prove that the
 * remaining production guards establish hardware/firmware quiescence.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "flea-power-types.h"
#include "bchp_clk.h"
#include "bchp_pri_arb_control_regs.h"
#include "bchp_ddr23_ctl_regs_0.h"
#include "bchp_ddr23_phy_byte_lane_0.h"
#include "bchp_ddr23_phy_byte_lane_1.h"
#include "bchp_ddr23_phy_control_regs.h"
#include "bchp_decode_cpuregs_0.h"
#include "bchp_decode_cpuregs2_0.h"

#define READ_ONCE(value) (value)
#define printk(...) ((void)0)
#define dev_err(...) ((void)0)

struct mock_device;
struct crystalhd_rx_dma_pkt { unsigned unused; };

struct crystalhd_hw {
    uint32_t FleaEnablePWM;
    enum FLEA_POWER_STATES FleaPowerState;
    enum LIST_STATUS TxList0Sts, TxList1Sts;
    uint32_t EmptyCnt, SingleThreadAppFIFOEmpty, FwCmdCnt;
    enum list_sts rx_list_sts[2];
    struct mock_device *adp;
    uint32_t (*pfnReadDevRegister)(struct mock_device *, uint32_t);
    void (*pfnWriteDevRegister)(struct mock_device *, uint32_t, uint32_t);
    BC_STATUS (*pfnDevDRAMWrite)(struct crystalhd_hw *, uint32_t,
                               uint32_t, uint32_t *);
    uint32_t DrvPauseCnt, FleaFLLUpdateAddr, PicQSts;
    uint32_t OLWatchDogTimer, ILWatchDogTimer;
    uint64_t TickStartInPD;
    bool PwrDwnPiQIntr, PwrDwnTxIntr;
    unsigned lock;
    bool fwcmd_active, fwcmd_poisoned;
};

enum { RX_CASES = 16 + 32 };
static unsigned transitions, cases, transition_cases;
static enum FLEA_POWER_STATES requested;

/* The exhaustive selector matrix records requests independently of the
 * source-extracted setter/MMIO fault-injection tests below.
 */
static bool record_power_request(struct crystalhd_hw *hw,
                                 enum FLEA_POWER_STATES state)
{
    transitions++;
    requested = state;
    hw->FleaPowerState = state;
    return true;
}

#define crystalhd_flea_set_power_state record_power_request
#include "flea-power-functions.h"
#undef crystalhd_flea_set_power_state

struct mock_device {
    bool idle, fail_self_refresh, fail_fll_write;
    bool require_lock, lock_held;
    unsigned reads, writes, dram_writes, delays, power_ups, fetches;
    unsigned status_reads, poll_delays, power_writes;
    unsigned picture_irqs, tx_irqs;
    struct crystalhd_hw *command_on_lock;
    uint32_t last_fll;
    struct { uint32_t addr, value; } registers[32];
    unsigned register_count;
};

static struct mock_device device;

static void check(bool condition, const char *what)
{
    if (condition) return;
    fprintf(stderr, "Flea transition: %s\n", what);
    exit(EXIT_FAILURE);
}

static void lock_hw(unsigned *lock)
{
    check(!*lock && !device.lock_held, "nested hardware lock");
    *lock = 1;
    device.lock_held = true;
    if (device.command_on_lock) {
        device.command_on_lock->fwcmd_active = true;
        device.command_on_lock = NULL;
    }
}

static void unlock_hw(unsigned *lock)
{
    check(*lock && device.lock_held, "unbalanced hardware unlock");
    *lock = 0;
    device.lock_held = false;
}

#define spin_lock_irqsave(lock, flags) do { (flags) = 0; lock_hw(lock); } while (0)
#define spin_unlock_irqrestore(lock, flags) do { (void)(flags); unlock_hw(lock); } while (0)

static uint32_t register_value(struct mock_device *dev, uint32_t addr)
{
    for (unsigned i = 0; i < dev->register_count; i++)
        if (dev->registers[i].addr == addr) return dev->registers[i].value;
    return 0;
}

static uint32_t read_register(struct mock_device *dev, uint32_t addr)
{
    check(!dev->require_lock || dev->lock_held, "power-down MMIO read outside lock");
    dev->reads++;
    if (addr == BCHP_DDR23_CTL_REGS_0_CTL_STATUS) {
        dev->status_reads++;
        return (dev->idle ? BCHP_DDR23_CTL_REGS_0_CTL_STATUS_idle_MASK : 0) |
               (dev->fail_self_refresh ? BCHP_DDR23_CTL_REGS_0_CTL_STATUS_clke_MASK : 0);
    }
    return register_value(dev, addr);
}

static void write_register(struct mock_device *dev, uint32_t addr, uint32_t value)
{
    unsigned i;
    check(!dev->require_lock || dev->lock_held, "power-down MMIO write outside lock");
    dev->writes++;
    switch (addr) {
    case BCHP_PRI_ARB_CONTROL_REGS_MASTER_CTL:
    case BCHP_DDR23_CTL_REGS_0_PARAMS2:
    case BCHP_DDR23_CTL_REGS_0_REFRESH_CMD:
    case BCHP_PRI_ARB_CONTROL_REGS_REFRESH_CTL_0:
    case BCHP_DECODE_CPUREGS_0_REG_WATCHDOG_TMR:
    case BCHP_DECODE_CPUREGS2_0_REG_WATCHDOG_TMR:
        break;
    default:
        dev->power_writes++;
        break;
    }
    for (i = 0; i < dev->register_count; i++)
        if (dev->registers[i].addr == addr) break;
    check(i < sizeof(dev->registers) / sizeof(dev->registers[0]),
          "mock register storage exhausted");
    if (i == dev->register_count) dev->register_count++;
    dev->registers[i].addr = addr;
    dev->registers[i].value = value;
}

static BC_STATUS write_fll(struct crystalhd_hw *hw, uint32_t addr,
                           uint32_t size, uint32_t *value)
{
    check(!hw->adp->require_lock || hw->adp->lock_held, "zero-FLL write outside lock");
    check(addr == hw->FleaFLLUpdateAddr && size == 1, "unexpected FLL write");
    hw->adp->dram_writes++;
    hw->adp->last_fll = *value;
    return hw->adp->fail_fll_write ? BC_STS_IO_ERROR : BC_STS_SUCCESS;
}

static uint32_t crystalhd_hw_count_free_rx_pkts(struct crystalhd_hw *hw)
{
    (void)hw;
    return 4;
}

static struct crystalhd_rx_dma_pkt *crystalhd_hw_fetch_free_rx_pkt(struct crystalhd_hw *hw)
{
    hw->adp->fetches++;
    return NULL;
}

static BC_STATUS crystalhd_hw_repost_cap_buffer(struct crystalhd_hw *hw,
                                                struct crystalhd_rx_dma_pkt *pkt)
{
    (void)hw;
    (void)pkt;
    check(false, "unexpected repost without a fetched packet");
    return BC_STS_ERROR;
}

static void crystalhd_flea_runtime_power_up(struct crystalhd_hw *hw)
{
    hw->adp->power_ups++;
}

static void crystalhd_flea_handle_PicQSts_intr(struct crystalhd_hw *hw)
{
    hw->adp->picture_irqs++;
}

static void crystalhd_flea_update_tx_buff_info(struct crystalhd_hw *hw)
{
    hw->adp->tx_irqs++;
}

static void udelay(unsigned usec)
{
    check(usec == 10 || usec == 5, "unexpected transition delay");
    if (usec == 10) device.poll_delays++;
    device.delays++;
}

static uint64_t rdtsc_ordered(void)
{
    return UINT64_C(123456);
}

#include "flea-power-transitions.h"

static struct crystalhd_hw transition_setup(enum FLEA_POWER_STATES state)
{
    memset(&device, 0, sizeof(device));
    device.idle = true;
    write_register(&device, BCHP_DECODE_CPUREGS_0_REG_WATCHDOG_TMR, 7);
    write_register(&device, BCHP_DECODE_CPUREGS2_0_REG_WATCHDOG_TMR, 11);
    write_register(&device, BCHP_DDR23_CTL_REGS_0_PARAMS2, UINT32_C(0x02400000));
    write_register(&device, BCHP_PRI_ARB_CONTROL_REGS_REFRESH_CTL_0,
                   UINT32_C(0x80) | BCHP_PRI_ARB_CONTROL_REGS_REFRESH_CTL_0_enable_MASK);
    device.writes = 0;
    device.require_lock = true;
    return (struct crystalhd_hw){
        .FleaEnablePWM = true, .FleaPowerState = state, .adp = &device,
        .pfnReadDevRegister = read_register, .pfnWriteDevRegister = write_register,
        .pfnDevDRAMWrite = write_fll, .FleaFLLUpdateAddr = UINT32_C(0x1000),
        .TickStartInPD = UINT64_C(99)
    };
}

static void transition_failures(void)
{
    const enum FLEA_POWER_STATES starts[] = {FLEA_PS_ACTIVE, FLEA_PS_LP_PENDING};
    const enum FLEA_POWER_STATES ends[] = {FLEA_PS_LP_PENDING, FLEA_PS_LP_COMPLETE};
    for (unsigned from = 0; from < 2; from++)
        for (unsigned to = 0; to < 2; to++) {
            if (starts[from] == ends[to]) continue;
            struct crystalhd_hw hw = transition_setup(starts[from]);
            device.fail_fll_write = true;
            check(!crystalhd_flea_set_power_state(&hw, ends[to]),
                  "failed zero-FLL publication reported success");
            check(hw.FleaPowerState == starts[from],
                  "failed zero-FLL publication changed state");
            check(device.dram_writes == 1 && device.last_fll == 0,
                  "zero-FLL publication not attempted exactly once");
            check(device.reads == 0 && device.writes == 0 &&
                  hw.TickStartInPD == UINT64_C(99),
                  "power-down continued after failed zero-FLL publication");
            check(!hw.lock && !device.lock_held, "FLL failure leaked hardware lock");
            transition_cases++;
        }

    for (unsigned from = 0; from < 2; from++)
        for (unsigned refresh = 0; refresh < 2; refresh++) {
            struct crystalhd_hw hw = transition_setup(starts[from]);
            device.idle = !!refresh;
            device.fail_self_refresh = !!refresh;
            check(!crystalhd_flea_set_power_state(&hw, FLEA_PS_LP_COMPLETE),
                  "idle/self-refresh timeout reported success");
            check(hw.FleaPowerState == FLEA_PS_LP_PENDING,
                  "timeout lost successful zero-FLL pending state");
            check(device.status_reads == (refresh ? 101U : 401U) &&
                  device.poll_delays == (refresh ? 0U : 401U),
                  "idle/self-refresh polls did not remain bounded");
            check(register_value(&device, BCHP_PRI_ARB_CONTROL_REGS_MASTER_CTL) ==
                  BCHP_PRI_ARB_CONTROL_REGS_MASTER_CTL_arb_disable_Enable,
                  "timeout did not restore arbiter");
            check(register_value(&device, BCHP_DECODE_CPUREGS_0_REG_WATCHDOG_TMR) == 7 &&
                  register_value(&device, BCHP_DECODE_CPUREGS2_0_REG_WATCHDOG_TMR) == 11,
                  "timeout did not preserve/restore watchdogs");
            check(register_value(&device, BCHP_DDR23_CTL_REGS_0_PARAMS2) == UINT32_C(0x02400000) &&
                  register_value(&device, BCHP_PRI_ARB_CONTROL_REGS_REFRESH_CTL_0) ==
                  (UINT32_C(0x80) | BCHP_PRI_ARB_CONTROL_REGS_REFRESH_CTL_0_enable_MASK),
                  "timeout did not preserve/restore refresh controls");
            check(device.power_writes == 0 && hw.DrvPauseCnt == 0 &&
                  hw.TickStartInPD == UINT64_C(99),
                  "timeout continued into clock gating or recorded power-down");
            check(!hw.lock && !device.lock_held, "timeout leaked hardware lock");
            transition_cases++;
        }

    for (unsigned complete = 0; complete < 2; complete++) {
        struct crystalhd_hw hw = transition_setup(complete ? FLEA_PS_LP_COMPLETE :
                                                             FLEA_PS_LP_PENDING);
        device.require_lock = false;
        device.fail_fll_write = true;
        hw.PwrDwnPiQIntr = hw.PwrDwnTxIntr = true;
        hw.PicQSts = 1;
        check(!crystalhd_flea_set_power_state(&hw, FLEA_PS_ACTIVE),
              "failed nonzero-FLL wake reported success");
        check(hw.FleaPowerState == FLEA_PS_LP_PENDING && device.power_ups == complete,
              "failed wake did not preserve physically-awake pending state");
        check(device.dram_writes == 1 && device.last_fll == 4 && device.fetches == 0 &&
              device.picture_irqs == 0 && device.tx_irqs == 0 &&
              hw.PwrDwnPiQIntr && hw.PwrDwnTxIntr,
              "failed wake replayed saved IRQs/reposted RX or lost FLL update");
        transition_cases++;

        device.fail_fll_write = false;
        check(crystalhd_flea_set_power_state(&hw, FLEA_PS_ACTIVE),
              "wake retry after restored FLL publication failed");
        check(hw.FleaPowerState == FLEA_PS_ACTIVE && device.power_ups == complete &&
              device.dram_writes == 2 && device.last_fll == 4 && device.fetches == 1 &&
              device.picture_irqs == 1 && device.tx_irqs == 1 &&
              !hw.PwrDwnPiQIntr && !hw.PwrDwnTxIntr,
              "wake retry lost/deferred saved IRQs or repeated physical wake");
        transition_cases++;
    }

    for (unsigned pending = 0; pending < 2; pending++)
        for (unsigned owner = 0; owner < 9; owner++) {
            enum FLEA_POWER_STATES state = pending ? FLEA_PS_LP_PENDING : FLEA_PS_ACTIVE;
            struct crystalhd_hw hw = transition_setup(state);
            hw.fwcmd_active = owner == 0;
            hw.fwcmd_poisoned = owner == 1;
            hw.FwCmdCnt = owner == 2;
            hw.TxList0Sts = owner == 3 ? TxListWaitingForIntr : ListStsFree;
            hw.TxList1Sts = owner == 4 ? TxListWaitingForIntr : ListStsFree;
            hw.rx_list_sts[0] = owner == 5 ? rx_waiting_y_intr : sts_free;
            hw.rx_list_sts[1] = owner == 6 ? rx_uv_error : sts_free;
            hw.EmptyCnt = owner == 7;
            hw.SingleThreadAppFIFOEmpty = owner == 8;
            check(!crystalhd_flea_set_power_state(&hw, FLEA_PS_LP_COMPLETE),
                  "power-down accepted busy owner/poisoned channel");
            check(hw.FleaPowerState == state && device.dram_writes == 0 &&
                  device.reads == 0 && device.writes == 0,
                  "power-down touched hardware despite outstanding owner");
            check(!hw.lock && !device.lock_held, "busy owner leaked hardware lock");
            transition_cases++;
        }

    /* Simulate a command admitted after the selector's idle snapshot but
     * before the transition owns the lock. This checks the final recheck,
     * not real concurrent scheduling or hardware quiescence.
     */
    for (unsigned pending = 0; pending < 2; pending++) {
        enum FLEA_POWER_STATES state = pending ? FLEA_PS_LP_PENDING : FLEA_PS_ACTIVE;
        struct crystalhd_hw hw = transition_setup(state);
        device.command_on_lock = &hw;
        check(!crystalhd_flea_set_power_state(&hw, FLEA_PS_LP_COMPLETE),
              "stale idle snapshot bypassed locked command-owner recheck");
        check(hw.fwcmd_active && hw.FleaPowerState == state && device.dram_writes == 0 &&
              device.reads == 0 && device.writes == 0 && !hw.lock && !device.lock_held,
              "lock-time command admission did not prevent power-down");
        transition_cases++;
    }
}

static void transition_success(void)
{
    struct crystalhd_hw hw = transition_setup(FLEA_PS_ACTIVE);
    check(crystalhd_flea_set_power_state(&hw, FLEA_PS_LP_PENDING),
          "successful zero-FLL publication rejected");
    check(hw.FleaPowerState == FLEA_PS_LP_PENDING && device.dram_writes == 1 &&
          device.last_fll == 0 && device.writes == 0,
          "pending transition did not stop before physical power-down");
    transition_cases++;

    device.require_lock = false;
    check(crystalhd_flea_set_power_state(&hw, FLEA_PS_ACTIVE),
          "pending-to-active FLL restoration rejected");
    check(hw.FleaPowerState == FLEA_PS_ACTIVE && device.dram_writes == 2 &&
          device.last_fll == 4 && device.power_ups == 0,
          "pending-to-active restoration incorrect");
    transition_cases++;

    for (unsigned pending = 0; pending < 2; pending++) {
        hw = transition_setup(pending ? FLEA_PS_LP_PENDING : FLEA_PS_ACTIVE);
        check(crystalhd_flea_set_power_state(&hw, FLEA_PS_LP_COMPLETE),
              "successful idle transition rejected");
        check(hw.FleaPowerState == FLEA_PS_LP_COMPLETE && device.dram_writes == 1 &&
              device.last_fll == 0 && hw.TickStartInPD == UINT64_C(123456) &&
              hw.DrvPauseCnt == 1,
              "successful idle transition not committed");
        check(register_value(&device, BCHP_CLK_PM_CTRL) &
              BCHP_CLK_PM_CTRL_DIS_DDR_216_CLK_MASK,
              "successful idle transition skipped final clock write");
        check(!hw.lock && !device.lock_held, "successful transition leaked hardware lock");
        transition_cases++;
    }
}

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
            hw->fwcmd_active == before.fwcmd_active &&
            hw->fwcmd_poisoned == before.fwcmd_poisoned &&
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

static void command_owners(void)
{
    for (unsigned pending = 0; pending < 2; pending++)
        for (unsigned active = 0; active < 2; active++)
            for (unsigned poisoned = 0; poisoned < 2; poisoned++) {
                struct crystalhd_hw hw = setup(pending ? FLEA_PS_LP_PENDING :
                                                        FLEA_PS_ACTIVE, 0, 0, 0);
                hw.fwcmd_active = !!active;
                hw.fwcmd_poisoned = !!poisoned;
                enum FLEA_POWER_STATES to = active || poisoned ? FLEA_PS_LP_PENDING :
                                                                FLEA_PS_LP_COMPLETE;
                step(&hw, pending ? FLEA_EVT_CMD_COMP : FLEA_EVT_FLL_CHANGE,
                     to, to != hw.FleaPowerState);
            }
}

int main(void)
{
    idle_matrix();
    retired_owners();
    disabled_pwm();
    events();
    command_owners();
    transition_failures();
    transition_success();
    printf("PASS: %u Flea power selector cases; %u mocked-MMIO transition cases; "
           "no device access\n", cases, transition_cases);
    return 0;
}
