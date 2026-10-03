/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Exact command/hardware PM and notify-mode functions; no device is opened.
 * Isolate failures so the original idle error and NULL dereferences are safe.
 */
#include <stdbool.h>
#include <stdint.h>
#include <errno.h>
#include <pthread.h>
#include <stddef.h>
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
#include "crystalhd_ioctl_limits.h"

#ifndef ERESTARTSYS
#define ERESTARTSYS 512
#endif

#define KERN_ERR ""
#define GFP_KERNEL 0
#define READ_ONCE(value) (value)
#define WRITE_ONCE(value, update) ((value) = (update))
#define BUILD_BUG_ON(condition) _Static_assert(!(condition), "BUILD_BUG_ON")
#define printk(...) ((void)0)
#define dev_err(dev, ...) ((void)(dev))
#define dev_info(dev, ...) do { (void)(dev); if (false) fprintf(stderr, __VA_ARGS__); } while (0)
#define dev_dbg(dev, ...) ((void)(dev))
#define lockdep_assert_held(lock) \
    Check((lock) == &adapter.user_lock && *(lock) == 1, \
          "firmware execution retains shared user admission")
#define lockdep_assert_held_write(lock) \
    Check((lock) == &adapter.user_lock && *(lock) == 1, \
          "firmware loading retains exclusive user admission")
#define spin_lock_irqsave(spin, flags) do { \
    Check(((spin) == &hardware.lock || (spin) == &adapter.dram_lock) && !*(spin), \
          "register access acquires its hardware or DRAM-window lock"); \
    *(spin) = 1; (flags) = 0; \
} while (0)
#define spin_unlock_irqrestore(spin, flags) do { \
    Check(((spin) == &hardware.lock || (spin) == &adapter.dram_lock) && *(spin) && !(flags), \
          "register access releases its hardware or DRAM-window lock"); \
    *(spin) = 0; \
} while (0)
struct device { int unused; };
struct firmware {
    size_t size;
    const uint8_t *data;
};
struct pci_dev { struct device dev; int irq; uint32_t device; };
struct crystalhd_adp;
typedef struct {
    uint32_t cmd[64];
    uint32_t rsp[64];
    uint32_t flags, add_data;
} BC_FW_CMD;
struct crystalhd_hw {
    struct crystalhd_adp *adp;
    void *rx_freeq;
    int lock;
    bool dma_fault, dev_started;
    enum list_sts rx_list_sts[2];
    enum LIST_STATUS TxList0Sts, TxList1Sts;
    uint32_t rx_list_post_index, tx_list_post_index;
    bool (*pfnStartDevice)(struct crystalhd_hw *);
    bool (*pfnStopDevice)(struct crystalhd_hw *);
    bool (*pfnFindAndClearIntr)(struct crystalhd_adp *, struct crystalhd_hw *);
    BC_STATUS (*pfnStopTxDMA)(struct crystalhd_hw *);
    BC_STATUS (*pfnFWDwnld)(struct crystalhd_hw *, const uint8_t *, uint32_t);
    BC_STATUS (*pfnIssuePause)(struct crystalhd_hw *, bool);
    BC_STATUS (*pfnDoFirmwareCmd)(struct crystalhd_hw *, BC_FW_CMD *);
    uint32_t (*pfnReadDevRegister)(struct crystalhd_adp *, uint32_t);
    void (*pfnWriteDevRegister)(struct crystalhd_adp *, uint32_t, uint32_t);
    uint32_t (*pfnReadFPGARegister)(struct crystalhd_adp *, uint32_t);
    void (*pfnWriteFPGARegister)(struct crystalhd_adp *, uint32_t, uint32_t);
    BC_STATUS (*pfnDevDRAMRead)(struct crystalhd_hw *, uint32_t, uint32_t, uint32_t *);
    BC_STATUS (*pfnDevDRAMWrite)(struct crystalhd_hw *, uint32_t, uint32_t,
                                 const uint32_t *);
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
    const void *session_owner;
    const void *session_lifetime_owner;
    const struct crystalhd_session_owner_ops *session_lifetime_ops;
    bool retain_rx_on_suspend;
    bool session_module_pinned;
    enum crystalhd_decoder_phase decoder_phase;
    enum crystalhd_decoder_codec decoder_codec;
    uint32_t fw_sequence, decoder_channel_id;
    void *stream;
    uint32_t tx_list_id, cin_wait_exit, pwr_state_change;
    struct crystalhd_hw *hw_ctx;
};
struct crystalhd_adp {
    struct pci_dev *pdev;
    unsigned cfg_users;
    bool present;
    bool dma_terminal_quiesced;
    void *fill_byte_pool, *elem_pool_head;
    int user_lock, dram_lock;
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
static unsigned downloads, download_resets;
static unsigned stream_prepares, stream_releases;
static unsigned quiesced_rx_retires;
static unsigned quiesced_tx_retires, retained_tx_puts;
static bool retained_tx_lease;
static unsigned fatal_master_clears, fatal_pending_waits;
static unsigned fatal_chip_masks;
static bool fatal_pending;
static unsigned normal_interrupts, fault_interrupts;
static bool interrupt_handled;
static unsigned module_refs, module_get_attempts, module_gets, module_puts;
static unsigned module_callbacks, module_identity;
static bool module_get_allowed, checking_module_callbacks;
static bool module_expect_no_hardware_on_put;
struct frontend_owner {
    unsigned refs, gets, retired, puts;
};
static struct frontend_owner frontend_owners[2];
static unsigned frontend_lifetime_step;
#define THIS_MODULE (&module_identity)
static int stream_prepare_error;
static uint8_t stream_cookie;
static const uint8_t *last_download_image;
static uint32_t last_download_size;
static unsigned kernel_fw_requests, kernel_fw_releases;
static int kernel_fw_request_error;
static const char *last_kernel_fw_name;
static const struct device *last_kernel_fw_device;
static const struct firmware *last_released_firmware;
static bool remove_during_fw_request;
static _Alignas(uint32_t)
    uint8_t kernel_fw_image[CRYSTALHD_LINK_MIN_FIRMWARE_SIZE];
static struct firmware kernel_fw_blob = {
    .size = CRYSTALHD_FLEA_MIN_FIRMWARE_SIZE,
    .data = kernel_fw_image,
};
static unsigned raw_calls[6];
static unsigned raw_dram_writes;
static uint32_t raw_offset, raw_value, raw_words, raw_memory[2];
static uint32_t *raw_buffer;
static BC_STATUS raw_status;
static bool start_ok, stop_ok;
static bool elem_live, dio_live, rings_live, hardware_allocated;
static bool hardware_alloc_fail, admission_pending, open_pending;
static bool expect_retire_irq;
static unsigned admission_uid;
static uint32_t admission_state, admission_wait;
static int elem_error, dio_error;
static BC_STATUS ring_status, hardware_open_status;
static BC_STATUS capture_status, cancel_status, download_status;
static BC_STATUS pause_status, firmware_status;
static unsigned pause_calls, firmware_calls;
static BC_FW_CMD *last_firmware_command;
static BC_FW_CMD firmware_command_snapshot;
static BC_FW_CMD firmware_command_snapshots[8];
static unsigned firmware_status_call, remove_during_firmware_call;
static BC_STATUS firmware_call_status;
static uint32_t firmware_open_channel;
static bool remove_during_fw_command, poison_on_firmware_timeout;
static bool pause_states[4];
static pthread_mutex_t transaction_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t transaction_audit = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t transaction_changed = PTHREAD_COND_INITIALIZER;
static bool transaction_mode, block_first_firmware;
static bool first_firmware_waiting, release_first_firmware;
static bool block_download_reset, download_reset_waiting, release_download_reset;
static unsigned transaction_attempts;
static char events[32];
static struct pci_dev endpoint = { .irq = 19, .device = BC_PCI_DEVID_FLEA };
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
static BC_STATUS Download(struct crystalhd_hw *hw, const uint8_t *data,
                          uint32_t size);
static int request_firmware(const struct firmware **firmware, const char *name,
                            struct device *device);
static void release_firmware(const struct firmware *firmware);
static BC_STATUS IssuePause(struct crystalhd_hw *hw, bool state);
static BC_STATUS FirmwareCommand(struct crystalhd_hw *hw, BC_FW_CMD *command);
static uint32_t ReadDevice(struct crystalhd_adp *adp, uint32_t offset);
static void WriteDevice(struct crystalhd_adp *adp, uint32_t offset, uint32_t value);
static uint32_t ReadLink(struct crystalhd_adp *adp, uint32_t offset);
static void WriteLink(struct crystalhd_adp *adp, uint32_t offset, uint32_t value);
static BC_STATUS ReadMemory(struct crystalhd_hw *hw, uint32_t offset, uint32_t words, uint32_t *buffer);
static BC_STATUS WriteMemory(struct crystalhd_hw *hw, uint32_t offset,
                             uint32_t words, const uint32_t *buffer);
static void CheckPendingAdmission(void);
BC_STATUS crystalhd_hw_suspend(struct crystalhd_hw *hw);
BC_STATUS crystalhd_hw_close_actual(struct crystalhd_hw *hw);
void crystalhd_hw_dma_fatal_stop(struct crystalhd_hw *hw);
static void Check(bool ok, const char *why)
{
    checks++;
    if (!ok) { failures++; fprintf(stderr, "FAIL: %s\n", why); }
}
static bool try_module_get(const void *module)
{
    Check(module == THIS_MODULE && !module_refs && !context.session_module_pinned &&
          !context.session_owner && context.state == BC_LINK_INVALID,
          "session pre-pin uses the exact module only after ownership and state admission");
    module_get_attempts++;
    if (!module_get_allowed)
        return false;
    module_refs++;
    module_gets++;
    return true;
}
static void module_put(const void *module)
{
    Check(module == THIS_MODULE && module_refs == 1 &&
          !context.session_module_pinned && !context.session_owner &&
          !context.retain_rx_on_suspend && !elem_live && !dio_live && !rings_live,
          "module put clears pin and ownership only after session resources are retired");
    if (frontend_lifetime_step) {
        Check(frontend_lifetime_step == 3 && !context.session_lifetime_owner &&
              !context.session_lifetime_ops,
              "module unpin follows owner notification and the last core frontend put");
        frontend_lifetime_step = 4;
    }
    if (module_expect_no_hardware_on_put)
        Check(!context.hw_ctx && !hardware_allocated && !context.stream &&
              context.decoder_phase == CRYSTALHD_DECODER_COLD &&
              context.decoder_codec == CRYSTALHD_DECODER_CODEC_INVALID,
              "final module put follows hardware, staging and decoder-owner teardown");
    if (module_refs)
        module_refs--;
    module_puts++;
}
static void ModuleCallback(void)
{
    if (!checking_module_callbacks)
        return;
    Check(context.session_module_pinned && module_refs == 1,
          "allocation and cleanup callbacks execute while the session module pin is held");
    module_callbacks++;
}
static struct frontend_owner *FrontendOwner(const void *owner)
{
    Check(owner == &frontend_owners[0] || owner == &frontend_owners[1],
          "frontend callbacks use the exact independently owned identity");
    Check(adapter.user_lock == 1,
          "frontend callbacks retain the caller's exclusive session barrier");
    return (struct frontend_owner *)owner;
}
static void FrontendGet(const void *owner)
{
    struct frontend_owner *frontend = FrontendOwner(owner);

    Check(frontend->refs && !frontend->gets && !frontend->retired &&
          !frontend->puts && !frontend_lifetime_step &&
          context.session_module_pinned && module_refs == 1 && !module_puts,
          "frontend get adopts exactly one reference while the caller and module are live");
    Check(!context.session_owner || context.session_owner == owner,
          "frontend adoption covers the exact published owner or retained unpublished setup");
    frontend->refs++;
    frontend->gets++;
    frontend_lifetime_step = 1;
}
static void FrontendRetired(const void *owner)
{
    struct frontend_owner *frontend = FrontendOwner(owner);

    Check(frontend->refs && frontend->gets == 1 && !frontend->retired &&
          !frontend->puts && frontend_lifetime_step == 1,
          "retirement notifies the still-referenced frontend exactly once");
    Check(!context.session_owner && !context.session_lifetime_owner &&
          !context.session_lifetime_ops && !context.hw_ctx && !hardware_allocated &&
          !context.stream && !elem_live && !dio_live && !rings_live &&
          !retained_tx_lease && !context.retain_rx_on_suspend &&
          context.state == BC_LINK_INVALID &&
          context.decoder_phase == CRYSTALHD_DECODER_COLD &&
          context.decoder_codec == CRYSTALHD_DECODER_CODEC_INVALID,
          "retirement clears all core owner identities and resources before frontend notification");
    Check(context.session_module_pinned && module_refs == 1 && !module_puts,
          "retirement notification still executes under the original module pin");
    frontend->retired++;
    frontend_lifetime_step = 2;
}
static void FrontendPut(const void *owner)
{
    struct frontend_owner *frontend = FrontendOwner(owner);

    Check(frontend->refs && frontend->retired == 1 && !frontend->puts &&
          frontend_lifetime_step == 2 && context.session_module_pinned &&
          module_refs == 1 && !module_puts && !context.session_lifetime_owner &&
          !context.session_lifetime_ops,
          "frontend put follows cleared-identity retirement before module unpin");
    frontend->refs--;
    frontend->puts++;
    frontend_lifetime_step = 3;
}
static const struct crystalhd_session_owner_ops frontend_ops = {
    .get = FrontendGet, .retired = FrontendRetired, .put = FrontendPut,
};
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
    *hw = (struct crystalhd_hw){ .adp = &adapter, .dev_started = true, .pfnStartDevice = Start,
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
    ModuleCallback();
    CheckPendingAdmission();
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
static void *kzalloc(size_t size, int flags)
{
    void *memory = kmalloc(size, flags);

    if (memory)
        memset(memory, 0, size);
    return memory;
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
    ModuleCallback();
    hardware_allocated = false;
    hardware_frees++;
}
static void disable_irq(int irq)
{
    Check(irq == endpoint.irq && irq_depth < 2,
          "device stop may nest one IRQ exclusion inside session retirement");
    if (expect_retire_irq)
        Check(context.cin_wait_exit == 1 &&
              context.pwr_state_change == BC_HW_RUNNING,
              "session cancellation is published before IRQ quiescence");
    irq_depth++; irq_disables++;
}
static void enable_irq(int irq)
{
    Check(irq == endpoint.irq && irq_depth >= 1 && irq_depth <= 2,
          "device stop and session retirement each balance their IRQ exclusion");
    irq_depth--; irq_enables++;
}
static BC_STATUS crystalhd_hw_open(struct crystalhd_hw *hw, struct crystalhd_adp *adp)
{
    static const struct crystalhd_hw zero_hardware;

    ModuleCallback();
    CheckPendingAdmission();
    Check(hw == &hardware && adp == &adapter,
          "user open initializes the allocated hardware context");
    Check(!memcmp(hw, &zero_hardware, sizeof(*hw)),
          "hardware opening receives zeroed allocation before callback initialization");
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
static BC_STATUS crystalhd_hw_close(struct crystalhd_hw *hw)
{
    unsigned previous_stops = stops;
    bool was_started = hw->dev_started;
    bool was_faulted = hw->dma_fault;
    BC_STATUS status;

    ModuleCallback();
    Check(hw == &hardware && hw->adp == &adapter,
          "session release closes the owned hardware context");
    hardware_closes++;
    last_close_cfg_users = adapter.cfg_users;
    status = crystalhd_hw_close_actual(hw);
    Check(stops == previous_stops + (was_started && !was_faulted),
          "real hardware close attempts only the eligible owned device stop");
    Check(status != BC_STS_SUCCESS || !hw->dev_started,
          "successful hardware close retires the started device");
    return status;
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
static void pci_clear_master(struct pci_dev *pdev)
{
    Check(pdev == &endpoint, "fatal stop targets the current PCI device");
    fatal_master_clears++;
}
static void crystalhd_flea_disable_interrupts(struct crystalhd_hw *hw)
{
    Check(hw == &hardware && endpoint.device == BC_PCI_DEVID_FLEA &&
          hw->dma_fault && !adapter.present && context.cin_wait_exit,
          "fatal Flea stop masks only its device after publishing cancellation");
    fatal_chip_masks++;
}
static void crystalhd_link_disable_interrupts(struct crystalhd_hw *hw)
{
    Check(hw == &hardware && endpoint.device == BC_PCI_DEVID_LINK &&
          hw->dma_fault && !adapter.present && context.cin_wait_exit,
          "fatal Link stop masks only its device after publishing cancellation");
    fatal_chip_masks++;
}
static int pci_wait_for_pending_transaction(struct pci_dev *pdev)
{
    Check(pdev == &endpoint, "fatal stop uses only a best-effort pending wait");
    fatal_pending_waits++;
    return fatal_pending;
}
static bool NormalInterrupt(struct crystalhd_adp *adp, struct crystalhd_hw *hw)
{
    Check(adp == &adapter && hw == &hardware && !hw->dma_fault,
          "normal interrupt dispatch receives only a nonfatal hardware owner");
    normal_interrupts++;
    return interrupt_handled;
}
/* Actual bounded register acknowledgement is extracted in dma-stop.c. */
static bool crystalhd_hw_ack_fault_interrupt(struct crystalhd_hw *hw)
{
    Check(hw == &hardware && hw->dma_fault && !adapter.present && context.cin_wait_exit,
          "late fatal interrupt selects acknowledgement without ordinary ownership callbacks");
    fault_interrupts++;
    return interrupt_handled;
}
static BC_STATUS StopTx(struct crystalhd_hw *hw)
{
    Check(hw == &hardware, "TX stop receives the owned hardware context");
    tx_stops++; Event('T'); return BC_STS_SUCCESS;
}
static BC_STATUS Download(struct crystalhd_hw *hw, const uint8_t *data,
                          uint32_t size)
{
    uint32_t minimum = endpoint.device == BC_PCI_DEVID_FLEA ?
        CRYSTALHD_FLEA_MIN_FIRMWARE_SIZE : CRYSTALHD_LINK_MIN_FIRMWARE_SIZE;

    Check(hw == &hardware && data && !((unsigned long)data & 3U) &&
          size >= minimum && size <= CRYSTALHD_MAX_FIRMWARE_SIZE &&
          !(size & 3U) && data[0] == 0x5a,
          "firmware admission reaches the expected hardware callback");
    downloads++;
    last_download_image = data;
    last_download_size = size;
    Event('D');
    return download_status;
}
static int request_firmware(const struct firmware **firmware, const char *name,
                            struct device *device)
{
    Check(firmware && name && device == &endpoint.dev && adapter.user_lock == 1,
          "kernel firmware request retains its device and user-lock contract");
    Check(!strcmp(name, CRYSTALHD_FLEA_FIRMWARE_NAME) ||
          !strcmp(name, CRYSTALHD_LINK_FIRMWARE_NAME),
          "kernel firmware request uses a declared CrystalHD basename");
    kernel_fw_requests++;
    last_kernel_fw_name = name;
    last_kernel_fw_device = device;
    Event('Q');
    *firmware = NULL;
    if (kernel_fw_request_error)
        return kernel_fw_request_error;
    *firmware = &kernel_fw_blob;
    if (remove_during_fw_request)
        adapter.present = false;
    return 0;
}
static void release_firmware(const struct firmware *firmware)
{
    int unlocked;

    Check(firmware == &kernel_fw_blob,
          "kernel firmware release receives the requested object exactly once");
    unlocked = pthread_mutex_trylock(&transaction_mutex);
    Check(!unlocked,
          "kernel firmware is released after the shared download transaction");
    if (!unlocked && pthread_mutex_unlock(&transaction_mutex)) abort();
    kernel_fw_releases++;
    last_released_firmware = firmware;
    Event('L');
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
    BC_STATUS status;

    Check(hw == &hardware && command != NULL,
          "firmware callback receives the active command");
    if (command->cmd[0] == eCMD_C011_DEC_CHAN_ACTIVATE)
        Check(raw_calls[0] && raw_calls[0] == raw_calls[1] &&
              raw_offset == 0x00502100U && (raw_value & 3U) == 2U,
              "packed YUY2 is programmed before firmware activation");
    firmware_command_snapshot = *command;
    if (firmware_calls < sizeof(firmware_command_snapshots) /
                         sizeof(firmware_command_snapshots[0]))
        firmware_command_snapshots[firmware_calls] = *command;
    if (remove_during_fw_command ||
        remove_during_firmware_call == firmware_calls + 1)
        adapter.present = false;
    if (!transaction_mode)
        last_firmware_command = command;
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
    status = firmware_status_call == firmware_calls + 1 ?
        firmware_call_status : firmware_status;
    firmware_calls++;
    if (status == BC_STS_SUCCESS) {
        command->rsp[0] = command->cmd[0] ^ 0x5a5a5a5aU;
        if (command->cmd[0] == eCMD_C011_DEC_CHAN_OPEN)
            command->rsp[3] = firmware_open_channel;
    } else if (status == BC_STS_TIMEOUT && poison_on_firmware_timeout) {
        hardware.fwcmd_poisoned = true;
        hardware.FwCmdCnt = 1;
    }
    return status;
}
static BC_STATUS crystalhd_hw_stop_capture(struct crystalhd_hw *hw, bool unmap)
{
    ModuleCallback();
    Check(hw == &hardware, "capture stop receives the owned hardware context");
    captures++;
    if (unmap)
        capture_unmaps++;
    else
        Event('C');
    if (capture_status != BC_STS_SUCCESS)
        crystalhd_hw_dma_fatal_stop(hw);
    return capture_status;
}
static void crystalhd_hw_retire_rx_quiesced(struct crystalhd_hw *hw)
{
    ModuleCallback();
    Check(hw == &hardware && hw->fetch_sem == 1,
          "quiesced command retirement delegates the exact hardware outside fetch serialization");
    quiesced_rx_retires++;
}
static BC_STATUS crystalhd_hw_cancel_all_tx(struct crystalhd_hw *hw)
{
    Check(hw == &hardware, "suspend cancels every TX-list owner");
    cancels++; Event('X');
    if (cancel_status != BC_STS_SUCCESS)
        crystalhd_hw_dma_fatal_stop(hw);
    return cancel_status;
}
/* Actual fixed-packet detach/put behavior is exercised by tx-admission.
 * This boundary checks ordering against the real command teardown below.
 */
static void crystalhd_hw_retire_tx_quiesced(struct crystalhd_hw *hw)
{
    Check(hw == &hardware, "terminal TX retirement receives the retained hardware");
    quiesced_tx_retires++;
    if (retained_tx_lease) {
        ModuleCallback();
        Check(dio_live && rings_live && context.stream == &stream_cookie,
              "terminal retained TX put precedes DIO, ring and stream-owner destruction");
        retained_tx_lease = false;
        retained_tx_puts++;
    }
}
static uint32_t RawRegister(struct crystalhd_adp *adp, unsigned operation,
                            uint32_t offset, uint32_t value)
{
    Check(adp == &adapter && operation < 4, "raw register callback receives the adapter");
    raw_calls[operation]++;
    if ((operation & 1) && adp->dram_lock) raw_dram_writes++;
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
static BC_STATUS ReadMemory(struct crystalhd_hw *hw, uint32_t offset,
                            uint32_t words, uint32_t *buffer)
{
    Check(hw == &hardware && buffer == raw_buffer && words <= 2,
          "raw memory callback receives the owned hardware and bounded buffer");
    raw_calls[4]++;
    raw_offset = offset;
    raw_words = words;
    if (raw_status == BC_STS_SUCCESS)
        for (uint32_t n = 0; n < words; n++) {
            buffer[n] = raw_memory[n];
        }
    return raw_status;
}
static BC_STATUS WriteMemory(struct crystalhd_hw *hw, uint32_t offset,
                             uint32_t words, const uint32_t *buffer)
{
    Check(hw == &hardware && buffer == raw_buffer && words <= 2,
          "raw memory callback receives the owned hardware and bounded buffer");
    raw_calls[5]++;
    raw_offset = offset;
    raw_words = words;
    if (raw_status == BC_STS_SUCCESS)
        for (uint32_t n = 0; n < words; n++)
            raw_memory[n] = buffer[n];
    return raw_status;
}
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
    ModuleCallback();
    CheckPendingAdmission();
    Check(adp == &adapter && size == BC_LINK_ELEM_POOL_SZ, "notify allocates element pool");
    pools++; elem_live = true; adp->elem_pool_head = &elem_live;
    return elem_error;
}
static void crystalhd_delete_elem_pool(struct crystalhd_adp *adp)
{
    ModuleCallback();
    Check(adp == &adapter, "element-pool teardown receives the adapter");
    elem_deletes++; elem_live = false; adp->elem_pool_head = NULL;
}
static int crystalhd_create_dio_pool(struct crystalhd_adp *adp, unsigned size)
{
    ModuleCallback();
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
    ModuleCallback();
    Check(adp == &adapter, "DMA-pool teardown receives the adapter");
    dio_destroys++; dio_live = false; adp->fill_byte_pool = NULL;
}
static BC_STATUS crystalhd_hw_setup_dma_rings(struct crystalhd_hw *hw)
{
    ModuleCallback();
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
    ModuleCallback();
    Check(hw == &hardware, "DMA-ring teardown receives the hardware context");
    if (hw->dma_fault && !adapter.dma_terminal_quiesced &&
        (rings_live || retained_tx_lease))
        return BC_STS_IO_ERROR;
    crystalhd_hw_retire_tx_quiesced(hw);
    Check(!retained_tx_lease, "TX retained backing is retired before descriptor-ring storage");
    ring_frees++; rings_live = false; hw->rx_freeq = NULL;
    return BC_STS_SUCCESS;
}
static void crystalhd_hw_fw_cmd_reset_locked(struct crystalhd_hw *hw)
{
    Check(hw == &hardware, "hardware reset clears firmware-command accounting");
    download_resets++;
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
static int crystalhd_stream_prepare(struct crystalhd_cmd *ctx)
{
    Check(ctx == &context && !ctx->stream,
          "typed channel allocates staging before firmware setup");
    stream_prepares++;
    if (stream_prepare_error)
        return stream_prepare_error;
    ctx->stream = &stream_cookie;
    return 0;
}
static void crystalhd_stream_release(struct crystalhd_cmd *ctx)
{
    if (!ctx || !ctx->stream)
        return;
    ModuleCallback();
    Check(ctx == &context && ctx->stream == &stream_cookie,
          "typed channel releases its exact staging owner");
    Check(!retained_tx_lease, "stream owner survives until its retained TX lease is returned");
    ctx->stream = NULL;
    stream_releases++;
}
/* Keep close accounting while executing its complete production body. */
#define crystalhd_hw_close crystalhd_hw_close_actual
#include "command-pm-hardware.h"
#undef crystalhd_hw_close
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
    expect_retire_irq = false;
    binding_count = binding_frees = device_reads = user_writes = 0;
    memset(binding_live, 0, sizeof(binding_live));
    chd_device_lock = 0;
    chd_device_generation = 42;
    adapter_visible = true;
    downloads = download_resets = 0;
    stream_prepares = stream_releases = 0;
    quiesced_rx_retires = 0;
    quiesced_tx_retires = retained_tx_puts = 0;
    retained_tx_lease = false;
    fatal_master_clears = fatal_pending_waits = 0;
    fatal_chip_masks = 0;
    fatal_pending = true;
    normal_interrupts = fault_interrupts = 0;
    interrupt_handled = false;
    module_refs = module_get_attempts = module_gets = module_puts = 0;
    module_callbacks = 0;
    module_get_allowed = true;
    checking_module_callbacks = module_expect_no_hardware_on_put = false;
    memset(frontend_owners, 0, sizeof(frontend_owners));
    frontend_owners[0].refs = frontend_owners[1].refs = 1;
    frontend_lifetime_step = 0;
    stream_prepare_error = 0;
    last_download_image = NULL;
    last_download_size = 0;
    kernel_fw_requests = kernel_fw_releases = 0;
    kernel_fw_request_error = 0;
    last_kernel_fw_name = NULL;
    last_kernel_fw_device = NULL;
    last_released_firmware = NULL;
    remove_during_fw_request = false;
    memset(kernel_fw_image, 0, sizeof(kernel_fw_image));
    kernel_fw_image[0] = 0x5a;
    kernel_fw_blob.size = CRYSTALHD_FLEA_MIN_FIRMWARE_SIZE;
    kernel_fw_blob.data = kernel_fw_image;
    memset(raw_calls, 0, sizeof(raw_calls));
    raw_dram_writes = 0;
    memset(raw_memory, 0, sizeof(raw_memory));
    raw_offset = raw_value = raw_words = 0;
    raw_buffer = NULL;
    raw_status = BC_STS_SUCCESS;
    events[0] = '\0'; start_ok = stop_ok = true;
    elem_live = dio_live = rings_live = hardware_allocated = false;
    elem_error = dio_error = 0;
    ring_status = hardware_open_status = BC_STS_SUCCESS;
    capture_status = cancel_status = download_status = BC_STS_SUCCESS;
    pause_status = firmware_status = BC_STS_SUCCESS;
    pause_calls = firmware_calls = 0;
    last_firmware_command = NULL;
    memset(&firmware_command_snapshot, 0, sizeof(firmware_command_snapshot));
    memset(firmware_command_snapshots, 0, sizeof(firmware_command_snapshots));
    firmware_status_call = remove_during_firmware_call = 0;
    firmware_call_status = BC_STS_SUCCESS;
    firmware_open_channel = 0;
    remove_during_fw_command = poison_on_firmware_timeout = false;
    memset(pause_states, 0, sizeof(pause_states));
    transaction_mode = block_first_firmware = false;
    first_firmware_waiting = release_first_firmware = false;
    block_download_reset = download_reset_waiting = release_download_reset = false;
    transaction_attempts = 0;
    endpoint.device = BC_PCI_DEVID_FLEA;
    adapter = (struct crystalhd_adp){ .pdev = &endpoint, .present = true };
    ConfigureHardware(&hardware);
    context = (struct crystalhd_cmd){ .state = state, .adp = &adapter,
        .hw_ctx = with_hardware ? &hardware : NULL, .cin_wait_exit = 1,
        .decoder_codec = CRYSTALHD_DECODER_CODEC_INVALID };
    for (n = 0; n < BC_LINK_MAX_OPENS; n++) {
        context.user[n].uid = n;
        context.user[n].mode = DTS_MODE_INV;
    }
}

static void ResetOwned(uint32_t state, bool with_hardware)
{
    Reset(state, with_hardware);
    adapter.user_lock = 1;
    context.user[0].in_use = 1;
    context.user[0].mode = DTS_PLAYBACK_MODE;
    context.session_owner = &context.user[0];
}

static void ResetFrontendOwned(uint32_t state, bool with_hardware,
                               const void *owner)
{
    Reset(state, with_hardware);
    adapter.user_lock = 1;
    context.session_owner = owner;
}

static void FirmwarePauseRollback(void)
{
    crystalhd_ioctl_data data = {0};

    ResetOwned(BC_LINK_INIT | BC_LINK_PAUSED, true);
    hardware.fwcmd_poisoned = true;
    data.udata.u.fwCmd.cmd[0] = eCMD_C011_DEC_CHAN_PAUSE;
    data.udata.u.fwCmd.cmd[3] = 0;
    Check(bc_cproc_do_fw_cmd(&context, &data) == BC_STS_BUSY,
          "poisoned firmware resume is rejected before preprocessing");
    Check((context.state & BC_LINK_PAUSED) && !pause_calls &&
          !firmware_calls && hardware.fetch_sem == 1,
          "poisoned resume cannot mutate local capture state");

    ResetOwned(BC_LINK_INIT, true);
    context.cin_wait_exit = 0;
    hardware.fwcmd_poisoned = true;
    data.udata.u.fwCmd.cmd[0] = eCMD_C011_DEC_CHAN_FLUSH;
    data.udata.u.fwCmd.cmd[3] = 1;
    Check(bc_cproc_do_fw_cmd(&context, &data) == BC_STS_BUSY,
          "poisoned firmware flush is rejected before preprocessing");
    Check(!context.cin_wait_exit && !pause_calls && !firmware_calls,
          "poisoned flush cannot publish local cancellation state");

    ResetOwned(BC_LINK_INIT | BC_LINK_PAUSED, true);
    data.udata.u.fwCmd.cmd[0] = eCMD_C011_DEC_CHAN_PAUSE;
    data.udata.u.fwCmd.cmd[3] = 0;
    firmware_status = BC_STS_TIMEOUT;
    Check(bc_cproc_do_fw_cmd(&context, &data) == BC_STS_TIMEOUT,
          "failed firmware resume preserves its original error");
    Check((context.state & BC_LINK_PAUSED) && pause_calls == 2 &&
          !pause_states[0] && pause_states[1] && firmware_calls == 1 &&
          hardware.fetch_sem == 1,
          "failed firmware resume restores the local paused capture state");

    ResetOwned(BC_LINK_INIT, true);
    data.udata.u.fwCmd.cmd[0] = eCMD_C011_DEC_CHAN_PAUSE;
    data.udata.u.fwCmd.cmd[3] = 0;
    firmware_status = BC_STS_TIMEOUT;
    Check(bc_cproc_do_fw_cmd(&context, &data) == BC_STS_TIMEOUT,
          "a redundant resume preserves its firmware failure");
    Check(!(context.state & BC_LINK_PAUSED) && pause_calls == 1 &&
          !pause_states[0] && firmware_calls == 1 && hardware.fetch_sem == 1,
          "failed redundant resume preserves the original running state");

    ResetOwned(BC_LINK_INIT | BC_LINK_PAUSED, true);
    data.udata.u.fwCmd.cmd[0] = eCMD_C011_DEC_CHAN_PAUSE;
    data.udata.u.fwCmd.cmd[3] = 0;
    pause_status = BC_STS_IO_ERROR;
    Check(bc_cproc_do_fw_cmd(&context, &data) == BC_STS_IO_ERROR,
          "failed local resume is returned before posting firmware work");
    Check((context.state & BC_LINK_PAUSED) && pause_calls == 2 &&
          !pause_states[0] && pause_states[1] && !firmware_calls &&
          hardware.fetch_sem == 1,
          "partial local resume is re-paused without changing command state");

    ResetOwned(BC_LINK_INIT | BC_LINK_PAUSED, true);
    data.udata.u.fwCmd.cmd[0] = eCMD_C011_DEC_CHAN_PAUSE;
    data.udata.u.fwCmd.cmd[3] = 0;
    Check(bc_cproc_do_fw_cmd(&context, &data) == BC_STS_SUCCESS,
          "successful resume reaches firmware");
    Check(!(context.state & BC_LINK_PAUSED) && pause_calls == 1 &&
          !pause_states[0] && firmware_calls == 1 && hardware.fetch_sem == 1,
          "successful resume commits the local running state once");

    ResetOwned(BC_LINK_INIT | BC_LINK_PAUSED, true);
    data.udata.u.fwCmd.cmd[0] = eCMD_C011_DEC_CHAN_PAUSE;
    data.udata.u.fwCmd.cmd[3] = 0;
    pause_status = BC_STS_NO_DATA;
    Check(bc_cproc_do_fw_cmd(&context, &data) == BC_STS_SUCCESS,
          "resume with no queued capture buffer still reaches firmware");
    Check(!(context.state & BC_LINK_PAUSED) && pause_calls == 1 &&
          !pause_states[0] && firmware_calls == 1 && hardware.fetch_sem == 1,
          "NO_DATA preserves the legacy successful resume transition");

    ResetOwned(BC_LINK_INIT, true);
    data.udata.u.fwCmd.cmd[0] = eCMD_C011_DEC_CHAN_PAUSE;
    data.udata.u.fwCmd.cmd[3] = 1;
    firmware_status = BC_STS_FW_CMD_ERR;
    Check(bc_cproc_do_fw_cmd(&context, &data) == BC_STS_FW_CMD_ERR,
          "failed firmware pause leaves capture running");
    Check(!(context.state & BC_LINK_PAUSED) && !pause_calls && firmware_calls == 1,
          "failed firmware pause publishes no local pause");
}

static void FirmwareSharedEntry(void)
{
    BC_FW_CMD command = {0};
    crystalhd_ioctl_data data = {0};
    const void *owner;
    int frontend_owner, wrong_owner;

    ResetOwned(BC_LINK_INIT, true);
    Check(crystalhd_fw_exec_locked(NULL, &frontend_owner, &command) == BC_STS_INV_ARG,
          "shared firmware command rejects a NULL context");
    Check(crystalhd_fw_exec_locked(&context, NULL, &command) == BC_STS_INV_ARG,
          "shared firmware command rejects a NULL owner");
    Check(crystalhd_fw_exec_locked(&context, context.session_owner, NULL) == BC_STS_INV_ARG,
          "shared firmware command rejects a NULL command");
    context.hw_ctx = NULL;
    Check(crystalhd_fw_exec_locked(&context, context.session_owner, &command) == BC_STS_INV_ARG,
          "shared firmware command rejects retired hardware");

    ResetOwned(BC_LINK_INIT, true);
    owner = context.session_owner;
    context.adp = NULL;
    Check(crystalhd_fw_exec_locked(&context, owner, &command) == BC_STS_INV_ARG,
          "shared firmware command rejects a missing adapter");

    ResetOwned(BC_LINK_INIT, true);
    adapter.pdev = NULL;
    Check(crystalhd_fw_exec_locked(&context, context.session_owner, &command) == BC_STS_INV_ARG,
          "shared firmware command rejects a missing PCI device");

    ResetOwned(BC_LINK_INIT, true);
    hardware.pfnDoFirmwareCmd = NULL;
    Check(crystalhd_fw_exec_locked(&context, context.session_owner, &command) == BC_STS_INV_ARG,
          "shared firmware command rejects a missing hardware callback");
    Check(!firmware_calls && !pause_calls,
          "invalid shared commands have no hardware effects");

    Reset(BC_LINK_INIT, true);
    adapter.user_lock = 1;
    Check(crystalhd_fw_exec_locked(&context, &frontend_owner, &command) == BC_STS_ERR_USAGE,
          "shared firmware command rejects an absent session owner");
    Check(!firmware_calls && !pause_calls,
          "owner rejection precedes every firmware and capture effect");

    ResetOwned(BC_LINK_INIT, true);
    Check(crystalhd_fw_exec_locked(&context, &wrong_owner, &command) == BC_STS_ERR_USAGE,
          "shared firmware command rejects a different session owner");
    Check(!firmware_calls && !pause_calls,
          "foreign owner rejection precedes every firmware and capture effect");

    Reset(BC_LINK_INIT, true);
    adapter.user_lock = 1;
    context.session_owner = &frontend_owner;
    command.cmd[0] = 0x12345678;
    Check(crystalhd_fw_exec_locked(&context, &frontend_owner, &command) == BC_STS_SUCCESS,
          "a frontend can execute a command without ioctl data");
    Check(firmware_calls == 1 && last_firmware_command == &command &&
          command.rsp[0] == (0x12345678U ^ 0x5a5a5a5aU),
          "the shared entry returns the response in the frontend command");

    ResetOwned(BC_LINK_INIT, true);
    data.udata.u.fwCmd.cmd[0] = 0x87654321;
    Check(bc_cproc_do_fw_cmd(&context, &data) == BC_STS_SUCCESS,
          "the legacy firmware ioctl adapter still succeeds");
    Check(firmware_calls == 1 &&
          last_firmware_command == &data.udata.u.fwCmd &&
          data.udata.u.fwCmd.rsp[0] == (0x87654321U ^ 0x5a5a5a5aU),
          "the legacy adapter returns the response through its ABI command");
    Check(bc_cproc_do_fw_cmd(&context, NULL) == BC_STS_INV_ARG,
          "the legacy adapter rejects missing ioctl data");

    ResetOwned(BC_LINK_INIT, true);
    context.user[1].in_use = 1;
    data.u_id = 1;
    Check(bc_cproc_do_fw_cmd(&context, &data) == BC_STS_ERR_USAGE &&
          !firmware_calls && !pause_calls,
          "an unconfigured secondary legacy handle cannot inject firmware commands");
    data.u_id = BC_LINK_MAX_OPENS;
    Check(bc_cproc_do_fw_cmd(&context, &data) == BC_STS_INV_ARG &&
          !firmware_calls,
          "the legacy adapter rejects an out-of-range owner index");
}

static void FirmwareDownloadSharedEntry(void)
{
    _Alignas(uint32_t) uint8_t firmware[CRYSTALHD_LINK_MIN_FIRMWARE_SIZE] = {0};
    static _Alignas(uint32_t)
        uint8_t maximum_firmware[CRYSTALHD_MAX_FIRMWARE_SIZE];
    const size_t bad_sizes[] = {
        CRYSTALHD_FLEA_MIN_FIRMWARE_SIZE - 4,
        CRYSTALHD_FLEA_MIN_FIRMWARE_SIZE - 1,
        CRYSTALHD_FLEA_MIN_FIRMWARE_SIZE + 1,
        CRYSTALHD_MAX_FIRMWARE_SIZE + 4U,
        SIZE_MAX,
    };
    int frontend_owner = 0, foreign_owner = 0;
    const void *owner;

    firmware[0] = 0x5a;
    maximum_firmware[0] = 0x5a;

    ResetFrontendOwned(BC_LINK_INVALID, true, &frontend_owner);
    Check(crystalhd_fw_download_locked(NULL, &frontend_owner, firmware,
                                       CRYSTALHD_FLEA_MIN_FIRMWARE_SIZE) ==
              BC_STS_INV_ARG,
          "shared firmware download rejects a NULL context");
    Check(crystalhd_fw_download_locked(&context, NULL, firmware,
                                       CRYSTALHD_FLEA_MIN_FIRMWARE_SIZE) ==
              BC_STS_INV_ARG,
          "shared firmware download rejects a NULL owner");
    Check(crystalhd_fw_download_locked(&context, &frontend_owner, NULL,
                                       CRYSTALHD_FLEA_MIN_FIRMWARE_SIZE) ==
              BC_STS_INV_ARG,
          "shared firmware download rejects a NULL image");
    Check(crystalhd_fw_download_locked(&context, &frontend_owner, firmware, 0) ==
              BC_STS_INV_ARG && !downloads && !download_resets,
          "invalid shared arguments have no hardware or reset effects");

    ResetFrontendOwned(BC_LINK_INVALID, true, &frontend_owner);
    owner = context.session_owner;
    context.adp = NULL;
    Check(crystalhd_fw_download_locked(&context, owner, firmware,
                                       CRYSTALHD_FLEA_MIN_FIRMWARE_SIZE) ==
              BC_STS_INV_ARG,
          "shared firmware download rejects a missing adapter");

    ResetFrontendOwned(BC_LINK_INVALID, true, &frontend_owner);
    adapter.pdev = NULL;
    Check(crystalhd_fw_download_locked(&context, &frontend_owner, firmware,
                                       CRYSTALHD_FLEA_MIN_FIRMWARE_SIZE) ==
              BC_STS_INV_ARG,
          "shared firmware download rejects a missing PCI device");

    ResetFrontendOwned(BC_LINK_INVALID, false, &frontend_owner);
    Check(crystalhd_fw_download_locked(&context, &frontend_owner, firmware,
                                       CRYSTALHD_FLEA_MIN_FIRMWARE_SIZE) ==
              BC_STS_INV_ARG,
          "shared firmware download rejects retired hardware");

    ResetFrontendOwned(BC_LINK_INVALID, true, &frontend_owner);
    hardware.pfnFWDwnld = NULL;
    Check(crystalhd_fw_download_locked(&context, &frontend_owner, firmware,
                                       CRYSTALHD_FLEA_MIN_FIRMWARE_SIZE) ==
              BC_STS_INV_ARG && !downloads,
          "shared firmware download rejects a missing hardware callback");

    Reset(BC_LINK_INVALID, true);
    adapter.user_lock = 1;
    endpoint.device = 0xffff;
    transaction_mode = true;
    Check(crystalhd_fw_download_locked(&context, &frontend_owner, firmware,
                                       1) ==
              BC_STS_ERR_USAGE && !downloads && !transaction_attempts,
          "shared firmware download rejects an absent owner before geometry or transaction");

    ResetFrontendOwned(BC_LINK_INVALID, true, &frontend_owner);
    endpoint.device = 0xffff;
    transaction_mode = true;
    Check(crystalhd_fw_download_locked(&context, &foreign_owner,
                                       firmware + 1, 1) ==
              BC_STS_ERR_USAGE && !downloads && !transaction_attempts,
          "shared firmware download rejects a foreign owner before geometry or transaction");

    for (unsigned n = 0; n < sizeof(bad_sizes) / sizeof(bad_sizes[0]); n++) {
        ResetFrontendOwned(BC_LINK_INVALID, true, &frontend_owner);
        transaction_mode = true;
        Check(crystalhd_fw_download_locked(&context, &frontend_owner, firmware,
                                           bad_sizes[n]) == BC_STS_INV_ARG &&
                  !downloads && !transaction_attempts,
              "shared firmware download rejects malformed Flea geometry before transaction");
    }

    ResetFrontendOwned(BC_LINK_INVALID, true, &frontend_owner);
    transaction_mode = true;
    Check(crystalhd_fw_download_locked(&context, &frontend_owner,
                                       firmware + 1,
                                       CRYSTALHD_FLEA_MIN_FIRMWARE_SIZE) ==
              BC_STS_INV_ARG && !downloads && !transaction_attempts,
          "shared firmware download rejects an unaligned image before transaction");

    ResetFrontendOwned(BC_LINK_INVALID, true, &frontend_owner);
    endpoint.device = 0xffff;
    transaction_mode = true;
    Check(crystalhd_fw_download_locked(&context, &frontend_owner, firmware,
                                       CRYSTALHD_FLEA_MIN_FIRMWARE_SIZE) ==
              BC_STS_INV_ARG && !downloads && !transaction_attempts,
          "shared firmware download rejects an unknown firmware layout");

    ResetFrontendOwned(BC_LINK_INVALID, true, &frontend_owner);
    endpoint.device = BC_PCI_DEVID_LINK;
    transaction_mode = true;
    Check(crystalhd_fw_download_locked(&context, &frontend_owner, firmware,
                                       CRYSTALHD_LINK_MIN_FIRMWARE_SIZE - 4) ==
              BC_STS_INV_ARG && !downloads && !transaction_attempts,
          "shared firmware download applies the Link minimum before transaction");

    ResetFrontendOwned(BC_LINK_INVALID, true, &frontend_owner);
    hardware.fwcmd_poisoned = true;
    hardware.FwCmdCnt = 17;
    Check(crystalhd_fw_download_locked(&context, &frontend_owner, firmware,
                                       CRYSTALHD_FLEA_MIN_FIRMWARE_SIZE) ==
              BC_STS_SUCCESS && downloads == 1 && download_resets == 1 &&
              last_download_image == firmware &&
              last_download_size == CRYSTALHD_FLEA_MIN_FIRMWARE_SIZE &&
              context.state == BC_LINK_INIT && !hardware.fwcmd_poisoned &&
              !hardware.FwCmdCnt,
          "shared Flea download forwards the exact image and resets quarantine on success");

    ResetFrontendOwned(BC_LINK_INVALID, true, &frontend_owner);
    Check(crystalhd_fw_download_locked(&context, &frontend_owner,
                                       maximum_firmware,
                                       CRYSTALHD_MAX_FIRMWARE_SIZE) ==
              BC_STS_SUCCESS && downloads == 1 && download_resets == 1 &&
              last_download_image == maximum_firmware &&
              last_download_size == CRYSTALHD_MAX_FIRMWARE_SIZE &&
              context.state == BC_LINK_INIT,
          "shared firmware download forwards the aligned maximum image size");

    ResetFrontendOwned(BC_LINK_INVALID, true, &frontend_owner);
    endpoint.device = BC_PCI_DEVID_LINK;
    Check(crystalhd_fw_download_locked(&context, &frontend_owner, firmware,
                                       CRYSTALHD_LINK_MIN_FIRMWARE_SIZE) ==
              BC_STS_SUCCESS && downloads == 1 && download_resets == 1 &&
              last_download_image == firmware &&
              last_download_size == CRYSTALHD_LINK_MIN_FIRMWARE_SIZE &&
              context.state == BC_LINK_INIT,
          "shared Link download forwards its distinct valid image geometry");

    ResetFrontendOwned(BC_LINK_INVALID, true, &frontend_owner);
    hardware.fwcmd_poisoned = true;
    hardware.FwCmdCnt = 23;
    context.pwr_state_change = BC_HW_SUSPEND;
    download_status = BC_STS_TIMEOUT;
    transaction_mode = true;
    Check(crystalhd_fw_download_locked(&context, &frontend_owner, firmware,
                                       CRYSTALHD_FLEA_MIN_FIRMWARE_SIZE) ==
              BC_STS_TIMEOUT && downloads == 1 && !download_resets &&
              context.state == BC_LINK_INVALID && hardware.fwcmd_poisoned &&
              hardware.FwCmdCnt == 23 &&
              context.pwr_state_change == BC_HW_RUNNING &&
              transaction_attempts == 1,
          "shared firmware download preserves an exact loader failure without publishing INIT");
    Check(!pthread_mutex_trylock(&transaction_mutex),
          "failed firmware download releases its transaction");
    if (pthread_mutex_unlock(&transaction_mutex)) abort();

    ResetFrontendOwned(BC_LINK_INVALID, true, &frontend_owner);
    hardware.fwcmd_pending = true;
    transaction_mode = true;
    Check(crystalhd_fw_download_locked(&context, &frontend_owner, firmware,
                                       CRYSTALHD_FLEA_MIN_FIRMWARE_SIZE) ==
              BC_STS_BUSY && !downloads && !download_resets &&
              transaction_attempts == 1 && context.state == BC_LINK_INVALID,
          "shared firmware download cannot replace a pending mailbox command");
    Check(!pthread_mutex_trylock(&transaction_mutex),
          "busy firmware recovery admission releases its transaction");
    if (pthread_mutex_unlock(&transaction_mutex)) abort();

    ResetFrontendOwned(BC_LINK_READY, true, &frontend_owner);
    transaction_mode = true;
    Check(crystalhd_fw_download_locked(&context, &frontend_owner, firmware,
                                       CRYSTALHD_FLEA_MIN_FIRMWARE_SIZE) ==
              BC_STS_ERR_USAGE && !downloads && !download_resets &&
              transaction_attempts == 1 && context.state == BC_LINK_READY,
          "shared firmware download checks active link state inside its transaction");
    Check(!pthread_mutex_trylock(&transaction_mutex),
          "invalid-state firmware download releases its transaction");
    if (pthread_mutex_unlock(&transaction_mutex)) abort();

    ResetFrontendOwned(BC_LINK_RESUME, true, &frontend_owner);
    Check(crystalhd_fw_download_locked(&context, &frontend_owner, firmware,
                                       CRYSTALHD_FLEA_MIN_FIRMWARE_SIZE) ==
              BC_STS_SUCCESS &&
              context.state == (BC_LINK_RESUME | BC_LINK_INIT),
          "shared firmware download preserves the resumed recovery state rule");
}

static void FirmwareDownloadLegacyAdapter(void)
{
    _Alignas(uint32_t) uint8_t firmware[CRYSTALHD_FLEA_MIN_FIRMWARE_SIZE] = {0};
    crystalhd_ioctl_data data = {
        .add_cdata = firmware,
        .add_cdata_sz = sizeof(firmware),
    };

    firmware[0] = 0x5a;

    ResetOwned(BC_LINK_INVALID, true);
    Check(bc_cproc_download_fw(&context, NULL) == BC_STS_INV_ARG &&
          !downloads && !download_resets,
          "the legacy firmware adapter rejects missing ioctl data");

    Reset(BC_LINK_INVALID, true);
    adapter.user_lock = 1;
    Check(bc_cproc_download_fw(&context, &data) == BC_STS_ERR_USAGE &&
          !downloads && context.state == BC_LINK_INVALID,
          "firmware download rejects an absent session owner before hardware");

    ResetOwned(BC_LINK_INVALID, true);
    context.user[1].in_use = 1;
    data.u_id = 1;
    Check(bc_cproc_download_fw(&context, &data) == BC_STS_ERR_USAGE &&
          !downloads && context.state == BC_LINK_INVALID,
          "firmware download rejects an unconfigured secondary handle");

    data.u_id = 0;
    kernel_fw_request_error = -ENOENT;
    Check(bc_cproc_download_fw(&context, &data) == BC_STS_SUCCESS &&
          downloads == 1 && download_resets == 1 &&
          !kernel_fw_requests && !kernel_fw_releases &&
          last_download_image == firmware &&
          last_download_size == CRYSTALHD_FLEA_MIN_FIRMWARE_SIZE &&
          context.state == BC_LINK_INIT,
          "the legacy adapter uses its appended image without requesting kernel firmware");

    ResetOwned(BC_LINK_INVALID, true);
    download_status = BC_STS_TIMEOUT;
    Check(bc_cproc_download_fw(&context, &data) == BC_STS_TIMEOUT &&
          downloads == 1 && !download_resets &&
          last_download_image == firmware &&
          last_download_size == sizeof(firmware) &&
          context.state == BC_LINK_INVALID,
          "the legacy adapter preserves the shared loader status");

    ResetOwned(BC_LINK_INVALID, true);
    data.u_id = BC_LINK_MAX_OPENS;
    Check(bc_cproc_download_fw(&context, &data) == BC_STS_INV_ARG &&
          !downloads && context.state == BC_LINK_INVALID,
          "firmware download rejects an out-of-range owner index");

    ResetOwned(BC_LINK_INVALID, true);
    data.u_id = 0;
    data.add_cdata = NULL;
    Check(bc_cproc_download_fw(&context, &data) == BC_STS_INV_ARG &&
          !downloads && context.state == BC_LINK_INVALID,
          "the legacy adapter rejects a missing appended image");

    data.add_cdata = firmware;
    data.add_cdata_sz = 0;
    Check(bc_cproc_download_fw(&context, &data) == BC_STS_INV_ARG &&
          !downloads && context.state == BC_LINK_INVALID,
          "the legacy adapter rejects an empty appended image");
}

static void ArmKernelFirmwareNoEffects(void)
{
    transaction_mode = true;
    context.pwr_state_change = BC_HW_SUSPEND;
    hardware.fwcmd_poisoned = true;
    hardware.FwCmdCnt = 41;
}

static bool NoKernelFirmwareEffects(uint32_t state)
{
    return !kernel_fw_requests && !kernel_fw_releases && !downloads &&
           !download_resets && !transaction_attempts &&
           context.state == state &&
           context.pwr_state_change == BC_HW_SUSPEND &&
           hardware.fwcmd_poisoned && hardware.FwCmdCnt == 41;
}

static void KernelFirmwarePreconditions(void)
{
    const uint32_t active_states[] = {
        BC_LINK_INIT,
        BC_LINK_READY,
        BC_LINK_RESUME | BC_LINK_INIT,
    };
    int frontend_owner = 0, foreign_owner = 0;
    const void *owner;

    ResetFrontendOwned(BC_LINK_INVALID, true, &frontend_owner);
    ArmKernelFirmwareNoEffects();
    Check(crystalhd_request_firmware_locked(NULL, &frontend_owner) == -EINVAL &&
          NoKernelFirmwareEffects(BC_LINK_INVALID),
          "kernel firmware rejects a NULL command context");
    Check(crystalhd_request_firmware_locked(&context, NULL) == -EINVAL &&
          NoKernelFirmwareEffects(BC_LINK_INVALID),
          "kernel firmware rejects a NULL owner");

    ResetFrontendOwned(BC_LINK_INVALID, true, &frontend_owner);
    ArmKernelFirmwareNoEffects();
    owner = context.session_owner;
    context.adp = NULL;
    Check(crystalhd_request_firmware_locked(&context, owner) == -ENODEV &&
          NoKernelFirmwareEffects(BC_LINK_INVALID),
          "kernel firmware rejects a missing adapter before requesting");

    ResetFrontendOwned(BC_LINK_INVALID, true, &frontend_owner);
    ArmKernelFirmwareNoEffects();
    adapter.pdev = NULL;
    Check(crystalhd_request_firmware_locked(&context, &frontend_owner) ==
              -ENODEV && NoKernelFirmwareEffects(BC_LINK_INVALID),
          "kernel firmware rejects a missing PCI device before requesting");

    ResetFrontendOwned(BC_LINK_INVALID, false, &frontend_owner);
    ArmKernelFirmwareNoEffects();
    Check(crystalhd_request_firmware_locked(&context, &frontend_owner) ==
              -ENODEV && NoKernelFirmwareEffects(BC_LINK_INVALID),
          "kernel firmware rejects retired hardware before requesting");

    ResetFrontendOwned(BC_LINK_INVALID, true, &frontend_owner);
    ArmKernelFirmwareNoEffects();
    hardware.pfnFWDwnld = NULL;
    Check(crystalhd_request_firmware_locked(&context, &frontend_owner) ==
              -ENODEV && NoKernelFirmwareEffects(BC_LINK_INVALID),
          "kernel firmware rejects a missing download callback before requesting");

    ResetFrontendOwned(BC_LINK_INVALID, true, &frontend_owner);
    ArmKernelFirmwareNoEffects();
    adapter.present = false;
    Check(crystalhd_request_firmware_locked(&context, &frontend_owner) ==
              -ENODEV && NoKernelFirmwareEffects(BC_LINK_INVALID),
          "kernel firmware rejects an unavailable device before requesting");

    Reset(BC_LINK_INVALID, true);
    adapter.user_lock = 1;
    ArmKernelFirmwareNoEffects();
    Check(crystalhd_request_firmware_locked(&context, &frontend_owner) ==
              -EINVAL && NoKernelFirmwareEffects(BC_LINK_INVALID),
          "kernel firmware requires an acquired session owner");

    ResetFrontendOwned(BC_LINK_INVALID, true, &frontend_owner);
    ArmKernelFirmwareNoEffects();
    Check(crystalhd_request_firmware_locked(&context, &foreign_owner) ==
              -EBUSY && NoKernelFirmwareEffects(BC_LINK_INVALID),
          "kernel firmware rejects a foreign session owner");

    ResetFrontendOwned(BC_LINK_SUSPEND, true, &frontend_owner);
    ArmKernelFirmwareNoEffects();
    Check(crystalhd_request_firmware_locked(&context, &frontend_owner) ==
              -EAGAIN && NoKernelFirmwareEffects(BC_LINK_SUSPEND),
          "kernel firmware does not load while the session is suspended");

    for (unsigned n = 0; n < sizeof(active_states) / sizeof(active_states[0]); n++) {
        ResetFrontendOwned(active_states[n], true, &frontend_owner);
        ArmKernelFirmwareNoEffects();
        Check(crystalhd_request_firmware_locked(&context, &frontend_owner) ==
                  -EBUSY && NoKernelFirmwareEffects(active_states[n]),
              "kernel firmware rejects an active or already initialized session");
    }

    ResetFrontendOwned(BC_LINK_INVALID, true, &frontend_owner);
    ArmKernelFirmwareNoEffects();
    endpoint.device = 0xffff;
    Check(crystalhd_request_firmware_locked(&context, &frontend_owner) ==
              -ENODEV && NoKernelFirmwareEffects(BC_LINK_INVALID),
          "kernel firmware rejects an unknown PCI device before requesting");
}

static void KernelFirmwareSelectionAndLifetime(void)
{
    const struct {
        uint32_t device;
        size_t size;
        uint32_t state;
        uint32_t final_state;
        const char *name;
    } cases[] = {
        { BC_PCI_DEVID_FLEA, CRYSTALHD_FLEA_MIN_FIRMWARE_SIZE,
          BC_LINK_INVALID, BC_LINK_INIT, "bcm70015fw.bin" },
        { BC_PCI_DEVID_LINK, CRYSTALHD_LINK_MIN_FIRMWARE_SIZE,
          BC_LINK_RESUME, BC_LINK_RESUME | BC_LINK_INIT,
          "bcm70012fw.bin" },
    };
    int frontend_owner = 0;

    for (unsigned n = 0; n < sizeof(cases) / sizeof(cases[0]); n++) {
        ResetFrontendOwned(cases[n].state, true, &frontend_owner);
        endpoint.device = cases[n].device;
        kernel_fw_blob.size = cases[n].size;
        hardware.fwcmd_poisoned = true;
        hardware.FwCmdCnt = 29;
        transaction_mode = true;

        Check(crystalhd_request_firmware_locked(&context, &frontend_owner) == 0,
              "kernel firmware loads through the shared owner boundary");
        Check(kernel_fw_requests == 1 && kernel_fw_releases == 1 &&
              last_kernel_fw_device == &endpoint.dev &&
              last_released_firmware == &kernel_fw_blob &&
              !strcmp(last_kernel_fw_name, cases[n].name),
              "kernel firmware selects and releases the exact chip image");
        Check(downloads == 1 && last_download_image == kernel_fw_image &&
              last_download_size == cases[n].size &&
              transaction_attempts == 1 && !strcmp(events, "QDL"),
              "kernel firmware forwards exact data and releases it after download");
        Check(context.state == cases[n].final_state && download_resets == 1 &&
              !hardware.fwcmd_poisoned && !hardware.FwCmdCnt,
              "kernel firmware publishes INIT and clears quarantine only on success");
    }
}

static void KernelFirmwareRequestAndAdmissionErrors(void)
{
    const int request_errors[] = { -ENOENT, -ENOMEM, -EINTR };
    const struct {
        uint32_t device;
        const uint8_t *data;
        size_t size;
    } malformed[] = {
        { BC_PCI_DEVID_FLEA, NULL, CRYSTALHD_FLEA_MIN_FIRMWARE_SIZE },
        { BC_PCI_DEVID_FLEA, kernel_fw_image, 0 },
        { BC_PCI_DEVID_LINK, kernel_fw_image,
          CRYSTALHD_LINK_MIN_FIRMWARE_SIZE - 4 },
        { BC_PCI_DEVID_FLEA, kernel_fw_image,
          CRYSTALHD_FLEA_MIN_FIRMWARE_SIZE + 1 },
        { BC_PCI_DEVID_FLEA, kernel_fw_image,
          CRYSTALHD_MAX_FIRMWARE_SIZE + 4U },
        { BC_PCI_DEVID_FLEA, kernel_fw_image, SIZE_MAX },
#if SIZE_MAX > UINT32_MAX
        { BC_PCI_DEVID_FLEA, kernel_fw_image,
          (size_t)UINT32_MAX + 1U + CRYSTALHD_FLEA_MIN_FIRMWARE_SIZE },
#endif
        { BC_PCI_DEVID_FLEA, kernel_fw_image + 1,
          CRYSTALHD_FLEA_MIN_FIRMWARE_SIZE },
    };
    int frontend_owner = 0;

    for (unsigned n = 0; n < sizeof(request_errors) / sizeof(request_errors[0]); n++) {
        ResetFrontendOwned(BC_LINK_INVALID, true, &frontend_owner);
        hardware.fwcmd_poisoned = true;
        hardware.FwCmdCnt = 31;
        kernel_fw_request_error = request_errors[n];
        transaction_mode = true;
        Check(crystalhd_request_firmware_locked(&context, &frontend_owner) ==
                  request_errors[n],
              "kernel firmware preserves the exact request_firmware error");
        Check(kernel_fw_requests == 1 && !kernel_fw_releases &&
              !downloads && !transaction_attempts && !strcmp(events, "Q") &&
              context.state == BC_LINK_INVALID && hardware.fwcmd_poisoned &&
              hardware.FwCmdCnt == 31,
              "failed firmware requests have no shared or hardware effects");
    }

    ResetFrontendOwned(BC_LINK_INVALID, true, &frontend_owner);
    remove_during_fw_request = true;
    transaction_mode = true;
    Check(crystalhd_request_firmware_locked(&context, &frontend_owner) ==
              -ENODEV,
          "kernel firmware rechecks device availability after a sleeping request");
    Check(kernel_fw_requests == 1 && kernel_fw_releases == 1 &&
          !downloads && !transaction_attempts && !strcmp(events, "QL") &&
          !adapter.present && context.state == BC_LINK_INVALID,
          "post-request removal releases the image without touching hardware");

    for (unsigned n = 0; n < sizeof(malformed) / sizeof(malformed[0]); n++) {
        ResetFrontendOwned(BC_LINK_INVALID, true, &frontend_owner);
        endpoint.device = malformed[n].device;
        kernel_fw_blob.data = malformed[n].data;
        kernel_fw_blob.size = malformed[n].size;
        transaction_mode = true;
        Check(crystalhd_request_firmware_locked(&context, &frontend_owner) ==
                  -EINVAL,
              "kernel firmware rejects a malformed requested image");
        Check(kernel_fw_requests == 1 && kernel_fw_releases == 1 &&
              !downloads && !transaction_attempts && !strcmp(events, "QL") &&
              context.state == BC_LINK_INVALID,
              "malformed requested firmware is released before hardware access");
    }

    ResetFrontendOwned(BC_LINK_INVALID, true, &frontend_owner);
    hardware.fwcmd_pending = true;
    transaction_mode = true;
    Check(crystalhd_request_firmware_locked(&context, &frontend_owner) ==
              -EBUSY,
          "kernel firmware preserves busy recovery admission");
    Check(kernel_fw_requests == 1 && kernel_fw_releases == 1 &&
          !downloads && transaction_attempts == 1 && !strcmp(events, "QL") &&
          context.state == BC_LINK_INVALID,
          "busy firmware admission releases the requested image and transaction");
}

static void KernelFirmwareStatusTranslation(void)
{
    const struct {
        BC_STATUS status;
        int error;
    } cases[] = {
        { BC_STS_SUCCESS, 0 },
        { BC_STS_INV_ARG, -EINVAL },
        { BC_STS_BUSY, -EBUSY },
        { BC_STS_INSUFF_RES, -ENOMEM },
        { BC_STS_NO_ACCESS, -EACCES },
        { BC_STS_TIMEOUT, -ETIMEDOUT },
        { BC_STS_IO_USER_ABORT, -ERESTARTSYS },
        { BC_STS_FW_AUTH_FAILED, -EKEYREJECTED },
        { BC_STS_CERT_VERIFY_ERROR, -EKEYREJECTED },
        { BC_STS_PWR_MGMT, -EAGAIN },
        { BC_STS_ERR_USAGE, -EIO },
        { BC_STS_IO_ERROR, -EIO },
    };
    int frontend_owner = 0;

    for (unsigned n = 0; n < sizeof(cases) / sizeof(cases[0]); n++) {
        ResetFrontendOwned(BC_LINK_INVALID, true, &frontend_owner);
        hardware.fwcmd_poisoned = true;
        hardware.FwCmdCnt = 37;
        download_status = cases[n].status;
        transaction_mode = true;

        Check(crystalhd_request_firmware_locked(&context, &frontend_owner) ==
                  cases[n].error,
              "kernel firmware maps the shared download status to Linux errno");
        Check(kernel_fw_requests == 1 && kernel_fw_releases == 1 &&
              downloads == 1 && transaction_attempts == 1 &&
              last_download_image == kernel_fw_image &&
              last_download_size == CRYSTALHD_FLEA_MIN_FIRMWARE_SIZE &&
              !strcmp(events, "QDL"),
              "every requested image is released after the shared transaction");
        if (cases[n].status == BC_STS_SUCCESS)
            Check(context.state == BC_LINK_INIT && download_resets == 1 &&
                  !hardware.fwcmd_poisoned && !hardware.FwCmdCnt,
                  "translated success publishes INIT and clears quarantine");
        else
            Check(context.state == BC_LINK_INVALID && !download_resets &&
                  hardware.fwcmd_poisoned && hardware.FwCmdCnt == 37,
                  "translated failure preserves state and quarantine");
    }
}

static void CheckKernelBootstrapPayload(uint32_t device)
{
    uint32_t expected[64] = {0};

    expected[0] = 0x73763001U;
    expected[1] = 1;
    expected[2] = 64U;
    expected[3] = 200000000U;
    expected[4] = 38400U;
    expected[5] = 0x1U | 0x2U;
    expected[6] = 1U;
    expected[8] = 2U;
    expected[9] = 1U;
    if (device == BC_PCI_DEVID_LINK)
        expected[13] = 1U;

    for (unsigned word = 0; word < 64; word++) {
        Check(firmware_command_snapshot.cmd[word] == expected[word],
              "kernel bootstrap submits the exact zero-filled INIT payload");
        Check(!firmware_command_snapshot.rsp[word],
              "kernel bootstrap submits a zero-filled response buffer");
    }
    Check(!firmware_command_snapshot.flags &&
          !firmware_command_snapshot.add_data,
          "kernel bootstrap leaves passthrough flags and data clear");
}

static void KernelFirmwareBootstrapPayload(void)
{
    const struct {
        uint32_t device;
        size_t size;
        uint32_t entry_state;
        uint32_t entry_power;
    } cases[] = {
        { BC_PCI_DEVID_FLEA, CRYSTALHD_FLEA_MIN_FIRMWARE_SIZE,
          BC_LINK_INVALID, BC_HW_SUSPEND },
        { BC_PCI_DEVID_LINK, CRYSTALHD_LINK_MIN_FIRMWARE_SIZE,
          BC_LINK_RESUME, BC_HW_RESUME },
    };
    int frontend_owner = 0;

    for (unsigned n = 0; n < sizeof(cases) / sizeof(cases[0]); n++) {
        ResetFrontendOwned(cases[n].entry_state, true, &frontend_owner);
        endpoint.device = cases[n].device;
        kernel_fw_blob.size = cases[n].size;
        context.pwr_state_change = cases[n].entry_power;
        hardware.fwcmd_poisoned = true;
        hardware.FwCmdCnt = 17;
        transaction_mode = true;

        Check(crystalhd_fw_bootstrap_locked(&context, &frontend_owner) == 0,
              "kernel bootstrap downloads and initializes either supported chip");
        Check(context.state == BC_LINK_INIT &&
              context.pwr_state_change == BC_HW_RUNNING &&
              context.decoder_phase == CRYSTALHD_DECODER_BOOTSTRAPPED &&
              context.fw_sequence == 1 && !context.decoder_channel_id &&
              context.session_owner == &frontend_owner,
              "kernel bootstrap commits one exact initialized owner state");
        Check(kernel_fw_requests == 1 && kernel_fw_releases == 1 &&
              downloads == 1 && download_resets == 1 &&
              firmware_calls == 1 && transaction_attempts == 2 &&
              !strcmp(events, "QDL"),
              "kernel bootstrap uses one shared download and one shared command transaction");
        Check(!hardware.fwcmd_poisoned && !hardware.FwCmdCnt,
              "kernel bootstrap recovers quarantine only through verified download");
        CheckKernelBootstrapPayload(cases[n].device);
    }
}

static void KernelFirmwareBootstrapPreconditions(void)
{
    int frontend_owner = 0, foreign_owner = 0;
    const void *owner;

    ResetFrontendOwned(BC_LINK_INVALID, true, &frontend_owner);
    context.pwr_state_change = BC_HW_SUSPEND;
    Check(crystalhd_fw_bootstrap_locked(NULL, &frontend_owner) == -EINVAL &&
          crystalhd_fw_bootstrap_locked(&context, NULL) == -EINVAL,
          "kernel bootstrap rejects missing context and owner tokens");
    Check(!kernel_fw_requests && !downloads && !firmware_calls &&
          context.state == BC_LINK_INVALID &&
          context.pwr_state_change == BC_HW_SUSPEND,
          "invalid bootstrap arguments have no request or state effects");

    ResetFrontendOwned(BC_LINK_INVALID, true, &frontend_owner);
    context.pwr_state_change = BC_HW_SUSPEND;
    owner = context.session_owner;
    context.adp = NULL;
    Check(crystalhd_fw_bootstrap_locked(&context, owner) == -ENODEV &&
          !kernel_fw_requests && !downloads && !firmware_calls &&
          context.state == BC_LINK_INVALID &&
          context.pwr_state_change == BC_HW_SUSPEND,
          "kernel bootstrap rejects a missing adapter before side effects");

    ResetFrontendOwned(BC_LINK_INVALID, true, &frontend_owner);
    context.pwr_state_change = BC_HW_SUSPEND;
    Check(crystalhd_fw_bootstrap_locked(&context, &foreign_owner) == -EBUSY &&
          !kernel_fw_requests && !downloads && !firmware_calls &&
          context.state == BC_LINK_INVALID &&
          context.pwr_state_change == BC_HW_SUSPEND,
          "kernel bootstrap preserves foreign-owner rejection and entry state");

    ResetFrontendOwned(BC_LINK_INIT, true, &frontend_owner);
    context.pwr_state_change = BC_HW_SUSPEND;
    Check(crystalhd_fw_bootstrap_locked(&context, &frontend_owner) == -EBUSY &&
          !kernel_fw_requests && !downloads && !firmware_calls &&
          context.state == BC_LINK_INIT &&
          context.pwr_state_change == BC_HW_SUSPEND,
          "kernel bootstrap rejects an already active firmware state");

    ResetFrontendOwned(BC_LINK_INVALID, true, &frontend_owner);
    context.pwr_state_change = BC_HW_SUSPEND;
    hardware.pfnDoFirmwareCmd = NULL;
    transaction_mode = true;
    Check(crystalhd_fw_bootstrap_locked(&context, &frontend_owner) == -EINVAL,
          "kernel bootstrap propagates a missing command transport");
    Check(kernel_fw_requests == 1 && kernel_fw_releases == 1 &&
          downloads == 1 && download_resets == 1 && !firmware_calls &&
          transaction_attempts == 1 && context.state == BC_LINK_INVALID &&
          context.pwr_state_change == BC_HW_SUSPEND,
          "post-download admission failure restores the exact entry state");
}

static void KernelFirmwareBootstrapFailures(void)
{
    const struct {
        BC_STATUS status;
        int error;
    } command_errors[] = {
        { BC_STS_INV_ARG, -EINVAL },
        { BC_STS_BUSY, -EBUSY },
        { BC_STS_INSUFF_RES, -ENOMEM },
        { BC_STS_NO_ACCESS, -EACCES },
        { BC_STS_TIMEOUT, -ETIMEDOUT },
        { BC_STS_IO_USER_ABORT, -ERESTARTSYS },
        { BC_STS_FW_AUTH_FAILED, -EKEYREJECTED },
        { BC_STS_CERT_VERIFY_ERROR, -EKEYREJECTED },
        { BC_STS_PWR_MGMT, -EAGAIN },
        { BC_STS_FW_CMD_ERR, -EIO },
        { BC_STS_IO_ERROR, -EIO },
    };
    int frontend_owner = 0;

    ResetFrontendOwned(BC_LINK_INVALID, true, &frontend_owner);
    context.pwr_state_change = BC_HW_SUSPEND;
    kernel_fw_request_error = -ENOENT;
    Check(crystalhd_fw_bootstrap_locked(&context, &frontend_owner) == -ENOENT &&
          kernel_fw_requests == 1 && !kernel_fw_releases && !downloads &&
          !firmware_calls && context.state == BC_LINK_INVALID &&
          context.decoder_phase == CRYSTALHD_DECODER_COLD &&
          context.pwr_state_change == BC_HW_SUSPEND,
          "kernel bootstrap preserves request errors and entry state");

    ResetFrontendOwned(BC_LINK_RESUME, true, &frontend_owner);
    context.pwr_state_change = BC_HW_RESUME;
    download_status = BC_STS_IO_ERROR;
    Check(crystalhd_fw_bootstrap_locked(&context, &frontend_owner) == -EIO &&
          kernel_fw_requests == 1 && kernel_fw_releases == 1 &&
          downloads == 1 && !download_resets && !firmware_calls &&
          context.state == BC_LINK_RESUME &&
          context.decoder_phase == CRYSTALHD_DECODER_COLD &&
          context.pwr_state_change == BC_HW_RESUME,
          "kernel bootstrap rolls back local power state after download failure");

    for (unsigned n = 0; n < sizeof(command_errors) / sizeof(command_errors[0]); n++) {
        ResetFrontendOwned(BC_LINK_RESUME, true, &frontend_owner);
        context.pwr_state_change = BC_HW_RESUME;
        firmware_status = command_errors[n].status;
        transaction_mode = true;
        Check(crystalhd_fw_bootstrap_locked(&context, &frontend_owner) ==
                  command_errors[n].error,
              "kernel bootstrap maps every shared command status to errno");
        Check(kernel_fw_requests == 1 && kernel_fw_releases == 1 &&
              downloads == 1 && download_resets == 1 &&
              firmware_calls == 1 && transaction_attempts == 2 &&
              context.state == BC_LINK_RESUME &&
              context.decoder_phase == CRYSTALHD_DECODER_RECOVERY_REQUIRED &&
              context.fw_sequence == 1 && !context.decoder_channel_id &&
              context.pwr_state_change == BC_HW_RESUME,
              "failed INIT restores the exact resume admission state");
    }

    ResetFrontendOwned(BC_LINK_INVALID, true, &frontend_owner);
    context.pwr_state_change = BC_HW_SUSPEND;
    remove_during_fw_request = true;
    Check(crystalhd_fw_bootstrap_locked(&context, &frontend_owner) == -ENODEV &&
          kernel_fw_requests == 1 && kernel_fw_releases == 1 &&
          !downloads && !firmware_calls && !adapter.present &&
          context.state == BC_LINK_INVALID &&
          context.decoder_phase == CRYSTALHD_DECODER_COLD &&
          context.pwr_state_change == BC_HW_SUSPEND,
          "kernel bootstrap stops after removal during firmware request");

    ResetFrontendOwned(BC_LINK_INVALID, true, &frontend_owner);
    context.pwr_state_change = BC_HW_SUSPEND;
    remove_during_fw_command = true;
    Check(crystalhd_fw_bootstrap_locked(&context, &frontend_owner) == -ENODEV &&
          kernel_fw_requests == 1 && kernel_fw_releases == 1 &&
          downloads == 1 && download_resets == 1 && firmware_calls == 1 &&
          !adapter.present && context.state == BC_LINK_INVALID &&
          context.decoder_phase == CRYSTALHD_DECODER_RECOVERY_REQUIRED &&
          context.fw_sequence == 1 && !context.decoder_channel_id &&
          context.pwr_state_change == BC_HW_SUSPEND,
          "kernel bootstrap rolls back if the device disappears at INIT completion");
}

static void KernelFirmwareBootstrapTimeoutRetry(void)
{
    int frontend_owner = 0;

    ResetFrontendOwned(BC_LINK_INVALID, true, &frontend_owner);
    context.pwr_state_change = BC_HW_SUSPEND;
    firmware_status = BC_STS_TIMEOUT;
    poison_on_firmware_timeout = true;
    transaction_mode = true;
    Check(crystalhd_fw_bootstrap_locked(&context, &frontend_owner) ==
              -ETIMEDOUT,
          "timed-out INIT returns its bounded timeout error");
    Check(context.state == BC_LINK_INVALID &&
          context.pwr_state_change == BC_HW_SUSPEND &&
          context.decoder_phase == CRYSTALHD_DECODER_RECOVERY_REQUIRED &&
          context.fw_sequence == 1 && !context.decoder_channel_id &&
          hardware.fwcmd_poisoned && hardware.FwCmdCnt == 1 &&
          kernel_fw_requests == 1 && downloads == 1 && firmware_calls == 1,
          "timed-out INIT restores admission but retains mailbox quarantine");

    firmware_status = BC_STS_SUCCESS;
    poison_on_firmware_timeout = false;
    Check(crystalhd_fw_bootstrap_locked(&context, &frontend_owner) == 0,
          "bootstrap retry recovers only through another verified download");
    Check(context.state == BC_LINK_INIT &&
          context.pwr_state_change == BC_HW_RUNNING &&
          context.decoder_phase == CRYSTALHD_DECODER_BOOTSTRAPPED &&
          context.fw_sequence == 1 && !context.decoder_channel_id &&
          !hardware.fwcmd_poisoned && !hardware.FwCmdCnt &&
          kernel_fw_requests == 2 && kernel_fw_releases == 2 &&
          downloads == 2 && download_resets == 2 && firmware_calls == 2 &&
          transaction_attempts == 4 && !strcmp(events, "QDLQDL"),
          "retry resets quarantine, reinitializes firmware and commits once");
    CheckKernelBootstrapPayload(BC_PCI_DEVID_FLEA);
}

static void ResetBootstrapped(const void *owner)
{
    ResetFrontendOwned(BC_LINK_INIT, true, owner);
    context.decoder_phase = CRYSTALHD_DECODER_BOOTSTRAPPED;
    context.decoder_codec = CRYSTALHD_DECODER_CODEC_INVALID;
    context.fw_sequence = 1;
    context.decoder_channel_id = 0;
}

static void ResetConfigured(const void *owner)
{
    ResetFrontendOwned(BC_LINK_INIT, true, owner);
    context.decoder_phase = CRYSTALHD_DECODER_CHANNEL_CONFIGURED;
    context.decoder_codec = CRYSTALHD_DECODER_CODEC_H264;
    context.fw_sequence = 3;
    context.decoder_channel_id = 0;
    context.stream = &stream_cookie;
}

static void ResetStarted(const void *owner)
{
    ResetFrontendOwned(BC_LINK_INIT, true, owner);
    context.decoder_phase = CRYSTALHD_DECODER_CHANNEL_STARTED;
    context.decoder_codec = CRYSTALHD_DECODER_CODEC_H264;
    context.fw_sequence = 5;
    context.decoder_channel_id = 0;
    context.stream = &stream_cookie;
}

static void CheckKernelChannelPayload(const BC_FW_CMD *command, bool opening,
                                      uint32_t sequence)
{
    uint32_t expected[64] = {0};

    expected[0] = opening ? 0x73763100U : 0x73763108U;
    expected[1] = sequence;
    if (opening) {
        expected[4] = 1U;
        expected[9] = 0U;
    } else {
        expected[2] = 0U;
        expected[3] = 1U;
    }

    for (unsigned word = 0; word < 64; word++) {
        Check(command->cmd[word] == expected[word],
              "typed channel setup submits the exact zero-filled request");
        Check(!command->rsp[word],
              "typed channel setup submits a zero-filled response buffer");
    }
    Check(!command->flags && !command->add_data,
          "typed channel setup leaves passthrough flags and data clear");
}

static void KernelDecoderChannelOpenPayload(void)
{
    const struct crystalhd_decoder_config config = {
        .codec = CRYSTALHD_DECODER_CODEC_H264,
    };
    int frontend_owner = 0;

    ResetFrontendOwned(BC_LINK_INVALID, true, &frontend_owner);
    transaction_mode = true;
    Check(crystalhd_fw_bootstrap_locked(&context, &frontend_owner) == 0,
          "typed channel setup starts from the shared bootstrap");
    Check(context.decoder_phase == CRYSTALHD_DECODER_BOOTSTRAPPED &&
          context.fw_sequence == 1 && !context.decoder_channel_id,
          "bootstrap publishes exactly one initialized command sequence");

    Check(crystalhd_decoder_channel_open_locked(&context, &frontend_owner,
                                                &config) == 0,
          "BCM70015 H.264 channel OPEN and INPUT_PARAMS succeed atomically");
    Check(context.state == BC_LINK_INIT &&
          context.decoder_phase == CRYSTALHD_DECODER_CHANNEL_CONFIGURED &&
          context.decoder_codec == CRYSTALHD_DECODER_CODEC_H264 &&
          context.fw_sequence == 3 && !context.decoder_channel_id &&
          context.session_owner == &frontend_owner && context.stream &&
          stream_prepares == 1 && !stream_releases,
          "typed channel setup publishes only the complete configured channel");
    Check(kernel_fw_requests == 1 && kernel_fw_releases == 1 &&
          downloads == 1 && download_resets == 1 && firmware_calls == 3 &&
          transaction_attempts == 4 && !strcmp(events, "QDL"),
          "typed channel setup reuses one download and three serialized commands");
    CheckKernelChannelPayload(&firmware_command_snapshots[1], true, 2);
    CheckKernelChannelPayload(&firmware_command_snapshots[2], false, 3);
}

static void KernelDecoderChannelOpenPreconditions(void)
{
    struct crystalhd_decoder_config config = {
        .codec = CRYSTALHD_DECODER_CODEC_H264,
    };
    int frontend_owner = 0, foreign_owner = 0;
    const void *owner;

    ResetBootstrapped(&frontend_owner);
    Check(crystalhd_decoder_channel_open_locked(NULL, &frontend_owner,
                                                &config) == -EINVAL &&
          crystalhd_decoder_channel_open_locked(&context, NULL,
                                                &config) == -EINVAL &&
          crystalhd_decoder_channel_open_locked(&context, &frontend_owner,
                                                NULL) == -EINVAL,
          "typed channel setup rejects missing arguments");
    Check(!firmware_calls && context.fw_sequence == 1 &&
          context.decoder_phase == CRYSTALHD_DECODER_BOOTSTRAPPED,
          "missing channel arguments have no command or state effects");

    ResetBootstrapped(&frontend_owner);
    owner = context.session_owner;
    context.adp = NULL;
    Check(crystalhd_decoder_channel_open_locked(&context, owner, &config) ==
              -ENODEV && !firmware_calls && context.fw_sequence == 1,
          "typed channel setup rejects a retired adapter before mutation");

    ResetBootstrapped(&frontend_owner);
    context.hw_ctx = NULL;
    Check(crystalhd_decoder_channel_open_locked(&context, &frontend_owner,
                                                &config) == -ENODEV &&
          !firmware_calls && context.fw_sequence == 1,
          "typed channel setup rejects a missing hardware context");

    ResetBootstrapped(&frontend_owner);
    hardware.pfnDoFirmwareCmd = NULL;
    Check(crystalhd_decoder_channel_open_locked(&context, &frontend_owner,
                                                &config) == -ENODEV &&
          !firmware_calls && context.fw_sequence == 1,
          "typed channel setup rejects a missing command transport");

    ResetBootstrapped(&frontend_owner);
    adapter.present = false;
    Check(crystalhd_decoder_channel_open_locked(&context, &frontend_owner,
                                                &config) == -ENODEV &&
          !firmware_calls && context.fw_sequence == 1,
          "typed channel setup rejects removal before command posting");

    ResetBootstrapped(&frontend_owner);
    context.session_owner = NULL;
    Check(crystalhd_decoder_channel_open_locked(&context, &frontend_owner,
                                                &config) == -EINVAL &&
          !firmware_calls && context.fw_sequence == 1,
          "typed channel setup requires an active session owner");

    ResetBootstrapped(&frontend_owner);
    Check(crystalhd_decoder_channel_open_locked(&context, &foreign_owner,
                                                &config) == -EBUSY &&
          !firmware_calls && context.fw_sequence == 1,
          "typed channel setup rejects a foreign session owner");

    ResetBootstrapped(&frontend_owner);
    endpoint.device = BC_PCI_DEVID_LINK;
    Check(crystalhd_decoder_channel_open_locked(&context, &frontend_owner,
                                                &config) == -EOPNOTSUPP &&
          !firmware_calls && context.fw_sequence == 1,
          "typed Flea channel setup rejects BCM70012 without posting Flea wire data");

    ResetBootstrapped(&frontend_owner);
    config.codec = (enum crystalhd_decoder_codec)UINT32_MAX;
    Check(crystalhd_decoder_channel_open_locked(&context, &frontend_owner,
                                                &config) == -EINVAL &&
          !firmware_calls && context.fw_sequence == 1,
          "typed channel setup rejects an unsupported codec before mutation");
    config.codec = CRYSTALHD_DECODER_CODEC_H264;

    ResetBootstrapped(&frontend_owner);
    stream_prepare_error = -ENOMEM;
    Check(crystalhd_decoder_channel_open_locked(&context, &frontend_owner,
                                                &config) == -ENOMEM &&
          stream_prepares == 1 && !stream_releases && !context.stream &&
          !firmware_calls && context.fw_sequence == 1 &&
          context.decoder_phase == CRYSTALHD_DECODER_BOOTSTRAPPED,
          "typed staging allocation fails before firmware mutation");

    for (unsigned which = 0; which < 3; which++) {
        ResetBootstrapped(&frontend_owner);
        if (which == 0)
            context.state = BC_LINK_RESUME;
        else if (which == 1)
            context.decoder_phase = CRYSTALHD_DECODER_COLD;
        else
            context.fw_sequence = 0;
        Check(crystalhd_decoder_channel_open_locked(&context, &frontend_owner,
                                                    &config) == -EBUSY &&
              !firmware_calls,
              "typed channel setup requires the exact bootstrap publication");
    }
}

static void KernelDecoderChannelOpenFailures(void)
{
    const struct crystalhd_decoder_config config = {
        .codec = CRYSTALHD_DECODER_CODEC_H264,
    };
    int frontend_owner = 0;

    ResetBootstrapped(&frontend_owner);
    firmware_status_call = 1;
    firmware_call_status = BC_STS_FW_CMD_ERR;
    Check(crystalhd_decoder_channel_open_locked(&context, &frontend_owner,
                                                &config) == -EIO &&
          context.decoder_phase == CRYSTALHD_DECODER_BOOTSTRAPPED &&
          context.fw_sequence == 2 && firmware_calls == 1 &&
          stream_prepares == 1 && stream_releases == 1 && !context.stream,
          "confirmed OPEN firmware rejection retains a retryable bootstrap");
    firmware_status_call = 0;
    Check(crystalhd_decoder_channel_open_locked(&context, &frontend_owner,
                                                &config) == 0 &&
          context.decoder_phase == CRYSTALHD_DECODER_CHANNEL_CONFIGURED &&
          context.fw_sequence == 4 && firmware_calls == 3,
          "retry after confirmed OPEN rejection consumes fresh sequence values");
    CheckKernelChannelPayload(&firmware_command_snapshots[1], true, 3);
    CheckKernelChannelPayload(&firmware_command_snapshots[2], false, 4);

    ResetBootstrapped(&frontend_owner);
    firmware_status = BC_STS_BUSY;
    Check(crystalhd_decoder_channel_open_locked(&context, &frontend_owner,
                                                &config) == -EBUSY &&
          context.decoder_phase == CRYSTALHD_DECODER_RECOVERY_REQUIRED &&
          context.fw_sequence == 2 && firmware_calls == 1,
          "ambiguous OPEN admission failure requires verified recovery");

    ResetBootstrapped(&frontend_owner);
    firmware_status = BC_STS_TIMEOUT;
    poison_on_firmware_timeout = true;
    transaction_mode = true;
    Check(crystalhd_decoder_channel_open_locked(&context, &frontend_owner,
                                                &config) == -ETIMEDOUT &&
          context.state == BC_LINK_INIT &&
          context.decoder_phase == CRYSTALHD_DECODER_RECOVERY_REQUIRED &&
          context.fw_sequence == 2 && hardware.fwcmd_poisoned &&
          firmware_calls == 1,
          "timed-out OPEN preserves active hardware state and requires reset");
    Check(crystalhd_decoder_channel_open_locked(&context, &frontend_owner,
                                                &config) == -EBUSY &&
          firmware_calls == 1,
          "timed-out OPEN cannot be retried without a fresh bootstrap");
    firmware_status = BC_STS_SUCCESS;
    poison_on_firmware_timeout = false;
    Check(crystalhd_fw_bootstrap_locked(&context, &frontend_owner) == 0 &&
          context.decoder_phase == CRYSTALHD_DECODER_BOOTSTRAPPED &&
          context.fw_sequence == 1 && !hardware.fwcmd_poisoned &&
          kernel_fw_requests == 1 && downloads == 1 && firmware_calls == 2,
          "verified redownload and INIT recover an ambiguous channel attempt");
    Check(crystalhd_decoder_channel_open_locked(&context, &frontend_owner,
                                                &config) == 0 &&
          context.decoder_phase == CRYSTALHD_DECODER_CHANNEL_CONFIGURED &&
          context.fw_sequence == 3 && firmware_calls == 4,
          "recovered channel setup restarts the firmware sequence at one");
    CheckKernelChannelPayload(&firmware_command_snapshots[2], true, 2);
    CheckKernelChannelPayload(&firmware_command_snapshots[3], false, 3);

    ResetBootstrapped(&frontend_owner);
    firmware_open_channel = 7;
    Check(crystalhd_decoder_channel_open_locked(&context, &frontend_owner,
                                                &config) == -EIO &&
          context.decoder_phase == CRYSTALHD_DECODER_RECOVERY_REQUIRED &&
          context.fw_sequence == 2 && !context.decoder_channel_id &&
          firmware_calls == 1,
          "nonzero Flea channel response is rejected as a partial open");

    ResetBootstrapped(&frontend_owner);
    remove_during_firmware_call = 1;
    Check(crystalhd_decoder_channel_open_locked(&context, &frontend_owner,
                                                &config) == -ENODEV &&
          !adapter.present &&
          context.decoder_phase == CRYSTALHD_DECODER_RECOVERY_REQUIRED &&
          context.fw_sequence == 2 && firmware_calls == 1,
          "removal at OPEN completion prevents INPUT and publication");

    ResetBootstrapped(&frontend_owner);
    firmware_status_call = 2;
    firmware_call_status = BC_STS_FW_CMD_ERR;
    Check(crystalhd_decoder_channel_open_locked(&context, &frontend_owner,
                                                &config) == -EIO &&
          context.decoder_phase == CRYSTALHD_DECODER_RECOVERY_REQUIRED &&
          context.fw_sequence == 3 && !context.decoder_channel_id &&
          firmware_calls == 2,
          "INPUT failure never publishes the already-open firmware channel");

    ResetBootstrapped(&frontend_owner);
    remove_during_firmware_call = 2;
    Check(crystalhd_decoder_channel_open_locked(&context, &frontend_owner,
                                                &config) == -ENODEV &&
          !adapter.present &&
          context.decoder_phase == CRYSTALHD_DECODER_RECOVERY_REQUIRED &&
          context.fw_sequence == 3 && firmware_calls == 2,
          "removal at INPUT completion prevents configured publication");
}

static void CheckKernelChannelStartPayload(const BC_FW_CMD *command,
                                           bool activating,
                                           uint32_t sequence)
{
    uint32_t expected[64] = {0};

    expected[0] = activating ? 0x73763102U : 0x7376311aU;
    expected[1] = sequence;
    if (!activating) {
        expected[18] = 1U;
        expected[20] = 1U;
        expected[32] = 1U;
    }

    for (unsigned word = 0; word < 64; word++) {
        Check(command->cmd[word] == expected[word],
              "typed decoder start submits the exact zero-filled request");
        Check(!command->rsp[word],
              "typed decoder start submits a zero-filled response buffer");
    }
    Check(!command->flags && !command->add_data,
          "typed decoder start leaves passthrough flags and data clear");
}

static void KernelDecoderChannelStartPayload(void)
{
    int frontend_owner = 0;

    ResetConfigured(&frontend_owner);
    raw_value = UINT32_MAX;
    Check(crystalhd_decoder_channel_start_locked(&context,
                                                 &frontend_owner) == 0,
          "BCM70015 channel ACTIVATE and START_VIDEO succeed atomically");
    Check(context.state == BC_LINK_INIT &&
          context.decoder_phase == CRYSTALHD_DECODER_CHANNEL_STARTED &&
          context.decoder_codec == CRYSTALHD_DECODER_CODEC_H264 &&
          context.fw_sequence == 5 && !context.decoder_channel_id &&
          context.session_owner == &frontend_owner,
          "typed decoder start publishes only the complete running channel");
    Check(raw_calls[0] == 1 && raw_calls[1] == 1 &&
          raw_offset == 0x00502100U && raw_value == 0x0000007eU &&
          !hardware.lock,
          "typed decoder start selects packed YUY2 before firmware activation");
    Check(firmware_calls == 2,
          "typed decoder start issues exactly two serialized commands");
    CheckKernelChannelStartPayload(&firmware_command_snapshots[0], true, 4);
    CheckKernelChannelStartPayload(&firmware_command_snapshots[1], false, 5);

    ResetConfigured(&frontend_owner);
    raw_value = 0x24U;
    Check(crystalhd_decoder_channel_start_locked(&context,
                                                 &frontend_owner) == 0 &&
          raw_offset == 0x00502100U && raw_value == 0x26U,
          "typed decoder start preserves independent live color controls");
}

static void KernelDecoderChannelStartPreconditions(void)
{
    int frontend_owner = 0, foreign_owner = 0;
    const void *owner;

    ResetConfigured(&frontend_owner);
    Check(crystalhd_decoder_channel_start_locked(NULL, &frontend_owner) ==
              -EINVAL &&
          crystalhd_decoder_channel_start_locked(&context, NULL) == -EINVAL,
          "typed decoder start rejects missing arguments");
    Check(!firmware_calls && !raw_calls[0] && !raw_calls[1] &&
          context.fw_sequence == 3,
          "missing start arguments have no hardware or state effects");

    ResetConfigured(&frontend_owner);
    owner = context.session_owner;
    context.adp = NULL;
    Check(crystalhd_decoder_channel_start_locked(&context, owner) == -ENODEV,
          "typed decoder start rejects a retired adapter");

    ResetConfigured(&frontend_owner);
    context.adp->pdev = NULL;
    Check(crystalhd_decoder_channel_start_locked(&context,
                                                 &frontend_owner) == -ENODEV,
          "typed decoder start rejects a missing PCI endpoint");

    ResetConfigured(&frontend_owner);
    context.hw_ctx = NULL;
    Check(crystalhd_decoder_channel_start_locked(&context,
                                                 &frontend_owner) == -ENODEV,
          "typed decoder start rejects a missing hardware context");

    for (unsigned callback = 0; callback < 3; callback++) {
        ResetConfigured(&frontend_owner);
        if (callback == 0)
            hardware.pfnDoFirmwareCmd = NULL;
        else if (callback == 1)
            hardware.pfnReadDevRegister = NULL;
        else
            hardware.pfnWriteDevRegister = NULL;
        Check(crystalhd_decoder_channel_start_locked(&context,
                                                     &frontend_owner) ==
                  -ENODEV && !firmware_calls && !raw_calls[0] &&
                  !raw_calls[1] && context.fw_sequence == 3,
              "typed decoder start requires every hardware callback");
    }

    ResetConfigured(&frontend_owner);
    adapter.present = false;
    Check(crystalhd_decoder_channel_start_locked(&context,
                                                 &frontend_owner) == -ENODEV,
          "typed decoder start rejects removal before hardware access");

    ResetConfigured(&frontend_owner);
    context.session_owner = NULL;
    Check(crystalhd_decoder_channel_start_locked(&context,
                                                 &frontend_owner) == -EINVAL,
          "typed decoder start requires an active session owner");

    ResetConfigured(&frontend_owner);
    Check(crystalhd_decoder_channel_start_locked(&context, &foreign_owner) ==
              -EBUSY,
          "typed decoder start rejects a foreign session owner");

    ResetConfigured(&frontend_owner);
    endpoint.device = BC_PCI_DEVID_LINK;
    Check(crystalhd_decoder_channel_start_locked(&context,
                                                 &frontend_owner) ==
              -EOPNOTSUPP,
          "typed Flea start rejects BCM70012 before hardware access");

    for (unsigned phase = CRYSTALHD_DECODER_COLD;
         phase <= CRYSTALHD_DECODER_RECOVERY_REQUIRED; phase++) {
        if (phase == CRYSTALHD_DECODER_CHANNEL_CONFIGURED)
            continue;
        ResetConfigured(&frontend_owner);
        context.decoder_phase = (enum crystalhd_decoder_phase)phase;
        Check(crystalhd_decoder_channel_start_locked(&context,
                                                     &frontend_owner) ==
                  -EBUSY,
              "typed decoder start requires exactly the configured phase");
    }

    for (unsigned condition = 0; condition < 4; condition++) {
        ResetConfigured(&frontend_owner);
        if (condition == 0)
            context.state = BC_LINK_RESUME;
        else if (condition == 1)
            context.fw_sequence = 0;
        else if (condition == 2)
            context.decoder_channel_id = 7;
        else
            context.decoder_codec = CRYSTALHD_DECODER_CODEC_INVALID;
        Check(crystalhd_decoder_channel_start_locked(&context,
                                                     &frontend_owner) ==
                  (condition == 3 ? -EINVAL : -EBUSY) &&
              !firmware_calls && !raw_calls[0] && !raw_calls[1],
              "typed decoder start rejects an inconsistent publication");
    }
}

static void KernelDecoderChannelStartFailures(void)
{
    const struct crystalhd_decoder_config config = {
        .codec = CRYSTALHD_DECODER_CODEC_H264,
    };
    int frontend_owner = 0;

    ResetConfigured(&frontend_owner);
    firmware_status_call = 1;
    firmware_call_status = BC_STS_FW_CMD_ERR;
    Check(crystalhd_decoder_channel_start_locked(&context,
                                                 &frontend_owner) == -EIO &&
          context.decoder_phase == CRYSTALHD_DECODER_CHANNEL_CONFIGURED &&
          context.decoder_codec == CRYSTALHD_DECODER_CODEC_H264 &&
          context.fw_sequence == 4 && firmware_calls == 1,
          "confirmed ACTIVATE firmware rejection retains configured retry state");
    firmware_status_call = 0;
    Check(crystalhd_decoder_channel_start_locked(&context,
                                                 &frontend_owner) == 0 &&
          context.decoder_phase == CRYSTALHD_DECODER_CHANNEL_STARTED &&
          context.fw_sequence == 6 && firmware_calls == 3,
          "ACTIVATE retry consumes fresh sequence values and starts once");
    CheckKernelChannelStartPayload(&firmware_command_snapshots[1], true, 5);
    CheckKernelChannelStartPayload(&firmware_command_snapshots[2], false, 6);

    ResetConfigured(&frontend_owner);
    firmware_status = BC_STS_BUSY;
    Check(crystalhd_decoder_channel_start_locked(&context,
                                                 &frontend_owner) == -EBUSY &&
          context.decoder_phase == CRYSTALHD_DECODER_RECOVERY_REQUIRED &&
          context.decoder_codec == CRYSTALHD_DECODER_CODEC_INVALID &&
          context.fw_sequence == 4 && firmware_calls == 1,
          "ambiguous ACTIVATE failure requires verified recovery");

    ResetConfigured(&frontend_owner);
    firmware_status = BC_STS_TIMEOUT;
    poison_on_firmware_timeout = true;
    Check(crystalhd_decoder_channel_start_locked(&context,
                                                 &frontend_owner) ==
              -ETIMEDOUT &&
          context.decoder_phase == CRYSTALHD_DECODER_RECOVERY_REQUIRED &&
          context.fw_sequence == 4 && hardware.fwcmd_poisoned &&
          firmware_calls == 1,
          "timed-out ACTIVATE quarantines the mailbox and decoder state");
    Check(crystalhd_decoder_channel_start_locked(&context,
                                                 &frontend_owner) == -EBUSY &&
          firmware_calls == 1,
          "timed-out ACTIVATE cannot be retried without fresh bootstrap");
    firmware_status = BC_STS_SUCCESS;
    poison_on_firmware_timeout = false;
    Check(crystalhd_fw_bootstrap_locked(&context, &frontend_owner) == 0 &&
          crystalhd_decoder_channel_open_locked(&context, &frontend_owner,
                                                &config) == 0 &&
          crystalhd_decoder_channel_start_locked(&context,
                                                 &frontend_owner) == 0 &&
          context.decoder_phase == CRYSTALHD_DECODER_CHANNEL_STARTED &&
          context.fw_sequence == 5 && !hardware.fwcmd_poisoned &&
          firmware_calls == 6,
          "fresh bootstrap, open and start recover an ambiguous activation");
    CheckKernelChannelStartPayload(&firmware_command_snapshots[4], true, 4);
    CheckKernelChannelStartPayload(&firmware_command_snapshots[5], false, 5);

    ResetConfigured(&frontend_owner);
    firmware_status_call = 2;
    firmware_call_status = BC_STS_FW_CMD_ERR;
    Check(crystalhd_decoder_channel_start_locked(&context,
                                                 &frontend_owner) == -EIO &&
          context.decoder_phase == CRYSTALHD_DECODER_RECOVERY_REQUIRED &&
          context.decoder_codec == CRYSTALHD_DECODER_CODEC_INVALID &&
          context.fw_sequence == 5 && firmware_calls == 2,
          "START rejection cannot publish or retry an activated channel");

    ResetConfigured(&frontend_owner);
    firmware_status_call = 2;
    firmware_call_status = BC_STS_TIMEOUT;
    poison_on_firmware_timeout = true;
    Check(crystalhd_decoder_channel_start_locked(&context,
                                                 &frontend_owner) ==
              -ETIMEDOUT &&
          context.decoder_phase == CRYSTALHD_DECODER_RECOVERY_REQUIRED &&
          context.fw_sequence == 5 && hardware.fwcmd_poisoned &&
          firmware_calls == 2,
          "timed-out START quarantines the partial activation");

    ResetConfigured(&frontend_owner);
    remove_during_firmware_call = 1;
    Check(crystalhd_decoder_channel_start_locked(&context,
                                                 &frontend_owner) ==
              -ENODEV && !adapter.present &&
          context.decoder_phase == CRYSTALHD_DECODER_RECOVERY_REQUIRED &&
          context.fw_sequence == 4 && firmware_calls == 1,
          "removal after ACTIVATE prevents START and running publication");

    ResetConfigured(&frontend_owner);
    remove_during_firmware_call = 2;
    Check(crystalhd_decoder_channel_start_locked(&context,
                                                 &frontend_owner) ==
              -ENODEV && !adapter.present &&
          context.decoder_phase == CRYSTALHD_DECODER_RECOVERY_REQUIRED &&
          context.fw_sequence == 5 && firmware_calls == 2,
          "removal after START prevents running publication");
}

static void CheckKernelChannelStopClosePayload(const BC_FW_CMD *command,
                                               bool closing,
                                               uint32_t sequence)
{
    uint32_t expected[64] = {0};

    expected[0] = closing ? 0x73763101U : 0x7376311bU;
    expected[1] = sequence;
    expected[3] = 1U;

    for (unsigned word = 0; word < 64; word++) {
        Check(command->cmd[word] == expected[word],
              "typed decoder teardown submits the exact zero-filled request");
        Check(!command->rsp[word],
              "typed decoder teardown submits a zero-filled response buffer");
    }
    Check(!command->flags && !command->add_data,
          "typed decoder teardown leaves passthrough flags and data clear");
}

static void KernelDecoderChannelStopClosePayload(void)
{
    const struct crystalhd_decoder_config config = {
        .codec = CRYSTALHD_DECODER_CODEC_H264,
    };
    int frontend_owner = 0;

    ResetStarted(&frontend_owner);
    Check(crystalhd_decoder_channel_stop_locked(&context,
                                                &frontend_owner) == 0 &&
          context.state == BC_LINK_INIT &&
          context.decoder_phase == CRYSTALHD_DECODER_CHANNEL_CONFIGURED &&
          context.decoder_codec == CRYSTALHD_DECODER_CODEC_H264 &&
          context.fw_sequence == 6 && !context.decoder_channel_id &&
          context.session_owner == &frontend_owner &&
          context.hw_ctx == &hardware,
          "typed STOP publishes a restartable configured channel");
    Check(firmware_calls == 1 && !raw_calls[0] && !raw_calls[1] &&
          !captures && !cancels && !ring_frees && !hardware_closes,
          "firmware STOP does not claim host DMA or resource teardown");
    CheckKernelChannelStopClosePayload(&firmware_command_snapshots[0],
                                       false, 6);

    Check(crystalhd_decoder_channel_close_locked(&context,
                                                 &frontend_owner) == 0 &&
          context.state == BC_LINK_INIT &&
          context.decoder_phase == CRYSTALHD_DECODER_BOOTSTRAPPED &&
          context.decoder_codec == CRYSTALHD_DECODER_CODEC_INVALID &&
          context.fw_sequence == 7 && !context.decoder_channel_id &&
          context.session_owner == &frontend_owner &&
          context.hw_ctx == &hardware && !context.stream &&
          stream_releases == 1,
          "typed CLOSE retains the bootstrapped owner and command sequence");
    Check(firmware_calls == 2 && !captures && !cancels &&
          !ring_frees && !hardware_closes,
          "firmware CLOSE leaves host resource retirement to its caller");
    CheckKernelChannelStopClosePayload(&firmware_command_snapshots[1],
                                       true, 7);

    Check(crystalhd_decoder_channel_open_locked(&context, &frontend_owner,
                                                &config) == 0 &&
          crystalhd_decoder_channel_start_locked(&context,
                                                 &frontend_owner) == 0 &&
          context.decoder_phase == CRYSTALHD_DECODER_CHANNEL_STARTED &&
          context.fw_sequence == 11 && firmware_calls == 6,
          "normal close preserves monotonic sequence and permits reopen");
    CheckKernelChannelPayload(&firmware_command_snapshots[2], true, 8);
    CheckKernelChannelPayload(&firmware_command_snapshots[3], false, 9);
    CheckKernelChannelStartPayload(&firmware_command_snapshots[4], true, 10);
    CheckKernelChannelStartPayload(&firmware_command_snapshots[5], false, 11);

    ResetStarted(&frontend_owner);
    context.state = BC_LINK_READY;
    Check(crystalhd_decoder_channel_stop_locked(&context,
                                                &frontend_owner) == 0 &&
          context.state == BC_LINK_READY &&
          context.decoder_phase == CRYSTALHD_DECODER_CHANNEL_CONFIGURED &&
          crystalhd_decoder_channel_close_locked(&context,
                                                 &frontend_owner) == -EBUSY &&
          firmware_calls == 1 && !captures,
          "active capture must be reclaimed between firmware STOP and CLOSE");
    context.state = BC_LINK_INIT;
    Check(crystalhd_decoder_channel_close_locked(&context,
                                                 &frontend_owner) == 0 &&
          context.decoder_phase == CRYSTALHD_DECODER_BOOTSTRAPPED &&
          firmware_calls == 2,
          "CLOSE succeeds after the caller publishes completed RX teardown");

    ResetConfigured(&frontend_owner);
    Check(crystalhd_decoder_channel_close_locked(&context,
                                                 &frontend_owner) == 0 &&
          context.decoder_phase == CRYSTALHD_DECODER_BOOTSTRAPPED &&
          context.decoder_codec == CRYSTALHD_DECODER_CODEC_INVALID &&
          context.fw_sequence == 4 && firmware_calls == 1,
          "a configured channel that never started closes without STOP");
    CheckKernelChannelStopClosePayload(&firmware_command_snapshots[0],
                                       true, 4);

    ResetStarted(&frontend_owner);
    hardware.pfnReadDevRegister = NULL;
    hardware.pfnWriteDevRegister = NULL;
    Check(crystalhd_decoder_channel_stop_locked(&context,
                                                &frontend_owner) == 0 &&
          crystalhd_decoder_channel_close_locked(&context,
                                                 &frontend_owner) == 0 &&
          firmware_calls == 2 &&
          context.decoder_phase == CRYSTALHD_DECODER_BOOTSTRAPPED,
          "typed STOP/CLOSE does not depend on color-register callbacks");

    ResetStarted(&frontend_owner);
    Check(crystalhd_decoder_channel_stop_locked(&context,
                                                &frontend_owner) == 0 &&
          crystalhd_decoder_channel_start_locked(&context,
                                                 &frontend_owner) == 0 &&
          context.decoder_phase == CRYSTALHD_DECODER_CHANNEL_STARTED &&
          context.decoder_codec == CRYSTALHD_DECODER_CODEC_H264 &&
          context.fw_sequence == 8 && firmware_calls == 3,
          "typed STOP returns the channel to the existing restart path");
    CheckKernelChannelStopClosePayload(&firmware_command_snapshots[0],
                                       false, 6);
    CheckKernelChannelStartPayload(&firmware_command_snapshots[1], true, 7);
    CheckKernelChannelStartPayload(&firmware_command_snapshots[2], false, 8);
}

static void KernelDecoderChannelStopClosePreconditions(void)
{
    int (*transition[2])(struct crystalhd_cmd *, const void *) = {
        crystalhd_decoder_channel_stop_locked,
        crystalhd_decoder_channel_close_locked,
    };
    int frontend_owner = 0, foreign_owner = 0;
    const void *owner;

    for (unsigned operation = 0; operation < 2; operation++) {
        if (operation)
            ResetConfigured(&frontend_owner);
        else
            ResetStarted(&frontend_owner);
        Check(transition[operation](NULL, &frontend_owner) == -EINVAL &&
              transition[operation](&context, NULL) == -EINVAL,
              "typed STOP/CLOSE rejects missing arguments");
        Check(!firmware_calls && context.fw_sequence ==
                  (operation ? 3U : 5U),
              "missing teardown arguments have no firmware effects");

        if (operation)
            ResetConfigured(&frontend_owner);
        else
            ResetStarted(&frontend_owner);
        owner = context.session_owner;
        context.adp = NULL;
        Check(transition[operation](&context, owner) == -ENODEV,
              "typed STOP/CLOSE rejects a retired adapter");

        if (operation)
            ResetConfigured(&frontend_owner);
        else
            ResetStarted(&frontend_owner);
        context.adp->pdev = NULL;
        Check(transition[operation](&context, &frontend_owner) == -ENODEV,
              "typed STOP/CLOSE rejects a missing PCI endpoint");

        if (operation)
            ResetConfigured(&frontend_owner);
        else
            ResetStarted(&frontend_owner);
        context.hw_ctx = NULL;
        Check(transition[operation](&context, &frontend_owner) == -ENODEV,
              "typed STOP/CLOSE rejects a missing hardware context");

        if (operation)
            ResetConfigured(&frontend_owner);
        else
            ResetStarted(&frontend_owner);
        hardware.pfnDoFirmwareCmd = NULL;
        Check(transition[operation](&context, &frontend_owner) == -ENODEV &&
              !firmware_calls,
              "typed STOP/CLOSE requires the firmware callback only");

        if (operation)
            ResetConfigured(&frontend_owner);
        else
            ResetStarted(&frontend_owner);
        adapter.present = false;
        Check(transition[operation](&context, &frontend_owner) == -ENODEV,
              "typed STOP/CLOSE rejects removal before firmware access");

        if (operation)
            ResetConfigured(&frontend_owner);
        else
            ResetStarted(&frontend_owner);
        context.session_owner = NULL;
        Check(transition[operation](&context, &frontend_owner) == -EINVAL,
              "typed STOP/CLOSE requires an active session owner");

        if (operation)
            ResetConfigured(&frontend_owner);
        else
            ResetStarted(&frontend_owner);
        Check(transition[operation](&context, &foreign_owner) == -EBUSY,
              "typed STOP/CLOSE rejects a foreign session owner");

        if (operation)
            ResetConfigured(&frontend_owner);
        else
            ResetStarted(&frontend_owner);
        endpoint.device = BC_PCI_DEVID_LINK;
        Check(transition[operation](&context, &frontend_owner) ==
                  -EOPNOTSUPP,
              "typed STOP/CLOSE rejects BCM70012 before firmware access");

        for (unsigned condition = 0; condition < 4; condition++) {
            if (operation)
                ResetConfigured(&frontend_owner);
            else
                ResetStarted(&frontend_owner);
            if (condition == 0)
                context.state = BC_LINK_RESUME;
            else if (condition == 1)
                context.fw_sequence = 0;
            else if (condition == 2)
                context.decoder_channel_id = 7;
            else
                context.decoder_codec = CRYSTALHD_DECODER_CODEC_INVALID;
            Check(transition[operation](&context, &frontend_owner) ==
                      (condition == 3 ? -EINVAL : -EBUSY) &&
                  !firmware_calls && !raw_calls[0] && !raw_calls[1],
                  "typed STOP/CLOSE rejects inconsistent cached state");
        }
    }

    ResetStarted(&frontend_owner);
    context.state = BC_LINK_INIT | BC_LINK_PAUSED;
    Check(crystalhd_decoder_channel_stop_locked(&context,
                                                &frontend_owner) == -EBUSY &&
          !firmware_calls,
          "typed STOP rejects unsupported paused-session teardown");

    ResetStarted(&frontend_owner);
    context.decoder_phase = CRYSTALHD_DECODER_CHANNEL_CONFIGURED;
    Check(crystalhd_decoder_channel_stop_locked(&context,
                                                &frontend_owner) == -EBUSY,
          "typed STOP requires a started channel");

    ResetConfigured(&frontend_owner);
    context.decoder_phase = CRYSTALHD_DECODER_CHANNEL_STARTED;
    Check(crystalhd_decoder_channel_close_locked(&context,
                                                 &frontend_owner) == -EBUSY,
          "typed CLOSE requires a configured or stopped channel");

    ResetConfigured(&frontend_owner);
    context.state = BC_LINK_READY;
    Check(crystalhd_decoder_channel_close_locked(&context,
                                                 &frontend_owner) == -EBUSY &&
          !firmware_calls,
          "typed CLOSE rejects unreclaimed active capture state");
}

static void KernelDecoderChannelStopCloseFailures(void)
{
    const struct crystalhd_decoder_config config = {
        .codec = CRYSTALHD_DECODER_CODEC_H264,
    };
    int frontend_owner = 0;

    ResetStarted(&frontend_owner);
    firmware_status_call = 1;
    firmware_call_status = BC_STS_FW_CMD_ERR;
    Check(crystalhd_decoder_channel_stop_locked(&context,
                                                &frontend_owner) == -EIO &&
          context.decoder_phase == CRYSTALHD_DECODER_CHANNEL_STARTED &&
          context.decoder_codec == CRYSTALHD_DECODER_CODEC_H264 &&
          context.fw_sequence == 6 && firmware_calls == 1,
          "confirmed STOP rejection retains the started retry state");
    firmware_status_call = 0;
    Check(crystalhd_decoder_channel_stop_locked(&context,
                                                &frontend_owner) == 0 &&
          crystalhd_decoder_channel_close_locked(&context,
                                                 &frontend_owner) == 0 &&
          context.decoder_phase == CRYSTALHD_DECODER_BOOTSTRAPPED &&
          context.fw_sequence == 8 && firmware_calls == 3,
          "STOP retry and CLOSE consume fresh sequence values");
    CheckKernelChannelStopClosePayload(&firmware_command_snapshots[1],
                                       false, 7);
    CheckKernelChannelStopClosePayload(&firmware_command_snapshots[2],
                                       true, 8);

    ResetStarted(&frontend_owner);
    firmware_status = BC_STS_BUSY;
    Check(crystalhd_decoder_channel_stop_locked(&context,
                                                &frontend_owner) == -EBUSY &&
          context.decoder_phase == CRYSTALHD_DECODER_RECOVERY_REQUIRED &&
          context.decoder_codec == CRYSTALHD_DECODER_CODEC_INVALID &&
          !context.decoder_channel_id && context.fw_sequence == 6 &&
          firmware_calls == 1,
          "ambiguous STOP failure requires verified recovery");

    ResetStarted(&frontend_owner);
    firmware_status = BC_STS_TIMEOUT;
    poison_on_firmware_timeout = true;
    Check(crystalhd_decoder_channel_stop_locked(&context,
                                                &frontend_owner) ==
              -ETIMEDOUT &&
          context.decoder_phase == CRYSTALHD_DECODER_RECOVERY_REQUIRED &&
          context.decoder_codec == CRYSTALHD_DECODER_CODEC_INVALID &&
          context.fw_sequence == 6 && hardware.fwcmd_poisoned &&
          firmware_calls == 1,
          "timed-out STOP quarantines the mailbox and decoder state");
    Check(crystalhd_decoder_channel_stop_locked(&context,
                                                &frontend_owner) == -EBUSY &&
          firmware_calls == 1,
          "timed-out STOP cannot be retried without fresh bootstrap");
    firmware_status = BC_STS_SUCCESS;
    poison_on_firmware_timeout = false;
    Check(crystalhd_fw_bootstrap_locked(&context, &frontend_owner) == 0 &&
          crystalhd_decoder_channel_open_locked(&context, &frontend_owner,
                                                &config) == 0 &&
          crystalhd_decoder_channel_start_locked(&context,
                                                 &frontend_owner) == 0 &&
          crystalhd_decoder_channel_stop_locked(&context,
                                                &frontend_owner) == 0 &&
          crystalhd_decoder_channel_close_locked(&context,
                                                 &frontend_owner) == 0 &&
          context.decoder_phase == CRYSTALHD_DECODER_BOOTSTRAPPED &&
          context.fw_sequence == 7 && !hardware.fwcmd_poisoned &&
          firmware_calls == 8,
          "fresh bootstrap recovers the complete start and close lifecycle");
    CheckKernelChannelStopClosePayload(&firmware_command_snapshots[6],
                                       false, 6);
    CheckKernelChannelStopClosePayload(&firmware_command_snapshots[7],
                                       true, 7);

    ResetStarted(&frontend_owner);
    remove_during_firmware_call = 1;
    Check(crystalhd_decoder_channel_stop_locked(&context,
                                                &frontend_owner) ==
              -ENODEV && !adapter.present &&
          context.decoder_phase == CRYSTALHD_DECODER_RECOVERY_REQUIRED &&
          context.decoder_codec == CRYSTALHD_DECODER_CODEC_INVALID &&
          context.fw_sequence == 6 && firmware_calls == 1,
          "removal after STOP prevents configured publication");

    ResetStarted(&frontend_owner);
    firmware_status_call = remove_during_firmware_call = 1;
    firmware_call_status = BC_STS_FW_CMD_ERR;
    Check(crystalhd_decoder_channel_stop_locked(&context,
                                                &frontend_owner) ==
              -ENODEV &&
          context.decoder_phase == CRYSTALHD_DECODER_RECOVERY_REQUIRED,
          "removal overrides an otherwise retryable STOP rejection");

    ResetStarted(&frontend_owner);
    Check(crystalhd_decoder_channel_stop_locked(&context,
                                                &frontend_owner) == 0,
          "STOP succeeds before testing CLOSE rejection");
    firmware_status_call = 2;
    firmware_call_status = BC_STS_FW_CMD_ERR;
    Check(crystalhd_decoder_channel_close_locked(&context,
                                                 &frontend_owner) == -EIO &&
          context.decoder_phase == CRYSTALHD_DECODER_CHANNEL_CONFIGURED &&
          context.decoder_codec == CRYSTALHD_DECODER_CODEC_H264 &&
          context.fw_sequence == 7 && firmware_calls == 2,
          "confirmed CLOSE rejection retains the configured retry state");
    firmware_status_call = 0;
    Check(crystalhd_decoder_channel_close_locked(&context,
                                                 &frontend_owner) == 0 &&
          context.decoder_phase == CRYSTALHD_DECODER_BOOTSTRAPPED &&
          context.fw_sequence == 8 && firmware_calls == 3,
          "CLOSE retry succeeds with a fresh sequence value");

    ResetConfigured(&frontend_owner);
    firmware_status = BC_STS_BUSY;
    Check(crystalhd_decoder_channel_close_locked(&context,
                                                 &frontend_owner) == -EBUSY &&
          context.decoder_phase == CRYSTALHD_DECODER_RECOVERY_REQUIRED &&
          context.decoder_codec == CRYSTALHD_DECODER_CODEC_INVALID &&
          context.fw_sequence == 4 && firmware_calls == 1,
          "ambiguous CLOSE failure requires verified recovery");

    ResetConfigured(&frontend_owner);
    firmware_status = BC_STS_TIMEOUT;
    poison_on_firmware_timeout = true;
    Check(crystalhd_decoder_channel_close_locked(&context,
                                                 &frontend_owner) ==
              -ETIMEDOUT &&
          context.decoder_phase == CRYSTALHD_DECODER_RECOVERY_REQUIRED &&
          context.fw_sequence == 4 && hardware.fwcmd_poisoned &&
          firmware_calls == 1,
          "timed-out CLOSE quarantines the mailbox and decoder state");

    ResetConfigured(&frontend_owner);
    remove_during_firmware_call = 1;
    Check(crystalhd_decoder_channel_close_locked(&context,
                                                 &frontend_owner) ==
              -ENODEV && !adapter.present &&
          context.decoder_phase == CRYSTALHD_DECODER_RECOVERY_REQUIRED &&
          context.decoder_codec == CRYSTALHD_DECODER_CODEC_INVALID &&
          context.fw_sequence == 4 && firmware_calls == 1,
          "removal after CLOSE prevents bootstrapped publication");
}

static void KernelDecoderChannelSuspendRecovery(void)
{
    crystalhd_ioctl_data data = {0};
    int frontend_owner = 0;

    ResetFrontendOwned(BC_LINK_INIT, true, &frontend_owner);
    context.decoder_phase = CRYSTALHD_DECODER_CHANNEL_CONFIGURED;
    context.decoder_codec = CRYSTALHD_DECODER_CODEC_H264;
    context.fw_sequence = 3;
    context.decoder_channel_id = 0;
    Check(crystalhd_suspend(&context, &data) == BC_STS_SUCCESS &&
          context.state == BC_LINK_SUSPEND &&
          context.decoder_phase == CRYSTALHD_DECODER_RECOVERY_REQUIRED &&
          context.decoder_codec == CRYSTALHD_DECODER_CODEC_INVALID &&
          !context.decoder_channel_id,
          "suspend invalidates a configured typed firmware channel");
    Check(crystalhd_resume(&context) == BC_STS_SUCCESS &&
          context.state == BC_LINK_RESUME &&
          context.decoder_phase == CRYSTALHD_DECODER_RECOVERY_REQUIRED,
          "resume retains the fresh-bootstrap requirement");
    Check(crystalhd_fw_bootstrap_locked(&context, &frontend_owner) == 0 &&
          context.state == BC_LINK_INIT &&
          context.decoder_phase == CRYSTALHD_DECODER_BOOTSTRAPPED &&
          context.fw_sequence == 1,
          "resumed typed session recovers through verified bootstrap");

    ResetFrontendOwned(BC_LINK_INIT, true, &frontend_owner);
    context.decoder_phase = CRYSTALHD_DECODER_CHANNEL_STARTED;
    context.decoder_codec = CRYSTALHD_DECODER_CODEC_H264;
    context.fw_sequence = 5;
    Check(crystalhd_suspend(&context, &data) == BC_STS_SUCCESS &&
          context.state == BC_LINK_SUSPEND &&
          context.decoder_phase == CRYSTALHD_DECODER_RECOVERY_REQUIRED &&
          context.decoder_codec == CRYSTALHD_DECODER_CODEC_INVALID,
          "suspend invalidates a started typed firmware channel");
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
    const void *owner;
    BC_STATUS status;
};

static void *RunFirmwareDownload(void *argument)
{
    struct download_thread *thread = argument;

    thread->status = crystalhd_fw_download_locked(&context, thread->owner,
                                                   thread->data.add_cdata,
                                                   thread->data.add_cdata_sz);
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
    _Alignas(uint32_t) uint8_t firmware[CRYSTALHD_FLEA_MIN_FIRMWARE_SIZE] = {0};
    struct download_thread download = {0};
    struct firmware_thread command = {0};
    pthread_t download_tid, command_tid;

    ResetOwned(BC_LINK_INVALID, true);
    firmware[0] = 0x5a;
    transaction_mode = true;
    block_download_reset = true;
    download.data.add_cdata = firmware;
    download.data.add_cdata_sz = sizeof(firmware);
    download.owner = context.session_owner;
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

    ResetOwned(BC_LINK_INIT | BC_LINK_PAUSED, true);
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
static void *RunHardwareClose(void *argument)
{
    struct suspend_thread *thread = argument;

    thread->status = crystalhd_hw_close_actual(&hardware);
    return NULL;
}

static void FirmwareSuspendSerialization(void)
{
    for (unsigned closing = 0; closing < 2; closing++) {
        struct firmware_thread command = {0};
        struct suspend_thread suspend = {0};
        pthread_t command_thread, suspend_thread;

        ResetOwned(BC_LINK_INIT | BC_LINK_PAUSED, true);
        adapter.cfg_users = 2;
        transaction_mode = true;
        block_first_firmware = true;
        command.data.udata.u.fwCmd.cmd[0] = eCMD_C011_DEC_CHAN_PAUSE;
        command.data.udata.u.fwCmd.cmd[3] = 0;

        if (pthread_create(&command_thread, NULL, RunFirmwareTransaction,
                           &command)) abort();
        WaitForFirstFirmware();
        if (pthread_create(&suspend_thread, NULL,
                           closing ? RunHardwareClose : RunHardwareSuspend,
                           &suspend)) abort();
        WaitForTransactionAttempts(2);
        Check(!stops && hardware.dev_started,
              "hardware suspend and close wait for the whole firmware transaction");

        ReleaseFirstFirmware();
        if (pthread_join(command_thread, NULL) ||
            pthread_join(suspend_thread, NULL)) abort();
        Check(command.status == BC_STS_TIMEOUT && suspend.status == BC_STS_SUCCESS &&
              stops == 1 && (!closing || !hardware.dev_started),
              "hardware stops only after firmware timeout recovery releases serialization");
        Check((context.state & BC_LINK_PAUSED) && pause_calls == 2 &&
              !pause_states[0] && pause_states[1] && hardware.fetch_sem == 1,
              "device stop observes the command layer after its local rollback is complete");
        transaction_mode = false;
    }
}
static void HardwareClose(void)
{
    const unsigned counts[] = { 0, 1, 2, BC_LINK_MAX_OPENS, UINT32_MAX };

    for (unsigned n = 0; n < sizeof(counts) / sizeof(counts[0]); n++) {
        for (unsigned started = 0; started < 2; started++) {
            for (unsigned fail = 0; fail < 2; fail++) {
                Reset(BC_LINK_INVALID, true);
                adapter.cfg_users = counts[n];
                hardware.dev_started = started;
                stop_ok = !fail;
                Check((crystalhd_hw_close_actual(&hardware) != BC_STS_SUCCESS) ==
                      (bool)(started && fail),
                      "hardware close propagates an unsuccessful device-stop barrier");
                Check(stops == started && hardware.dev_started == (bool)(started && fail) &&
                      adapter.cfg_users == counts[n] && !starts && !captures && !tx_stops,
                      "only a successful stop retires a started device regardless of file count");
                Check((crystalhd_hw_close_actual(&hardware) != BC_STS_SUCCESS) ==
                      (bool)(started && fail) &&
                      stops == started,
                      "repeated hardware close neither retries a fatal engine nor stops a retired device");
                Check(hardware.dma_fault == (bool)(started && fail) &&
                      adapter.present == !(started && fail),
                      "device-stop failure remains terminal rather than allowing reopen");
            }
        }
    }
    Reset(BC_LINK_INVALID, false);
    Check(crystalhd_hw_close_actual(NULL) == BC_STS_SUCCESS && !stops,
          "NULL hardware close remains a successful no-op");
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
          !rings_live && ring_frees == 1 && hardware_closes == 1 && !capture_unmaps &&
          !irq_depth && irq_disables == 2 + stops && irq_enables == 2 + stops,
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
          elem_deletes == 2 && dio_destroys == 2 && !capture_unmaps && !irq_depth,
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
    Check(hardware_opens == 1 && hardware_closes == 1 && !capture_unmaps &&
          ring_frees == 1 && dio_destroys == 1 && elem_deletes == 1 &&
          irq_disables == 2 + stops && irq_enables == 2 + stops && !irq_depth,
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
          elem_deletes == 1 && !capture_unmaps,
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
          elem_deletes == 2 && !capture_unmaps && irq_disables == 4 + stops &&
          irq_enables == 4 + stops && !irq_depth,
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
          !context.session_owner && !context.retain_rx_on_suspend &&
          !context.session_module_pinned && !module_refs && module_gets == module_puts &&
          !context.hw_ctx && !hardware_allocated && !elem_live && !dio_live &&
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
    Check(bc_cproc_notify_mode(&context, &data) == BC_STS_SUCCESS &&
          context.retain_rx_on_suspend,
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
static void QuiescedRxPolicy(void)
{
    static const uint32_t states[] = {
        BC_LINK_INVALID, BC_LINK_INIT, BC_LINK_READY,
        BC_LINK_READY | BC_LINK_CAP_EN, BC_LINK_SUSPEND, BC_LINK_RESUME,
    };

    for (unsigned state = 0; state < sizeof(states) / sizeof(states[0]); state++) {
        for (unsigned retain = 0; retain < 2; retain++) {
            for (unsigned suspend_only = 0; suspend_only < 2; suspend_only++) {
                struct crystalhd_cmd before;
                struct crystalhd_hw hardware_before;
                unsigned expected = !(retain && suspend_only);

                Reset(states[state], true);
                context.retain_rx_on_suspend = retain;
                before = context;
                hardware_before = hardware;
                crystalhd_rx_retire_quiesced(&context, suspend_only);
                Check(quiesced_rx_retires == expected &&
                      !memcmp(&context, &before, sizeof(before)) &&
                      !memcmp(&hardware, &hardware_before, sizeof(hardware_before)),
                      "RX-only retirement honors only legacy suspend retention without revoking session or state");
                crystalhd_rx_retire_quiesced(&context, suspend_only);
                Check(quiesced_rx_retires == expected * 2 &&
                      !starts && !stops && !captures && !cancels &&
                      !irq_disables && !irq_enables && !hardware_closes &&
                      !ring_frees && !dio_destroys && !elem_deletes,
                      "repeated policy calls delegate memory-only retirement without device or session teardown");
            }
        }
    }
    Reset(BC_LINK_INVALID, false);
    context.retain_rx_on_suspend = true;
    {
        struct crystalhd_cmd before = context;

        crystalhd_rx_retire_quiesced(NULL, false);
        crystalhd_rx_retire_quiesced(NULL, true);
        crystalhd_rx_retire_quiesced(&context, false);
        crystalhd_rx_retire_quiesced(&context, true);
        Check(!quiesced_rx_retires && !memcmp(&context, &before, sizeof(before)),
              "quiesced retirement tolerates NULL and early contexts without hardware");
    }
}

static void RxRetentionAdmission(void)
{
    int owner, other;
    static const uint32_t modes[] = {
        DTS_PLAYBACK_MODE, DTS_DIAG_MODE,
        0x100 | DTS_PLAYBACK_MODE, 0x100 | DTS_DIAG_MODE,
    };

    Reset(BC_LINK_INVALID, false);
    context.retain_rx_on_suspend = true; /* A new generic owner must not inherit it. */
    Check(crystalhd_session_acquire_locked(&context, &owner) == BC_STS_SUCCESS &&
          !context.retain_rx_on_suspend,
          "successful generic acquisition resets stale legacy suspend retention");
    crystalhd_rx_retire_quiesced(&context, true);
    Check(quiesced_rx_retires == 1 && context.session_owner == &owner,
          "generic suspend retires RX while retaining the exact session owner");
    Check(crystalhd_session_release_locked(&context, &owner) == BC_STS_SUCCESS &&
          !context.retain_rx_on_suspend,
          "generic release leaves suspend policy at its safe default");

    for (unsigned mode = 0; mode < sizeof(modes) / sizeof(modes[0]); mode++) {
        Reset(BC_LINK_INVALID, false);
        context.user[0].in_use = 1;
        Check(crystalhd_user_set_mode(&context, &context.user[0], modes[mode]) ==
                  BC_STS_SUCCESS && context.retain_rx_on_suspend,
              "successful legacy playback and diagnostic admission opt into RX suspend retention");
        crystalhd_rx_retire_quiesced(&context, true);
        Check(!quiesced_rx_retires && context.session_owner == &context.user[0],
              "legacy suspend keeps its registrations and session owner");
        Check(crystalhd_session_acquire_locked(&context, &other) == BC_STS_BUSY &&
              crystalhd_session_release_locked(&context, &other) == BC_STS_ERR_USAGE &&
              context.retain_rx_on_suspend,
              "rejected foreign acquire or release cannot erase the live legacy policy");
        Check(crystalhd_user_set_mode(&context, &context.user[1], DTS_MONITOR_MODE) ==
                  BC_STS_SUCCESS && context.retain_rx_on_suspend,
              "a nonowning monitor cannot overwrite the legacy owner policy");
        crystalhd_rx_retire_quiesced(&context, false);
        Check(quiesced_rx_retires == 1 && context.retain_rx_on_suspend,
              "remove or fail-closed retirement ignores legacy suspend retention");
        Check(crystalhd_session_release_locked(&context, &context.user[0]) ==
                  BC_STS_SUCCESS && !context.retain_rx_on_suspend,
              "legacy release clears retention before a later owner can acquire");
        Check(crystalhd_session_acquire_locked(&context, &owner) == BC_STS_SUCCESS &&
              !context.retain_rx_on_suspend,
              "generic reacquisition after a legacy owner uses non-retaining suspend policy");
        Check(crystalhd_session_release_locked(&context, &owner) == BC_STS_SUCCESS,
              "reacquired generic session releases cleanly");
    }

    for (unsigned legacy = 0; legacy < 2; legacy++) {
        for (unsigned failure = 0; failure < 3; failure++) {
            BC_STATUS expected = failure == 2 ? BC_STS_INSUFF_RES : BC_STS_ERROR;

            Reset(BC_LINK_INVALID, false);
            context.user[0].in_use = legacy;
            if (failure == 0) elem_error = -1;
            if (failure == 1) dio_error = -1;
            if (failure == 2) ring_status = BC_STS_INSUFF_RES;
            Check((legacy ?
                      crystalhd_user_set_mode(&context, &context.user[0], DTS_PLAYBACK_MODE) :
                      crystalhd_session_acquire_locked(&context, &owner)) == expected &&
                  !context.session_owner && !context.retain_rx_on_suspend,
                  "failed session setup cannot publish legacy RX retention");
            elem_error = dio_error = 0;
            ring_status = BC_STS_SUCCESS;
            Check((legacy ?
                      crystalhd_user_set_mode(&context, &context.user[0], DTS_PLAYBACK_MODE) :
                      crystalhd_session_acquire_locked(&context, &owner)) == BC_STS_SUCCESS &&
                  context.retain_rx_on_suspend == (bool)legacy,
                  "setup retry publishes retention for precisely the successful frontend");
            Check(crystalhd_session_release_locked(&context,
                      legacy ? (const void *)&context.user[0] : &owner) == BC_STS_SUCCESS &&
                  !context.retain_rx_on_suspend,
                  "successful retry release clears its RX retention policy");
        }
    }

    Reset(BC_LINK_INVALID, false);
    Check(crystalhd_user_set_mode(&context, &context.user[0], DTS_MONITOR_MODE) ==
              BC_STS_SUCCESS && !context.retain_rx_on_suspend && !context.session_owner,
          "monitor-only admission never enables legacy owner retention");
    context.session_owner = &owner;
    context.retain_rx_on_suspend = true;
    Check(crystalhd_session_release_locked(&context, &owner) == BC_STS_SUCCESS &&
          !context.retain_rx_on_suspend && !context.session_owner,
          "release resets retention even for an already retired hardware context");
}

static void RxRetentionContextReset(void)
{
    for (unsigned failure = 0; failure < 3; failure++) {
        BC_STATUS expected = failure == 0 ? BC_STS_SUCCESS :
            failure == 1 ? BC_STS_ERROR : BC_STS_IO_ERROR;

        Reset(BC_LINK_INVALID, false);
        context.retain_rx_on_suspend = true;
        if (failure == 1) hardware_alloc_fail = true;
        if (failure == 2) hardware_open_status = BC_STS_IO_ERROR;
        Check(crystalhd_setup_cmd_context(&context, &adapter) == expected &&
              !context.retain_rx_on_suspend && !context.session_owner && !context.hw_ctx,
              "actual context setup clears stale retention on success, allocation failure and open failure");
        Check(!hardware_allocated && !irq_depth && irq_disables == irq_enables,
              "context setup policy reset leaves no hardware allocation or IRQ imbalance");
    }

    for (unsigned with_session = 0; with_session < 2; with_session++) {
        int owner;

        Reset(BC_LINK_INVALID, false);
        if (with_session)
            Check(crystalhd_session_acquire_locked(&context, &owner) == BC_STS_SUCCESS,
                  "prepare resources for actual command-context deletion");
        context.retain_rx_on_suspend = true;
        {
            unsigned previous_stops = stops, previous_irqs = irq_disables;

            Check(crystalhd_delete_cmd_context(&context) == BC_STS_SUCCESS &&
                  !context.retain_rx_on_suspend && !context.session_owner &&
                  !context.hw_ctx && !context.adp && !hardware_allocated &&
                  !elem_live && !dio_live && !rings_live,
                  "actual context deletion clears retention and all owned resources, including early contexts");
            Check(stops == previous_stops && irq_disables == previous_irqs,
                  "post-quiescence command deletion has no hardware stop or IRQ side effects");
        }
    }
}

static void ModulePinAdmission(void)
{
    int owner;

    for (unsigned gate = 0; gate < 7; gate++) {
        struct crystalhd_cmd *argument;
        const void *token = &owner;
        BC_STATUS expected = gate < 3 ? BC_STS_INV_ARG :
            gate == 3 || gate == 6 ? BC_STS_BUSY : BC_STS_ERR_USAGE;

        Reset(BC_LINK_INVALID, false);
        argument = &context;
        if (gate == 0) argument = NULL;
        if (gate == 1) context.adp = NULL;
        if (gate == 2) token = NULL;
        if (gate == 3) context.session_owner = &owner;
        if (gate == 4) context.state = BC_LINK_INIT;
        if (gate == 5) context.state = BC_LINK_SUSPEND;
        if (gate == 6) {
            context.session_module_pinned = true;
            module_refs = 1;
        }
        {
            struct crystalhd_cmd before = context;

            Check(crystalhd_session_acquire_locked(argument, token) == expected &&
                  !memcmp(&before, &context, sizeof(before)) &&
                  !module_get_attempts && !module_gets && !module_puts &&
                  !hardware_alloc_attempts && !pools && !rings,
                  "argument, owner, state and unexpected live-pin gates precede module acquisition");
        }
    }

    for (unsigned existing = 0; existing < 2; existing++) {
        Reset(BC_LINK_INVALID, false);
        if (existing)
            Check(crystalhd_ensure_hw_context(&context) == BC_STS_SUCCESS,
                  "prepare a pre-existing file-owned hardware context");
        module_get_allowed = false;
        {
            struct crystalhd_cmd before = context;

            Check(crystalhd_session_acquire_locked(&context, &owner) == BC_STS_NO_ACCESS &&
                  !memcmp(&before, &context, sizeof(before)) &&
                  module_get_attempts == 1 && !module_gets && !module_puts && !module_refs &&
                  hardware_alloc_attempts == existing && hardware_opens == existing &&
                  !hardware_closes && !hardware_frees && !pools && !rings,
                  "denied shared pre-pin precedes every hardware or session allocation in that call");
        }
        module_get_allowed = true;
        checking_module_callbacks = true;
        Check(crystalhd_session_acquire_locked(&context, &owner) == BC_STS_SUCCESS &&
              module_get_attempts == 2 && module_gets == 1 && module_refs == 1 &&
              context.session_module_pinned,
              "a denied shared pre-pin can retry and publish exactly one held module reference");
        module_expect_no_hardware_on_put = true;
        Check(crystalhd_session_release_locked(&context, &owner) == BC_STS_SUCCESS &&
              module_puts == 1 && !module_refs && !context.session_module_pinned,
              "successful retry releases its single module reference after cleanup");
    }

    for (unsigned existing = 0; existing < 2; existing++) {
        struct crystalhd_user *user;

        Reset(BC_LINK_INVALID, false);
        user = &context.user[0];
        if (existing)
            Check(crystalhd_user_open(&context, &user) == BC_STS_SUCCESS,
                  "legacy open holds its independent file lifetime before mode admission");
        else
            user->in_use = 1; /* An existing handle whose earlier owner retired hardware. */
        adapter.cfg_users = 1;
        module_get_allowed = false;
        Check(crystalhd_user_set_mode(&context, user, DTS_PLAYBACK_MODE) == BC_STS_NO_ACCESS &&
              module_get_attempts == 1 && !module_gets && !module_refs && !module_puts &&
              !context.session_module_pinned && !context.session_owner &&
              user->mode == (uint32_t)DTS_MODE_INV && !pools && !rings &&
              context.hw_ctx == &hardware && hardware_allocated && hardware_opens == 1,
              "legacy pin denial retains its historical pre-open but allocates no session pools or rings");
        crystalhd_user_close(&context, user);
        CheckNoSession();
        Check(hardware_closes == 1 && hardware_frees == 1 && !module_puts,
              "normal legacy close retires a denied pre-open without an unmatched module put");
    }
}

static void ModulePinPrivateSetup(void)
{
    Reset(BC_LINK_INVALID, false);
    context.adp = NULL; /* The pin guard must precede even resource dereferences. */
    {
        struct crystalhd_cmd before = context;

        Check(crystalhd_session_setup(&context) == BC_STS_NO_ACCESS &&
              !memcmp(&context, &before, sizeof(before)) &&
              !pools && !rings && !elem_deletes && !dio_destroys && !ring_frees &&
              !hardware_alloc_attempts && !module_get_attempts && !module_puts,
              "private session setup rejects an absent pre-pin before any resource access or allocation");
    }

    Reset(BC_LINK_INVALID, false);
    Check(try_module_get(THIS_MODULE), "model the shared caller acquiring its module pre-pin");
    context.session_module_pinned = true;
    checking_module_callbacks = true;
    Check(crystalhd_ensure_hw_context(&context) == BC_STS_SUCCESS,
          "prepare hardware under the private setup caller pin");
    Check(crystalhd_session_setup(&context) == BC_STS_SUCCESS &&
          pools == 2 && rings == 1 && elem_live && dio_live && rings_live &&
          context.session_module_pinned && module_refs == 1 && module_gets == 1 &&
          !module_puts && !context.session_owner,
          "pinned private setup allocates the complete resource set without publishing an owner or changing the pin");
    module_expect_no_hardware_on_put = true;
    Check(crystalhd_delete_cmd_context(&context) == BC_STS_SUCCESS &&
          !module_refs && module_gets == 1 && module_puts == 1,
          "unpublished private setup resources still retire completely before the single module put");
}

static void ModulePinSetupFailures(void)
{
    int owner;

    for (unsigned existing = 0; existing < 2; existing++) {
        for (unsigned failure = existing ? 2 : 0; failure < 5; failure++) {
            BC_STATUS expected = failure == 1 ? BC_STS_IO_ERROR :
                failure == 4 ? BC_STS_INSUFF_RES : BC_STS_ERROR;

            Reset(BC_LINK_INVALID, false);
            if (existing)
                Check(crystalhd_ensure_hw_context(&context) == BC_STS_SUCCESS,
                      "prepare caller-owned hardware for setup rollback");
            if (failure == 0) hardware_alloc_fail = true;
            if (failure == 1) hardware_open_status = BC_STS_IO_ERROR;
            if (failure == 2) elem_error = -1;
            if (failure == 3) dio_error = -1;
            if (failure == 4) ring_status = BC_STS_INSUFF_RES;
            checking_module_callbacks = true;
            module_expect_no_hardware_on_put = !existing;
            Check(crystalhd_session_acquire_locked(&context, &owner) == expected &&
                  module_get_attempts == 1 && module_gets == 1 && module_puts == 1 &&
                  !module_refs && !context.session_module_pinned && !context.session_owner &&
                  !elem_live && !dio_live && !rings_live && module_callbacks &&
                  context.hw_ctx == (existing ? &hardware : NULL) &&
                  hardware_allocated == (bool)existing,
                  "every allocation/open/pool/ring failure rolls back under its pin then puts exactly once");
            hardware_alloc_fail = false;
            hardware_open_status = BC_STS_SUCCESS;
            elem_error = dio_error = 0;
            ring_status = BC_STS_SUCCESS;
            Check(crystalhd_session_acquire_locked(&context, &owner) == BC_STS_SUCCESS &&
                  module_gets == 2 && module_puts == 1 && module_refs == 1 &&
                  context.session_module_pinned,
                  "each rollback edge permits a fresh balanced module acquisition on retry");
            module_expect_no_hardware_on_put = true;
            Check(crystalhd_session_release_locked(&context, &owner) == BC_STS_SUCCESS &&
                  module_puts == 2 && !module_refs,
                  "retry teardown balances both successful pre-pins");
        }
    }
}

static void ModulePinOwnerLifetime(void)
{
    int owner, wrong_owner;

    for (unsigned legacy = 0; legacy < 2; legacy++) {
        const void *token;

        Reset(BC_LINK_INVALID, false);
        if (legacy) {
            Check(crystalhd_ensure_hw_context(&context) == BC_STS_SUCCESS,
                  "legacy owner enters shared admission with its pre-opened hardware");
            context.user[0].in_use = 1;
        }
        checking_module_callbacks = true;
        token = legacy ? (const void *)&context.user[0] : &owner;
        Check((legacy ? crystalhd_user_set_mode(&context, &context.user[0], DTS_PLAYBACK_MODE) :
              crystalhd_session_acquire_locked(&context, token)) == BC_STS_SUCCESS &&
              context.session_module_pinned && module_refs == 1 && module_gets == 1,
              "generic and legacy resource owners each retain one pre-acquired module reference");
        {
            struct crystalhd_cmd before = context;
            unsigned callbacks = module_callbacks;

            Check(crystalhd_session_acquire_locked(&context, &wrong_owner) == BC_STS_BUSY &&
                  crystalhd_session_release_locked(&context, &wrong_owner) == BC_STS_ERR_USAGE &&
                  crystalhd_session_release_locked(&context, NULL) == BC_STS_INV_ARG &&
                  !memcmp(&before, &context, sizeof(before)) &&
                  module_get_attempts == 1 && !module_puts && module_refs == 1 &&
                  module_callbacks == callbacks,
                  "foreign or repeated ownership operations cannot consume or release the active module pin");
        }
        Check(crystalhd_user_set_mode(&context, &context.user[1], DTS_MONITOR_MODE) ==
                  BC_STS_SUCCESS && module_gets == 1 && !module_puts,
              "a nonowning monitor neither acquires nor drops the session module reference");
        context.stream = &stream_cookie;
        context.decoder_phase = CRYSTALHD_DECODER_CHANNEL_STARTED;
        context.decoder_codec = CRYSTALHD_DECODER_CODEC_H264;
        module_expect_no_hardware_on_put = true;
        Check(crystalhd_session_release_locked(&context, token) == BC_STS_SUCCESS &&
              module_puts == 1 && !module_refs && !context.session_module_pinned &&
              stream_releases == 1,
              "owner release keeps callback code pinned until staging and all resources are retired");
        Check(crystalhd_session_release_locked(&context, token) == BC_STS_ERR_USAGE &&
              module_puts == 1,
              "a repeated owner release cannot put the module twice");
        crystalhd_session_unpin(&context);
        Check(module_puts == 1, "the extracted unpin helper is idempotent for an empty pin");
    }
}

static void ModulePinContextGuards(void)
{
    int owner;

    Reset(BC_LINK_INVALID, false);
    Check(crystalhd_session_acquire_locked(&context, &owner) == BC_STS_SUCCESS,
          "prepare live module ownership for a repeated command-context setup");
    {
        struct crystalhd_cmd before = context;
        unsigned allocations = hardware_alloc_attempts;

        Check(crystalhd_setup_cmd_context(&context, &adapter) == BC_STS_BUSY &&
              !memcmp(&before, &context, sizeof(before)) &&
              hardware_alloc_attempts == allocations && module_refs == 1 &&
              module_gets == 1 && !module_puts,
              "repeated context setup cannot erase a live module pin or reinitialize its owner");
    }
    checking_module_callbacks = module_expect_no_hardware_on_put = true;
    context.stream = &stream_cookie;
    Check(crystalhd_delete_cmd_context(&context) == BC_STS_SUCCESS &&
          !context.adp && !context.hw_ctx && !context.session_owner &&
          !context.session_module_pinned && !module_refs && module_puts == 1,
          "quiesced command deletion puts only after ring/pool/staging callbacks and owner clearing");

    for (unsigned via_delete = 0; via_delete < 2; via_delete++) {
        Reset(BC_LINK_INVALID, false);
        /* Model already-retired hardware while the exact logical owner/pin survives. */
        context.session_owner = &owner;
        context.session_module_pinned = context.retain_rx_on_suspend = true;
        module_refs = module_gets = 1;
        context.stream = &stream_cookie;
        checking_module_callbacks = module_expect_no_hardware_on_put = true;
        Check((via_delete ? crystalhd_delete_cmd_context(&context) :
              crystalhd_session_release_locked(&context, &owner)) == BC_STS_SUCCESS &&
              !context.session_owner && !context.session_module_pinned &&
              !module_refs && module_puts == 1 && stream_releases == 1 &&
              !hardware_closes && !hardware_frees,
              "no-hardware release and delete still retire staging before balancing the surviving pin");
    }
}

static void ModulePinPowerLifetime(void)
{
    int owner;
    crystalhd_ioctl_data data = {0};

    Reset(BC_LINK_INVALID, false);
    checking_module_callbacks = true;
    Check(crystalhd_session_acquire_locked(&context, &owner) == BC_STS_SUCCESS,
          "prepare generic module ownership for suspend and RX-only retirement");
    context.state = BC_LINK_INIT;
    Check(crystalhd_suspend(&context, &data) == BC_STS_SUCCESS &&
          context.session_owner == &owner && context.session_module_pinned &&
          module_refs == 1 && !module_puts,
          "successful command suspend preserves the session token and module pin");
    crystalhd_rx_retire_quiesced(&context, true);
    Check(quiesced_rx_retires == 1 && context.session_owner == &owner &&
          context.session_module_pinned && module_refs == 1 && !module_puts,
          "RX-only quiesced retirement never revokes the module-owning session");
    Check(crystalhd_resume(&context) == BC_STS_SUCCESS && module_refs == 1 && !module_puts,
          "resume recovery neither acquires a second pin nor drops the existing pin");
    module_expect_no_hardware_on_put = true;
    Check(crystalhd_session_release_locked(&context, &owner) == BC_STS_SUCCESS &&
          module_gets == 1 && module_puts == 1 && !module_refs,
          "explicit owner release finally balances the pin retained across PM");

    for (unsigned failure = 0; failure < 3; failure++) {
        struct file file;
        const void *token;

        Reset(BC_LINK_INVALID, false);
        file = OpenFile(DTS_PLAYBACK_MODE);
        token = context.session_owner;
        context.state = BC_LINK_READY | BC_LINK_CAP_EN;
        if (failure == 0) capture_status = BC_STS_TIMEOUT;
        if (failure == 1) cancel_status = BC_STS_IO_ERROR;
        if (failure == 2) stop_ok = false;
        checking_module_callbacks = true;
        Check(crystalhd_suspend(&context, &data) != BC_STS_SUCCESS &&
              context.session_owner == token && context.session_module_pinned &&
              module_refs == 1 && !module_puts,
              "every failed PM stop stage retains its resources and their module owner");
        Check(!adapter.present && hardware.dma_fault,
              "actual fatal-stop publication precedes accounting-only file close");
        CloseFile(&file);
        Check(context.session_owner == token && context.session_module_pinned &&
              module_refs == 1 && !module_puts && hardware_allocated && rings_live,
              "accounting-only last file close after failed PM cannot unload retained callback code");
        Check(!adapter.dma_terminal_quiesced && !ring_frees && !hardware_frees,
              "failed-PM fixture ends retained without inventing terminal DMA proof");
    }
}

static void ResetReferencedOwner(void)
{
    Reset(BC_LINK_INVALID, false);
    adapter.user_lock = 1;
}

static void ReferencedOwnerAdmission(void)
{
    for (unsigned gate = 0; gate < 8; gate++) {
        struct crystalhd_session_owner_ops ops = frontend_ops;
        const struct crystalhd_session_owner_ops *callbacks = &ops;
        struct crystalhd_cmd *ctx;
        const void *owner;

        ResetReferencedOwner();
        ctx = &context;
        owner = &frontend_owners[0];
        if (gate == 0) ctx = NULL;
        if (gate == 1) context.adp = NULL;
        if (gate == 2) owner = NULL;
        if (gate == 3) callbacks = NULL;
        if (gate == 4) ops.get = NULL;
        if (gate == 5) ops.retired = NULL;
        if (gate == 6) ops.put = NULL;
        if (gate == 7) module_get_allowed = false;
        {
            struct crystalhd_cmd before = context;

            Check(crystalhd_session_acquire_ref_locked(ctx, owner, callbacks) ==
                  (gate == 7 ? BC_STS_NO_ACCESS : BC_STS_INV_ARG) &&
                  !memcmp(&before, &context, sizeof(context)) &&
                  !frontend_owners[0].gets && !frontend_owners[0].retired &&
                  !frontend_owners[0].puts && frontend_owners[0].refs == 1 &&
                  !module_gets && !module_puts && !hardware_alloc_attempts &&
                  module_get_attempts == (gate == 7),
                  "invalid reference contract or denied module pin has no owner-reference side effects");
        }
    }

    for (unsigned gate = 0; gate < 5; gate++) {
        ResetReferencedOwner();
        if (gate == 0) context.session_owner = &frontend_owners[1];
        if (gate == 1) {
            context.session_module_pinned = true;
            module_refs = 1;
        }
        if (gate == 2) context.session_lifetime_owner = &frontend_owners[1];
        if (gate == 3) context.session_lifetime_ops = &frontend_ops;
        if (gate == 4) context.state = BC_LINK_INIT;
        {
            struct crystalhd_cmd before = context;
            BC_STATUS expected = gate == 4 ? BC_STS_ERR_USAGE : BC_STS_BUSY;

            Check(crystalhd_session_acquire_ref_locked(&context,
                      &frontend_owners[0], &frontend_ops) == expected &&
                  !memcmp(&before, &context, sizeof(context)) &&
                  !frontend_owners[0].gets && !frontend_owners[1].gets &&
                  !module_get_attempts && !module_puts && !hardware_alloc_attempts,
                  "busy or invalid-state acquire cannot adopt an existing pin or lifetime identity");
            Check(crystalhd_session_acquire_locked(&context, &frontend_owners[0]) == expected &&
                  !memcmp(&before, &context, sizeof(context)) && !module_get_attempts,
                  "legacy acquisition cannot bypass the reference-backed lifetime gates");
            if (gate == 2 || gate == 3)
                Check(crystalhd_setup_cmd_context(&context, &adapter) == BC_STS_BUSY &&
                      !memcmp(&before, &context, sizeof(context)) && !hardware_alloc_attempts,
                      "context reinitialization cannot erase either surviving lifetime field");
        }
    }
}

static void ReferencedOwnerSetupRollback(void)
{
    for (unsigned existing = 0; existing < 2; existing++) {
        for (unsigned failure = existing ? 2 : 0; failure < 5; failure++) {
            BC_STATUS expected = failure == 1 ? BC_STS_IO_ERROR :
                failure == 4 ? BC_STS_INSUFF_RES : BC_STS_ERROR;

            ResetReferencedOwner();
            if (existing)
                Check(crystalhd_ensure_hw_context(&context) == BC_STS_SUCCESS,
                      "prepare a pre-opened hardware context for safe reference-acquire rollback");
            if (failure == 0) hardware_alloc_fail = true;
            if (failure == 1) hardware_open_status = BC_STS_IO_ERROR;
            if (failure == 2) elem_error = -1;
            if (failure == 3) dio_error = -1;
            if (failure == 4) ring_status = BC_STS_INSUFF_RES;
            checking_module_callbacks = true;
            Check(crystalhd_session_acquire_ref_locked(&context,
                      &frontend_owners[0], &frontend_ops) == expected &&
                  !context.session_owner && !context.session_lifetime_owner &&
                  !context.session_lifetime_ops && !context.session_module_pinned &&
                  module_gets == 1 && module_puts == 1 && !module_refs &&
                  frontend_owners[0].refs == 1 && !frontend_owners[0].gets &&
                  !frontend_owners[0].retired && !frontend_owners[0].puts &&
                  !frontend_lifetime_step && !elem_live && !dio_live && !rings_live &&
                  context.hw_ctx == (existing ? &hardware : NULL),
                  "safe setup rollback never adopts or notifies a frontend reference");
        }
    }

    for (unsigned failure = 0; failure < 3; failure++) {
        struct frontend_owner *owner = &frontend_owners[0];

        ResetReferencedOwner();
        if (failure == 0) elem_error = -1;
        if (failure == 1) dio_error = -1;
        if (failure == 2) ring_status = BC_STS_INSUFF_RES;
        stop_ok = false;
        Check(crystalhd_session_acquire_ref_locked(&context, owner, &frontend_ops) ==
                  (failure == 2 ? BC_STS_INSUFF_RES : BC_STS_ERROR) &&
              !context.session_owner && context.session_lifetime_owner == owner &&
              context.session_lifetime_ops == &frontend_ops &&
              context.session_module_pinned && module_refs == 1 && !module_puts &&
              owner->gets == 1 && owner->refs == 2 && !owner->retired && !owner->puts,
              "uncertain setup rollback adopts one frontend reference despite an unpublished session owner");
        owner->refs--; /* The unsuccessful caller releases only its own reference. */
        Check(crystalhd_session_release_locked(&context, owner) == BC_STS_ERR_USAGE &&
              crystalhd_session_acquire_ref_locked(&context, &frontend_owners[1],
                                                   &frontend_ops) == BC_STS_BUSY &&
              owner->refs == 1 && owner->gets == 1 && !owner->retired && !owner->puts &&
              !frontend_owners[1].gets && module_gets == 1 && !module_puts &&
              context.hw_ctx == &hardware && hardware_allocated && hardware.dma_fault &&
              !adapter.present && !adapter.dma_terminal_quiesced && !hardware_frees,
              "failed caller departure leaves unpublished fatal ownership referenced without a retry or fake proof");
    }
}

static void ReferencedOwnerRetirement(void)
{
    for (unsigned terminal = 0; terminal < 2; terminal++) {
        for (unsigned departed = 0; departed < 2; departed++) {
            struct frontend_owner *owner = &frontend_owners[0];

            ResetReferencedOwner();
            checking_module_callbacks = module_expect_no_hardware_on_put = true;
            Check(crystalhd_session_acquire_ref_locked(&context, owner, &frontend_ops) ==
                      BC_STS_SUCCESS && context.session_owner == owner &&
                  context.session_lifetime_owner == owner &&
                  context.session_lifetime_ops == &frontend_ops &&
                  owner->refs == 2 && owner->gets == 1 && !owner->retired && !owner->puts,
                  "successful reference acquisition publishes one independent lifetime owner");
            {
                struct crystalhd_cmd before = context;

                Check(crystalhd_session_acquire_ref_locked(&context, owner, &frontend_ops) ==
                          BC_STS_BUSY &&
                      crystalhd_session_acquire_ref_locked(&context, &frontend_owners[1],
                                                           &frontend_ops) == BC_STS_BUSY &&
                      crystalhd_session_release_locked(&context, &frontend_owners[1]) ==
                          BC_STS_ERR_USAGE &&
                      !memcmp(&before, &context, sizeof(context)) && owner->gets == 1 &&
                      !frontend_owners[1].gets && owner->refs == 2 && !module_puts,
                      "same-owner reacquire and foreign acquire/release cannot consume lifetime references");
            }
            context.stream = &stream_cookie;
            context.decoder_phase = CRYSTALHD_DECODER_CHANNEL_STARTED;
            context.decoder_codec = CRYSTALHD_DECODER_CODEC_H264;
            if (departed)
                owner->refs--; /* Leave only the core-owned frontend reference. */
            if (terminal) {
                /* Independent terminal caller-precondition model; the PCI/IRQ
                 * proof itself is exercised by device-lifetime, not invented
                 * by a successful local stop or by clearing dma_fault here.
                 */
                hardware.dma_fault = true;
                adapter.present = false;
                adapter.dma_terminal_quiesced = true;
                context.cin_wait_exit = 1;
                retained_tx_lease = true;
            }
            Check((terminal ? crystalhd_delete_cmd_context(&context) :
                  crystalhd_session_release_locked(&context, owner)) == BC_STS_SUCCESS &&
                  owner->retired == 1 && owner->puts == 1 && owner->refs == !departed &&
                  frontend_lifetime_step == 4 && module_puts == 1 && !module_refs &&
                  !context.session_lifetime_owner && !context.session_lifetime_ops,
                  "normal and terminal retirement notify once, put the frontend, then unpin the module");
            if (terminal)
                Check(hardware.dma_fault && !adapter.present && !context.adp &&
                      retained_tx_puts == 1 && !stops && !captures && !cancels,
                      "terminal referenced-owner retirement never resumes or retries uncertain engines");
            Check(crystalhd_session_release_locked(&context, owner) == BC_STS_ERR_USAGE,
                  "repeated owner release cannot retire a departed session twice");
            crystalhd_session_retire_owner(&context);
            crystalhd_session_unpin(&context);
            Check(owner->retired == 1 && owner->puts == 1 && module_puts == 1 &&
                  frontend_lifetime_step == 4,
                  "empty lifetime and module helpers cannot repeat notification or either put");
        }
    }

    ResetReferencedOwner();
    /* Independently model the retained unpublished setup outcome. Its actual
     * failed acquisition is covered above; terminal PCI/IRQ proof is supplied
     * by the caller, without recovering the previous unsafe test context.
     */
    Check(try_module_get(THIS_MODULE), "prepare the unpublished owner's original module pin");
    context.session_module_pinned = true;
    Check(crystalhd_ensure_hw_context(&context) == BC_STS_SUCCESS,
          "prepare the separately retained unpublished hardware context");
    FrontendGet(&frontend_owners[0]);
    context.session_lifetime_owner = &frontend_owners[0];
    context.session_lifetime_ops = &frontend_ops;
    frontend_owners[0].refs--;
    hardware.dma_fault = true;
    adapter.present = false;
    adapter.dma_terminal_quiesced = true;
    context.cin_wait_exit = 1;
    Check(!context.session_owner && crystalhd_delete_cmd_context(&context) == BC_STS_SUCCESS &&
          frontend_owners[0].retired == 1 && frontend_owners[0].puts == 1 &&
          !frontend_owners[0].refs && frontend_lifetime_step == 4 && module_puts == 1,
          "terminal deletion retires an unpublished lifetime owner even though session_owner is NULL");
}

static void ReferencedOwnerFaultRetention(void)
{
    for (unsigned failure = 0; failure < 4; failure++) {
        struct frontend_owner *owner = &frontend_owners[0];
        unsigned attempts;

        ResetReferencedOwner();
        Check(crystalhd_session_acquire_ref_locked(&context, owner, &frontend_ops) ==
                  BC_STS_SUCCESS,
              "prepare a referenced owner for each fatal retirement boundary");
        context.stream = &stream_cookie;
        if (failure == 0) crystalhd_hw_dma_fatal_stop(&hardware);
        if (failure == 1) capture_status = BC_STS_TIMEOUT;
        if (failure == 2) cancel_status = BC_STS_IO_ERROR;
        if (failure == 3) stop_ok = false;
        Check(crystalhd_session_release_locked(&context, owner) == BC_STS_IO_ERROR,
              "each uncertain retirement stage retains the frontend owner");
        owner->refs--;
        attempts = captures + cancels + stops;
        Check(crystalhd_session_release_locked(&context, owner) == BC_STS_IO_ERROR &&
              crystalhd_delete_cmd_context(&context) == BC_STS_IO_ERROR &&
              captures + cancels + stops == attempts && hardware.dma_fault &&
              !adapter.present && !adapter.dma_terminal_quiesced &&
              context.session_owner == owner && context.session_lifetime_owner == owner &&
              context.session_lifetime_ops == &frontend_ops && context.session_module_pinned &&
              owner->refs == 1 && owner->gets == 1 && !owner->retired && !owner->puts &&
              frontend_lifetime_step == 1 && module_refs == 1 && !module_puts &&
              elem_live && dio_live && rings_live && hardware_allocated &&
              context.stream == &stream_cookie && !stream_releases && !hardware_frees,
              "fatal release and unproven terminal deletion never notify, put, unpin or retry hardware");
    }
}

static void ReferencedOwnerPowerLifetime(void)
{
    struct frontend_owner *owner = &frontend_owners[0];
    crystalhd_ioctl_data data = {0};

    ResetReferencedOwner();
    Check(crystalhd_session_acquire_ref_locked(&context, owner, &frontend_ops) == BC_STS_SUCCESS,
          "prepare reference-backed ownership across successful PM");
    context.state = BC_LINK_INIT;
    Check(crystalhd_suspend(&context, &data) == BC_STS_SUCCESS,
          "successful suspend stops hardware without revoking the frontend owner");
    crystalhd_rx_retire_quiesced(&context, true);
    crystalhd_rx_retire_quiesced(&context, false);
    Check(quiesced_rx_retires == 2 && context.session_owner == owner &&
          context.session_lifetime_owner == owner && context.session_lifetime_ops == &frontend_ops &&
          owner->refs == 2 && owner->gets == 1 && !owner->retired && !owner->puts &&
          context.session_module_pinned && module_refs == 1 && !module_puts && frontend_lifetime_step == 1,
          "suspend and both RX-only retirement modes preserve reference-backed session ownership");
    Check(crystalhd_resume(&context) == BC_STS_SUCCESS && owner->gets == 1 &&
          owner->refs == 2 && !owner->retired && !owner->puts,
          "resume keeps the existing frontend reference without adopting another");
    Check(crystalhd_session_release_locked(&context, owner) == BC_STS_SUCCESS &&
          owner->retired == 1 && owner->puts == 1 && owner->refs == 1 &&
          frontend_lifetime_step == 4 && module_puts == 1,
          "explicit post-PM release alone retires the referenced session");
}

static void FrontendNeutralSessionOwner(void)
{
    int foreign_owner, wrong_owner;
    struct file legacy, monitor, contender;
    struct crystalhd_user *contender_user;
    crystalhd_ioctl_data data = {0};

    for (unsigned which = 0; which < 3; which++) {
        BC_STATUS expected = which == 2 ? BC_STS_INSUFF_RES : BC_STS_ERROR;

        Reset(BC_LINK_INVALID, false);
        if (which == 0) elem_error = -1;
        if (which == 1) dio_error = -1;
        if (which == 2) ring_status = BC_STS_INSUFF_RES;
        Check(crystalhd_session_acquire_locked(&context, &foreign_owner) == expected,
              "frontend-only acquisition propagates every resource setup failure");
        Check(!context.session_owner && !context.hw_ctx && !adapter.cfg_users &&
              !bc_get_userhandle_count(&context) && !hardware_allocated &&
              !elem_live && !dio_live && !rings_live,
              "failed frontend-only acquisition leaves no owner or hardware context");
        Check(hardware_opens == 1 && hardware_closes == 1 &&
              hardware_allocations == 1 && hardware_frees == 1 &&
              irq_disables == 2 + stops && irq_enables == 2 + stops && !irq_depth,
              "failed frontend-only acquisition closes exactly the context it opened");
    }

    Reset(BC_LINK_INVALID, false);
    Check(crystalhd_session_acquire_locked(NULL, &foreign_owner) == BC_STS_INV_ARG &&
          crystalhd_session_acquire_locked(&context, NULL) == BC_STS_INV_ARG,
          "shared acquisition rejects missing context and owner tokens");
    Check(crystalhd_session_acquire_locked(&context, &foreign_owner) == BC_STS_SUCCESS &&
          !adapter.cfg_users && !bc_get_userhandle_count(&context),
          "a frontend can acquire the decoder without inventing a legacy user");
    Check(context.session_owner == &foreign_owner && context.hw_ctx == &hardware &&
          elem_live && dio_live && rings_live,
          "frontend-only acquisition publishes one complete resource set");
    Check(crystalhd_session_acquire_locked(&context, &wrong_owner) == BC_STS_BUSY &&
          crystalhd_session_release_locked(&context, &wrong_owner) == BC_STS_ERR_USAGE,
          "foreign acquire and release tokens cannot replace the exact owner");
    context.pwr_state_change = BC_HW_SUSPEND;
    expect_retire_irq = true;
    Check(crystalhd_session_release_locked(&context, &foreign_owner) == BC_STS_SUCCESS,
          "a frontend-only session releases through the shared boundary");
    expect_retire_irq = false;
    CheckNoSession();

    legacy = OpenResourceFile(DTS_PLAYBACK_MODE);
    Check(context.session_owner ==
          ((struct crystalhd_file *)legacy.private_data)->user,
          "legacy playback reacquires the same shared boundary after release");
    CloseFile(&legacy);
    CheckNoSession();

    Reset(BC_LINK_INVALID, false);
    monitor = OpenFile(DTS_MONITOR_MODE);
    Check(crystalhd_session_acquire_locked(&context, &foreign_owner) == BC_STS_SUCCESS,
          "a non-legacy frontend can acquire the shared decoder session");
    Check(context.session_owner == &foreign_owner && context.hw_ctx == &hardware &&
          elem_live && dio_live && rings_live && hardware_opens == 1,
          "shared acquisition records only the opaque owner and one resource set");

    contender = OpenFile(DTS_MODE_INV);
    contender_user = ((struct crystalhd_file *)contender.private_data)->user;
    data.u_id = contender_user->uid;
    data.udata.u.NotifyMode.Mode = DTS_PLAYBACK_MODE;
    Check(bc_cproc_notify_mode(&context, &data) == BC_STS_ERR_USAGE &&
          contender_user->mode == (uint32_t)DTS_MODE_INV &&
          context.session_owner == &foreign_owner,
          "legacy admission cannot replace a shared frontend owner");
    Check(crystalhd_session_acquire_locked(&context, &wrong_owner) == BC_STS_BUSY,
          "a second shared frontend owner receives a deterministic busy result");

    CloseFile(&contender);
    CloseFile(&monitor);
    Check(!adapter.cfg_users && context.session_owner == &foreign_owner &&
          context.hw_ctx == &hardware && elem_live && dio_live && rings_live &&
          !hardware_closes && !hardware_frees,
          "closing the last legacy file cannot retire another frontend's session");
    Check(crystalhd_session_release_locked(&context, &wrong_owner) == BC_STS_ERR_USAGE &&
          context.session_owner == &foreign_owner && context.hw_ctx == &hardware,
          "only the exact opaque owner can release the shared session");
    Check(crystalhd_session_release_locked(&context, NULL) == BC_STS_INV_ARG &&
          context.session_owner == &foreign_owner,
          "a NULL release token cannot alter shared ownership");
    Check(crystalhd_session_release_locked(&context, &foreign_owner) == BC_STS_SUCCESS,
          "the exact shared owner can release its decoder session");
    CheckNoSession();
    Check(hardware_closes == 1 && hardware_frees == 1 && ring_frees == 1 &&
          dio_destroys == 1 && elem_deletes == 1 && !irq_depth,
          "shared release retires every playback resource exactly once");
}
static struct file OpenPendingAfterOwnerClose(uint32_t mode)
{
    struct file owner, pending;

    Reset(BC_LINK_INVALID, false);
    owner = OpenResourceFile(mode);
    pending = OpenFile(DTS_MODE_INV);
    hardware.fwcmd_poisoned = true;
    CloseFile(&owner);
    Check(!context.session_owner && !context.hw_ctx && context.user[1].in_use &&
          adapter.cfg_users == 1 && hardware_opens == 1 && hardware_closes == 1,
          "owner close leaves a pending unconfigured handle without hardware");
    return pending;
}
static void PendingOpenAfterOwnerClose(void)
{
    unsigned mode;

    for (mode = 0; mode < sizeof(resource_modes) / sizeof(resource_modes[0]); mode++) {
        struct file pending, reopened;
        struct crystalhd_user *pending_user;
        crystalhd_ioctl_data data = {0};

        pending = OpenPendingAfterOwnerClose(resource_modes[mode]);
        pending_user = ((struct crystalhd_file *)pending.private_data)->user;
        data.u_id = pending_user->uid;
        data.udata.u.NotifyMode.Mode = resource_modes[mode];
        Check(bc_cproc_notify_mode(&context, &data) == BC_STS_SUCCESS,
              "pending handle acquires fresh hardware without reopening its file");
        Check(context.session_owner == pending_user && context.hw_ctx == &hardware &&
              pending_user->mode == resource_modes[mode] && adapter.cfg_users == 1 &&
              pools == 4 && rings == 2 && elem_live && dio_live && rings_live &&
              elem_deletes == 1 && dio_destroys == 1 &&
              hardware_opens == 2 && hardware_allocations == 2 && !irq_depth &&
              irq_disables == 3 + stops && irq_enables == 3 + stops &&
              !hardware.dma_fault && !hardware.fwcmd_poisoned,
              "pending acquisition recreates one complete session with unchanged file accounting");
        reopened = OpenFile(DTS_MONITOR_MODE);
        Check(context.session_owner == pending_user && hardware_opens == 2,
              "a new monitor reuses the old owner's slot without reopening hardware");
        CloseFile(&pending);
        Check(!context.session_owner && !context.hw_ctx &&
              adapter.cfg_users == 1 && hardware_closes == 2,
              "closing the new owner retires its fresh hardware while the monitor remains");
        CloseFile(&reopened);
        CheckNoSession();
    }
}
static void PendingAcquisitionFailures(void)
{
    for (unsigned mode = 0; mode < sizeof(resource_modes) / sizeof(resource_modes[0]); mode++) {
        for (unsigned which = 0; which < 5; which++) {
            for (unsigned retry = 0; retry < 2; retry++) {
                struct file pending = OpenPendingAfterOwnerClose(resource_modes[mode]);
                struct crystalhd_user *user = ((struct crystalhd_file *)pending.private_data)->user;
                crystalhd_ioctl_data data = { .u_id = user->uid };
                BC_STATUS expected = which == 1 ? BC_STS_IO_ERROR :
                    which == 4 ? BC_STS_INSUFF_RES : BC_STS_ERROR;

                admission_pending = true;
                admission_uid = user->uid;
                admission_state = context.state;
                admission_wait = context.cin_wait_exit;
                hardware_alloc_fail = which == 0;
                if (which == 1) hardware_open_status = BC_STS_IO_ERROR;
                if (which == 2) elem_error = -1;
                if (which == 3) dio_error = -1;
                if (which == 4) ring_status = BC_STS_INSUFF_RES;
                data.udata.u.NotifyMode.Mode = resource_modes[mode];
                Check(bc_cproc_notify_mode(&context, &data) == expected,
                      "pending acquisition propagates allocation, hardware-open and setup failures");
                Check(!context.session_owner && user->mode == (uint32_t)DTS_MODE_INV &&
                      user->in_use && adapter.cfg_users == 1 &&
                      context.state == BC_LINK_INVALID && context.cin_wait_exit == 1 &&
                      context.pwr_state_change == BC_HW_RUNNING &&
                      !elem_live && !dio_live && !rings_live &&
                      !irq_depth && irq_disables == 3 + stops && irq_enables == 3 + stops,
                      "failed pending acquisition preserves its handle and rolls back session ownership");
                Check((context.hw_ctx != NULL) == (which >= 2) &&
                      hardware_allocated == (which >= 2) && hardware_alloc_attempts == 2 &&
                      hardware_allocations == 1 + (which != 0) &&
                      hardware_frees == 1 + (which == 1) &&
                      hardware_opens == 1 + (which != 0) && hardware_closes == 1,
                      "failed hardware opening is freed; successful hardware remains available for setup retry");
                if (which < 2)
                    Check(pools == 2 && rings == 1,
                          "hardware creation failure does not enter pool or ring allocation");
                if (retry) {
                    hardware_alloc_fail = false;
                    hardware_open_status = ring_status = BC_STS_SUCCESS;
                    elem_error = dio_error = 0;
                    Check(bc_cproc_notify_mode(&context, &data) == BC_STS_SUCCESS,
                          "the same pending handle can retry every acquisition failure");
                    Check(context.session_owner == user && user->mode == resource_modes[mode] &&
                          user->in_use && adapter.cfg_users == 1 && context.hw_ctx == &hardware &&
                          elem_live && dio_live && rings_live && !context.cin_wait_exit &&
                          hardware_alloc_attempts == (which < 2 ? 3U : 2U) &&
                          hardware_opens == (which == 1 ? 3U : 2U) &&
                          !hardware.dma_fault && !hardware.fwcmd_poisoned &&
                          !irq_depth && irq_disables == irq_enables,
                          "retry reuses successfully opened hardware and publishes exactly one owner");
                }
                admission_pending = false;
                CloseFile(&pending);
                CheckNoSession();
                Check(hardware_frees == hardware_allocations &&
                      hardware_closes == (retry || which >= 2 ? 2U : 1U) &&
                      !irq_depth && irq_disables == irq_enables,
                      "close after failed or retried acquisition releases all hardware and balances IRQs");
            }
        }
    }
}
static void AdmissionWithoutReopeningHardware(void)
{
    const uint32_t monitor_modes[] = { DTS_MONITOR_MODE, 0x100 | DTS_MONITOR_MODE,
        0x81000000 | DTS_MONITOR_MODE };

    for (unsigned n = 0; n < sizeof(monitor_modes) / sizeof(monitor_modes[0]); n++) {
        struct file pending = OpenPendingAfterOwnerClose(DTS_PLAYBACK_MODE);
        struct crystalhd_user *user = ((struct crystalhd_file *)pending.private_data)->user;
        crystalhd_ioctl_data data = { .u_id = user->uid };

        data.udata.u.NotifyMode.Mode = monitor_modes[n];
        Check(bc_cproc_notify_mode(&context, &data) == BC_STS_SUCCESS &&
              user->mode == monitor_modes[n] && !context.hw_ctx && !context.session_owner,
              "pending monitor admission does not recreate retired decoder hardware");
        data.udata.u.NotifyMode.Mode = DTS_PLAYBACK_MODE;
        Check(bc_cproc_notify_mode(&context, &data) == BC_STS_ERR_USAGE &&
              hardware_opens == 1 && hardware_alloc_attempts == 1 &&
              pools == 2 && rings == 1 && irq_disables == 2 + stops && irq_enables == 2 + stops,
              "monitor reconfiguration rejection does not create hardware or session resources");
        CloseFile(&pending);
        CheckNoSession();
    }
    for (unsigned which = 0; which < 3; which++) {
        for (unsigned mode = 0; mode < sizeof(resource_modes) / sizeof(resource_modes[0]); mode++) {
            crystalhd_ioctl_data data = { .u_id = 1 };

            Reset(BC_LINK_INVALID, false);
            context.user[1].in_use = 1;
            adapter.cfg_users = 1;
            if (which == 0) context.state = BC_LINK_READY;
            if (which == 1) context.user[0].mode = DTS_PLAYBACK_MODE;
            if (which == 2) context.session_owner = &context.user[0];
            data.udata.u.NotifyMode.Mode = resource_modes[mode];
            Check(bc_cproc_notify_mode(&context, &data) == BC_STS_ERR_USAGE &&
                  context.user[1].mode == (uint32_t)DTS_MODE_INV &&
                  context.user[1].in_use && adapter.cfg_users == 1 &&
                  !context.hw_ctx && !hardware_alloc_attempts && !hardware_opens &&
                  !pools && !rings && !irq_disables && !irq_enables,
                  "link-state, legacy-mode and explicit-owner arbitration precedes hardware creation");
        }
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
    const uint32_t values[] = { 0, 0x89abcdef, UINT32_MAX,
        0x00502120, 0x00002120, 0x0000fff8, 0x0000fffc,
        0x0050fff8, 0x0050fffc };
    const uint32_t devices[] = { BC_PCI_DEVID_FLEA, BC_PCI_DEVID_LINK };

    for (unsigned chip = 0; chip < sizeof(devices) / sizeof(devices[0]); chip++) {
        for (unsigned op = 0; op < 4; op++) {
            for (unsigned n = 0; n < sizeof(values) / sizeof(values[0]); n++) {
                crystalhd_ioctl_data data = {0};

                Reset(BC_LINK_INVALID, true);
                adapter.pdev->device = devices[chip];
                data.udata.u.regAcc.Offset = values[n];
                data.udata.u.regAcc.Value = op & 1 ? values[n] : ~values[n];
                raw_value = op & 1 ? ~values[n] : values[n];
                Check(raw_commands[op](&context, &data) == BC_STS_SUCCESS,
                      "valid device and link register commands retain successful status");
                Check(raw_calls[op] == 1 && RawCallCount() == 1 && raw_offset == values[n] &&
                      raw_value == values[n] && data.udata.u.regAcc.Value == values[n],
                      "register commands preserve offset and read or write the exact 32-bit value");
                Check(raw_dram_writes == ((devices[chip] == BC_PCI_DEVID_FLEA &&
                                          (op & 1)) ? 1U : 0U) && !adapter.dram_lock,
                      "both Flea raw-write routes serialize window aliases and GISB portals");
            }
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
        Check(captures == 1 && !capture_unmaps && ring_frees == 1 &&
              dio_destroys == 1 && elem_deletes == 1 &&
              pools == (owner ? 2U : 0U) && rings == owner,
              "common close safely handles empty resources without new allocations");
        Check(irq_disables == 2 + stops && irq_enables == 2 + stops && device_reads == 1 &&
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
                  !capture_unmaps && dio_destroys == hardware_opens &&
                  elem_deletes == hardware_opens && irq_disables == 2 * hardware_opens + stops &&
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
            Check(last_close_cfg_users == 1 && !capture_unmaps && ring_frees == 1 &&
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
              irq_disables == 2 + stops && irq_enables == 2 + stops && binding_frees == 1,
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
              irq_disables == 2 + stops && irq_enables == 2 + stops && binding_frees == 1,
              "file close after failed acquisition safely completes empty-resource cleanup");
    }
}
static void CloseCaptureFailure(void)
{
    for (unsigned failure = 0; failure < 3; failure++) {
        for (unsigned pending = 0; pending < 2; pending++) {
            for (unsigned via_release = 0; via_release < 3; via_release++) {
                struct file file = {0};
                crystalhd_ioctl_data data = {0};
                int generic_owner;
                const void *owner;
                unsigned before_captures, before_cancels, before_stops;

                Reset(BC_LINK_INVALID, false);
                if (via_release == 2) {
                    Check(crystalhd_session_acquire_locked(&context, &generic_owner) == BC_STS_SUCCESS,
                          "prepare generic ownership for a failed release barrier");
                } else {
                    file = OpenFile(DTS_PLAYBACK_MODE);
                    data.u_id = ((struct crystalhd_file *)file.private_data)->user->uid;
                }
                owner = context.session_owner;
                context.stream = &stream_cookie;
                context.decoder_phase = CRYSTALHD_DECODER_CHANNEL_STARTED;
                context.decoder_codec = CRYSTALHD_DECODER_CODEC_H264;
                context.decoder_channel_id = 7;
                fatal_pending = pending;
                if (failure == 0) capture_status = BC_STS_TIMEOUT;
                if (failure == 1) cancel_status = BC_STS_IO_ERROR;
                if (failure == 2) stop_ok = false;
                checking_module_callbacks = true;
                if (via_release == 2) {
                    Check(crystalhd_session_release_locked(&context, owner) != BC_STS_SUCCESS,
                          "generic release reports that its stop barrier did not retire ownership");
                } else {
                    if (via_release == 1)
                        Check(bc_cproc_release_user(&context, &data) == BC_STS_SUCCESS,
                              "legacy RELEASE keeps its accounting result while retaining unsafe backing");
                    CloseFile(&file);
                    Check(binding_frees == 1 && !adapter.cfg_users && !context.user[data.u_id].in_use,
                          "failed release still retires only the logical file binding");
                }
                Check(hardware.dma_fault && !adapter.present && context.cin_wait_exit &&
                      !adapter.dma_terminal_quiesced,
                      "every failed engine stop is terminal even when the local pending wait succeeds");
                Check(context.hw_ctx == &hardware && hardware_allocated && rings_live &&
                      dio_live && elem_live && !ring_frees && !dio_destroys && !elem_deletes &&
                      !hardware_frees && context.stream == &stream_cookie && !stream_releases,
                      "failed release retains payload owner, rings, pools and hardware context");
                Check(context.session_owner == owner && context.session_module_pinned &&
                      module_refs == 1 && !module_puts &&
                      context.decoder_phase == CRYSTALHD_DECODER_CHANNEL_STARTED &&
                      context.decoder_channel_id == 7,
                      "failed release does not reset decoder ownership or unload its callbacks");
                Check(!irq_depth && irq_disables == irq_enables,
                      "conditional teardown balances IRQ exclusion without pretending it stopped DMA");
                before_captures = captures;
                before_cancels = cancels;
                before_stops = stops;
                Check(crystalhd_session_release_locked(&context, owner) != BC_STS_SUCCESS &&
                      crystalhd_session_acquire_locked(&context, &generic_owner) != BC_STS_SUCCESS,
                      "terminal ownership cannot be released by retry or replaced by a new owner");
                Check(captures == before_captures && cancels == before_cancels && stops == before_stops &&
                      !ring_frees && !module_puts && module_refs == 1,
                      "rejected terminal retries have no stop, free or module-put effects");
            }
        }
    }
}
static void FatalCommandOwnership(void)
{
    struct file owner_file, pending_file;
    crystalhd_ioctl_data data = {0};
    const void *owner;

    Reset(BC_LINK_INVALID, false);
    owner_file = OpenResourceFile(DTS_PLAYBACK_MODE);
    pending_file = OpenFile(DTS_MODE_INV);
    owner = context.session_owner;
    data.u_id = ((struct crystalhd_file *)pending_file.private_data)->user->uid;
    data.udata.u.NotifyMode.Mode = DTS_PLAYBACK_MODE;
    crystalhd_hw_dma_fatal_stop(&hardware);
    CloseFile(&owner_file);
    Check(bc_cproc_notify_mode(&context, &data) != BC_STS_SUCCESS &&
          context.session_owner == owner && context.hw_ctx == &hardware,
          "pending unconfigured file cannot reopen a fatally retained owner's hardware");
    CloseFile(&pending_file);
    Check(!adapter.cfg_users && binding_frees == 2 && context.session_owner == owner &&
          context.session_module_pinned && module_refs == 1 && !module_puts &&
          elem_live && dio_live && rings_live && hardware_allocated &&
          !captures && !cancels && !stops && !hardware_frees && !ring_frees,
          "closing all files after fatal publication leaves every DMA owner pinned");

    for (unsigned proof = 0; proof < 2; proof++) {
        int generic_owner;
        unsigned previous_irqs;

        Reset(BC_LINK_INVALID, false);
        Check(crystalhd_session_acquire_locked(&context, &generic_owner) == BC_STS_SUCCESS,
              "prepare exact retained resources for terminal command-deletion entry");
        context.stream = &stream_cookie;
        retained_tx_lease = true;
        hardware.dma_fault = true;
        adapter.present = false;
        context.cin_wait_exit = 1;
        /* Independent caller-precondition cases, not a fabricated recovery
         * of the preceding failed-stop model. device-lifetime executes the
         * actual global-writer PCI/IRQ proof before command deletion.
         */
        adapter.dma_terminal_quiesced = proof;
        previous_irqs = irq_disables;
        checking_module_callbacks = module_expect_no_hardware_on_put = true;
        if (!proof) {
            Check(crystalhd_delete_cmd_context(&context) == BC_STS_IO_ERROR &&
                  context.adp == &adapter && context.hw_ctx == &hardware &&
                  context.stream == &stream_cookie && retained_tx_lease,
                  "terminal deletion without proof refuses to free uncertain command ownership");
            Check(elem_live && dio_live && rings_live && hardware_allocated &&
                  context.session_owner == &generic_owner && module_refs == 1 &&
                  !module_puts && !quiesced_tx_retires && !retained_tx_puts && !ring_frees,
                  "rejected terminal deletion retains the TX lease, pools and module owner");
        } else {
            Check(crystalhd_delete_cmd_context(&context) == BC_STS_SUCCESS &&
                  !context.adp && !context.hw_ctx && !context.stream && !retained_tx_lease,
                  "caller-proven command deletion consumes the exact retained ownership");
            Check(quiesced_tx_retires == 1 && retained_tx_puts == 1 && ring_frees == 1 &&
                  dio_destroys == 1 && elem_deletes == 1 && stream_releases == 1 &&
                  module_puts == 1 && !module_refs && !hardware_allocated,
                  "proven deletion returns the TX lease before pools, stream and final module pin");
        }
        Check(!captures && !cancels && !stops && irq_disables == previous_irqs &&
              irq_enables == previous_irqs,
              "terminal command deletion never attempts a new hardware stop or IRQ transition");
    }

    Reset(BC_LINK_INVALID, false);
    owner_file = OpenFile(DTS_MONITOR_MODE);
    stop_ok = false;
    CloseFile(&owner_file);
    Check(hardware.dma_fault && !adapter.present && hardware_allocated &&
          context.hw_ctx == &hardware && !context.session_owner && !context.stream &&
          !context.session_module_pinned && !module_refs && !module_gets &&
          !elem_live && !dio_live && !rings_live,
          "failed empty-monitor stop leaves no session or DMA backing to quarantine");
    Check(!adapter.dma_terminal_quiesced &&
          crystalhd_delete_cmd_context(&context) == BC_STS_SUCCESS &&
          !context.hw_ctx && !context.adp && !hardware_allocated &&
          !module_gets && !module_puts && !retained_tx_puts,
          "empty monitor context can be deleted from absence proof without a late rescue pin");

    for (unsigned failure = 0; failure < 3; failure++) {
        int generic_owner;
        BC_STATUS expected = failure == 2 ? BC_STS_INSUFF_RES : BC_STS_ERROR;

        Reset(BC_LINK_INVALID, false);
        if (failure == 0) elem_error = -1;
        if (failure == 1) dio_error = -1;
        if (failure == 2) ring_status = BC_STS_INSUFF_RES;
        stop_ok = false;
        Check(crystalhd_session_acquire_locked(&context, &generic_owner) == expected &&
              hardware.dma_fault && !adapter.present && context.hw_ctx == &hardware &&
              hardware_allocated && !context.session_owner,
              "failed setup rollback preserves hardware when its device stop also fails");
        Check(context.session_module_pinned && module_refs == 1 && module_gets == 1 &&
              !module_puts && !hardware_frees && !elem_live && !dio_live && !rings_live,
              "failed rollback retains its pre-acquired module pin after ordinary setup resources unwind");
        Check(crystalhd_session_acquire_locked(&context, &generic_owner) == BC_STS_BUSY &&
              module_gets == 1 && !module_puts && hardware_opens == 1 && stops == 1,
              "an unpublished retained module owner blocks new setup without another hardware attempt");
    }
}
static void FaultInterruptDispatch(void)
{
    for (unsigned fault = 0; fault < 2; fault++) {
        for (unsigned handled = 0; handled < 2; handled++) {
            struct crystalhd_cmd before_context;
            struct crystalhd_hw before_hw;

            Reset(BC_LINK_READY, true);
            hardware.pfnFindAndClearIntr = NormalInterrupt;
            interrupt_handled = handled;
            if (fault)
                crystalhd_hw_dma_fatal_stop(&hardware);
            before_context = context;
            before_hw = hardware;
            Check(crystalhd_cmd_interrupt(&context) == (bool)handled &&
                  normal_interrupts == !fault && fault_interrupts == fault,
                  "actual interrupt entry chooses exactly the normal or fatal-acknowledgement path");
            Check(!memcmp(&context, &before_context, sizeof(context)) &&
                  !memcmp(&hardware, &before_hw, sizeof(hardware)) &&
                  !captures && !cancels && !starts && !stops && !ring_frees &&
                  !stream_releases && !module_puts && !irq_disables && !irq_enables,
                  "late interrupt acknowledgement cannot complete, retire, repost or reset ownership");
        }
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
              context.session_owner == user && context.session_module_pinned &&
              module_refs == 1 && module_gets == 1 && !module_puts &&
              dio_live && rings_live && !captures && !stops && !ring_frees &&
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
            context.session_module_pinned = true;
            module_refs = module_gets = 1;
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
              hardware_opens == which + 1 && irq_disables == 2 + stops && irq_enables == 2 + stops,
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
    uint32_t firmware[CRYSTALHD_FLEA_MIN_FIRMWARE_SIZE / 4] = {0};
    crystalhd_ioctl_data data = {
        .u_id = 1,
        .add_cdata = firmware,
        .add_cdata_sz = sizeof(firmware),
    };
    unsigned cycle;
    ((uint8_t *)firmware)[0] = 0x5a;
    Reset(BC_LINK_INVALID, true);
    CheckNotify();
    for (cycle = 0; cycle < 4; cycle++) {
        Check(crystalhd_suspend(&context, &data) == BC_STS_SUCCESS, "pre-firmware suspend succeeds");
        Check(crystalhd_resume(&context) == BC_STS_SUCCESS, "pre-firmware resume succeeds");
        Check(context.state == BC_LINK_INVALID && context.user[1].mode == DTS_PLAYBACK_MODE &&
              context.pwr_state_change == BC_HW_RUNNING && starts == cycle + 1 && !stops,
              "pre-firmware playback retains its configured owner and idle state");
    }
    adapter.user_lock = 1;
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
        Check(!strcmp(events, "CXS"),
              "capture and engine-wide TX cancellation precede device suspend");
        Check(context.state == BC_LINK_SUSPEND && context.pwr_state_change == BC_HW_SUSPEND,
              "active suspend reports its power state");
        Check(crystalhd_suspend(&context, &data) == BC_STS_SUCCESS, "duplicate suspend is harmless");
        Check(stops == 1 && captures == 1 && cancels == 1 && !tx_stops,
              "duplicate suspend does not stop or cancel hardware twice");
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
        hardware.fwcmd_pending = hardware.fwcmd_poisoned = true;
        hardware.FwCmdCnt = 7;
        rings_live = true;
        hardware.rx_freeq = &rings_live;
        Check(crystalhd_resume(&context) == (fault ? BC_STS_IO_ERROR : BC_STS_ERROR),
              "DMA-fault/device-start failures propagate for active and monitor contexts");
        Check(!memcmp(&before, &context, sizeof(context)), "resume failure does not publish success state");
        Check(starts == (fault ? 0U : 1U), "DMA fault prevents hardware restart");
        Check(!download_resets && hardware.fwcmd_pending &&
              hardware.fwcmd_poisoned && hardware.FwCmdCnt == 7,
              "failed warm resume does not clear command quarantine");
        Check(rings_live && hardware.rx_freeq == &rings_live && !ring_frees &&
              !dio_destroys && !elem_deletes && !hardware_closes,
              "failed warm resume retains DMA owners and session storage");
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
        {"frontend-neutral decoder session arbitration", FrontendNeutralSessionOwner},
        {"session module pre-pin admission and denial", ModulePinAdmission},
        {"private session setup requires an existing module pin", ModulePinPrivateSetup},
        {"session module pre-pin setup rollback and retry", ModulePinSetupFailures},
        {"session module owner and callback lifetime", ModulePinOwnerLifetime},
        {"session module setup guards and final context teardown", ModulePinContextGuards},
        {"session module lifetime through PM and accounting-only close", ModulePinPowerLifetime},
        {"reference-backed owner admission and callback contract", ReferencedOwnerAdmission},
        {"reference-backed safe and uncertain setup rollback", ReferencedOwnerSetupRollback},
        {"reference-backed normal and terminal exact-once retirement", ReferencedOwnerRetirement},
        {"reference-backed fatal ownership retention", ReferencedOwnerFaultRetention},
        {"reference-backed owner lifetime across suspend and RX retirement", ReferencedOwnerPowerLifetime},
        {"quiesced RX retirement policy and early states", QuiescedRxPolicy},
        {"RX retention frontend admission and release", RxRetentionAdmission},
        {"RX retention context setup and deletion", RxRetentionContextReset},
        {"pending unconfigured handle after resource owner close", PendingOpenAfterOwnerClose},
        {"pending acquisition failure rollback, retry and close", PendingAcquisitionFailures},
        {"monitor and rejected admission avoid hardware recreation", AdmissionWithoutReopeningHardware},
        {"raw command invalid arguments and retired hardware", RawInvalidArguments},
        {"raw device and link register read/write propagation", RawRegisterCommands},
        {"raw memory bounds, read/write propagation and callback errors", RawMemoryCommands},
        {"surviving monitor raw commands after actual owner close", MonitorRawAfterOwnerClose},
        {"user open failure rollback and retry", OpenFailuresAndRetry},
        {"legacy user slot exhaustion", OpenSlotExhaustion},
        {"invalid admission adapter arguments", InvalidAdmissionArguments},
        {"shared and legacy firmware command entry points", FirmwareSharedEntry},
        {"frontend-neutral firmware download entry point", FirmwareDownloadSharedEntry},
        {"legacy firmware download adapter", FirmwareDownloadLegacyAdapter},
        {"kernel firmware request preconditions", KernelFirmwarePreconditions},
        {"kernel firmware chip selection and lifetime", KernelFirmwareSelectionAndLifetime},
        {"kernel firmware request and shared admission errors", KernelFirmwareRequestAndAdmissionErrors},
        {"kernel firmware status translation", KernelFirmwareStatusTranslation},
        {"kernel firmware bootstrap preconditions", KernelFirmwareBootstrapPreconditions},
        {"kernel firmware bootstrap INIT payload", KernelFirmwareBootstrapPayload},
        {"kernel firmware bootstrap failure rollback", KernelFirmwareBootstrapFailures},
        {"kernel firmware bootstrap timeout retry", KernelFirmwareBootstrapTimeoutRetry},
        {"BCM70015 typed channel payload and publication", KernelDecoderChannelOpenPayload},
        {"BCM70015 typed channel preconditions", KernelDecoderChannelOpenPreconditions},
        {"BCM70015 typed channel failure and recovery", KernelDecoderChannelOpenFailures},
        {"BCM70015 typed start payload and publication", KernelDecoderChannelStartPayload},
        {"BCM70015 typed start preconditions", KernelDecoderChannelStartPreconditions},
        {"BCM70015 typed start failure and recovery", KernelDecoderChannelStartFailures},
        {"BCM70015 typed STOP/CLOSE payload and publication", KernelDecoderChannelStopClosePayload},
        {"BCM70015 typed STOP/CLOSE preconditions", KernelDecoderChannelStopClosePreconditions},
        {"BCM70015 typed STOP/CLOSE failure and recovery", KernelDecoderChannelStopCloseFailures},
        {"BCM70015 typed channel suspend recovery", KernelDecoderChannelSuspendRecovery},
        {"firmware pause/resume rollback", FirmwarePauseRollback},
        {"firmware download serialization", FirmwareDownloadSerialization},
        {"firmware transaction serialization", FirmwareTransactionSerialization},
        {"firmware command versus hardware suspend and close", FirmwareSuspendSerialization},
        {"actual hardware close across handle counts and stop failures", HardwareClose},
        {"active session open, busy, release and reopen", SessionOwnership},
        {"last monitor releases empty session resources", MonitorOnlyRelease},
        {"playback owner closes before monitor and reacquires", OwnerBeforeMonitor},
        {"actual file close across unconfigured, monitor, playback and diagnostic modes", FileCloseModes},
        {"actual file close owner/monitor ordering and reacquisition", FileOwnerBeforeMonitor},
        {"RELEASE then actual file close", ReleaseThenFileClose},
        {"actual file close after session setup failure", FileCloseAfterSetupFailure},
        {"conditional close retains ownership after every engine-stop failure", CloseCaptureFailure},
        {"fatal pending-file and terminal command ownership", FatalCommandOwnership},
        {"late fatal interrupt acknowledgement dispatch", FaultInterruptDispatch},
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
