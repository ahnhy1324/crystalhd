// SPDX-License-Identifier: LGPL-2.1-or-later
#ifndef CRYSTALHD_FLEA_POWER_OBSERVER_H
#define CRYSTALHD_FLEA_POWER_OBSERVER_H

#include "flea/70015/magnum/basemodules/chp/70015/rdb/a0/bchp_clk.h"
#include "flea/70015/magnum/basemodules/chp/70015/rdb/a0/bchp_ddr23_ctl_regs_0.h"
#include "flea/70015/magnum/basemodules/chp/70015/rdb/a0/bchp_ddr23_phy_control_regs.h"
#include "flea/70015/magnum/basemodules/chp/70015/rdb/a0/bchp_ddr23_phy_byte_lane_0.h"
#include "flea/70015/magnum/basemodules/chp/70015/rdb/a0/bchp_ddr23_phy_byte_lane_1.h"
#include "flea/70015/magnum/basemodules/chp/70015/rdb/a0/bchp_pri_arb_control_regs.h"
#include "flea/70015/magnum/basemodules/chp/70015/rdb/a0/bchp_misc1.h"
#include "flea/70015/magnum/basemodules/chp/70015/rdb/a0/bchp_intr.h"

// Fixed target-register reads during ordinary decode. The legacy accessor may
// write its indirect address portal; no target control register is changed.
// These observations do not disable the arbiter, request self-refresh or enable
// runtime power management, and successful reads do not certify fetch errors.
// The separate host-only exercise makes no observer raw accesses and records
// only ordinary lifecycle stages and stock firmware PAUSE ON/OFF receipts.
struct FleaPowerObserver {
    using Reader = BC_STATUS (*)(HANDLE, uint32_t, uint32_t *);
    using Command = BC_STATUS (*)(HANDLE);
    using FirmwareCommand = BC_STATUS (*)(HANDLE, uint32_t);
    FirmwareCommand firmware_command = nullptr;
    bool host_only = false;
    unsigned host_command_attempts = 0;
    bool configuration = false;
    bool firmware_dma = false;
    bool enabled = false, failed = false;
    unsigned next = 0, reads = 0;
    static uint32_t Address(unsigned field) {
        const uint32_t addresses[] = {BCHP_CLK_PM_CTRL,
            BCHP_PRI_ARB_CONTROL_REGS_MASTER_CTL,
            BCHP_DDR23_CTL_REGS_0_CTL_STATUS, BCHP_CLK_PM_CTRL};
        return addresses[field];
    }
    static bool Valid(unsigned field, uint32_t raw) {
        const uint32_t clocks = BCHP_CLK_PM_CTRL_DIS_ARM_CLK_MASK |
            BCHP_CLK_PM_CTRL_DIS_AVD_CLK_MASK |
            BCHP_CLK_PM_CTRL_DIS_BLINK_108_CLK_MASK |
            BCHP_CLK_PM_CTRL_DIS_BLINK_216_CLK_MASK |
            BCHP_CLK_PM_CTRL_DIS_AVD_108_CLK_MASK |
            BCHP_CLK_PM_CTRL_DIS_AVD_216_CLK_MASK |
            BCHP_CLK_PM_CTRL_DIS_DDR_108_CLK_MASK |
            BCHP_CLK_PM_CTRL_DIS_DDR_216_CLK_MASK;
        if (field == 0 || field == 3) return raw != UINT32_MAX && !(raw & clocks);
        if (field == 1) return raw == BCHP_PRI_ARB_CONTROL_REGS_MASTER_CTL_arb_disable_Enable;
        return field == 2 && !(raw &
            (BCHP_DDR23_CTL_REGS_0_CTL_STATUS_reserved0_MASK |
             BCHP_DDR23_CTL_REGS_0_CTL_STATUS_reserved1_MASK));
    }
    static uint32_t DmaAddress(unsigned field) {
        const uint32_t addresses[] = {BCHP_CLK_PM_CTRL,
            BCHP_PRI_ARB_CONTROL_REGS_MASTER_CTL,
            BCHP_MISC1_TX_FIRST_DESC_L_ADDR_LIST0,
            BCHP_MISC1_TX_FIRST_DESC_L_ADDR_LIST1,
            BCHP_INTR_INTR_STATUS, BCHP_DDR23_CTL_REGS_0_CTL_STATUS,
            BCHP_CLK_PM_CTRL};
        return addresses[field];
    }
    static bool DmaValid(unsigned field, uint32_t raw) {
        if (field < 2) return Valid(field, raw);
        if (field == 5) return Valid(2, raw);
        if (field == 6) return Valid(0, raw);
        if (field == 2)
            return !(raw & BCHP_MISC1_TX_FIRST_DESC_L_ADDR_LIST0_reserved0_MASK);
        if (field == 3)
            return !(raw & BCHP_MISC1_TX_FIRST_DESC_L_ADDR_LIST1_reserved0_MASK);
        const uint32_t reserved = BCHP_INTR_INTR_STATUS_reserved0_MASK |
            BCHP_INTR_INTR_STATUS_reserved1_MASK | BCHP_INTR_INTR_STATUS_reserved2_MASK |
            BCHP_INTR_INTR_STATUS_reserved3_MASK;
        const uint32_t errors = BCHP_INTR_INTR_STATUS_HAT_INTR_MASK |
            BCHP_INTR_INTR_STATUS_PCIE_TGT_CA_ATTN_MASK |
            BCHP_INTR_INTR_STATUS_PCIE_TGT_UR_ATTN_MASK |
            BCHP_INTR_INTR_STATUS_L1_HIF_RX_DMA_ERR_INTR_MASK |
            BCHP_INTR_INTR_STATUS_L1_Y_RX_DMA_ERR_INTR_MASK |
            BCHP_INTR_INTR_STATUS_L1_TX_DMA_ERR_INTR_MASK |
            BCHP_INTR_INTR_STATUS_L0_HIF_RX_DMA_ERR_INTR_MASK |
            BCHP_INTR_INTR_STATUS_L0_Y_RX_DMA_ERR_INTR_MASK |
            BCHP_INTR_INTR_STATUS_L0_TX_DMA_ERR_INTR_MASK;
        return field == 4 && !(raw & (reserved | errors));
    }
    bool Observe(HANDLE handle, unsigned stage, Reader reader = DtsDevRegisterRead,
                 bool report = true) {
        if (!enabled) return true;
        if (host_only) return HostStage(handle, stage, report);
        if (firmware_dma && (!firmware_command || configuration)) {
            failed = true;
            return false;
        }
        if (configuration) {
            if (stage == 0) return ObserveConfiguration(handle, reader, report);
            if (!failed && next == 1 && stage < 6) return true;
            failed = true;
            return false;
        }
        if (failed || !handle || !reader || stage != next || stage >= 6) {
            failed = true;
            return false;
        }
        const char *const names[] = {"pre-START", "first-released", "pause-API-return",
            "resume-API-return", "last-released", "EOS"};
        const char *name = firmware_command && (stage == 2 || stage == 3)
            ? (stage == 2 ? "firmware-pause-return" : "firmware-resume-return") : names[stage];
        // Only the post-PAUSE bracket adds DMA status reads. No polling or
        // descriptor/IRQ acknowledgement writes; these words are non-atomic.
        const bool dma = firmware_dma && stage == 2;
        const unsigned fields = dma ? 7U : 4U;
        uint32_t raw[7] = {};
        unsigned measured = 0;
        BC_STATUS status = BC_STS_SUCCESS;
        const gint64 before = g_get_monotonic_time();
        for (unsigned i = 0; i < fields; ++i) {
            ++reads;
            status = reader(handle, dma ? DmaAddress(i) : Address(i), &raw[i]);
            if (status != BC_STS_SUCCESS) { failed = true; break; }
            ++measured;
            if (!(dma ? DmaValid(i, raw[i]) : Valid(i, raw[i]))) { failed = true; break; }
        }
        if (measured == fields && raw[0] != raw[fields - 1]) failed = true;
        if (!failed) ++next;
        if (report) {
            std::printf("Power observation: stage=%s monotonic-us=%lld duration-us=%lld "
                "reads=%u measured=%u api-status=%d result=%s", name,
                static_cast<long long>(before),
                static_cast<long long>(g_get_monotonic_time() - before),
                reads, measured, status, failed ? "FAIL" : "PASS");
            for (unsigned i = 0; i < measured; ++i)
                std::printf(" reg%u@%08x=%08x", i, dma ? DmaAddress(i) : Address(i), raw[i]);
            if (dma) {
                // Raw descriptors may contain host DMA addresses: retain this
                // diagnostic log locally; publish VALID bits, not the words.
                if (measured > 2)
                    std::printf(" tx0-valid=%u", !!(raw[2] &
                        BCHP_MISC1_TX_FIRST_DESC_L_ADDR_LIST0_TX_DESC_LIST0_VALID_MASK));
                if (measured > 3)
                    std::printf(" tx1-valid=%u", !!(raw[3] &
                        BCHP_MISC1_TX_FIRST_DESC_L_ADDR_LIST1_TX_DESC_LIST1_VALID_MASK));
                std::printf(" clock-bracket=%s physical-quiescence=unproven",
                    measured < fields ? "incomplete" : raw[0] == raw[fields - 1]
                        ? "consistent" : "inconsistent");
            }
            std::printf(" atomic=no power-gating-tested=no\n");
            std::fflush(stdout);
        }
        return !failed;
    }
    bool HostModeValid() const {
        return firmware_command && !configuration && !firmware_dma;
    }
    void ReportHostStage(unsigned stage, BC_STATUS status, gint64 before, bool report) const {
        if (!report) return;
        const char *const names[] = {"pre-START", "first-released", "firmware-pause-return",
            "firmware-resume-return", "last-released", "EOS"};
        std::printf("Power host exercise: stage=%s monotonic-us=%lld duration-us=%lld "
            "api-status=%d result=%s reads=0 observer-target-control-writes=0 "
            "physical-quiescence=unproven power-gating=unproven\n", names[stage],
            static_cast<long long>(before),
            static_cast<long long>(g_get_monotonic_time() - before), status,
            status == BC_STS_SUCCESS ? "PASS" : "FAIL");
        std::fflush(stdout);
    }
    bool HostStage(HANDLE handle, unsigned stage, bool report) {
        if (failed || !HostModeValid() || !handle || stage != next || stage >= 6 ||
            stage == 2 || stage == 3 || (stage >= 4 && host_command_attempts != 2)) {
            failed = true;
            return false;
        }
        ++next;
        ReportHostStage(stage, BC_STS_SUCCESS, g_get_monotonic_time(), report);
        return true;
    }
    static uint32_t ConfigurationAddress(unsigned field) {
        const uint32_t addresses[] = {BCHP_CLK_PM_CTRL,
            BCHP_DDR23_PHY_CONTROL_REGS_CLK_PM_CTRL,
            BCHP_DDR23_PHY_CONTROL_REGS_PLL_CONFIG,
            BCHP_DDR23_PHY_CONTROL_REGS_CLOCK_REG_CONTROL,
            BCHP_DDR23_PHY_BYTE_LANE_0_CLOCK_REG_CONTROL,
            BCHP_DDR23_PHY_BYTE_LANE_1_CLOCK_REG_CONTROL,
            BCHP_DDR23_PHY_CONTROL_REGS_IDLE_PAD_CONTROL,
            BCHP_DDR23_PHY_BYTE_LANE_0_IDLE_PAD_CONTROL,
            BCHP_DDR23_PHY_BYTE_LANE_1_IDLE_PAD_CONTROL,
            BCHP_DDR23_CTL_REGS_0_PARAMS2,
            BCHP_PRI_ARB_CONTROL_REGS_REFRESH_CTL_0,
            BCHP_PRI_ARB_CONTROL_REGS_MASTER_CTL,
            BCHP_DDR23_PHY_CONTROL_REGS_PLL_STATUS,
            BCHP_DDR23_CTL_REGS_0_CTL_STATUS, BCHP_CLK_PM_CTRL};
        return addresses[field];
    }
    bool ObserveConfiguration(HANDLE handle, Reader reader, bool report) {
        if (!enabled || !configuration) return true;
        if (failed || next || !handle || !reader) { failed = true; return false; }
        uint32_t raw[15] = {};
        unsigned measured = 0;
        BC_STATUS status = BC_STS_SUCCESS;
        const gint64 before = g_get_monotonic_time();
        for (unsigned i = 0; i < 15; ++i) {
            ++reads;
            status = reader(handle, ConfigurationAddress(i), &raw[i]);
            if (status != BC_STS_SUCCESS) { failed = true; break; }
            ++measured;
            // Avoid later DDR reads if the observed clocks/pads are disabled.
            const bool valid = raw[i] != UINT32_MAX &&
                ((i != 0 && i != 14) || Valid(0, raw[i])) &&
                (!(i >= 1 && i <= 5) || !(raw[i] & 1U)) &&
                (!(i >= 6 && i <= 8) || !(raw[i] & 0x80000000U)) &&
                (i != 11 || Valid(1, raw[i])) &&
                (i != 12 || !(raw[i] & BCHP_DDR23_PHY_CONTROL_REGS_PLL_STATUS_reserved0_MASK)) &&
                (i != 13 || Valid(2, raw[i]));
            if (!valid) { failed = true; break; }
        }
        if (measured == 15 && raw[0] != raw[14]) failed = true;
        if (!failed) ++next;
        if (report) {
            std::printf("Power configuration: stage=pre-START monotonic-us=%lld duration-us=%lld "
                "reads=%u measured=%u api-status=%d result=%s",
                static_cast<long long>(before),
                static_cast<long long>(g_get_monotonic_time() - before),
                reads, measured, status, failed ? "FAIL" : "PASS");
            for (unsigned i = 0; i < measured; ++i)
                std::printf(" reg%u@%08x=%08x", i, ConfigurationAddress(i), raw[i]);
            std::printf(" atomic=no power-gating-tested=no\n");
            std::fflush(stdout);
        }
        return !failed;
    }
    bool PauseAndResume(HANDLE handle, Reader reader = DtsDevRegisterRead,
                        Command pause = DtsPauseDecoder, Command resume = DtsResumeDecoder,
                        bool report = true) {
        if (!enabled) return true;
        if (host_only) return HostPauseAndResume(handle, report);
        if (firmware_dma && (!firmware_command || configuration)) {
            failed = true;
            return false;
        }
        if (configuration) {
            if (!failed && next == 1) return true;
            failed = true;
            return false;
        }
        if (firmware_command) return FirmwarePauseAndResume(handle, reader, report);
        if (failed || next != 2 || !handle || !reader || !pause || !resume) {
            failed = true;
            return false;
        }
        // On BCM70015 these public APIs change library state only; they do
        // not send the firmware PAUSE command or establish decoder idle.
        const BC_STATUS paused = pause(handle);
        if (paused != BC_STS_SUCCESS) {
            if (report)
                std::printf("Power observation commands: pause=%d resume=NOT-RUN\n", paused);
            failed = true;
            return false;
        }
        const bool observed = Observe(handle, 2, reader, report);
        // Restore the ordinary decoder state even if the observation failed.
        const BC_STATUS resumed = resume(handle);
        if (report)
            std::printf("Power observation commands: pause=%d resume=%d firmware-pause=no\n",
                        paused, resumed);
        if (!observed || resumed != BC_STS_SUCCESS) { failed = true; return false; }
        return Observe(handle, 3, reader, report);
    }
    bool HostPauseAndResume(HANDLE handle, bool report) {
        if (failed || !HostModeValid() || next != 2 || !handle || host_command_attempts) {
            failed = true;
            return false;
        }
        // Host-only receipt: no raw register or memory access, including after
        // a command error. OFF is attempted once even if ON may have committed.
        const gint64 before = g_get_monotonic_time();
        ++host_command_attempts;
        const BC_STATUS paused = firmware_command(handle, 1);
        failed = failed || paused != BC_STS_SUCCESS;
        ++next;
        ReportHostStage(2, paused, before, report);
        const gint64 resume_before = g_get_monotonic_time();
        ++host_command_attempts;
        const BC_STATUS resumed = firmware_command(handle, 0);
        failed = failed || resumed != BC_STS_SUCCESS;
        ++next;
        ReportHostStage(3, resumed, resume_before, report);
        // Timings include the host mailbox wait. Successful API returns alone
        // do not prove physical quiescence or an actual clock-gating cycle.
        return !failed;
    }
    bool FirmwarePauseAndResume(HANDLE handle, Reader reader, bool report) {
        if (failed || next != 2 || !handle || !reader || !firmware_command) {
            failed = true;
            return false;
        }
        // Internal stock PAUSE ON/OFF, not the public Flea state-only API.
        // An error may follow a committed command: attempt OFF once even then.
        // Never bypass a poisoned mailbox or retry a refused cleanup command.
        const gint64 before = g_get_monotonic_time();
        const BC_STATUS paused = firmware_command(handle, 1);
        const gint64 pause_us = g_get_monotonic_time() - before;
        const bool observed = paused == BC_STS_SUCCESS && Observe(handle, 2, reader, report);
        const gint64 resume_before = g_get_monotonic_time();
        const BC_STATUS resumed = firmware_command(handle, 0);
        if (report)
            std::printf("Power observation firmware commands: pause=%d resume=%d "
                "pause-host-us=%lld resume-host-us=%lld whole-device-idle=unproven\n",
                paused, resumed, static_cast<long long>(pause_us),
                static_cast<long long>(g_get_monotonic_time() - resume_before));
        // Host timings include the driver's mailbox wait; they are not isolated
        // firmware latency. Neither successful status nor MEMC idle is a lease.
        if (!observed || resumed != BC_STS_SUCCESS) { failed = true; return false; }
        return Observe(handle, 3, reader, report);
    }
    bool Finish(bool native_ok) const {
        if (enabled && host_only)
            return native_ok && !failed && HostModeValid() && next == 6 &&
                reads == 0 && host_command_attempts == 2;
        return native_ok && (!enabled || (!failed &&
            !(firmware_dma && (!firmware_command || configuration)) &&
            next == (configuration ? 1U : 6U) &&
            reads == (configuration ? 15U : firmware_dma ? 27U : 24U)));
    }
};

template<class Check> static void FleaPowerObserverSelfTest(const Check &check)
{
    struct Fixture {
        unsigned calls = 0, pauses = 0, resumes = 0;
        bool addresses_valid = true;
        BC_STATUS status[24] = {};
        uint32_t raw[24] = {};
        BC_STATUS pause_status = BC_STS_SUCCESS, resume_status = BC_STS_SUCCESS;
        Fixture() {
            for (unsigned i = 2; i < 24; i += 4) raw[i] = 0x72;
        }
        static BC_STATUS Read(HANDLE handle, uint32_t address, uint32_t *value) {
            auto *f = static_cast<Fixture *>(handle);
            const uint32_t addresses[] = {0x00070004, 0x0040cb30, 0x01800004, 0x00070004};
            const unsigned i = f->calls++;
            if (i >= 24) { f->addresses_valid = false; return BC_STS_ERROR; }
            f->addresses_valid &= address == addresses[i % 4];
            *value = f->raw[i];
            return f->status[i];
        }
        static BC_STATUS Pause(HANDLE handle) {
            auto *f = static_cast<Fixture *>(handle); ++f->pauses; return f->pause_status;
        }
        static BC_STATUS Resume(HANDLE handle) {
            auto *f = static_cast<Fixture *>(handle); ++f->resumes; return f->resume_status;
        }
        static BC_STATUS Firmware(HANDLE handle, uint32_t operation) {
            auto *f = static_cast<Fixture *>(handle);
            if ((operation == 1 && !f->pauses && !f->resumes) ||
                (operation == 0 && f->pauses == 1 && !f->resumes))
                return operation ? Pause(handle) : Resume(handle);
            f->addresses_valid = false;
            return BC_STS_ERROR;
        }
    };
    for (unsigned bit = 0; bit < 32; ++bit) {
        const uint32_t raw = UINT32_C(1) << bit;
        // Independent literals pin the named RDB fields used by the observer.
        check(FleaPowerObserver::Valid(0, raw) == !(raw & 0x00061c1cU) &&
              FleaPowerObserver::Valid(3, raw) == !(raw & 0x00061c1cU),
              "power observation clock mask includes ARM, AVD, BLINK and DDR clocks only");
        check(!FleaPowerObserver::Valid(1, raw),
              "power observation requires the exact enabled-arbiter value");
        check(FleaPowerObserver::Valid(2, raw) == !!(raw & 0x73U),
              "power observation accepts only the documented MEMC status fields");
    }
    for (unsigned field = 0; field < 4; ++field)
        check(!FleaPowerObserver::Valid(field, UINT32_MAX),
              "power observation rejects all-ones raw values at every field");
    for (unsigned status = 0; status < 4; ++status) {
        Fixture f;
        for (unsigned i = 2; i < 24; i += 4) f.raw[i] = 0x70 | status;
        FleaPowerObserver o; o.enabled = true;
        check(o.Observe(&f, 0, Fixture::Read, false) &&
              o.Observe(&f, 1, Fixture::Read, false) &&
              o.PauseAndResume(&f, Fixture::Read, Fixture::Pause, Fixture::Resume, false) &&
              o.Observe(&f, 4, Fixture::Read, false) && o.Observe(&f, 5, Fixture::Read, false) &&
              o.Finish(true) && !o.Finish(false) && f.addresses_valid &&
              f.calls == 24 && o.reads == 24 && f.pauses == 1 && f.resumes == 1,
              "power observation collects six ordered snapshots without interpreting idle or CKE as quiescence");
    }
    for (unsigned bad = 0; bad < 24; ++bad) {
        for (unsigned api = 0; api < 2; ++api) {
            Fixture f;
            if (api) f.status[bad] = BC_STS_IO_ERROR;
            else f.raw[bad] = UINT32_MAX;
            FleaPowerObserver o; o.enabled = true;
            for (unsigned stage = 0; stage <= bad / 4; ++stage)
                check(o.Observe(&f, stage, Fixture::Read, false) == (stage < bad / 4),
                      "power observation stops at the first rejected read in every stage");
            check(o.failed && o.next == bad / 4 && o.reads == bad + 1 &&
                  f.calls == bad + 1 && f.addresses_valid && !o.Finish(true) &&
                  !o.Observe(&f, o.next, Fixture::Read, false) && f.calls == bad + 1,
                  "read failure or invalid data preserves only the attempted prefix and prevents retries");
        }
    }
    for (unsigned stage = 0; stage < 6; ++stage) {
        Fixture f; FleaPowerObserver o; o.enabled = true;
        f.raw[stage * 4 + 3] = BCHP_CLK_PM_CTRL_DIS_MISC_OTP_9_CLK_MASK;
        for (unsigned n = 0; n <= stage; ++n)
            check(o.Observe(&f, n, Fixture::Read, false) == (n < stage),
                  "unequal valid clock brackets are rejected without an atomicity claim");
        check(o.failed && o.next == stage && o.reads == (stage + 1) * 4 && !o.Finish(true),
              "clock bracket mismatch cannot count as a completed snapshot");
    }
    for (unsigned bit : {2U, 3U, 4U, 10U, 11U, 12U, 17U, 18U}) {
        Fixture f; f.raw[0] = UINT32_C(1) << bit;
        FleaPowerObserver o; o.enabled = true;
        check(!o.Observe(&f, 0, Fixture::Read, false) && f.calls == 1 && o.reads == 1,
              "a disabled relevant clock prevents the later arbiter and MEMC accesses");
    }
    for (unsigned bad = 0; bad < 7; ++bad) {
        Fixture f; FleaPowerObserver o; o.enabled = true;
        if (bad == 0) {
            check(o.Observe(&f, 0, Fixture::Read, false), "power duplicate test first stage");
            check(!o.Observe(&f, 0, Fixture::Read, false) && f.calls == 4,
                  "duplicate power stage adds no register access");
        } else {
            check(!o.Observe(&f, bad, Fixture::Read, false) && !f.calls,
                  "out-of-order or out-of-range power stage adds no register access");
        }
        check(o.failed && !o.Finish(true), "invalid power stage makes completion fail closed");
    }
    for (unsigned invalid = 0; invalid < 2; ++invalid) {
        Fixture f; FleaPowerObserver o; o.enabled = true;
        check(!o.Observe(invalid ? &f : nullptr, 0, invalid ? nullptr : Fixture::Read, false) &&
              !f.calls && !o.reads && o.failed,
              "missing power read arguments fail without dereference or register access");
    }
    for (unsigned next : {0U, 1U, 3U, 4U, 5U, 6U}) {
        Fixture f; FleaPowerObserver o; o.enabled = true; o.next = next;
        check(!o.PauseAndResume(&f, Fixture::Read, Fixture::Pause, Fixture::Resume, false) &&
              !f.calls && !f.pauses && !f.resumes && o.failed,
              "pause/resume observation is admitted only after the first released picture");
    }
    for (unsigned invalid = 0; invalid < 5; ++invalid) {
        Fixture f; FleaPowerObserver o; o.enabled = true; o.next = 2;
        if (invalid == 4) o.failed = true;
        check(!o.PauseAndResume(invalid == 0 ? nullptr : &f,
                  invalid == 1 ? nullptr : Fixture::Read,
                  invalid == 2 ? nullptr : Fixture::Pause,
                  invalid == 3 ? nullptr : Fixture::Resume, false) &&
              !f.calls && !f.pauses && !f.resumes && o.failed,
              "invalid power command arguments fail before pause or register access");
    }
    for (unsigned fault = 0; fault < 10; ++fault) {
        Fixture f; FleaPowerObserver o; o.enabled = true;
        check(o.Observe(&f, 0, Fixture::Read, false) && o.Observe(&f, 1, Fixture::Read, false),
              "power command failure tests start after the first released picture");
        if (fault == 0) f.pause_status = BC_STS_IO_ERROR;
        else if (fault == 1) f.resume_status = BC_STS_IO_ERROR;
        else f.status[8 + fault - 2] = BC_STS_IO_ERROR;
        const unsigned expected_reads = fault == 0 ? 8 : fault == 1 ? 12 : 7 + fault;
        check(!o.PauseAndResume(&f, Fixture::Read, Fixture::Pause, Fixture::Resume, false) &&
              o.failed && !o.Finish(true) && f.calls == expected_reads &&
              f.pauses == 1 && f.resumes == (fault != 0),
              "paused observation failure resumes once; failed pause never resumes and failed resume never samples");
        check(!o.PauseAndResume(&f, Fixture::Read, Fixture::Pause, Fixture::Resume, false) &&
              f.calls == expected_reads && f.pauses == 1 && f.resumes == (fault != 0),
              "power command failures are sticky and never retry commands or reads");
    }
    for (unsigned fault = 0; fault < 12; ++fault) {
        Fixture f; FleaPowerObserver o; o.enabled = true; o.firmware_command = Fixture::Firmware;
        check(o.Observe(&f, 0, Fixture::Read, false) && o.Observe(&f, 1, Fixture::Read, false),
              "stock firmware pause starts after the first released output");
        if (fault == 1 || fault == 3) f.pause_status = BC_STS_IO_ERROR;
        if (fault == 2 || fault == 3) f.resume_status = BC_STS_IO_ERROR;
        if (fault >= 4) f.status[8 + fault - 4] = BC_STS_IO_ERROR;
        const unsigned expected_reads = fault == 1 || fault == 3 ? 8 : fault == 2 ? 12 :
            fault >= 4 ? 5 + fault : 16;
        check(o.PauseAndResume(&f, Fixture::Read, nullptr, nullptr, false) == (fault == 0) &&
              f.calls == expected_reads && f.pauses == 1 && f.resumes == 1 && f.addresses_valid,
              "stock firmware pause attempts OFF exactly once even after ambiguous ON or snapshot failure");
        if (!fault) {
            check(o.Observe(&f, 4, Fixture::Read, false) && o.Observe(&f, 5, Fixture::Read, false) &&
                  o.Finish(true) && !o.Finish(false), "stock firmware pause preserves native completion requirements");
        } else {
            check(o.failed && !o.Finish(true) &&
                  !o.PauseAndResume(&f, Fixture::Read, nullptr, nullptr, false) &&
                  f.calls == expected_reads && f.pauses == 1 && f.resumes == 1,
                  "stock firmware failure cannot be erased by successful OFF or repeat any command");
        }
    }
    for (unsigned next : {0U, 1U, 3U, 4U, 5U, 6U}) {
        Fixture f; FleaPowerObserver o; o.enabled = true; o.firmware_command = Fixture::Firmware; o.next = next;
        check(!o.PauseAndResume(&f, Fixture::Read, nullptr, nullptr, false) &&
              !f.calls && !f.pauses && !f.resumes && o.failed,
              "stock firmware pause is not invoked outside the released-output barrier");
    }
    for (unsigned invalid = 0; invalid < 3; ++invalid) {
        Fixture f; FleaPowerObserver o; o.enabled = true; o.firmware_command = Fixture::Firmware; o.next = 2;
        if (invalid == 2) o.failed = true;
        check(!o.PauseAndResume(invalid == 0 ? nullptr : &f,
                  invalid == 1 ? nullptr : Fixture::Read, nullptr, nullptr, false) &&
              !f.calls && !f.pauses && !f.resumes && o.failed,
              "invalid stock firmware observation arguments cannot invoke a command");
    }
    for (BC_STATUS pause_status : {BC_STS_SUCCESS, BC_STS_IO_ERROR, BC_STS_BUSY, BC_STS_TIMEOUT}) {
        for (BC_STATUS resume_status : {BC_STS_SUCCESS, BC_STS_IO_ERROR, BC_STS_BUSY, BC_STS_TIMEOUT}) {
            Fixture f; f.pause_status = pause_status; f.resume_status = resume_status;
            FleaPowerObserver o; o.enabled = o.host_only = true;
            o.firmware_command = Fixture::Firmware;
            check(o.Observe(&f, 0, Fixture::Read, false) &&
                  o.Observe(&f, 1, Fixture::Read, false) && !f.calls && !o.reads,
                  "host-only stages add no raw reads before the first released picture");
            const bool commands_ok = pause_status == BC_STS_SUCCESS && resume_status == BC_STS_SUCCESS;
            check(o.PauseAndResume(&f, Fixture::Read, Fixture::Pause, Fixture::Resume, false) == commands_ok &&
                  !f.calls && !o.reads && f.pauses == 1 && f.resumes == 1 &&
                  f.addresses_valid && o.host_command_attempts == 2 && o.next == 4,
                  "host-only ON/OFF are attempted exactly once without raw reads, including both-error cleanup");
            if (commands_ok) {
                check(o.Observe(&f, 4, nullptr, false) && o.Observe(&f, 5, nullptr, false) &&
                      o.Finish(true) && !o.Finish(false) && !f.calls && !o.reads,
                      "host-only success requires all six stages and actual native delivery/EOS/cleanup");
            } else {
                check(o.failed && !o.Observe(&f, 4, Fixture::Read, false) &&
                      !o.PauseAndResume(&f, Fixture::Read, Fixture::Pause, Fixture::Resume, false) &&
                      !o.Finish(true) && !f.calls && f.pauses == 1 && f.resumes == 1,
                      "host-only first command failure stays failed after OFF and never retries commands or raw reads");
            }
        }
    }
    for (unsigned next : {0U, 1U, 3U, 4U, 5U, 6U}) {
        Fixture f; FleaPowerObserver o; o.enabled = o.host_only = true;
        o.firmware_command = Fixture::Firmware; o.next = next;
        check(!o.PauseAndResume(&f, Fixture::Read, Fixture::Pause, Fixture::Resume, false) &&
              o.failed && !f.calls && !f.pauses && !f.resumes,
              "host-only commands require the first released-picture stage and cannot be replayed");
    }
    for (unsigned invalid = 0; invalid < 6; ++invalid) {
        Fixture f; FleaPowerObserver o; o.enabled = o.host_only = true;
        o.firmware_command = Fixture::Firmware; o.next = 2;
        if (invalid == 1) o.firmware_command = nullptr;
        if (invalid == 2) o.configuration = true;
        if (invalid == 3) o.firmware_dma = true;
        if (invalid == 4) o.failed = true;
        if (invalid == 5) o.host_command_attempts = 2;
        check(!o.PauseAndResume(invalid ? &f : nullptr, Fixture::Read, Fixture::Pause, Fixture::Resume, false) &&
              !f.calls && !f.pauses && !f.resumes && o.failed && !o.Finish(true),
              "invalid or mixed host-only command state fails before command or raw access");
    }
    for (unsigned stage : {1U, 2U, 3U, 4U, 5U, 6U}) {
        Fixture f; FleaPowerObserver o; o.enabled = o.host_only = true;
        o.firmware_command = Fixture::Firmware;
        check(!o.Observe(&f, stage, Fixture::Read, false) && o.failed &&
              !f.calls && !f.pauses && !f.resumes && !o.Finish(true),
              "out-of-order host-only stage cannot manufacture a successful receipt");
    }
    {
        Fixture f; FleaPowerObserver o; o.enabled = o.host_only = true;
        o.firmware_command = Fixture::Firmware;
        check(o.Observe(&f, 0, nullptr, false) &&
              !o.Observe(&f, 0, Fixture::Read, false) && o.failed && !f.calls,
              "host-only permits an absent raw reader but a duplicate marker stays failed");
    }
    {
        Fixture f; FleaPowerObserver off; off.host_only = true;
        off.firmware_command = Fixture::Firmware;
        check(off.Observe(&f, 0, Fixture::Read, false) &&
              off.PauseAndResume(&f, Fixture::Read, Fixture::Pause, Fixture::Resume, false) &&
              off.Finish(true) && !f.calls && !f.pauses && !f.resumes,
              "disabled host-only mode does not add reads or commands to ordinary playback");
    }
    struct DmaFixture {
        unsigned calls = 0, pauses = 0, resumes = 0;
        bool valid = true;
        uint32_t raw[27] = {};
        BC_STATUS status[27] = {};
        BC_STATUS pause_status = BC_STS_SUCCESS, resume_status = BC_STS_SUCCESS;
        static uint32_t Address(unsigned i) {
            const uint32_t ordinary[] = {0x00070004, 0x0040cb30, 0x01800004, 0x00070004};
            const uint32_t paused[] = {0x00070004, 0x0040cb30, 0x00502000,
                0x00502008, 0x00500700, 0x01800004, 0x00070004};
            return i < 8 ? ordinary[i % 4] : i < 15 ? paused[i - 8] : ordinary[(i - 15) % 4];
        }
        DmaFixture() {
            for (unsigned i = 0; i < 27; ++i)
                raw[i] = Address(i) == 0x00070004 ? 0x03000000 :
                    Address(i) == 0x01800004 ? 0x72 : 0;
            // Synthetic host addresses, including a VALID descriptor, and all
            // completion/mailbox bits. None is an error or an idle proof.
            raw[10] = 0x12345661; raw[11] = 0x76543200; raw[12] = 0x000f1515;
        }
        static BC_STATUS Read(HANDLE handle, uint32_t address, uint32_t *value) {
            auto *f = static_cast<DmaFixture *>(handle);
            const unsigned i = f->calls++;
            if (i >= 27) { f->valid = false; return BC_STS_ERROR; }
            f->valid &= address == Address(i);
            // The three extra reads must be between PAUSE ON and OFF.
            if (i >= 8 && i < 15) f->valid &= f->pauses == 1 && !f->resumes;
            if (i >= 15) f->valid &= f->pauses == 1 && f->resumes == 1;
            *value = f->raw[i];
            return f->status[i];
        }
        static BC_STATUS Firmware(HANDLE handle, uint32_t operation) {
            auto *f = static_cast<DmaFixture *>(handle);
            if (operation == 1 && !f->pauses && !f->resumes && f->calls == 8) {
                ++f->pauses; return f->pause_status;
            }
            if (operation == 0 && f->pauses == 1 && !f->resumes) {
                ++f->resumes; return f->resume_status;
            }
            f->valid = false;
            return BC_STS_ERROR;
        }
        bool Run(FleaPowerObserver &o) {
            return o.Observe(this, 0, Read, false) && o.Observe(this, 1, Read, false) &&
                o.PauseAndResume(this, Read, nullptr, nullptr, false) &&
                o.Observe(this, 4, Read, false) && o.Observe(this, 5, Read, false);
        }
    };
    for (unsigned bit = 0; bit < 32; ++bit) {
        const uint32_t raw = UINT32_C(1) << bit;
        check(FleaPowerObserver::DmaValid(2, raw) == !(raw & 0x1eU) &&
              FleaPowerObserver::DmaValid(3, raw) == !(raw & 0x1eU),
              "TX lower words preserve address and VALID fields but reject reserved bits 4:1");
        check(FleaPowerObserver::DmaValid(4, raw) == !!(raw & 0x000f1515U),
              "INTR accepts only completion/mailbox bits, rejecting every reserved, DMA, halt and PCIe error bit");
        DmaFixture f; f.raw[12] = raw;
        FleaPowerObserver o; o.enabled = o.firmware_dma = true;
        o.firmware_command = DmaFixture::Firmware;
        const bool allowed = !!(raw & 0x000f1515U);
        check(f.Run(o) == allowed && o.Finish(true) == allowed && f.valid &&
              f.calls == (allowed ? 27U : 13U) && f.pauses == 1 && f.resumes == 1,
              "invalid or error IRQ stops before MEMC and the closing clock read but still attempts OFF once");
    }
    for (unsigned field = 0; field < 7; ++field)
        check(!FleaPowerObserver::DmaValid(field, UINT32_MAX),
              "DMA observation rejects all-ones values at all seven fields");
    for (unsigned valid = 0; valid < 4; ++valid) {
        DmaFixture f; f.raw[10] = valid & 1U; f.raw[11] = (valid >> 1) & 1U; f.raw[12] = 0;
        FleaPowerObserver o; o.enabled = o.firmware_dma = true;
        o.firmware_command = DmaFixture::Firmware;
        check(f.Run(o) && o.Finish(true) && !o.Finish(false) && f.valid &&
              f.calls == 27 && o.reads == 27 && o.next == 6 && f.pauses == 1 && f.resumes == 1,
              "all VALID combinations and zero IRQ remain observations in the exact 27-read profile");
        o.reads = 24;
        check(!o.Finish(true), "DMA observation cannot use the historical 24-read completion count");
    }
    for (unsigned bad = 0; bad < 27; ++bad) {
        for (unsigned api = 0; api < 2; ++api) {
            DmaFixture f;
            if (api) f.status[bad] = BC_STS_IO_ERROR;
            else f.raw[bad] = UINT32_MAX;
            FleaPowerObserver o; o.enabled = o.firmware_dma = true;
            o.firmware_command = DmaFixture::Firmware;
            check(!f.Run(o) && o.failed && !o.Finish(true) && f.valid &&
                  f.calls == bad + 1 && o.reads == bad + 1 &&
                  f.pauses == (bad >= 8) && f.resumes == (bad >= 8),
                  "every DMA-profile API/raw fault preserves the exact prefix and once-only OFF cleanup");
            check(!o.Observe(&f, o.next, DmaFixture::Read, false) &&
                  !o.PauseAndResume(&f, DmaFixture::Read, nullptr, nullptr, false) &&
                  f.calls == bad + 1 && f.pauses == (bad >= 8) && f.resumes == (bad >= 8),
                  "a failed DMA profile never retries a read or command");
        }
    }
    for (unsigned end : {3U, 7U, 14U, 18U, 22U, 26U}) {
        DmaFixture f; f.raw[end] ^= BCHP_CLK_PM_CTRL_DIS_MISC_OTP_9_CLK_MASK;
        FleaPowerObserver o; o.enabled = o.firmware_dma = true;
        o.firmware_command = DmaFixture::Firmware;
        check(!f.Run(o) && o.failed && f.valid && f.calls == end + 1 &&
              f.pauses == (end >= 8) && f.resumes == (end >= 8) && !o.Finish(true),
              "DMA mode retains clock brackets at every stage including the seven-read PAUSE stage");
    }
    for (unsigned fault = 1; fault < 4; ++fault) {
        DmaFixture f;
        if (fault & 1U) f.pause_status = BC_STS_IO_ERROR;
        if (fault & 2U) f.resume_status = BC_STS_IO_ERROR;
        FleaPowerObserver o; o.enabled = o.firmware_dma = true;
        o.firmware_command = DmaFixture::Firmware;
        const unsigned expected = fault & 1U ? 8 : 15;
        check(!f.Run(o) && o.failed && f.valid && f.calls == expected &&
              f.pauses == 1 && f.resumes == 1 && !o.Finish(true) &&
              !o.PauseAndResume(&f, DmaFixture::Read, nullptr, nullptr, false) &&
              f.calls == expected && f.pauses == 1 && f.resumes == 1,
              "DMA profile attempts OFF once after failed ON and never reads after failed OFF");
    }
    for (unsigned mixed = 0; mixed < 2; ++mixed) {
        DmaFixture f; FleaPowerObserver o; o.enabled = o.firmware_dma = true;
        if (mixed) { o.configuration = true; o.firmware_command = DmaFixture::Firmware; }
        check(!o.Observe(&f, 0, DmaFixture::Read, false) &&
              !o.PauseAndResume(&f, DmaFixture::Read, nullptr, nullptr, false) &&
              !o.Finish(true) && !f.calls && !f.pauses && !f.resumes,
              "DMA mode rejects a missing firmware callback or configuration mix before any I/O");
    }
    DmaFixture disabled_dma; FleaPowerObserver dma_off;
    dma_off.firmware_dma = true; dma_off.firmware_command = DmaFixture::Firmware;
    check(dma_off.Observe(&disabled_dma, 0, DmaFixture::Read, false) &&
          dma_off.PauseAndResume(&disabled_dma, DmaFixture::Read, nullptr, nullptr, false) &&
          dma_off.Finish(true) && !disabled_dma.calls && !disabled_dma.pauses && !disabled_dma.resumes,
          "an unenabled DMA observer performs no commands or reads");
    struct ConfigurationFixture {
        unsigned calls = 0;
        bool valid = true;
        BC_STATUS status[15] = {};
        uint32_t raw[15] = {};
        ConfigurationFixture() { raw[0] = raw[14] = 0x03000000; raw[12] = 1; raw[13] = 0x73; }
        static BC_STATUS Read(HANDLE handle, uint32_t address, uint32_t *value) {
            auto *f = static_cast<ConfigurationFixture *>(handle);
            const uint32_t addresses[] = {0x00070004, 0x01801004, 0x01801014,
                0x01801044, 0x0180124c, 0x0180114c, 0x01801038, 0x0180123c,
                0x0180113c, 0x01800014, 0x0040cb00, 0x0040cb30,
                0x01801010, 0x01800004, 0x00070004};
            const unsigned i = f->calls++;
            if (i >= 15) { f->valid = false; return BC_STS_ERROR; }
            f->valid &= address == addresses[i];
            *value = f->raw[i];
            return f->status[i];
        }
    };
    for (unsigned locked : {0U, 1U}) {
        ConfigurationFixture f; f.raw[12] = locked;
        FleaPowerObserver o; o.enabled = o.configuration = true;
        check(o.Observe(&f, 0, ConfigurationFixture::Read, false) && f.calls == 15 && f.valid &&
              o.Observe(&f, 1, nullptr, false) &&
              o.PauseAndResume(nullptr, nullptr, nullptr, nullptr, false) &&
              o.Observe(&f, 5, nullptr, false) && o.Finish(true) && !o.Finish(false) &&
              f.calls == 15 && o.reads == 15,
              "configuration collects fifteen ordered reads once and reports LOCK without inferring readiness");
    }
    for (unsigned bad = 0; bad < 15; ++bad) {
        for (unsigned api = 0; api < 2; ++api) {
            ConfigurationFixture f;
            if (api) f.status[bad] = BC_STS_IO_ERROR;
            else f.raw[bad] = UINT32_MAX;
            FleaPowerObserver o; o.enabled = o.configuration = true;
            check(!o.Observe(&f, 0, ConfigurationFixture::Read, false) && f.valid &&
                  o.failed && !o.Finish(true) && f.calls == bad + 1 && o.reads == bad + 1 &&
                  !o.Observe(&f, 0, ConfigurationFixture::Read, false) && f.calls == bad + 1,
                  "configuration stops at first invalid/API read and never retries or reads later fields");
        }
    }
    for (unsigned field : {0U, 1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U, 11U, 12U, 13U, 14U}) {
        ConfigurationFixture f;
        f.raw[field] |= field == 0 || field == 14 ? 4U : field >= 6 && field <= 8
            ? 0x80000000U : field >= 12 ? 4U : 1U;
        FleaPowerObserver o; o.enabled = o.configuration = true;
        check(!o.Observe(&f, 0, ConfigurationFixture::Read, false) && f.calls == field + 1 && f.valid,
              "configuration refuses gated clock/pad or reserved status before later target reads");
    }
    ConfigurationFixture mismatch; mismatch.raw[14] = 0;
    FleaPowerObserver mismatched; mismatched.enabled = mismatched.configuration = true;
    check(!mismatched.Observe(&mismatch, 0, ConfigurationFixture::Read, false) && mismatch.calls == 15,
          "configuration refuses inconsistent clock brackets without an atomic snapshot claim");
    ConfigurationFixture unused; FleaPowerObserver disabled; disabled.configuration = true;
    check(disabled.ObserveConfiguration(&unused, ConfigurationFixture::Read, false) && !unused.calls,
          "disabled configuration performs no reads");
    for (unsigned invalid = 0; invalid < 4; ++invalid) {
        ConfigurationFixture f; FleaPowerObserver o; o.enabled = o.configuration = true;
        const bool result = invalid == 0 ? o.Observe(nullptr, 0, ConfigurationFixture::Read, false) :
            invalid == 1 ? o.Observe(&f, 0, nullptr, false) :
            invalid == 2 ? o.Observe(&f, 1, ConfigurationFixture::Read, false) :
            o.PauseAndResume(&f, ConfigurationFixture::Read, nullptr, nullptr, false);
        check(!result && !f.calls && o.failed && !o.Finish(true),
              "configuration rejects missing arguments or skipped initial snapshot without I/O");
    }
    for (unsigned bad = 0; bad < 5; ++bad) {
        Fixture f; FleaPowerObserver o; o.enabled = true; o.firmware_command = Fixture::Firmware;
        check(o.Observe(&f, 0, Fixture::Read, false) && o.Observe(&f, 1, Fixture::Read, false),
              "stock firmware invalid-snapshot test starts at released output");
        if (bad < 4) f.raw[8 + bad] = UINT32_MAX;
        else f.raw[11] = BCHP_CLK_PM_CTRL_DIS_MISC_OTP_9_CLK_MASK;
        const unsigned expected_reads = bad < 4 ? 9 + bad : 12;
        check(!o.PauseAndResume(&f, Fixture::Read, nullptr, nullptr, false) &&
              o.failed && !o.Finish(true) && f.calls == expected_reads &&
              f.pauses == 1 && f.resumes == 1 && f.addresses_valid,
              "invalid or inconsistent paused snapshot still attempts OFF once without resume reads");
    }
    Fixture disabled_fw; FleaPowerObserver off_fw; off_fw.firmware_command = Fixture::Firmware;
    check(off_fw.PauseAndResume(&disabled_fw, Fixture::Read, nullptr, nullptr, false) &&
          !disabled_fw.calls && !disabled_fw.pauses && !disabled_fw.resumes && !off_fw.failed,
          "disabled observer cannot invoke an installed firmware callback");
    Fixture untouched; FleaPowerObserver off;
    check(off.Observe(nullptr, UINT32_MAX, nullptr, false) &&
          off.PauseAndResume(nullptr, nullptr, nullptr, nullptr, false) &&
          off.Finish(true) && !off.Finish(false) && !off.reads && !off.failed &&
          !untouched.calls && !untouched.pauses && !untouched.resumes,
          "disabled power observation is a no-op and never overrides native failure");
}

#endif
