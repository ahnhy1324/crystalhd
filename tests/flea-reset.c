/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Actual production reset/DDR/start/stop bodies; no device or MMIO access. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include "70015/magnum/basemodules/chp/70015/rdb/a0/bchp_misc3.h"
#include "FleaDefs.h"
#include "flea-reset-limit.h"

#define BC_BIT(bit) (UINT32_C(1) << (bit))
#define dev_dbg(dev, ...) ((void)(dev))
#define printk(...) ((void)0)

struct device { int unused; };
struct pci_dev { struct device dev; };
struct crystalhd_adp { struct pci_dev *pdev; };
struct crystalhd_hw {
    struct crystalhd_adp *adp;
    uint32_t rx_list_post_index, RxCaptureState;
    uint32_t (*pfnReadDevRegister)(struct crystalhd_adp *, uint32_t);
    void (*pfnWriteDevRegister)(struct crystalhd_adp *, uint32_t, uint32_t);
    enum FLEA_POWER_STATES FleaPowerState;
    bool FleaEnablePWM;
};
#include "crystalhd_flea_ddr.h"

static struct pci_dev endpoint;
static struct crystalhd_adp adapter = { .pdev = &endpoint };
static unsigned clear_at, reset_reads, reset_writes, other_reads, other_writes;
static unsigned sleeps, sleep_ms, callbacks, cases;
static uint32_t unrelated_bits;
static bool reset_complete;
static bool require_reset, detected_ddr3;

enum event_kind { READ, WRITE, SLEEP, CALL };
enum callback_id { CLEAR, DISABLE, ENABLE, DETECT_DDR, CONTROLLER, ARBITER,
                   TEMPERATURE_ON, TEMPERATURE_OFF, POWER_INIT, START, STOP, CALLBACK_END };
struct event { enum event_kind kind; uint32_t address, value; };
static struct event events[4096];
static unsigned event_count, callback_count[CALLBACK_END];
struct register_value { uint32_t address, value; };
static struct register_value registers[96];
static unsigned register_count;
struct status_script { unsigned ready_at, terminal, reads; uint32_t noise, observed; };
static struct status_script pll, lane[2], zq;
static unsigned calibration_clear, override_reads;
static bool calibration_started;

static void record(enum event_kind kind, uint32_t address, uint32_t value)
{
    assert(event_count < sizeof(events) / sizeof(events[0]));
    events[event_count++] = (struct event){kind, address, value};
}

static uint32_t stored_register(uint32_t reg)
{
    for (unsigned i = 0; i < register_count; i++)
        if (registers[i].address == reg)
            return registers[i].value;
    return 0;
}

static void store_register(uint32_t reg, uint32_t value)
{
    for (unsigned i = 0; i < register_count; i++)
        if (registers[i].address == reg) {
            registers[i].value = value;
            return;
        }
    assert(register_count < sizeof(registers) / sizeof(registers[0]));
    registers[register_count++] = (struct register_value){reg, value};
}

static uint32_t status_read(struct status_script *script, uint32_t mask)
{
    script->reads++;
    script->observed = script->noise | (script->ready_at && script->reads >= script->ready_at ?
                                       script->terminal & mask : 0);
    return script->observed;
}

static uint32_t read_register(struct crystalhd_adp *adp, uint32_t reg)
{
    uint32_t value;
    assert(adp == &adapter && (!require_reset || reset_writes == 1));
    if (reg == BCHP_MISC3_RESET_CTRL) {
        reset_reads++;
        reset_complete = clear_at && reset_reads >= clear_at;
        value = unrelated_bits | (reset_complete ? 0 : BCHP_MISC3_RESET_CTRL_CORE_RESET_MASK);
        record(READ, reg, value);
        return value;
    }
    assert(reset_complete);
    other_reads++;
    if (reg == BCHP_DDR23_PHY_CONTROL_REGS_PLL_STATUS)
        value = status_read(&pll, 1);
    else if (reg == BCHP_DDR23_PHY_BYTE_LANE_0_VDL_STATUS) {
        if (calibration_clear == 3) {
            override_reads++;
            value = lane[0].observed;
        } else
            value = status_read(&lane[0], 3);
    } else if (reg == BCHP_DDR23_PHY_BYTE_LANE_1_VDL_STATUS)
        value = status_read(&lane[1], 3);
    else if (reg == BCHP_DDR23_PHY_CONTROL_REGS_ZQ_PVT_COMP_CTL)
        value = status_read(&zq, BCHP_DDR23_PHY_CONTROL_REGS_ZQ_PVT_COMP_CTL_sample_done_MASK);
    else if (reg == BCHP_DDR23_CTL_REGS_0_CTL_STATUS)
        value = BCHP_DDR23_CTL_REGS_0_CTL_STATUS_idle_MASK;
    else
        value = stored_register(reg);
    record(READ, reg, value);
    return value;
}

static void write_register(struct crystalhd_adp *adp, uint32_t reg, uint32_t value)
{
    assert(adp == &adapter);
    if (reg == BCHP_MISC3_RESET_CTRL) {
        assert(!reset_writes && !reset_reads &&
               value == BCHP_MISC3_RESET_CTRL_CORE_RESET_MASK);
        reset_writes++;
        record(WRITE, reg, value);
        return;
    }
    assert(reset_complete);
    other_writes++;
    record(WRITE, reg, value);
    store_register(reg, value);
    if (reg == BCHP_DDR23_PHY_BYTE_LANE_0_VDL_CALIBRATE ||
        reg == BCHP_DDR23_PHY_BYTE_LANE_1_VDL_CALIBRATE) {
        unsigned bit = reg == BCHP_DDR23_PHY_BYTE_LANE_0_VDL_CALIBRATE ? 1 : 2;
        if (value == 3) {
            calibration_started = true;
            calibration_clear &= ~bit;
        } else if (calibration_started && value == 0)
            calibration_clear |= bit;
    }
}

static int msleep_interruptible(unsigned delay)
{
    assert(delay == 1 || delay == 5);
    sleeps++;
    sleep_ms += delay;
    record(SLEEP, delay, 0);
    return 0;
}

static void callback(struct crystalhd_hw *hw, enum callback_id id)
{
    assert(hw->adp == &adapter && reset_complete);
    callback_count[id]++;
    record(CALL, id, 0);
    if (id != DETECT_DDR && id != CONTROLLER && id != ARBITER)
        callbacks++;
}

static void crystalhd_flea_clear_interrupts(struct crystalhd_hw *hw) { callback(hw, CLEAR); }
static void crystalhd_flea_disable_interrupts(struct crystalhd_hw *hw) { callback(hw, DISABLE); }
static void crystalhd_flea_enable_interrupts(struct crystalhd_hw *hw) { callback(hw, ENABLE); }
static bool crystalhd_flea_detect_ddr3(struct crystalhd_hw *hw)
{
    callback(hw, DETECT_DDR);
    return detected_ddr3;
}
void crystalhd_flea_ddr_ctrl_init(struct crystalhd_hw *hw, int32_t port, int32_t ddr3,
                                int32_t speed, int32_t col, int32_t bank, int32_t row,
                                uint32_t tmode)
{
    assert(port == 0 && ddr3 == detected_ddr3 && speed == DDR2_400MHZ &&
           col == COL_BITS_10 && bank == BANK_SIZE_8 &&
           row == (detected_ddr3 ? ROW_SIZE_16K : ROW_SIZE_8K) && tmode == 0);
    callback(hw, CONTROLLER);
}
void crystalhd_flea_ddr_arb_rts_init(struct crystalhd_hw *hw) { callback(hw, ARBITER); }
static void crystalhd_flea_init_power_state(struct crystalhd_hw *hw)
{
    callback(hw, POWER_INIT);
    hw->FleaPowerState = FLEA_PS_NONE;
    hw->FleaEnablePWM = false;
}
static void crystalhd_flea_init_temperature_measure(struct crystalhd_hw *hw, bool on)
{
    callback(hw, on ? TEMPERATURE_ON : TEMPERATURE_OFF);
}
static void crystalhd_flea_set_next_power_state(struct crystalhd_hw *hw, enum FLEA_STATE_CH_EVENT event)
{
    assert(event == FLEA_EVT_START_DEVICE || event == FLEA_EVT_STOP_DEVICE);
    callback(hw, event == FLEA_EVT_START_DEVICE ? START : STOP);
    hw->FleaPowerState = event == FLEA_EVT_START_DEVICE ? FLEA_PS_ACTIVE : FLEA_PS_STOPPED;
}

#include "flea-reset-functions.h"

static void setup(void)
{
    clear_at = 1;
    unrelated_bits = 0;
    reset_reads = reset_writes = other_reads = other_writes = 0;
    sleeps = sleep_ms = callbacks = event_count = register_count = 0;
    calibration_clear = override_reads = 0;
    calibration_started = false;
    require_reset = false;
    reset_complete = true;
    detected_ddr3 = false;
    pll = (struct status_script){1, 1, 0, 0, 0};
    lane[0] = lane[1] = (struct status_script){1, 3, 0, 0x2a0, 0x2a3};
    zq = (struct status_script){1, BCHP_DDR23_PHY_CONTROL_REGS_ZQ_PVT_COMP_CTL_sample_done_MASK, 0, 0, 0};
    for (unsigned i = 0; i < CALLBACK_END; i++)
        callback_count[i] = 0;
}

static struct crystalhd_hw hardware(void)
{
    return (struct crystalhd_hw){
        .adp = &adapter, .rx_list_post_index = 17, .RxCaptureState = 23,
        .pfnReadDevRegister = read_register, .pfnWriteDevRegister = write_register,
        .FleaPowerState = FLEA_PS_STOPPED, .FleaEnablePWM = true,
    };
}

static unsigned occurrences(enum event_kind kind, uint32_t address, uint32_t value)
{
    unsigned count = 0;
    for (unsigned i = 0; i < event_count; i++)
        if (events[i].kind == kind && events[i].address == address && events[i].value == value)
            count++;
    return count;
}

static unsigned accesses(enum event_kind kind, uint32_t address)
{
    unsigned count = 0;
    for (unsigned i = 0; i < event_count; i++)
        if (events[i].kind == kind && events[i].address == address)
            count++;
    return count;
}

static unsigned position(enum event_kind kind, uint32_t address, uint32_t value)
{
    for (unsigned i = 0; i < event_count; i++)
        if (events[i].kind == kind && events[i].address == address && events[i].value == value)
            return i;
    assert(!"missing expected event");
    return event_count;
}

static void no_phy_tail(void)
{
    const uint32_t tail[] = {
        BCHP_DDR23_PHY_CONTROL_REGS_STATIC_VDL_OVERRIDE, BCHP_DDR23_PHY_CONTROL_REGS_ZQ_PVT_COMP_CTL,
        BCHP_DDR23_CTL_REGS_0_UPDATE_VDL, BCHP_DDR23_PHY_CONTROL_REGS_DRIVE_PAD_CTL,
        BCHP_DDR23_PHY_BYTE_LANE_0_VDL_OVERRIDE_0, BCHP_DDR23_PHY_BYTE_LANE_0_VDL_OVERRIDE_1,
        BCHP_DDR23_PHY_BYTE_LANE_0_VDL_OVERRIDE_2, BCHP_DDR23_PHY_BYTE_LANE_0_VDL_OVERRIDE_3,
        BCHP_DDR23_PHY_BYTE_LANE_1_VDL_OVERRIDE_0, BCHP_DDR23_PHY_BYTE_LANE_1_VDL_OVERRIDE_1,
        BCHP_DDR23_PHY_BYTE_LANE_1_VDL_OVERRIDE_2, BCHP_DDR23_PHY_BYTE_LANE_1_VDL_OVERRIDE_3,
    };
    for (unsigned i = 0; i < sizeof(tail) / sizeof(tail[0]); i++)
        assert(!accesses(READ, tail[i]) && !accesses(WRITE, tail[i]));
}

static void run_case(unsigned operation, unsigned release, uint32_t other)
{
    struct crystalhd_hw hw;
    bool result;

    setup();
    hw = hardware();
    cases++;
    clear_at = release;
    unrelated_bits = other;
    reset_reads = reset_writes = other_reads = other_writes = 0;
    sleeps = sleep_ms = callbacks = 0;
    reset_complete = false;
    require_reset = true;
    result = operation == 0 ? crystalhd_flea_core_reset(&hw) :
             operation == 1 ? crystalhd_flea_start_device(&hw) :
                              crystalhd_flea_stop_device(&hw);
    assert(result == (release != 0));
    assert(reset_writes == 1);
    assert(reset_reads == (release ? release : MAX_VALID_POLL_CNT + 1U));
    if (!release) {
        assert(!other_reads && !other_writes && !callbacks);
        assert(sleeps == MAX_VALID_POLL_CNT + 1U && sleep_ms == sleeps);
        assert(hw.rx_list_post_index == 17 && hw.RxCaptureState == 23);
    } else if (operation == 0) {
        assert(!other_reads && !other_writes && !callbacks);
        assert(sleeps == release + 2U && sleep_ms == release + 6U);
    } else if (operation == 1) {
        assert(other_reads && other_writes && callbacks == 7);
        assert(callback_count[CONTROLLER] == 1 && callback_count[ARBITER] == 1);
        assert(sleeps == release + 4U && sleep_ms == release + 12U);
        assert(!hw.rx_list_post_index && !hw.RxCaptureState);
    } else {
        assert(other_reads && other_writes && callbacks == 4);
        assert(sleeps == release + 2U && sleep_ms == release + 6U);
        assert(hw.rx_list_post_index == 17 && hw.RxCaptureState == 23);
    }
}

static unsigned poll_reads(const struct status_script *script, unsigned limit)
{
    return script->terminal && script->ready_at && script->ready_at <= limit ? script->ready_at : limit;
}

/* operation0 tests PLL alone,1 includes real init_dram,2 includes real start. */
static void run_ddr(unsigned operation, uint32_t tmode)
{
    struct crystalhd_hw hw = hardware();
    int32_t speed[2] = {DDR2_400MHZ, DDR2_400MHZ};
    bool p_ok = pll.ready_at && pll.ready_at <= 11;
    bool l0_ok = lane[0].terminal == 3 && lane[0].ready_at && lane[0].ready_at <= 101;
    bool l1_ok = lane[1].terminal == 3 && lane[1].ready_at && lane[1].ready_at <= 101;
    bool z_ok = zq.ready_at && zq.ready_at <= 103;
    bool success = tmode || (p_ok && l0_ok && l1_ok && z_ok);
    unsigned expected_sleeps = 0, expected_reads;
    bool result;
    assert(operation < 3 && (!operation || !tmode));
    if (operation == 2) {
        require_reset = true;
        reset_complete = false;
    }
    cases++;
    result = operation == 0 ? crystalhd_flea_ddr_pll_config(&hw, speed, 1, tmode) :
             operation == 1 ? crystalhd_flea_init_dram(&hw) : crystalhd_flea_start_device(&hw);
    assert(result == success);
    expected_reads = tmode ? 0 : poll_reads(&pll, 11);
    assert(pll.reads == expected_reads);
    if (!tmode)
        expected_sleeps += expected_reads - 1;
    if (!tmode && !p_ok) {
        assert(!lane[0].reads && !lane[1].reads && !zq.reads && !calibration_started);
        assert(!accesses(WRITE, BCHP_DDR23_PHY_BYTE_LANE_0_VDL_CALIBRATE) &&
               !accesses(WRITE, BCHP_DDR23_PHY_BYTE_LANE_1_VDL_CALIBRATE));
        assert(accesses(WRITE, BCHP_DDR23_PHY_CONTROL_REGS_PLL_CONFIG) == 2 &&
               accesses(READ, BCHP_DDR23_PHY_CONTROL_REGS_PLL_CONFIG) == 1);
        no_phy_tail();
        assert(stored_register(BCHP_DDR23_PHY_CONTROL_REGS_PLL_CONFIG) == UINT32_C(0x80000008));
    } else {
        assert(calibration_started && calibration_clear == 3);
        assert(occurrences(WRITE, BCHP_DDR23_PHY_BYTE_LANE_0_VDL_CALIBRATE, 0) == 2);
        assert(occurrences(WRITE, BCHP_DDR23_PHY_BYTE_LANE_1_VDL_CALIBRATE, 0) == 2);
        assert(accesses(WRITE, BCHP_DDR23_PHY_BYTE_LANE_0_VDL_CALIBRATE) == 3 &&
               accesses(WRITE, BCHP_DDR23_PHY_BYTE_LANE_1_VDL_CALIBRATE) == 3);
        expected_reads = tmode ? 0 : poll_reads(&lane[0], 101);
        assert(lane[0].reads == expected_reads);
        if (!tmode)
            expected_sleeps += expected_reads - 1;
        else
            expected_sleeps++;
        expected_reads = tmode || !l0_ok ? 0 : poll_reads(&lane[1], 101);
        assert(lane[1].reads == expected_reads);
        if (expected_reads)
            expected_sleeps += expected_reads - 1;
        if (!tmode && (!l0_ok || !l1_ok)) {
            assert(!override_reads && !zq.reads);
            no_phy_tail();
        } else {
            assert(override_reads == 1 && zq.reads == poll_reads(&zq, 103));
            assert(position(WRITE, BCHP_DDR23_PHY_BYTE_LANE_1_VDL_CALIBRATE, 3) <
                   position(WRITE, BCHP_DDR23_PHY_CONTROL_REGS_ZQ_PVT_COMP_CTL, 0));
        }
    }
    assert(callback_count[CONTROLLER] == (operation && success));
    assert(callback_count[ARBITER] == (operation && success));
    assert(callback_count[DETECT_DDR] == (operation != 0));
    assert(accesses(READ, BCHP_DDR23_PHY_BYTE_LANE_1_CLOCK_PAD_DISABLE) == (operation && success));
    assert(accesses(READ, BCHP_DDR23_PHY_BYTE_LANE_0_READ_CONTROL) == (operation && success));
    assert(accesses(READ, BCHP_DDR23_PHY_BYTE_LANE_1_READ_CONTROL) == (operation && success));
    assert(accesses(WRITE, BCHP_DDR23_PHY_BYTE_LANE_1_CLOCK_PAD_DISABLE) == (operation && success));
    assert(accesses(WRITE, BCHP_DDR23_PHY_BYTE_LANE_0_READ_CONTROL) == (operation && success));
    assert(accesses(WRITE, BCHP_DDR23_PHY_BYTE_LANE_1_READ_CONTROL) == (operation && success));
    assert(occurrences(WRITE, BCHP_DDR23_PHY_BYTE_LANE_1_CLOCK_PAD_DISABLE,
                       BCHP_DDR23_PHY_BYTE_LANE_1_CLOCK_PAD_DISABLE_clk_pad_dis_MASK) == (operation && success));
    if (operation && success) {
        assert(position(CALL, CONTROLLER, 0) < position(CALL, ARBITER, 0));
        assert(position(CALL, ARBITER, 0) < position(WRITE, BCHP_DDR23_PHY_BYTE_LANE_1_CLOCK_PAD_DISABLE,
                                                  BCHP_DDR23_PHY_BYTE_LANE_1_CLOCK_PAD_DISABLE_clk_pad_dis_MASK));
    }
    if (operation == 2) {
        expected_sleeps += success ? 5 : 3;
        assert(reset_reads == 1 && reset_writes == 1);
        assert(callback_count[CLEAR] == (success ? 2U : 1U));
        assert(callback_count[DISABLE] == 1 && callback_count[ENABLE] == success);
        assert(callback_count[TEMPERATURE_ON] == success && callback_count[POWER_INIT] == success && callback_count[START] == success);
        assert(hw.FleaPowerState == (success ? FLEA_PS_ACTIVE : FLEA_PS_STOPPED));
        assert(hw.rx_list_post_index == (success ? 0U : 17U) && hw.RxCaptureState == (success ? 0U : 23U));
        const uint32_t dma[] = {BCHP_MISC1_TX_DMA_CTRL, BCHP_MISC1_HIF_DMA_CTRL,
                                BCHP_MISC1_Y_RX_SW_DESC_LIST_CTRL_STS};
        for (unsigned i = 0; i < 3; i++) {
            assert(occurrences(WRITE, dma[i], 1) == 1 && occurrences(WRITE, dma[i], 0) == !success);
            if (!success) {
                assert(event_count >= 3 && events[event_count - 3 + i].kind == WRITE &&
                       events[event_count - 3 + i].address == dma[i] && !events[event_count - 3 + i].value);
            }
        }
        assert(accesses(READ, BCHP_PCIE_TL_TRANSACTION_CONFIGURATION) == success);
        assert(accesses(WRITE, BCHP_PCIE_TL_TRANSACTION_CONFIGURATION) == success);
        if (success) {
            assert(position(CALL, ARBITER, 0) < position(CALL, TEMPERATURE_ON, 0));
            assert(position(CALL, POWER_INIT, 0) < position(CALL, START, 0));
            assert(position(CALL, START, 0) < position(CALL, ENABLE, 0));
        }
    } else {
        assert(!reset_writes && !reset_reads && !callback_count[START] && !callback_count[ENABLE]);
        assert(hw.FleaPowerState == FLEA_PS_STOPPED);
    }
    assert(sleeps == expected_sleeps);
    assert(sleep_ms == expected_sleeps + (operation == 2 ? (success ? 8U : 4U) : 0U));
}

static void expect_events(unsigned offset, const struct event *expected, unsigned count)
{
    assert(offset + count <= event_count);
    for (unsigned i = 0; i < count; i++)
        assert(events[offset + i].kind == expected[i].kind &&
               events[offset + i].address == expected[i].address && events[offset + i].value == expected[i].value);
}

static void successful_trace(void)
{
    const struct event expected[] = {
        {WRITE, BCHP_DDR23_CTL_REGS_0_SCRATCH, 0},
        {WRITE, BCHP_DDR23_PHY_CONTROL_REGS_PLL_CONFIG, 0x8000000c},
        {WRITE, BCHP_DDR23_PHY_CONTROL_REGS_PLL_PRE_DIVIDER, 0x01003b11},
        {WRITE, BCHP_DDR23_PHY_CONTROL_REGS_PLL_DIVIDER, 0x02000000},
        {READ, BCHP_DDR23_PHY_CONTROL_REGS_PLL_CONFIG, 0x8000000c},
        {WRITE, BCHP_DDR23_PHY_CONTROL_REGS_PLL_CONFIG, 0x80000008},
        {READ, BCHP_DDR23_PHY_CONTROL_REGS_PLL_STATUS, 1},
        {READ, BCHP_DDR23_PHY_CONTROL_REGS_PLL_CONFIG, 0x80000008},
        {WRITE, BCHP_DDR23_PHY_CONTROL_REGS_PLL_CONFIG, 0x80000000},
        {READ, BCHP_DDR23_PHY_CONTROL_REGS_PLL_CONFIG, 0x80000000},
        {WRITE, BCHP_DDR23_PHY_CONTROL_REGS_PLL_CONFIG, 0},
        {WRITE, BCHP_DDR23_PHY_BYTE_LANE_0_VDL_CALIBRATE, 0},
        {WRITE, BCHP_DDR23_PHY_BYTE_LANE_1_VDL_CALIBRATE, 0},
        {WRITE, BCHP_DDR23_PHY_BYTE_LANE_0_VDL_CALIBRATE, 3},
        {WRITE, BCHP_DDR23_PHY_BYTE_LANE_1_VDL_CALIBRATE, 3},
        {READ, BCHP_DDR23_PHY_BYTE_LANE_0_VDL_STATUS, 0x2a3},
        {READ, BCHP_DDR23_PHY_BYTE_LANE_1_VDL_STATUS, 0x2a3},
        {WRITE, BCHP_DDR23_PHY_BYTE_LANE_0_VDL_CALIBRATE, 0},
        {WRITE, BCHP_DDR23_PHY_BYTE_LANE_1_VDL_CALIBRATE, 0},
        {READ, BCHP_DDR23_PHY_BYTE_LANE_0_VDL_STATUS, 0x2a3},
        {WRITE, BCHP_DDR23_PHY_CONTROL_REGS_STATIC_VDL_OVERRIDE, 0x110002},
        {WRITE, BCHP_DDR23_PHY_CONTROL_REGS_ZQ_PVT_COMP_CTL, 0},
        {WRITE, BCHP_DDR23_PHY_CONTROL_REGS_ZQ_PVT_COMP_CTL, BCHP_DDR23_PHY_CONTROL_REGS_ZQ_PVT_COMP_CTL_sample_en_MASK},
        {READ, BCHP_DDR23_PHY_CONTROL_REGS_ZQ_PVT_COMP_CTL, BCHP_DDR23_PHY_CONTROL_REGS_ZQ_PVT_COMP_CTL_sample_done_MASK},
    };
    const struct event start_prefix[] = {
        {WRITE, BCHP_MISC3_RESET_CTRL, BCHP_MISC3_RESET_CTRL_CORE_RESET_MASK},
        {SLEEP, 1, 0}, {READ, BCHP_MISC3_RESET_CTRL, 0}, {SLEEP, 1, 0}, {SLEEP, 5, 0},
        {WRITE, BCHP_SUN_GISB_ARB_TIMER, 0xd80}, {CALL, CLEAR, 0}, {CALL, DISABLE, 0},
        {READ, BCHP_MISC1_DMA_DEBUG_OPTIONS_REG, 0}, {WRITE, BCHP_MISC1_DMA_DEBUG_OPTIONS_REG, 0x10},
        {WRITE, BCHP_MISC1_TX_DMA_CTRL, 1}, {WRITE, BCHP_MISC1_HIF_DMA_CTRL, 1},
        {WRITE, BCHP_MISC1_Y_RX_SW_DESC_LIST_CTRL_STS, 1},
        {READ, BCHP_MISC_PERST_CLOCK_CTRL, 0}, {WRITE, BCHP_MISC_PERST_CLOCK_CTRL, 0},
        {CALL, DETECT_DDR, 0},
    };
    const struct event init_prefix[] = {{CALL, DETECT_DDR, 0}};
    const struct event init_tail[] = {
        {CALL, CONTROLLER, 0}, {CALL, ARBITER, 0},
        {READ, BCHP_DDR23_PHY_BYTE_LANE_1_CLOCK_PAD_DISABLE, 0},
        {WRITE, BCHP_DDR23_PHY_BYTE_LANE_1_CLOCK_PAD_DISABLE, BCHP_DDR23_PHY_BYTE_LANE_1_CLOCK_PAD_DISABLE_clk_pad_dis_MASK},
        {READ, BCHP_DDR23_PHY_BYTE_LANE_0_READ_CONTROL, 0}, {WRITE, BCHP_DDR23_PHY_BYTE_LANE_0_READ_CONTROL, 0},
        {READ, BCHP_DDR23_PHY_BYTE_LANE_1_READ_CONTROL, 0}, {WRITE, BCHP_DDR23_PHY_BYTE_LANE_1_READ_CONTROL, 0},
    };
    const struct event start_tail[] = {
        {SLEEP, 5, 0}, {READ, BCHP_PCIE_TL_TRANSACTION_CONFIGURATION, 0},
        {WRITE, BCHP_PCIE_TL_TRANSACTION_CONFIGURATION, 0}, {CALL, TEMPERATURE_ON, 0},
        {CALL, POWER_INIT, 0}, {CALL, START, 0}, {CALL, CLEAR, 0}, {CALL, ENABLE, 0}, {SLEEP, 1, 0},
    };
    for (unsigned ddr3 = 0; ddr3 < 2; ddr3++)
      for (unsigned operation = 0; operation < 3; operation++) {
        unsigned cursor = 0;
        if (ddr3 && !operation)
            continue;
        setup();
        detected_ddr3 = ddr3;
        run_ddr(operation, 0);
        if (operation) {
            unsigned count = operation == 1 ? sizeof(init_prefix) / sizeof(init_prefix[0]) :
                                             sizeof(start_prefix) / sizeof(start_prefix[0]);
            expect_events(cursor, operation == 1 ? init_prefix : start_prefix, count);
            cursor += count;
        }
        expect_events(cursor, expected, sizeof(expected) / sizeof(expected[0]));
        cursor += sizeof(expected) / sizeof(expected[0]);
        if (operation) {
            expect_events(cursor, init_tail, sizeof(init_tail) / sizeof(init_tail[0]));
            cursor += sizeof(init_tail) / sizeof(init_tail[0]);
        }
        if (operation == 2) {
            expect_events(cursor, start_tail, sizeof(start_tail) / sizeof(start_tail[0]));
            cursor += sizeof(start_tail) / sizeof(start_tail[0]);
        }
        assert(cursor == event_count);
    }
}

static void bypass_trace(unsigned ready_at)
{
    const struct event prefix[] = {
        {WRITE, BCHP_DDR23_CTL_REGS_0_SCRATCH, 0},
        {WRITE, BCHP_DDR23_PHY_BYTE_LANE_0_VDL_CALIBRATE, 0},
        {WRITE, BCHP_DDR23_PHY_BYTE_LANE_1_VDL_CALIBRATE, 0},
        {WRITE, BCHP_DDR23_PHY_BYTE_LANE_0_VDL_CALIBRATE, 3},
        {WRITE, BCHP_DDR23_PHY_BYTE_LANE_1_VDL_CALIBRATE, 3}, {SLEEP, 1, 0},
        {WRITE, BCHP_DDR23_PHY_BYTE_LANE_0_VDL_CALIBRATE, 0},
        {WRITE, BCHP_DDR23_PHY_BYTE_LANE_1_VDL_CALIBRATE, 0},
        {READ, BCHP_DDR23_PHY_BYTE_LANE_0_VDL_STATUS, 0},
        {WRITE, BCHP_DDR23_PHY_BYTE_LANE_0_VDL_OVERRIDE_0, 0x1003f},
        {WRITE, BCHP_DDR23_PHY_BYTE_LANE_0_VDL_OVERRIDE_1, 0x1003f},
        {WRITE, BCHP_DDR23_PHY_BYTE_LANE_0_VDL_OVERRIDE_2, 0x1003f},
        {WRITE, BCHP_DDR23_PHY_BYTE_LANE_0_VDL_OVERRIDE_3, 0x1003f},
        {WRITE, BCHP_DDR23_PHY_BYTE_LANE_1_VDL_OVERRIDE_0, 0x1003f},
        {WRITE, BCHP_DDR23_PHY_BYTE_LANE_1_VDL_OVERRIDE_1, 0x1003f},
        {WRITE, BCHP_DDR23_PHY_BYTE_LANE_1_VDL_OVERRIDE_2, 0x1003f},
        {WRITE, BCHP_DDR23_PHY_BYTE_LANE_1_VDL_OVERRIDE_3, 0x1003f},
        {WRITE, BCHP_DDR23_CTL_REGS_0_UPDATE_VDL, BCHP_DDR23_CTL_REGS_0_UPDATE_VDL_refresh_MASK},
        {WRITE, BCHP_DDR23_PHY_CONTROL_REGS_STATIC_VDL_OVERRIDE, 0x11003f},
        {WRITE, BCHP_DDR23_PHY_CONTROL_REGS_ZQ_PVT_COMP_CTL, 0},
        {WRITE, BCHP_DDR23_PHY_CONTROL_REGS_ZQ_PVT_COMP_CTL, BCHP_DDR23_PHY_CONTROL_REGS_ZQ_PVT_COMP_CTL_sample_en_MASK},
    };
    const struct event tail[] = {
        {WRITE, BCHP_DDR23_PHY_CONTROL_REGS_ZQ_PVT_COMP_CTL, 0x0300ffff},
        {READ, BCHP_DDR23_PHY_CONTROL_REGS_DRIVE_PAD_CTL, 0},
        {WRITE, BCHP_DDR23_PHY_CONTROL_REGS_DRIVE_PAD_CTL, 0},
    };
    unsigned cursor = sizeof(prefix) / sizeof(prefix[0]);
    unsigned reads = ready_at && ready_at <= 103 ? ready_at : 103;
    setup();
    pll.ready_at = lane[0].ready_at = lane[1].ready_at = 0;
    lane[0].observed = 0;
    zq.ready_at = ready_at;
    run_ddr(0, 1);
    expect_events(0, prefix, cursor);
    for (unsigned i = 0; i < reads; i++) {
        uint32_t value = ready_at && i + 1 >= ready_at ?
                         BCHP_DDR23_PHY_CONTROL_REGS_ZQ_PVT_COMP_CTL_sample_done_MASK : 0;
        const struct event read = {READ, BCHP_DDR23_PHY_CONTROL_REGS_ZQ_PVT_COMP_CTL, value};
        expect_events(cursor++, &read, 1);
    }
    expect_events(cursor, tail, sizeof(tail) / sizeof(tail[0]));
    assert(cursor + sizeof(tail) / sizeof(tail[0]) == event_count);
}

static void ddr_cases(void)
{
    const unsigned pll_at[] = {0, 1, 2, 10, 11, 12};
    const unsigned vdl_at[] = {0, 1, 2, 100, 101, 102};
    const unsigned zq_at[] = {0, 1, 2, 102, 103, 104};
    const uint32_t noise[] = {0, UINT32_MAX};
    successful_trace();
    for (unsigned operation = 0; operation < 3; operation++) {
        for (unsigned n = 0; n < 2; n++) {
            for (unsigned i = 0; i < sizeof(pll_at) / sizeof(pll_at[0]); i++) {
                setup(); pll.ready_at = pll_at[i]; pll.noise = noise[n] & ~1U;
                run_ddr(operation, 0);
            }
            for (unsigned l = 0; l < 2; l++)
                for (unsigned state = 0; state < 4; state++)
                    for (unsigned i = 0; i < sizeof(vdl_at) / sizeof(vdl_at[0]); i++) {
                        setup(); lane[l].ready_at = vdl_at[i]; lane[l].terminal = state;
                        lane[l].noise = noise[n] & ~3U;
                        run_ddr(operation, 0);
                    }
            for (unsigned i = 0; i < sizeof(zq_at) / sizeof(zq_at[0]); i++) {
                setup(); zq.ready_at = zq_at[i];
                zq.noise = noise[n] & ~BCHP_DDR23_PHY_CONTROL_REGS_ZQ_PVT_COMP_CTL_sample_done_MASK;
                run_ddr(operation, 0);
            }
        }
        for (unsigned s0 = 0; s0 < 4; s0++)
            for (unsigned s1 = 0; s1 < 4; s1++) {
                setup(); lane[0].terminal = s0; lane[1].terminal = s1;
                run_ddr(operation, 0);
            }
    }
    for (unsigned ddr3 = 0; ddr3 < 2; ddr3++)
        for (unsigned operation = 1; operation < 3; operation++) {
            setup(); detected_ddr3 = ddr3;
            run_ddr(operation, 0);
        }
    for (unsigned i = 0; i < sizeof(zq_at) / sizeof(zq_at[0]); i++)
        bypass_trace(zq_at[i]);
}

int main(void)
{
    const unsigned releases[] = {0, 1, 2, MAX_VALID_POLL_CNT, MAX_VALID_POLL_CNT + 1U};
    const uint32_t other[] = {0, BCHP_MISC3_RESET_CTRL_PLL_RESET_MASK |
                             BCHP_MISC3_RESET_CTRL_LOW_POWER_MASK |
                             BCHP_MISC3_RESET_CTRL_POR_RESET_MASK, UINT32_MAX & ~1U};

    for (unsigned operation = 0; operation < 3; operation++)
        for (unsigned i = 0; i < sizeof(releases) / sizeof(releases[0]); i++)
            for (unsigned j = 0; j < sizeof(other) / sizeof(other[0]); j++)
                run_case(operation, releases[i], other[j]);
    ddr_cases();
    printf("Flea reset/DDR: %u production reset/poll/init/start/stop scenarios passed\n", cases);
    return 0;
}
