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
#define __packed __attribute__((packed))
#define static_assert _Static_assert
#include "ioctl-compat.h"
#define READ_ONCE(value) (value)
#define dev_err(dev, ...) ((void)(dev))
#define dev_dbg(dev, ...) ((void)(dev))
#define ERESTARTSYS 512
#define CAP_SYS_RAWIO 17

struct device { int unused; };
struct pci_dev { struct device dev; unsigned int device; };
struct test_lock { unsigned readers, writers; };
struct crystalhd_cmd {
    uint32_t state;
    struct crystalhd_user user[BC_LINK_MAX_OPENS];
};
struct crystalhd_adp {
    struct pci_dev *pdev;
    bool present;
    struct crystalhd_cmd cmds;
    struct test_lock user_lock, tx_lock;
};
struct crystalhd_file { struct crystalhd_user *user; uint64_t generation; };
struct file { void *private_data; };
typedef BC_STATUS (*crystalhd_cmd_proc)(struct crystalhd_cmd *, crystalhd_ioctl_data *);
struct crystalhd_cmd_tbl {
    uint32_t cmd_id;
    const crystalhd_cmd_proc cmd_proc;
    uint32_t block_mon;
};

static struct pci_dev pci;
static struct crystalhd_adp adapter, *current_adapter;
static struct crystalhd_file binding;
static struct file file;
static struct test_lock chd_device_lock;
static uint64_t chd_device_generation;
static crystalhd_ioctl_data pooled;
static BC_IOCTL_DATA caller_data;
static unsigned checks, handler_calls, color_calls, allocations, frees, copies;
static unsigned device_locks, user_reads, user_writes, tx_locks, extra_frees;
static unsigned expected_cmd;
static unsigned long expected_address;
static bool expected_compat, privileged, pool_live, pool_empty;
static bool fail_user_admission, fail_tx_admission, interrupt_tx;
static bool release_user, attach_extra;
static int copy_in_error, copy_out_error;
static BC_STATUS handler_status;

static void Check(bool okay, const char *message)
{
    checks++;
    if (!okay) { fprintf(stderr, "FAIL: %s\n", message); exit(1); }
}
static struct device *chddev(void) { return &pci.dev; }
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
static void mutex_unlock(struct test_lock *lock)
{
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
    Check(adp == &adapter && io == &pooled && pool_live &&
          ua == expected_address && compat == expected_compat &&
          io->cmd == expected_cmd && io->u_id == 1,
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
    Check(ctx == &adapter.cmds && io == &pooled && io->cmd == command &&
          command == expected_cmd, "production command table selects the correct handler");
    Check(chd_device_lock.readers == 1, "handler retains device lifetime protection");
    if (command == BCM_IOC_RELEASE || command == BCM_IOC_NOTIFY_MODE)
        Check(adapter.user_lock.writers == 1, "session mutation has exclusive admission");
    else
        Check(adapter.user_lock.readers == 1, "ordinary ioctl has shared admission");
    Check(adapter.tx_lock.writers == (command == BCM_IOC_PROC_INPUT),
          "only input holds TX serialization; receive remains independent");
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
    Check(!pool_live && !chd_device_lock.readers && !adapter.user_lock.readers &&
          !adapter.user_lock.writers && !adapter.tx_lock.writers,
          "previous operation released all admission and temporary ownership");
    pci = (struct pci_dev){ .device = BC_PCI_DEVID_FLEA };
    adapter = (struct crystalhd_adp){ .pdev = &pci, .present = true };
    adapter.cmds.user[1] = (struct crystalhd_user){ .uid = 1, .in_use = 1, .mode = DTS_PLAYBACK_MODE };
    current_adapter = &adapter;
    chd_device_generation = 3;
    binding = (struct crystalhd_file){ .user = &adapter.cmds.user[1], .generation = 3 };
    file.private_data = &binding;
    caller_data = (BC_IOCTL_DATA){0};
    handler_calls = color_calls = allocations = frees = copies = extra_frees = 0;
    device_locks = user_reads = user_writes = tx_locks = 0;
    privileged = true;
    pool_empty = fail_user_admission = fail_tx_admission = interrupt_tx = false;
    release_user = attach_extra = false;
    copy_in_error = copy_out_error = 0;
    handler_status = BC_STS_SUCCESS;
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
static bool MonitorBlocked(unsigned number)
{
    return number == DRV_CMD_WR_PCI_CFG || number == DRV_CMD_FW_DOWNLOAD ||
        number == DRV_ISSUE_FW_CMD || number == DRV_CMD_PROC_INPUT ||
        number == DRV_CMD_ADD_RXBUFFS || number == DRV_CMD_FETCH_RXBUFF ||
        number == DRV_CMD_START_RX_CAP || number == DRV_CMD_FLUSH_RX_CAP;
}
static void Routing(void)
{
    for (unsigned compat = 0; compat < 2; compat++) {
        for (unsigned number = 0; number < DRV_CMD_END; number++) {
            unsigned command = _IOC(_IOC_READ | _IOC_WRITE, BC_IOC_BASE, number, sizeof(BC_IOCTL_DATA));
            Reset();
            Check(Call(command, compat) == 0 && handler_calls == 1 && copies == 2 && frees == 1,
                  "every native/compat command dispatches and returns its result");
            Reset();
            binding.user->mode = DTS_MONITOR_MODE;
            Check(Call(command, compat) == (MonitorBlocked(number) ? -ENOTTY : 0),
                  "monitor command whitelist remains compatible");
            Check(handler_calls == (MonitorBlocked(number) ? 0U : 1U), "blocked monitor command never reaches hardware");
            Reset();
            adapter.cmds.state = BC_LINK_SUSPEND;
            Check(Call(command, compat) == 0, "suspended legacy calls return status through the ioctl envelope");
            Check(caller_data.RetSts == (command == BCM_IOC_GET_DRV_STAT ? BC_STS_SUCCESS : BC_STS_PWR_MGMT),
                  "only statistics dispatches while suspended");
            Check(handler_calls == (command == BCM_IOC_GET_DRV_STAT ? 1U : 0U), "suspended commands avoid leaf handlers");
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
    const unsigned non_tx[] = { BCM_IOC_GET_VERSION, BCM_IOC_NOTIFY_MODE };
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
    Check(Call(BCM_IOC_RELEASE, false) == 0 && !binding.user && user_writes == 1,
          "successful release invalidates this file binding");
    Reset(); handler_status = BC_STS_IO_ERROR;
    Check(Call(BCM_IOC_RELEASE, false) == 0 && binding.user && caller_data.RetSts == BC_STS_IO_ERROR,
          "failed release preserves a still-owned binding");
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
int main(void)
{
    Routing();
    Admission();
    ResultsAndCleanup();
    Permissions();
    Reset();
    printf("Legacy ioctl dispatch passed (%u checks)\n", checks);
    return 0;
}
