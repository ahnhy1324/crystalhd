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
#define dev_info(dev, ...) ((void)(dev))
#define dev_dbg(dev, ...) ((void)(dev))
#define eCMD_C011_CMD_BASE 0x73763000U
#define eCMD_C011_DEC_CHAN_FLUSH (eCMD_C011_CMD_BASE + 0x104U)
#define eCMD_C011_DEC_CHAN_PAUSE (eCMD_C011_CMD_BASE + 0x11dU)
struct device { int unused; };
struct pci_dev { struct device dev; int irq; };
struct crystalhd_adp { struct pci_dev *pdev; unsigned cfg_users; };
typedef struct {
    uint32_t cmd[64];
    uint32_t rsp[64];
    uint32_t flags, add_data;
} BC_FW_CMD;
struct crystalhd_hw {
    struct crystalhd_adp *adp;
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
    uint32_t tx_list_id, cin_wait_exit, pwr_state_change;
    struct crystalhd_hw *hw_ctx;
};
typedef struct {
    uint32_t u_id;
    struct { union {
        struct { uint32_t Mode; } NotifyMode;
        BC_FW_CMD fwCmd;
    } u; } udata;
    void *add_cdata;
    uint32_t add_cdata_sz;
} crystalhd_ioctl_data;

static unsigned checks, failures, starts, stops, tx_stops, cancels, captures, pools, rings;
static unsigned elem_deletes, dio_destroys, ring_frees, hardware_opens, hardware_closes;
static unsigned irq_depth, irq_disables, irq_enables, capture_unmaps;
static unsigned downloads;
static bool start_ok, stop_ok;
static bool elem_live, dio_live, rings_live, hardware_allocated;
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
static struct crystalhd_cmd context;
static bool Start(struct crystalhd_hw *hw);
static bool Stop(struct crystalhd_hw *hw);
static BC_STATUS StopTx(struct crystalhd_hw *hw);
static BC_STATUS Download(struct crystalhd_hw *hw, uint8_t *data, uint32_t size);
static BC_STATUS IssuePause(struct crystalhd_hw *hw, bool state);
static BC_STATUS FirmwareCommand(struct crystalhd_hw *hw, BC_FW_CMD *command);
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
static void ConfigureHardware(struct crystalhd_hw *hw)
{
    *hw = (struct crystalhd_hw){ .adp = &adapter, .pfnStartDevice = Start,
        .pfnStopDevice = Stop, .pfnStopTxDMA = StopTx, .pfnFWDwnld = Download,
        .pfnIssuePause = IssuePause, .pfnDoFirmwareCmd = FirmwareCommand,
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
    hardware_allocated = true;
    return &hardware;
}
static void kfree(void *memory)
{
    Check(memory == &hardware && hardware_allocated,
          "session teardown frees its hardware context exactly once");
    hardware_allocated = false;
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
static int crystalhd_create_elem_pool(struct crystalhd_adp *adp, unsigned size)
{
    Check(adp == &adapter && size == BC_LINK_ELEM_POOL_SZ, "notify allocates element pool");
    pools++; elem_live = true; return elem_error;
}
static void crystalhd_delete_elem_pool(struct crystalhd_adp *adp)
{
    Check(adp == &adapter, "element-pool teardown receives the adapter");
    elem_deletes++; elem_live = false;
}
static int crystalhd_create_dio_pool(struct crystalhd_adp *adp, unsigned size)
{
    Check(adp == &adapter && size == BC_LINK_MAX_SGLS, "notify allocates DMA pool");
    pools++;
    if (dio_error) return dio_error;
    dio_live = true;
    return 0;
}
static void crystalhd_destroy_dio_pool(struct crystalhd_adp *adp)
{
    Check(adp == &adapter, "DMA-pool teardown receives the adapter");
    dio_destroys++; dio_live = false;
}
static BC_STATUS crystalhd_hw_setup_dma_rings(struct crystalhd_hw *hw)
{
    Check(hw == &hardware, "notify uses the initialized hardware context");
    rings++;
    rings_live = ring_status == BC_STS_SUCCESS;
    return ring_status;
}
static BC_STATUS crystalhd_hw_free_dma_rings(struct crystalhd_hw *hw)
{
    Check(hw == &hardware, "DMA-ring teardown receives the hardware context");
    ring_frees++; rings_live = false;
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

static void Reset(uint32_t state, bool with_hardware)
{
    unsigned n;
    starts = stops = tx_stops = cancels = captures = pools = rings = 0;
    elem_deletes = dio_destroys = ring_frees = hardware_opens = hardware_closes = 0;
    irq_depth = irq_disables = irq_enables = capture_unmaps = 0;
    downloads = 0;
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
    adapter.cfg_users = 0;
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
    unsigned which;

    data.udata.u.NotifyMode.Mode = DTS_PLAYBACK_MODE;
    for (which = 0; which < 3; which++) {
        BC_STATUS expected = which == 2 ? BC_STS_INSUFF_RES : BC_STS_ERROR;

        Reset(BC_LINK_INVALID, true);
        context.user[1].in_use = 1;
        if (which == 0) elem_error = -1;
        if (which == 1) dio_error = -1;
        if (which == 2) ring_status = BC_STS_INSUFF_RES;
        Check(bc_cproc_notify_mode(&context, &data) == expected,
              "notify-mode propagates each session setup failure");
        Check(context.user[1].mode == DTS_MODE_INV && context.cin_wait_exit == 1,
              "failed setup does not publish playback ownership");
        Check(!elem_live && !dio_live && !rings_live,
              "failed setup leaves no session allocation live");
        Check(elem_deletes == 1 && dio_destroys == (which == 2) && !ring_frees,
              "failed setup releases every successfully created pool");

        elem_error = dio_error = 0;
        ring_status = BC_STS_SUCCESS;
        Check(bc_cproc_notify_mode(&context, &data) == BC_STS_SUCCESS,
              "the same handle can retry session setup after failure");
        Check(context.user[1].mode == DTS_PLAYBACK_MODE && !context.cin_wait_exit &&
              elem_live && dio_live && rings_live,
              "successful retry commits one complete playback session");
    }
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
        {"firmware pause/resume rollback", FirmwarePauseRollback},
        {"firmware download serialization", FirmwareDownloadSerialization},
        {"firmware transaction serialization", FirmwareTransactionSerialization},
        {"firmware command versus hardware suspend", FirmwareSuspendSerialization},
        {"active session open, busy, release and reopen", SessionOwnership},
        {"last monitor releases empty session resources", MonitorOnlyRelease},
        {"playback owner closes before monitor and reacquires", OwnerBeforeMonitor},
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
