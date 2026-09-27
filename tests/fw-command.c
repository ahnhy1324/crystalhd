/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Exact firmware-command state helpers; no device is opened. */
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
struct _BC_DTS_PROC_OUT;
#include "bc_dts_defs.h"

struct mutex { pthread_mutex_t native; };
typedef pthread_mutex_t spinlock_t;
typedef struct {
    pthread_mutex_t lock;
    pthread_cond_t changed;
    unsigned wakeups;
} wait_queue_head_t;

struct crystalhd_hw {
    spinlock_t lock;
    struct mutex fwcmd_trans_mutex;
    struct mutex fwcmd_mutex;
    wait_queue_head_t fwcmd_event;
    bool fwcmd_pending;
    bool fwcmd_poisoned;
    int fwcmd_evt_sts;
    uint32_t FwCmdCnt;
};

enum wait_mode {
    WAIT_BLOCK,
    WAIT_TIMEOUT,
    WAIT_COMPLETE_AT_TIMEOUT,
};

static struct crystalhd_hw hardware;
static enum wait_mode wait_mode;
static wait_queue_head_t *last_woken;
static pthread_mutex_t audit_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t audit_changed = PTHREAD_COND_INITIALIZER;
static unsigned mutex_attempts;

static void crystalhd_hw_fw_cmd_complete(struct crystalhd_hw *hw);

static int TestMutexLock(struct mutex *mutex)
{
    pthread_mutex_lock(&audit_lock);
    mutex_attempts++;
    pthread_cond_broadcast(&audit_changed);
    pthread_mutex_unlock(&audit_lock);
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
    if (wait_mode == WAIT_TIMEOUT)
        return -EBUSY;
    if (wait_mode == WAIT_COMPLETE_AT_TIMEOUT) {
        crystalhd_hw_fw_cmd_complete(&hardware);
        return -EBUSY;
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
    if (pthread_mutex_lock(lock)) abort(); \
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

#include "fw-command-functions.h"

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
    if (pthread_mutex_init(&hardware.lock, NULL) ||
        pthread_mutex_init(&hardware.fwcmd_trans_mutex.native, NULL) ||
        pthread_mutex_init(&hardware.fwcmd_mutex.native, NULL) ||
        pthread_mutex_init(&hardware.fwcmd_event.lock, NULL) ||
        pthread_cond_init(&hardware.fwcmd_event.changed, NULL))
        abort();
    last_woken = NULL;
    wait_mode = WAIT_BLOCK;
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

int main(void)
{
    AdmissionBeforePreprocess();
    NormalAndLateCompletion();
    TimeoutLateIrqAndRetry();
    TimeoutBoundaryAndReset();
    SerializedCommands();
    printf("Firmware command recovery: %u checks, %u failures (no hardware)\n",
           checks, failures);
    return failures ? 1 : 0;
}
