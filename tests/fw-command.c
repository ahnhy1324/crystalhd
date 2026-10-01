/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Exact firmware-command state helpers; no device is opened. */
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "bc_dts_types.h"
struct _BC_DTS_PROC_OUT;
#include "bc_dts_defs.h"
#include "crystalhd_ioctl_limits.h"
#include "crystalhd_fw_if.h"
#include "DriverFwShare.h"
#include "FleaDefs.h"

#define FW_CMD_BUFF_SZ 64U
#define C011_RET_SUCCESS 0U
typedef struct {
    uint32_t cmd[FW_CMD_BUFF_SZ], rsp[FW_CMD_BUFF_SZ], flags, add_data;
} BC_FW_CMD;
_Static_assert(sizeof(BC_FW_CMD) == 520U, "firmware command ABI fixture changed");

struct mutex { pthread_mutex_t native; };
typedef pthread_mutex_t spinlock_t;
typedef struct {
    pthread_mutex_t lock;
    pthread_cond_t changed;
    unsigned wakeups;
} wait_queue_head_t;

struct device { int unused; };
struct pci_dev { struct device dev; };
struct crystalhd_adp { bool present; struct pci_dev *pdev; };

struct crystalhd_hw {
    struct crystalhd_adp *adp;
    bool dma_fault;
    spinlock_t lock;
    struct mutex fwcmd_trans_mutex;
    struct mutex fwcmd_mutex;
    wait_queue_head_t fwcmd_event;
    bool fwcmd_pending;
    bool fwcmd_poisoned;
    int fwcmd_evt_sts;
    uint32_t FwCmdCnt;
    BC_STATUS (*pfnDevDRAMWrite)(struct crystalhd_hw *, uint32_t,
                               uint32_t, const uint32_t *);
    BC_STATUS (*pfnDevDRAMRead)(struct crystalhd_hw *, uint32_t,
                              uint32_t, uint32_t *);
    uint32_t (*pfnReadDevRegister)(struct crystalhd_adp *, uint32_t);
    void (*pfnWriteDevRegister)(struct crystalhd_adp *, uint32_t, uint32_t);
    uint32_t fwcmdPostAddr, fwcmdPostMbox, fwcmdRespMbox;
    uint32_t channelNum, TxBuffInfoAddr, pib_del_Q_addr, pib_rel_Q_addr;
    TX_INPUT_BUFFER_INFO TxFwInputBuffInfo;
    enum FLEA_POWER_STATES FleaPowerState;
    bool PwrDwnTxIntr, PwrDwnPiQIntr, SingleThreadAppFIFOEmpty;
    uint32_t EmptyCnt;
};

enum wait_mode {
    WAIT_BLOCK,
    WAIT_TIMEOUT,
    WAIT_COMPLETE_AT_TIMEOUT,
    WAIT_COMPLETE_IMMEDIATELY,
    WAIT_SIGNAL,
    WAIT_IO_ERROR,
};

static struct crystalhd_hw hardware;
static struct crystalhd_adp adapter;
static struct pci_dev pci;
static enum wait_mode wait_mode;
static wait_queue_head_t *last_woken;
static pthread_mutex_t audit_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t audit_changed = PTHREAD_COND_INITIALIZER;
static unsigned mutex_attempts;
static bool interrupt_mailbox_begin;
static bool complete_before_publication;
static unsigned wait_calls;

static void crystalhd_hw_fw_cmd_complete(struct crystalhd_hw *hw);
static void TestSpinLock(spinlock_t *lock);
static void AssertSpinHeld(spinlock_t *lock);
static void AssertMutexHeld(struct mutex *mutex);

static int TestMutexLock(struct mutex *mutex)
{
    pthread_mutex_lock(&audit_lock);
    mutex_attempts++;
    pthread_cond_broadcast(&audit_changed);
    pthread_mutex_unlock(&audit_lock);
    if (interrupt_mailbox_begin && mutex == &hardware.fwcmd_mutex)
        return -EINTR;
    return pthread_mutex_lock(&mutex->native);
}

static void TestMutexUnlock(struct mutex *mutex)
{
    if (pthread_mutex_unlock(&mutex->native)) abort();
}

static void TestWake(wait_queue_head_t *event)
{
    if (pthread_mutex_lock(&event->lock)) abort();
    last_woken = event;
    event->wakeups++;
    pthread_cond_broadcast(&event->changed);
    if (pthread_mutex_unlock(&event->lock)) abort();
}

static int TestWait(wait_queue_head_t *event)
{
    wait_calls++;
    if (wait_mode == WAIT_TIMEOUT)
        return -EBUSY;
    if (wait_mode == WAIT_SIGNAL)
        return -EINTR;
    if (wait_mode == WAIT_IO_ERROR)
        return -EIO;
    if (wait_mode == WAIT_COMPLETE_AT_TIMEOUT) {
        crystalhd_hw_fw_cmd_complete(&hardware);
        return -EBUSY;
    }
    if (wait_mode == WAIT_COMPLETE_IMMEDIATELY) {
        crystalhd_hw_fw_cmd_complete(&hardware);
        return 0;
    }
    if (pthread_mutex_lock(&event->lock)) abort();
    while (!hardware.fwcmd_evt_sts)
        if (pthread_cond_wait(&event->changed, &event->lock)) abort();
    if (pthread_mutex_unlock(&event->lock)) abort();
    return 0;
}

#define READ_ONCE(value) (value)
#define mutex_lock(mutex) do { if (pthread_mutex_lock(&(mutex)->native)) abort(); } while (0)
#define mutex_lock_interruptible(mutex) TestMutexLock(mutex)
#define mutex_unlock(mutex) TestMutexUnlock(mutex)
#define spin_lock_irqsave(lock, flags) do { \
    (flags) = 0; \
    TestSpinLock(lock); \
} while (0)
#define spin_unlock_irqrestore(lock, flags) do { \
    (void)(flags); \
    if (pthread_mutex_unlock(lock)) abort(); \
} while (0)
#define crystalhd_set_event(event) TestWake(event)
#define crystalhd_wait_on_event(event, condition, timeout, ret, nosig) do { \
    (void)(condition); (void)(timeout); (void)(nosig); \
    (ret) = TestWait(event); \
} while (0)
#define KERN_ERR ""
#define printk(...) ((void)0)
#define dev_dbg(dev, ...) ((void)(dev))
#define dev_err(dev, ...) ((void)(dev))
#define dev_info(dev, ...) ((void)(dev))
#define msleep_interruptible(milliseconds) TestSleep(milliseconds)
#define lockdep_assert_held(lock) _Generic((lock), \
    struct mutex *: AssertMutexHeld, spinlock_t *: AssertSpinHeld)(lock)

static int TestSleep(unsigned milliseconds);
static bool crystalhd_link_load_firmware_config(struct crystalhd_hw *hw);
static void crystalhd_flea_set_next_power_state(struct crystalhd_hw *hw,
                                               enum FLEA_STATE_CH_EVENT event);

#include "fw-command-functions.h"
#include "fw-transport-functions.h"

static void TestSpinLock(spinlock_t *lock)
{
    if (complete_before_publication && lock == &hardware.lock &&
        hardware.fwcmd_pending) {
        complete_before_publication = false;
        crystalhd_hw_fw_cmd_complete(&hardware);
    }
    if (pthread_mutex_lock(lock)) abort();
}

static unsigned checks, failures;
static void Check(bool condition, const char *why)
{
    checks++;
    if (!condition) {
        failures++;
        fprintf(stderr, "FAIL: %s\n", why);
    }
}

static void InitHardware(void)
{
    memset(&hardware, 0, sizeof(hardware));
    memset(&adapter, 0, sizeof(adapter));
    adapter.present = true;
    adapter.pdev = &pci;
    hardware.adp = &adapter;
    if (pthread_mutex_init(&hardware.lock, NULL) ||
        pthread_mutex_init(&hardware.fwcmd_trans_mutex.native, NULL) ||
        pthread_mutex_init(&hardware.fwcmd_mutex.native, NULL) ||
        pthread_mutex_init(&hardware.fwcmd_event.lock, NULL) ||
        pthread_cond_init(&hardware.fwcmd_event.changed, NULL))
        abort();
    last_woken = NULL;
    wait_mode = WAIT_BLOCK;
    interrupt_mailbox_begin = false;
    complete_before_publication = false;
    wait_calls = 0;
    pthread_mutex_lock(&audit_lock);
    mutex_attempts = 0;
    pthread_mutex_unlock(&audit_lock);
}

static void DestroyHardware(void)
{
    if (pthread_cond_destroy(&hardware.fwcmd_event.changed) ||
        pthread_mutex_destroy(&hardware.fwcmd_event.lock) ||
        pthread_mutex_destroy(&hardware.fwcmd_mutex.native) ||
        pthread_mutex_destroy(&hardware.fwcmd_trans_mutex.native) ||
        pthread_mutex_destroy(&hardware.lock))
        abort();
}

static void AdmissionBeforePreprocess(void)
{
    int rc;

    InitHardware();
    hardware.fwcmd_poisoned = true;
    Check(crystalhd_hw_fw_cmd_enter(&hardware) == BC_STS_BUSY,
          "normal transaction admission rejects a poisoned mailbox");
    rc = pthread_mutex_trylock(&hardware.fwcmd_trans_mutex.native);
    Check(!rc, "rejected poisoned admission releases transaction serialization");
    if (!rc) pthread_mutex_unlock(&hardware.fwcmd_trans_mutex.native);

    hardware.fwcmd_poisoned = false;
    hardware.fwcmd_pending = true;
    Check(crystalhd_hw_fw_cmd_enter(&hardware) == BC_STS_BUSY,
          "normal transaction admission rejects externally pending mailbox work");
    rc = pthread_mutex_trylock(&hardware.fwcmd_trans_mutex.native);
    Check(!rc, "rejected pending admission releases transaction serialization");
    if (!rc) pthread_mutex_unlock(&hardware.fwcmd_trans_mutex.native);
    Check(crystalhd_hw_fw_cmd_recovery_enter(&hardware) == BC_STS_BUSY,
          "reset recovery cannot overtake actively pending mailbox work");

    hardware.fwcmd_pending = false;
    Check(crystalhd_hw_fw_cmd_enter(&hardware) == BC_STS_SUCCESS,
          "clean normal transaction admission retains serialization");
    crystalhd_hw_fw_cmd_leave(&hardware);

    hardware.fwcmd_poisoned = true;
    Check(crystalhd_hw_fw_cmd_recovery_enter(&hardware) == BC_STS_SUCCESS,
          "verified-reset transaction may enter a poisoned mailbox");
    crystalhd_hw_fw_cmd_leave(&hardware);
    DestroyHardware();
}

static void CheckUnlocked(pthread_mutex_t *lock, const char *why)
{
    int rc = pthread_mutex_trylock(lock);

    Check(!rc, why);
    if (!rc && pthread_mutex_unlock(lock)) abort();
}

static void UnavailableAdmission(void)
{
    BC_STATUS (*const enter[])(struct crystalhd_hw *) = {
        crystalhd_hw_fw_cmd_enter,
        crystalhd_hw_fw_cmd_recovery_enter,
    };
    unsigned entry, unavailable, mailbox;

    for (entry = 0; entry < sizeof(enter) / sizeof(enter[0]); entry++) {
        for (unavailable = 1; unavailable < 4; unavailable++) {
            for (mailbox = 0; mailbox < 4; mailbox++) {
                /* Each case is a fresh device, not a recovery that clears a
                 * sticky DMA fault or revives a removed adapter.
                 */
                InitHardware();
                hardware.dma_fault = !!(unavailable & 1);
                adapter.present = !(unavailable & 2);
                hardware.fwcmd_pending = !!(mailbox & 1);
                hardware.fwcmd_poisoned = !!(mailbox & 2);
                hardware.fwcmd_evt_sts = 7;
                hardware.FwCmdCnt = 11;
                hardware.fwcmd_event.wakeups = 13;
                last_woken = &hardware.fwcmd_event;

                Check(enter[entry](&hardware) == BC_STS_IO_ERROR,
                      "both entries reject DMA fault or absence before mailbox admission");
                Check(hardware.fwcmd_pending == !!(mailbox & 1) &&
                      hardware.fwcmd_poisoned == !!(mailbox & 2) &&
                      hardware.fwcmd_evt_sts == 7 && hardware.FwCmdCnt == 11,
                      "unavailable admission preserves all mailbox state, even pending/poisoned");
                Check(hardware.fwcmd_event.wakeups == 13 &&
                      last_woken == &hardware.fwcmd_event,
                      "unavailable admission neither wakes nor replaces the firmware waitqueue");
                Check(hardware.adp == &adapter &&
                      hardware.dma_fault == !!(unavailable & 1) &&
                      adapter.present == !(unavailable & 2),
                      "denied admission cannot clear fault or restore device presence");
                Check(mutex_attempts == 1,
                      "denied admission acquires only transaction serialization");
                CheckUnlocked(&hardware.fwcmd_trans_mutex.native,
                              "denied admission releases transaction serialization");
                CheckUnlocked(&hardware.lock,
                              "denied admission releases the hardware state lock");
                CheckUnlocked(&hardware.fwcmd_mutex.native,
                              "denied admission leaves mailbox serialization available");
                CheckUnlocked(&hardware.fwcmd_event.lock,
                              "denied admission leaves the firmware waitqueue unlocked");
                DestroyHardware();
            }
        }
    }
}

static void NormalAndLateCompletion(void)
{
    unsigned wakes;

    InitHardware();
    Check(crystalhd_hw_fw_cmd_begin(&hardware) == BC_STS_SUCCESS,
          "a fresh firmware command is admitted");
    Check(hardware.fwcmd_pending && hardware.FwCmdCnt == 1,
          "admission publishes exactly one pending command");
    crystalhd_hw_fw_cmd_complete(&hardware);
    Check(last_woken == &hardware.fwcmd_event && hardware.fwcmd_evt_sts == 1 &&
          !hardware.fwcmd_pending && !hardware.FwCmdCnt,
          "the ISR wakes the persistent per-hardware waitqueue and balances the count");
    Check(crystalhd_hw_fw_cmd_wait(&hardware) == BC_STS_SUCCESS,
          "the completed command observes its response");
    crystalhd_hw_fw_cmd_end(&hardware);

    wakes = hardware.fwcmd_event.wakeups;
    crystalhd_hw_fw_cmd_complete(&hardware);
    Check(hardware.fwcmd_event.wakeups == wakes && !hardware.FwCmdCnt,
          "an idle late or duplicate interrupt is harmless after caller return");
    DestroyHardware();
}

static void TimeoutLateIrqAndRetry(void)
{
    InitHardware();
    wait_mode = WAIT_TIMEOUT;
    Check(crystalhd_hw_fw_cmd_begin(&hardware) == BC_STS_SUCCESS,
          "the timeout case admits one command");
    Check(crystalhd_hw_fw_cmd_wait(&hardware) == BC_STS_TIMEOUT,
          "a bounded wait reports timeout");
    Check(!hardware.fwcmd_pending && hardware.fwcmd_poisoned &&
          hardware.FwCmdCnt == 1,
          "timeout quarantines and keeps the outstanding command counted");
    crystalhd_hw_fw_cmd_end(&hardware);
    Check(crystalhd_hw_fw_cmd_begin(&hardware) == BC_STS_BUSY,
          "a retry cannot overtake the timed-out response");

    crystalhd_hw_fw_cmd_complete(&hardware);
    Check(hardware.fwcmd_poisoned && !hardware.fwcmd_pending &&
          !hardware.FwCmdCnt && last_woken == &hardware.fwcmd_event,
          "the late IRQ retires the power count but preserves quarantine");
    Check(crystalhd_hw_fw_cmd_begin(&hardware) == BC_STS_BUSY,
          "a late response cannot make an unreconciled mailbox reusable");

    crystalhd_hw_fw_cmd_reset(&hardware);
    Check(!hardware.fwcmd_poisoned && !hardware.fwcmd_pending &&
          !hardware.FwCmdCnt,
          "a verified reset clears the timed-out command quarantine");

    wait_mode = WAIT_BLOCK;
    Check(crystalhd_hw_fw_cmd_begin(&hardware) == BC_STS_SUCCESS &&
          hardware.FwCmdCnt == 1,
          "retry is admitted after the stale completion is consumed");
    crystalhd_hw_fw_cmd_complete(&hardware);
    Check(crystalhd_hw_fw_cmd_wait(&hardware) == BC_STS_SUCCESS,
          "the retried command completes normally");
    crystalhd_hw_fw_cmd_end(&hardware);
    Check(!hardware.FwCmdCnt && !hardware.fwcmd_poisoned,
          "retry leaves balanced firmware-command state");
    DestroyHardware();
}

static void TimeoutBoundaryAndReset(void)
{
    InitHardware();
    wait_mode = WAIT_COMPLETE_AT_TIMEOUT;
    Check(crystalhd_hw_fw_cmd_begin(&hardware) == BC_STS_SUCCESS,
          "the boundary case admits one command");
    Check(crystalhd_hw_fw_cmd_wait(&hardware) == BC_STS_SUCCESS,
          "an IRQ racing the timeout boundary wins when completion is recorded");
    Check(!hardware.fwcmd_pending && !hardware.fwcmd_poisoned &&
          !hardware.FwCmdCnt,
          "the timeout/IRQ race decrements the count exactly once");
    crystalhd_hw_fw_cmd_end(&hardware);

    wait_mode = WAIT_TIMEOUT;
    Check(crystalhd_hw_fw_cmd_begin(&hardware) == BC_STS_SUCCESS &&
          crystalhd_hw_fw_cmd_wait(&hardware) == BC_STS_TIMEOUT,
          "a second command reaches the reset recovery case");
    crystalhd_hw_fw_cmd_end(&hardware);
    Check(hardware.fwcmd_poisoned && hardware.FwCmdCnt == 1,
          "reset recovery starts with one quarantined outstanding command");
    crystalhd_hw_fw_cmd_reset(&hardware);
    Check(!hardware.fwcmd_pending && !hardware.fwcmd_poisoned &&
          !hardware.fwcmd_evt_sts && !hardware.FwCmdCnt,
          "a quiesced hardware reset clears stale command state");
    DestroyHardware();
}

struct command_thread {
    bool acquired;
    BC_STATUS begin_status, wait_status;
};
static struct command_thread threaded[2];

static void *RunCommand(void *argument)
{
    struct command_thread *thread = argument;

    thread->begin_status = crystalhd_hw_fw_cmd_begin(&hardware);
    pthread_mutex_lock(&audit_lock);
    thread->acquired = thread->begin_status == BC_STS_SUCCESS;
    pthread_cond_broadcast(&audit_changed);
    pthread_mutex_unlock(&audit_lock);
    if (thread->begin_status == BC_STS_SUCCESS) {
        thread->wait_status = crystalhd_hw_fw_cmd_wait(&hardware);
        crystalhd_hw_fw_cmd_end(&hardware);
    }
    return NULL;
}

static void SerializedCommands(void)
{
    pthread_t first, second;

    InitHardware();
    memset(threaded, 0, sizeof(threaded));
    if (pthread_create(&first, NULL, RunCommand, &threaded[0])) abort();
    pthread_mutex_lock(&audit_lock);
    while (!threaded[0].acquired)
        pthread_cond_wait(&audit_changed, &audit_lock);
    pthread_mutex_unlock(&audit_lock);

    if (pthread_create(&second, NULL, RunCommand, &threaded[1])) abort();
    pthread_mutex_lock(&audit_lock);
    while (mutex_attempts < 2)
        pthread_cond_wait(&audit_changed, &audit_lock);
    Check(!threaded[1].acquired && hardware.fwcmd_pending &&
          hardware.FwCmdCnt == 1,
          "a concurrent caller blocks before it can replace the active command");
    pthread_mutex_unlock(&audit_lock);

    crystalhd_hw_fw_cmd_complete(&hardware);
    pthread_mutex_lock(&audit_lock);
    while (!threaded[1].acquired)
        pthread_cond_wait(&audit_changed, &audit_lock);
    Check(threaded[0].wait_status == BC_STS_SUCCESS &&
          hardware.fwcmd_pending && hardware.FwCmdCnt == 1,
          "the second command starts only after the first releases serialization");
    pthread_mutex_unlock(&audit_lock);

    crystalhd_hw_fw_cmd_complete(&hardware);
    if (pthread_join(first, NULL) || pthread_join(second, NULL)) abort();
    Check(threaded[0].begin_status == BC_STS_SUCCESS &&
          threaded[0].wait_status == BC_STS_SUCCESS &&
          threaded[1].begin_status == BC_STS_SUCCESS &&
          threaded[1].wait_status == BC_STS_SUCCESS,
          "both serialized callers receive their own completion");
    Check(!hardware.fwcmd_pending && !hardware.fwcmd_poisoned &&
          !hardware.FwCmdCnt,
          "serialized completion leaves no outstanding command count");
    DestroyHardware();
}

static BC_FW_CMD *transfer_command;
static uint32_t response_words[FW_CMD_BUFF_SZ], response_address;
static BC_STATUS response_read_status;
static BC_STATUS command_write_status, flush_read_status;
static bool partial_response;
static bool partial_command;
static uint32_t written_command[FW_CMD_BUFF_SZ];
static unsigned transfer_reads, transfer_writes, mailbox_reads, mailbox_posts;
static unsigned sleeps, firmware_config_calls, power_wakes;

static bool WordsZero(const uint32_t *words, unsigned count)
{
    unsigned word;

    for (word = 0; word < count; word++)
        if (words[word]) return false;
    return true;
}

static void CheckHardwareLocked(void)
{
    int rc = pthread_mutex_trylock(&hardware.lock);

    Check(rc == EBUSY, "mailbox and DRAM callbacks run under the hardware lock");
    if (!rc && pthread_mutex_unlock(&hardware.lock)) abort();
}

static void AssertSpinHeld(spinlock_t *lock)
{
    Check(lock == &hardware.lock, "abort asserts the active hardware lock");
    CheckHardwareLocked();
}

static void AssertMutexHeld(struct mutex *mutex)
{
    int rc = pthread_mutex_trylock(&mutex->native);

    Check(mutex == &hardware.fwcmd_mutex && rc == EBUSY,
          "abort requires the current mailbox serialization lock");
    if (!rc && pthread_mutex_unlock(&mutex->native)) abort();
}

static int TestSleep(unsigned milliseconds)
{
    Check(milliseconds == 50, "production mailbox wait retains its bounded delay");
    sleeps++;
    return 0;
}

static bool crystalhd_link_load_firmware_config(struct crystalhd_hw *hw)
{
    Check(hw == &hardware, "Link INIT postprocessing uses the active hardware");
    firmware_config_calls++;
    return true;
}

static void crystalhd_flea_set_next_power_state(struct crystalhd_hw *hw,
                                               enum FLEA_STATE_CH_EVENT event)
{
    Check(hw == &hardware && event == FLEA_EVT_FW_CMD_POST,
          "Flea power wake uses only the firmware-post event");
    power_wakes++;
}

static BC_STATUS WriteCommand(struct crystalhd_hw *hw, uint32_t address,
                              uint32_t count, const uint32_t *words)
{
    CheckHardwareLocked();
    Check(hw == &hardware && address == hw->fwcmdPostAddr &&
          count == FW_CMD_BUFF_SZ && words == transfer_command->cmd,
          "production sends the unchanged complete command buffer");
    transfer_writes++;
    if (command_write_status == BC_STS_SUCCESS || partial_command)
        memcpy(written_command, words,
               (partial_command ? 8U : count) * sizeof(*words));
    return command_write_status;
}

static BC_STATUS ReadTransfer(struct crystalhd_hw *hw, uint32_t address,
                             uint32_t count, uint32_t *words)
{
    CheckHardwareLocked();
    Check(hw == &hardware, "DRAM reads use the active hardware");
    transfer_reads++;
    if (count == 1) {
        Check(address == hw->fwcmdPostAddr && transfer_reads == 1,
              "command publication flush precedes reply read");
        *words = transfer_command->cmd[0];
        return flush_read_status;
    }
    Check(count == FW_CMD_BUFF_SZ && address == response_address &&
          words == transfer_command->rsp && transfer_reads == 2,
          "production reads the complete reply from the response mailbox address");
    Check(WordsZero(words, count), "caller-supplied response data is cleared before reading");
    if (!crystalhd_valid_dram_range(address, count))
        return BC_STS_INV_ARG;
    if (response_read_status == BC_STS_SUCCESS || partial_response)
        memcpy(words, response_words,
               (partial_response ? 12U : count) * sizeof(*words));
    return response_read_status;
}

static uint32_t ReadMailbox(struct crystalhd_adp *adp, uint32_t address)
{
    CheckHardwareLocked();
    Check(adp == &adapter && address == hardware.fwcmdRespMbox,
          "only the firmware response mailbox register is read");
    mailbox_reads++;
    return response_address;
}

static void PostMailbox(struct crystalhd_adp *adp, uint32_t address, uint32_t value)
{
    CheckHardwareLocked();
    Check(adp == &adapter && address == hardware.fwcmdPostMbox &&
          value == hardware.fwcmdPostAddr,
          "command mailbox publishes the command address exactly once");
    mailbox_posts++;
}

static void InitTransfer(BC_FW_CMD *command, uint32_t id, bool stale)
{
    unsigned word;

    InitHardware();
    memset(command, 0, sizeof(*command));
    command->cmd[0] = id;
    command->cmd[1] = 17;
    command->flags = 0x1234;
    command->add_data = 0x5678;
    if (stale)
        for (word = 0; word < FW_CMD_BUFF_SZ; word++)
            command->rsp[word] = word == 2 ? 0 : 0x11110000U + word;
    transfer_command = command;
    memset(response_words, 0, sizeof(response_words));
    response_words[0] = id;
    response_words[1] = 17;
    response_words[3] = 7;
    response_words[5] = 0x12300;
    response_words[6] = 0x12400;
    response_words[11] = 0x12500;
    response_address = 0x200;
    response_read_status = BC_STS_SUCCESS;
    command_write_status = flush_read_status = BC_STS_SUCCESS;
    partial_response = false;
    partial_command = false;
    memset(written_command, 0, sizeof(written_command));
    transfer_reads = transfer_writes = mailbox_reads = mailbox_posts = 0;
    sleeps = firmware_config_calls = power_wakes = 0;
    hardware.pfnDevDRAMWrite = WriteCommand;
    hardware.pfnDevDRAMRead = ReadTransfer;
    hardware.pfnReadDevRegister = ReadMailbox;
    hardware.pfnWriteDevRegister = PostMailbox;
    hardware.fwcmdPostAddr = 0x100;
    hardware.fwcmdPostMbox = 0x1000;
    hardware.fwcmdRespMbox = 0x1004;
    hardware.FleaPowerState = FLEA_PS_ACTIVE;
    hardware.channelNum = 0xaabb;
    hardware.TxBuffInfoAddr = 0xbbcc;
    hardware.pib_del_Q_addr = 0xccdd;
    hardware.pib_rel_Q_addr = 0xddee;
    memset(&hardware.TxFwInputBuffInfo, 0x5a, sizeof(hardware.TxFwInputBuffInfo));
    hardware.PwrDwnTxIntr = hardware.PwrDwnPiQIntr = true;
    hardware.SingleThreadAppFIFOEmpty = true;
    hardware.EmptyCnt = 123;
    wait_mode = WAIT_COMPLETE_IMMEDIATELY;
}

static void ReplyReadFailure(void)
{
    BC_STATUS (*const execute[])(struct crystalhd_hw *, BC_FW_CMD *) = {
        crystalhd_flea_do_fw_cmd, crystalhd_link_do_fw_cmd,
    };
    const uint32_t commands[] = {
        eCMD_C011_INIT, eCMD_C011_GET_VERSION, eCMD_C011_DEC_CHAN_OPEN,
        eCMD_C011_DEC_CHAN_STATUS, eCMD_C011_DEC_CHAN_CLOSE,
        eCMD_C011_DEC_CHAN_START_VIDEO, eCMD_C011_DEC_CHAN_STREAM_OPEN,
    };
    const BC_STATUS errors[] = {
        BC_STS_INV_ARG, BC_STS_BUSY, BC_STS_ERROR, BC_STS_IO_ERROR,
        BC_STS_FW_CMD_ERR,
    };
    unsigned generation, cmd, error, content;

    for (generation = 0; generation < 2; generation++) {
        for (cmd = 0; cmd < sizeof(commands) / sizeof(commands[0]); cmd++) {
            for (error = 0; error < sizeof(errors) / sizeof(errors[0]); error++) {
                for (content = 0; content < 3; content++) {
                    BC_FW_CMD command, original;
                    TX_INPUT_BUFFER_INFO input;
                    unsigned attempts;

                    InitTransfer(&command, commands[cmd], content != 0);
                    original = command;
                    input = hardware.TxFwInputBuffInfo;
                    response_read_status = errors[error];
                    partial_response = content == 2;
                    /* An error after a valid-looking success header/pointers
                     * must not reach either firmware-status or postprocessing.
                     */
                    Check(execute[generation](&hardware, &command) == BC_STS_IO_ERROR,
                          "every failed reply read reports transport IO_ERROR, not firmware rejection");
                    Check(WordsZero(command.rsp, FW_CMD_BUFF_SZ),
                          "failed reads discard zero, supplied and partially written replies");
                    Check(!memcmp(command.cmd, original.cmd, sizeof(command.cmd)) &&
                          command.flags == original.flags && command.add_data == original.add_data,
                          "reply failure preserves the request and ancillary ABI fields");
                    Check(hardware.channelNum == 0xaabb && hardware.TxBuffInfoAddr == 0xbbcc &&
                          hardware.pib_del_Q_addr == 0xccdd && hardware.pib_rel_Q_addr == 0xddee &&
                          !memcmp(&hardware.TxFwInputBuffInfo, &input, sizeof(input)) &&
                          hardware.PwrDwnTxIntr && hardware.PwrDwnPiQIntr &&
                          hardware.SingleThreadAppFIFOEmpty && hardware.EmptyCnt == 123 &&
                          !firmware_config_calls,
                          "unreadable replies cannot publish channels, queues, power or INIT config state");
                    Check(hardware.fwcmd_poisoned && !hardware.fwcmd_pending &&
                          !hardware.FwCmdCnt && hardware.fwcmd_evt_sts == 1,
                          "completed reply-read failure quarantines without inventing outstanding work");
                    Check(transfer_reads == 2 && transfer_writes == 1 &&
                          mailbox_posts == 1 && mailbox_reads == 1 && sleeps == 1 && !power_wakes,
                          "failure performs only the original single command and reply attempt");
                    CheckUnlocked(&hardware.lock, "reply failure releases hardware serialization");
                    CheckUnlocked(&hardware.fwcmd_mutex.native,
                                  "reply failure releases mailbox serialization");
                    attempts = transfer_reads + transfer_writes + mailbox_reads + mailbox_posts;
                    Check(execute[generation](&hardware, &command) == BC_STS_BUSY &&
                          attempts == transfer_reads + transfer_writes + mailbox_reads + mailbox_posts,
                          "quarantine blocks direct retries before any hardware callback");
                    Check(crystalhd_hw_fw_cmd_enter(&hardware) == BC_STS_BUSY,
                          "quarantine blocks normal transaction preprocessing");
                    crystalhd_hw_fw_cmd_complete(&hardware);
                    Check(hardware.fwcmd_poisoned && !hardware.FwCmdCnt,
                          "a duplicate/late IRQ cannot reopen an unreconciled mailbox");
                    Check(crystalhd_hw_fw_cmd_recovery_enter(&hardware) == BC_STS_SUCCESS,
                          "completed read failure permits only verified-reset recovery");
                    crystalhd_hw_fw_cmd_reset_locked(&hardware);
                    crystalhd_hw_fw_cmd_leave(&hardware);
                    response_read_status = BC_STS_SUCCESS;
                    partial_response = false;
                    transfer_reads = 0;
                    Check(execute[generation](&hardware, &command) == BC_STS_SUCCESS &&
                          !hardware.fwcmd_poisoned && !hardware.FwCmdCnt,
                          "a successful reply is usable after verified-reset state recovery");
                    DestroyHardware();
                }
            }
        }
    }
}

static void ReplyRangeAndFirmwareStatus(void)
{
    BC_STATUS (*const execute[])(struct crystalhd_hw *, BC_FW_CMD *) = {
        crystalhd_flea_do_fw_cmd, crystalhd_link_do_fw_cmd,
    };
    const uint32_t invalid[] = { 3U, CRYSTALHD_DEVICE_DRAM_SIZE,
                               CRYSTALHD_DEVICE_DRAM_SIZE - 252U, UINT32_MAX };
    unsigned generation, range, status;

    for (generation = 0; generation < 2; generation++) {
        for (range = 0; range < sizeof(invalid) / sizeof(invalid[0]); range++) {
            BC_FW_CMD command;

            InitTransfer(&command, eCMD_C011_DEC_CHAN_OPEN, true);
            response_address = invalid[range];
            Check(execute[generation](&hardware, &command) == BC_STS_IO_ERROR &&
                  WordsZero(command.rsp, FW_CMD_BUFF_SZ) && hardware.fwcmd_poisoned,
                  "unaligned, overflowing and out-of-DRAM replies cannot appear successful");
            DestroyHardware();
        }
        for (status = 0; status < 3; status++) {
            BC_FW_CMD command;
            const uint32_t firmware_status[] = { 0U, 1U, UINT32_MAX };
            BC_STATUS expected = status ? BC_STS_FW_CMD_ERR : BC_STS_SUCCESS;

            InitTransfer(&command, eCMD_C011_DEC_CHAN_START_VIDEO, true);
            response_address = CRYSTALHD_DEVICE_DRAM_SIZE - sizeof(command.rsp);
            response_words[2] = firmware_status[status];
            Check(execute[generation](&hardware, &command) == expected,
                  "last valid DRAM reply and firmware status retain existing semantics");
            Check(!memcmp(command.rsp, response_words, sizeof(command.rsp)) &&
                  !hardware.fwcmd_poisoned && !hardware.fwcmd_pending && !hardware.FwCmdCnt,
                  "valid success and rejection preserve the complete reply and clean mailbox");
            Check(hardware.pib_del_Q_addr == (status ? 0xccddU : response_words[5]) &&
                  hardware.pib_rel_Q_addr == (status ? 0xddeeU : response_words[6]),
                  "only a successfully read firmware success reply publishes PIB queues");
            DestroyHardware();
        }
    }
}

static void ReplyAdmissionAndWaitFailure(void)
{
    BC_STATUS (*const execute[])(struct crystalhd_hw *, BC_FW_CMD *) = {
        crystalhd_flea_do_fw_cmd, crystalhd_link_do_fw_cmd,
    };
    const enum wait_mode waits[] = { WAIT_TIMEOUT, WAIT_SIGNAL, WAIT_IO_ERROR };
    const BC_STATUS expected[] = {
        BC_STS_TIMEOUT, BC_STS_IO_USER_ABORT, BC_STS_IO_ERROR,
    };
    unsigned generation, fault;

    for (generation = 0; generation < 2; generation++) {
        for (fault = 0; fault < 6; fault++) {
            BC_FW_CMD command;

            InitTransfer(&command, eCMD_C011_DEC_CHAN_OPEN, true);
            if (fault < 2) {
                hardware.fwcmd_poisoned = !fault;
                hardware.fwcmd_pending = !!fault;
                hardware.FwCmdCnt = 1;
                Check(execute[generation](&hardware, &command) == BC_STS_BUSY,
                      "poisoned and already-pending attempts retain admission result");
                Check(!transfer_reads && !transfer_writes && !mailbox_reads &&
                      !mailbox_posts && !sleeps && !power_wakes,
                      "denied admission never reaches any hardware callback");
                Check(hardware.fwcmd_poisoned == !fault &&
                      hardware.fwcmd_pending == !!fault && hardware.FwCmdCnt == 1,
                      "denied admission preserves the earlier command lifetime");
            } else if (fault == 2) {
                interrupt_mailbox_begin = true;
                Check(execute[generation](&hardware, &command) == BC_STS_IO_USER_ABORT,
                      "interrupted begin preserves its user-abort admission result");
                Check(!transfer_reads && !transfer_writes && !mailbox_reads &&
                      !mailbox_posts && !sleeps && !power_wakes,
                      "interrupted begin never reaches any hardware callback");
                Check(!hardware.fwcmd_poisoned && !hardware.fwcmd_pending &&
                      !hardware.FwCmdCnt,
                      "interrupted begin creates neither a command lifetime nor quarantine");
            } else {
                wait_mode = waits[fault - 3];
                Check(execute[generation](&hardware, &command) == expected[fault - 3],
                      "timeout and interrupted/failed wait preserve their transport status");
                Check(transfer_reads == 1 && transfer_writes == 1 &&
                      !mailbox_reads && mailbox_posts == 1 && sleeps == 1,
                      "wait failure cannot read or postprocess a reply");
                Check(hardware.fwcmd_poisoned && !hardware.fwcmd_pending &&
                      hardware.FwCmdCnt == 1,
                      "wait failure retains the outstanding count until its late completion");
                crystalhd_hw_fw_cmd_complete(&hardware);
                Check(hardware.fwcmd_poisoned && !hardware.FwCmdCnt,
                      "late completion balances wait failure but cannot clear quarantine");
            }
            Check(WordsZero(command.rsp, FW_CMD_BUFF_SZ),
                  "all denied or incomplete attempts invalidate supplied response data");
            Check(hardware.channelNum == 0xaabb && hardware.TxBuffInfoAddr == 0xbbcc &&
                  !firmware_config_calls,
                  "admission/wait failures never publish reply-derived host state");
            CheckUnlocked(&hardware.lock, "admission/wait failure releases the hardware lock");
            CheckUnlocked(&hardware.fwcmd_mutex.native,
                          "admission/wait failure releases mailbox serialization");
            DestroyHardware();
        }
    }
}

static void PreservedReplyTransport(void)
{
    BC_STATUS (*const execute[])(struct crystalhd_hw *, BC_FW_CMD *) = {
        crystalhd_flea_do_fw_cmd, crystalhd_link_do_fw_cmd,
    };
    BC_FW_CMD command;
    unsigned generation;

    for (generation = 0; generation < 2; generation++) {
        InitTransfer(&command, 0xdeadbeefU, true);
        response_words[0] = 0x01020304;
        response_words[1] = 0xfedcba98;
        Check(execute[generation](&hardware, &command) == BC_STS_SUCCESS &&
              !memcmp(command.rsp, response_words, sizeof(command.rsp)),
              "reply-read hardening neither restricts commands nor imposes new echo equality");
        Check(!hardware.fwcmd_poisoned && !hardware.FwCmdCnt,
              "valid opaque replies retain the existing clean transport result");
        DestroyHardware();
    }

    InitTransfer(&command, eCMD_C011_DEC_CHAN_OPEN, true);
    Check(crystalhd_flea_do_fw_cmd(&hardware, &command) == BC_STS_SUCCESS &&
          hardware.channelNum == response_words[3] &&
          hardware.TxBuffInfoAddr == response_words[11] &&
          !hardware.TxFwInputBuffInfo.DramBuffAdd &&
          !hardware.TxFwInputBuffInfo.DramBuffSzInBytes &&
          !hardware.TxFwInputBuffInfo.Flags &&
          !hardware.TxFwInputBuffInfo.HostXferSzInBytes &&
          !hardware.TxFwInputBuffInfo.SeqNum &&
          !hardware.PwrDwnTxIntr && !hardware.PwrDwnPiQIntr &&
          !hardware.SingleThreadAppFIFOEmpty && !hardware.EmptyCnt,
          "a valid Flea OPEN reply still publishes and resets its existing channel state");
    DestroyHardware();

    InitTransfer(&command, eCMD_C011_INIT, true);
    Check(crystalhd_link_do_fw_cmd(&hardware, &command) == BC_STS_SUCCESS &&
          firmware_config_calls == 1,
          "a valid Link INIT reply still performs its existing configuration postprocessing");
    DestroyHardware();
}

static void CommandPublicationFailure(void)
{
    BC_STATUS (*const execute[])(struct crystalhd_hw *, BC_FW_CMD *) = {
        crystalhd_flea_do_fw_cmd, crystalhd_link_do_fw_cmd,
    };
    const BC_STATUS errors[] = {
        BC_STS_INV_ARG, BC_STS_BUSY, BC_STS_ERROR, BC_STS_IO_ERROR,
        BC_STS_FW_CMD_ERR,
    };
    unsigned generation, error, stage, interrupt;

    for (generation = 0; generation < 2; generation++) {
        for (error = 0; error < sizeof(errors) / sizeof(errors[0]); error++) {
            for (stage = 0; stage < 3; stage++) {
                for (interrupt = 0; interrupt < 2; interrupt++) {
                    BC_FW_CMD command, original;
                    TX_INPUT_BUFFER_INFO input;
                    unsigned events;

                    InitTransfer(&command, eCMD_C011_DEC_CHAN_OPEN, true);
                    command.cmd[FW_CMD_BUFF_SZ - 1] = 0x33445566U;
                    original = command;
                    input = hardware.TxFwInputBuffInfo;
                    if (stage < 2) {
                        command_write_status = errors[error];
                        partial_command = stage == 1;
                    } else {
                        flush_read_status = errors[error];
                    }
                    complete_before_publication = !!interrupt;
                    Check(execute[generation](&hardware, &command) == BC_STS_IO_ERROR,
                          "write/partial-write/flush failure reports transport IO_ERROR");
                    Check(transfer_writes == 1 && transfer_reads == (stage == 2 ? 1U : 0U) &&
                          !mailbox_posts && !mailbox_reads && !sleeps && !wait_calls,
                          "failed command publication stops before flush, mailbox, wait or reply as applicable");
                    Check(WordsZero(command.rsp, FW_CMD_BUFF_SZ) &&
                          !memcmp(command.cmd, original.cmd, sizeof(command.cmd)) &&
                          command.flags == original.flags && command.add_data == original.add_data,
                          "unposted failure invalidates reply without changing command ABI fields");
                    Check(hardware.fwcmd_poisoned && !hardware.fwcmd_pending &&
                          !hardware.FwCmdCnt && !hardware.fwcmd_evt_sts,
                          "unposted abort retires exactly once and clears a spurious completion");
                    Check(hardware.fwcmd_event.wakeups == interrupt &&
                          !complete_before_publication,
                          "abort itself neither wakes an event nor invokes completion");
                    Check(hardware.channelNum == 0xaabb && hardware.TxBuffInfoAddr == 0xbbcc &&
                          !memcmp(&hardware.TxFwInputBuffInfo, &input, sizeof(input)) &&
                          hardware.PwrDwnTxIntr && hardware.PwrDwnPiQIntr &&
                          hardware.SingleThreadAppFIFOEmpty && hardware.EmptyCnt == 123 &&
                          !firmware_config_calls,
                          "unposted failure cannot publish firmware reply or channel state");
                    if (stage == 1)
                        Check(written_command[0] == command.cmd[0] &&
                              WordsZero(written_command + 8, FW_CMD_BUFF_SZ - 8) &&
                              written_command[FW_CMD_BUFF_SZ - 1] !=
                                  command.cmd[FW_CMD_BUFF_SZ - 1],
                              "partial DRAM write is reproduced without posting its incomplete command");
                    CheckUnlocked(&hardware.lock, "unposted failure releases the hardware lock");
                    CheckUnlocked(&hardware.fwcmd_mutex.native,
                                  "unposted failure releases mailbox serialization");
                    Check(execute[generation](&hardware, &command) == BC_STS_BUSY &&
                          transfer_writes == 1 && !mailbox_posts && !wait_calls,
                          "poison blocks a direct retry before any command write or mailbox post");
                    Check(crystalhd_hw_fw_cmd_enter(&hardware) == BC_STS_BUSY,
                          "poison blocks transaction preprocessing after unposted failure");
                    events = hardware.fwcmd_event.wakeups;
                    crystalhd_hw_fw_cmd_complete(&hardware);
                    Check(hardware.fwcmd_poisoned && !hardware.fwcmd_pending &&
                          !hardware.FwCmdCnt && !hardware.fwcmd_evt_sts &&
                          hardware.fwcmd_event.wakeups == events,
                          "late IRQ after abort cannot double-retire, signal success or release quarantine");
                    Check(crystalhd_hw_fw_cmd_recovery_enter(&hardware) == BC_STS_SUCCESS,
                          "no pending unposted work remains to block verified recovery");
                    crystalhd_hw_fw_cmd_reset_locked(&hardware);
                    crystalhd_hw_fw_cmd_leave(&hardware);
                    command_write_status = flush_read_status = BC_STS_SUCCESS;
                    partial_command = false;
                    transfer_reads = 0;
                    Check(execute[generation](&hardware, &command) == BC_STS_SUCCESS &&
                          mailbox_posts == 1 && wait_calls == 1 && !hardware.fwcmd_poisoned &&
                          !hardware.fwcmd_pending && !hardware.FwCmdCnt,
                          "valid publication resumes only after verified-reset state recovery");
                    DestroyHardware();
                }
            }
        }
    }
}

static void UnpostedCountState(void)
{
    unsigned pending, count;

    for (pending = 0; pending < 2; pending++) {
        for (count = 0; count < 3; count++) {
            const uint32_t original[] = { 0U, 1U, 7U };
            uint32_t expected = pending && original[count] ? original[count] - 1 : original[count];

            InitHardware();
            hardware.fwcmd_pending = !!pending;
            hardware.FwCmdCnt = original[count];
            hardware.fwcmd_evt_sts = 1;
            if (TestMutexLock(&hardware.fwcmd_mutex)) abort();
            if (pthread_mutex_lock(&hardware.lock)) abort();
            crystalhd_hw_fw_cmd_abort_unposted_locked(&hardware);
            Check(hardware.FwCmdCnt == expected && !hardware.fwcmd_pending &&
                  hardware.fwcmd_poisoned && !hardware.fwcmd_evt_sts,
                  "abort balances only a present current pending count, without underflow or resetting others");
            Check(!hardware.fwcmd_event.wakeups && !last_woken,
                  "count-state abort performs no completion or wake callback");
            if (pthread_mutex_unlock(&hardware.lock)) abort();
            TestMutexUnlock(&hardware.fwcmd_mutex);
            DestroyHardware();
        }
    }
}

int main(void)
{
    AdmissionBeforePreprocess();
    UnavailableAdmission();
    NormalAndLateCompletion();
    TimeoutLateIrqAndRetry();
    TimeoutBoundaryAndReset();
    SerializedCommands();
    ReplyReadFailure();
    ReplyRangeAndFirmwareStatus();
    ReplyAdmissionAndWaitFailure();
    PreservedReplyTransport();
    CommandPublicationFailure();
    UnpostedCountState();
    printf("Firmware command recovery: %u checks, %u failures (no hardware)\n",
           checks, failures);
    return failures ? 1 : 0;
}
