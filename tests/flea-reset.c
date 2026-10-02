/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Actual production reset/start/stop bodies; no device or MMIO access. */
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
};

static struct pci_dev endpoint;
static struct crystalhd_adp adapter = { .pdev = &endpoint };
static unsigned clear_at, reset_reads, reset_writes, other_reads, other_writes;
static unsigned sleeps, sleep_ms, callbacks, cases;
static uint32_t unrelated_bits;
static bool reset_complete;

static uint32_t read_register(struct crystalhd_adp *adp, uint32_t reg)
{
    assert(adp == &adapter && reset_writes == 1);
    if (reg == BCHP_MISC3_RESET_CTRL) {
        reset_reads++;
        reset_complete = clear_at && reset_reads >= clear_at;
        return unrelated_bits | (reset_complete ? 0 :
                                 BCHP_MISC3_RESET_CTRL_CORE_RESET_MASK);
    }
    assert(reset_complete);
    other_reads++;
    /* Both successful shutdown polls complete without changing their logic. */
    return reg == BCHP_DDR23_CTL_REGS_0_CTL_STATUS ?
           BCHP_DDR23_CTL_REGS_0_CTL_STATUS_idle_MASK : 0;
}

static void write_register(struct crystalhd_adp *adp, uint32_t reg, uint32_t value)
{
    assert(adp == &adapter);
    if (reg == BCHP_MISC3_RESET_CTRL) {
        assert(!reset_writes && !reset_reads &&
               value == BCHP_MISC3_RESET_CTRL_CORE_RESET_MASK);
        reset_writes++;
        return;
    }
    assert(reset_complete);
    other_writes++;
}

static int msleep_interruptible(unsigned delay)
{
    assert(delay == 1 || delay == 5);
    sleeps++;
    sleep_ms += delay;
    return 0;
}

static void callback(struct crystalhd_hw *hw)
{
    assert(hw->adp == &adapter && reset_complete);
    callbacks++;
}

static void crystalhd_flea_clear_interrupts(struct crystalhd_hw *hw) { callback(hw); }
static void crystalhd_flea_disable_interrupts(struct crystalhd_hw *hw) { callback(hw); }
static void crystalhd_flea_enable_interrupts(struct crystalhd_hw *hw) { callback(hw); }
static void crystalhd_flea_init_dram(struct crystalhd_hw *hw) { callback(hw); }
static void crystalhd_flea_init_power_state(struct crystalhd_hw *hw) { callback(hw); }
static void crystalhd_flea_init_temperature_measure(struct crystalhd_hw *hw, bool on)
{
    (void)on;
    callback(hw);
}
static void crystalhd_flea_set_next_power_state(struct crystalhd_hw *hw, enum FLEA_STATE_CH_EVENT event)
{
    assert(event == FLEA_EVT_START_DEVICE || event == FLEA_EVT_STOP_DEVICE);
    callback(hw);
}

#include "flea-reset-functions.h"

static void run_case(unsigned operation, unsigned release, uint32_t other)
{
    struct crystalhd_hw hw = {
        .adp = &adapter, .rx_list_post_index = 17, .RxCaptureState = 23,
        .pfnReadDevRegister = read_register, .pfnWriteDevRegister = write_register,
    };
    bool result;

    cases++;
    clear_at = release;
    unrelated_bits = other;
    reset_reads = reset_writes = other_reads = other_writes = 0;
    sleeps = sleep_ms = callbacks = 0;
    reset_complete = false;
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
        assert(other_reads && other_writes && callbacks == 8);
        assert(sleeps == release + 4U && sleep_ms == release + 12U);
        assert(!hw.rx_list_post_index && !hw.RxCaptureState);
    } else {
        assert(other_reads && other_writes && callbacks == 4);
        assert(sleeps == release + 2U && sleep_ms == release + 6U);
        assert(hw.rx_list_post_index == 17 && hw.RxCaptureState == 23);
    }
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
    printf("Flea reset: %u production reset/start/stop scenarios passed\n", cases);
    return 0;
}
