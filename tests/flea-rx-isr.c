/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Exercise the production Flea RX ISR with deterministic lock and ownership
 * boundaries. The handlers and MMIO are stubs so the test controls the exact
 * completion interleaving without emulating the device.
 */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "bc_dts_types.h"
struct _BC_DTS_PROC_OUT;
#include "bc_dts_defs.h"
#include "bcm_70012_regs.h"
#include "FleaDefs.h"
#include "rx-isr-types.h"

#define KERN_ERR ""
#define printk(...) ((void)0)
#define dev_info(dev, ...) ((void)(dev))
#define MAYBE_UNUSED __attribute__((unused))

struct device { int unused; };
struct pci_dev { struct device dev; };
struct crystalhd_adp { struct pci_dev *pdev; };
struct crystalhd_rx_dma_pkt { unsigned id; };
struct crystalhd_hw_stats { uint32_t rx_success, rx_errors; };
struct crystalhd_hw {
    struct crystalhd_adp *adp;
    int rx_lock;
    enum list_sts rx_list_sts[DMA_ENGINE_CNT];
    struct crystalhd_hw_stats stats;
    uint32_t (*pfnReadDevRegister)(struct crystalhd_adp *, uint32_t);
    void (*pfnWriteDevRegister)(struct crystalhd_adp *, uint32_t, uint32_t);
    void (*pfnHWGetDoneSize)(struct crystalhd_hw *, uint32_t,
                             uint32_t *, uint32_t *);
};

static struct pci_dev endpoint;
static struct crystalhd_adp adapter = { .pdev = &endpoint };
static struct crystalhd_hw hardware;
static struct crystalhd_rx_dma_pkt old_packet = { .id = 1 };
static struct crystalhd_rx_dma_pkt fresh_packet = { .id = 2 };
static struct crystalhd_rx_dma_pkt *active_packet;
static struct crystalhd_rx_dma_pkt *detached_packet;
static struct crystalhd_rx_dma_pkt *free_packet;
static struct crystalhd_rx_dma_pkt *ready_packet;
static unsigned checks, failures, lock_recursions;
static unsigned detach_calls, explicit_complete_calls, legacy_complete_calls;
static unsigned fresh_posts, start_calls, list0_calls, list1_calls;
static unsigned duplicate_active_tags;
static bool inject_fresh_post;
static bool success_scenario, success_done_under_lock, done_size_under_lock;

static void check(bool condition, const char *message)
{
    checks++;
    if (!condition) {
        failures++;
        fprintf(stderr, "FAIL: %s\n", message);
    }
}

static void post_fresh_at_unlock(void);

static void test_spin_lock(int *lock, unsigned long *flags)
{
    *flags = 0;
    if (*lock) {
        lock_recursions++;
        return;
    }
    *lock = 1;
}

static void test_spin_unlock(int *lock, unsigned long flags)
{
    (void)flags;
    assert(*lock == 1);
    *lock = 0;
    post_fresh_at_unlock();
}

#define spin_lock_irqsave(lock, flags) test_spin_lock((lock), &(flags))
#define spin_unlock_irqrestore(lock, flags) test_spin_unlock((lock), (flags))

static void post_fresh_at_unlock(void)
{
    unsigned long flags;

    if (!inject_fresh_post || fresh_posts ||
        hardware.rx_list_sts[0] != sts_free)
        return;

    fresh_posts++;
    spin_lock_irqsave(&hardware.rx_lock, flags);
    check(list0_calls == 1 && list1_calls == 1,
          "the ISR snapshots both list states before exposing a free slot");
    if (active_packet)
        duplicate_active_tags++;
    check(active_packet == NULL,
          "the completed packet is detached before its list can be reused");
    active_packet = &fresh_packet;
    hardware.rx_list_sts[0] = rx_waiting_y_intr;
    spin_unlock_irqrestore(&hardware.rx_lock, flags);
}

static uint32_t read_register(struct crystalhd_adp *adp, uint32_t reg)
{
    assert(adp == &adapter);
    if (reg == BCHP_MISC1_Y_RX_ERROR_STATUS)
        return success_scenario ? 0 :
            MISC1_Y_RX_ERROR_STATUS_RX_L0_OVERRUN_ERROR_MASK;
    if (reg == BCHP_MISC1_HIF_RX_ERROR_STATUS)
        return 0;
    assert(false);
    return 0;
}

static void write_register(struct crystalhd_adp *adp, uint32_t reg,
                           uint32_t value)
{
    (void)reg;
    (void)value;
    assert(adp == &adapter);
}

static void get_done_size(struct crystalhd_hw *hw, uint32_t index,
                          uint32_t *y_size, uint32_t *uv_size)
{
    assert(hw == &hardware && index == 0);
    done_size_under_lock = hw->rx_lock == 1;
    *y_size = 128;
    *uv_size = 64;
}

static bool crystalhd_flea_rx_list0_handler(
    struct crystalhd_hw *hw, union FLEA_INTR_BITS_COMMON intr_sts,
    uint32_t y_err_sts, uint32_t uv_err_sts)
{
    assert(hw == &hardware && hw->rx_lock == 1);
    list0_calls++;
    if (success_scenario) {
        assert(intr_sts.L0YRxDMADone && !y_err_sts && !uv_err_sts);
        hw->rx_list_sts[0] = sts_free;
    } else {
        assert(intr_sts.L0YRxDMAErr && y_err_sts && !uv_err_sts);
        hw->rx_list_sts[0] = rx_y_error;
    }
    return true;
}

static bool crystalhd_flea_rx_list1_handler(
    struct crystalhd_hw *hw, union FLEA_INTR_BITS_COMMON intr_sts,
    uint32_t y_err_sts, uint32_t uv_err_sts)
{
    (void)intr_sts;
    (void)y_err_sts;
    (void)uv_err_sts;
    assert(hw == &hardware && hw->rx_lock == 1);
    list1_calls++;
    return false;
}

static MAYBE_UNUSED struct crystalhd_rx_dma_pkt *crystalhd_rx_pkt_detach(
    struct crystalhd_hw *hw, uint32_t list_index, BC_STATUS status)
{
    detach_calls++;
    check(hw == &hardware && hw->rx_lock == 1,
          "the ISR detaches completion ownership while holding rx_lock");
    check(list_index == 0 && status == BC_STS_ERROR,
          "the ISR detaches the failed list with its completion status");
    if (active_packet != &old_packet)
        return NULL;
    active_packet = NULL;
    detached_packet = &old_packet;
    return detached_packet;
}

static MAYBE_UNUSED BC_STATUS crystalhd_rx_pkt_complete(
    struct crystalhd_hw *hw, struct crystalhd_rx_dma_pkt *packet,
    uint32_t list_index, BC_STATUS status)
{
    unsigned long flags;

    explicit_complete_calls++;
    check(hw == &hardware && !hw->rx_lock,
          "explicit packet completion runs outside rx_lock");
    check(packet == &old_packet && packet == detached_packet &&
          list_index == 0 && status == BC_STS_ERROR,
          "completion retains the packet detached for this failed list");

    spin_lock_irqsave(&hw->rx_lock, flags);
    if (hw->rx_lock && active_packet)
        free_packet = packet;
    else
        active_packet = packet;
    spin_unlock_irqrestore(&hw->rx_lock, flags);
    detached_packet = NULL;
    return active_packet == packet ? BC_STS_SUCCESS : BC_STS_BUSY;
}

static MAYBE_UNUSED BC_STATUS crystalhd_rx_pkt_done(
    struct crystalhd_hw *hw, uint32_t list_index, BC_STATUS status)
{
    legacy_complete_calls++;
    check(list_index == 0 &&
          status == (success_scenario ? BC_STS_SUCCESS : BC_STS_ERROR),
          "legacy completion receives the expected list status");
    if (success_scenario) {
        uint32_t y_size, uv_size;

        success_done_under_lock = hw->rx_lock == 1;
        hw->pfnHWGetDoneSize(hw, list_index, &y_size, &uv_size);
        check(y_size == 128 && uv_size == 64,
              "successful completion samples the expected done sizes");
        if (active_packet == &old_packet) {
            active_packet = NULL;
            ready_packet = &old_packet;
        }
        return BC_STS_SUCCESS;
    }
    if (hw->rx_lock)
        lock_recursions++;
    if (active_packet == &old_packet) {
        active_packet = NULL;
        detached_packet = &old_packet;
        free_packet = &old_packet;
    }
    return BC_STS_BUSY;
}

static BC_STATUS crystalhd_hw_start_capture(struct crystalhd_hw *hw)
{
    assert(hw == &hardware && !hw->rx_lock);
    start_calls++;
    return BC_STS_SUCCESS;
}

#include "rx-flea-isr.h"

static void reset_scenario(bool success)
{
    memset(&hardware, 0, sizeof(hardware));
    hardware.adp = &adapter;
    hardware.pfnReadDevRegister = read_register;
    hardware.pfnWriteDevRegister = write_register;
    hardware.pfnHWGetDoneSize = get_done_size;
    hardware.rx_list_sts[0] = rx_waiting_y_intr;
    active_packet = &old_packet;
    detached_packet = free_packet = ready_packet = NULL;
    lock_recursions = detach_calls = explicit_complete_calls = 0;
    legacy_complete_calls = fresh_posts = start_calls = 0;
    list0_calls = list1_calls = duplicate_active_tags = 0;
    inject_fresh_post = !success;
    success_scenario = success;
    success_done_under_lock = done_size_under_lock = false;
}

static void run_error_scenario(void)
{
    union FLEA_INTR_BITS_COMMON interrupt = { .WholeReg = 0 };

    reset_scenario(false);
    interrupt.L0YRxDMAErr = 1;

    crystalhd_flea_rx_isr(&hardware, interrupt);

    check(!hardware.rx_lock && !lock_recursions,
          "Flea RX error retry never recursively acquires rx_lock");
    check(detach_calls == 1 && explicit_complete_calls == 1 &&
          legacy_complete_calls == 0,
          "the ISR detaches once and completes the explicit packet once");
    check(fresh_posts == 1 && active_packet == &fresh_packet &&
          free_packet == &old_packet && !detached_packet &&
          !duplicate_active_tags,
          "a concurrent fresh post cannot duplicate or steal the old owner");
    check(hardware.rx_list_sts[0] == rx_waiting_y_intr &&
          hardware.stats.rx_errors == 1 && !hardware.stats.rx_success,
          "error accounting and the freshly posted list state are preserved");
    check(list0_calls == 1 && list1_calls == 1 && !start_calls,
          "the error interrupt visits both lists without a success restart");
}

static void run_success_scenario(void)
{
    union FLEA_INTR_BITS_COMMON interrupt = { .WholeReg = 0 };

    reset_scenario(true);
    interrupt.L0YRxDMADone = 1;

    crystalhd_flea_rx_isr(&hardware, interrupt);

    check(!hardware.rx_lock && !lock_recursions &&
          success_done_under_lock && done_size_under_lock,
          "successful completion samples live list sizes while holding rx_lock");
    check(!detach_calls && !explicit_complete_calls &&
          legacy_complete_calls == 1,
          "successful completion stays on the synchronous list path");
    check(!active_packet && ready_packet == &old_packet &&
          !detached_packet && !free_packet,
          "successful completion transfers the old owner directly to ready");
    check(hardware.rx_list_sts[0] == sts_free &&
          hardware.stats.rx_success == 1 && !hardware.stats.rx_errors,
          "success accounting and the completed list state are preserved");
    check(list0_calls == 1 && list1_calls == 1 && start_calls == 1,
          "successful completion restarts capture only after releasing rx_lock");
}

static void run_missing_owner_scenario(void)
{
    union FLEA_INTR_BITS_COMMON interrupt = { .WholeReg = 0 };

    reset_scenario(false);
    active_packet = NULL;
    inject_fresh_post = false;
    interrupt.L0YRxDMAErr = 1;

    crystalhd_flea_rx_isr(&hardware, interrupt);

    check(!hardware.rx_lock && !lock_recursions && detach_calls == 1 &&
          !explicit_complete_calls && !legacy_complete_calls,
          "a missing active owner is not completed or retried");
    check(!active_packet && !detached_packet && !free_packet &&
          hardware.stats.rx_errors == 1 && !start_calls,
          "a missing owner leaves queues untouched and the lock balanced");
}

int main(void)
{
    run_error_scenario();
    run_success_scenario();
    run_missing_owner_scenario();

    printf("Flea RX ISR lock: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
