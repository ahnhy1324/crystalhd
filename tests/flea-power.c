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
    bool FleaPowerTransition, FleaPowerRecheck, hw_pause_issued;
    unsigned FleaIrqActive, FleaTxActive;
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
    uint64_t TickStartInPD, TickSpentInPD, TickCntDecodePU;
    uint32_t PDRatio, PauseThreshold, DefaultPauseThreshold;
    bool PwrDwnPiQIntr, PwrDwnTxIntr;
    bool dma_fault, dev_started;
    unsigned lock;
    bool fwcmd_active, fwcmd_poisoned, fwcmd_trans_active;
};

enum { RX_CASES = 16 + 32 };
static unsigned transitions, cases, transition_cases, sequence_cases;
static unsigned register_cycles;
static enum FLEA_POWER_STATES requested;
static void check(bool condition, const char *what);

/* The exhaustive selector matrix records requests independently of the
 * source-extracted setter/MMIO fault-injection tests below.
 */
static BC_STATUS record_power_request(struct crystalhd_hw *hw,
                                     enum FLEA_POWER_STATES state)
{
    check(hw->FleaPowerTransition && !hw->lock,
          "selector called setter without an unlocked reservation");
    transitions++;
    requested = state;
    hw->FleaPowerState = state;
    return BC_STS_SUCCESS;
}

enum nested_point { NEST_NONE, NEST_UNLOCK, NEST_PICTURE, NEST_FETCH,
                    NEST_REPOST, NEST_TX };

struct mock_pci_device { unsigned device; };

struct mock_device {
    struct mock_pci_device *pdev;
    bool present;
    bool idle, fail_self_refresh, fail_fll_write;
    bool require_lock, lock_held;
    unsigned reads, writes, dram_writes, delays, power_ups, fetches;
    unsigned status_reads, poll_delays, power_writes;
    unsigned picture_irqs, tx_irqs;
    struct crystalhd_hw *command_on_lock;
    unsigned command_lock_countdown;
    struct crystalhd_hw *health_on_lock;
    unsigned health_lock_countdown, health_failure;
    struct crystalhd_hw *nested_hw;
    enum nested_point nested_point;
    enum FLEA_POWER_STATES nested_state;
    enum FLEA_STATE_CH_EVENT nested_event;
    bool nested_explicit_event;
    unsigned nested_calls, reposts;
    bool return_packet, relatch_irqs;
    BC_STATUS repost_status;
    struct crystalhd_hw *completion_hw;
    unsigned completion_unlock_countdown, completion_calls;
    bool fail_completion_replay;
    struct crystalhd_hw *intent_hw;
    struct crystalhd_hw *clear_intent_on_lock;
    bool expected_intent, intent_observed;
    bool firmware_alive, cleanup_completion, resume_cleanup_on_unlock;
    unsigned cleanup_probes, cleanup_resumes;
    uint32_t last_fll;
    struct { uint32_t addr, value; } registers[32];
    unsigned register_count;
};

static struct mock_device device;
static struct crystalhd_rx_dma_pkt packet;

static bool crystalhd_flea_set_power_state(struct crystalhd_hw *hw,
                                           enum FLEA_POWER_STATES state);
static BC_STATUS real_set_next_power_state(struct crystalhd_hw *hw,
                                          enum FLEA_STATE_CH_EVENT event);
static void nested_transition(enum nested_point point);

static void check(bool condition, const char *what)
{
    if (condition) return;
    fprintf(stderr, "Flea transition: %s\n", what);
    exit(EXIT_FAILURE);
}

static void mark_unhealthy(struct crystalhd_hw *hw, unsigned failure)
{
    if (failure == 0) hw->adp->present = false;
    else if (failure == 1) hw->dma_fault = true;
    else hw->dev_started = false;
}

static void lock_hw(unsigned *lock)
{
    check(!*lock && !device.lock_held, "nested hardware lock");
    *lock = 1;
    device.lock_held = true;
    if (device.clear_intent_on_lock) {
        device.clear_intent_on_lock->hw_pause_issued = false;
        device.clear_intent_on_lock = NULL;
    }
    if (device.command_on_lock && (!device.command_lock_countdown ||
                                  --device.command_lock_countdown == 0)) {
        device.command_on_lock->fwcmd_active = true;
        device.command_on_lock = NULL;
    }
    if (device.health_on_lock && --device.health_lock_countdown == 0) {
        mark_unhealthy(device.health_on_lock, device.health_failure);
        device.health_on_lock = NULL;
    }
}

static void unlock_hw(unsigned *lock)
{
    check(*lock && device.lock_held, "unbalanced hardware unlock");
    *lock = 0;
    device.lock_held = false;
    if (device.completion_hw && device.completion_unlock_countdown &&
        !--device.completion_unlock_countdown) {
        struct crystalhd_hw *hw = device.completion_hw;
        device.completion_hw = NULL;
        hw->FleaTxActive = 0;
        check(hw->FleaPowerTransition, "completion hook missed reserved window");
        if (hw->FleaPowerState == FLEA_PS_LP_PENDING || hw->hw_pause_issued)
            for (unsigned repeat = 0; repeat < 2; repeat++)
                check(real_set_next_power_state(hw, FLEA_EVT_CMD_COMP) == BC_STS_BUSY &&
                      hw->FleaPowerRecheck,
                      "completion under reservation did not latch deferred recheck");
        device.completion_calls++;
        if (device.fail_completion_replay) device.fail_fll_write = true;
    }
    if (device.intent_hw && !device.intent_hw->FleaPowerTransition) {
        check(device.intent_hw->hw_pause_issued == device.expected_intent,
              "RX intent was not committed before reservation release");
        device.intent_observed = true;
        device.intent_hw = NULL;
    }
    if (device.resume_cleanup_on_unlock && device.cleanup_probes &&
        device.nested_hw && !device.nested_hw->FleaPowerTransition) {
        device.resume_cleanup_on_unlock = false;
        check(real_set_next_power_state(device.nested_hw, FLEA_EVT_RX_RESUME) == BC_STS_SUCCESS,
              "new RX resume could not linearize after cleanup reservation release");
        device.cleanup_resumes++;
    }
    if (device.nested_hw && device.nested_hw->FleaPowerTransition)
        nested_transition(NEST_UNLOCK);
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
    /* The production helper locks hw->lock for its retained-buffer count. */
    lock_hw(&hw->lock);
    unlock_hw(&hw->lock);
    return 4;
}

static struct crystalhd_rx_dma_pkt *crystalhd_hw_fetch_free_rx_pkt(struct crystalhd_hw *hw)
{
    check(!hw->lock && !hw->FleaPowerTransition,
          "RX fetch before wake reservation release");
    hw->adp->fetches++;
    nested_transition(NEST_FETCH);
    return hw->adp->return_packet ? &packet : NULL;
}

static BC_STATUS crystalhd_hw_repost_cap_buffer(struct crystalhd_hw *hw,
                                                struct crystalhd_rx_dma_pkt *pkt)
{
    check(!hw->lock && !hw->FleaPowerTransition,
          "RX repost before wake reservation release");
    check(pkt == &packet && hw->adp->return_packet,
          "unexpected repost without a fetched packet");
    hw->adp->reposts++;
    nested_transition(NEST_REPOST);
    return hw->adp->repost_status;
}

static void crystalhd_flea_runtime_power_up(struct crystalhd_hw *hw)
{
    check(hw->lock && hw->FleaPowerTransition,
          "physical wake outside locked transition reservation");
    hw->adp->power_ups++;
}

static void crystalhd_flea_handle_PicQSts_intr(struct crystalhd_hw *hw)
{
    check(!hw->lock, "picture replay under hardware lock");
    hw->adp->picture_irqs++;
    nested_transition(NEST_PICTURE);
    if (hw->adp->relatch_irqs) hw->PwrDwnPiQIntr = true;
}

static void crystalhd_flea_update_tx_buff_info(struct crystalhd_hw *hw)
{
    check(!hw->lock, "TX replay under hardware lock");
    hw->adp->tx_irqs++;
    nested_transition(NEST_TX);
    if (hw->adp->relatch_irqs) hw->PwrDwnTxIntr = true;
}

static void udelay(unsigned usec)
{
    check(usec == 10 || usec == 5 || usec == 25, "unexpected transition delay");
    if (usec == 10) device.poll_delays++;
    device.delays++;
}

static uint64_t rdtsc_ordered(void)
{
    return UINT64_C(123456);
}

static bool crystalhd_flea_detect_fw_alive(struct crystalhd_hw *hw)
{
    check(!hw->lock && hw->FleaPowerTransition && hw->FleaPowerState == FLEA_PS_ACTIVE,
          "cleanup heartbeat lacked an unlocked active reservation");
    device.cleanup_probes++;
    if (device.cleanup_completion) {
        check(real_set_next_power_state(hw, FLEA_EVT_CMD_COMP) == BC_STS_BUSY &&
              hw->FleaPowerRecheck,
              "ACTIVE cleanup lost completion for accepted RX pause intent");
    }
    return device.firmware_alive;
}

#define crystalhd_flea_set_power_state record_power_request
#include "flea-power-functions.h"
#undef crystalhd_flea_set_power_state

#define crystalhd_flea_set_power_state private_set_power_state
#include "flea-power-transitions.h"
#undef crystalhd_flea_set_power_state
#include "flea-power-done.h"

/* The state-machine cases retain their wake stub. These additional tests
 * execute the actual wake helper, separately from those reservation cases.
 */
#define crystalhd_flea_runtime_power_up real_runtime_power_up
#include "flea-power-up.h"
#undef crystalhd_flea_runtime_power_up

/* Exercise the same selector with its real setter as well as the recording
 * matrix. This is not a separate state-machine implementation.
 */
#define crystalhd_flea_init_power_state real_init_power_state
#define crystalhd_flea_set_next_power_state real_set_next_power_state
#define crystalhd_flea_set_power_state private_set_power_state
#define crystalhd_flea_fw_transaction_done real_fw_transaction_done
#include "flea-power-functions.h"
#include "flea-power-pause.h"
#include "flea-power-done.h"
#include "flea-power-fll.h"
#undef crystalhd_flea_fw_transaction_done
#undef crystalhd_flea_set_power_state
#undef crystalhd_flea_set_next_power_state
#undef crystalhd_flea_init_power_state

/* Lower-level fault cases enter with the reservation the production selector
 * supplies. Runtime reentrancy is tested through the real selector below.
 */
static bool crystalhd_flea_set_power_state(struct crystalhd_hw *hw,
                                          enum FLEA_POWER_STATES state)
{
    if (state == FLEA_PS_ACTIVE)
        return real_set_next_power_state(hw, FLEA_EVT_RESUME) == BC_STS_SUCCESS;
    if (hw->FleaPowerTransition) return false;
    hw->FleaPowerTransition = true;
    BC_STATUS status = private_set_power_state(hw, state);
    hw->FleaPowerTransition = false;
    return status == BC_STS_SUCCESS;
}

/* Deterministic unlocked wake interleavings, not scheduler/hardware proof. */
static void nested_transition(enum nested_point point)
{
    struct crystalhd_hw *hw = device.nested_hw;
    if (!hw || device.nested_point != point) return;
    const enum FLEA_POWER_STATES state = hw->FleaPowerState;
    const unsigned reads = device.reads, writes = device.writes;
    const unsigned dram_writes = device.dram_writes, power_ups = device.power_ups;
    const unsigned picture_irqs = device.picture_irqs, tx_irqs = device.tx_irqs;
    const unsigned fetches = device.fetches, reposts = device.reposts;
    device.nested_point = NEST_NONE;
    if (point == NEST_FETCH || point == NEST_REPOST) {
        check(!hw->lock && !hw->FleaPowerTransition,
              "RX callback still owns the wake reservation");
        check(real_set_next_power_state(hw, device.nested_explicit_event ?
              device.nested_event : FLEA_EVT_PAUSE) == BC_STS_SUCCESS &&
              hw->FleaPowerState == FLEA_PS_LP_COMPLETE && !hw->FleaPowerTransition,
              "competing pause after wake release did not complete");
        device.nested_calls++;
        return;
    }
    check(!hw->lock && hw->FleaPowerTransition,
          "wake replay does not own an unlocked transition reservation");
    enum FLEA_STATE_CH_EVENT event = device.nested_explicit_event ?
        device.nested_event : device.nested_state == FLEA_PS_ACTIVE ?
        FLEA_EVT_RESUME : FLEA_EVT_PAUSE;
    check(real_set_next_power_state(hw, event) == BC_STS_BUSY,
          "nested selector entered an unfinished wake transaction");
    check(hw->FleaPowerState == state && hw->FleaPowerTransition &&
          !hw->lock && !device.lock_held && device.reads == reads &&
          device.writes == writes && device.dram_writes == dram_writes &&
          device.power_ups == power_ups && device.picture_irqs == picture_irqs &&
          device.tx_irqs == tx_irqs && device.fetches == fetches &&
          device.reposts == reposts,
          "rejected nested setter changed state, reservation or hardware");
    device.nested_calls++;
}

static struct crystalhd_hw transition_setup(enum FLEA_POWER_STATES state)
{
    memset(&device, 0, sizeof(device));
    device.idle = true;
    device.present = true;
    device.firmware_alive = true;
    write_register(&device, BCHP_DECODE_CPUREGS_0_REG_WATCHDOG_TMR, 7);
    write_register(&device, BCHP_DECODE_CPUREGS2_0_REG_WATCHDOG_TMR, 11);
    write_register(&device, BCHP_DDR23_CTL_REGS_0_PARAMS2, UINT32_C(0x02400000));
    write_register(&device, BCHP_PRI_ARB_CONTROL_REGS_REFRESH_CTL_0,
                   UINT32_C(0x80) | BCHP_PRI_ARB_CONTROL_REGS_REFRESH_CTL_0_enable_MASK);
    device.writes = 0;
    device.require_lock = true;
    return (struct crystalhd_hw){
        .FleaEnablePWM = true, .FleaPowerState = state, .adp = &device,
        .dev_started = true,
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
            check(!hw.FleaPowerTransition, "FLL failure leaked transition reservation");
            transition_cases++;
        }

    for (unsigned from = 0; from < 2; from++)
        for (unsigned refresh = 0; refresh < 2; refresh++) {
            struct crystalhd_hw hw = transition_setup(starts[from]);
            device.idle = !!refresh;
            device.fail_self_refresh = !!refresh;
            check(crystalhd_flea_set_power_state(&hw, FLEA_PS_LP_COMPLETE),
                  "idle/self-refresh timeout lost accepted zero-FLL pause");
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
            check(!hw.FleaPowerTransition, "timeout leaked transition reservation");
            transition_cases++;
        }

    for (unsigned complete = 0; complete < 2; complete++) {
        struct crystalhd_hw hw = transition_setup(complete ? FLEA_PS_LP_COMPLETE :
                                                             FLEA_PS_LP_PENDING);
        device.fail_fll_write = true;
        hw.PwrDwnPiQIntr = hw.PwrDwnTxIntr = true;
        hw.PicQSts = 1;
        check(!crystalhd_flea_set_power_state(&hw, FLEA_PS_ACTIVE),
              "failed nonzero-FLL wake reported success");
        check(!hw.FleaPowerTransition, "failed wake leaked transition reservation");
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
        check(!hw.FleaPowerTransition, "wake retry leaked transition reservation");
        check(hw.FleaPowerState == FLEA_PS_ACTIVE && device.power_ups == complete &&
              device.dram_writes == 2 && device.last_fll == 4 && device.fetches == 1 &&
              device.picture_irqs == 1 && device.tx_irqs == 1 &&
              !hw.PwrDwnPiQIntr && !hw.PwrDwnTxIntr,
              "wake retry lost/deferred saved IRQs or repeated physical wake");
        transition_cases++;
    }

    for (unsigned pending = 0; pending < 2; pending++)
        for (unsigned owner = 0; owner < 12; owner++) {
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
            hw.fwcmd_trans_active = owner == 9;
            hw.FleaIrqActive = owner == 10;
            hw.FleaTxActive = owner == 11;
            check(crystalhd_flea_set_power_state(&hw, FLEA_PS_LP_COMPLETE),
                  "busy owner lost accepted zero-FLL pause");
            check(hw.FleaPowerState == FLEA_PS_LP_PENDING && device.dram_writes == 1 &&
                  device.last_fll == 0 &&
                  device.reads == 0 && device.writes == 0,
                  "pause gated hardware despite outstanding owner");
            check(!hw.lock && !device.lock_held, "busy owner leaked hardware lock");
            check(!hw.FleaPowerTransition, "busy owner leaked transition reservation");
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
        check(crystalhd_flea_set_power_state(&hw, FLEA_PS_LP_COMPLETE),
              "new command owner lost accepted zero-FLL pause");
        check(hw.fwcmd_active && hw.FleaPowerState == FLEA_PS_LP_PENDING &&
              device.dram_writes == 1 && device.last_fll == 0 &&
              device.reads == 0 && device.writes == 0 && !hw.lock && !device.lock_held,
              "lock-time command admission did not prevent power-down");
        transition_cases++;
    }
}

static void wake_interleavings(void)
{
    const enum FLEA_POWER_STATES targets[] = {
        FLEA_PS_ACTIVE, FLEA_PS_LP_PENDING, FLEA_PS_LP_COMPLETE
    };
    for (unsigned complete = 0; complete < 2; complete++)
        for (unsigned target = 0; target < 3; target++)
            for (enum nested_point point = NEST_UNLOCK; point <= NEST_TX; point++) {
                if (point == NEST_FETCH || point == NEST_REPOST) continue;
                struct crystalhd_hw hw = transition_setup(complete ?
                    FLEA_PS_LP_COMPLETE : FLEA_PS_LP_PENDING);
                device.nested_hw = &hw;
                device.nested_point = point;
                device.nested_state = targets[target];
                device.return_packet = true;
                device.relatch_irqs = true;
                hw.PwrDwnPiQIntr = hw.PwrDwnTxIntr = true;
                hw.PicQSts = 1;
                check(crystalhd_flea_set_power_state(&hw, FLEA_PS_ACTIVE),
                      "outer wake failed during blocked nested setter");
                check(device.nested_calls == 1 && hw.FleaPowerState == FLEA_PS_ACTIVE &&
                      !hw.FleaPowerTransition && !hw.lock && !device.lock_held,
                      "wake interleaving missed hook or leaked transition reservation");
                check(device.power_ups == complete && device.dram_writes == 1 &&
                      device.last_fll == 4 && device.fetches == 1 &&
                      device.reposts == 1 && device.picture_irqs == 1 && device.tx_irqs == 1,
                      "wake interleaving lost or duplicated hardware/callback work");
                check(hw.PwrDwnPiQIntr && hw.PwrDwnTxIntr,
                      "wake replay erased a newly latched notification");
                transition_cases++;
            }

    for (unsigned from = 0; from < 3; from++)
        for (unsigned to = 0; to < 3; to++) {
            struct crystalhd_hw hw = transition_setup(targets[from]);
            hw.FleaPowerTransition = true;
            check(real_set_next_power_state(&hw, targets[to] == FLEA_PS_ACTIVE ?
                  FLEA_EVT_RESUME : FLEA_EVT_PAUSE) == BC_STS_BUSY,
                  "selector ignored an existing transition reservation");
            check(hw.FleaPowerTransition && hw.FleaPowerState == targets[from] &&
                  !hw.lock && !device.lock_held && device.reads == 0 &&
                  device.writes == 0 && device.dram_writes == 0 && device.power_ups == 0,
                  "busy setter changed state or hardware");
            transition_cases++;
        }

    /* Low-level reply copy has ended; outer command rollback/postprocessing
     * still owns the device, even though the IRQ and mailbox counters are idle.
     */
    for (unsigned pending = 0; pending < 2; pending++) {
        struct crystalhd_hw hw = transition_setup(pending ? FLEA_PS_LP_PENDING :
                                                          FLEA_PS_ACTIVE);
        hw.fwcmd_trans_active = true;
        check(crystalhd_flea_set_power_state(&hw, FLEA_PS_LP_COMPLETE),
              "outer command transaction lost accepted zero-FLL pause");
        check(hw.FleaPowerState == FLEA_PS_LP_PENDING && device.dram_writes == 1 &&
              !device.last_fll && device.power_writes == 0 &&
              !hw.FleaPowerTransition, "outer owner did not inhibit hardware down");
        hw.fwcmd_trans_active = false;
        check(crystalhd_flea_set_power_state(&hw, FLEA_PS_LP_COMPLETE) &&
              hw.FleaPowerState == FLEA_PS_LP_COMPLETE && !hw.FleaPowerTransition,
              "retired outer owner permanently inhibited an otherwise idle transition");
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

static void register_round_trips(void)
{
    static const struct {
        uint32_t addr, mask;
        bool set_on_down;
    } regs[] = {
        {BCHP_CLK_PM_CTRL,
         BCHP_CLK_PM_CTRL_DIS_ARM_CLK_MASK |
         BCHP_CLK_PM_CTRL_DIS_AVD_CLK_MASK |
         BCHP_CLK_PM_CTRL_DIS_BLINK_108_CLK_MASK |
         BCHP_CLK_PM_CTRL_DIS_AVD_108_CLK_MASK |
         BCHP_CLK_PM_CTRL_DIS_BLINK_216_CLK_MASK |
         BCHP_CLK_PM_CTRL_DIS_AVD_216_CLK_MASK |
         BCHP_CLK_PM_CTRL_DIS_DDR_108_CLK_MASK |
         BCHP_CLK_PM_CTRL_DIS_DDR_216_CLK_MASK, true},
        {BCHP_DDR23_PHY_CONTROL_REGS_CLK_PM_CTRL,
         BCHP_DDR23_PHY_CONTROL_REGS_CLK_PM_CTRL_DIS_DDR_CLK_MASK, true},
        {BCHP_DDR23_PHY_CONTROL_REGS_CLOCK_REG_CONTROL,
         BCHP_DDR23_PHY_CONTROL_REGS_CLOCK_REG_CONTROL_pwrdn_MASK, true},
        {BCHP_DDR23_PHY_CONTROL_REGS_PLL_CONFIG,
         BCHP_DDR23_PHY_CONTROL_REGS_PLL_CONFIG_PWRDN_MASK, true},
        {BCHP_DDR23_PHY_BYTE_LANE_0_CLOCK_REG_CONTROL,
         BCHP_DDR23_PHY_BYTE_LANE_0_CLOCK_REG_CONTROL_pwrdn_MASK, true},
        {BCHP_DDR23_PHY_BYTE_LANE_1_CLOCK_REG_CONTROL,
         BCHP_DDR23_PHY_BYTE_LANE_1_CLOCK_REG_CONTROL_pwrdn_MASK, true},
        {BCHP_DDR23_CTL_REGS_0_PARAMS2,
         BCHP_DDR23_CTL_REGS_0_PARAMS2_clke_MASK, true},
        {BCHP_PRI_ARB_CONTROL_REGS_REFRESH_CTL_0,
         BCHP_PRI_ARB_CONTROL_REGS_REFRESH_CTL_0_enable_MASK, false}
    };

    /* Zero, all-one and walking-one backgrounds exercise every unrelated
     * readback bit, including SUN/MISC and PHY reserved bits. This is a
     * targeted register-image test, not full pad/IDDQ restoration or proof
     * of PLL/DRAM readiness: wake currently uses delays, not readiness polls.
     */
    for (unsigned image = 0; image < 34; image++) {
        uint32_t seed = image == 0 ? 0 : image == 1 ? UINT32_MAX :
                        UINT32_C(1) << (image - 2);
        uint32_t initial[sizeof(regs) / sizeof(regs[0])];
        struct crystalhd_hw hw = transition_setup(FLEA_PS_ACTIVE);
        hw.FleaPowerTransition = true;
        lock_hw(&hw.lock);
        for (unsigned i = 0; i < sizeof(regs) / sizeof(regs[0]); i++) {
            initial[i] = seed & ~regs[i].mask;
            if (!regs[i].set_on_down) initial[i] |= regs[i].mask;
            write_register(&device, regs[i].addr, initial[i]);
        }
        for (unsigned repeat = 0; repeat < 2; repeat++) {
            check(crystalhd_flea_runtime_power_dn(&hw),
                  "paired register test failed to enter mocked power-down");
            for (unsigned i = 0; i < sizeof(regs) / sizeof(regs[0]); i++) {
                uint32_t expected = regs[i].set_on_down ?
                                    initial[i] | regs[i].mask :
                                    initial[i] & ~regs[i].mask;
                check(register_value(&device, regs[i].addr) == expected,
                      "power-down changed unrelated register bits or missed its mask");
            }
            real_runtime_power_up(&hw);
            for (unsigned i = 0; i < sizeof(regs) / sizeof(regs[0]); i++)
                check(register_value(&device, regs[i].addr) == initial[i],
                      "power-up did not restore targeted clock/refresh register image");
            check(register_value(&device, BCHP_DECODE_CPUREGS_0_REG_WATCHDOG_TMR) == 7 &&
                  register_value(&device, BCHP_DECODE_CPUREGS2_0_REG_WATCHDOG_TMR) == 11,
                  "paired power cycle changed watchdog timer values");
            register_cycles++;
        }
        hw.FleaPowerTransition = false;
        unlock_hw(&hw.lock);
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
    device.present = true;
    return (struct crystalhd_hw){
        .FleaEnablePWM = true, .FleaPowerState = state,
        .adp = &device, .dev_started = true,
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
            hw->fwcmd_trans_active == before.fwcmd_trans_active &&
            hw->FleaIrqActive == before.FleaIrqActive &&
            hw->FleaTxActive == before.FleaTxActive &&
            hw->FleaPowerTransition == before.FleaPowerTransition &&
            hw->rx_list_sts[0] == before.rx_list_sts[0] &&
            hw->rx_list_sts[1] == before.rx_list_sts[1],
            "selector changed ownership or PWM", &before, event);
    cases++;
}

static void idle_matrix(void)
{
    for (unsigned branch = 0; branch < 2; branch++) {
        enum FLEA_POWER_STATES from = branch ? FLEA_PS_LP_PENDING : FLEA_PS_ACTIVE;
        enum FLEA_STATE_CH_EVENT event = branch ? FLEA_EVT_CMD_COMP : FLEA_EVT_PAUSE;
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
                step(&hw, FLEA_EVT_PAUSE, FLEA_PS_LP_PENDING, 1);
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
    hw.FleaPowerTransition = true;
    hw.FleaIrqActive = 17;
    hw.FleaTxActive = 23;
    hw.FleaPowerRecheck = true;
    const struct crystalhd_hw before = hw;
    crystalhd_flea_init_power_state(&hw);
    require(hw.FleaEnablePWM == false && hw.FleaPowerState == FLEA_PS_NONE &&
            hw.FleaPowerTransition == false,
            "PWM unexpectedly enabled at init", &before, FLEA_EVT_NONE);
    require(!hw.FleaIrqActive, "initializer retained IRQ owner", &before, FLEA_EVT_NONE);
    require(!hw.FleaTxActive, "initializer retained TX owner", &before, FLEA_EVT_NONE);
    require(!hw.FleaPowerRecheck, "initializer retained deferred power check", &before, FLEA_EVT_NONE);
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
            else if (state == FLEA_PS_LP_PENDING && event == FLEA_EVT_CMD_COMP)
                to = FLEA_PS_LP_COMPLETE;
            else if (state != FLEA_PS_ACTIVE &&
                     event == FLEA_EVT_FW_CMD_POST)
                to = FLEA_PS_ACTIVE;
            calls = to != state && event != FLEA_EVT_START_DEVICE &&
                    event != FLEA_EVT_STOP_DEVICE;
            step(&hw, event, to, calls);
        }
}

static void command_owners(void)
{
    for (unsigned pending = 0; pending < 2; pending++)
        for (unsigned owners = 0; owners < 32; owners++) {
            struct crystalhd_hw hw = setup(pending ? FLEA_PS_LP_PENDING :
                                                    FLEA_PS_ACTIVE, 0, 0, 0);
            hw.fwcmd_active = !!(owners & 1);
            hw.fwcmd_poisoned = !!(owners & 2);
            hw.fwcmd_trans_active = !!(owners & 4);
            hw.FleaIrqActive = !!(owners & 8);
            hw.FleaTxActive = !!(owners & 16);
            enum FLEA_POWER_STATES to = owners ? FLEA_PS_LP_PENDING : FLEA_PS_LP_COMPLETE;
            step(&hw, pending ? FLEA_EVT_CMD_COMP : FLEA_EVT_PAUSE,
                 to, to != hw.FleaPowerState);
        }
}

static void reserved_selector(void)
{
    const enum FLEA_STATE_CH_EVENT events[] = {
        FLEA_EVT_FW_CMD_POST, FLEA_EVT_FLL_CHANGE, FLEA_EVT_CMD_COMP,
        FLEA_EVT_PAUSE, FLEA_EVT_RESUME
    };
    for (unsigned state = FLEA_PS_ACTIVE; state <= FLEA_PS_LP_COMPLETE; state++)
        for (unsigned event = 0; event < sizeof(events) / sizeof(events[0]); event++) {
            struct crystalhd_hw hw = setup(state, 0, 0, 0);
            hw.FleaPowerTransition = true;
            step(&hw, events[event], state, 0);
        }
    /* Lifecycle publication cannot overwrite an in-flight transition. The
     * production PWM-off path remains unchanged.
     */
    for (unsigned pwm = 0; pwm < 2; pwm++)
        for (unsigned stop = 0; stop < 2; stop++) {
            struct crystalhd_hw hw = setup(FLEA_PS_LP_PENDING, 0, 0, 0);
            hw.FleaEnablePWM = pwm;
            hw.FleaPowerTransition = true;
            step(&hw, stop ? FLEA_EVT_STOP_DEVICE : FLEA_EVT_START_DEVICE,
                 pwm ? FLEA_PS_LP_PENDING : FLEA_PS_ACTIVE, 0);
        }
    for (unsigned event = 0; event < sizeof(events) / sizeof(events[0]); event++) {
        struct crystalhd_hw hw = setup(FLEA_PS_LP_PENDING, 0, 0, 0);
        hw.FleaEnablePWM = false;
        hw.FleaPowerTransition = true;
        step(&hw, events[event], FLEA_PS_ACTIVE, 0);
    }
}

static void transaction_done(void)
{
    for (unsigned flea = 0; flea < 2; flea++)
        for (unsigned pwm = 0; pwm < 2; pwm++)
            for (unsigned state = FLEA_PS_ACTIVE; state <= FLEA_PS_LP_COMPLETE; state++)
                for (unsigned outer = 0; outer < 2; outer++) {
                    struct mock_pci_device pci = {
                        .device = flea ? BC_PCI_DEVID_FLEA : BC_PCI_DEVID_LINK
                    };
                    struct crystalhd_hw hw = transition_setup(state);
                    hw.FleaEnablePWM = pwm;
                    hw.fwcmd_trans_active = !!outer;
                    device.pdev = &pci;
                    transitions = 0;
                    crystalhd_flea_fw_transaction_done(&hw);
                    const bool down = flea && pwm && state == FLEA_PS_LP_PENDING && !outer;
                    check(hw.FleaPowerState == (down ? FLEA_PS_LP_COMPLETE : state) &&
                          transitions == (unsigned)down && hw.fwcmd_trans_active == !!outer,
                          "transaction-done reevaluation ignored chip/PWM/state/owner gate");
                    cases++;
                }
    for (unsigned inhibitor = 0; inhibitor < 4; inhibitor++) {
        struct mock_pci_device pci = {.device = BC_PCI_DEVID_FLEA};
        struct crystalhd_hw hw = transition_setup(FLEA_PS_LP_PENDING);
        device.pdev = &pci;
        hw.FleaPowerTransition = inhibitor == 0;
        hw.fwcmd_poisoned = inhibitor == 1;
        hw.fwcmd_active = inhibitor == 2;
        hw.FwCmdCnt = inhibitor == 3;
        transitions = 0;
        crystalhd_flea_fw_transaction_done(&hw);
        check(hw.FleaPowerState == FLEA_PS_LP_PENDING && transitions == 0,
              "transaction-done ignored a pending transition/mailbox inhibitor");
        cases++;
    }
}

static void unhealthy_down(void)
{
    for (unsigned failure = 0; failure < 3; failure++) {
        struct mock_pci_device pci = {.device = BC_PCI_DEVID_FLEA};
        struct crystalhd_hw hw = transition_setup(FLEA_PS_LP_PENDING);
        device.pdev = &pci;
        mark_unhealthy(&hw, failure);
        transitions = 0;
        crystalhd_flea_fw_transaction_done(&hw);
        check(hw.FleaPowerState == FLEA_PS_LP_PENDING && transitions == 0,
              "transaction-done requested down on absent/faulted/stopped device");
        cases++;
        check(!crystalhd_flea_set_power_state(&hw, FLEA_PS_LP_COMPLETE) &&
              device.reads == 0 && device.writes == 0 && device.dram_writes == 0 &&
              hw.FleaPowerState == FLEA_PS_LP_PENDING && !hw.lock &&
              !hw.FleaPowerTransition,
              "direct down touched absent/faulted/stopped device");
        transition_cases++;

        hw = transition_setup(FLEA_PS_LP_PENDING);
        device.health_on_lock = &hw;
        /* First lock is the free-list count, second is final admission. */
        device.health_lock_countdown = 2;
        device.health_failure = failure;
        check(!crystalhd_flea_set_power_state(&hw, FLEA_PS_LP_COMPLETE) &&
              !device.health_on_lock && device.reads == 0 && device.writes == 0 &&
              device.dram_writes == 0 && hw.FleaPowerState == FLEA_PS_LP_PENDING &&
              !hw.lock && !hw.FleaPowerTransition,
              "final locked admission missed new absent/faulted/stopped state");
        transition_cases++;
    }
}

static void direction_sequences(void)
{
    struct crystalhd_hw hw = transition_setup(FLEA_PS_LP_COMPLETE);
    real_init_power_state(&hw);
    check(!hw.FleaEnablePWM && hw.FleaPowerState == FLEA_PS_NONE &&
          !hw.FleaPowerTransition, "real initializer changed disabled default");
    sequence_cases++;

    for (unsigned state = FLEA_PS_NONE; state <= FLEA_PS_LP_COMPLETE; state++)
        for (unsigned pause = 0; pause < 2; pause++) {
            hw = transition_setup(state);
            hw.FleaEnablePWM = false;
            hw.FleaPowerTransition = true;
            check(crystalhd_flea_hw_pause(&hw, pause) == BC_STS_SUCCESS &&
                  hw.FleaPowerState == FLEA_PS_ACTIVE && hw.FleaPowerTransition &&
                  !device.reads && !device.writes && !device.dram_writes &&
                  !device.power_ups, "PWM-off pause/resume changed existing behavior");
            sequence_cases++;
        }

    for (unsigned state = FLEA_PS_ACTIVE; state <= FLEA_PS_LP_COMPLETE; state++) {
        hw = transition_setup(state);
        check(crystalhd_flea_hw_pause(&hw, false) == BC_STS_SUCCESS &&
              hw.FleaPowerState == FLEA_PS_ACTIVE && !hw.FleaPowerTransition &&
              device.power_ups == (unsigned)(state == FLEA_PS_LP_COMPLETE) &&
              device.dram_writes == (unsigned)(state != FLEA_PS_ACTIVE) &&
              !device.power_writes, "resume slept or failed to wake");
        unsigned writes = device.dram_writes;
        check(crystalhd_flea_hw_pause(&hw, false) == BC_STS_SUCCESS &&
              hw.FleaPowerState == FLEA_PS_ACTIVE && device.dram_writes == writes &&
              !device.power_writes, "repeated resume was not idempotent");
        sequence_cases++;

        hw = transition_setup(state);
        check(crystalhd_flea_hw_pause(&hw, true) == BC_STS_SUCCESS &&
              hw.FleaPowerState == FLEA_PS_LP_COMPLETE && !hw.FleaPowerTransition &&
              !device.power_ups, "idle pause woke or failed to accept pause");
        writes = device.dram_writes;
        check(crystalhd_flea_hw_pause(&hw, true) == BC_STS_SUCCESS &&
              hw.FleaPowerState == FLEA_PS_LP_COMPLETE &&
              device.dram_writes == writes && !device.power_ups,
              "repeated completed pause was not idempotent");
        sequence_cases++;
    }

    /* PAUSE success means accepted admission closure, not physical quiescence.
     * The real retirement callback may complete down only after ownership ends.
     */
    struct mock_pci_device pci = {.device = BC_PCI_DEVID_FLEA};
    hw = transition_setup(FLEA_PS_ACTIVE);
    device.pdev = &pci;
    hw.fwcmd_trans_active = true;
    for (unsigned repeat = 0; repeat < 2; repeat++) {
        check(crystalhd_flea_hw_pause(&hw, true) == BC_STS_SUCCESS &&
              hw.FleaPowerState == FLEA_PS_LP_PENDING && !device.power_ups &&
              !device.power_writes && !hw.FleaPowerTransition,
              "busy/repeated pause woke or gated a live owner");
        sequence_cases++;
    }
    real_fw_transaction_done(&hw);
    check(hw.FleaPowerState == FLEA_PS_LP_PENDING && !device.power_writes,
          "retirement callback ignored the live outer owner");
    hw.fwcmd_trans_active = false;
    real_fw_transaction_done(&hw);
    check(hw.FleaPowerState == FLEA_PS_LP_COMPLETE && !hw.FleaPowerTransition &&
          hw.DrvPauseCnt == 1, "retired owner left the accepted pause stuck");
    sequence_cases++;

    for (unsigned complete = 0; complete < 2; complete++) {
        hw = transition_setup(complete ? FLEA_PS_LP_COMPLETE : FLEA_PS_LP_PENDING);
        device.fail_fll_write = true;
        check(crystalhd_flea_hw_pause(&hw, false) == BC_STS_IO_ERROR &&
              hw.FleaPowerState == FLEA_PS_LP_PENDING && !hw.FleaPowerTransition &&
              device.power_ups == complete,
              "resume hid failed FLL publication or leaked reservation");
        device.fail_fll_write = false;
        check(crystalhd_flea_hw_pause(&hw, false) == BC_STS_SUCCESS &&
              hw.FleaPowerState == FLEA_PS_ACTIVE && !hw.FleaPowerTransition &&
              device.power_ups == complete, "resume retry repeated physical wake");
        sequence_cases++;
    }
    hw = transition_setup(FLEA_PS_ACTIVE);
    device.fail_fll_write = true;
    check(crystalhd_flea_hw_pause(&hw, true) == BC_STS_IO_ERROR &&
          hw.FleaPowerState == FLEA_PS_ACTIVE && !hw.FleaPowerTransition &&
          !device.power_writes, "pause hid failed zero-FLL publication");
    device.fail_fll_write = false;
    check(crystalhd_flea_hw_pause(&hw, true) == BC_STS_SUCCESS &&
          hw.FleaPowerState == FLEA_PS_LP_COMPLETE, "pause retry failed");
    sequence_cases++;

    hw = transition_setup(FLEA_PS_ACTIVE);
    device.idle = false;
    check(crystalhd_flea_hw_pause(&hw, true) == BC_STS_SUCCESS &&
          hw.FleaPowerState == FLEA_PS_LP_PENDING && !device.power_writes &&
          !hw.FleaPowerTransition, "optional down failure lost accepted pause");
    device.idle = true;
    check(crystalhd_flea_hw_pause(&hw, true) == BC_STS_SUCCESS &&
          hw.FleaPowerState == FLEA_PS_LP_COMPLETE, "pending pause retry failed");
    sequence_cases++;

    hw = transition_setup(FLEA_PS_ACTIVE);
    device.command_on_lock = &hw;
    /* Selector, free-list count, final setter admission. */
    device.command_lock_countdown = 3;
    check(crystalhd_flea_hw_pause(&hw, true) == BC_STS_SUCCESS &&
          hw.fwcmd_active && hw.FleaPowerState == FLEA_PS_LP_PENDING &&
          device.dram_writes == 1 && !device.last_fll && !device.power_writes &&
          !hw.FleaPowerTransition, "late owner bypassed down recheck or lost pause");
    sequence_cases++;

    const enum FLEA_STATE_CH_EVENT competing[] = {
        FLEA_EVT_PAUSE, FLEA_EVT_RESUME, FLEA_EVT_START_DEVICE, FLEA_EVT_STOP_DEVICE
    };
    for (unsigned event = 0; event < sizeof(competing) / sizeof(competing[0]); event++) {
        hw = transition_setup(FLEA_PS_LP_COMPLETE);
        hw.PwrDwnPiQIntr = true;
        device.nested_hw = &hw;
        device.nested_point = NEST_PICTURE;
        device.nested_explicit_event = true;
        device.nested_event = competing[event];
        check(crystalhd_flea_hw_pause(&hw, false) == BC_STS_SUCCESS &&
              device.nested_calls == 1 && hw.FleaPowerState == FLEA_PS_ACTIVE &&
              !hw.FleaPowerTransition && device.power_ups == 1,
              "competing event overwrote reserved wake");
        sequence_cases++;
    }

    for (unsigned state = FLEA_PS_NONE; state <= FLEA_PS_STOPPED; state++)
        for (unsigned pause = 0; pause < 2; pause++) {
            hw = transition_setup(state);
            check(crystalhd_flea_hw_pause(&hw, pause) == BC_STS_ERR_USAGE &&
                  hw.FleaPowerState == state && !device.dram_writes &&
                  !device.power_writes && !hw.FleaPowerTransition,
                  "runtime event accepted an unstarted/stopped power state");
            sequence_cases++;
        }
    for (unsigned failure = 0; failure < 3; failure++)
        for (unsigned pause = 0; pause < 2; pause++) {
            hw = transition_setup(FLEA_PS_ACTIVE);
            mark_unhealthy(&hw, failure);
            check(crystalhd_flea_hw_pause(&hw, pause) == BC_STS_IO_ERROR &&
                  hw.FleaPowerState == FLEA_PS_ACTIVE && !device.dram_writes &&
                  !device.power_writes && !hw.FleaPowerTransition,
                  "runtime event accepted an absent/faulted/stopped device");
            sequence_cases++;
        }
    for (unsigned pause = 0; pause < 2; pause++) {
        hw = transition_setup(FLEA_PS_LP_PENDING);
        hw.FleaPowerTransition = true;
        check(crystalhd_flea_hw_pause(&hw, pause) == BC_STS_BUSY &&
              hw.FleaPowerState == FLEA_PS_LP_PENDING && hw.FleaPowerTransition &&
              !device.dram_writes && !device.power_writes,
              "pause wrapper hid transition contention");
        sequence_cases++;
    }
    for (enum nested_point point = NEST_FETCH; point <= NEST_REPOST; point++) {
        hw = transition_setup(FLEA_PS_LP_COMPLETE);
        hw.PicQSts = 1;
        device.return_packet = true;
        device.repost_status = BC_STS_BUSY;
        device.nested_hw = &hw;
        device.nested_point = point;
        check(crystalhd_flea_hw_pause(&hw, false) == BC_STS_SUCCESS &&
              device.nested_calls == 1 && device.fetches == 1 && device.reposts == 1 &&
              hw.FleaPowerState == FLEA_PS_LP_COMPLETE && !hw.FleaPowerTransition &&
              !hw.lock && !device.lock_held,
              "post-release competing pause did not defer exactly one RX repost");
        sequence_cases++;
    }
    const unsigned active_owners[] = {1, 2, UINT32_MAX};
    for (unsigned tx = 0; tx < 2; tx++) {
        for (unsigned pending = 0; pending < 2; pending++) {
            for (unsigned owner = 0; owner < sizeof(active_owners) / sizeof(active_owners[0]); owner++) {
                hw = transition_setup(pending ? FLEA_PS_LP_PENDING : FLEA_PS_ACTIVE);
                unsigned *counter = tx ? &hw.FleaTxActive : &hw.FleaIrqActive;
                *counter = active_owners[owner];
                check(crystalhd_flea_hw_pause(&hw, true) == BC_STS_SUCCESS &&
                      hw.FleaPowerState == FLEA_PS_LP_PENDING && !device.power_writes &&
                      *counter == active_owners[owner],
                      "pause gated hardware before full IRQ/TX processing retired");
                *counter = 0;
                check(real_set_next_power_state(&hw, FLEA_EVT_CMD_COMP) == BC_STS_SUCCESS &&
                      hw.FleaPowerState == FLEA_PS_LP_COMPLETE && hw.DrvPauseCnt == 1,
                      "completed IRQ/TX owner left accepted pause stuck");
                sequence_cases++;
            }
        }
    }
}

static void deferred_completion_sequences(void)
{
    for (unsigned window = 1; window <= 3; window++)
        for (unsigned variant = 0; variant < 5; variant++) {
            struct crystalhd_hw hw = transition_setup(variant == 4 ?
                FLEA_PS_LP_PENDING : FLEA_PS_ACTIVE);
            hw.FleaTxActive = 1;
            hw.FleaIrqActive = variant == 1;
            device.idle = variant != 2;
            device.fail_fll_write = variant == 3;
            device.completion_hw = &hw;
            device.completion_unlock_countdown = window;
            const BC_STATUS status = crystalhd_flea_hw_pause(&hw, variant != 4);
            check(status == (variant == 3 ? BC_STS_IO_ERROR : BC_STS_SUCCESS) &&
                  device.completion_calls == 1 && !device.completion_unlock_countdown &&
                  !hw.FleaPowerTransition && !hw.FleaPowerRecheck && !hw.lock,
                  "deferred completion lost request result, event or reservation");
            check(hw.FleaPowerState == (variant == 0 ? FLEA_PS_LP_COMPLETE :
                  variant < 3 ? FLEA_PS_LP_PENDING : FLEA_PS_ACTIVE),
                  "last-owner completion stranded accepted pause or changed failed wake");
            check(device.dram_writes == (variant == 0 || variant == 2 ? 2U : 1U) &&
                  hw.DrvPauseCnt == (variant == 0),
                  "completion replay repeated idle failure or failed to stop at busy owner");
            sequence_cases++;
        }
    struct crystalhd_hw hw = transition_setup(FLEA_PS_ACTIVE);
    hw.FleaTxActive = 1;
    device.completion_hw = &hw;
    device.completion_unlock_countdown = 3;
    device.fail_completion_replay = true;
    check(crystalhd_flea_hw_pause(&hw, true) == BC_STS_SUCCESS &&
          hw.FleaPowerState == FLEA_PS_LP_PENDING && device.dram_writes == 2 &&
          device.completion_calls == 1 && !hw.FleaPowerTransition && !hw.FleaPowerRecheck,
          "failed advisory recheck revoked an already accepted pause");
    sequence_cases++;
}

static void rx_intent_sequences(void)
{
    for (unsigned state = FLEA_PS_ACTIVE; state <= FLEA_PS_LP_COMPLETE; state++)
        for (unsigned pause = 0; pause < 2; pause++)
            for (unsigned prior = 0; prior < 2; prior++) {
                struct crystalhd_hw hw = transition_setup(state);
                hw.hw_pause_issued = prior;
                hw.FleaTxActive = pause && state == FLEA_PS_LP_PENDING;
                device.intent_hw = &hw;
                device.expected_intent = pause;
                check(crystalhd_flea_rx_pause(&hw, pause) == BC_STS_SUCCESS &&
                      hw.hw_pause_issued == !!pause && device.intent_observed &&
                      !hw.FleaPowerTransition,
                      "RX pause/resume did not commit accepted or idempotent intent");
                sequence_cases++;
            }
    for (unsigned pause = 0; pause < 2; pause++)
        for (unsigned prior = 0; prior < 2; prior++)
            for (unsigned failure = 0; failure < 3; failure++) {
                struct crystalhd_hw hw = transition_setup(pause ?
                    FLEA_PS_ACTIVE : FLEA_PS_LP_PENDING);
                hw.hw_pause_issued = prior;
                hw.FleaPowerTransition = failure == 0;
                device.fail_fll_write = failure == 1;
                device.present = failure != 2;
                check(crystalhd_flea_rx_pause(&hw, pause) ==
                      (failure == 0 ? BC_STS_BUSY : BC_STS_IO_ERROR) &&
                      hw.hw_pause_issued == !!prior,
                      "rejected RX request changed previously accepted intent");
                sequence_cases++;
            }
    const enum FLEA_STATE_CH_EVENT firmware_events[] = {
        FLEA_EVT_PAUSE, FLEA_EVT_RESUME, FLEA_EVT_FW_CMD_POST
    };
    for (unsigned event = 0; event < sizeof(firmware_events) / sizeof(firmware_events[0]); event++)
        for (unsigned state = FLEA_PS_ACTIVE; state <= FLEA_PS_LP_COMPLETE; state++)
            for (unsigned prior = 0; prior < 2; prior++) {
                struct crystalhd_hw hw = transition_setup(state);
                hw.hw_pause_issued = prior;
                check(real_set_next_power_state(&hw, firmware_events[event]) == BC_STS_SUCCESS &&
                      hw.hw_pause_issued == !!prior,
                      "firmware wake/pause overwrote independent RX threshold intent");
                sequence_cases++;
            }
    for (enum nested_point point = NEST_FETCH; point <= NEST_REPOST; point++) {
        struct crystalhd_hw hw = transition_setup(FLEA_PS_LP_COMPLETE);
        hw.hw_pause_issued = true;
        hw.PicQSts = 1;
        device.return_packet = true;
        device.repost_status = BC_STS_BUSY;
        device.nested_hw = &hw;
        device.nested_point = point;
        device.nested_explicit_event = true;
        device.nested_event = FLEA_EVT_RX_PAUSE;
        check(crystalhd_flea_rx_pause(&hw, false) == BC_STS_SUCCESS &&
              device.nested_calls == 1 && hw.hw_pause_issued &&
              hw.FleaPowerState == FLEA_PS_LP_COMPLETE && !hw.FleaPowerTransition,
              "completed resume overwrote newer RX pause intent during repost");
        sequence_cases++;
    }
}

static void firmware_wake_intent_sequences(void)
{
    struct mock_pci_device pci = {.device = BC_PCI_DEVID_FLEA};
    for (unsigned complete = 0; complete < 2; complete++)
        for (unsigned resume = 0; resume < 2; resume++) {
            struct crystalhd_hw hw = transition_setup(complete ?
                FLEA_PS_LP_COMPLETE : FLEA_PS_LP_PENDING);
            device.pdev = &pci;
            hw.hw_pause_issued = true;
            hw.fwcmd_trans_active = true;
            check(real_set_next_power_state(&hw, FLEA_EVT_FW_CMD_POST) == BC_STS_SUCCESS &&
                  hw.FleaPowerState == FLEA_PS_ACTIVE && hw.hw_pause_issued &&
                  hw.fwcmd_trans_active && device.power_ups == complete,
                  "temporary firmware wake lost RX intent or outer command ownership");
            if (resume)
                check(crystalhd_flea_rx_pause(&hw, false) == BC_STS_SUCCESS &&
                      !hw.hw_pause_issued, "new RX resume failed to supersede old pause");
            unsigned writes = device.dram_writes;
            hw.fwcmd_trans_active = false;
            real_fw_transaction_done(&hw);
            check(hw.FleaPowerState == (resume ? FLEA_PS_ACTIVE : FLEA_PS_LP_COMPLETE) &&
                  hw.hw_pause_issued == !resume &&
                  device.dram_writes == writes + !resume && !hw.FleaPowerTransition,
                  "firmware completion failed to restore current RX intent");
            check(real_set_next_power_state(&hw, FLEA_EVT_CMD_COMP) == BC_STS_SUCCESS &&
                  hw.FleaPowerState == (resume ? FLEA_PS_ACTIVE : FLEA_PS_LP_COMPLETE) &&
                  device.dram_writes == writes + !resume,
                  "stale completion overrode a newer RX resume or repeated completed pause");
            sequence_cases++;
        }
    for (unsigned owner = 0; owner < 3; owner++) {
        struct crystalhd_hw hw = transition_setup(FLEA_PS_ACTIVE);
        device.pdev = &pci;
        hw.hw_pause_issued = true;
        hw.fwcmd_trans_active = owner == 0;
        hw.FleaTxActive = owner == 1;
        hw.FleaIrqActive = owner == 2;
        check(real_set_next_power_state(&hw, FLEA_EVT_CMD_COMP) == BC_STS_SUCCESS &&
              hw.FleaPowerState == (owner == 2 ? FLEA_PS_LP_PENDING : FLEA_PS_ACTIVE) &&
              hw.hw_pause_issued &&
              !device.power_writes && !hw.FleaPowerTransition,
              "ACTIVE pause restoration gated a live command/TX/IRQ owner");
        hw.fwcmd_trans_active = false;
        hw.FleaTxActive = hw.FleaIrqActive = 0;
        real_fw_transaction_done(&hw);
        check(hw.FleaPowerState == FLEA_PS_LP_COMPLETE && hw.hw_pause_issued,
              "last-owner retirement did not finish deferred RX pause restoration");
        sequence_cases++;
    }
    struct crystalhd_hw hw = transition_setup(FLEA_PS_ACTIVE);
    device.pdev = &pci;
    hw.hw_pause_issued = true;
    /* Callback observed old intent; RX_RESUME linearized before its selector. */
    device.clear_intent_on_lock = &hw;
    real_fw_transaction_done(&hw);
    check(!device.clear_intent_on_lock && !hw.hw_pause_issued &&
          hw.FleaPowerState == FLEA_PS_ACTIVE && !device.dram_writes,
          "callback's stale RX intent snapshot overrode latest locked intent");
    sequence_cases++;

    hw = transition_setup(FLEA_PS_ACTIVE);
    device.pdev = &pci;
    hw.FleaEnablePWM = false;
    hw.hw_pause_issued = true;
    real_fw_transaction_done(&hw);
    check(real_set_next_power_state(&hw, FLEA_EVT_CMD_COMP) == BC_STS_SUCCESS &&
          hw.FleaPowerState == FLEA_PS_ACTIVE && hw.hw_pause_issued &&
          !device.dram_writes, "intent restoration changed disabled PWM behavior");
    sequence_cases++;

    for (unsigned complete = 0; complete < 2; complete++)
        for (unsigned tx = 0; tx < 2; tx++) {
            hw = transition_setup(complete ? FLEA_PS_LP_COMPLETE : FLEA_PS_LP_PENDING);
            device.pdev = &pci;
            hw.hw_pause_issued = true;
            hw.fwcmd_trans_active = true;
            hw.PwrDwnPiQIntr = hw.PwrDwnTxIntr = true;
            device.nested_hw = &hw;
            device.nested_point = tx ? NEST_TX : NEST_PICTURE;
            device.nested_explicit_event = true;
            device.nested_event = FLEA_EVT_CMD_COMP;
            check(real_set_next_power_state(&hw, FLEA_EVT_FW_CMD_POST) == BC_STS_SUCCESS &&
                  device.nested_calls == 1 && hw.FleaPowerState == FLEA_PS_ACTIVE &&
                  hw.hw_pause_issued && hw.fwcmd_trans_active &&
                  !hw.FleaPowerTransition && !hw.FleaPowerRecheck &&
                  device.dram_writes == 1 && !device.power_writes,
                  "wake callback completion undid admitted firmware wake before command post");
            hw.fwcmd_trans_active = false;
            real_fw_transaction_done(&hw);
            check(hw.FleaPowerState == FLEA_PS_LP_COMPLETE && hw.hw_pause_issued &&
                  device.dram_writes == 2 && hw.DrvPauseCnt == 1,
                  "completed temporary command owner did not restore RX pause intent");
            sequence_cases++;
        }
}

static void cleanup_completion_sequences(void)
{
    for (unsigned alive = 0; alive < 2; alive++)
        for (unsigned fail = 0; fail < 2; fail++)
            for (unsigned resume = 0; resume < 2; resume++) {
                struct crystalhd_hw hw = transition_setup(FLEA_PS_ACTIVE);
                hw.hw_pause_issued = true;
                device.firmware_alive = alive;
                device.fail_fll_write = fail;
                device.cleanup_completion = true;
                device.resume_cleanup_on_unlock = resume;
                device.nested_hw = &hw;
                bool success = alive && !fail;
                check(crystalhd_flea_publish_fll(&hw, true) ==
                      (success ? BC_STS_SUCCESS : BC_STS_IO_ERROR),
                      "advisory cleanup replay replaced original FLL result");
                check(hw.FleaPowerState == (!resume && success ?
                      FLEA_PS_LP_COMPLETE : FLEA_PS_ACTIVE) &&
                      hw.hw_pause_issued == !resume && !hw.FleaPowerTransition &&
                      hw.FleaPowerRecheck == !success && !hw.lock && device.cleanup_probes == 1 &&
                      device.cleanup_resumes == resume,
                      "cleanup completion lost pause or overrode newer RX resume");
                check(device.dram_writes == alive + (success && !resume) &&
                      (success || !device.power_writes),
                      "failed cleanup performed DRAM/PM replay or successful replay was lost");
                sequence_cases++;
            }
}

static void failed_intent_transition_sequences(void)
{
    for (unsigned pause = 0; pause < 2; pause++) {
        struct crystalhd_hw hw = transition_setup(pause ? FLEA_PS_ACTIVE : FLEA_PS_LP_PENDING);
        hw.hw_pause_issued = true;
        hw.FleaTxActive = 1;
        device.fail_fll_write = true;
        device.completion_hw = &hw;
        device.completion_unlock_countdown = 1;
        check(crystalhd_flea_rx_pause(&hw, pause) == BC_STS_IO_ERROR &&
              device.completion_calls == 1 && device.dram_writes == 1 &&
              hw.hw_pause_issued && hw.FleaPowerRecheck && !hw.FleaPowerTransition &&
              hw.FleaPowerState == (pause ? FLEA_PS_ACTIVE : FLEA_PS_LP_PENDING) &&
              !device.power_writes,
              "failed FLL transition performed unsolicited advisory retry or lost intent/latch");
        device.fail_fll_write = false;
        check(crystalhd_flea_rx_pause(&hw, pause) == BC_STS_SUCCESS &&
              device.dram_writes == 2 && !hw.FleaPowerRecheck && !hw.FleaPowerTransition &&
              hw.hw_pause_issued == !!pause &&
              hw.FleaPowerState == (pause ? FLEA_PS_LP_COMPLETE : FLEA_PS_ACTIVE),
              "explicit retry did not consume preserved completion with current RX intent");
        sequence_cases++;
    }
}

int main(void)
{
    idle_matrix();
    retired_owners();
    disabled_pwm();
    events();
    command_owners();
    reserved_selector();
    transaction_done();
    unhealthy_down();
    transition_failures();
    transition_success();
    register_round_trips();
    wake_interleavings();
    direction_sequences();
    deferred_completion_sequences();
    rx_intent_sequences();
    firmware_wake_intent_sequences();
    cleanup_completion_sequences();
    failed_intent_transition_sequences();
    printf("PASS: %u Flea power selector cases; %u mocked-MMIO transition cases; "
           "%u integrated event sequences; %u targeted register cycles; no device access\n",
           cases, transition_cases, sequence_cases, register_cycles);
    return 0;
}
