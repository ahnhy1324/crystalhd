/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Actual Flea DRAM and raw-register command bodies; no device access. */
#include <assert.h>
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "bc_dts_types.h"
struct _BC_DTS_PROC_OUT;
#include "bc_dts_defs.h"
#include "crystalhd_ioctl_limits.h"
#include "FleaDefs.h"
#include "flea-dram-types.h"

#define KERN_ERR ""
#define printk(...) ((void)0)
#define WINDOW_MASK BCHP_MISC2_DIRECT_WINDOW_CONTROL_DIRECT_WINDOW_BASE_ADDR_MASK
#define WINDOW_ENABLE BCHP_MISC2_DIRECT_WINDOW_CONTROL_DIRECT_WINDOW_ENABLE_MASK
#define WINDOW_BYTES (~(uint32_t)WINDOW_MASK + 1U)
#define WINDOW_WORDS (WINDOW_BYTES / 4U)

struct pci_dev { unsigned device; };
struct crystalhd_adp {
    struct pci_dev *pdev;
    uint8_t *mem_addr;
    pthread_mutex_t dram_lock, gisb_lock;
};
struct crystalhd_hw {
    struct crystalhd_adp *adp;
    enum FLEA_POWER_STATES FleaPowerState;
    void (*pfnWriteDevRegister)(struct crystalhd_adp *, uint32_t, uint32_t);
    void (*pfnWriteFPGARegister)(struct crystalhd_adp *, uint32_t, uint32_t);
};
struct crystalhd_cmd { struct crystalhd_adp *adp; struct crystalhd_hw *hw_ctx; };
typedef struct {
    struct { union { struct { uint32_t Offset, Value; } regAcc; } u; } udata;
} crystalhd_ioctl_data;

enum access_kind { DRAM_READ, DRAM_WRITE, RAW_DEVICE, RAW_LINK };
struct operation {
    enum access_kind kind;
    unsigned id, done, locks, unlocks, selects, reads, writes, flushes;
    unsigned burst_reads, burst_writes, burst_selects, burst_flushes;
    uint32_t start, count, burst_start, burst_count, words[260];
    bool locked;
    unsigned long irq_state, initial_irq_state;
    BC_STATUS status;
};

static _Alignas(uint32_t) uint8_t aperture[WINDOW_BYTES];
/* Sparse backing covers sixteen real windows plus the final DRAM window. */
static uint32_t backing[17][WINDOW_WORDS];
static struct pci_dev endpoint = { .device = BC_PCI_DEVID_FLEA };
static struct crystalhd_adp adapter = {
    .pdev = &endpoint, .mem_addr = aperture,
    .dram_lock = PTHREAD_MUTEX_INITIALIZER, .gisb_lock = PTHREAD_MUTEX_INITIALIZER
};
static struct crystalhd_hw hardware;
static pthread_mutex_t control = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed = PTHREAD_COND_INITIALIZER;
static _Thread_local struct operation *current;
static uint32_t selected;
static unsigned violations, mmio_count, cases, raw_calls;
static bool race, first_selected, contender_attempted, contender_selected, first_pio;

static unsigned slot(uint32_t address)
{
    unsigned window = (address & WINDOW_MASK) / WINDOW_BYTES;
    assert(address < CRYSTALHD_DEVICE_DRAM_SIZE && !(address & 3U));
    if (window < 16) return window;
    assert(window == CRYSTALHD_DEVICE_DRAM_SIZE / WINDOW_BYTES - 1U);
    return 16;
}

static uint32_t *cell(uint32_t address)
{
    return &backing[slot(address)][(address & ~WINDOW_MASK) / 4U];
}

static uint32_t pattern(uint32_t address)
{
    return UINT32_C(0xa5000000) ^ address ^ (address >> 8);
}

static uint32_t burst_size(uint32_t address, uint32_t remaining)
{
    uint32_t n = (WINDOW_BYTES - (address & ~WINDOW_MASK)) / 4U;
    if (n > 64) n = 64;
    return n < remaining ? n : remaining;
}

static void require(bool condition)
{
    if (!condition) violations++;
}

static unsigned long lock_dram(pthread_mutex_t *lock)
{
    unsigned long flags;
    assert(current && !current->locked && lock == &adapter.dram_lock);
    assert(!pthread_mutex_lock(&control));
    if (race && current->id == 1) {
        contender_attempted = true;
        assert(!pthread_cond_broadcast(&changed));
    }
    assert(!pthread_mutex_unlock(&control));
    assert(!pthread_mutex_lock(lock));
    flags = current->irq_state;
    current->irq_state = 1;
    current->locked = true;
    current->locks++;
    current->burst_reads = current->burst_writes = current->burst_selects = 0;
    current->burst_flushes = 0;
    current->burst_start = current->start + current->done * 4U;
    current->burst_count = burst_size(current->burst_start,
                                    current->count - current->done);
    return flags;
}

static void unlock_dram(pthread_mutex_t *lock, unsigned long flags)
{
    assert(current && current->locked && current->irq_state == 1 &&
           lock == &adapter.dram_lock && flags == current->initial_irq_state);
    assert(!pthread_mutex_lock(&control));
    if (current->kind == DRAM_READ) {
        require(current->burst_selects == 1 &&
                current->burst_reads == current->burst_count &&
                !current->burst_writes && !current->burst_flushes);
    } else if (current->kind == DRAM_WRITE) {
        require(current->burst_selects == 1 &&
                current->burst_writes == current->burst_count &&
                current->burst_reads == 1 && current->burst_flushes == 1);
    }
    current->locked = false;
    current->irq_state = flags;
    current->unlocks++;
    assert(!pthread_mutex_unlock(&control));
    assert(!pthread_mutex_unlock(lock));
}

#define spin_lock_irqsave(lock, flags) ((flags) = lock_dram(lock))
#define spin_unlock_irqrestore(lock, flags) unlock_dram((lock), (flags))

static void select_window(struct crystalhd_adp *adp, uint32_t reg, uint32_t value)
{
    assert(current && adp == &adapter && reg == BCHP_MISC2_DIRECT_WINDOW_CONTROL);
    assert(!pthread_mutex_lock(&control));
    require(current->locked && current->irq_state == 1);
    require(value == (((current->start + current->done * 4U) & WINDOW_MASK) |
                      WINDOW_ENABLE));
    selected = value & WINDOW_MASK;
    current->selects++;
    current->burst_selects++;
    mmio_count++;
    if (race && current->id == 0 && !first_selected) {
        first_selected = true;
        assert(!pthread_cond_broadcast(&changed));
        /* A contender must reach its lock attempt (or, for broken unlocked
         * code, overwrite WINDOW) before the first transaction can do PIO.
         */
        while (!contender_attempted && !contender_selected)
            assert(!pthread_cond_wait(&changed, &control));
    } else if (race && current->id == 1 && !contender_attempted) {
        contender_selected = true;
        assert(!pthread_cond_broadcast(&changed));
        while (!first_pio)
            assert(!pthread_cond_wait(&changed, &control));
    }
    assert(!pthread_mutex_unlock(&control));
}

static uint32_t pio(const void *pointer, bool writing, uint32_t value)
{
    uintptr_t offset = (uintptr_t)pointer - (uintptr_t)adapter.mem_addr;
    uint32_t expected, address, result;
    assert(current && offset < WINDOW_BYTES && !(offset & 3U));
    assert(!pthread_mutex_lock(&control));
    require(current->locked && current->irq_state == 1);
    if (current->kind == DRAM_WRITE && !writing) {
        expected = current->burst_start;
        require(current->burst_writes == current->burst_count &&
                !current->burst_flushes);
        current->flushes++;
        current->burst_flushes++;
    } else {
        expected = current->start + current->done * 4U;
        current->done++;
    }
    address = selected + (uint32_t)offset;
    require(address == expected &&
            (address & WINDOW_MASK) == (current->burst_start & WINDOW_MASK));
    result = *cell(address);
    if (writing) {
        *cell(address) = value;
        current->writes++;
        current->burst_writes++;
    } else {
        current->reads++;
        current->burst_reads++;
    }
    mmio_count++;
    if (race && current->id == 0 && !first_pio) {
        first_pio = true;
        assert(!pthread_cond_broadcast(&changed));
    }
    assert(!pthread_mutex_unlock(&control));
    return result;
}

static uint32_t readl(const void *pointer) { return pio(pointer, false, 0); }
static void writel(uint32_t value, void *pointer) { (void)pio(pointer, true, value); }
#include "flea-dram-functions.h"

static void raw_callback(struct crystalhd_adp *adp, uint32_t reg, uint32_t value,
                         enum access_kind kind)
{
    assert(current && adp == &adapter && current->kind == kind);
    require(current->locked == (endpoint.device == BC_PCI_DEVID_FLEA));
    require(reg == current->start && value == current->words[0]);
    raw_calls++;
}
static void raw_device(struct crystalhd_adp *adp, uint32_t reg, uint32_t value)
{ raw_callback(adp, reg, value, RAW_DEVICE); }
static void raw_link(struct crystalhd_adp *adp, uint32_t reg, uint32_t value)
{ raw_callback(adp, reg, value, RAW_LINK); }
#include "flea-dram-commands.h"

static void reset(void)
{
    assert(!current);
    violations = mmio_count = raw_calls = 0;
    race = first_selected = contender_attempted = contender_selected = first_pio = false;
    selected = UINT32_MAX & WINDOW_MASK;
    endpoint.device = BC_PCI_DEVID_FLEA;
    hardware = (struct crystalhd_hw){
        .adp = &adapter, .FleaPowerState = FLEA_PS_ACTIVE,
        .pfnWriteDevRegister = select_window, .pfnWriteFPGARegister = raw_link
    };
}

static void prepare(struct operation *op, enum access_kind kind,
                    uint32_t start, unsigned count, unsigned id)
{
    *op = (struct operation){ .kind = kind, .start = start, .count = count, .id = id };
    assert(count <= sizeof(op->words) / sizeof(op->words[0]));
    for (unsigned i = 0; i < count; i++) {
        *cell(start + 4U * i) = pattern(start + 4U * i);
        op->words[i] = kind == DRAM_WRITE ? (UINT32_C(0x5a123400) ^ start ^ i) : 0;
    }
}

static void *run(void *argument)
{
    struct operation *op = argument;
    current = op;
    if (race && op->id == 1) {
        assert(!pthread_mutex_lock(&control));
        while (!first_selected)
            assert(!pthread_cond_wait(&changed, &control));
        assert(!pthread_mutex_unlock(&control));
    }
    op->status = op->kind == DRAM_READ ?
        crystalhd_flea_mem_rd(&hardware, op->start, op->count, op->words) :
        crystalhd_flea_mem_wr(&hardware, op->start, op->count, op->words);
    current = NULL;
    return NULL;
}

static void verify(const struct operation *op)
{
    unsigned bursts = 0;
    uint32_t address = op->start, remaining = op->count;
    while (remaining) {
        uint32_t n = burst_size(address, remaining);
        bursts++;
        address += 4U * n;
        remaining -= n;
    }
    assert(op->status == BC_STS_SUCCESS && !op->locked &&
           op->irq_state == op->initial_irq_state &&
           op->done == op->count && op->locks == bursts &&
           op->unlocks == bursts && op->selects == bursts);
    assert(op->writes == (op->kind == DRAM_WRITE ? op->count : 0));
    assert(op->reads == (op->kind == DRAM_WRITE ? bursts : op->count));
    assert(op->flushes == (op->kind == DRAM_WRITE ? bursts : 0));
    for (unsigned i = 0; i < op->count; i++) {
        if (op->kind == DRAM_WRITE)
            assert(*cell(op->start + 4U * i) == op->words[i]);
        else
            assert(op->words[i] == pattern(op->start + 4U * i));
    }
    assert(!violations);
}

static void sequential(void)
{
    const struct { uint32_t start, count; } spans[] = {
        {0, 0}, {0, 1}, {4, 63}, {4, 64}, {4, 65}, {4, 129},
        {WINDOW_BYTES - 4U, 1}, {WINDOW_BYTES - 4U, 2},
        {WINDOW_BYTES - 8U, 130}, {8U * WINDOW_BYTES - 4U, 130},
        {CRYSTALHD_DEVICE_DRAM_SIZE - 4U, 1},
        {CRYSTALHD_DEVICE_DRAM_SIZE - 256U, 64}
    };
    for (unsigned kind = DRAM_READ; kind <= DRAM_WRITE; kind++) {
        for (unsigned n = 0; n < sizeof(spans) / sizeof(spans[0]); n++) {
            struct operation op;
            reset();
            prepare(&op, kind, spans[n].start, spans[n].count, 0);
            run(&op);
            verify(&op);
            if (!op.count) assert(!mmio_count);
            cases++;
        }
    }
}

static void concurrent(void)
{
    for (unsigned a = DRAM_READ; a <= DRAM_WRITE; a++) {
        for (unsigned b = DRAM_READ; b <= DRAM_WRITE; b++) {
            struct operation op[2];
            pthread_t thread[2];
            reset();
            prepare(&op[0], a, WINDOW_BYTES + 64U, 129, 0);
            prepare(&op[1], b, 9U * WINDOW_BYTES + 64U, 129, 1);
            race = true;
            assert(!pthread_create(&thread[0], NULL, run, &op[0]));
            assert(!pthread_create(&thread[1], NULL, run, &op[1]));
            assert(!pthread_join(thread[0], NULL));
            assert(!pthread_join(thread[1], NULL));
            assert(first_selected && contender_attempted && first_pio &&
                   !contender_selected);
            verify(&op[0]);
            verify(&op[1]);
            cases++;
        }
    }
}

static void disabled_irq(void)
{
    for (unsigned kind = DRAM_READ; kind <= DRAM_WRITE; kind++) {
        struct operation op;
        reset();
        prepare(&op, kind, 4, 65, 0);
        op.irq_state = op.initial_irq_state = 1;
        run(&op);
        verify(&op);
        cases++;
    }
}

static void rejected(void)
{
    for (unsigned kind = DRAM_READ; kind <= DRAM_WRITE; kind++) {
        struct operation op;
        struct crystalhd_hw absent = {0};
        const struct { uint32_t start, count; } invalid[] = {
            {1, 1}, {1, 0}, {CRYSTALHD_DEVICE_DRAM_SIZE, 1},
            {CRYSTALHD_DEVICE_DRAM_SIZE - 4U, 2}, {UINT32_MAX - 3U, 1},
            {0, UINT32_MAX}, {0, CRYSTALHD_DEVICE_DRAM_SIZE / 4U + 1U}
        };
        reset();
        prepare(&op, kind, 0, 1, 0);
        current = &op;
#define CALL(hw, address, count, buffer) (kind == DRAM_READ ? \
        crystalhd_flea_mem_rd((hw), (address), (count), (buffer)) : \
        crystalhd_flea_mem_wr((hw), (address), (count), (buffer)))
        assert(CALL(NULL, 0, 1, op.words) == BC_STS_INV_ARG);
        assert(CALL(&absent, 0, 1, op.words) == BC_STS_INV_ARG);
        assert(CALL(&hardware, 0, 1, NULL) == BC_STS_INV_ARG);
        hardware.FleaPowerState = FLEA_PS_LP_COMPLETE;
        assert(CALL(&hardware, 0, 1, op.words) == BC_STS_BUSY);
        hardware.FleaPowerState = FLEA_PS_ACTIVE;
        for (unsigned n = 0; n < sizeof(invalid) / sizeof(invalid[0]); n++)
            assert(CALL(&hardware, invalid[n].start, invalid[n].count,
                        op.words) == BC_STS_ERROR);
#undef CALL
        assert(!mmio_count && !op.locks && !op.unlocks && !op.done && !violations);
        current = NULL;
        cases++;
    }
}

static void raw_commands(void)
{
    const uint32_t offsets[] = {
        BCHP_MISC2_DIRECT_WINDOW_CONTROL,
        BCHP_MISC2_DIRECT_WINDOW_CONTROL & UINT32_C(0xffff),
        FLEA_GISB_INDIRECT_ADDRESS, FLEA_GISB_INDIRECT_DATA,
        (FLEA_GISB_DIRECT_BASE << 16) | FLEA_GISB_INDIRECT_ADDRESS,
        (FLEA_GISB_DIRECT_BASE << 16) | FLEA_GISB_INDIRECT_DATA,
        UINT32_C(0xdeadbeef)
    };
    for (unsigned link = 0; link < 2; link++) {
        for (unsigned flea = 0; flea < 2; flea++) {
            for (unsigned n = 0; n < sizeof(offsets) / sizeof(offsets[0]); n++) {
                struct operation op = {
                    .kind = link ? RAW_LINK : RAW_DEVICE,
                    .start = offsets[n], .words = {UINT32_C(0x87650001)}
                };
                struct crystalhd_cmd cmd = { .adp = &adapter, .hw_ctx = &hardware };
                crystalhd_ioctl_data data = { .udata.u.regAcc = {
                    .Offset = op.start, .Value = op.words[0] } };
                reset();
                endpoint.device = flea ? BC_PCI_DEVID_FLEA : BC_PCI_DEVID_LINK;
                hardware.pfnWriteDevRegister = raw_device;
                current = &op;
                BC_STATUS (*command)(struct crystalhd_cmd *, crystalhd_ioctl_data *) =
                    link ? bc_cproc_link_reg_wr : bc_cproc_reg_wr;
                assert(command(NULL, &data) == BC_STS_INV_ARG);
                assert(command(&cmd, NULL) == BC_STS_INV_ARG);
                cmd.hw_ctx = NULL;
                assert(command(&cmd, &data) == BC_STS_INV_ARG && !raw_calls);
                cmd.hw_ctx = &hardware;
                assert(command(&cmd, &data) == BC_STS_SUCCESS);
                assert(raw_calls == 1 && !violations && !op.locked && !op.irq_state &&
                       op.locks == flea && op.unlocks == flea && !mmio_count);
                current = NULL;
                cases++;
            }
        }
    }
}

int main(void)
{
    _Static_assert(WINDOW_BYTES == 65536U, "Flea RDB window is 64KiB, not 512KiB");
    assert(crystalhd_flea_dram_burst(0, 0) == 0);
    assert(crystalhd_flea_dram_burst(0, 65) == 64);
    assert(crystalhd_flea_dram_burst(WINDOW_BYTES - 4U, 65) == 1);
    sequential();
    rejected();
    disabled_irq();
    concurrent();
    raw_commands();
    printf("Flea DRAM: %u cases; deterministic four-way contention PASS\n", cases);
    return 0;
}
