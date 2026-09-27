/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Exact command/hardware PM and notify-mode functions; no device is opened.
 * Isolate failures so the original idle error and NULL dereferences are safe.
 */
#include <stdbool.h>
#include <stdint.h>
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
#define READ_ONCE(value) (value)
#define printk(...) ((void)0)
#define dev_err(dev, ...) ((void)(dev))
#define dev_info(dev, ...) ((void)(dev))
#define dev_dbg(dev, ...) ((void)(dev))
struct device { int unused; };
struct pci_dev { struct device dev; };
struct crystalhd_adp { struct pci_dev *pdev; };
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
    struct { union { struct { uint32_t Mode; } NotifyMode; } u; } udata;
    void *add_cdata;
    uint32_t add_cdata_sz;
} crystalhd_ioctl_data;

static unsigned checks, failures, starts, stops, tx_stops, cancels, captures, pools, rings;
static unsigned downloads;
static bool start_ok, stop_ok;
static BC_STATUS capture_status, cancel_status;
static char events[32];
static struct pci_dev endpoint;
static struct crystalhd_adp adapter = { &endpoint };
static struct crystalhd_hw hardware;
static struct crystalhd_cmd context;
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
static struct device *chddev(void) { return &endpoint.dev; }
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
static BC_STATUS crystalhd_hw_stop_capture(struct crystalhd_hw *hw, bool unmap)
{
    Check(hw == &hardware && !unmap, "suspend retains registered capture buffers");
    captures++; Event('C'); return capture_status;
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
    pools++; return 0;
}
static int crystalhd_create_dio_pool(struct crystalhd_adp *adp, unsigned size)
{
    Check(adp == &adapter && size == BC_LINK_MAX_SGLS, "notify allocates DMA pool");
    pools++; return 0;
}
static BC_STATUS crystalhd_hw_setup_dma_rings(struct crystalhd_hw *hw)
{
    Check(hw == &hardware, "notify uses the initialized hardware context");
    rings++; return BC_STS_SUCCESS;
}
#include "command-pm-hardware.h"
#include "command-pm-functions.h"

static void Reset(uint32_t state, bool with_hardware)
{
    unsigned n;
    starts = stops = tx_stops = cancels = captures = pools = rings = 0;
    downloads = 0;
    events[0] = '\0'; start_ok = stop_ok = true;
    capture_status = cancel_status = BC_STS_SUCCESS;
    hardware = (struct crystalhd_hw){ .adp = &adapter, .pfnStartDevice = Start,
        .pfnStopDevice = Stop, .pfnStopTxDMA = StopTx, .pfnFWDwnld = Download,
        .FwCmdCnt = 64,
        .rx_list_sts = {rx_sts_waiting, rx_sts_waiting},
        .TxList0Sts = TxListWaitingForIntr, .TxList1Sts = TxListWaitingForIntr,
        .rx_list_post_index = 1, .tx_list_post_index = 1 };
    context = (struct crystalhd_cmd){ .state = state, .adp = &adapter,
        .hw_ctx = with_hardware ? &hardware : NULL, .cin_wait_exit = 1 };
    for (n = 0; n < BC_LINK_MAX_OPENS; n++) {
        context.user[n].uid = n;
        context.user[n].mode = DTS_MODE_INV;
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
    Check(pools == 2 && rings == 1, "notify-mode reaches real allocation/ring call sequence");
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
