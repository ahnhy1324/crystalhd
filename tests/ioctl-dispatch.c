/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Execute the production ioctl entry points, command table and API wrapper.
 * User copies and leaf handlers are stubs; no device is opened.
 */
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include "bc_dts_defs.h"
#include "7411d.h"
typedef struct C011_PIB C011_PIB;
#include "bc_dts_glob_lnx.h"
#include "crystalhd_ioctl_limits.h"
#include "ioctl-types.h"

typedef uint32_t u32;
typedef uint32_t compat_uptr_t;
typedef uint8_t u8;
typedef int32_t s32;
typedef unsigned int __poll_t;
typedef struct { int counter; } atomic_t;
typedef struct { int unused; } wait_queue_head_t;
typedef struct poll_table_struct { int unused; } poll_table;
#define __packed __attribute__((packed))
#define static_assert _Static_assert
#include "ioctl-compat.h"
#define READ_ONCE(value) (value)
#define WRITE_ONCE(value, next) ((value) = (next))
#define dev_err(dev, ...) ((void)(dev))
#define dev_dbg(dev, ...) ((void)(dev))
#define ERESTARTSYS 512
#define CAP_SYS_RAWIO 17
#define GFP_KERNEL 0

struct device { int unused; };
struct pci_dev { struct device dev; unsigned int device; };
struct test_lock { unsigned readers, writers; };
struct crystalhd_hw;
struct crystalhd_cmd {
    uint32_t state;
    struct crystalhd_user user[BC_LINK_MAX_OPENS];
    const void *session_owner;
    struct crystalhd_hw *hw_ctx;
};
struct crystalhd_adp {
    struct pci_dev *pdev;
    bool present;
    bool hw_accessible;
    uint64_t generation;
    struct crystalhd_cmd cmds;
    struct test_lock user_lock, tx_lock;
    atomic_t rx_poll_ready;
};
struct crystalhd_hw {
    struct crystalhd_adp *adp;
    unsigned lock;
    bool dma_fault, dev_started, FleaEnablePWM, FleaPowerTransition;
    bool hw_pause_issued;
    enum FLEA_POWER_STATES FleaPowerState;
    uint32_t FleaRawActive;
};
struct crystalhd_file { struct crystalhd_user *user; struct test_lock release_lock; uint32_t tx_stopping; uint64_t generation; };
struct file { void *private_data; };
typedef BC_STATUS (*crystalhd_cmd_proc)(struct crystalhd_cmd *, crystalhd_ioctl_data *);
struct crystalhd_cmd_tbl {
    uint32_t cmd_id;
    const crystalhd_cmd_proc cmd_proc;
    uint32_t requires_session_owner;
};

static struct pci_dev pci;
static struct crystalhd_adp adapter, *current_adapter;
static struct crystalhd_hw hardware;
static struct crystalhd_file binding;
static struct file file;
static struct test_lock chd_device_lock;
static uint64_t chd_device_generation;
static crystalhd_ioctl_data pooled, release_envelope;
static bool release_live, release_alloc_fail;
static BC_IOCTL_DATA caller_data;
static wait_queue_head_t chd_rx_poll_wait;
static unsigned checks, handler_calls, color_calls, allocations, frees, copies;
static unsigned device_locks, user_reads, user_writes, tx_locks, extra_frees;
static unsigned poll_wakes;
static unsigned poll_registrations;
static unsigned expected_cmd;
static unsigned long expected_address;
static bool expected_compat, privileged, pool_live, pool_empty;
static bool fail_user_admission, fail_tx_admission, interrupt_tx;
static bool release_user, attach_extra;
static int copy_in_error, copy_out_error;
static BC_STATUS handler_status;
static bool expect_raw_lease, pause_during_raw, fault_during_raw;
static unsigned raw_rechecks, raw_initial_owners;

BC_STATUS crystalhd_flea_raw_enter(struct crystalhd_hw *, bool, bool *);
void crystalhd_flea_raw_leave(struct crystalhd_hw *, bool);
static bool crystalhd_rawio_command(unsigned int);

static void Check(bool okay, const char *message)
{
    checks++;
    if (!okay) { fprintf(stderr, "FAIL: %s\n", message); exit(1); }
}
#define spin_lock_irqsave(spin, flags) do { \
    Check((spin) == &hardware.lock && !*(spin), "raw lease lock is nonrecursive"); \
    *(spin) = 1; (flags) = 0; \
} while (0)
#define spin_unlock_irqrestore(spin, flags) do { \
    Check((spin) == &hardware.lock && *(spin) == 1 && !(flags), "raw lease lock balances"); \
    *(spin) = 0; \
} while (0)
static BC_STATUS crystalhd_flea_set_next_power_state(struct crystalhd_hw *hw,
                                                     enum FLEA_STATE_CH_EVENT event)
{
    Check(hw == &hardware && !hw->lock && event == FLEA_EVT_CMD_COMP &&
          hw->FleaRawActive == raw_initial_owners,
          "raw completion reevaluates power only after releasing its own lease");
    raw_rechecks++;
    return BC_STS_SUCCESS;
}
static struct device *chddev(void) { return &pci.dev; }
static void crystalhd_rx_poll_wake(struct crystalhd_adp *adp, unsigned events)
{
    Check(adp == &adapter && events == (EPOLLERR | EPOLLHUP),
          "release wakes output pollers with a terminal event");
    poll_wakes++;
}
static int atomic_read(const atomic_t *value) { return value->counter; }
static void poll_wait(struct file *fd, wait_queue_head_t *wait,
                      poll_table *table)
{
    Check(fd == &file && wait == &chd_rx_poll_wait && table,
          "poll registers on the module-lifetime waitqueue");
    Check(chd_device_lock.readers == 1,
          "poll registration retains the removal barrier");
    poll_registrations++;
}
static struct crystalhd_adp *chd_get_adp(void)
{
    Check(chd_device_lock.readers == 1, "device lookup holds the removal barrier");
    return current_adapter;
}
static void down_read(struct test_lock *lock)
{
    Check(!lock->readers && !lock->writers, "read lock starts balanced");
    lock->readers++;
    if (lock == &chd_device_lock) { device_locks++; return; }
    Check(lock == &adapter.user_lock && chd_device_lock.readers == 1,
          "user admission nests inside the device barrier");
    user_reads++;
    if (fail_user_admission) adapter.present = false;
}
static void up_read(struct test_lock *lock)
{
    Check(lock->readers == 1 && !lock->writers, "read lock balances on every exit");
    lock->readers--;
}
static void down_write(struct test_lock *lock)
{
    Check(lock == &adapter.user_lock && chd_device_lock.readers == 1 &&
          !lock->readers && !lock->writers, "exclusive ioctl holds the user barrier");
    lock->writers++;
    user_writes++;
    if (fail_user_admission) adapter.present = false;
}
static void up_write(struct test_lock *lock)
{
    Check(lock == &adapter.user_lock && lock->writers == 1 && !lock->readers,
          "exclusive user admission is released");
    lock->writers--;
}
static int mutex_lock_interruptible(struct test_lock *lock)
{
    Check(lock == &adapter.tx_lock && adapter.user_lock.readers == 1 &&
          !lock->writers, "TX serialization nests inside user admission");
    tx_locks++;
    if (interrupt_tx) return -EINTR;
    lock->writers++;
    if (fail_tx_admission) adapter.present = false;
    return 0;
}
static void mutex_lock(struct test_lock *lock)
{
    Check(lock == &binding.release_lock && !lock->writers &&
          !adapter.user_lock.readers && !adapter.user_lock.writers,
          "RELEASE intent serializes before user admission");
    lock->writers++;
}
static void mutex_unlock(struct test_lock *lock)
{
    if (lock == &binding.release_lock) {
        Check(lock->writers == 1 && !adapter.user_lock.readers &&
              !adapter.user_lock.writers && !binding.tx_stopping,
              "RELEASE clears its intent after dropping user admission");
        lock->writers--;
        return;
    }
    Check(lock == &adapter.tx_lock && lock->writers == 1 &&
          adapter.user_lock.readers == 1, "TX serialization ends before user admission");
    lock->writers--;
}
static bool capable(int cap)
{
    Check(cap == CAP_SYS_RAWIO, "only the raw-I/O capability is consulted");
    return privileged;
}
static void *compat_ptr(unsigned long address) { return (void *)(uintptr_t)(uint32_t)address; }
static void *kzalloc(size_t bytes, int flags)
{
    Check(bytes == sizeof(release_envelope) && flags == GFP_KERNEL && !release_live,
          "RELEASE obtains its private envelope independently of the shared pool");
    allocations++;
    if (release_alloc_fail) return NULL;
    memset(&release_envelope, 0, sizeof(release_envelope));
    release_live = true;
    return &release_envelope;
}
static void kfree(void *memory)
{
    Check(memory == &release_envelope && release_live && !release_envelope.add_cdata,
          "RELEASE retires its private envelope exactly once");
    release_live = false;
    frees++;
}
static bool LiveEnvelope(crystalhd_ioctl_data *io)
{
    return (io == &pooled && pool_live) || (io == &release_envelope && release_live);
}
static crystalhd_ioctl_data *chd_dec_alloc_iodata(struct crystalhd_adp *adp, bool isr)
{
    Check(adp == &adapter && !isr && !pool_live, "allocate the sole ioctl object once");
    allocations++;
    if (pool_empty) return NULL;
    memset(&pooled, 0, sizeof(pooled));
    pool_live = true;
    return &pooled;
}
static void chd_dec_free_iodata(struct crystalhd_adp *adp, crystalhd_ioctl_data *io, bool isr)
{
    Check(adp == &adapter && io == &pooled && !isr && pool_live && !io->add_cdata,
          "return the ioctl object with no attached allocation");
    pool_live = false;
    frees++;
}
static int chd_dec_proc_user_data(struct crystalhd_adp *adp, crystalhd_ioctl_data *io,
                                unsigned long ua, bool set, bool compat)
{
    Check(adp == &adapter && LiveEnvelope(io) &&
          ua == expected_address && compat == expected_compat &&
          io->cmd == expected_cmd &&
          (io->u_id == 1 || (!set && io->cmd == BCM_IOC_RELEASE)),
          "copy boundary receives native command, caller identity and pointer mode");
    copies++;
    if (!set) {
        io->udata = caller_data;
        if (attach_extra) {
            io->add_cdata = malloc(8);
            Check(io->add_cdata != NULL, "test allocation succeeds");
        }
        return copy_in_error;
    }
    caller_data = io->udata;
    return copy_out_error;
}
static void vfree(void *memory) { Check(memory != NULL, "attached allocation is live"); free(memory); extra_frees++; }
static BC_STATUS Handle(struct crystalhd_cmd *ctx, crystalhd_ioctl_data *io, unsigned command)
{
    Check(ctx == &adapter.cmds && LiveEnvelope(io) && io->cmd == command &&
          command == expected_cmd, "production command table selects the correct handler");
    Check(chd_device_lock.readers == 1, "handler retains device lifetime protection");
    if (command == BCM_IOC_RELEASE || command == BCM_IOC_NOTIFY_MODE ||
        command == BCM_IOC_FW_DOWNLOAD)
        Check(adapter.user_lock.writers == 1, "session mutation has exclusive admission");
    else
        Check(adapter.user_lock.readers == 1, "ordinary ioctl has shared admission");
    Check(adapter.tx_lock.writers == (command == BCM_IOC_PROC_INPUT),
          "only input holds TX serialization; receive remains independent");
    if (expect_raw_lease)
        Check(!hardware.lock && hardware.FleaRawActive == raw_initial_owners + 1,
              "raw handler holds its exact active-device lease through the last access");
    if (pause_during_raw) {
        hardware.FleaPowerState = FLEA_PS_LP_PENDING;
        hardware.hw_pause_issued = true;
    }
    if (fault_during_raw) hardware.dma_fault = true;
    handler_calls++;
    if (command == BCM_IOC_RELEASE && release_user) ctx->user[io->u_id].in_use = 0;
    return handler_status;
}
#define HANDLER(name, command) \
    static BC_STATUS name(struct crystalhd_cmd *ctx, crystalhd_ioctl_data *io) \
    { return Handle(ctx, io, command); }
HANDLER(bc_cproc_get_version, BCM_IOC_GET_VERSION)
HANDLER(bc_cproc_get_hwtype, BCM_IOC_GET_HWTYPE)
HANDLER(bc_cproc_reg_rd, BCM_IOC_REG_RD)
HANDLER(bc_cproc_reg_wr, BCM_IOC_REG_WR)
HANDLER(bc_cproc_link_reg_rd, BCM_IOC_FPGA_RD)
HANDLER(bc_cproc_link_reg_wr, BCM_IOC_FPGA_WR)
HANDLER(bc_cproc_mem_rd, BCM_IOC_MEM_RD)
HANDLER(bc_cproc_mem_wr, BCM_IOC_MEM_WR)
HANDLER(bc_cproc_cfg_rd, BCM_IOC_RD_PCI_CFG)
HANDLER(bc_cproc_cfg_wr, BCM_IOC_WR_PCI_CFG)
HANDLER(bc_cproc_download_fw, BCM_IOC_FW_DOWNLOAD)
HANDLER(bc_cproc_do_fw_cmd, BCM_IOC_FW_CMD)
HANDLER(bc_cproc_proc_input, BCM_IOC_PROC_INPUT)
HANDLER(bc_cproc_add_cap_buff, BCM_IOC_ADD_RXBUFFS)
HANDLER(bc_cproc_fetch_frame, BCM_IOC_FETCH_RXBUFF)
HANDLER(bc_cproc_start_capture, BCM_IOC_START_RX_CAP)
HANDLER(bc_cproc_flush_cap_buffs, BCM_IOC_FLUSH_RX_CAP)
HANDLER(bc_cproc_get_stats, BCM_IOC_GET_DRV_STAT)
HANDLER(bc_cproc_reset_stats, BCM_IOC_RST_DRV_STAT)
HANDLER(bc_cproc_notify_mode, BCM_IOC_NOTIFY_MODE)
HANDLER(bc_cproc_release_user, BCM_IOC_RELEASE)
HANDLER(bc_cproc_query_caps, BCM_IOC_QUERY_CAPS)
static BC_STATUS crystalhd_legacy_color_access(struct crystalhd_cmd *ctx, crystalhd_ioctl_data *io)
{
    Check(io->udata.u.regAcc.Offset == CRYSTALHD_FLEA_COLOR_REGISTER,
          "unprivileged color access reaches only its restricted handler");
    color_calls++;
    return Handle(ctx, io, io->cmd);
}
#include "ioctl-table.h"
#include "ioctl-functions.h"

static void Reset(void)
{
    Check(!pool_live && !release_live && !chd_device_lock.readers && !adapter.user_lock.readers &&
          !adapter.user_lock.writers && !adapter.tx_lock.writers,
          "previous operation released all admission and temporary ownership");
    pci = (struct pci_dev){ .device = BC_PCI_DEVID_FLEA };
    adapter = (struct crystalhd_adp){
        .pdev = &pci, .present = true, .hw_accessible = true,
        .generation = 3,
    };
    adapter.cmds.user[1] = (struct crystalhd_user){ .uid = 1, .in_use = 1, .mode = DTS_PLAYBACK_MODE };
    adapter.cmds.session_owner = &adapter.cmds.user[1];
    hardware = (struct crystalhd_hw){ .adp = &adapter, .dev_started = true,
        .FleaPowerState = FLEA_PS_ACTIVE };
    adapter.cmds.hw_ctx = &hardware;
    current_adapter = &adapter;
    chd_device_generation = 3;
    binding = (struct crystalhd_file){ .user = &adapter.cmds.user[1], .generation = 3 };
    file.private_data = &binding;
    caller_data = (BC_IOCTL_DATA){0};
    handler_calls = color_calls = allocations = frees = copies = extra_frees = 0;
    poll_wakes = 0;
    poll_registrations = 0;
    device_locks = user_reads = user_writes = tx_locks = 0;
    privileged = true;
    release_alloc_fail = false;
    pool_empty = fail_user_admission = fail_tx_admission = interrupt_tx = false;
    release_user = attach_extra = false;
    copy_in_error = copy_out_error = 0;
    handler_status = BC_STS_SUCCESS;
    expect_raw_lease = pause_during_raw = fault_during_raw = false;
    raw_rechecks = raw_initial_owners = 0;
}
static long Call(unsigned command, bool compat)
{
    expected_cmd = command;
    expected_compat = compat;
    expected_address = compat ? 0xf1234567UL : (unsigned long)&caller_data;
    if (compat) {
        unsigned compat_command = _IOC(_IOC_READ | _IOC_WRITE, BC_IOC_BASE,
                                        _IOC_NR(command), sizeof(struct crystalhd_ioctl_data32));
        return chd_dec_compat_ioctl(&file, compat_command, expected_address);
    }
    return chd_dec_ioctl(&file, command, expected_address);
}
static bool SessionRequired(unsigned number)
{
    return number == DRV_CMD_WR_PCI_CFG || number == DRV_CMD_FW_DOWNLOAD ||
        number == DRV_ISSUE_FW_CMD || number == DRV_CMD_PROC_INPUT ||
        number == DRV_CMD_ADD_RXBUFFS || number == DRV_CMD_FETCH_RXBUFF ||
        number == DRV_CMD_START_RX_CAP || number == DRV_CMD_FLUSH_RX_CAP;
}
static void PollReadiness(void)
{
    poll_table table = {0};

    Reset();
    Check(chd_dec_poll(&file, &table) == 0 && poll_registrations == 1,
          "an empty active owner poll blocks after registering its waitqueue");
    adapter.rx_poll_ready.counter = 2;
    Check(chd_dec_poll(&file, &table) == (EPOLLIN | EPOLLRDNORM),
          "a persistent ready count is level-readable for the session owner");
    adapter.cmds.session_owner = &adapter.cmds.user[0];
    Check(chd_dec_poll(&file, &table) == 0,
          "a monitor or foreign handle cannot consume owner output");
    adapter.cmds.session_owner = binding.user;
    adapter.cmds.state = BC_LINK_SUSPEND;
    Check(chd_dec_poll(&file, &table) == EPOLLERR,
          "suspend wakes a live binding as temporarily unavailable");
    adapter.cmds.state = 0;
    adapter.hw_accessible = false;
    Check(chd_dec_poll(&file, &table) == EPOLLERR,
          "inaccessible hardware reports an output poll error");
    adapter.hw_accessible = true;
    binding.user->in_use = 0;
    Check(chd_dec_poll(&file, &table) == (EPOLLERR | EPOLLHUP),
          "a released user is terminal for its poll binding");

    Reset();
    binding.generation--;
    Check(chd_dec_poll(&file, &table) == (EPOLLERR | EPOLLHUP) &&
          !poll_registrations,
          "a stale generation is rejected before waitqueue registration");
    Reset();
    current_adapter = NULL;
    Check(chd_dec_poll(&file, &table) == (EPOLLERR | EPOLLHUP) &&
          !poll_registrations,
          "an absent adapter is terminal without touching a dead waitqueue");
}
static void Routing(void)
{
    const uint32_t modes[] = { DTS_PLAYBACK_MODE, DTS_DIAG_MODE,
        DTS_HWINIT_MODE, DTS_MONITOR_MODE };
    const uint32_t flags[] = { 0, 0x100, 0x81000000 };

    for (unsigned compat = 0; compat < 2; compat++) {
        for (unsigned number = 0; number < DRV_CMD_END; number++) {
            unsigned command = _IOC(_IOC_READ | _IOC_WRITE, BC_IOC_BASE, number, sizeof(BC_IOCTL_DATA));
            for (unsigned m = 0; m < sizeof(modes) / sizeof(modes[0]); m++) {
                for (unsigned f = 0; f < sizeof(flags) / sizeof(flags[0]); f++) {
                    bool blocked = modes[m] == DTS_MONITOR_MODE && SessionRequired(number);
                    unsigned calls = blocked ? 0 : 1;

                    Reset();
                    binding.user->mode = modes[m] | flags[f];
                    if (modes[m] == DTS_MONITOR_MODE)
                        adapter.cmds.session_owner = NULL;
                    Check(Call(command, compat) == (blocked ? -ENOTTY : 0),
                          "native/compat monitor whitelist depends on mode, not high flags");
                    Check(handler_calls == calls && allocations == calls &&
                          copies == 2 * calls && frees == calls,
                          "blocked monitor commands reach neither leaf handlers nor user copies");
                    Check(binding.user->mode == (modes[m] | flags[f]),
                          "dispatch preserves the complete mode value");
                }
            }
            Reset();
            adapter.cmds.state = BC_LINK_SUSPEND;
            Check(Call(command, compat) == 0, "suspended legacy calls return status through the ioctl envelope");
            bool allowed = command == BCM_IOC_GET_DRV_STAT ||
                command == BCM_IOC_QUERY_CAPS;
            Check(caller_data.RetSts == (allowed ? BC_STS_SUCCESS : BC_STS_PWR_MGMT),
                  "only statistics and capability discovery dispatch while suspended");
            Check(handler_calls == (allowed ? 1U : 0U),
                  "suspended commands avoid hardware-facing leaf handlers");
        }
    }
}
static void SessionOwnership(void)
{
    for (unsigned compat = 0; compat < 2; compat++) {
        for (unsigned number = 0; number < DRV_CMD_END; number++) {
            unsigned command;

            if (!SessionRequired(number))
                continue;
            command = _IOC(_IOC_READ | _IOC_WRITE, BC_IOC_BASE, number,
                           sizeof(BC_IOCTL_DATA));
            for (unsigned foreign = 0; foreign < 2; foreign++) {
                Reset();
                binding.user->mode = DTS_MODE_INV;
                adapter.cmds.session_owner = foreign ?
                    &adapter.cmds.user[0] : NULL;
                Check(Call(command, compat) == 0 &&
                      caller_data.RetSts == BC_STS_ERR_USAGE,
                      "nonowner session commands return usage status in the ioctl envelope");
                Check(!handler_calls && allocations == 1 && copies == 2 && frees == 1,
                      "nonowner session commands reach no stateful leaf handler");
                Check(user_writes == (number == DRV_CMD_FW_DOWNLOAD) &&
                      user_reads == (number != DRV_CMD_FW_DOWNLOAD),
                      "firmware reset is exclusive even when ownership rejects it");
            }
        }
    }
}
static void Admission(void)
{
    const unsigned malformed[] = {
        _IOC(_IOC_READ | _IOC_WRITE, BC_IOC_BASE + 1, 0, sizeof(BC_IOCTL_DATA)),
        _IOC(_IOC_READ, BC_IOC_BASE, 0, sizeof(BC_IOCTL_DATA)),
        _IOC(_IOC_READ | _IOC_WRITE, BC_IOC_BASE, 0, sizeof(BC_IOCTL_DATA) - 1),
        _IOC(_IOC_READ | _IOC_WRITE, BC_IOC_BASE, DRV_CMD_END, sizeof(BC_IOCTL_DATA))
    };
    for (unsigned i = 0; i < sizeof(malformed) / sizeof(malformed[0]); i++) {
        Reset();
        adapter.present = false;
        adapter.cmds.state = BC_LINK_SUSPEND;
        Check(chd_dec_ioctl(&file, malformed[i], 0) == -ENOTTY && !user_reads && !allocations,
              "native malformed encoding wins over suspend/removal and never copies data");
        unsigned bad_compat = _IOC(_IOC_DIR(malformed[i]), _IOC_TYPE(malformed[i]),
            _IOC_NR(malformed[i]), sizeof(struct crystalhd_ioctl_data32) - (i == 2));
        device_locks = 0;
        Check(chd_dec_compat_ioctl(&file, bad_compat, 0) == -ENOTTY && !device_locks,
              "compat malformed encoding is rejected before admission");
    }
    for (unsigned which = 0; which < 6; which++) {
        Reset();
        switch (which) {
        case 0: current_adapter = NULL; break;
        case 1: adapter.present = false; break;
        case 2: file.private_data = NULL; break;
        case 3: binding.generation--; binding.user = (void *)(uintptr_t)1; break;
        case 4: fail_user_admission = true; break;
        case 5: fail_tx_admission = true; break;
        }
        Check(Call(BCM_IOC_PROC_INPUT, false) == -ENODEV && !allocations && !handler_calls,
              "absent, stale and failed admission paths avoid the ioctl pool and hardware");
    }
    const unsigned non_tx[] = {
        BCM_IOC_GET_VERSION, BCM_IOC_NOTIFY_MODE, BCM_IOC_FW_DOWNLOAD
    };
    for (unsigned i = 0; i < sizeof(non_tx) / sizeof(non_tx[0]); i++) {
        Reset(); fail_user_admission = true;
        Check(Call(non_tx[i], false) == -ENODEV && !allocations && !handler_calls && !tx_locks,
              "ordinary and exclusive ioctls revalidate failed PM after the user barrier");
    }
    Reset(); binding.user = NULL;
    Check(Call(BCM_IOC_GET_VERSION, false) == -ENODATA && !allocations, "released binding cannot dispatch");
    Reset(); interrupt_tx = true;
    Check(Call(BCM_IOC_PROC_INPUT, false) == -ERESTARTSYS && !allocations && tx_locks == 1,
          "interrupted TX lock does not allocate or unlock an unowned mutex");
    Reset(); release_user = true;
    Check(Call(BCM_IOC_RELEASE, false) == 0 && !binding.user && user_writes == 1 &&
          poll_wakes == 1,
          "successful release invalidates this file binding and wakes pollers");
    Reset(); handler_status = BC_STS_IO_ERROR;
    Check(Call(BCM_IOC_RELEASE, false) == 0 && binding.user && caller_data.RetSts == BC_STS_IO_ERROR,
          "failed release preserves a still-owned binding");
}
static void ReleaseIntent(void)
{
    for (unsigned compat = 0; compat < 2; compat++) {
        Reset();
        pool_empty = true;
        release_user = true;
        Check(Call(BCM_IOC_RELEASE, compat) == 0 && !binding.user &&
              !binding.tx_stopping && !binding.release_lock.writers && frees == 1,
              "RELEASE can retire its file even when the shared ioctl pool is exhausted");
        Reset();
        release_alloc_fail = true;
        Check(Call(BCM_IOC_RELEASE, compat) == -ENOMEM && !binding.tx_stopping &&
              !user_writes && !handler_calls && !frees && !release_live,
              "failed private allocation leaves input and admission unchanged");
        Reset();
        copy_in_error = -EFAULT;
        Check(Call(BCM_IOC_RELEASE, compat) == -EFAULT &&
              !binding.tx_stopping && !binding.release_lock.writers &&
              !user_writes && !handler_calls && frees == 1,
              "an invalid RELEASE envelope never publishes stopping or waits for input");
        Reset();
        fail_user_admission = true;
        Check(Call(BCM_IOC_RELEASE, compat) == -ENODEV &&
              !binding.tx_stopping && !binding.release_lock.writers && frees == 1,
              "failed RELEASE admission returns its prepared envelope and stop intent");
        Reset();
        adapter.cmds.state = BC_LINK_SUSPEND;
        Check(Call(BCM_IOC_RELEASE, compat) == 0 &&
              caller_data.RetSts == BC_STS_PWR_MGMT && binding.user &&
              !binding.tx_stopping && !binding.release_lock.writers && frees == 1,
              "PM-deferred RELEASE preserves the handle and permits later input");
    }
}
static void ResultsAndCleanup(void)
{
    const BC_STATUS statuses[] = { BC_STS_SUCCESS, BC_STS_BUSY, BC_STS_IO_ERROR,
                                  BC_STS_IO_USER_ABORT, BC_STS_PENDING };
    for (unsigned i = 0; i < sizeof(statuses) / sizeof(statuses[0]); i++) {
        Reset(); handler_status = statuses[i];
        Check(Call(BCM_IOC_FW_CMD, false) == 0 &&
              caller_data.RetSts == (statuses[i] == BC_STS_PENDING ? BC_STS_NOT_IMPL : statuses[i]),
              "BC status stays in the envelope; pending maps to not-implemented");
    }
    Reset(); pool_empty = true;
    Check(Call(BCM_IOC_GET_VERSION, false) == -EINVAL && !copies && !frees, "pool exhaustion avoids copies and handlers");
    for (unsigned output = 0; output < 2; output++) {
        Reset(); attach_extra = true;
        if (output) copy_out_error = -EFAULT; else copy_in_error = -EFAULT;
        Check(Call(BCM_IOC_FW_CMD, true) == -EFAULT && frees == 1 && extra_frees == 1,
              "both copy failures release the envelope and attached allocation exactly once");
        Check(handler_calls == output, "input-copy failure prevents handler entry");
    }
}
static void Permissions(void)
{
    const unsigned raw[] = { BCM_IOC_REG_RD, BCM_IOC_REG_WR, BCM_IOC_FPGA_RD,
        BCM_IOC_FPGA_WR, BCM_IOC_MEM_RD, BCM_IOC_MEM_WR, BCM_IOC_WR_PCI_CFG };
    for (unsigned i = 0; i < sizeof(raw) / sizeof(raw[0]); i++) {
        Reset(); privileged = false; pci.device = 0;
        Check(Call(raw[i], false) == -EPERM && !allocations, "unprivileged raw I/O fails before user copies");
    }
    Reset(); privileged = false;
    caller_data.u.pciCfg.Size = sizeof(u32);
    Check(Call(BCM_IOC_RD_PCI_CFG, false) == 0 && handler_calls == 1, "legacy PCI identity discovery remains available");
    for (unsigned which = 0; which < 2; which++) {
        Reset(); privileged = false;
        caller_data.u.pciCfg.Size = which ? 8 : sizeof(u32);
        caller_data.u.pciCfg.Offset = which ? 0 : 4;
        Check(Call(BCM_IOC_RD_PCI_CFG, false) == -EPERM && !handler_calls && frees == 1,
              "other unprivileged PCI reads release their envelope without hardware access");
    }
    for (unsigned write = 0; write < 2; write++) {
        unsigned command = write ? BCM_IOC_REG_WR : BCM_IOC_REG_RD;
        Reset(); privileged = false;
        caller_data.u.regAcc.Offset = CRYSTALHD_FLEA_COLOR_REGISTER;
        Check(Call(command, false) == 0 && color_calls == 1, "legacy color access uses the restricted handler");
        Reset(); privileged = false;
        caller_data.u.regAcc.Offset = CRYSTALHD_FLEA_COLOR_REGISTER + 4;
        Check(Call(command, false) == -EPERM && !color_calls && !handler_calls && frees == 1,
              "neighboring registers are never exposed by the color exception");
    }
}
static void RawPowerAdmission(void)
{
    const unsigned commands[] = { BCM_IOC_REG_RD, BCM_IOC_FPGA_RD,
        BCM_IOC_MEM_RD, BCM_IOC_REG_WR, BCM_IOC_FPGA_WR,
        BCM_IOC_MEM_WR, BCM_IOC_WR_PCI_CFG };
    const BC_STATUS results[] = { BC_STS_SUCCESS, BC_STS_IO_ERROR, BC_STS_TIMEOUT };

    for (unsigned compat = 0; compat < 2; compat++) {
        for (unsigned op = 0; op < sizeof(commands) / sizeof(commands[0]); op++) {
            for (unsigned result = 0; result < sizeof(results) / sizeof(results[0]); result++) {
                Reset();
                hardware.FleaEnablePWM = true;
                hardware.FleaRawActive = raw_initial_owners = 2;
                expect_raw_lease = op < 3;
                handler_status = results[result];
                Check(Call(commands[op], compat) == 0 &&
                      caller_data.RetSts == (op < 3 ? results[result] : BC_STS_BUSY),
                      "enabled raw reads retain callback status while arbitrary writes are refused");
                Check(handler_calls == (op < 3) && hardware.FleaRawActive == 2 &&
                      !hardware.lock && !raw_rechecks && frees == 1,
                      "success and callback failure release only this call's raw lease");
            }
            for (unsigned state = FLEA_PS_NONE; state <= FLEA_PS_FAULT; state++) {
                Reset();
                hardware.FleaEnablePWM = true;
                hardware.FleaPowerState = state;
                expect_raw_lease = op < 3 && state == FLEA_PS_ACTIVE;
                BC_STATUS expected = state == FLEA_PS_FAULT ? BC_STS_IO_ERROR :
                    expect_raw_lease ? BC_STS_SUCCESS : BC_STS_BUSY;
                Check(Call(commands[op], compat) == 0 && caller_data.RetSts == expected &&
                      handler_calls == expect_raw_lease && !hardware.FleaRawActive,
                      "raw entry cannot access stopped, pending, asleep or faulted hardware");
            }
            for (unsigned fault = 0; fault < 4; fault++) {
                Reset();
                hardware.FleaEnablePWM = true;
                if (fault == 0) hardware.dma_fault = true;
                if (fault == 1) hardware.dev_started = false;
                if (fault == 2) hardware.FleaPowerTransition = true;
                if (fault == 3) hardware.FleaRawActive = UINT32_MAX;
                Check(Call(commands[op], compat) == 0 &&
                      caller_data.RetSts == (fault < 2 ? BC_STS_IO_ERROR : BC_STS_BUSY) &&
                      !handler_calls && hardware.FleaRawActive == (fault == 3 ? UINT32_MAX : 0),
                      "raw health, transition and overflow refusals never call a handler or leak a lease");
            }
        }
        for (unsigned write = 0; write < 2; write++) {
            Reset();
            privileged = false;
            hardware.FleaEnablePWM = true;
            expect_raw_lease = !write;
            caller_data.u.regAcc.Offset = CRYSTALHD_FLEA_COLOR_REGISTER;
            Check(Call(write ? BCM_IOC_REG_WR : BCM_IOC_REG_RD, compat) == 0 &&
                  caller_data.RetSts == (write ? BC_STS_BUSY : BC_STS_SUCCESS) &&
                  color_calls == !write && !hardware.FleaRawActive,
                  "legacy color handler replacement obeys the same raw power admission");
        }
        for (unsigned fault = 0; fault < 2; fault++) {
            Reset();
            hardware.FleaEnablePWM = expect_raw_lease = pause_during_raw = true;
            fault_during_raw = fault;
            Check(Call(BCM_IOC_REG_RD, compat) == 0 && handler_calls == 1 &&
                  !hardware.FleaRawActive && raw_rechecks == !fault,
                  "late pause is reevaluated after raw release but a fault is not resumed");
        }
        for (unsigned copy = 0; copy < 2; copy++) {
            Reset();
            hardware.FleaEnablePWM = true;
            expect_raw_lease = copy;
            if (copy) copy_out_error = -EFAULT; else copy_in_error = -EFAULT;
            Check(Call(BCM_IOC_MEM_RD, compat) == -EFAULT && handler_calls == copy &&
                  !hardware.FleaRawActive && frees == 1,
                  "user copy failures do not leak an active-device raw lease");
        }
        Reset();
        hardware.FleaEnablePWM = true;
        Check(Call(BCM_IOC_FW_DOWNLOAD, compat) == 0 && handler_calls == 1 &&
              !hardware.FleaRawActive && caller_data.RetSts == BC_STS_SUCCESS,
              "kernel-owned firmware download is not misclassified as external raw writes");
        Reset();
        pci.device = BC_PCI_DEVID_LINK;
        hardware.FleaEnablePWM = true;
        Check(Call(BCM_IOC_REG_WR, compat) == 0 && handler_calls == 1 &&
              !hardware.FleaRawActive && caller_data.RetSts == BC_STS_SUCCESS,
              "LINK legacy raw behavior is not gated by Flea runtime-power policy");
    }
}

int main(void)
{
    PollReadiness();
    Routing();
    SessionOwnership();
    Admission();
    ReleaseIntent();
    ResultsAndCleanup();
    Permissions();
    RawPowerAdmission();
    Reset();
    printf("Legacy ioctl dispatch passed (%u checks)\n", checks);
    return 0;
}
