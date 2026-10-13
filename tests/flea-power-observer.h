// SPDX-License-Identifier: LGPL-2.1-or-later
#ifndef CRYSTALHD_FLEA_POWER_OBSERVER_H
#define CRYSTALHD_FLEA_POWER_OBSERVER_H

#include "flea/70015/magnum/basemodules/chp/70015/rdb/a0/bchp_clk.h"
#include "flea/70015/magnum/basemodules/chp/70015/rdb/a0/bchp_ddr23_ctl_regs_0.h"
#include "flea/70015/magnum/basemodules/chp/70015/rdb/a0/bchp_pri_arb_control_regs.h"

// Fixed target-register reads during ordinary decode. The legacy accessor may
// write its indirect address portal; no target control register is changed.
// These observations do not disable the arbiter, request self-refresh or enable
// runtime power management, and successful reads do not certify fetch errors.
struct FleaPowerObserver {
    using Reader = BC_STATUS (*)(HANDLE, uint32_t, uint32_t *);
    using Command = BC_STATUS (*)(HANDLE);
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
    bool Observe(HANDLE handle, unsigned stage, Reader reader = DtsDevRegisterRead,
                 bool report = true) {
        if (!enabled) return true;
        if (failed || !handle || !reader || stage != next || stage >= 6) {
            failed = true;
            return false;
        }
        const char *const names[] = {"pre-START", "first-released", "pause-API-return",
            "resume-API-return", "last-released", "EOS"};
        uint32_t raw[4] = {};
        unsigned measured = 0;
        BC_STATUS status = BC_STS_SUCCESS;
        const gint64 before = g_get_monotonic_time();
        for (unsigned i = 0; i < 4; ++i) {
            ++reads;
            status = reader(handle, Address(i), &raw[i]);
            if (status != BC_STS_SUCCESS) { failed = true; break; }
            ++measured;
            if (!Valid(i, raw[i])) { failed = true; break; }
        }
        if (measured == 4 && raw[0] != raw[3]) failed = true;
        if (!failed) ++next;
        if (report) {
            std::printf("Power observation: stage=%s monotonic-us=%lld duration-us=%lld "
                "reads=%u measured=%u api-status=%d result=%s", names[stage],
                static_cast<long long>(before),
                static_cast<long long>(g_get_monotonic_time() - before),
                reads, measured, status, failed ? "FAIL" : "PASS");
            for (unsigned i = 0; i < measured; ++i)
                std::printf(" reg%u@%08x=%08x", i, Address(i), raw[i]);
            std::printf(" atomic=no power-gating-tested=no\n");
            std::fflush(stdout);
        }
        return !failed;
    }
    bool PauseAndResume(HANDLE handle, Reader reader = DtsDevRegisterRead,
                        Command pause = DtsPauseDecoder, Command resume = DtsResumeDecoder,
                        bool report = true) {
        if (!enabled) return true;
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
    bool Finish(bool native_ok) const {
        return native_ok && (!enabled || (!failed && next == 6 && reads == 24));
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
    Fixture untouched; FleaPowerObserver off;
    check(off.Observe(nullptr, UINT32_MAX, nullptr, false) &&
          off.PauseAndResume(nullptr, nullptr, nullptr, nullptr, false) &&
          off.Finish(true) && !off.Finish(false) && !off.reads && !off.failed &&
          !untouched.calls && !untouched.pauses && !untouched.resumes,
          "disabled power observation is a no-op and never overrides native failure");
}

#endif
