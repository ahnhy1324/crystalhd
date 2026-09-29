/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Exact command/hardware PM and notify-mode functions; no device is opened.
 * Isolate failures so the original idle error and NULL dereferences are safe.
 */
#include <stdbool.h>
#include <stdint.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
#include "bc_dts_types.h"
struct _BC_DTS_PROC_OUT;
#include "bc_dts_defs.h"
#include "command-pm-types.h"

#define KERN_ERR ""
#define GFP_KERNEL 0
#define READ_ONCE(value) (value)
#define printk(...) ((void)0)
#define dev_err(dev, ...) ((void)(dev))
#define dev_info(dev, ...) do { (void)(dev); if (false) fprintf(stderr, __VA_ARGS__); } while (0)
#define dev_dbg(dev, ...) ((void)(dev))
#define eCMD_C011_CMD_BASE 0x73763000U
#define eCMD_C011_DEC_CHAN_FLUSH (eCMD_C011_CMD_BASE + 0x104U)
#define eCMD_C011_DEC_CHAN_PAUSE (eCMD_C011_CMD_BASE + 0x11dU)
struct device { int unused; };
struct pci_dev { struct device dev; int irq; };
struct crystalhd_adp;
typedef struct {
    uint32_t cmd[64];
    uint32_t rsp[64];
    uint32_t flags, add_data;
} BC_FW_CMD;
struct crystalhd_hw {
    struct crystalhd_adp *adp;
    void *rx_freeq;
    bool dma_fault;
    enum list_sts rx_list_sts[2];
    enum LIST_STATUS TxList0Sts, TxList1Sts;
    uint32_t rx_list_post_index, tx_list_post_index;
    bool (*pfnStartDevice)(struct crystalhd_hw *);
    bool (*pfnStopDevice)(struct crystalhd_hw *);
    BC_STATUS (*pfnStopTxDMA)(struct crystalhd_hw *);
    BC_STATUS (*pfnFWDwnld)(struct crystalhd_hw *, uint8_t *, uint32_t);
    BC_STATUS (*pfnIssuePause)(struct crystalhd_hw *, bool);
    BC_STATUS (*pfnDoFirmwareCmd)(struct crystalhd_hw *, BC_FW_CMD *);
    uint32_t (*pfnReadDevRegister)(struct crystalhd_adp *, uint32_t);
    void (*pfnWriteDevRegister)(struct crystalhd_adp *, uint32_t, uint32_t);
    uint32_t (*pfnReadFPGARegister)(struct crystalhd_adp *, uint32_t);
    void (*pfnWriteFPGARegister)(struct crystalhd_adp *, uint32_t, uint32_t);
    BC_STATUS (*pfnDevDRAMRead)(struct crystalhd_hw *, uint32_t, uint32_t, uint32_t *);
    BC_STATUS (*pfnDevDRAMWrite)(struct crystalhd_hw *, uint32_t, uint32_t, uint32_t *);
    int fwcmd_trans_mutex;
    bool fwcmd_pending, fwcmd_poisoned;
    int fetch_sem;
    uint32_t FwCmdCnt;
};
struct crystalhd_user { uint32_t uid, in_use, mode; };
struct crystalhd_cmd {
    uint32_t state;
    struct crystalhd_adp *adp;
    struct crystalhd_user user[BC_LINK_MAX_OPENS];
    struct crystalhd_user *session_owner;
    uint32_t tx_list_id, cin_wait_exit, pwr_state_change;
    struct crystalhd_hw *hw_ctx;
};
struct crystalhd_adp {
    struct pci_dev *pdev;
    unsigned cfg_users;
    bool present;
    void *fill_byte_pool, *elem_pool_head;
    int user_lock;
    struct crystalhd_cmd cmds;
};
struct crystalhd_file { struct crystalhd_user *user; uint64_t generation; };
struct file { void *private_data; };
struct inode { int unused; };
typedef struct {
    uint32_t u_id;
    struct { union {
        struct { uint32_t Mode; } NotifyMode;
        struct { uint32_t Offset, Value; } regAcc;
        struct { uint32_t StartOff, NumDwords; } devMem;
        BC_FW_CMD fwCmd;
    } u; } udata;
    void *add_cdata;
    uint32_t add_cdata_sz;
} crystalhd_ioctl_data;

static unsigned checks, failures, starts, stops, tx_stops, cancels, captures, pools, rings;
static unsigned elem_deletes, dio_destroys, ring_frees, hardware_opens, hardware_closes;
static unsigned irq_depth, irq_disables, irq_enables, capture_unmaps;
static unsigned hardware_allocations, hardware_frees, last_close_cfg_users;
static unsigned hardware_alloc_attempts;
static unsigned binding_count, binding_frees, device_reads, user_writes;
static unsigned downloads;
static unsigned raw_calls[6];
static uint32_t raw_offset, raw_value, raw_words, raw_memory[2];
static uint32_t *raw_buffer;
static BC_STATUS raw_status;
static bool start_ok, stop_ok;
static bool elem_live, dio_live, rings_live, hardware_allocated;
static bool hardware_alloc_fail, admission_pending, open_pending;
static unsigned admission_uid;
static uint32_t admission_state, admission_wait;
static int elem_error, dio_error;
static BC_STATUS ring_status, hardware_open_status;
static BC_STATUS capture_status, cancel_status;
static BC_STATUS pause_status, firmware_status;
static unsigned pause_calls, firmware_calls;
static bool pause_states[4];
static pthread_mutex_t transaction_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t transaction_audit = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t transaction_changed = PTHREAD_COND_INITIALIZER;
static bool transaction_mode, block_first_firmware;
static bool first_firmware_waiting, release_first_firmware;
static bool block_download_reset, download_reset_waiting, release_download_reset;
static unsigned transaction_attempts;
static char events[32];
static struct pci_dev endpoint = { .irq = 19 };
static struct crystalhd_adp adapter = { .pdev = &endpoint };
static struct crystalhd_hw hardware;
#define context adapter.cmds
static struct crystalhd_file bindings[BC_LINK_MAX_OPENS * 2];
static bool binding_live[BC_LINK_MAX_OPENS * 2], adapter_visible;
static uint64_t chd_device_generation;
static int chd_device_lock;
static bool Start(struct crystalhd_hw *hw);
static bool Stop(struct crystalhd_hw *hw);
static BC_STATUS StopTx(struct crystalhd_hw *hw);
static BC_STATUS Download(struct crystalhd_hw *hw, uint8_t *data, uint32_t size);
static BC_STATUS IssuePause(struct crystalhd_hw *hw, bool state);
static BC_STATUS FirmwareCommand(struct crystalhd_hw *hw, BC_FW_CMD *command);
static uint32_t ReadDevice(struct crystalhd_adp *adp, uint32_t offset);
static void WriteDevice(struct crystalhd_adp *adp, uint32_t offset, uint32_t value);
static uint32_t ReadLink(struct crystalhd_adp *adp, uint32_t offset);
static void WriteLink(struct crystalhd_adp *adp, uint32_t offset, uint32_t value);
static BC_STATUS ReadMemory(struct crystalhd_hw *hw, uint32_t offset, uint32_t words, uint32_t *buffer);
static BC_STATUS WriteMemory(struct crystalhd_hw *hw, uint32_t offset, uint32_t words, uint32_t *buffer);
static void Check(bool ok, const char *why)
{
    checks++;
    if (!ok) { failures++; fprintf(stderr, "FAIL: %s\n", why); }
}
static void Event(char event)
{
    size_t n = strlen(events);
    if (n + 1 >= sizeof(events)) abort();
    events[n] = event; events[n + 1] = '\0';
}
static void TransactionLock(int *lock)
{
    if (lock != &hardware.fwcmd_trans_mutex) abort();
    if (transaction_mode) {
        if (pthread_mutex_lock(&transaction_audit)) abort();
        transaction_attempts++;
        pthread_cond_broadcast(&transaction_changed);
        if (pthread_mutex_unlock(&transaction_audit)) abort();
    }
    if (pthread_mutex_lock(&transaction_mutex)) abort();
}
static void TransactionUnlock(int *lock)
{
    if (lock != &hardware.fwcmd_trans_mutex ||
        pthread_mutex_unlock(&transaction_mutex)) abort();
}
#define mutex_lock(lock) TransactionLock(lock)
#define mutex_unlock(lock) TransactionUnlock(lock)
static struct device *chddev(void) { return &endpoint.dev; }
static struct crystalhd_adp *chd_get_adp(void)
{
    Check(chd_device_lock == 1, "file close holds the device read lock");
    return adapter_visible ? &adapter : NULL;
}
static void down_read(int *lock)
{
    Check(lock == &chd_device_lock && !*lock,
          "file close acquires the device read lock once");
    *lock = 1;
    device_reads++;
}
static void up_read(int *lock)
{
    Check(lock == &chd_device_lock && *lock == 1 && !adapter.user_lock,
          "file close releases the device lock after the user lock");
    *lock = 0;
}
static void down_write(int *lock)
{
    Check(lock == &adapter.user_lock && !*lock && chd_device_lock == 1,
          "file close serializes users inside the device read lock");
    *lock = 1;
    user_writes++;
}
static void up_write(int *lock)
{
    Check(lock == &adapter.user_lock && *lock == 1,
          "file close releases the user write lock once");
    *lock = 0;
}
static void ConfigureHardware(struct crystalhd_hw *hw)
{
    *hw = (struct crystalhd_hw){ .adp = &adapter, .pfnStartDevice = Start,
        .pfnStopDevice = Stop, .pfnStopTxDMA = StopTx, .pfnFWDwnld = Download,
        .pfnIssuePause = IssuePause, .pfnDoFirmwareCmd = FirmwareCommand,
        .pfnReadDevRegister = ReadDevice, .pfnWriteDevRegister = WriteDevice,
        .pfnReadFPGARegister = ReadLink, .pfnWriteFPGARegister = WriteLink,
        .pfnDevDRAMRead = ReadMemory, .pfnDevDRAMWrite = WriteMemory,
        .fetch_sem = 1,
        .FwCmdCnt = 64,
        .rx_list_sts = {rx_sts_waiting, rx_sts_waiting},
        .TxList0Sts = TxListWaitingForIntr, .TxList1Sts = TxListWaitingForIntr,
        .rx_list_post_index = 1, .tx_list_post_index = 1 };
}
static void *kmalloc(size_t size, int flags)
{
    Check(size == sizeof(hardware) && flags == GFP_KERNEL && !hardware_allocated,
          "user open allocates one fresh hardware context");
    hardware_alloc_attempts++;
    if (open_pending)
        Check(!context.user[0].in_use && !adapter.cfg_users && !context.hw_ctx &&
              context.pwr_state_change == BC_HW_SUSPEND,
              "hardware allocation precedes user and power-state publication");
    if (hardware_alloc_fail)
        return NULL;
    hardware_allocated = true;
    hardware_allocations++;
    return &hardware;
}
static void kfree(void *memory)
{
    unsigned n;

    if (!memory)
        return;
    for (n = 0; n < binding_count; n++) {
        if (memory != &bindings[n])
            continue;
        Check(binding_live[n] && !chd_device_lock && !adapter.user_lock,
              "file close frees its binding once after unlocking");
        binding_live[n] = false;
        binding_frees++;
        return;
    }
    Check(memory == &hardware && hardware_allocated,
          "session teardown frees its hardware context exactly once");
    hardware_allocated = false;
    hardware_frees++;
}
static void disable_irq(int irq)
{
    Check(irq == endpoint.irq && !irq_depth, "session transition disables the device IRQ");
    irq_depth++; irq_disables++;
}
static void enable_irq(int irq)
{
    Check(irq == endpoint.irq && irq_depth == 1, "session transition reenables the device IRQ");
    irq_depth--; irq_enables++;
}
static BC_STATUS crystalhd_hw_open(struct crystalhd_hw *hw, struct crystalhd_adp *adp)
{
    Check(hw == &hardware && adp == &adapter,
          "user open initializes the allocated hardware context");
    if (open_pending)
        Check(!context.user[0].in_use && !adapter.cfg_users &&
              context.pwr_state_change == BC_HW_SUSPEND,
              "hardware initialization precedes user and power-state publication");
    hardware_opens++;
    if (hardware_open_status != BC_STS_SUCCESS)
        return hardware_open_status;
    ConfigureHardware(hw);
    return BC_STS_SUCCESS;
}
static BC_STATUS crystalhd_hw_close(struct crystalhd_hw *hw, struct crystalhd_adp *adp)
{
    Check(hw == &hardware && adp == &adapter,
          "session release closes the owned hardware context");
    hardware_closes++;
    last_close_cfg_users = adp->cfg_users;
    return BC_STS_SUCCESS;
}
static bool Start(struct crystalhd_hw *hw)
{
    Check(hw == &hardware, "start receives the owned hardware context");
    starts++; Event('R'); return start_ok;
}
static bool Stop(struct crystalhd_hw *hw)
{
    Check(hw == &hardware, "stop receives the owned hardware context");
    stops++; Event('S'); return stop_ok;
}
static BC_STATUS StopTx(struct crystalhd_hw *hw)
{
    Check(hw == &hardware, "TX stop receives the owned hardware context");
    tx_stops++; Event('T'); return BC_STS_SUCCESS;
}
static BC_STATUS Download(struct crystalhd_hw *hw, uint8_t *data, uint32_t size)
{
    Check(hw == &hardware && data && size == 1 && data[0] == 0x5a,
          "firmware admission reaches the expected hardware callback");
    downloads++; return BC_STS_SUCCESS;
}
static int down_interruptible(int *sem)
{
    Check(sem == &hardware.fetch_sem && *sem == 1, "pause transition acquires fetch serialization");
    *sem = 0;
    return 0;
}
static void down(int *sem)
{
    Check(!down_interruptible(sem), "rollback acquires fetch serialization");
}
static void up(int *sem)
{
    Check(sem == &hardware.fetch_sem && *sem == 0, "pause transition releases fetch serialization");
    *sem = 1;
}
static BC_STATUS IssuePause(struct crystalhd_hw *hw, bool state)
{
    Check(hw == &hardware && pause_calls < sizeof(pause_states) / sizeof(pause_states[0]),
          "pause callback receives the active hardware context");
    pause_states[pause_calls++] = state;
    return pause_status;
}
static BC_STATUS FirmwareCommand(struct crystalhd_hw *hw, BC_FW_CMD *command)
{
    unsigned ordinal;

    Check(hw == &hardware && command != NULL,
          "firmware callback receives the active command");
    if (block_first_firmware) {
        if (pthread_mutex_lock(&transaction_audit)) abort();
        ordinal = firmware_calls++;
        if (!ordinal) {
            first_firmware_waiting = true;
            pthread_cond_broadcast(&transaction_changed);
            while (!release_first_firmware)
                if (pthread_cond_wait(&transaction_changed,
                                      &transaction_audit)) abort();
        }
        if (pthread_mutex_unlock(&transaction_audit)) abort();
        return ordinal ? BC_STS_SUCCESS : BC_STS_TIMEOUT;
    }
    firmware_calls++;
    return firmware_status;
}
static BC_STATUS crystalhd_hw_stop_capture(struct crystalhd_hw *hw, bool unmap)
{
    Check(hw == &hardware, "capture stop receives the owned hardware context");
    captures++;
    if (unmap)
        capture_unmaps++;
    else
        Event('C');
    return capture_status;
}
static BC_STATUS crystalhd_hw_cancel_tx(struct crystalhd_hw *hw, uint32_t tag)
{
    Check(hw == &hardware && tag == context.tx_list_id && tag != 0,
          "suspend cancels the active TX owner");
    cancels++; Event('X'); return cancel_status;
}
static uint32_t RawRegister(struct crystalhd_adp *adp, unsigned operation,
                            uint32_t offset, uint32_t value)
{
    Check(adp == &adapter && operation < 4, "raw register callback receives the adapter");
    raw_calls[operation]++;
    raw_offset = offset;
    if (operation & 1)
        raw_value = value;
    return raw_value;
}
static uint32_t ReadDevice(struct crystalhd_adp *adp, uint32_t offset)
{ return RawRegister(adp, 0, offset, 0); }
static void WriteDevice(struct crystalhd_adp *adp, uint32_t offset, uint32_t value)
{ (void)RawRegister(adp, 1, offset, value); }
static uint32_t ReadLink(struct crystalhd_adp *adp, uint32_t offset)
{ return RawRegister(adp, 2, offset, 0); }
static void WriteLink(struct crystalhd_adp *adp, uint32_t offset, uint32_t value)
{ (void)RawRegister(adp, 3, offset, value); }
static BC_STATUS RawMemory(struct crystalhd_hw *hw, uint32_t offset, uint32_t words,
                           uint32_t *buffer, bool write)
{
    Check(hw == &hardware && buffer == raw_buffer && words <= 2,
          "raw memory callback receives the owned hardware and bounded buffer");
    raw_calls[write ? 5 : 4]++;
    raw_offset = offset;
    raw_words = words;
    if (raw_status == BC_STS_SUCCESS) {
        for (uint32_t n = 0; n < words; n++) {
            if (write) raw_memory[n] = buffer[n];
            else buffer[n] = raw_memory[n];
        }
    }
    return raw_status;
}
static BC_STATUS ReadMemory(struct crystalhd_hw *hw, uint32_t offset, uint32_t words, uint32_t *buffer)
{ return RawMemory(hw, offset, words, buffer, false); }
static BC_STATUS WriteMemory(struct crystalhd_hw *hw, uint32_t offset, uint32_t words, uint32_t *buffer)
{ return RawMemory(hw, offset, words, buffer, true); }
static void CheckPendingAdmission(void)
{
    if (admission_pending)
        Check(context.user[admission_uid].mode == (uint32_t)DTS_MODE_INV &&
              !context.session_owner && context.state == admission_state &&
              context.cin_wait_exit == admission_wait,
              "resource setup does not publish mode or owner or change command state early");
}
static int crystalhd_create_elem_pool(struct crystalhd_adp *adp, unsigned size)
{
    CheckPendingAdmission();
    Check(adp == &adapter && size == BC_LINK_ELEM_POOL_SZ, "notify allocates element pool");
    pools++; elem_live = true; adp->elem_pool_head = &elem_live;
    return elem_error;
}
static void crystalhd_delete_elem_pool(struct crystalhd_adp *adp)
{
    Check(adp == &adapter, "element-pool teardown receives the adapter");
    elem_deletes++; elem_live = false; adp->elem_pool_head = NULL;
}
static int crystalhd_create_dio_pool(struct crystalhd_adp *adp, unsigned size)
{
    CheckPendingAdmission();
    Check(adp == &adapter && size == BC_LINK_MAX_SGLS, "notify allocates DMA pool");
    pools++;
    if (dio_error) return dio_error;
    dio_live = true;
    adp->fill_byte_pool = &dio_live;
    return 0;
}
static void crystalhd_destroy_dio_pool(struct crystalhd_adp *adp)
{
    Check(adp == &adapter, "DMA-pool teardown receives the adapter");
    dio_destroys++; dio_live = false; adp->fill_byte_pool = NULL;
}
static BC_STATUS crystalhd_hw_setup_dma_rings(struct crystalhd_hw *hw)
{
    CheckPendingAdmission();
    rings++;
    if (!hw)
        return BC_STS_INV_ARG;
    Check(hw == &hardware, "notify uses the initialized hardware context");
    rings_live = ring_status == BC_STS_SUCCESS;
    hw->rx_freeq = rings_live ? &rings_live : NULL;
    return ring_status;
}
static BC_STATUS crystalhd_hw_free_dma_rings(struct crystalhd_hw *hw)
{
    Check(hw == &hardware, "DMA-ring teardown receives the hardware context");
    ring_frees++; rings_live = false; hw->rx_freeq = NULL;
    return BC_STS_SUCCESS;
}
static void crystalhd_hw_fw_cmd_reset_locked(struct crystalhd_hw *hw)
{
    Check(hw == &hardware, "hardware reset clears firmware-command accounting");
    hw->fwcmd_pending = hw->fwcmd_poisoned = false;
    hw->FwCmdCnt = 0;
    if (block_download_reset) {
        if (pthread_mutex_lock(&transaction_audit)) abort();
        download_reset_waiting = true;
        pthread_cond_broadcast(&transaction_changed);
        while (!release_download_reset)
            if (pthread_cond_wait(&transaction_changed,
                                  &transaction_audit)) abort();
        if (pthread_mutex_unlock(&transaction_audit)) abort();
    }
}
static BC_STATUS crystalhd_hw_fw_cmd_enter(struct crystalhd_hw *hw)
{
    if (hw != &hardware) return BC_STS_INV_ARG;
    TransactionLock(&hw->fwcmd_trans_mutex);
    if (hw->fwcmd_poisoned || hw->fwcmd_pending) {
        TransactionUnlock(&hw->fwcmd_trans_mutex);
        return BC_STS_BUSY;
    }
    return BC_STS_SUCCESS;
}
static BC_STATUS crystalhd_hw_fw_cmd_recovery_enter(struct crystalhd_hw *hw)
{
    if (hw != &hardware) return BC_STS_INV_ARG;
    TransactionLock(&hw->fwcmd_trans_mutex);
    if (hw->fwcmd_pending) {
        TransactionUnlock(&hw->fwcmd_trans_mutex);
        return BC_STS_BUSY;
    }
    return BC_STS_SUCCESS;
}
static void crystalhd_hw_fw_cmd_leave(struct crystalhd_hw *hw)
{
    if (hw != &hardware) abort();
    TransactionUnlock(&hw->fwcmd_trans_mutex);
}
#include "command-pm-hardware.h"
#include "command-pm-functions.h"
/* The kernel release signature has an unused inode argument. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#include "command-pm-close.h"
#pragma GCC diagnostic pop

static void Reset(uint32_t state, bool with_hardware)
{
    unsigned n;
    starts = stops = tx_stops = cancels = captures = pools = rings = 0;
    elem_deletes = dio_destroys = ring_frees = hardware_opens = hardware_closes = 0;
    irq_depth = irq_disables = irq_enables = capture_unmaps = 0;
    hardware_allocations = hardware_frees = last_close_cfg_users = 0;
    hardware_alloc_attempts = 0;
    hardware_alloc_fail = admission_pending = open_pending = false;
    binding_count = binding_frees = device_reads = user_writes = 0;
    memset(binding_live, 0, sizeof(binding_live));
    chd_device_lock = 0;
    chd_device_generation = 42;
    adapter_visible = true;
    downloads = 0;
    memset(raw_calls, 0, sizeof(raw_calls));
    memset(raw_memory, 0, sizeof(raw_memory));
    raw_offset = raw_value = raw_words = 0;
    raw_buffer = NULL;
    raw_status = BC_STS_SUCCESS;
    events[0] = '\0'; start_ok = stop_ok = true;
    elem_live = dio_live = rings_live = hardware_allocated = false;
    elem_error = dio_error = 0;
    ring_status = hardware_open_status = BC_STS_SUCCESS;
    capture_status = cancel_status = BC_STS_SUCCESS;
    pause_status = firmware_status = BC_STS_SUCCESS;
    pause_calls = firmware_calls = 0;
    memset(pause_states, 0, sizeof(pause_states));
    transaction_mode = block_first_firmware = false;
    first_firmware_waiting = release_first_firmware = false;
    block_download_reset = download_reset_waiting = release_download_reset = false;
    transaction_attempts = 0;
    adapter = (struct crystalhd_adp){ .pdev = &endpoint, .present = true };
    ConfigureHardware(&hardware);
    context = (struct crystalhd_cmd){ .state = state, .adp = &adapter,
        .hw_ctx = with_hardware ? &hardware : NULL, .cin_wait_exit = 1 };
    for (n = 0; n < BC_LINK_MAX_OPENS; n++) {
        context.user[n].uid = n;
        context.user[n].mode = DTS_MODE_INV;
    }
}
static void FirmwarePauseRollback(void)
{
    crystalhd_ioctl_data data = {0};

    Reset(BC_LINK_INIT | BC_LINK_PAUSED, true);
    hardware.fwcmd_poisoned = true;
    data.udata.u.fwCmd.cmd[0] = eCMD_C011_DEC_CHAN_PAUSE;
    data.udata.u.fwCmd.cmd[3] = 0;
    Check(bc_cproc_do_fw_cmd(&context, &data) == BC_STS_BUSY,
          "poisoned firmware resume is rejected before preprocessing");
    Check((context.state & BC_LINK_PAUSED) && !pause_calls &&
          !firmware_calls && hardware.fetch_sem == 1,
          "poisoned resume cannot mutate local capture state");

    Reset(BC_LINK_INIT, true);
    context.cin_wait_exit = 0;
    hardware.fwcmd_poisoned = true;
    data.udata.u.fwCmd.cmd[0] = eCMD_C011_DEC_CHAN_FLUSH;
    data.udata.u.fwCmd.cmd[3] = 1;
    Check(bc_cproc_do_fw_cmd(&context, &data) == BC_STS_BUSY,
          "poisoned firmware flush is rejected before preprocessing");
    Check(!context.cin_wait_exit && !pause_calls && !firmware_calls,
          "poisoned flush cannot publish local cancellation state");

    Reset(BC_LINK_INIT | BC_LINK_PAUSED, true);
    data.udata.u.fwCmd.cmd[0] = eCMD_C011_DEC_CHAN_PAUSE;
    data.udata.u.fwCmd.cmd[3] = 0;
    firmware_status = BC_STS_TIMEOUT;
    Check(bc_cproc_do_fw_cmd(&context, &data) == BC_STS_TIMEOUT,
          "failed firmware resume preserves its original error");
    Check((context.state & BC_LINK_PAUSED) && pause_calls == 2 &&
          !pause_states[0] && pause_states[1] && firmware_calls == 1 &&
          hardware.fetch_sem == 1,
          "failed firmware resume restores the local paused capture state");

    Reset(BC_LINK_INIT, true);
    data.udata.u.fwCmd.cmd[0] = eCMD_C011_DEC_CHAN_PAUSE;
    data.udata.u.fwCmd.cmd[3] = 0;
    firmware_status = BC_STS_TIMEOUT;
    Check(bc_cproc_do_fw_cmd(&context, &data) == BC_STS_TIMEOUT,
          "a redundant resume preserves its firmware failure");
    Check(!(context.state & BC_LINK_PAUSED) && pause_calls == 1 &&
          !pause_states[0] && firmware_calls == 1 && hardware.fetch_sem == 1,
          "failed redundant resume preserves the original running state");

    Reset(BC_LINK_INIT | BC_LINK_PAUSED, true);
    data.udata.u.fwCmd.cmd[0] = eCMD_C011_DEC_CHAN_PAUSE;
    data.udata.u.fwCmd.cmd[3] = 0;
    pause_status = BC_STS_IO_ERROR;
    Check(bc_cproc_do_fw_cmd(&context, &data) == BC_STS_IO_ERROR,
          "failed local resume is returned before posting firmware work");
    Check((context.state & BC_LINK_PAUSED) && pause_calls == 2 &&
          !pause_states[0] && pause_states[1] && !firmware_calls &&
          hardware.fetch_sem == 1,
          "partial local resume is re-paused without changing command state");

    Reset(BC_LINK_INIT | BC_LINK_PAUSED, true);
    data.udata.u.fwCmd.cmd[0] = eCMD_C011_DEC_CHAN_PAUSE;
    data.udata.u.fwCmd.cmd[3] = 0;
    Check(bc_cproc_do_fw_cmd(&context, &data) == BC_STS_SUCCESS,
          "successful resume reaches firmware");
    Check(!(context.state & BC_LINK_PAUSED) && pause_calls == 1 &&
          !pause_states[0] && firmware_calls == 1 && hardware.fetch_sem == 1,
          "successful resume commits the local running state once");

    Reset(BC_LINK_INIT | BC_LINK_PAUSED, true);
    data.udata.u.fwCmd.cmd[0] = eCMD_C011_DEC_CHAN_PAUSE;
    data.udata.u.fwCmd.cmd[3] = 0;
    pause_status = BC_STS_NO_DATA;
    Check(bc_cproc_do_fw_cmd(&context, &data) == BC_STS_SUCCESS,
          "resume with no queued capture buffer still reaches firmware");
    Check(!(context.state & BC_LINK_PAUSED) && pause_calls == 1 &&
          !pause_states[0] && firmware_calls == 1 && hardware.fetch_sem == 1,
          "NO_DATA preserves the legacy successful resume transition");

    Reset(BC_LINK_INIT, true);
    data.udata.u.fwCmd.cmd[0] = eCMD_C011_DEC_CHAN_PAUSE;
    data.udata.u.fwCmd.cmd[3] = 1;
    firmware_status = BC_STS_FW_CMD_ERR;
    Check(bc_cproc_do_fw_cmd(&context, &data) == BC_STS_FW_CMD_ERR,
          "failed firmware pause leaves capture running");
    Check(!(context.state & BC_LINK_PAUSED) && !pause_calls && firmware_calls == 1,
          "failed firmware pause publishes no local pause");
}

struct firmware_thread {
    crystalhd_ioctl_data data;
    BC_STATUS status;
};

static void *RunFirmwareTransaction(void *argument)
{
    struct firmware_thread *thread = argument;

    thread->status = bc_cproc_do_fw_cmd(&context, &thread->data);
    return NULL;
}

static void WaitForFirstFirmware(void)
{
    if (pthread_mutex_lock(&transaction_audit)) abort();
    while (!first_firmware_waiting)
        if (pthread_cond_wait(&transaction_changed, &transaction_audit)) abort();
    if (pthread_mutex_unlock(&transaction_audit)) abort();
}

static void ReleaseFirstFirmware(void)
{
    if (pthread_mutex_lock(&transaction_audit)) abort();
    release_first_firmware = true;
    pthread_cond_broadcast(&transaction_changed);
    if (pthread_mutex_unlock(&transaction_audit)) abort();
}

static void WaitForTransactionAttempts(unsigned wanted)
{
    if (pthread_mutex_lock(&transaction_audit)) abort();
    while (transaction_attempts < wanted)
        if (pthread_cond_wait(&transaction_changed, &transaction_audit)) abort();
    if (pthread_mutex_unlock(&transaction_audit)) abort();
}

struct download_thread {
    crystalhd_ioctl_data data;
    BC_STATUS status;
};

static void *RunFirmwareDownload(void *argument)
{
    struct download_thread *thread = argument;

    thread->status = bc_cproc_download_fw(&context, &thread->data);
    return NULL;
}

static void WaitForDownloadReset(void)
{
    if (pthread_mutex_lock(&transaction_audit)) abort();
    while (!download_reset_waiting)
        if (pthread_cond_wait(&transaction_changed, &transaction_audit)) abort();
    if (pthread_mutex_unlock(&transaction_audit)) abort();
}

static void ReleaseDownloadReset(void)
{
    if (pthread_mutex_lock(&transaction_audit)) abort();
    release_download_reset = true;
    pthread_cond_broadcast(&transaction_changed);
    if (pthread_mutex_unlock(&transaction_audit)) abort();
}

static void FirmwareDownloadSerialization(void)
{
    uint8_t firmware = 0x5a;
    struct download_thread download = {0};
    struct firmware_thread command = {0};
    pthread_t download_tid, command_tid;

    Reset(BC_LINK_INVALID, true);
    transaction_mode = true;
    block_download_reset = true;
    download.data.add_cdata = &firmware;
    download.data.add_cdata_sz = 1;
    command.data.udata.u.fwCmd.cmd[0] = 0x12345678;

    if (pthread_create(&download_tid, NULL, RunFirmwareDownload,
                       &download)) abort();
    WaitForDownloadReset();
    Check((context.state & BC_LINK_INIT) && downloads == 1,
          "firmware download publishes initialized state only inside its transaction");
    if (pthread_create(&command_tid, NULL, RunFirmwareTransaction,
                       &command)) abort();
    WaitForTransactionAttempts(2);
    Check(!firmware_calls,
          "a firmware command cannot enter while download reset is unfinished");

    ReleaseDownloadReset();
    if (pthread_join(download_tid, NULL) ||
        pthread_join(command_tid, NULL)) abort();
    Check(download.status == BC_STS_SUCCESS &&
          command.status == BC_STS_SUCCESS && firmware_calls == 1,
          "the command starts after the successful download transaction leaves");
    transaction_mode = false;
}

static void FirmwareTransactionSerialization(void)
{
    struct firmware_thread first = {0}, second = {0};
    pthread_t first_thread, second_thread;

    Reset(BC_LINK_INIT | BC_LINK_PAUSED, true);
    transaction_mode = true;
    block_first_firmware = true;
    first.data.udata.u.fwCmd.cmd[0] = eCMD_C011_DEC_CHAN_PAUSE;
    first.data.udata.u.fwCmd.cmd[3] = 0;
    second.data = first.data;

    if (pthread_create(&first_thread, NULL, RunFirmwareTransaction, &first)) abort();
    WaitForFirstFirmware();
    if (pthread_create(&second_thread, NULL, RunFirmwareTransaction, &second)) abort();
    WaitForTransactionAttempts(2);
    Check(pause_calls == 1 && !pause_states[0] && firmware_calls == 1 &&
          !(context.state & BC_LINK_PAUSED),
          "a second resume cannot preprocess while the first firmware transaction waits");

    ReleaseFirstFirmware();
    if (pthread_join(first_thread, NULL) ||
        pthread_join(second_thread, NULL)) abort();
    Check(first.status == BC_STS_TIMEOUT && second.status == BC_STS_SUCCESS,
          "serialized resume callers retain their own firmware result");
    Check(pause_calls == 3 && !pause_states[0] && pause_states[1] &&
          !pause_states[2] && firmware_calls == 2 &&
          !(context.state & BC_LINK_PAUSED) && hardware.fetch_sem == 1,
          "timeout rollback finishes before the next resume commits local state");
    transaction_mode = false;
}

struct suspend_thread {
    BC_STATUS status;
};

static void *RunHardwareSuspend(void *argument)
{
    struct suspend_thread *thread = argument;

    thread->status = crystalhd_hw_suspend(&hardware);
    return NULL;
}

static void FirmwareSuspendSerialization(void)
{
    struct firmware_thread command = {0};
    struct suspend_thread suspend = {0};
    pthread_t command_thread, suspend_thread;

    Reset(BC_LINK_INIT | BC_LINK_PAUSED, true);
    transaction_mode = true;
    block_first_firmware = true;
    command.data.udata.u.fwCmd.cmd[0] = eCMD_C011_DEC_CHAN_PAUSE;
    command.data.udata.u.fwCmd.cmd[3] = 0;

    if (pthread_create(&command_thread, NULL, RunFirmwareTransaction,
                       &command)) abort();
    WaitForFirstFirmware();
    if (pthread_create(&suspend_thread, NULL, RunHardwareSuspend,
                       &suspend)) abort();
    WaitForTransactionAttempts(2);
    Check(!stops, "hardware suspend waits for the whole firmware transaction");

    ReleaseFirstFirmware();
    if (pthread_join(command_thread, NULL) ||
        pthread_join(suspend_thread, NULL)) abort();
    Check(command.status == BC_STS_TIMEOUT && suspend.status == BC_STS_SUCCESS &&
          stops == 1,
          "hardware stops only after firmware timeout recovery releases serialization");
    Check((context.state & BC_LINK_PAUSED) && pause_calls == 2 &&
          !pause_states[0] && pause_states[1] && hardware.fetch_sem == 1,
          "suspend observes the command layer after its local rollback is complete");
    transaction_mode = false;
}

static void SessionOwnership(void)
{
    struct crystalhd_user *owner = NULL, *contender = NULL, *reopened = NULL;
    crystalhd_ioctl_data owner_data = {0}, contender_data = {0};
    unsigned pools_before, rings_before;

    Reset(BC_LINK_INVALID, false);
    Check(crystalhd_user_open(&context, &owner) == BC_STS_SUCCESS,
          "the first user opens a hardware-backed session handle");
    adapter.cfg_users++;
    Check(owner == &context.user[0] && owner->in_use && owner->mode == (uint32_t)DTS_MODE_INV &&
          context.hw_ctx == &hardware && hardware_allocated && hardware_opens == 1 &&
          !irq_depth && irq_disables == 1 && irq_enables == 1,
          "first open publishes one unconfigured user and balanced IRQ transition");

    owner_data.u_id = owner->uid;
    owner_data.udata.u.NotifyMode.Mode = DTS_PLAYBACK_MODE;
    Check(bc_cproc_notify_mode(&context, &owner_data) == BC_STS_SUCCESS,
          "the first playback claimant acquires the active session");
    Check(owner->mode == DTS_PLAYBACK_MODE && elem_live && dio_live && rings_live,
          "successful acquisition owns every session resource");

    Check(crystalhd_user_open(&context, &contender) == BC_STS_SUCCESS,
          "a second harmless unconfigured handle may open");
    adapter.cfg_users++;
    Check(contender == &context.user[1] && contender->in_use && hardware_opens == 1,
          "the second open shares the existing hardware context");
    contender_data.u_id = contender->uid;
    pools_before = pools; rings_before = rings;
    contender_data.udata.u.NotifyMode.Mode = DTS_PLAYBACK_MODE;
    Check(bc_cproc_notify_mode(&context, &contender_data) == BC_STS_ERR_USAGE,
          "a second playback claimant is rejected as busy");
    contender_data.udata.u.NotifyMode.Mode = DTS_DIAG_MODE;
    Check(bc_cproc_notify_mode(&context, &contender_data) == BC_STS_ERR_USAGE,
          "a diagnostic claimant cannot bypass the active playback owner");
    Check(contender->mode == (uint32_t)DTS_MODE_INV && pools == pools_before &&
          rings == rings_before && elem_live && dio_live && rings_live,
          "busy rejection neither claims ownership nor mutates live resources");

    contender_data.udata.u.NotifyMode.Mode = DTS_MONITOR_MODE;
    Check(bc_cproc_notify_mode(&context, &contender_data) == BC_STS_SUCCESS,
          "the losing handle may remain a non-owning monitor");
    Check(bc_cproc_release_user(&context, &contender_data) == BC_STS_SUCCESS,
          "releasing the monitor leaves the active playback session intact");
    Check(adapter.cfg_users == 1 && owner->in_use && context.hw_ctx == &hardware &&
          hardware_allocated && !ring_frees && !hardware_closes && elem_live &&
          dio_live && rings_live,
          "non-owner release does not tear down the active owner's resources");

    Check(bc_cproc_release_user(&context, &owner_data) == BC_STS_SUCCESS,
          "the active owner releases its session");
    Check(!adapter.cfg_users && !owner->in_use && owner->mode == (uint32_t)DTS_MODE_INV &&
          context.hw_ctx == NULL && !hardware_allocated && !elem_live && !dio_live &&
          !rings_live && ring_frees == 1 && hardware_closes == 1 && capture_unmaps == 1 &&
          !irq_depth && irq_disables == 2 && irq_enables == 2,
          "owner release retires each resource once and balances the user/IRQ state");

    Check(crystalhd_user_open(&context, &reopened) == BC_STS_SUCCESS,
          "a fresh handle reopens after owner release");
    adapter.cfg_users++;
    owner_data.u_id = reopened->uid;
    owner_data.udata.u.NotifyMode.Mode = DTS_PLAYBACK_MODE;
    Check(bc_cproc_notify_mode(&context, &owner_data) == BC_STS_SUCCESS,
          "the reopened handle reacquires playback ownership");
    Check(reopened == &context.user[0] && hardware_opens == 2 && reopened->in_use &&
          reopened->mode == DTS_PLAYBACK_MODE && elem_live && dio_live && rings_live,
          "reopen reuses the released slot with a fresh complete session");
    Check(bc_cproc_release_user(&context, &owner_data) == BC_STS_SUCCESS,
          "the reopened owner releases cleanly");
    Check(!adapter.cfg_users && context.hw_ctx == NULL && !hardware_allocated &&
          hardware_opens == 2 && hardware_closes == 2 && ring_frees == 2 &&
          elem_deletes == 2 && dio_destroys == 2 && capture_unmaps == 2 && !irq_depth,
          "the complete open/busy/release/reopen cycle leaves no owner or allocation");
}
static void MonitorOnlyRelease(void)
{
    struct crystalhd_user *monitor = NULL;
    crystalhd_ioctl_data data = {0};

    Reset(BC_LINK_INVALID, false);
    Check(crystalhd_user_open(&context, &monitor) == BC_STS_SUCCESS,
          "a monitor opens a hardware-backed handle");
    adapter.cfg_users++;
    data.u_id = monitor->uid;
    data.udata.u.NotifyMode.Mode = DTS_MONITOR_MODE;
    Check(bc_cproc_notify_mode(&context, &data) == BC_STS_SUCCESS,
          "the only handle enters monitor mode without session allocations");
    Check(!elem_live && !dio_live && !rings_live && !pools && !rings,
          "monitor admission creates no playback resources");

    Check(bc_cproc_release_user(&context, &data) == BC_STS_SUCCESS,
          "the final monitor handle releases cleanly");
    Check(!adapter.cfg_users && !monitor->in_use &&
          monitor->mode == (uint32_t)DTS_MODE_INV && context.hw_ctx == NULL &&
          !hardware_allocated && !elem_live && !dio_live && !rings_live,
          "final monitor release leaves no handle, hardware context, or allocation");
    Check(hardware_opens == 1 && hardware_closes == 1 && capture_unmaps == 1 &&
          ring_frees == 1 && dio_destroys == 1 && elem_deletes == 1 &&
          irq_disables == 2 && irq_enables == 2 && !irq_depth,
          "final monitor release performs each safe empty teardown exactly once");
}
static void OwnerBeforeMonitor(void)
{
    struct crystalhd_user *owner = NULL, *monitor = NULL, *reopened = NULL;
    crystalhd_ioctl_data owner_data = {0}, monitor_data = {0};

    Reset(BC_LINK_INVALID, false);
    Check(crystalhd_user_open(&context, &owner) == BC_STS_SUCCESS,
          "the playback owner opens first");
    adapter.cfg_users++;
    owner_data.u_id = owner->uid;
    owner_data.udata.u.NotifyMode.Mode = DTS_PLAYBACK_MODE;
    Check(bc_cproc_notify_mode(&context, &owner_data) == BC_STS_SUCCESS,
          "the first handle acquires playback resources");

    Check(crystalhd_user_open(&context, &monitor) == BC_STS_SUCCESS,
          "a monitor opens beside the playback owner");
    adapter.cfg_users++;
    monitor_data.u_id = monitor->uid;
    monitor_data.udata.u.NotifyMode.Mode = DTS_MONITOR_MODE;
    Check(bc_cproc_notify_mode(&context, &monitor_data) == BC_STS_SUCCESS,
          "the second handle becomes a non-owning monitor");

    Check(bc_cproc_release_user(&context, &owner_data) == BC_STS_SUCCESS,
          "the playback owner may close before its monitor");
    Check(adapter.cfg_users == 1 && monitor->in_use &&
          monitor->mode == DTS_MONITOR_MODE && context.hw_ctx == NULL &&
          !hardware_allocated && !elem_live && !dio_live && !rings_live,
          "owner-first release retains only the monitor handle");
    Check(hardware_closes == 1 && ring_frees == 1 && dio_destroys == 1 &&
          elem_deletes == 1 && capture_unmaps == 1,
          "owner-first release retires each playback resource exactly once");

    Check(crystalhd_user_open(&context, &reopened) == BC_STS_SUCCESS,
          "a fresh handle opens while the monitor remains");
    adapter.cfg_users++;
    owner_data.u_id = reopened->uid;
    owner_data.udata.u.NotifyMode.Mode = DTS_PLAYBACK_MODE;
    Check(bc_cproc_notify_mode(&context, &owner_data) == BC_STS_SUCCESS,
          "the fresh handle reacquires playback beside the existing monitor");
    Check(reopened == &context.user[0] && adapter.cfg_users == 2 &&
          monitor->in_use && monitor->mode == DTS_MONITOR_MODE &&
          reopened->mode == DTS_PLAYBACK_MODE && elem_live && dio_live && rings_live,
          "reacquisition restores one complete playback owner without changing the monitor");

    Check(bc_cproc_release_user(&context, &owner_data) == BC_STS_SUCCESS,
          "the replacement playback owner releases first");
    Check(bc_cproc_release_user(&context, &monitor_data) == BC_STS_SUCCESS,
          "the remaining monitor releases after playback teardown");
    Check(!adapter.cfg_users && context.hw_ctx == NULL && !hardware_allocated &&
          !owner->in_use && !monitor->in_use && hardware_opens == 2 &&
          hardware_closes == 2 && ring_frees == 2 && dio_destroys == 2 &&
          elem_deletes == 2 && capture_unmaps == 2 && irq_disables == 4 &&
          irq_enables == 4 && !irq_depth,
          "owner-first close, reacquire, and final close preserve exact teardown counts");
}
static struct file OpenFile(uint32_t mode)
{
    struct crystalhd_user *user = NULL;
    crystalhd_ioctl_data data = {0};
    struct file file;

    Check(crystalhd_user_open(&context, &user) == BC_STS_SUCCESS,
          "file setup uses the actual user-open implementation");
    if (!user || binding_count == sizeof(bindings) / sizeof(bindings[0]))
        abort();
    adapter.cfg_users++;
    bindings[binding_count] = (struct crystalhd_file){
        .user = user, .generation = chd_device_generation };
    binding_live[binding_count] = true;
    file.private_data = &bindings[binding_count++];
    if (mode != (uint32_t)DTS_MODE_INV) {
        data.u_id = user->uid;
        data.udata.u.NotifyMode.Mode = mode;
        Check(bc_cproc_notify_mode(&context, &data) == BC_STS_SUCCESS,
              "file setup uses actual notify-mode admission");
    }
    return file;
}
static void CloseFile(struct file *file)
{
    Check(chd_dec_close(NULL, file) == 0,
          "the actual file-release wrapper preserves successful close status");
    Check(!file->private_data && !chd_device_lock && !adapter.user_lock && !irq_depth,
          "file release clears its binding and balances every lock and IRQ");
}
static void CheckNoSession(void)
{
    Check(!adapter.cfg_users && !bc_get_userhandle_count(&context) &&
          !context.session_owner && !context.hw_ctx && !hardware_allocated && !elem_live && !dio_live &&
          !rings_live && !adapter.fill_byte_pool && !adapter.elem_pool_head &&
          !hardware.rx_freeq,
          "the final close leaves no session user, hardware, or pool allocation");
}
static const uint32_t resource_modes[] = { DTS_PLAYBACK_MODE, DTS_DIAG_MODE,
    DTS_HWINIT_MODE, 0x7f, UINT32_MAX, 0x100 | DTS_PLAYBACK_MODE,
    0x81000000 | DTS_DIAG_MODE, 0x100 | DTS_HWINIT_MODE, 0x8100007f };
static struct file OpenResourceFile(uint32_t mode)
{
    struct file file = OpenFile(DTS_MODE_INV);
    struct crystalhd_user *user = ((struct crystalhd_file *)file.private_data)->user;
    crystalhd_ioctl_data data = { .u_id = user->uid };

    admission_pending = true;
    admission_uid = user->uid;
    admission_state = context.state;
    admission_wait = context.cin_wait_exit;
    data.udata.u.NotifyMode.Mode = mode;
    Check(bc_cproc_notify_mode(&context, &data) == BC_STS_SUCCESS,
          "all legacy non-monitor modes can acquire an idle resource set");
    admission_pending = false;
    Check(context.session_owner == user && user->mode == mode &&
          elem_live && dio_live && rings_live,
          "successful setup records the allocating user without discarding mode flags");
    return file;
}
static void ResourceOwnership(void)
{
    unsigned mode, request;

    for (mode = 0; mode < sizeof(resource_modes) / sizeof(resource_modes[0]); mode++) {
        struct file owner, contender;
        struct crystalhd_user *owner_user, *contender_user;
        crystalhd_ioctl_data data = {0};

        Reset(BC_LINK_INVALID, false);
        owner = OpenResourceFile(resource_modes[mode]);
        owner_user = ((struct crystalhd_file *)owner.private_data)->user;
        contender = OpenFile(DTS_MODE_INV);
        contender_user = ((struct crystalhd_file *)contender.private_data)->user;
        for (request = 0; request < sizeof(resource_modes) / sizeof(resource_modes[0]); request++) {
            data.u_id = contender_user->uid;
            data.udata.u.NotifyMode.Mode = resource_modes[request];
            Check(bc_cproc_notify_mode(&context, &data) == BC_STS_ERR_USAGE,
                  "every resource-owning mode excludes overlapping decoder allocation");
            data.u_id = owner_user->uid;
            Check(bc_cproc_notify_mode(&context, &data) == BC_STS_ERR_USAGE,
                  "the recorded owner cannot reconfigure or allocate its resources again");
            Check(context.session_owner == owner_user &&
                  owner_user->mode == resource_modes[mode] &&
                  contender_user->mode == (uint32_t)DTS_MODE_INV &&
                  pools == 2 && rings == 1 && adapter.cfg_users == 2 &&
                  elem_live && dio_live && rings_live,
                  "rejected ownership requests preserve the complete live resource set");
        }
        data.u_id = owner_user->uid;
        data.udata.u.NotifyMode.Mode = 0x100 | DTS_MONITOR_MODE;
        Check(bc_cproc_notify_mode(&context, &data) == BC_STS_ERR_USAGE,
              "even a sentinel-valued owner cannot turn into a monitor");
        data.u_id = contender_user->uid;
        Check(bc_cproc_notify_mode(&context, &data) == BC_STS_SUCCESS &&
              contender_user->mode == (0x100 | DTS_MONITOR_MODE) &&
              context.session_owner == owner_user && pools == 2 && rings == 1,
              "a separate flagged monitor can coexist without replacing the resource owner");
        data.u_id = owner_user->uid;
        Check(bc_cproc_release_user(&context, &data) ==
              (resource_modes[mode] == (uint32_t)DTS_MODE_INV ? BC_STS_ERR_USAGE : BC_STS_SUCCESS),
              "RELEASE preserves its sentinel-mode error and releases other resource owners");
        if (resource_modes[mode] == (uint32_t)DTS_MODE_INV)
            Check(context.session_owner == owner_user && owner_user->in_use &&
                  context.hw_ctx == &hardware && !hardware_closes,
                  "a rejected sentinel RELEASE leaves resource ownership for file close");
        else
            Check(!context.session_owner && !context.hw_ctx && adapter.cfg_users == 1 &&
                  hardware_closes == 1 && !elem_live && !dio_live && !rings_live,
                  "owner RELEASE retires resources while its monitor remains open");
        CloseFile(&owner);
        CloseFile(&contender);
        CheckNoSession();
        Check(hardware_closes == 1 && hardware_frees == 1 && ring_frees == 1 &&
              elem_deletes == 1 && dio_destroys == 1,
              "owner RELEASE and file close cannot retire resources twice");
    }
}
static void PendingOpenAfterOwnerClose(void)
{
    unsigned mode;

    for (mode = 0; mode < sizeof(resource_modes) / sizeof(resource_modes[0]); mode++) {
        struct file owner, pending, reopened;
        struct crystalhd_user *pending_user;
        crystalhd_ioctl_data data = {0};

        Reset(BC_LINK_INVALID, false);
        owner = OpenResourceFile(resource_modes[mode]);
        pending = OpenFile(DTS_MODE_INV);
        pending_user = ((struct crystalhd_file *)pending.private_data)->user;
        CloseFile(&owner);
        Check(!context.session_owner && !context.hw_ctx && pending_user->in_use &&
              adapter.cfg_users == 1 && hardware_opens == 1 && hardware_closes == 1,
              "owner close leaves a pending unconfigured handle without hardware");
        data.u_id = pending_user->uid;
        data.udata.u.NotifyMode.Mode = DTS_PLAYBACK_MODE;
        Check(bc_cproc_notify_mode(&context, &data) == BC_STS_INV_ARG,
              "pending handle retains the existing no-hardware setup error");
        Check(!context.session_owner && !context.hw_ctx &&
              pending_user->mode == (uint32_t)DTS_MODE_INV && adapter.cfg_users == 1 &&
              pools == 4 && rings == 2 && !elem_live && !dio_live && !rings_live &&
              elem_deletes == 2 && dio_destroys == 2 &&
              hardware_opens == 1 && hardware_allocations == 1,
              "failed pending acquisition rolls back pools without reopening hardware");
        reopened = OpenResourceFile(DTS_DIAG_MODE);
        Check(context.session_owner == &context.user[0] && hardware_opens == 2,
              "a new open reuses the retired slot with fresh hardware ownership");
        CloseFile(&pending);
        Check(context.session_owner == &context.user[0] && context.hw_ctx == &hardware &&
              adapter.cfg_users == 1 && hardware_closes == 1,
              "closing the pending non-owner preserves the newly acquired session");
        CloseFile(&reopened);
        CheckNoSession();
    }
}
static BC_STATUS (*const raw_commands[])(struct crystalhd_cmd *, crystalhd_ioctl_data *) = {
    bc_cproc_reg_rd, bc_cproc_reg_wr, bc_cproc_link_reg_rd, bc_cproc_link_reg_wr,
    bc_cproc_mem_rd, bc_cproc_mem_wr
};
static unsigned RawCallCount(void)
{
    unsigned count = 0;
    for (unsigned n = 0; n < sizeof(raw_calls) / sizeof(raw_calls[0]); n++)
        count += raw_calls[n];
    return count;
}
static void RawInvalidArguments(void)
{
    for (unsigned op = 0; op < sizeof(raw_commands) / sizeof(raw_commands[0]); op++) {
        uint32_t buffer[2] = { 0x12345678, 0x87654321 };
        crystalhd_ioctl_data data = { .add_cdata = buffer, .add_cdata_sz = sizeof(buffer) };
        crystalhd_ioctl_data before = data;

        Reset(BC_LINK_INVALID, false);
        Check(raw_commands[op](NULL, &data) == BC_STS_INV_ARG &&
              raw_commands[op](&context, NULL) == BC_STS_INV_ARG &&
              raw_commands[op](NULL, NULL) == BC_STS_INV_ARG,
              "raw register and memory commands reject NULL command arguments");
        Check(raw_commands[op](&context, &data) == BC_STS_INV_ARG,
              "raw commands reject a retired hardware context");
        Check(!RawCallCount() && !memcmp(&data, &before, sizeof(data)) &&
              buffer[0] == 0x12345678 && buffer[1] == 0x87654321,
              "invalid raw commands invoke no callback and leave output untouched");
    }
}
static void RawRegisterCommands(void)
{
    const uint32_t values[] = { 0, 0x89abcdef, UINT32_MAX };

    for (unsigned op = 0; op < 4; op++) {
        for (unsigned n = 0; n < sizeof(values) / sizeof(values[0]); n++) {
            crystalhd_ioctl_data data = {0};

            Reset(BC_LINK_INVALID, true);
            data.udata.u.regAcc.Offset = values[n];
            data.udata.u.regAcc.Value = op & 1 ? values[n] : ~values[n];
            raw_value = op & 1 ? ~values[n] : values[n];
            Check(raw_commands[op](&context, &data) == BC_STS_SUCCESS,
                  "valid device and link register commands retain successful status");
            Check(raw_calls[op] == 1 && RawCallCount() == 1 && raw_offset == values[n] &&
                  raw_value == values[n] && data.udata.u.regAcc.Value == values[n],
                  "register commands preserve offset and read or write the exact 32-bit value");
        }
    }
}
static void RawMemoryCommands(void)
{
    const BC_STATUS statuses[] = { BC_STS_SUCCESS, BC_STS_IO_ERROR, BC_STS_TIMEOUT };

    for (unsigned op = 4; op < 6; op++) {
        for (unsigned s = 0; s < sizeof(statuses) / sizeof(statuses[0]); s++) {
            for (unsigned words = 0; words <= 2; words++) {
                uint32_t buffer[2] = { 0x12345678, 0x87654321 };
                const uint32_t payload[] = { buffer[0], buffer[1] };
                const uint32_t dram[] = { 0xabcdef01, 0x10fedcba };
                crystalhd_ioctl_data data = { .add_cdata = buffer, .add_cdata_sz = 4 * words };

                Reset(BC_LINK_INVALID, true);
                data.udata.u.devMem.StartOff = 0x98765432;
                data.udata.u.devMem.NumDwords = words;
                raw_buffer = buffer;
                raw_status = statuses[s];
                memcpy(raw_memory, dram, sizeof(dram));
                Check(raw_commands[op](&context, &data) == statuses[s],
                      "memory commands propagate success and hardware callback errors");
                Check(raw_calls[op] == 1 && RawCallCount() == 1 &&
                      raw_offset == 0x98765432 && raw_words == words,
                      "memory commands forward exact offset, word count and buffer");
                for (unsigned n = 0; n < 2; n++) {
                    bool copied = statuses[s] == BC_STS_SUCCESS && n < words;
                    Check(buffer[n] == (copied && op == 4 ? dram[n] : payload[n]) &&
                          raw_memory[n] == (copied && op == 5 ? payload[n] : dram[n]),
                          "memory reads and writes affect only the requested successful transfer");
                }
            }
        }
        for (unsigned bad = 0; bad < 4; bad++) {
            uint32_t buffer[2] = {0};
            crystalhd_ioctl_data data = { .add_cdata = buffer, .add_cdata_sz = sizeof(buffer) };

            Reset(BC_LINK_INVALID, true);
            data.udata.u.devMem.NumDwords = 2;
            if (bad == 0) data.add_cdata = NULL;
            if (bad == 1) data.add_cdata_sz = 7;
            if (bad == 2) data.add_cdata_sz = 3;
            if (bad == 3) {
                data.add_cdata_sz = UINT32_MAX;
                data.udata.u.devMem.NumDwords = UINT32_MAX;
            }
            Check(raw_commands[op](&context, &data) == BC_STS_INV_ARG && !RawCallCount(),
                  "missing or undersized memory buffers fail before callbacks without overflow");
        }
    }
}
static void MonitorRawAfterOwnerClose(void)
{
    struct file owner, monitor;
    struct crystalhd_user *monitor_user;
    uint32_t buffer[2] = {0};
    crystalhd_ioctl_data data = { .add_cdata = buffer, .add_cdata_sz = sizeof(buffer) };

    Reset(BC_LINK_INVALID, false);
    owner = OpenResourceFile(DTS_PLAYBACK_MODE);
    monitor = OpenFile(0x100 | DTS_MONITOR_MODE);
    monitor_user = ((struct crystalhd_file *)monitor.private_data)->user;
    CloseFile(&owner);
    Check(monitor_user->in_use && !context.hw_ctx && !context.session_owner &&
          adapter.cfg_users == 1 && hardware_closes == 1,
          "real owner close leaves the surviving monitor without hardware");
    data.u_id = monitor_user->uid;
    for (unsigned op = 0; op < sizeof(raw_commands) / sizeof(raw_commands[0]); op++)
        Check(raw_commands[op](&context, &data) == BC_STS_INV_ARG,
              "a surviving monitor cannot dereference retired raw-command hardware");
    Check(!RawCallCount() && hardware_opens == 1 && hardware_closes == 1,
          "rejected monitor raw commands neither reach hardware nor recreate it");
    CloseFile(&monitor);
    CheckNoSession();
}
static void FileCloseModes(void)
{
    const uint32_t modes[] = { DTS_MODE_INV, DTS_MONITOR_MODE, DTS_PLAYBACK_MODE,
        DTS_DIAG_MODE, 0x100 | DTS_PLAYBACK_MODE, 0x100 | DTS_DIAG_MODE };
    unsigned n;

    for (n = 0; n < sizeof(modes) / sizeof(modes[0]); n++) {
        struct file file;
        bool owner = n >= 2;

        Reset(BC_LINK_INVALID, false);
        file = OpenFile(modes[n]);
        context.cin_wait_exit = 0;
        context.pwr_state_change = BC_HW_SUSPEND;
        CloseFile(&file);
        CheckNoSession();
        Check(context.state == BC_LINK_INVALID && context.cin_wait_exit == 1 &&
              context.pwr_state_change == BC_HW_RUNNING,
              "normal close resets command cancellation and power state");
        Check(hardware_allocations == 1 && hardware_frees == 1 &&
              hardware_opens == 1 && hardware_closes == 1 && last_close_cfg_users == 1,
              "normal close retires hardware before decrementing the last user");
        Check(captures == 1 && capture_unmaps == 1 && ring_frees == 1 &&
              dio_destroys == 1 && elem_deletes == 1 &&
              pools == (owner ? 2U : 0U) && rings == owner,
              "common close safely handles empty resources without new allocations");
        Check(irq_disables == 2 && irq_enables == 2 && device_reads == 1 &&
              user_writes == 1 && binding_frees == 1,
              "normal open and file close balance hardware and binding ownership");
    }
}
static void FileOwnerBeforeMonitor(void)
{
    struct file owner, monitor, reopened;
    struct crystalhd_user *owner_user, *monitor_user;
    unsigned mode, monitor_first;

    for (mode = 0; mode < sizeof(resource_modes) / sizeof(resource_modes[0]); mode++) {
        for (monitor_first = 0; monitor_first < 2; monitor_first++) {
            Reset(BC_LINK_INVALID, false);
            owner = OpenResourceFile(resource_modes[mode]);
            owner_user = ((struct crystalhd_file *)owner.private_data)->user;
            monitor = OpenFile(DTS_MONITOR_MODE);
            monitor_user = ((struct crystalhd_file *)monitor.private_data)->user;
            if (monitor_first) {
                CloseFile(&monitor);
                Check(adapter.cfg_users == 1 && context.hw_ctx == &hardware &&
                      context.session_owner == owner_user &&
                      !hardware_closes && !hardware_frees && !ring_frees &&
                      !captures && elem_live && dio_live && rings_live,
                      "monitor-first file close preserves every session resource");
                CloseFile(&owner);
            } else {
                CloseFile(&owner);
                Check(adapter.cfg_users == 1 && monitor_user->in_use &&
                      monitor_user->mode == DTS_MONITOR_MODE && !context.hw_ctx &&
                      !context.session_owner &&
                      hardware_closes == 1 && hardware_frees == 1 &&
                      last_close_cfg_users == 2 && !elem_live && !dio_live && !rings_live,
                      "owner-first file close retires hardware while preserving its monitor");
                reopened = OpenResourceFile(DTS_DIAG_MODE);
                Check(adapter.cfg_users == 2 && monitor_user->in_use &&
                      context.session_owner == owner_user && owner_user->in_use &&
                      monitor_user->mode == DTS_MONITOR_MODE && hardware_opens == 2 &&
                      elem_live && dio_live && rings_live,
                      "a diagnostic owner reacquires fresh hardware beside the existing monitor");
                CloseFile(&reopened);
                CloseFile(&monitor);
            }
            CheckNoSession();
            Check(hardware_opens == 2 - monitor_first &&
                  hardware_closes == hardware_opens && hardware_allocations == hardware_opens &&
                  hardware_frees == hardware_opens && ring_frees == hardware_opens &&
                  capture_unmaps == hardware_opens && dio_destroys == hardware_opens &&
                  elem_deletes == hardware_opens && irq_disables == 2 * hardware_opens &&
                  irq_enables == irq_disables && binding_frees == 3 - monitor_first,
                  "both owner/monitor close orders retain exact allocation and teardown counts");
        }
    }
}
static void ReleaseThenFileClose(void)
{
    const uint32_t modes[] = { DTS_MODE_INV, DTS_MONITOR_MODE, DTS_PLAYBACK_MODE,
        DTS_DIAG_MODE };
    unsigned n;

    for (n = 0; n < sizeof(modes) / sizeof(modes[0]); n++) {
        struct file file;
        crystalhd_ioctl_data data = {0};
        bool configured = n != 0;

        Reset(BC_LINK_INVALID, false);
        file = OpenFile(modes[n]);
        data.u_id = ((struct crystalhd_file *)file.private_data)->user->uid;
        Check(bc_cproc_release_user(&context, &data) ==
              (configured ? BC_STS_SUCCESS : BC_STS_ERR_USAGE),
              "RELEASE preserves unconfigured rejection and configured success");
        if (configured) {
            CheckNoSession();
            Check(last_close_cfg_users == 1 && capture_unmaps == 1 && ring_frees == 1 &&
                  dio_destroys == 1 && elem_deletes == 1,
                  "RELEASE retains its safe empty teardown calls for the last monitor");
            Check(bc_cproc_release_user(&context, &data) == BC_STS_ERR_USAGE,
                  "repeated RELEASE keeps its existing already-closed error");
        } else {
            Check(adapter.cfg_users == 1 && hardware_allocated && !hardware_closes,
                  "rejected unconfigured RELEASE leaves the handle for file close");
        }
        CloseFile(&file);
        CheckNoSession();
        Check(hardware_closes == 1 && hardware_allocations == 1 && hardware_frees == 1 &&
              irq_disables == 2 && irq_enables == 2 && binding_frees == 1,
              "RELEASE followed by file close retires hardware and binding exactly once");
    }
}
static void FileCloseAfterSetupFailure(void)
{
    unsigned which;

    for (which = 0; which < 3; which++) {
        struct file file;
        crystalhd_ioctl_data data = {0};

        Reset(BC_LINK_INVALID, false);
        file = OpenFile(DTS_MODE_INV);
        data.u_id = ((struct crystalhd_file *)file.private_data)->user->uid;
        data.udata.u.NotifyMode.Mode = DTS_PLAYBACK_MODE;
        if (which == 0) elem_error = -1;
        if (which == 1) dio_error = -1;
        if (which == 2) ring_status = BC_STS_INSUFF_RES;
        Check(bc_cproc_notify_mode(&context, &data) ==
              (which == 2 ? BC_STS_INSUFF_RES : BC_STS_ERROR),
              "session setup failure retains its original status before file close");
        CloseFile(&file);
        CheckNoSession();
        Check(hardware_closes == 1 && hardware_allocations == 1 && hardware_frees == 1 &&
              last_close_cfg_users == 1 && captures == 1 && ring_frees == 1 &&
              elem_deletes == 2 && dio_destroys == 1 + (which == 2) &&
              irq_disables == 2 && irq_enables == 2 && binding_frees == 1,
              "file close after failed acquisition safely completes empty-resource cleanup");
    }
}
static void CloseCaptureFailure(void)
{
    unsigned via_release;

    for (via_release = 0; via_release < 2; via_release++) {
        struct file file;
        crystalhd_ioctl_data data = {0};

        Reset(BC_LINK_INVALID, false);
        file = OpenFile(DTS_PLAYBACK_MODE);
        capture_status = BC_STS_TIMEOUT;
        if (via_release) {
            data.u_id = ((struct crystalhd_file *)file.private_data)->user->uid;
            Check(bc_cproc_release_user(&context, &data) == BC_STS_SUCCESS,
                  "RELEASE retains its existing success result after capture-stop failure");
        }
        CloseFile(&file);
        CheckNoSession();
        Check(captures == 1 && capture_unmaps == 1 && ring_frees == 1 &&
              dio_destroys == 1 && elem_deletes == 1 && hardware_closes == 1 &&
              hardware_frees == 1 && last_close_cfg_users == 1 &&
              irq_disables == 2 && irq_enables == 2 && binding_frees == 1,
              "both close paths complete existing teardown after capture-stop failure");
    }
}
static void FileCloseUnavailableDevice(void)
{
    struct file file;
    struct crystalhd_user *user;
    unsigned which;

    for (which = 0; which < 3; which++) {
        Reset(BC_LINK_INVALID, false);
        file = OpenFile(DTS_PLAYBACK_MODE);
        user = ((struct crystalhd_file *)file.private_data)->user;
        if (which == 0) adapter.present = false;
        if (which == 1) adapter_visible = false;
        if (which == 2) chd_device_generation++;
        CloseFile(&file);
        Check(context.hw_ctx == &hardware && hardware_allocated && elem_live &&
              context.session_owner == user &&
              dio_live && rings_live && !captures && !ring_frees &&
              !dio_destroys && !elem_deletes && !hardware_closes && !hardware_frees &&
              irq_disables == 1 && irq_enables == 1 && binding_frees == 1,
              "failed PM or stale device close leaves hardware for remove's quiesced cleanup");
        Check(user->in_use == (which != 0) && adapter.cfg_users == (which != 0) &&
              user_writes == (which == 0),
              "only the matching failed-PM device retires its logical file user");
    }
}
static void OwnerCloseWithoutHardware(void)
{
    unsigned n, monitor;

    for (n = 0; n < sizeof(resource_modes) / sizeof(resource_modes[0]); n++) {
        for (monitor = 0; monitor < 2; monitor++) {
            struct crystalhd_user *user;

            Reset(BC_LINK_INVALID, false);
            user = &context.user[0];
            user->in_use = 1;
            user->mode = resource_modes[n];
            context.session_owner = user;
            context.user[1].in_use = monitor;
            context.user[1].mode = DTS_MONITOR_MODE;
            adapter.cfg_users = 1 + monitor;
            crystalhd_user_close(&context, user);
            Check(!user->in_use && user->mode == (uint32_t)DTS_MODE_INV &&
                  !context.session_owner && adapter.cfg_users == monitor,
                  "owner close can retire a logical user after hardware is already gone");
            crystalhd_user_close(&context, user);
            crystalhd_user_close(&context, &context.user[1]);
            CheckNoSession();
            Check(!hardware_allocations && !hardware_frees && !hardware_closes &&
                  !captures && !ring_frees && !dio_destroys && !elem_deletes &&
                  !irq_disables && !irq_enables,
                  "missing-hardware and repeated close perform no device or pool operation");
        }
    }
}
static void CheckNotify(void)
{
    crystalhd_ioctl_data data = { .u_id = 1 };
    /* A later user_open creates a fresh hardware context after an idle close. */
    context.hw_ctx = &hardware;
    context.user[1].in_use = 1;
    data.udata.u.NotifyMode.Mode = DTS_PLAYBACK_MODE;
    Check(bc_cproc_notify_mode(&context, &data) == BC_STS_SUCCESS,
          "actual notify-mode admits the next playback after idle resume");
    Check(context.user[1].mode == DTS_PLAYBACK_MODE && !context.cin_wait_exit,
          "notify-mode commits the new playback owner");
    Check(pools == 2 && rings == 1 && elem_live && dio_live && rings_live,
          "notify-mode reaches real allocation/ring call sequence");
}
static void NotifyFailures(void)
{
    crystalhd_ioctl_data data = { .u_id = 1 };
    unsigned mode, which;

    for (mode = 0; mode < sizeof(resource_modes) / sizeof(resource_modes[0]); mode++) {
        data.udata.u.NotifyMode.Mode = resource_modes[mode];
        for (which = 0; which < 3; which++) {
            BC_STATUS expected = which == 2 ? BC_STS_INSUFF_RES : BC_STS_ERROR;

            Reset(BC_LINK_INVALID, true);
            context.user[1].in_use = 1;
            admission_pending = true;
            admission_uid = 1;
            admission_state = context.state;
            admission_wait = context.cin_wait_exit;
            if (which == 0) elem_error = -1;
            if (which == 1) dio_error = -1;
            if (which == 2) ring_status = BC_STS_INSUFF_RES;
            Check(bc_cproc_notify_mode(&context, &data) == expected,
                  "notify-mode propagates each session setup failure");
            Check(context.user[1].mode == DTS_MODE_INV && !context.session_owner &&
                  context.cin_wait_exit == 1,
                  "failed setup does not publish resource ownership");
            Check(!elem_live && !dio_live && !rings_live,
                  "failed setup leaves no session allocation live");
            Check(elem_deletes == 1 && dio_destroys == (which == 2) && !ring_frees,
                  "failed setup releases every successfully created pool");

            elem_error = dio_error = 0;
            ring_status = BC_STS_SUCCESS;
            Check(bc_cproc_notify_mode(&context, &data) == BC_STS_SUCCESS,
                  "the same handle can retry session setup after failure");
            Check(context.user[1].mode == resource_modes[mode] &&
                  context.session_owner == &context.user[1] && !context.cin_wait_exit &&
                  elem_live && dio_live && rings_live,
                  "successful retry commits one complete session resource owner");
        }
    }
}
static void MonitorAdmission(void)
{
    const uint32_t states[] = { BC_LINK_INVALID, BC_LINK_INIT, BC_LINK_READY,
        BC_LINK_READY | BC_LINK_PAUSED, BC_LINK_SUSPEND, BC_LINK_RESUME };
    const uint32_t modes[] = { DTS_MONITOR_MODE, 0x100 | DTS_MONITOR_MODE,
        0x81000000 | DTS_MONITOR_MODE };
    unsigned s, m;

    for (s = 0; s < sizeof(states) / sizeof(states[0]); s++) {
        for (m = 0; m < sizeof(modes) / sizeof(modes[0]); m++) {
            crystalhd_ioctl_data data = { .u_id = 1 };

            Reset(states[s], true);
            context.user[0].in_use = context.user[1].in_use = 1;
            context.user[0].mode = 0x100 | DTS_PLAYBACK_MODE;
            adapter.cfg_users = 2;
            data.udata.u.NotifyMode.Mode = modes[m];
            Check(bc_cproc_notify_mode(&context, &data) == BC_STS_SUCCESS,
                  "monitor admission bypasses owner and decoder-state exclusion");
            Check(context.user[1].mode == modes[m] && context.user[1].in_use &&
                  context.user[0].mode == (0x100 | DTS_PLAYBACK_MODE) &&
                  context.state == states[s] && context.cin_wait_exit == 1 &&
                  adapter.cfg_users == 2 && context.hw_ctx == &hardware,
                  "monitor mode preserves flags and leaves active ownership unchanged");
            Check(!pools && !rings && !hardware_opens && !hardware_closes &&
                  !irq_disables && !irq_enables,
                  "monitor admission performs no allocation or device transition");
        }
    }
}
static void NotifyAdmissionMatrix(void)
{
    const uint32_t owners[] = { DTS_PLAYBACK_MODE, DTS_DIAG_MODE,
        0x100 | DTS_PLAYBACK_MODE, 0x81000000 | DTS_DIAG_MODE };
    const uint32_t requests[] = { DTS_PLAYBACK_MODE, DTS_DIAG_MODE,
        0x100 | DTS_PLAYBACK_MODE, 0x81000000 | DTS_DIAG_MODE, DTS_HWINIT_MODE,
        0x100 | DTS_HWINIT_MODE };
    const uint32_t states[] = { BC_LINK_INIT, BC_LINK_READY,
        BC_LINK_READY | BC_LINK_PAUSED, BC_LINK_SUSPEND, BC_LINK_RESUME };
    unsigned o, r, s;

    for (o = 0; o < sizeof(owners) / sizeof(owners[0]); o++) {
        for (r = 0; r < sizeof(requests) / sizeof(requests[0]); r++) {
            crystalhd_ioctl_data data = { .u_id = 1 };

            Reset(BC_LINK_INVALID, true);
            context.user[0].in_use = context.user[1].in_use = 1;
            context.user[0].mode = owners[o];
            adapter.cfg_users = 2;
            data.udata.u.NotifyMode.Mode = requests[r];
            Check(bc_cproc_notify_mode(&context, &data) == BC_STS_ERR_USAGE,
                  "playback and diagnostic owners reject all decoder-mode contenders");
            Check(context.user[0].mode == owners[o] &&
                  context.user[1].mode == (uint32_t)DTS_MODE_INV &&
                  context.user[1].in_use && adapter.cfg_users == 2 &&
                  context.state == BC_LINK_INVALID && context.cin_wait_exit == 1 &&
                  !pools && !rings && !hardware_opens && !irq_disables,
                  "busy admission preserves owner, unconfigured contender and resources");
            context.user[0].in_use = 0;
            Check(bc_cproc_notify_mode(&context, &data) == BC_STS_ERR_USAGE,
                  "legacy owner scan also blocks a stale playback or diagnostic mode");
            Check(!context.user[0].in_use && context.user[0].mode == owners[o] &&
                  context.user[1].mode == (uint32_t)DTS_MODE_INV &&
                  !pools && !rings,
                  "owner scan does not silently add an in-use filter during extraction");
        }
    }
    for (s = 0; s < sizeof(states) / sizeof(states[0]); s++) {
        for (r = 0; r < sizeof(requests) / sizeof(requests[0]); r++) {
            crystalhd_ioctl_data data = {0};

            Reset(states[s], true);
            context.user[0].in_use = 1;
            data.udata.u.NotifyMode.Mode = requests[r];
            Check(bc_cproc_notify_mode(&context, &data) == BC_STS_ERR_USAGE,
                  "non-invalid link state rejects decoder-mode admission");
            Check(context.user[0].mode == (uint32_t)DTS_MODE_INV &&
                  context.state == states[s] && context.cin_wait_exit == 1 &&
                  !pools && !rings,
                  "link-state rejection occurs before ownership or allocation changes");
        }
    }
    for (o = 0; o < sizeof(owners) / sizeof(owners[0]); o++) {
        crystalhd_ioctl_data data = { .u_id = 1 };

        Reset(BC_LINK_INVALID, true);
        context.user[1].in_use = 1;
        admission_pending = true;
        admission_uid = 1;
        admission_state = context.state;
        admission_wait = context.cin_wait_exit;
        data.udata.u.NotifyMode.Mode = owners[o];
        Check(bc_cproc_notify_mode(&context, &data) == BC_STS_SUCCESS,
              "playback and diagnostic modes with flags may acquire an idle session");
        Check(context.user[1].mode == owners[o] && !context.cin_wait_exit &&
              context.state == BC_LINK_INVALID && pools == 2 && rings == 1 &&
              elem_live && dio_live && rings_live,
              "successful decoder admission preserves the complete mode value");
    }
}
static void RepeatedMode(void)
{
    const uint32_t modes[] = { DTS_MONITOR_MODE, DTS_PLAYBACK_MODE, DTS_DIAG_MODE,
        DTS_HWINIT_MODE, 0x100 | DTS_MONITOR_MODE, 0x81000000 | DTS_PLAYBACK_MODE,
        0x100 | DTS_DIAG_MODE, 0x81000000 | DTS_HWINIT_MODE };
    unsigned old, next;

    for (old = 0; old < sizeof(modes) / sizeof(modes[0]); old++) {
        for (next = 0; next < sizeof(modes) / sizeof(modes[0]); next++) {
            crystalhd_ioctl_data data = {0};

            Reset(BC_LINK_INVALID, true);
            context.user[0].in_use = 1;
            context.user[0].mode = modes[old];
            data.udata.u.NotifyMode.Mode = modes[next];
            Check(bc_cproc_notify_mode(&context, &data) == BC_STS_ERR_USAGE,
                  "configured handles cannot repeat or change their mode");
            Check(context.user[0].mode == modes[old] && context.user[0].in_use &&
                  context.state == BC_LINK_INVALID && context.cin_wait_exit == 1 &&
                  !pools && !rings,
                  "repeat-mode rejection leaves the original mode and resources unchanged");
        }
    }
}
static void HwInitAdmission(void)
{
    const uint32_t modes[] = { DTS_HWINIT_MODE, 0x100 | DTS_HWINIT_MODE,
        0x81000000 | DTS_HWINIT_MODE };
    unsigned m;

    for (m = 0; m < sizeof(modes) / sizeof(modes[0]); m++) {
        crystalhd_ioctl_data data = {0};

        Reset(BC_LINK_INVALID, true);
        context.user[0].in_use = context.user[1].in_use = 1;
        admission_pending = true;
        admission_uid = 0;
        admission_state = context.state;
        admission_wait = context.cin_wait_exit;
        data.udata.u.NotifyMode.Mode = modes[m];
        Check(bc_cproc_notify_mode(&context, &data) == BC_STS_SUCCESS,
              "legacy HWINIT mode follows decoder resource setup");
        Check(context.user[0].mode == modes[m] &&
              context.session_owner == &context.user[0] && !context.cin_wait_exit &&
              context.state == BC_LINK_INVALID && pools == 2 && rings == 1 &&
              elem_live && dio_live && rings_live,
              "HWINIT keeps its flag bits and publishes only complete setup");

        /* HWINIT owns the same allocation set as playback and diagnostic. */
        admission_uid = 1;
        admission_wait = context.cin_wait_exit;
        data.u_id = 1;
        data.udata.u.NotifyMode.Mode = DTS_PLAYBACK_MODE;
        Check(bc_cproc_notify_mode(&context, &data) == BC_STS_ERR_USAGE,
              "HWINIT session rejects overlapping playback resource setup");
        Check(context.user[0].mode == modes[m] &&
              context.user[1].mode == (uint32_t)DTS_MODE_INV && pools == 2 && rings == 1,
              "HWINIT retains its resources without duplicate pool or ring allocation");
    }
}
static void OpenFailuresAndRetry(void)
{
    unsigned which;

    for (which = 0; which < 2; which++) {
        struct crystalhd_user sentinel = {0}, *user = &sentinel;

        Reset(BC_LINK_INVALID, false);
        context.pwr_state_change = BC_HW_SUSPEND;
        open_pending = true;
        hardware_alloc_fail = which == 0;
        hardware_open_status = which == 1 ? BC_STS_IO_ERROR : BC_STS_SUCCESS;
        Check(crystalhd_user_open(&context, &user) ==
              (which == 0 ? BC_STS_ERROR : BC_STS_IO_ERROR),
              "user open preserves allocation and hardware initialization error codes");
        Check(user == &sentinel && !bc_get_userhandle_count(&context) &&
              !adapter.cfg_users && !context.hw_ctx && !hardware_allocated &&
              context.user[0].mode == (uint32_t)DTS_MODE_INV &&
              context.pwr_state_change == BC_HW_SUSPEND,
              "failed user open publishes no slot, output handle, or power state");
        Check(hardware_alloc_attempts == 1 && hardware_allocations == which &&
              hardware_frees == which && hardware_opens == which &&
              !hardware_closes && irq_disables == 1 && irq_enables == 1 && !irq_depth,
              "failed open frees only allocated hardware and balances the IRQ");

        hardware_alloc_fail = false;
        hardware_open_status = BC_STS_SUCCESS;
        Check(crystalhd_user_open(&context, &user) == BC_STS_SUCCESS,
              "the same unclaimed user slot can retry open after failure");
        open_pending = false;
        Check(user == &context.user[0] && user->in_use &&
              user->mode == (uint32_t)DTS_MODE_INV &&
              context.hw_ctx == &hardware && hardware_allocated &&
              context.pwr_state_change == BC_HW_RUNNING && !adapter.cfg_users &&
              hardware_alloc_attempts == 2 && hardware_allocations == which + 1 &&
              hardware_opens == which + 1 && irq_disables == 2 && irq_enables == 2,
              "successful retry publishes a fresh user but leaves file accounting to caller");
        adapter.cfg_users++;
        crystalhd_user_close(&context, user);
        CheckNoSession();
    }
}
static void OpenSlotExhaustion(void)
{
    struct crystalhd_user sentinel = {0}, *user;
    unsigned n;

    Reset(BC_LINK_INVALID, false);
    for (n = 0; n < BC_LINK_MAX_OPENS; n++) {
        user = NULL;
        Check(crystalhd_user_open(&context, &user) == BC_STS_SUCCESS,
              "every available legacy user slot can open");
        Check(user == &context.user[n] && user->in_use &&
              user->mode == (uint32_t)DTS_MODE_INV && adapter.cfg_users == n &&
              hardware_allocations == 1 && hardware_opens == 1,
              "each open claims a distinct slot without reallocating shared hardware");
        adapter.cfg_users++;
    }
    user = &sentinel;
    Check(crystalhd_user_open(&context, &user) == BC_STS_BUSY,
          "exhausted legacy user slots reject another open as busy");
    Check(user == &sentinel && adapter.cfg_users == BC_LINK_MAX_OPENS &&
          bc_get_userhandle_count(&context) == BC_LINK_MAX_OPENS &&
          hardware_allocations == 1 && hardware_opens == 1 && !hardware_frees &&
          irq_disables == 1 && irq_enables == 1,
          "slot exhaustion changes no output handle, accounting or hardware state");
    for (n = 0; n < BC_LINK_MAX_OPENS; n++)
        crystalhd_user_close(&context, &context.user[n]);
    CheckNoSession();
}
static void InvalidAdmissionArguments(void)
{
    crystalhd_ioctl_data data = {0};
    struct crystalhd_user sentinel = {0}, *user = &sentinel;

    Reset(BC_LINK_INVALID, false);
    Check(bc_cproc_notify_mode(NULL, &data) == BC_STS_INV_ARG &&
          bc_cproc_notify_mode(&context, NULL) == BC_STS_INV_ARG &&
          bc_cproc_notify_mode(NULL, NULL) == BC_STS_INV_ARG,
          "legacy notify adapter rejects NULL context or ioctl data");
    Check(crystalhd_user_set_mode(NULL, &context.user[0], DTS_PLAYBACK_MODE) == BC_STS_INV_ARG &&
          crystalhd_user_set_mode(&context, NULL, DTS_PLAYBACK_MODE) == BC_STS_INV_ARG &&
          crystalhd_user_set_mode(NULL, NULL, DTS_PLAYBACK_MODE) == BC_STS_INV_ARG,
          "shared mode admission rejects NULL context or user");
    Check(crystalhd_user_open(NULL, &user) == BC_STS_INV_ARG &&
          crystalhd_user_open(&context, NULL) == BC_STS_INV_ARG &&
          crystalhd_user_open(NULL, NULL) == BC_STS_INV_ARG,
          "user open rejects NULL context or output pointer");
    Check(user == &sentinel && !bc_get_userhandle_count(&context) &&
          !adapter.cfg_users && !context.hw_ctx && !hardware_alloc_attempts &&
          !pools && !rings && !irq_disables && !irq_enables,
          "invalid admission arguments do not alter user or device resources");
}
static void Idle(void)
{
    crystalhd_ioctl_data data = {0};
    unsigned cycle;
    Reset(BC_LINK_INVALID, false);
    for (cycle = 0; cycle < 4; cycle++) {
        Check(crystalhd_suspend(&context, &data) == BC_STS_SUCCESS, "idle suspend succeeds");
        Check(crystalhd_resume(&context) == BC_STS_SUCCESS, "idle NULL-context resume succeeds");
        Check(context.state == BC_LINK_INVALID && context.pwr_state_change == BC_HW_RUNNING,
              "idle resume remains eligible for first playback");
        Check(!starts && !stops && !captures && !cancels && !tx_stops,
              "idle lifecycle performs no decoder hardware operation");
    }
    CheckNotify();
}
static void IdleMonitor(void)
{
    crystalhd_ioctl_data data = {0};
    unsigned cycle;
    Reset(BC_LINK_INVALID, true);
    context.user[0].in_use = 1; context.user[0].mode = DTS_MONITOR_MODE;
    for (cycle = 0; cycle < 4; cycle++) {
        Check(crystalhd_suspend(&context, &data) == BC_STS_SUCCESS, "monitor-only suspend succeeds");
        Check(crystalhd_resume(&context) == BC_STS_SUCCESS, "monitor hardware resume succeeds");
        Check(starts == cycle + 1 && !stops && !tx_stops, "initialized monitor hardware is restarted");
        Check(context.state == BC_LINK_INVALID && context.pwr_state_change == BC_HW_RUNNING,
              "monitor resume does not fabricate an active playback session");
    }
    CheckNotify();
}
static void Unconfigured(void)
{
    crystalhd_ioctl_data data = {0};
    Reset(BC_LINK_INVALID, true);
    context.user[0].in_use = 1;
    Check(crystalhd_suspend(&context, &data) == BC_STS_SUCCESS, "unconfigured suspend succeeds");
    Check(crystalhd_resume(&context) == BC_STS_SUCCESS, "unconfigured hardware resume succeeds");
    Check(starts == 1 && !stops && !tx_stops && context.state == BC_LINK_INVALID &&
          context.user[0].mode == (uint32_t)DTS_MODE_INV &&
          context.pwr_state_change == BC_HW_RUNNING, "unconfigured mode and admission remain unchanged");
    CheckNotify();
}
static void BeforeFirmware(void)
{
    uint8_t firmware = 0x5a;
    crystalhd_ioctl_data data = { .u_id = 1, .add_cdata = &firmware, .add_cdata_sz = 1 };
    unsigned cycle;
    Reset(BC_LINK_INVALID, true);
    CheckNotify();
    for (cycle = 0; cycle < 4; cycle++) {
        Check(crystalhd_suspend(&context, &data) == BC_STS_SUCCESS, "pre-firmware suspend succeeds");
        Check(crystalhd_resume(&context) == BC_STS_SUCCESS, "pre-firmware resume succeeds");
        Check(context.state == BC_LINK_INVALID && context.user[1].mode == DTS_PLAYBACK_MODE &&
              context.pwr_state_change == BC_HW_RUNNING && starts == cycle + 1 && !stops,
              "pre-firmware playback retains its configured owner and idle state");
    }
    Check(bc_cproc_download_fw(&context, &data) == BC_STS_SUCCESS,
          "actual firmware admission accepts resumed pre-firmware playback");
    Check(downloads == 1 && context.state == BC_LINK_INIT && !hardware.FwCmdCnt &&
          context.pwr_state_change == BC_HW_RUNNING, "firmware admission publishes initialized state");
}
static void NullResume(void)
{
    Reset(BC_LINK_INVALID, false);
    Check(crystalhd_resume(NULL) == BC_STS_INV_ARG, "NULL resume context is rejected");
    Check(!starts, "NULL resume has no hardware effects");
}
static void InvalidSuspend(void)
{
    crystalhd_ioctl_data data = {0};
    Reset(BC_LINK_INIT, true);
    Check(crystalhd_suspend(NULL, &data) == BC_STS_ERROR, "NULL suspend context is rejected");
    Check(crystalhd_suspend(&context, NULL) == BC_STS_ERROR, "NULL suspend request is rejected");
    Check(!starts && !stops && !tx_stops, "invalid suspend has no hardware effects");
}
static void MissingHardware(void)
{
    const uint32_t states[] = { BC_LINK_INIT, BC_LINK_READY, BC_LINK_SUSPEND, BC_LINK_RESUME };
    crystalhd_ioctl_data data = {0};
    unsigned n;
    for (n = 0; n < sizeof(states) / sizeof(states[0]); n++) {
        struct crystalhd_cmd before;
        Reset(states[n], false);
        context.user[0].in_use = 1; context.user[0].mode = DTS_PLAYBACK_MODE;
        before = context;
        Check(crystalhd_suspend(&context, &data) == BC_STS_INV_ARG,
              "non-idle suspend requires a hardware context");
        Check(!memcmp(&before, &context, sizeof(context)), "bad suspend leaves command state unchanged");
        Check(crystalhd_resume(&context) == BC_STS_INV_ARG,
              "non-idle resume cannot silently accept missing hardware");
        Check(!memcmp(&before, &context, sizeof(context)), "bad resume leaves command state unchanged");
        Check(!starts && !stops && !tx_stops && !captures && !cancels,
              "inconsistent context never reaches hardware");
    }
}
static void Active(void)
{
    crystalhd_ioctl_data data = {0};
    unsigned active_tx, mode;
    for (active_tx = 0; active_tx < 2; active_tx++) for (mode = 0; mode < 2; mode++) {
        Reset(BC_LINK_READY, true);
        context.tx_list_id = active_tx ? 7 : 0;
        context.user[2].in_use = 1;
        context.user[2].mode = 0x100 | (mode ? DTS_DIAG_MODE : DTS_PLAYBACK_MODE);
        Check(crystalhd_suspend(&context, &data) == BC_STS_SUCCESS, "active suspend succeeds");
        Check(!strcmp(events, active_tx ? "CXS" : "CTS"), "capture and TX stop precede device suspend");
        Check(context.state == BC_LINK_SUSPEND && context.pwr_state_change == BC_HW_SUSPEND,
              "active suspend reports its power state");
        Check(crystalhd_suspend(&context, &data) == BC_STS_SUCCESS, "duplicate suspend is harmless");
        Check(stops == 1 && captures == 1, "duplicate suspend does not stop hardware twice");
        Check(crystalhd_resume(&context) == BC_STS_SUCCESS, "active resume succeeds");
        Check(context.state == BC_LINK_RESUME && context.pwr_state_change == BC_HW_RESUME,
              "active resume retains the userspace reopen contract");
        Check(starts == 1 && hardware.rx_list_sts[0] == sts_free &&
              hardware.rx_list_sts[1] == sts_free && hardware.TxList0Sts == ListStsFree &&
              hardware.TxList1Sts == ListStsFree && !hardware.rx_list_post_index &&
              !hardware.tx_list_post_index, "actual hardware resume resets DMA list bookkeeping");
    }
}
static void ResumeErrors(void)
{
    unsigned idle, fault;
    for (idle = 0; idle < 2; idle++) for (fault = 0; fault < 2; fault++) {
        struct crystalhd_cmd before;
        Reset(idle ? BC_LINK_INVALID : BC_LINK_SUSPEND, true);
        context.pwr_state_change = idle ? BC_HW_RUNNING : BC_HW_SUSPEND;
        context.user[0].in_use = 1;
        context.user[0].mode = idle ? DTS_MONITOR_MODE : DTS_PLAYBACK_MODE;
        before = context;
        hardware.dma_fault = fault != 0; start_ok = false;
        Check(crystalhd_resume(&context) == (fault ? BC_STS_IO_ERROR : BC_STS_ERROR),
              "DMA-fault/device-start failures propagate for active and monitor contexts");
        Check(!memcmp(&before, &context, sizeof(context)), "resume failure does not publish success state");
        Check(starts == (fault ? 0U : 1U), "DMA fault prevents hardware restart");
    }
}
static void SuspendErrors(void)
{
    crystalhd_ioctl_data data = {0};
    unsigned which;
    for (which = 0; which < 3; which++) {
        Reset(BC_LINK_READY, true);
        context.user[0].in_use = 1; context.user[0].mode = DTS_PLAYBACK_MODE;
        context.tx_list_id = 7;
        if (which == 0) capture_status = BC_STS_TIMEOUT;
        if (which == 1) cancel_status = BC_STS_IO_ERROR;
        if (which == 2) stop_ok = false;
        Check(crystalhd_suspend(&context, &data) ==
              (which == 0 ? BC_STS_TIMEOUT : which == 1 ? BC_STS_IO_ERROR : BC_STS_ERROR),
              "existing capture/TX/device suspend failures remain errors");
        Check(!strcmp(events, which == 0 ? "C" : which == 1 ? "CX" : "CXS"),
              "suspend failure stops later callbacks");
        Check(context.pwr_state_change == BC_HW_SUSPEND, "active failure retains existing cancellation notification");
    }
}
int main(void)
{
    const struct rlimit no_core = {0, 0};
    const struct { const char *name; void (*run)(void); } cases[] = {
        {"idle first/recent-session resume and actual playback admission", Idle},
        {"monitor-only resume and actual playback admission", IdleMonitor},
        {"unconfigured handle and actual playback admission", Unconfigured},
        {"playback before firmware and actual firmware admission", BeforeFirmware},
        {"playback session setup failure rollback and retry", NotifyFailures},
        {"monitor admission during active decoder states and mode flags", MonitorAdmission},
        {"decoder ownership and link-state admission matrix", NotifyAdmissionMatrix},
        {"repeated and changed mode rejection", RepeatedMode},
        {"HWINIT admission rejects overlapping resource setup", HwInitAdmission},
        {"explicit resource ownership across legacy modes", ResourceOwnership},
        {"pending unconfigured handle after resource owner close", PendingOpenAfterOwnerClose},
        {"raw command invalid arguments and retired hardware", RawInvalidArguments},
        {"raw device and link register read/write propagation", RawRegisterCommands},
        {"raw memory bounds, read/write propagation and callback errors", RawMemoryCommands},
        {"surviving monitor raw commands after actual owner close", MonitorRawAfterOwnerClose},
        {"user open failure rollback and retry", OpenFailuresAndRetry},
        {"legacy user slot exhaustion", OpenSlotExhaustion},
        {"invalid admission adapter arguments", InvalidAdmissionArguments},
        {"firmware pause/resume rollback", FirmwarePauseRollback},
        {"firmware download serialization", FirmwareDownloadSerialization},
        {"firmware transaction serialization", FirmwareTransactionSerialization},
        {"firmware command versus hardware suspend", FirmwareSuspendSerialization},
        {"active session open, busy, release and reopen", SessionOwnership},
        {"last monitor releases empty session resources", MonitorOnlyRelease},
        {"playback owner closes before monitor and reacquires", OwnerBeforeMonitor},
        {"actual file close across unconfigured, monitor, playback and diagnostic modes", FileCloseModes},
        {"actual file close owner/monitor ordering and reacquisition", FileOwnerBeforeMonitor},
        {"RELEASE then actual file close", ReleaseThenFileClose},
        {"actual file close after session setup failure", FileCloseAfterSetupFailure},
        {"existing close behavior after capture-stop failure", CloseCaptureFailure},
        {"actual file close after failed PM or stale device binding", FileCloseUnavailableDevice},
        {"owner close without hardware and repeated close", OwnerCloseWithoutHardware},
        {"NULL resume argument", NullResume}, {"invalid suspend arguments", InvalidSuspend},
        {"inconsistent non-idle NULL hardware", MissingHardware},
        {"active playback/diagnostic suspend and resume", Active},
        {"hardware resume errors", ResumeErrors}, {"existing suspend errors", SuspendErrors}
    };
    unsigned n, failed_groups = 0;
    if (setrlimit(RLIMIT_CORE, &no_core)) return 2;
    for (n = 0; n < sizeof(cases) / sizeof(cases[0]); n++) {
        int status;
        pid_t child;
        fflush(NULL); child = fork();
        if (child < 0) return 2;
        if (!child) {
            alarm(10); cases[n].run();
            printf("%s: %u checks, %u failures\n", cases[n].name, checks, failures);
            exit(failures ? 1 : 0);
        }
        if (waitpid(child, &status, 0) != child) return 2;
        if (!WIFEXITED(status) || WEXITSTATUS(status)) {
            failed_groups++;
            fprintf(stderr, "FAIL group %s: %s %d\n", cases[n].name,
                WIFSIGNALED(status) ? "signal" : "exit",
                WIFSIGNALED(status) ? WTERMSIG(status) : WEXITSTATUS(status));
        }
    }
    printf("Command PM: %zu isolated groups, %u failures (no hardware)\n",
        sizeof(cases) / sizeof(cases[0]), failed_groups);
    return failed_groups ? 1 : 0;
}
