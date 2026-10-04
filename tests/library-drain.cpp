// SPDX-License-Identifier: LGPL-2.1-or-later
// Optional direct-library drain probe, not a pixel-quality benchmark.
// Use an external timeout as well: a userspace deadline cannot bound a stuck
// kernel ioctl or device close. --preflight never opens the CrystalHD device.
#include <bc_dts_types.h>
#include <libcrystalhd_if.h>
#include "crystalhd_ioctl_limits.h"
#include "flea/70015/magnum/basemodules/chp/70015/rdb/a0/bchp_mfd.h"
#include "flea/70015/magnum/basemodules/chp/70015/rdb/a0/bchp_scl_hd.h"
#include "../filters/gst/gst-plugin-1.0/gstcrystalhd-input.h"
extern "C" {
#include <libavformat/avformat.h>
}
#include <cerrno>
#include <climits>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <linux/capability.h>
#include <set>
#include <string>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <utility>
#include <vector>
#include "phase1-progress.h"

// Optional probe-only ABI from libcrystalhd_int_if.h. Avoid pulling its
// unrelated register-map dependencies into this direct-library test.
extern "C" BC_STATUS DtsDevRegisterRead(HANDLE handle, uint32_t offset, uint32_t *value);
extern "C" BC_STATUS DtsDevRegisterWr(HANDLE handle, uint32_t offset, uint32_t value);

struct ChromaConfiguration {
    uint32_t lac = 0, sampling = 0;
};

// Two named configuration reads only; no FIFO/status, pointer or write access.
// Separate reads do not certify atomicity, fetch errors or a source-surface lease.
static BC_STATUS ReadChromaConfiguration(HANDLE handle, ChromaConfiguration *config,
    BC_STATUS (*read_register)(HANDLE, uint32_t, uint32_t *) = DtsDevRegisterRead)
{
    const BC_STATUS status = read_register(handle, BCHP_MFD_LAC_CNTL, &config->lac);
    if (status != BC_STS_SUCCESS) return status;
    return read_register(handle, BCHP_MFD_CHROMA_SAMPLING_CNTL, &config->sampling);
}

static bool CanReadChromaConfiguration()
{
    __user_cap_header_struct header = {_LINUX_CAPABILITY_VERSION_3, 0};
    __user_cap_data_struct capabilities[2] = {};
    return syscall(SYS_capget, &header, capabilities) == 0 &&
        (capabilities[CAP_SYS_RAWIO / 32].effective & (1U << (CAP_SYS_RAWIO % 32)));
}

struct ChromaReadFixture {
    unsigned calls = 0;
    bool addresses_valid = true;
    BC_STATUS status[2] = {BC_STS_SUCCESS, BC_STS_SUCCESS};
    uint32_t values[2] = {};
    static BC_STATUS Read(HANDLE handle, uint32_t address, uint32_t *value) {
        auto *fixture = static_cast<ChromaReadFixture *>(handle);
        const uint32_t addresses[] = {BCHP_MFD_LAC_CNTL, BCHP_MFD_CHROMA_SAMPLING_CNTL};
        const unsigned index = fixture->calls++;
        if (index >= 2) {
            fixture->addresses_valid = false;
            return BC_STS_ERROR;
        }
        fixture->addresses_valid &= address == addresses[index];
        *value = fixture->values[index];
        return fixture->status[index];
    }
};

// Fixed raw register observations, not a scaler fetch/completion or bus-error
// certificate. The library's indirect read portal may write its own selectors;
// this observer never writes any SCL register, clears status or follows data.
static const uint32_t kSclAddresses[8] = {
    BCHP_SCL_HD_REVISION_ID, BCHP_SCL_HD_TOP_CONTROL, BCHP_SCL_HD_ENABLE,
    BCHP_SCL_HD_BVB_IN_SIZE, BCHP_SCL_HD_SRC_PIC_SIZE, BCHP_SCL_HD_DEST_PIC_SIZE,
    BCHP_SCL_HD_BVB_IN_STATUS, BCHP_SCL_HD_REVISION_ID
};
static const char *const kSclNames[8] = {
    "rev", "top", "enable", "bvb-in-size", "src-pic-size", "dest-pic-size",
    "bvb-in-status", "closing-rev"
};
enum class SclFailure { None, Argument, Read, Revision, Reserved, EngineStatus };
static SclFailure SclScalarFailure(unsigned field, uint32_t raw)
{
    const uint32_t masks[8] = {
        0x0000ffffU, 0x0000000eU, 0x00000001U, 0x07ff07ffU,
        0x07ff07ffU, 0x07ff07ffU, 0x000000ffU, 0x0000ffffU
    };
    if (field >= 8) return SclFailure::Argument;
    if (field == 0 || field == 7)
        return raw == 0x80U ? SclFailure::None : SclFailure::Revision;
    if (raw & ~masks[field]) return SclFailure::Reserved;
    return field == 6 && raw ? SclFailure::EngineStatus : SclFailure::None;
}
struct SclSnapshot {
    uint32_t raw[2][8] = {};
    unsigned reads = 0, measured = 0;
    BC_STATUS status = BC_STS_SUCCESS;
    SclFailure failure = SclFailure::None;
    bool Stable() const {
        return measured == 16 && !std::memcmp(raw[0], raw[1], sizeof(raw[0]));
    }
};
static bool ReadSclConfiguration(HANDLE handle, SclSnapshot *snapshot,
    BC_STATUS (*read_register)(HANDLE, uint32_t, uint32_t *) = DtsDevRegisterRead)
{
    if (!snapshot) return false;
    *snapshot = SclSnapshot{};
    if (!read_register) { snapshot->failure = SclFailure::Argument; return false; }
    for (unsigned pass = 0; pass < 2; ++pass) {
        for (unsigned field = 0; field < 8; ++field) {
            uint32_t raw = 0;
            ++snapshot->reads;
            snapshot->status = read_register(handle, kSclAddresses[field], &raw);
            if (snapshot->status != BC_STS_SUCCESS) {
                snapshot->failure = SclFailure::Read;
                return false;
            }
            snapshot->raw[pass][field] = raw;
            ++snapshot->measured;
            snapshot->failure = SclScalarFailure(field, raw);
            if (snapshot->failure != SclFailure::None) return false;
        }
    }
    return true;
}
enum class SclStage { PreStart, FormatChange, FirstReleased, LastReleased, EosBarrier };
struct SclObserver {
    bool enabled = false, failed = false;
    unsigned attempted = 0, reads = 0;
    bool Observe(HANDLE handle, SclStage stage,
        BC_STATUS (*read_register)(HANDLE, uint32_t, uint32_t *) = DtsDevRegisterRead,
        bool report = true) {
        if (!enabled) return true;
        const unsigned index = static_cast<unsigned>(stage);
        if (failed || index >= 5 || (attempted & (1U << index))) return false;
        attempted |= 1U << index; // One attempt, including admission/read failure.
        SclSnapshot snapshot;
        const bool ok = ReadSclConfiguration(handle, &snapshot, read_register);
        reads += snapshot.reads;
        failed = !ok;
        if (report) {
            const char *const stages[] = {"pre-START", "first-FMT-API-return",
                "first-output-after-release", "last-output-after-release", "final-EOS-barrier"};
            const char *const failures[] = {"none", "argument", "read-status",
                "revision", "reserved-bits", "observed-engine-status"};
            std::printf("SCL config: stage=%s reads=%u total-reads=%u measured=%u "
                "api-status=%d failure=%s raw-stable=%s atomic=no transport-certified=no "
                "clock/source/lease/completion-certified=no\n", stages[index], snapshot.reads,
                reads, snapshot.measured, snapshot.status,
                failures[static_cast<unsigned>(snapshot.failure)],
                snapshot.measured == 16 ? (snapshot.Stable() ? "yes" : "no") : "NOT-READ");
            for (unsigned pass = 0; pass < 2; ++pass) {
                std::printf("SCL config raw: stage=%s pass=%u", stages[index], pass);
                for (unsigned field = 0; field < 8; ++field) {
                    std::printf(" %s@%08x=", kSclNames[field], kSclAddresses[field]);
                    if (pass * 8 + field < snapshot.measured)
                        std::printf("%08x", snapshot.raw[pass][field]);
                    else std::printf("NOT-READ");
                }
                std::printf("\n");
            }
            std::fflush(stdout);
        }
        return ok;
    }
};

struct SclReadFixture {
    unsigned calls = 0, fail_at = 16;
    bool addresses_valid = true;
    BC_STATUS status = BC_STS_ERROR;
    uint32_t raw[16] = {0x80, 0xc, 0, 0, 0, 0, 0, 0x80,
                       0x80, 0xc, 0, 0, 0, 0, 0, 0x80};
    static BC_STATUS Read(HANDLE handle, uint32_t address, uint32_t *value) {
        auto *fixture = static_cast<SclReadFixture *>(handle);
        // Independent literals, including both closing REV reads.
        const uint32_t expected[8] = {0x540800, 0x540804, 0x540854, 0x540810,
            0x540818, 0x54081c, 0x5408a4, 0x540800};
        const unsigned index = fixture->calls++;
        if (index >= 16) { fixture->addresses_valid = false; return BC_STS_ERROR; }
        fixture->addresses_valid &= address == expected[index % 8];
        *value = index == fixture->fail_at ? 0xffffffffU : fixture->raw[index];
        return index == fixture->fail_at ? fixture->status : BC_STS_SUCCESS;
    }
};

// An explicitly uncertain active-firmware test-mux experiment. Separate API
// calls do not establish selector ownership or atomicity; a firmware race can
// remain even after a matching readback. Only TP_ADDR is ever written here.
enum class SclViewFailure { None, Argument, Api, Revision, Reserved, Admission, Selector, EngineStatus };
enum class SclViewField { Revision, InitialTop, InitialEnable, SavedControl, SelectedControl,
                          RestoredControl, Status, Data };
struct SclViewTrace {
    uint32_t raw[12] = {};
    unsigned reads = 0, measured = 0;
};
struct SclViewProbe {
    typedef BC_STATUS (*Reader)(HANDLE, uint32_t, uint32_t *);
    typedef BC_STATUS (*Writer)(HANDLE, uint32_t, uint32_t);
    unsigned selector = 0, milestones = 0, reads = 0, writes = 0;
    bool begun = false, dirty = false, selected = false, restore_attempted = false;
    bool restored = false, failed = false, access_lost = false;
    HANDLE owner = nullptr;
    BC_STATUS api_status = BC_STS_SUCCESS;
    SclViewFailure failure = SclViewFailure::None;
    bool Enabled() const { return selector != 0; }
    bool Reject(SclViewFailure why, bool lost = true) {
        failed = true;
        access_lost |= lost;
        if (failure == SclViewFailure::None || lost) failure = why;
        return false;
    }
    bool Read(HANDLE handle, SclViewTrace *trace, unsigned index, uint32_t address,
              SclViewField field, Reader reader, bool cleanup = false) {
        uint32_t raw = 0;
        ++reads; ++trace->reads;
        api_status = reader(handle, address, &raw);
        if (api_status != BC_STS_SUCCESS) return Reject(SclViewFailure::Api);
        trace->raw[index] = raw;
        ++trace->measured; // Invalid measured words are printed, unread tails are not.
        switch (field) {
        case SclViewField::Revision:
            return raw == 0x80U || Reject(SclViewFailure::Revision);
        case SclViewField::InitialTop:
            if (raw & ~0xeU) return Reject(SclViewFailure::Reserved);
            return raw == 0xcU || Reject(SclViewFailure::Admission);
        case SclViewField::InitialEnable:
            if (raw & ~1U) return Reject(SclViewFailure::Reserved);
            return raw == 0 || Reject(SclViewFailure::Admission);
        case SclViewField::SavedControl:
        case SclViewField::SelectedControl:
        case SclViewField::RestoredControl:
            if (raw & ~3U) return Reject(SclViewFailure::Reserved);
            return raw == (field == SclViewField::SelectedControl ? selector : 0U) ||
                Reject(field == SclViewField::SavedControl ? SclViewFailure::Admission : SclViewFailure::Selector);
        case SclViewField::Status:
            if (raw & ~0xffU) return Reject(SclViewFailure::Reserved);
            if (raw) { Reject(SclViewFailure::EngineStatus, false); return cleanup; }
            return true;
        case SclViewField::Data:
            return true; // All 32 bits are opaque, never an address or enum oracle.
        }
        return Reject(SclViewFailure::Argument);
    }
    void Report(const char *stage, const SclViewTrace &trace, const uint32_t *addresses,
                const char *const *names, unsigned count, unsigned pass_fields,
                bool report) const {
        if (!report) return;
        const char *const failures[] = {"none", "argument", "api-status", "revision",
            "reserved-bits", "admission", "selector-ownership-lost", "observed-engine-status"};
        std::printf("SCL view: stage=%s selector=%u reads=%u total-reads=%u target-write-attempts=%u "
            "measured=%u api-status=%d failure=%s experiment-result=%s restored-observed=%s "
            "api-calls-not-bus-certificate=yes atomic=no active-FW-race/safety-unproved=yes "
            "power/clock/source/lease/completion-certified=no\n", stage, selector, trace.reads,
            reads, writes, trace.measured, api_status, failures[static_cast<unsigned>(failure)],
            failed ? "FAIL" : (restored && milestones == 3 ? "PASS" : "INCOMPLETE"),
            restored ? "yes" : "no");
        for (unsigned index = 0; index < count; ++index) {
            std::printf("SCL view raw: stage=%s pass=%u %s@%08x=", stage,
                pass_fields ? index / pass_fields : 0, names[index % (pass_fields ? pass_fields : count)],
                addresses[index % (pass_fields ? pass_fields : count)]);
            if (index < trace.measured) std::printf("%08x", trace.raw[index]);
            else std::printf("NOT-READ");
            std::printf("\n");
        }
        if (pass_fields && count == pass_fields * 2)
            std::printf("SCL view repeats: stage=%s raw-stable=%s (opaque numeric equality only)\n",
                stage, trace.measured == count ?
                (!std::memcmp(trace.raw, trace.raw + pass_fields, pass_fields * sizeof(uint32_t)) ? "yes" : "no") : "NOT-READ");
        std::fflush(stdout);
    }
    bool Begin(HANDLE handle, Reader reader = DtsDevRegisterRead,
               Writer writer = DtsDevRegisterWr, bool report = true) {
        if (!Enabled()) return true;
        if (failed) return false;
        if (begun || failed || !handle || !reader || !writer || (selector != 2 && selector != 3))
            return Reject(SclViewFailure::Argument);
        begun = true; owner = handle; // One attempt, before any experiment I/O.
        const uint32_t addresses[] = {BCHP_SCL_HD_REVISION_ID, BCHP_SCL_HD_TOP_CONTROL,
            BCHP_SCL_HD_ENABLE, BCHP_SCL_HD_TEST_PORT_CONTROL, BCHP_SCL_HD_BVB_IN_STATUS,
            BCHP_SCL_HD_REVISION_ID};
        const char *const names[] = {"rev", "top", "enable", "ctrl", "status", "closing-rev"};
        const SclViewField fields[] = {SclViewField::Revision, SclViewField::InitialTop,
            SclViewField::InitialEnable, SclViewField::SavedControl, SclViewField::Status, SclViewField::Revision};
        SclViewTrace trace;
        bool ok = true;
        for (unsigned index = 0; index < 12 && ok; ++index)
            ok = Read(handle, &trace, index, addresses[index % 6], fields[index % 6], reader);
        Report("after-OPEN/pre-START", trace, addresses, names, 12, 6, report);
        if (!ok) return false;
        dirty = true; ++writes; // A failed API return cannot prove the write did not occur.
        api_status = writer(handle, BCHP_SCL_HD_TEST_PORT_CONTROL, selector);
        SclViewTrace readback;
        if (api_status != BC_STS_SUCCESS) ok = Reject(SclViewFailure::Api);
        else ok = Read(handle, &readback, 0, BCHP_SCL_HD_TEST_PORT_CONTROL, SclViewField::SelectedControl, reader);
        const uint32_t control[] = {BCHP_SCL_HD_TEST_PORT_CONTROL}; const char *const control_name[] = {"ctrl"};
        Report("select-readback", readback, control, control_name, 1, 0, report);
        selected = ok;
        return ok;
    }
    bool Observe(HANDLE handle, unsigned milestone, Reader reader = DtsDevRegisterRead,
                 bool report = true) {
        if (!Enabled()) return true;
        if (failed) return false;
        if (!selected || handle != owner || !reader || milestone >= 2 ||
            (milestones & (1U << milestone))) return Reject(SclViewFailure::Argument);
        milestones |= 1U << milestone;
        const uint32_t addresses[] = {BCHP_SCL_HD_REVISION_ID, BCHP_SCL_HD_TEST_PORT_CONTROL,
            BCHP_SCL_HD_TEST_PORT_DATA, BCHP_SCL_HD_BVB_IN_STATUS, BCHP_SCL_HD_REVISION_ID};
        const char *const names[] = {"rev", "ctrl", "data", "status", "closing-rev"};
        const SclViewField fields[] = {SclViewField::Revision, SclViewField::SelectedControl,
            SclViewField::Data, SclViewField::Status, SclViewField::Revision};
        SclViewTrace trace;
        bool ok = true;
        for (unsigned index = 0; index < 10 && ok; ++index)
            ok = Read(handle, &trace, index, addresses[index % 5], fields[index % 5], reader);
        Report(milestone ? "penultimate-179-after-release-and-owned-write" :
               "first-output-after-release-and-owned-write", trace, addresses, names, 10, 5, report);
        return ok;
    }
    bool AfterDelivered(HANDLE handle, unsigned frame, bool released, bool owned_written,
                        Reader reader = DtsDevRegisterRead, Writer writer = DtsDevRegisterWr,
                        bool report = true) {
        if (!Enabled()) return true;
        if (!released || !owned_written) return false; // Never touch the device before these host barriers.
        if (frame != 1 && frame != 179) return !failed;
        if (!Observe(handle, frame == 1 ? 0 : 1, reader, report)) return false;
        return frame != 179 || Restore(handle, reader, writer, report);
    }
    bool Restore(HANDLE handle, Reader reader = DtsDevRegisterRead,
                 Writer writer = DtsDevRegisterWr, bool report = true) {
        if (!Enabled()) return true;
        if (restored || restore_attempted || !dirty || access_lost) return !failed;
        if (handle != owner || !handle || !reader || !writer) return Reject(SclViewFailure::Argument);
        restore_attempted = true; // No retry, including a failed cleanup guard.
        const uint32_t addresses[] = {BCHP_SCL_HD_REVISION_ID, BCHP_SCL_HD_TEST_PORT_CONTROL,
            BCHP_SCL_HD_BVB_IN_STATUS, BCHP_SCL_HD_REVISION_ID};
        const char *const names[] = {"rev", "ctrl", "status", "closing-rev"};
        const SclViewField fields[] = {SclViewField::Revision, SclViewField::SelectedControl,
            SclViewField::Status, SclViewField::Revision};
        SclViewTrace guard;
        bool ok = true;
        for (unsigned index = 0; index < 4 && ok; ++index)
            ok = Read(handle, &guard, index, addresses[index], fields[index], reader, true);
        Report("restore-guard", guard, addresses, names, 4, 0, report);
        if (!ok) return false;
        ++writes;
        api_status = writer(handle, BCHP_SCL_HD_TEST_PORT_CONTROL, 0);
        SclViewTrace verification;
        if (api_status != BC_STS_SUCCESS) ok = Reject(SclViewFailure::Api);
        else {
            ok = Read(handle, &verification, 0, BCHP_SCL_HD_TEST_PORT_CONTROL, SclViewField::RestoredControl, reader);
            if (ok) ok = Read(handle, &verification, 1, BCHP_SCL_HD_REVISION_ID, SclViewField::Revision, reader);
        }
        const uint32_t verified[] = {BCHP_SCL_HD_TEST_PORT_CONTROL, BCHP_SCL_HD_REVISION_ID};
        const char *const verified_names[] = {"ctrl", "rev"};
        restored = ok;
        if (restored) { dirty = false; selected = false; }
        Report("restore-readback", verification, verified, verified_names, 2, 0, report);
        return ok && !failed;
    }
};

struct SclViewFixture {
    struct Event { bool write; uint32_t address, raw; };
    std::vector<Event> events;
    unsigned calls = 0, data_reads = 0, write_calls = 0;
    size_t fail_at = 41;
    BC_STATUS status = BC_STS_ERROR;
    bool valid = true, dirty_at_write = true;
    SclViewProbe *probe = nullptr;
    explicit SclViewFixture(unsigned selector = 2) {
        const uint32_t pre[] = {0x540800, 0x540804, 0x540854, 0x540880, 0x5408a4, 0x540800};
        const uint32_t initial[] = {0x80, 0xc, 0, 0, 0, 0x80};
        for (unsigned index = 0; index < 12; ++index)
            events.push_back(Event{false, pre[index % 6], initial[index % 6]});
        events.push_back(Event{true, 0x540880, selector});
        events.push_back(Event{false, 0x540880, selector});
        const uint32_t observe[] = {0x540800, 0x540880, 0x540884, 0x5408a4, 0x540800};
        const uint32_t active[] = {0x80, selector, 0xffffffffU, 0, 0x80};
        for (unsigned index = 0; index < 20; ++index)
            events.push_back(Event{false, observe[index % 5], active[index % 5]});
        events.push_back(Event{false, 0x540800, 0x80});
        events.push_back(Event{false, 0x540880, selector});
        events.push_back(Event{false, 0x5408a4, 0});
        events.push_back(Event{false, 0x540800, 0x80});
        events.push_back(Event{true, 0x540880, 0});
        events.push_back(Event{false, 0x540880, 0});
        events.push_back(Event{false, 0x540800, 0x80});
    }
    static BC_STATUS Read(HANDLE handle, uint32_t address, uint32_t *value) {
        auto *fixture = static_cast<SclViewFixture *>(handle);
        const unsigned index = fixture->calls++;
        if (index >= fixture->events.size()) { fixture->valid = false; return BC_STS_ERROR; }
        const Event &event = fixture->events[index];
        fixture->valid &= !event.write && event.address == address && value;
        fixture->data_reads += address == 0x540884;
        if (value) *value = index == fixture->fail_at ? 0xdeadbeefU : event.raw;
        return index == fixture->fail_at ? fixture->status : BC_STS_SUCCESS;
    }
    static BC_STATUS Write(HANDLE handle, uint32_t address, uint32_t value) {
        auto *fixture = static_cast<SclViewFixture *>(handle);
        const unsigned index = fixture->calls++;
        ++fixture->write_calls;
        fixture->dirty_at_write &= fixture->probe && fixture->probe->dirty;
        if (index >= fixture->events.size()) { fixture->valid = false; return BC_STS_ERROR; }
        const Event &event = fixture->events[index];
        fixture->valid &= event.write && event.address == address && event.raw == value;
        return index == fixture->fail_at ? fixture->status : BC_STS_SUCCESS;
    }
    bool Exercise(SclViewProbe *subject) {
        probe = subject;
        return subject->Begin(this, Read, Write, false) &&
            subject->Observe(this, 0, Read, false) && subject->Observe(this, 1, Read, false) &&
            subject->Restore(this, Read, Write, false);
    }
};

// Passive, fixed configuration reads only. MFD 0x50/SCL 0x80 and TEST_MODE 4
// pin this board's observed profile, not universal defaults. Saturation bit 2
// remains set; BVB test/pulse/state bits remain clear, without target writes.
// Remap/colour are opaque; matching brackets do not establish ownership.
static const uint32_t kMfdAdmissionAddresses[10] = {
    BCHP_MFD_REVISION_ID, BCHP_MFD_FEEDER_CNTL, BCHP_MFD_FIXED_COLOUR,
    BCHP_MFD_DATA_MODE, BCHP_MFD_RANGE_EXP_REMAP_CNTL, BCHP_MFD_TEST_MODE_CNTL,
    BCHP_SCL_HD_REVISION_ID, BCHP_SCL_HD_BVB_IN_STATUS,
    BCHP_MFD_REVISION_ID, BCHP_SCL_HD_REVISION_ID
};
static const char *const kMfdAdmissionNames[10] = {"mfd-rev", "ctrl", "fixed-colour",
    "data-mode", "remap", "test-mode", "scl-rev", "scl-status", "closing-mfd-rev", "closing-scl-rev"};
enum class MfdAdmissionFailure { None, Argument, Read, Revision, Reserved, Mode, EngineStatus, Unstable };
struct MfdAdmissionSnapshot {
    uint32_t raw[20] = {};
    unsigned reads = 0, measured = 0;
    BC_STATUS status = BC_STS_SUCCESS;
    MfdAdmissionFailure failure = MfdAdmissionFailure::None;
};
static bool ReadMfdAdmission(HANDLE handle, MfdAdmissionSnapshot *snapshot,
    SclViewProbe::Reader reader = DtsDevRegisterRead)
{
    if (!snapshot) return false;
    *snapshot = MfdAdmissionSnapshot{};
    if (!handle || !reader) { snapshot->failure = MfdAdmissionFailure::Argument; return false; }
    const uint32_t masks[10] = {0xffff, 0xf, 0xffffff, 1, 0x3ff, 0xf, 0xffff, 0xff, 0xffff, 0xffff};
    for (unsigned index = 0; index < 20; ++index) {
        const unsigned field = index % 10;
        uint32_t raw = 0;
        ++snapshot->reads;
        snapshot->status = reader(handle, kMfdAdmissionAddresses[field], &raw);
        if (snapshot->status != BC_STS_SUCCESS) snapshot->failure = MfdAdmissionFailure::Read;
        else {
            snapshot->raw[index] = raw; ++snapshot->measured;
            if (raw & ~masks[field]) snapshot->failure = MfdAdmissionFailure::Reserved;
            else if ((field == 0 || field == 8) && raw != 0x50)
                snapshot->failure = MfdAdmissionFailure::Revision;
            else if ((field == 6 || field == 9) && raw != 0x80)
                snapshot->failure = MfdAdmissionFailure::Revision;
            else if (((field == 1 || field == 3) && raw) || (field == 5 && raw != 4))
                snapshot->failure = MfdAdmissionFailure::Mode;
            else if (field == 7 && raw) snapshot->failure = MfdAdmissionFailure::EngineStatus;
        }
        if (snapshot->failure != MfdAdmissionFailure::None) return false;
    }
    if (std::memcmp(snapshot->raw, snapshot->raw + 10, 10 * sizeof(uint32_t))) {
        snapshot->failure = MfdAdmissionFailure::Unstable; return false;
    }
    return true;
}
struct MfdAdmissionObserver {
    bool enabled = false, failed = false;
    unsigned attempted = 0, reads = 0;
    HANDLE owner = nullptr;
    bool Observe(HANDLE handle, unsigned stage, SclViewProbe::Reader reader = DtsDevRegisterRead,
                 bool report = true) {
        if (!enabled) return true;
        if (failed) return false;
        if (!handle || !reader || stage > 1 || (attempted & (1U << stage)) ||
            (stage == 1 && (attempted != 1 || handle != owner))) { failed = true; return false; }
        attempted |= 1U << stage;
        if (stage == 0) owner = handle;
        MfdAdmissionSnapshot snapshot;
        const bool ok = ReadMfdAdmission(handle, &snapshot, reader);
        reads += snapshot.reads; failed = !ok;
        if (report) {
            const char *name = stage ? "first-output-after-release-and-owned-write" : "after-OPEN/pre-START";
            const char *const failures[] = {"none", "argument", "read-status", "revision",
                "reserved-bits", "mode-not-profile", "observed-engine-status", "tuple-unstable"};
            std::printf("MFD admission: stage=%s reads=%u total-reads=%u measured=%u api-status=%d "
                "failure=%s result=%s raw-stable=%s board-profile=MFD50/SCL80/TEST4 target-writes=0 non-atomic=yes "
                "API-success-not-transport-certificate=yes ownership/source-lease/completion-certified=no\n",
                name, snapshot.reads, reads, snapshot.measured, snapshot.status,
                failures[static_cast<unsigned>(snapshot.failure)], ok ? "PASS" : "FAIL",
                snapshot.measured == 20 ? (std::memcmp(snapshot.raw, snapshot.raw + 10,
                    10 * sizeof(uint32_t)) ? "no" : "yes") : "NOT-READ");
            for (unsigned index = 0; index < 20; ++index) {
                std::printf("MFD admission raw: stage=%s pass=%u %s@%08x=", name, index / 10,
                    kMfdAdmissionNames[index % 10], kMfdAdmissionAddresses[index % 10]);
                if (index < snapshot.measured) std::printf("%08x", snapshot.raw[index]);
                else std::printf("NOT-READ");
                std::printf("\n");
            }
            std::fflush(stdout);
        }
        return ok;
    }
    bool AfterDelivered(HANDLE handle, unsigned frame, bool released, bool written,
                        SclViewProbe::Reader reader = DtsDevRegisterRead, bool report = true) {
        if (!enabled) return true;
        if (!released || !written) return false;
        return frame == 1 ? Observe(handle, 1, reader, report) : !failed;
    }
};
struct MfdAdmissionFixture {
    unsigned calls = 0, fail_at = 20;
    bool valid = true;
    BC_STATUS status = BC_STS_ERROR;
    uint32_t raw[20] = {0x50, 0, 0x405ac3, 0, 0x21, 4, 0x80, 0, 0x50, 0x80,
                       0x50, 0, 0x405ac3, 0, 0x21, 4, 0x80, 0, 0x50, 0x80};
    static BC_STATUS Read(HANDLE handle, uint32_t address, uint32_t *value) {
        auto *fixture = static_cast<MfdAdmissionFixture *>(handle);
        const uint32_t expected[] = {0x540000, 0x540004, 0x540008, 0x540044, 0x54004c,
            0x540074, 0x540800, 0x5408a4, 0x540000, 0x540800};
        const unsigned index = fixture->calls++;
        if (index >= 20) { fixture->valid = false; return BC_STS_ERROR; }
        fixture->valid &= address == expected[index % 10] && value;
        *value = index == fixture->fail_at ? 0xdeadbeefU : fixture->raw[index];
        return index == fixture->fail_at ? fixture->status : BC_STS_SUCCESS;
    }
};

// An explicitly uncertain native-path experiment, not a firmware-control
// lease. Only FIXED_COLOUR and FEEDER_CNTL bit 3 are target-written; equality
// cannot exclude a firmware race between API calls. No source pointers follow.
enum class MfdColourFailure { None, Argument, Api, Revision, Reserved, ExpectedValue, EngineStatus, Unstable };
struct MfdColourTrace { uint32_t raw[20] = {}; unsigned reads = 0, measured = 0; };
struct MfdColourProbe {
    typedef SclViewProbe::Reader Reader;
    typedef SclViewProbe::Writer Writer;
    unsigned stimulus = 0, reads = 0, writes = 0;
    bool begun = false, first_attempted = false, observed = false, seeded = false;
    bool colour_dirty = false, control_dirty = false, selected = false;
    bool restore_attempted = false, restored = false, failed = false, access_lost = false;
    HANDLE owner = nullptr;
    uint32_t saved_colour = 0, saved_remap = 0;
    BC_STATUS api_status = BC_STS_SUCCESS;
    MfdColourFailure failure = MfdColourFailure::None;
    bool Enabled() const { return stimulus != 0; }
    uint32_t Colour() const { return stimulus == 1 ? 0x405ac3U : 0xb2d32bU; }
    bool Reject(MfdColourFailure why, bool lost = true) {
        failed = true; access_lost |= lost;
        if (failure == MfdColourFailure::None || lost) failure = why;
        return false;
    }
    void Report(const char *stage, const MfdColourTrace &trace, const uint32_t *addresses,
                const char *const *names, unsigned count, bool report, unsigned pass_fields = 0) const {
        if (!report) return;
        const char *const failures[] = {"none", "argument", "api-status", "revision", "reserved-bits",
            "expected-value-lost", "observed-engine-status", "tuple-unstable"};
        std::printf("MFD colour: stage=%s stimulus=%c reads=%u total-reads=%u target-write-attempts=%u "
            "measured=%u api-status=%d failure=%s experiment-result=%s restored-observed=%s "
            "API-calls-not-bus-certificate=yes atomic=no active-FW-race/safety-unproved=yes "
            "clock/source/lease/routing/completion-certified=no\n", stage, stimulus == 1 ? 'a' : 'b',
            trace.reads, reads, writes, trace.measured, api_status, failures[static_cast<unsigned>(failure)],
            failed ? "FAIL" : restored && observed ? "PASS" : "INCOMPLETE", restored ? "yes" : "no");
        for (unsigned index = 0; index < count; ++index) {
            const unsigned field = pass_fields ? index % pass_fields : index;
            std::printf("MFD colour raw: stage=%s pass=%u %s@%08x=", stage,
                pass_fields ? index / pass_fields : 0, names[field], addresses[field]);
            if (index < trace.measured) std::printf("%08x", trace.raw[index]); else std::printf("NOT-READ");
            std::printf("\n");
        }
        if (pass_fields)
            std::printf("MFD colour repeats: stage=%s raw-stable=%s (non-atomic numeric comparison)\n", stage,
                trace.measured == count ? (!std::memcmp(trace.raw, trace.raw + pass_fields,
                    pass_fields * sizeof(uint32_t)) ? "yes" : "no") : "NOT-READ");
        std::fflush(stdout);
    }
    bool Admission(HANDLE handle, const char *stage, Reader reader, bool report, bool save) {
        MfdAdmissionSnapshot snapshot;
        const bool ok = ReadMfdAdmission(handle, &snapshot, reader);
        reads += snapshot.reads; api_status = snapshot.status;
        if (!ok) {
            const MfdColourFailure errors[] = {MfdColourFailure::None, MfdColourFailure::Argument,
                MfdColourFailure::Api, MfdColourFailure::Revision, MfdColourFailure::Reserved,
                MfdColourFailure::ExpectedValue, MfdColourFailure::EngineStatus, MfdColourFailure::Unstable};
            Reject(errors[static_cast<unsigned>(snapshot.failure)], snapshot.failure != MfdAdmissionFailure::EngineStatus);
        } else if (save) { saved_colour = snapshot.raw[2]; saved_remap = snapshot.raw[4]; seeded = true; }
        MfdColourTrace trace; std::memcpy(trace.raw, snapshot.raw, sizeof(trace.raw));
        trace.reads = snapshot.reads; trace.measured = snapshot.measured;
        Report(stage, trace, kMfdAdmissionAddresses, kMfdAdmissionNames, 20, report, 10);
        if (save && seeded && report) {
            std::printf("MFD colour saved-current: colour=%08x remap=%08x pre-START-value-not-used=yes\n",
                saved_colour, saved_remap); std::fflush(stdout);
        }
        return ok;
    }
    bool Read(HANDLE handle, MfdColourTrace *trace, unsigned index, uint32_t address,
              uint32_t mask, uint32_t expected, Reader reader, bool status = false, bool cleanup = false) {
        if (access_lost) return false;
        if (!handle || handle != owner || !trace || index >= 20 || !reader) return Reject(MfdColourFailure::Argument);
        uint32_t raw = 0; ++reads; ++trace->reads;
        api_status = reader(handle, address, &raw);
        if (api_status != BC_STS_SUCCESS) return Reject(MfdColourFailure::Api);
        trace->raw[index] = raw; ++trace->measured;
        if (raw & ~mask) return Reject(MfdColourFailure::Reserved);
        if (status && raw) { Reject(MfdColourFailure::EngineStatus, false); return cleanup; }
        return raw == expected || Reject(address == BCHP_MFD_REVISION_ID || address == BCHP_SCL_HD_REVISION_ID ?
            MfdColourFailure::Revision : MfdColourFailure::ExpectedValue);
    }
    bool Guard(HANDLE handle, const char *stage, uint32_t control, Reader reader, bool report, bool cleanup = false) {
        const uint32_t masks[] = {0xffff, 0xf, 0xffffff, 1, 0x3ff, 0xf, 0xffff, 0xff, 0xffff, 0xffff};
        const uint32_t expected[] = {0x50, control, Colour(), 0, saved_remap, 4, 0x80, 0, 0x50, 0x80};
        MfdColourTrace trace; bool ok = true;
        for (unsigned index = 0; index < 10 && ok; ++index)
            ok = Read(handle, &trace, index, kMfdAdmissionAddresses[index], masks[index], expected[index],
                reader, index == 7, cleanup);
        Report(stage, trace, kMfdAdmissionAddresses, kMfdAdmissionNames, 10, report);
        return ok;
    }
    bool Write(HANDLE handle, uint32_t address, uint32_t raw, Writer writer, bool report) {
        if (access_lost) return false;
        if (!handle || handle != owner || !writer) return Reject(MfdColourFailure::Argument);
        ++writes;
        if (report) { std::printf("MFD colour target-write: address=%08x value=%08x attempt=%u\n", address, raw, writes); std::fflush(stdout); }
        api_status = writer(handle, address, raw);
        const bool ok = api_status == BC_STS_SUCCESS || Reject(MfdColourFailure::Api);
        if (report) { std::printf("MFD colour write-return: address=%08x api-status=%d result=%s\n",
            address, api_status, ok ? "SUCCESS" : "FAIL"); std::fflush(stdout); }
        return ok;
    }
    bool Readback(HANDLE handle, const char *stage, uint32_t address, uint32_t expected,
                  Reader reader, bool report) {
        MfdColourTrace trace;
        const bool ok = Read(handle, &trace, 0, address, address == BCHP_MFD_FEEDER_CNTL ? 0xf : 0xffffff,
            expected, reader);
        const char *const names[] = {address == BCHP_MFD_FEEDER_CNTL ? "ctrl" : "fixed-colour"};
        Report(stage, trace, &address, names, 1, report);
        return ok;
    }
    bool PreStart(HANDLE handle, Reader reader = DtsDevRegisterRead, bool report = true) {
        if (!Enabled()) return true;
        if (failed) return false;
        if (begun || !handle || !reader || stimulus > 2) return Reject(MfdColourFailure::Argument);
        begun = true; owner = handle; // One attempt, before I/O.
        return Admission(handle, "after-OPEN/pre-START", reader, report, false);
    }
    bool First(HANDLE handle, Reader reader, Writer writer, bool report) {
        if (failed) return false;
        if (!begun || first_attempted || handle != owner || !reader || !writer) return Reject(MfdColourFailure::Argument);
        first_attempted = true;
        if (!Admission(handle, "first-output-after-release-and-owned-write", reader, report, true)) return false;
        colour_dirty = true; // Even a failed write return cannot prove it did not occur.
        if (!Write(handle, BCHP_MFD_FIXED_COLOUR, Colour(), writer, report) ||
            !Readback(handle, "injected-colour-readback", BCHP_MFD_FIXED_COLOUR, Colour(), reader, report) ||
            !Guard(handle, "pre-enable-guard", 0, reader, report)) return false;
        control_dirty = true;
        selected = Write(handle, BCHP_MFD_FEEDER_CNTL, 8, writer, report) &&
            Readback(handle, "selected-control-readback", BCHP_MFD_FEEDER_CNTL, 8, reader, report);
        return selected;
    }
    bool Restore(HANDLE handle, Reader reader = DtsDevRegisterRead, Writer writer = DtsDevRegisterWr, bool report = true) {
        if (!Enabled()) return true;
        if (restored || restore_attempted || (!colour_dirty && !control_dirty) || access_lost) return !failed;
        if (!seeded || !handle || handle != owner || !reader || !writer) return Reject(MfdColourFailure::Argument);
        restore_attempted = true;
        if (!Guard(handle, "restore-guard", control_dirty ? 8 : 0, reader, report, true)) return false;
        if (control_dirty) {
            if (!Write(handle, BCHP_MFD_FEEDER_CNTL, 0, writer, report) ||
                !Readback(handle, "restored-control-readback", BCHP_MFD_FEEDER_CNTL, 0, reader, report)) return false;
            control_dirty = false;
        }
        if (!Readback(handle, "fresh-colour-before-restore", BCHP_MFD_FIXED_COLOUR, Colour(), reader, report)) return false;
        if (!Write(handle, BCHP_MFD_FIXED_COLOUR, saved_colour, writer, report) ||
            !Readback(handle, "restored-colour-readback", BCHP_MFD_FIXED_COLOUR, saved_colour, reader, report)) return false;
        MfdColourTrace tail;
        bool ok = Read(handle, &tail, 0, BCHP_MFD_REVISION_ID, 0xffff, 0x50, reader);
        if (ok) ok = Read(handle, &tail, 1, BCHP_SCL_HD_REVISION_ID, 0xffff, 0x80, reader);
        const uint32_t addresses[] = {BCHP_MFD_REVISION_ID, BCHP_SCL_HD_REVISION_ID};
        const char *const names[] = {"mfd-rev", "scl-rev"};
        if (ok) { colour_dirty = false; selected = false; restored = true; }
        Report("restoration-closing-revisions", tail, addresses, names, 2, report);
        return ok && !failed;
    }
    bool AfterDelivered(HANDLE handle, unsigned frame, bool released, bool written,
                        Reader reader = DtsDevRegisterRead, Writer writer = DtsDevRegisterWr, bool report = true) {
        if (!Enabled()) return true;
        if (failed) return false;
        if (!released || !written) return false;
        if (frame == 1) return First(handle, reader, writer, report);
        if (frame != 90) return true;
        if (!selected || observed || handle != owner || !reader || !writer) return Reject(MfdColourFailure::Argument);
        observed = true;
        return Guard(handle, "frame-90-after-release-and-owned-write", 8, reader, report) &&
            Restore(handle, reader, writer, report);
    }
};

struct MfdColourFixture {
    struct Event { bool write; uint32_t address, raw; };
    std::vector<Event> events;
    unsigned calls = 0, write_calls = 0;
    size_t fail_at = SIZE_MAX;
    BC_STATUS status = BC_STS_ERROR;
    bool valid = true;
    MfdColourProbe *probe = nullptr;
    explicit MfdColourFixture(unsigned stimulus = 1, uint32_t saved = 0x909070) {
        const uint32_t colour = stimulus == 1 ? 0x405ac3 : 0xb2d32b;
        Tuple(0, 0x108080, 2); Tuple(0, saved, 2);
        events.push_back({true, 0x540008, colour}); events.push_back({false, 0x540008, colour});
        Tuple(0, colour, 1);
        events.push_back({true, 0x540004, 8}); events.push_back({false, 0x540004, 8});
        Tuple(8, colour, 1); Tuple(8, colour, 1);
        events.push_back({true, 0x540004, 0}); events.push_back({false, 0x540004, 0});
        events.push_back({false, 0x540008, colour}); events.push_back({true, 0x540008, saved});
        events.push_back({false, 0x540008, saved}); events.push_back({false, 0x540000, 0x50});
        events.push_back({false, 0x540800, 0x80});
    }
    void Tuple(uint32_t control, uint32_t colour, unsigned passes) {
        const uint32_t addresses[] = {0x540000, 0x540004, 0x540008, 0x540044, 0x54004c,
            0x540074, 0x540800, 0x5408a4, 0x540000, 0x540800};
        const uint32_t values[] = {0x50, control, colour, 0, 0x108, 4, 0x80, 0, 0x50, 0x80};
        for (unsigned pass = 0; pass < passes; ++pass)
            for (unsigned field = 0; field < 10; ++field) events.push_back({false, addresses[field], values[field]});
    }
    static BC_STATUS Read(HANDLE handle, uint32_t address, uint32_t *value) {
        auto *fixture = static_cast<MfdColourFixture *>(handle); const unsigned index = fixture->calls++;
        if (index >= fixture->events.size()) { fixture->valid = false; return BC_STS_ERROR; }
        const auto &event = fixture->events[index];
        fixture->valid &= !event.write && event.address == address && value;
        *value = index == fixture->fail_at ? 0xdeadbeefU : event.raw;
        return index == fixture->fail_at ? fixture->status : BC_STS_SUCCESS;
    }
    static BC_STATUS Write(HANDLE handle, uint32_t address, uint32_t value) {
        auto *fixture = static_cast<MfdColourFixture *>(handle); const unsigned index = fixture->calls++;
        if (index >= fixture->events.size()) { fixture->valid = false; return BC_STS_ERROR; }
        const auto &event = fixture->events[index]; ++fixture->write_calls;
        fixture->valid &= event.write && event.address == address && event.raw == value && fixture->probe &&
            fixture->probe->colour_dirty && (address != 0x540004 || fixture->probe->control_dirty);
        return index == fixture->fail_at ? fixture->status : BC_STS_SUCCESS;
    }
    bool Exercise(MfdColourProbe *subject, bool report = false) {
        probe = subject;
        return subject->PreStart(this, Read, report) &&
            subject->AfterDelivered(this, 1, true, true, Read, Write, report) &&
            subject->AfterDelivered(this, 90, true, true, Read, Write, report);
    }
};

struct PackingReadFixture {
    unsigned reads = 0, writes = 0;
    bool valid = true;
    BC_STATUS status = BC_STS_SUCCESS;
    static BC_STATUS Read(HANDLE handle, uint32_t address, uint32_t *value) {
        auto *fixture = static_cast<PackingReadFixture *>(handle); ++fixture->reads;
        fixture->valid &= address == 0x502100 && value;
        *value = fixture->status == BC_STS_SUCCESS ? 0x16 : 0xdeadbeefU;
        return fixture->status;
    }
    static BC_STATUS Write(HANDLE handle, uint32_t, uint32_t) {
        ++static_cast<PackingReadFixture *>(handle)->writes; return BC_STS_ERROR;
    }
};

static bool PackingState(HANDLE handle, const char *stage, MfdColourProbe *colour = nullptr,
    SclViewProbe *view = nullptr, SclViewProbe::Reader reader = DtsDevRegisterRead, bool report = true);

static volatile std::sig_atomic_t interrupted;
static void Interrupt(int) { interrupted = 1; }
static const unsigned kMaximumPackets = 10000;
static const unsigned kMaximumIterations = 1000;
static const uint64_t kTokenStep = 100000;
static const unsigned long kRssGrowthLimitKiB = 32UL * 1024UL;
static const uint64_t kMaximumCaptureBytes = 256ULL * 1024 * 1024;

struct Deadline {
    gint64 end;
    explicit Deadline(unsigned seconds) : end(g_get_monotonic_time() + seconds * G_USEC_PER_SEC) {}
    bool expired() const { return interrupted || g_get_monotonic_time() >= end; }
    static int Check(void *opaque) { return static_cast<Deadline *>(opaque)->expired(); }
};

struct Packet {
    std::vector<uint8_t> data;
    size_t size;
    gsize reservation;
};
struct Input {
    AVCodecID codec = AV_CODEC_ID_NONE;
    BC_MEDIA_SUBTYPE subtype = BC_MSUBTYPE_INVALID;
    unsigned width = 0, height = 0;
    bool progressive = false;
    std::vector<uint8_t> metadata;
    std::vector<Packet> packets;
};

static bool Number(const char *text, unsigned maximum, unsigned *value)
{
    if (!text || !*text || *text == '-' || *text == '+') return false;
    char *end = nullptr;
    errno = 0;
    const unsigned long parsed = std::strtoul(text, &end, 10);
    if (errno || !end || *end || parsed == 0 || parsed > maximum) return false;
    *value = static_cast<unsigned>(parsed);
    return true;
}

enum class Mode { SelfTest, Preflight, Hardware };

struct Options {
    Mode mode = Mode::SelfTest;
    const char *path = nullptr;
    unsigned expected = 0;
    unsigned seconds = 30;
    unsigned iterations = 1;
    bool scaler_test = false;
    unsigned scale_width = 0;
    bool mpeg1_via_mpeg2 = false;
    bool h263_via_divx = false;
    bool open_only = false;
    const char *capture_path = nullptr;
    bool observe_chroma = false;
    bool observe_scl_config = false;
    unsigned observe_scl_view = 0;
    bool observe_mfd_config = false;
    unsigned inject_mfd_colour = 0;
    BC_OUTPUT_FORMAT output_format = OUTPUT_MODE422_YUY2;
};

static uint32_t ProbeDeviceMode(const Options &options)
{
    uint32_t mode = DTS_PLAYBACK_MODE | DTS_LOAD_FILE_PLAY_FW | DTS_SKIP_TX_CHK_CPB |
        DTS_PLAYBACK_DROP_RPT_MODE | DTS_DFLT_RESOLUTION(vdecRESOLUTION_1080p23_976);
    // Native-width controls must avoid the single-thread 1280-pixel preset.
    if (!options.scaler_test || options.scale_width)
        mode |= DTS_SINGLE_THREADED_MODE;
    return mode;
}

static bool ParseArguments(std::vector<const char *> arguments, Options *options)
{
    if (arguments.size() >= 3 &&
        (!std::strcmp(arguments[arguments.size() - 2], "--capture-yuy2") ||
         !std::strcmp(arguments[arguments.size() - 2], "--capture-uyvy"))) {
        const char *path = arguments.back();
        if (!*path || !std::strcmp(path, "-")) return false;
        options->output_format = !std::strcmp(arguments[arguments.size() - 2], "--capture-uyvy")
            ? OUTPUT_MODE422_UYVY : OUTPUT_MODE422_YUY2;
        options->capture_path = path;
        arguments.resize(arguments.size() - 2);
    }
    if (arguments.size() >= 2 && !std::strcmp(arguments.back(), "--observe-scl-config")) {
        options->observe_scl_config = true;
        arguments.pop_back();
    }
    if (arguments.size() >= 2 && !std::strcmp(arguments.back(), "--observe-mfd-config")) {
        options->observe_mfd_config = true;
        arguments.pop_back();
    }
    if (arguments.size() >= 3 && !std::strcmp(arguments[arguments.size() - 2], "--inject-mfd-colour")) {
        if (std::strcmp(arguments.back(), "a") && std::strcmp(arguments.back(), "b")) return false;
        options->inject_mfd_colour = arguments.back()[0] == 'a' ? 1 : 2;
        arguments.resize(arguments.size() - 2);
    }
    if (arguments.size() >= 3 && !std::strcmp(arguments[arguments.size() - 2], "--observe-scl-view")) {
        const char *selector = arguments.back();
        if (std::strcmp(selector, "2") && std::strcmp(selector, "3")) return false;
        options->observe_scl_view = selector[0] - '0';
        arguments.resize(arguments.size() - 2);
    }
    if (arguments.size() >= 2 && !std::strcmp(arguments.back(), "--observe-chroma")) {
        options->observe_chroma = true;
        arguments.pop_back();
    }
    if (arguments.size() >= 2 && !std::strcmp(arguments.back(), "--open-only")) {
        options->open_only = true;
        arguments.pop_back();
    }
    if (arguments.size() >= 2 && !std::strcmp(arguments.back(), "--mpeg1-via-mpeg2")) {
        options->mpeg1_via_mpeg2 = true;
        arguments.pop_back();
    } else if (arguments.size() >= 2 && !std::strcmp(arguments.back(), "--h263-via-divx")) {
        options->h263_via_divx = true;
        arguments.pop_back();
    }
    if ((options->mpeg1_via_mpeg2 && options->h263_via_divx) ||
        (options->open_only && !options->h263_via_divx)) return false;
    if (arguments.size() >= 4 &&
        !std::strcmp(arguments[arguments.size() - 2], "--scaler-test")) {
        const char *width = arguments.back();
        if (std::strcmp(width, "0")) {
            for (const char *digit = width; *digit; ++digit)
                if (*digit < '0' || *digit > '9') return false;
            if (!Number(width, 1918, &options->scale_width) ||
                options->scale_width < 128 || (options->scale_width & 1))
                return false;
        }
        options->scaler_test = true;
        arguments.resize(arguments.size() - 2);
    }
    if (arguments.size() == 2 && !std::strcmp(arguments[1], "--self-test")) {
        if (options->capture_path || options->observe_chroma || options->observe_scl_config || options->observe_scl_view || options->observe_mfd_config || options->inject_mfd_colour || options->scaler_test || options->mpeg1_via_mpeg2 ||
            options->h263_via_divx || options->open_only) return false;
        options->mode = Mode::SelfTest;
        return true;
    }
    if (arguments.size() < 4 || arguments.size() > 6) return false;
    const bool preflight = !std::strcmp(arguments[1], "--preflight");
    const bool hardware = !std::strcmp(arguments[1], "--hardware");
    if ((!preflight && !hardware) || (preflight && arguments.size() > 5) ||
        (options->open_only && (!hardware || options->scaler_test)) ||
        !Number(arguments[3], kMaximumPackets, &options->expected) ||
        (arguments.size() >= 5 && !Number(arguments[4], 300, &options->seconds)) ||
        (arguments.size() == 6 &&
         !Number(arguments[5], kMaximumIterations, &options->iterations)))
        return false;
    options->mode = preflight ? Mode::Preflight : Mode::Hardware;
    options->path = arguments[2];
    if (options->observe_chroma &&
        (!hardware || !options->capture_path || !options->scaler_test || options->scale_width))
        return false;
    if (options->observe_scl_config &&
        (!hardware || !options->capture_path || !options->scaler_test ||
         options->iterations != 1 || options->output_format != OUTPUT_MODE422_YUY2 ||
         options->mpeg1_via_mpeg2 || options->h263_via_divx || options->open_only ||
         options->observe_chroma || (options->scale_width != 0 &&
         options->scale_width != 320 && options->scale_width != 640))) return false;
    if (options->observe_scl_view &&
        (!hardware || !options->capture_path || !options->scaler_test || options->scale_width ||
         options->expected != 180 || options->iterations != 1 ||
         options->output_format != OUTPUT_MODE422_YUY2 || options->observe_scl_config ||
         options->observe_chroma || options->mpeg1_via_mpeg2 || options->h263_via_divx ||
         options->open_only)) return false;
    if (options->observe_mfd_config &&
        (!hardware || !options->capture_path || !options->scaler_test || options->scale_width ||
         options->expected != 180 || options->iterations != 1 || options->output_format != OUTPUT_MODE422_YUY2 ||
         options->observe_scl_config || options->observe_scl_view || options->observe_chroma ||
         options->mpeg1_via_mpeg2 || options->h263_via_divx || options->open_only)) return false;
    if (options->inject_mfd_colour &&
        (!hardware || !options->capture_path || !options->scaler_test || options->scale_width ||
         options->expected != 180 || options->iterations != 1 || options->output_format != OUTPUT_MODE422_YUY2 ||
         options->observe_mfd_config || options->observe_scl_config || options->observe_scl_view || options->observe_chroma ||
         options->mpeg1_via_mpeg2 || options->h263_via_divx || options->open_only)) return false;
    return !options->capture_path ||
        (hardware && options->scaler_test && !options->open_only && options->iterations == 1);
}

static bool SclInputAdmitted(const Options &options, const Input &input)
{
    return (!options.observe_scl_config && !options.observe_scl_view && !options.observe_mfd_config && !options.inject_mfd_colour) ||
        (options.expected == 180 && input.codec == AV_CODEC_ID_MPEG2VIDEO &&
         input.subtype == BC_MSUBTYPE_MPEG2VIDEO && input.progressive &&
         input.width == 640 && input.height == 360 && input.packets.size() == 180);
}

static bool NeedsRawIo(const Options &options)
{
    return options.observe_chroma || options.observe_scl_config ||
        options.observe_scl_view || options.observe_mfd_config || options.inject_mfd_colour;
}

// Test-only MPEG-1 admission through the existing algorithm-1 channel. Do not
// use BC_MSUBTYPE_MPEG1VIDEO: the format setter has no MPEG-1 algorithm branch.
static BC_MEDIA_SUBTYPE InputSubtype(AVCodecID codec, const char *demuxer,
                                    bool mpeg1_via_mpeg2,
                                    bool h263_via_divx = false, int extradata_size = 0)
{
    if (!demuxer || (mpeg1_via_mpeg2 && h263_via_divx)) return BC_MSUBTYPE_INVALID;
    // Test-only baseline H.263 bytes through the existing algorithm-6 route.
    // No container, H.263+ protocol, or synthesized MPEG-4 metadata is admitted.
    if (h263_via_divx)
        return codec == AV_CODEC_ID_H263 && !std::strcmp(demuxer, "h263") &&
               extradata_size == 0 ? BC_MSUBTYPE_DIVX : BC_MSUBTYPE_INVALID;
    if (mpeg1_via_mpeg2)
        return codec == AV_CODEC_ID_MPEG1VIDEO && !std::strcmp(demuxer, "mpegvideo")
            ? BC_MSUBTYPE_MPEG2VIDEO : BC_MSUBTYPE_INVALID;
    if (codec == AV_CODEC_ID_H264 && !std::strcmp(demuxer, "h264"))
        return BC_MSUBTYPE_H264;
    if (codec == AV_CODEC_ID_MPEG2VIDEO && !std::strcmp(demuxer, "mpegvideo"))
        return BC_MSUBTYPE_MPEG2VIDEO;
    if (codec == AV_CODEC_ID_VC1 && !std::strcmp(demuxer, "vc1"))
        return BC_MSUBTYPE_VC1;
    if (codec == AV_CODEC_ID_WMV3 && !std::strcmp(demuxer, "asf"))
        return BC_MSUBTYPE_WMV3;
    return BC_MSUBTYPE_INVALID;
}

static unsigned H263Bits(const uint8_t *data, unsigned offset, unsigned count)
{
    unsigned value = 0;
    for (unsigned bit = offset; bit < offset + count; ++bit)
        value = (value << 1) | ((data[bit / 8] >> (7 - bit % 8)) & 1);
    return value;
}

// Only the first 50 bits are inspected, after checking the seven-byte prefix.
// This checks the curated baseline picture header, not decoder conformance.
static bool BaselineH263Picture(const uint8_t *data, size_t size,
                                unsigned width, unsigned height)
{
    static const unsigned dimensions[][2] = {
        {128, 96}, {176, 144}, {352, 288}, {704, 576},
    };
    if (!data || size < 7 || H263Bits(data, 0, 22) != 0x20 ||
        H263Bits(data, 30, 1) != 1 || H263Bits(data, 31, 4) != 0 ||
        H263Bits(data, 39, 4) != 0 || H263Bits(data, 43, 5) == 0 ||
        H263Bits(data, 48, 2) != 0)
        return false;
    const unsigned source_format = H263Bits(data, 35, 3);
    return source_format >= 1 && source_format <= 4 &&
           width == dimensions[source_format - 1][0] &&
           height == dimensions[source_format - 1][1];
}

// Conditional full-frame firmware expectation, not a general scaling oracle.
// Width zero selects an unscaled control; reject upscaling and ambiguous edges.
static bool ScalerGeometry(unsigned source_width, unsigned source_height,
                           unsigned scale_width, unsigned *width, unsigned *height)
{
    if (!source_width || source_width > 1920 || !source_height ||
        source_height > 1088 || (source_width & 1) || (source_height & 1) ||
        (scale_width && (scale_width < 128 || scale_width > 1918 ||
                         (scale_width & 1) || scale_width > source_width)))
        return false;
    *width = scale_width ? scale_width : source_width;
    *height = source_height * *width / source_width;
    *height += *height & 1;
    return *height != 0;
}

static const char *PackedName(BC_OUTPUT_FORMAT format)
{
    if (format == OUTPUT_MODE422_YUY2) return "YUY2";
    if (format == OUTPUT_MODE422_UYVY) return "UYVY";
    return nullptr;
}

static bool HashActivePixels(GChecksum *checksum, const BC_DTS_PROC_OUT &output,
                             unsigned width, unsigned height,
                             BC_OUTPUT_FORMAT format = OUTPUT_MODE422_YUY2)
{
    const uint64_t bytes = static_cast<uint64_t>(width) * height * 2;
    if (!checksum || !width || width > 1920 || (width & 1) || !height ||
        height > 1088 || !output.Ybuff || !PackedName(format) || output.b422Mode != format ||
        output.PicInfo.width != width || output.PicInfo.height != height ||
        static_cast<uint64_t>(output.YBuffDoneSz) * 4 < bytes)
        return false;
    // BCM70015 packed 4:2:2 has width*2 stride; do not relabel or convert bytes.
    // Only active rows are hashed, while the successful NoCopy lease is owned.
    g_checksum_update(checksum, output.Ybuff, static_cast<gssize>(bytes));
    return true;
}

static bool CaptureBudget(unsigned width, unsigned height, unsigned expected,
                          uint64_t *frame_bytes, uint64_t *total_bytes)
{
    if (!frame_bytes || !total_bytes || !width || width > 1920 || (width & 1) || !height || height > 1088 ||
        (height & 1) || !expected || expected > kMaximumPackets)
        return false;
    *frame_bytes = static_cast<uint64_t>(width) * height * 2;
    *total_bytes = *frame_bytes * expected;
    return *total_bytes <= kMaximumCaptureBytes;
}

struct CapturedFrame {
    std::vector<uint8_t> pixels;
    BC_OUTPUT_FORMAT output_format = OUTPUT_MODE422_YUY2;
    uint64_t token = 0;
    uint32_t picture_number = 0, width = 0, height = 0, flags = 0;
    uint32_t chroma_format = 0, output_flags = 0, aspect_ratio = 0, colour_primaries = 0;
};

// This copies only a validated host output lease. No pointer survives release.
// The reported format is not an independent hardware byte-order oracle.
static bool CopyCapturedPixels(const BC_DTS_PROC_OUT &output, unsigned width,
                               unsigned height, uint64_t limit, CapturedFrame *frame,
                               BC_OUTPUT_FORMAT format = OUTPUT_MODE422_YUY2)
{
    uint64_t bytes = 0, total = 0;
    if (!frame || !CaptureBudget(width, height, 1, &bytes, &total) || bytes > limit ||
        !output.Ybuff || !PackedName(format) || output.b422Mode != format ||
        !(output.PoutFlags & BC_POUT_FLAGS_PIB_VALID) ||
        (output.PoutFlags & BC_POUT_FLAGS_ENCRYPTED) ||
        (output.PicInfo.flags & (VDEC_FLAG_INTERLACED_SRC | VDEC_FLAG_EOS)) ||
        output.PicInfo.width != width || output.PicInfo.height != height ||
        static_cast<uint64_t>(output.YBuffDoneSz) * 4 < bytes)
        return false;
    try {
        frame->pixels.assign(output.Ybuff, output.Ybuff + static_cast<size_t>(bytes));
    } catch (...) {
        return false;
    }
    frame->token = output.PicInfo.timeStamp;
    frame->output_format = format;
    frame->picture_number = output.PicInfo.picture_number;
    frame->width = width;
    frame->height = height;
    frame->flags = output.PicInfo.flags;
    frame->chroma_format = output.PicInfo.chroma_format;
    frame->output_flags = output.PoutFlags;
    frame->aspect_ratio = output.PicInfo.aspect_ratio;
    frame->colour_primaries = output.PicInfo.colour_primaries;
    return true;
}

struct PixelCapture {
    FILE *file = nullptr;
    const char *path = nullptr;
    unsigned expected = 0, frames = 0;
    uint64_t frame_bytes = 0, total_bytes = 0, written = 0;
    BC_OUTPUT_FORMAT output_format = OUTPUT_MODE422_YUY2;
    bool report;
    explicit PixelCapture(bool report_values = true) : report(report_values) {}
    PixelCapture(const PixelCapture &) = delete;
    PixelCapture &operator=(const PixelCapture &) = delete;
    ~PixelCapture() { if (file) std::fclose(file); }
    bool Open(const char *destination, unsigned width, unsigned height, unsigned count,
              BC_OUTPUT_FORMAT format = OUTPUT_MODE422_YUY2) {
        if (file || path || !PackedName(format)) return false;
        output_format = format;
        if (!destination) return true;
        if (!CaptureBudget(width, height, count, &frame_bytes, &total_bytes)) {
            if (report) std::fprintf(stderr, "%s capture exceeds geometry/frame/256-MiB bounds\n", PackedName(output_format));
            return false;
        }
        const int fd = open(destination, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (fd < 0) {
            if (report) std::fprintf(stderr, "Cannot exclusively create %s capture '%s': %s\n",
                         PackedName(output_format), destination, std::strerror(errno));
            return false;
        }
        path = destination;
        expected = count;
        file = fdopen(fd, "wb");
        if (!file) {
            close(fd);
            if (report) std::fprintf(stderr, "Failed %s capture retained at '%s' (empty)\n", PackedName(output_format), path);
            return false;
        }
        return true;
    }
    bool Write(const CapturedFrame &frame) {
        if (!file || frames >= expected || frame.output_format != output_format || frame.pixels.size() != frame_bytes ||
            written > total_bytes || frame_bytes > total_bytes - written)
            return false;
        const size_t bytes = std::fwrite(frame.pixels.data(), 1, frame.pixels.size(), file);
        written += bytes;
        if (bytes != frame.pixels.size()) return false;
        if (report) std::printf("Captured packed output: requested-format=%s frame-index=%u token=%llu picture-number=%u "
                    "geometry=%ux%u flags=%x chroma-format=%x output-flags=%x "
                    "aspect-ratio=%u colour-primaries=%u\n", PackedName(output_format), frames,
                    static_cast<unsigned long long>(frame.token), frame.picture_number,
                    frame.width, frame.height, frame.flags, frame.chroma_format,
                    frame.output_flags, frame.aspect_ratio, frame.colour_primaries);
        ++frames;
        return true;
    }
    bool Finish(bool completed) {
        if (!path) return true;
        bool closed = false;
        if (file) {
            closed = std::fclose(file) == 0;
            file = nullptr;
        }
        const bool ok = completed && closed && frames == expected && written == total_bytes;
        if (report) std::printf("Packed capture: requested-format=%s frames=%u/%u bytes=%llu/%llu transport-result=%s\n",
                    PackedName(output_format), frames, expected, static_cast<unsigned long long>(written),
                    static_cast<unsigned long long>(total_bytes), ok ? "PASS" : "FAIL");
        if (!ok && report)
            std::fprintf(stderr, "Failed/partial %s capture retained at '%s'; do not use as evidence\n", PackedName(output_format), path);
        return ok;
    }
};

struct Resources {
    unsigned long rss_kib = 0;
    unsigned fds = 0;
    unsigned threads = 0;
};

static bool StatusValue(const char *line, const char *name, unsigned long *value)
{
    const size_t length = std::strlen(name);
    if (std::strncmp(line, name, length) || line[length] != ':') return false;
    const char *number = line + length + 1;
    while (*number == ' ' || *number == '\t') ++number;
    if (!*number || *number == '-' || *number == '+') return false;
    char *end = nullptr;
    errno = 0;
    const unsigned long parsed = std::strtoul(number, &end, 10);
    if (errno || end == number) return false;
    *value = parsed;
    return true;
}

static bool SampleResources(Resources *resources)
{
    FILE *status = std::fopen("/proc/self/status", "r");
    if (!status) return false;
    bool have_rss = false, have_threads = false;
    char line[256];
    while (std::fgets(line, sizeof(line), status)) {
        unsigned long value = 0;
        if (StatusValue(line, "VmRSS", &value)) {
            resources->rss_kib = value;
            have_rss = true;
        } else if (StatusValue(line, "Threads", &value) && value <= UINT_MAX) {
            resources->threads = static_cast<unsigned>(value);
            have_threads = true;
        }
    }
    const bool status_read_ok = !std::ferror(status);
    const bool status_ok = std::fclose(status) == 0 && status_read_ok;

    DIR *directory = opendir("/proc/self/fd");
    if (!directory) return false;
    const int scan_fd = dirfd(directory);
    unsigned fds = 0;
    errno = 0;
    while (const struct dirent *entry = readdir(directory)) {
        if (entry->d_name[0] == '.') continue;
        char *end = nullptr;
        const long fd = std::strtol(entry->d_name, &end, 10);
        if (!end || *end || fd < 0 || fd == scan_fd) continue;
        ++fds;
    }
    const bool directory_read_ok = errno == 0;
    const bool directory_ok = closedir(directory) == 0 && directory_read_ok;
    resources->fds = fds;
    return status_ok && directory_ok && have_rss && have_threads;
}

static void ReportResources(unsigned iteration, unsigned iterations,
                            const Resources &resources)
{
    std::printf("Resources: iteration=%u/%u rss-kib=%lu fds=%u threads=%u\n",
                iteration, iterations, resources.rss_kib, resources.fds,
                resources.threads);
    std::fflush(stdout);
}

static uint64_t Token(unsigned generation, size_t packet)
{
    // Each generation owns 10,001 timestamp slots. The input cap is 10,000,
    // so an output left behind by an earlier session cannot match this one.
    return (static_cast<uint64_t>(generation) * (kMaximumPackets + 1) +
            packet + 1) * kTokenStep;
}

static bool BoundedRssGrowth(const std::vector<unsigned long> &samples,
                             unsigned long limit, unsigned long *early,
                             unsigned long *late)
{
    if (samples.empty()) return false;
    size_t window = samples.size() / 10;
    if (window < 3) window = 3;
    size_t warmup = samples.size() / 10;
    if (warmup + window > samples.size()) {
        warmup = 0;
        window = samples.size();
    }
    unsigned long long early_sum = 0, late_sum = 0;
    for (size_t index = warmup; index < warmup + window; ++index)
        early_sum += samples[index];
    for (size_t index = samples.size() - window; index < samples.size(); ++index)
        late_sum += samples[index];
    *early = static_cast<unsigned long>(early_sum / window);
    *late = static_cast<unsigned long>(late_sum / window);
    return samples.size() < 6 || *late <= *early + limit;
}

static bool FreshBeforeFlush(bool observed_eos)
{
    return !observed_eos;
}

static bool SelfTest()
{
    bool ok = true;
    unsigned checks = 0;
    const auto check = [&](bool condition, const char *description) {
        ++checks;
        if (!condition) {
            std::fprintf(stderr, "Self-test failed: %s\n", description);
            ok = false;
        }
    };

    unsigned number = 0;
    check(Number("1", 1, &number) && number == 1, "minimum number");
    check(Number("1000", 1000, &number) && number == 1000, "maximum number");
    check(!Number("0", 1000, &number) && !Number("1001", 1000, &number) &&
          !Number("-1", 1000, &number) && !Number("1x", 1000, &number),
          "invalid numbers");

    Options options;
    const uint32_t legacy_mode = ProbeDeviceMode(options);
    check((legacy_mode & DTS_SINGLE_THREADED_MODE) != 0,
          "ordinary probe retains its single-thread mode");
    Options native_mode;
    native_mode.scaler_test = true;
    check(ProbeDeviceMode(native_mode) == (legacy_mode & ~DTS_SINGLE_THREADED_MODE),
          "native-width control disables only the scaling preset mode");
    native_mode.scale_width = 320;
    check(ProbeDeviceMode(native_mode) == legacy_mode,
          "explicit scaled control retains mode and width override");
    check(ParseArguments({"probe", "--hardware", "fixture", "12"}, &options) &&
          options.mode == Mode::Hardware && options.expected == 12 &&
          options.seconds == 30 && options.iterations == 1 &&
          options.output_format == OUTPUT_MODE422_YUY2 && !options.observe_chroma &&
          !options.observe_scl_config,
          "legacy hardware arguments");
    options = Options{};
    check(ParseArguments({"probe", "--hardware", "fixture", "12", "9", "1000"},
                         &options) && options.seconds == 9 && options.iterations == 1000,
          "churn arguments");
    options = Options{};
    check(!ParseArguments({"probe", "--hardware", "fixture", "12", "9", "1001"},
                          &options) &&
          !ParseArguments({"probe", "--preflight", "fixture", "12", "9", "2"},
                          &options),
          "iteration argument bounds");

    for (const char *width : {"0", "128", "320", "640", "1918"}) {
        options = Options{};
        check(ParseArguments({"probe", "--hardware", "fixture", "12", "9", "2",
                              "--scaler-test", width}, &options) &&
              options.scaler_test && options.iterations == 2,
              "opt-in scaler arguments");
    }
    for (const char *width : {"", "127", "319", "1919", "1920", "1921", "-1",
                             "+128", " 128", "128x", "4294967296"}) {
        options = Options{};
        check(!ParseArguments({"probe", "--hardware", "fixture", "12",
                               "--scaler-test", width}, &options),
              "invalid scaler width rejected before hardware");
    }
    options = Options{};
    check(ParseArguments({"probe", "--preflight", "fixture", "12", "--scaler-test", "320"},
                         &options) && options.mode == Mode::Preflight,
          "device-free scaler preflight");
    options = Options{};
    check(!ParseArguments({"probe", "--self-test", "--scaler-test", "320"}, &options),
          "self-test does not accept hardware options");
    for (const char *capture : {"--capture-yuy2", "--capture-uyvy"}) {
        for (const char *width : {"0", "320", "640"}) {
            options = Options{};
            check(ParseArguments({"probe", "--hardware", "fixture", "12", "9", "1",
                                  "--scaler-test", width, capture, "owned.raw"}, &options) &&
                  options.mode == Mode::Hardware && options.scaler_test &&
                  options.iterations == 1 && options.capture_path &&
                  options.output_format == (!std::strcmp(capture, "--capture-uyvy")
                      ? OUTPUT_MODE422_UYVY : OUTPUT_MODE422_YUY2) &&
                  !std::strcmp(options.capture_path, "owned.raw"),
                  "single-run scaler capture is an explicit trailing option");
        }
    }
    options = Options{};
    check(ParseArguments({"probe", "--hardware", "fixture", "12", "--scaler-test", "320",
                          "--capture-yuy2", "path with spaces.raw"}, &options),
          "capture path is one argument without shell interpretation");
    for (const std::vector<const char *> &invalid : {
             std::vector<const char *>{"probe", "--self-test", "--capture-yuy2", "owned.raw"},
             {"probe", "--preflight", "fixture", "12", "--scaler-test", "320", "--capture-yuy2", "owned.raw"},
             {"probe", "--hardware", "fixture", "12", "--capture-yuy2", "owned.raw"},
             {"probe", "--hardware", "fixture", "12", "9", "2", "--scaler-test", "320", "--capture-yuy2", "owned.raw"},
             {"probe", "--hardware", "fixture", "12", "--h263-via-divx", "--open-only", "--capture-yuy2", "owned.raw"},
             {"probe", "--hardware", "fixture", "12", "--scaler-test", "320", "--h263-via-divx", "--open-only", "--capture-yuy2", "owned.raw"},
             {"probe", "--hardware", "fixture", "12", "--scaler-test", "320", "--capture-yuy2", ""},
             {"probe", "--hardware", "fixture", "12", "--scaler-test", "320", "--capture-yuy2", "-"},
             {"probe", "--hardware", "fixture", "12", "--scaler-test", "320", "--capture-yuy2"},
             {"probe", "--hardware", "fixture", "12", "--capture-yuy2", "owned.raw", "--scaler-test", "320"},
             {"probe", "--hardware", "fixture", "12", "--scaler-test", "320", "--capture-yuy2", "a.raw", "--capture-yuy2", "b.raw"}}) {
        for (const char *capture : {"--capture-yuy2", "--capture-uyvy"}) {
            std::vector<const char *> variant = invalid;
            for (const char *&argument : variant)
                if (!std::strcmp(argument, "--capture-yuy2")) argument = capture;
            options = Options{};
            check(!ParseArguments(variant, &options), "invalid capture mode/count/path/placement rejected");
        }
    }
    options = Options{};
    check(!ParseArguments({"probe", "--hardware", "fixture", "12", "--scaler-test", "320",
                          "--capture-yuy2", "a.raw", "--capture-uyvy", "b.raw"}, &options),
          "mixed capture formats are rejected rather than silently overriding a request");
    for (const char *capture : {"--capture-yuy2", "--capture-uyvy"}) {
        options = Options{};
        check(ParseArguments({"probe", "--hardware", "fixture", "30", "--scaler-test", "0",
                              "--observe-chroma", capture, "owned.raw"}, &options) &&
              options.observe_chroma && options.capture_path && options.scale_width == 0,
              "chroma observation requires an explicit native capture");
        options = Options{};
        check(ParseArguments({"probe", "--hardware", "fixture", "30", "--scaler-test", "0",
                              "--mpeg1-via-mpeg2", "--observe-chroma", capture, "owned.raw"}, &options) &&
              options.observe_chroma && options.mpeg1_via_mpeg2,
              "chroma observation preserves the MPEG-1 research gate");
    }
    for (const std::vector<const char *> &invalid : {
             std::vector<const char *>{"probe", "--self-test", "--observe-chroma"},
             {"probe", "--hardware", "fixture", "30", "--observe-chroma"},
             {"probe", "--hardware", "fixture", "30", "--scaler-test", "0", "--observe-chroma"},
             {"probe", "--preflight", "fixture", "30", "--scaler-test", "0", "--observe-chroma", "--capture-yuy2", "owned.raw"},
             {"probe", "--hardware", "fixture", "30", "--scaler-test", "320", "--observe-chroma", "--capture-yuy2", "owned.raw"},
             {"probe", "--hardware", "fixture", "30", "9", "2", "--scaler-test", "0", "--observe-chroma", "--capture-yuy2", "owned.raw"},
             {"probe", "--hardware", "fixture", "30", "--scaler-test", "0", "--observe-chroma", "--observe-chroma", "--capture-yuy2", "owned.raw"},
             {"probe", "--hardware", "fixture", "30", "--scaler-test", "0", "--observe-chroma", "--mpeg1-via-mpeg2", "--capture-yuy2", "owned.raw"},
             {"probe", "--hardware", "fixture", "30", "--scaler-test", "0", "--capture-yuy2", "owned.raw", "--observe-chroma"}}) {
        options = Options{};
        check(!ParseArguments(invalid, &options), "invalid chroma observation scope or placement");
    }
    for (uint32_t raw : {0U, 0xffffffffU, 0x10U, 0x8U, 1U}) {
        ChromaReadFixture fixture;
        fixture.values[0] = raw;
        fixture.values[1] = ~raw;
        ChromaConfiguration config;
        check(ReadChromaConfiguration(&fixture, &config, ChromaReadFixture::Read) == BC_STS_SUCCESS &&
              fixture.calls == 2 && fixture.addresses_valid && config.lac == raw &&
              config.sampling == ~raw, "two fixed reads preserve every raw bit pattern");
    }
    for (int raw = -1; raw <= BC_STS_PWR_MGMT; ++raw) {
        if (raw == BC_STS_SUCCESS) continue;
        for (unsigned failed_read : {0U, 1U}) {
            ChromaReadFixture fixture;
            fixture.status[failed_read] = static_cast<BC_STATUS>(raw);
            ChromaConfiguration config;
            check(ReadChromaConfiguration(&fixture, &config, ChromaReadFixture::Read) == raw &&
                  fixture.calls == failed_read + 1 && fixture.addresses_valid,
                  "chroma read failure stops without retries or another register");
        }
    }
    for (const char *width : {"0", "320", "640"}) {
        options = Options{};
        check(ParseArguments({"probe", "--hardware", "fixture", "180", "30", "1",
            "--scaler-test", width, "--observe-scl-config", "--capture-yuy2", "owned.raw"},
            &options) && options.observe_scl_config && options.iterations == 1 &&
            options.output_format == OUTPUT_MODE422_YUY2,
            "SCL raw configuration admits only explicit native YUY2 scaler captures");
    }
    for (const std::vector<const char *> &invalid : {
        std::vector<const char *>{"probe", "--self-test", "--observe-scl-config"},
        {"probe", "--preflight", "fixture", "180", "--scaler-test", "320", "--observe-scl-config", "--capture-yuy2", "owned.raw"},
        {"probe", "--hardware", "fixture", "180", "--observe-scl-config", "--capture-yuy2", "owned.raw"},
        {"probe", "--hardware", "fixture", "180", "--scaler-test", "320", "--observe-scl-config"},
        {"probe", "--hardware", "fixture", "180", "30", "2", "--scaler-test", "320", "--observe-scl-config", "--capture-yuy2", "owned.raw"},
        {"probe", "--hardware", "fixture", "180", "--scaler-test", "128", "--observe-scl-config", "--capture-yuy2", "owned.raw"},
        {"probe", "--hardware", "fixture", "180", "--scaler-test", "320", "--observe-scl-config", "--capture-uyvy", "owned.raw"},
        {"probe", "--hardware", "fixture", "180", "--scaler-test", "320", "--mpeg1-via-mpeg2", "--observe-scl-config", "--capture-yuy2", "owned.raw"},
        {"probe", "--hardware", "fixture", "180", "--h263-via-divx", "--open-only", "--observe-scl-config", "--capture-yuy2", "owned.raw"},
        {"probe", "--hardware", "fixture", "180", "--scaler-test", "320", "--h263-via-divx", "--observe-scl-config", "--capture-yuy2", "owned.raw"},
        {"probe", "--hardware", "fixture", "180", "--scaler-test", "0", "--observe-chroma", "--observe-scl-config", "--capture-yuy2", "owned.raw"},
        {"probe", "--hardware", "fixture", "180", "--scaler-test", "0", "--observe-scl-config", "--observe-chroma", "--capture-yuy2", "owned.raw"},
        {"probe", "--hardware", "fixture", "180", "--scaler-test", "320", "--observe-scl-config", "--observe-scl-config", "--capture-yuy2", "owned.raw"},
        {"probe", "--hardware", "fixture", "180", "--observe-scl-config", "--scaler-test", "320", "--capture-yuy2", "owned.raw"},
        {"probe", "--hardware", "fixture", "180", "--scaler-test", "320", "--capture-yuy2", "owned.raw", "--observe-scl-config"},
        {"probe", "--hardware", "fixture", "180", "--scaler-test", "320", "--observe-scl"}}) {
        options = Options{};
        check(!ParseArguments(invalid, &options), "SCL observer rejects mixed/default/alternate/misordered options");
    }
    {
        Input native;
        native.codec = AV_CODEC_ID_MPEG2VIDEO;
        native.subtype = BC_MSUBTYPE_MPEG2VIDEO;
        native.progressive = true;
        native.width = 640; native.height = 360; native.packets.resize(180);
        Options admitted;
        admitted.observe_scl_config = true; admitted.expected = 180;
        check(SclInputAdmitted(admitted, native), "exact native progressive fixture shape admitted");
        for (unsigned fault = 0; fault < 7; ++fault) {
            Input changed = native;
            Options altered = admitted;
            if (fault == 0) changed.codec = AV_CODEC_ID_MPEG1VIDEO;
            if (fault == 1) changed.subtype = BC_MSUBTYPE_DIVX;
            if (fault == 2) changed.progressive = false;
            if (fault == 3) changed.width = 638;
            if (fault == 4) changed.height = 358;
            if (fault == 5) changed.packets.pop_back();
            if (fault == 6) altered.expected = 179;
            check(!SclInputAdmitted(altered, changed) && SclInputAdmitted(Options{}, changed),
                "SCL-specific input admission preserves legacy input predicates");
        }
    }
    {
        SclReadFixture fixture;
        SclSnapshot snapshot;
        check(ReadSclConfiguration(&fixture, &snapshot, SclReadFixture::Read) &&
            fixture.addresses_valid && fixture.calls == 16 && snapshot.reads == 16 &&
            snapshot.measured == 16 && snapshot.Stable(), "two independent fixed REV-bracketed bundles");
        for (unsigned field = 1; field <= 5; ++field) {
            SclReadFixture changing;
            changing.raw[8 + field] ^= field == 1 ? 2 : 1;
            check(ReadSclConfiguration(&changing, &snapshot, SclReadFixture::Read) &&
                changing.calls == 16 && !snapshot.Stable() &&
                snapshot.raw[1][field] == changing.raw[8 + field],
                "normal duplicate raw differences are recorded, not rejected or normalized");
        }
        check(!ReadSclConfiguration(&fixture, nullptr, SclReadFixture::Read) && fixture.calls == 16,
            "null snapshot refuses before reads");
        check(!ReadSclConfiguration(&fixture, &snapshot, nullptr) && fixture.calls == 16 &&
            snapshot.failure == SclFailure::Argument && snapshot.reads == 0,
            "null read callback refuses before reads");
    }
    for (unsigned failed_read = 0; failed_read < 16; ++failed_read) {
        for (int status = -1; status <= BC_STS_PWR_MGMT; ++status) {
            if (status == BC_STS_SUCCESS) continue;
            SclReadFixture fixture;
            fixture.fail_at = failed_read; fixture.status = static_cast<BC_STATUS>(status);
            struct { uint32_t before = 0x12345678; SclSnapshot value; uint32_t after = 0x87654321; } guarded;
            check(!ReadSclConfiguration(&fixture, &guarded.value, SclReadFixture::Read) &&
                fixture.calls == failed_read + 1 && fixture.addresses_valid &&
                guarded.value.reads == failed_read + 1 && guarded.value.measured == failed_read &&
                guarded.value.status == status && guarded.value.failure == SclFailure::Read &&
                guarded.value.raw[failed_read / 8][failed_read % 8] == 0 &&
                guarded.before == 0x12345678 && guarded.after == 0x87654321,
                "every read status stops exactly once without publishing failed read data or crossing canaries");
            fixture.calls = 0;
            SclObserver observer; observer.enabled = true;
            check(!observer.Observe(&fixture, SclStage::FirstReleased, SclReadFixture::Read, false) &&
                observer.failed && observer.reads == failed_read + 1 &&
                !observer.Observe(&fixture, SclStage::LastReleased, SclReadFixture::Read, false) &&
                fixture.calls == failed_read + 1, "failed observer latches and permits no later diagnostic reads");
        }
    }
    const uint32_t scl_allowed[8] = {0xffff, 0xe, 1, 0x07ff07ff, 0x07ff07ff, 0x07ff07ff, 0xff, 0xffff};
    for (unsigned position = 0; position < 16; ++position) {
        const unsigned field = position % 8;
        for (unsigned bit = 0; bit < 32; ++bit) {
            if (scl_allowed[field] & (1U << bit)) continue;
            SclReadFixture fixture;
            fixture.raw[position] |= 1U << bit;
            SclSnapshot snapshot;
            check(!ReadSclConfiguration(&fixture, &snapshot, SclReadFixture::Read) &&
                fixture.calls == position + 1 && snapshot.measured == position + 1 &&
                snapshot.raw[position / 8][field] == fixture.raw[position],
                "all reserved bits fail immediately but retain the measured invalid raw word");
        }
        if (field == 0 || field == 7) {
            for (uint32_t raw : {0U, 1U, 0x50U, 0x81U, 0xffffU, 0xffffffffU}) {
                SclReadFixture fixture; fixture.raw[position] = raw;
                SclSnapshot snapshot;
                check(!ReadSclConfiguration(&fixture, &snapshot, SclReadFixture::Read) &&
                    snapshot.failure == SclFailure::Revision && fixture.calls == position + 1,
                    "each opening/closing revision must equal the qualified 0x80 raw value");
            }
        } else if (field == 6) {
            for (unsigned bit = 0; bit < 8; ++bit) {
                SclReadFixture fixture; fixture.raw[position] = 1U << bit;
                SclSnapshot snapshot;
                check(!ReadSclConfiguration(&fixture, &snapshot, SclReadFixture::Read) &&
                    snapshot.failure == SclFailure::EngineStatus && fixture.calls == position + 1,
                    "each defined observed status bit stops without clear or followup reads");
            }
        } else {
            SclReadFixture fixture; fixture.raw[position] = scl_allowed[field];
            SclSnapshot snapshot;
            check(ReadSclConfiguration(&fixture, &snapshot, SclReadFixture::Read) && fixture.calls == 16,
                "maximum documented field bits accepted without interpreting geometry/enable semantics");
        }
    }
    {
        SclObserver observer;
        SclReadFixture fixture;
        check(observer.Observe(&fixture, SclStage::PreStart, SclReadFixture::Read, false) &&
            fixture.calls == 0 && observer.reads == 0 && observer.attempted == 0,
            "default-off observer has zero additional read calls");
        observer.enabled = true;
        for (SclStage stage : {SclStage::PreStart, SclStage::FormatChange, SclStage::FirstReleased,
                              SclStage::LastReleased, SclStage::EosBarrier}) {
            fixture = SclReadFixture{};
            check(observer.Observe(&fixture, stage, SclReadFixture::Read, false) && fixture.calls == 16 &&
                !observer.Observe(&fixture, stage, SclReadFixture::Read, false) && fixture.calls == 16,
                "five labelled stages each have one fixed bounded attempt");
        }
        check(observer.reads == 80 && observer.attempted == 31 && !observer.failed,
            "maximum five-stage budget is exactly eighty additional API read calls");
    }
    options = Options{};
    check(ParseArguments({"probe", "--preflight", "fixture", "30",
                          "--mpeg1-via-mpeg2"}, &options) &&
          options.mode == Mode::Preflight && options.mpeg1_via_mpeg2,
          "device-free MPEG-1 research preflight");
    options = Options{};
    check(ParseArguments({"probe", "--hardware", "fixture", "30", "10", "2",
                          "--scaler-test", "0", "--mpeg1-via-mpeg2"}, &options) &&
          options.mode == Mode::Hardware && options.mpeg1_via_mpeg2 &&
          options.scaler_test && options.scale_width == 0 && options.iterations == 2,
          "explicit MPEG-1 research and pixel-hash arguments");
    for (const std::vector<const char *> &invalid : {
             std::vector<const char *>{"probe", "--self-test", "--mpeg1-via-mpeg2"},
             {"probe", "--hardware", "fixture", "30", "--mpeg1-via-mpeg2", "--mpeg1-via-mpeg2"},
             {"probe", "--hardware", "fixture", "30", "--mpeg1-via-mpeg2", "--scaler-test", "0"}}) {
        options = Options{};
        check(!ParseArguments(invalid, &options), "invalid research option placement");
    }
    check(InputSubtype(AV_CODEC_ID_MPEG1VIDEO, "mpegvideo", true) == BC_MSUBTYPE_MPEG2VIDEO &&
          InputSubtype(AV_CODEC_ID_MPEG1VIDEO, "mpegvideo", false) == BC_MSUBTYPE_INVALID &&
          InputSubtype(AV_CODEC_ID_MPEG1VIDEO, "mpeg", true) == BC_MSUBTYPE_INVALID &&
          InputSubtype(AV_CODEC_ID_MPEG1VIDEO, nullptr, true) == BC_MSUBTYPE_INVALID,
          "MPEG-1 probe is explicit and elementary-stream-only");
    for (const std::pair<AVCodecID, const char *> &native : {
             std::pair<AVCodecID, const char *>{AV_CODEC_ID_H264, "h264"},
             {AV_CODEC_ID_MPEG2VIDEO, "mpegvideo"}, {AV_CODEC_ID_VC1, "vc1"},
             {AV_CODEC_ID_WMV3, "asf"}})
        check(InputSubtype(native.first, native.second, false) != BC_MSUBTYPE_INVALID &&
              InputSubtype(native.first, native.second, true) == BC_MSUBTYPE_INVALID,
              "native admission unchanged and excluded from MPEG-1 probe");
    check(InputSubtype(AV_CODEC_ID_MPEG2VIDEO, "mpegvideo", false) == BC_MSUBTYPE_MPEG2VIDEO &&
          InputSubtype(AV_CODEC_ID_H264, "mpegvideo", false) == BC_MSUBTYPE_INVALID &&
          InputSubtype(AV_CODEC_ID_HEVC, "hevc", true) == BC_MSUBTYPE_INVALID &&
          InputSubtype(AV_CODEC_ID_HEVC, "hevc", false) == BC_MSUBTYPE_INVALID,
          "research option does not admit another decoder protocol");
    for (const std::vector<const char *> &valid : {
             std::vector<const char *>{"probe", "--preflight", "fixture", "30", "--h263-via-divx"},
             {"probe", "--hardware", "fixture", "30", "--h263-via-divx"},
             {"probe", "--hardware", "fixture", "30", "9", "2", "--h263-via-divx"},
             {"probe", "--hardware", "fixture", "30", "--scaler-test", "0", "--h263-via-divx"}}) {
        options = Options{};
        check(ParseArguments(valid, &options) && options.h263_via_divx &&
              !options.mpeg1_via_mpeg2 && !options.open_only,
              "explicit H.263 research parser path");
    }
    for (const std::vector<const char *> &valid : {
             std::vector<const char *>{"probe", "--hardware", "fixture", "30", "--h263-via-divx", "--open-only"},
             {"probe", "--hardware", "fixture", "30", "9", "2", "--h263-via-divx", "--open-only"}}) {
        options = Options{};
        check(ParseArguments(valid, &options) && options.mode == Mode::Hardware &&
              options.h263_via_divx && options.open_only && !options.scaler_test,
              "H.263 OPEN-only hardware parser path");
    }
    for (const std::vector<const char *> &invalid : {
             std::vector<const char *>{"probe", "--self-test", "--h263-via-divx"},
             {"probe", "--self-test", "--open-only"},
             {"probe", "--preflight", "fixture", "30", "--h263-via-divx", "--open-only"},
             {"probe", "--hardware", "fixture", "30", "--open-only"},
             {"probe", "--hardware", "fixture", "30", "--mpeg1-via-mpeg2", "--open-only"},
             {"probe", "--hardware", "fixture", "30", "--h263-via-divx", "--mpeg1-via-mpeg2"},
             {"probe", "--hardware", "fixture", "30", "--mpeg1-via-mpeg2", "--h263-via-divx"},
             {"probe", "--hardware", "fixture", "30", "--h263-via-divx", "--h263-via-divx"},
             {"probe", "--hardware", "fixture", "30", "--open-only", "--h263-via-divx"},
             {"probe", "--hardware", "fixture", "30", "--h263-via-divx", "--open-only", "--open-only"},
             {"probe", "--hardware", "fixture", "30", "--h263-via-divx", "--scaler-test", "0"},
             {"probe", "--hardware", "fixture", "30", "--scaler-test", "0", "--h263-via-divx", "--open-only"},
             {"probe", "--hardware", "fixture", "30", "--scaler-test", "128", "--h263-via-divx", "--open-only"}}) {
        options = Options{};
        check(!ParseArguments(invalid, &options), "H.263 option conflict or misplaced OPEN-only");
    }
    check(InputSubtype(AV_CODEC_ID_H263, "h263", false, true, 0) == BC_MSUBTYPE_DIVX &&
          InputSubtype(AV_CODEC_ID_H263, "h263", false) == BC_MSUBTYPE_INVALID &&
          InputSubtype(AV_CODEC_ID_H263, "h263", true, true) == BC_MSUBTYPE_INVALID,
          "H.263 DIVX admission is explicit and excludes MPEG-1 flag");
    for (const char *demuxer : {"avi", "mov,mp4,m4a,3gp,3g2,mj2", "mpeg", "h263p", ""})
        check(InputSubtype(AV_CODEC_ID_H263, demuxer, false, true) == BC_MSUBTYPE_INVALID,
              "H.263 containers and alternate demuxers rejected");
    check(InputSubtype(AV_CODEC_ID_H263, nullptr, false, true) == BC_MSUBTYPE_INVALID &&
          InputSubtype(AV_CODEC_ID_H263, "h263", false, true, 1) == BC_MSUBTYPE_INVALID &&
          InputSubtype(AV_CODEC_ID_H263, "h263", false, true, -1) == BC_MSUBTYPE_INVALID,
          "H.263 requires raw demuxer and empty extradata");
    for (AVCodecID codec : {AV_CODEC_ID_H263P, AV_CODEC_ID_H263I, AV_CODEC_ID_MPEG4,
                           AV_CODEC_ID_H264, AV_CODEC_ID_MPEG1VIDEO,
                           AV_CODEC_ID_MPEG2VIDEO, AV_CODEC_ID_VC1,
                           AV_CODEC_ID_WMV3, AV_CODEC_ID_HEVC, AV_CODEC_ID_NONE})
        check(InputSubtype(codec, "h263", false, true) == BC_MSUBTYPE_INVALID,
              "H.263 research flag rejects every other tested codec");

    // Synthetic headers for admission tests only; no coded picture is created.
    const auto put_bits = [](std::vector<uint8_t> *data, unsigned offset,
                             unsigned count, unsigned value) {
        for (unsigned bit = 0; bit < count; ++bit) {
            const unsigned position = offset + bit;
            const uint8_t mask = static_cast<uint8_t>(1U << (7 - position % 8));
            (*data)[position / 8] = static_cast<uint8_t>(((*data)[position / 8] & ~mask) |
                (((value >> (count - bit - 1)) & 1U) ? mask : 0));
        }
    };
    const auto h263_header = [&put_bits](unsigned source_format, bool predicted, unsigned quantizer) {
        std::vector<uint8_t> data(7, 0);
        put_bits(&data, 0, 22, 0x20);
        put_bits(&data, 22, 8, 255);
        put_bits(&data, 30, 1, 1);
        put_bits(&data, 35, 3, source_format);
        put_bits(&data, 38, 1, predicted);
        put_bits(&data, 43, 5, quantizer);
        return data;
    };
    static const unsigned h263_dimensions[][2] = {
        {128, 96}, {176, 144}, {352, 288}, {704, 576},
    };
    for (unsigned source_format = 1; source_format <= 4; ++source_format) {
        for (bool predicted : {false, true}) {
            const std::vector<uint8_t> data = h263_header(source_format, predicted, predicted ? 31 : 1);
            check(BaselineH263Picture(data.data(), data.size(),
                                      h263_dimensions[source_format - 1][0],
                                      h263_dimensions[source_format - 1][1]),
                  "standard baseline H.263 I/P picture header");
        }
        const std::vector<uint8_t> data = h263_header(source_format, false, 2);
        check(!BaselineH263Picture(data.data(), data.size(),
                                   h263_dimensions[source_format - 1][0] + 2,
                                   h263_dimensions[source_format - 1][1]) &&
              !BaselineH263Picture(data.data(), data.size(),
                                   h263_dimensions[source_format - 1][0],
                                   h263_dimensions[source_format - 1][1] + 2),
              "H.263 source-format geometry must match demux geometry");
    }
    const std::vector<uint8_t> header = h263_header(2, false, 2);
    check(!BaselineH263Picture(nullptr, 7, 176, 144), "null H.263 header rejected");
    for (size_t size = 0; size < 7; ++size) {
        const std::vector<uint8_t> truncated(header.begin(), header.begin() + size);
        check(!BaselineH263Picture(truncated.data(), truncated.size(), 176, 144),
              "truncated H.263 header rejected before bit access");
    }
    for (unsigned bit : {0U, 21U, 31U, 32U, 33U, 34U, 39U, 40U, 41U, 42U, 48U, 49U}) {
        std::vector<uint8_t> changed = header;
        changed[bit / 8] ^= static_cast<uint8_t>(1U << (7 - bit % 8));
        check(!BaselineH263Picture(changed.data(), changed.size(), 176, 144),
              "wrong PSC, id, or extended H.263 feature bit rejected");
    }
    std::vector<uint8_t> changed = header;
    put_bits(&changed, 30, 1, 0);
    check(!BaselineH263Picture(changed.data(), changed.size(), 176, 144),
          "H.263 marker bit required");
    changed = header;
    put_bits(&changed, 43, 5, 0);
    check(!BaselineH263Picture(changed.data(), changed.size(), 176, 144),
          "H.263 quantizer must be nonzero");
    for (unsigned source_format : {0U, 5U, 6U, 7U}) {
        changed = h263_header(source_format, false, 2);
        check(!BaselineH263Picture(changed.data(), changed.size(), 176, 144),
              "unsupported H.263 source format rejected");
    }
    changed = header;
    changed.insert(changed.end(), {0, 0, 1, 0xb6});
    const std::vector<uint8_t> original = changed;
    check(BaselineH263Picture(changed.data(), changed.size(), 176, 144) && changed == original,
          "H.263 header validation preserves every packet byte");
    unsigned width = 0, height = 0;
    check(ScalerGeometry(640, 360, 0, &width, &height) && width == 640 && height == 360 &&
          ScalerGeometry(640, 360, 640, &width, &height) && width == 640 && height == 360 &&
          ScalerGeometry(640, 360, 320, &width, &height) && width == 320 && height == 180 &&
          ScalerGeometry(640, 362, 320, &width, &height) && height == 182,
          "unscaled identity downscale and odd-height rounding");
    check(!ScalerGeometry(640, 360, 1280, &width, &height) &&
          !ScalerGeometry(0, 360, 320, &width, &height) &&
          !ScalerGeometry(640, 361, 320, &width, &height),
          "ambiguous scaler geometry rejected");
    uint8_t pixels[12] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};
    BC_DTS_PROC_OUT picture = {};
    picture.Ybuff = pixels; picture.YBuffDoneSz = 3; picture.b422Mode = TRUE;
    picture.PicInfo.width = 2; picture.PicInfo.height = 2;
    GChecksum *checksum = g_checksum_new(G_CHECKSUM_SHA256);
    GChecksum *reference = g_checksum_new(G_CHECKSUM_SHA256);
    g_checksum_update(reference, pixels, 8);
    check(HashActivePixels(checksum, picture, 2, 2) &&
          !std::strcmp(g_checksum_get_string(checksum), g_checksum_get_string(reference)),
          "digest covers active rows but not trailing bytes");
    picture.YBuffDoneSz = 1;
    check(!HashActivePixels(checksum, picture, 2, 2), "short lease rejected before read");
    picture.YBuffDoneSz = 3; picture.b422Mode = FALSE;
    check(!HashActivePixels(checksum, picture, 2, 2), "wrong output format rejected");
    picture.b422Mode = TRUE; picture.Ybuff = nullptr;
    check(!HashActivePixels(checksum, picture, 2, 2), "missing pixel lease rejected");
    g_checksum_free(checksum); g_checksum_free(reference);

    uint64_t frame_bytes = 0, total_bytes = 0;
    check(CaptureBudget(640, 360, 180, &frame_bytes, &total_bytes) &&
          frame_bytes == 460800 && total_bytes == 82944000,
          "capture byte budget follows exact active geometry and expected frames");
    check(CaptureBudget(128, 2, kMaximumPackets, &frame_bytes, &total_bytes) &&
          total_bytes == 5120000,
          "maximum frame count remains bounded");
    for (const std::vector<unsigned> &invalid : {
             std::vector<unsigned>{0, 2, 1}, {2, 0, 1}, {3, 2, 1}, {2, 3, 1},
             {1922, 2, 1}, {2, 1090, 1}, {2, 2, 0}, {2, 2, kMaximumPackets + 1},
             {1920, 1088, 65}, {UINT_MAX, UINT_MAX, UINT_MAX}})
        check(!CaptureBudget(invalid[0], invalid[1], invalid[2], &frame_bytes, &total_bytes),
              "capture rejects malformed geometry, frame overflow and disk budget excess");
    picture = BC_DTS_PROC_OUT{};
    picture.Ybuff = pixels; picture.YBuffDoneSz = 3;
    picture.b422Mode = OUTPUT_MODE422_YUY2;
    picture.PoutFlags = BC_POUT_FLAGS_PIB_VALID;
    picture.PicInfo.width = 2; picture.PicInfo.height = 2;
    picture.PicInfo.timeStamp = 12300000; picture.PicInfo.picture_number = 7;
    picture.PicInfo.chroma_format = 0x422;
    picture.PicInfo.aspect_ratio = 1; picture.PicInfo.colour_primaries = 5;
    CapturedFrame frame;
    check(CopyCapturedPixels(picture, 2, 2, 8, &frame) && frame.pixels.size() == 8 &&
          frame.pixels == std::vector<uint8_t>(pixels, pixels + 8) &&
          frame.token == 12300000 && frame.picture_number == 7 &&
          frame.width == 2 && frame.height == 2 && frame.flags == 0 &&
          frame.chroma_format == 0x422 && frame.output_flags == BC_POUT_FLAGS_PIB_VALID &&
          frame.aspect_ratio == 1 && frame.colour_primaries == 5 &&
          frame.output_format == OUTPUT_MODE422_YUY2,
          "capture owns exactly active pixels and frozen value-only metadata");
    pixels[0] = 99; picture.PicInfo.timeStamp = 0; picture.Ybuff = nullptr;
    check(frame.pixels[0] == 0 && frame.token == 12300000,
          "owned capture survives source mutation, pointer retirement and metadata reuse");
    pixels[0] = 0; picture.Ybuff = pixels;
    check(!CopyCapturedPixels(picture, 2, 2, 7, &frame) &&
          !CopyCapturedPixels(picture, 2, 2, 8, nullptr) &&
          !CopyCapturedPixels(picture, 4, 2, 16, &frame),
          "capture validates destination budget and exact source geometry before copying");
    for (unsigned fault = 0; fault < 7; ++fault) {
        BC_DTS_PROC_OUT invalid = picture;
        if (fault == 0) invalid.Ybuff = nullptr;
        if (fault == 1) invalid.YBuffDoneSz = 1;
        if (fault == 2) invalid.b422Mode = OUTPUT_MODE422_UYVY;
        if (fault == 3) invalid.PoutFlags = 0;
        if (fault == 4) invalid.PoutFlags |= BC_POUT_FLAGS_ENCRYPTED;
        if (fault == 5) invalid.PicInfo.flags |= VDEC_FLAG_INTERLACED_SRC;
        if (fault == 6) invalid.PicInfo.flags |= VDEC_FLAG_EOS;
        check(!CopyCapturedPixels(invalid, 2, 2, 8, &frame),
              "capture rejects missing, short, wrong-format, invalid, encrypted, interlaced or EOS data");
    }
    check(!CaptureBudget(2, 2, 1, nullptr, &total_bytes) &&
          !CaptureBudget(2, 2, 1, &frame_bytes, nullptr),
          "capture budget rejects missing result storage");
    CapturedFrame uyvy;
    picture.b422Mode = OUTPUT_MODE422_UYVY;
    check(CopyCapturedPixels(picture, 2, 2, 8, &uyvy, OUTPUT_MODE422_UYVY) &&
          uyvy.pixels == frame.pixels && uyvy.output_format == OUTPUT_MODE422_UYVY,
          "explicit UYVY capture preserves bytes and records its format without conversion");
    check(!CopyCapturedPixels(picture, 2, 2, 8, &uyvy) &&
          !CopyCapturedPixels(picture, 2, 2, 8, &uyvy, OUTPUT_MODE420),
          "UYVY cannot be captured as YUY2 or planar output");
    checksum = g_checksum_new(G_CHECKSUM_SHA256);
    check(HashActivePixels(checksum, picture, 2, 2, OUTPUT_MODE422_UYVY) &&
          !HashActivePixels(checksum, picture, 2, 2) &&
          !HashActivePixels(checksum, picture, 2, 2, OUTPUT_MODE420),
          "pixel hash requires the exact selected packed format");
    g_checksum_free(checksum);
    check(PackedName(OUTPUT_MODE422_YUY2) && PackedName(OUTPUT_MODE422_UYVY) &&
          !PackedName(OUTPUT_MODE420), "only the two packed output formats are admitted");

    // These tests own this private directory and remove only their exact files.
    char capture_directory[] = "/tmp/crystalhd-yuy2-selftest.XXXXXX";
    char *created_directory = mkdtemp(capture_directory);
    check(created_directory != nullptr, "capture filesystem test directory created");
    if (created_directory) {
        const std::string base = std::string(created_directory) + "/";
        const std::string complete_path = base + "complete.raw";
        const std::string uyvy_path = base + "uyvy.raw";
        const std::string short_path = base + "short.raw";
        const std::string oversized_path = base + "oversized.raw";
        const std::string failed_path = base + "failed.raw";
        const std::string destructor_path = base + "destructor.raw";
        const std::string budget_path = base + "budget.raw";
        const std::string alias_path = base + "alias.raw";
        const std::string dangling_path = base + "dangling.raw";
        const std::string missing_path = base + "missing.raw";
        const auto closed_fd = [](int fd) {
            errno = 0;
            return fd >= 0 && fcntl(fd, F_GETFD) == -1 && errno == EBADF;
        };
        const auto exact_file = [&frame](const std::string &path, unsigned count) {
            FILE *read = std::fopen(path.c_str(), "rb");
            if (!read) return false;
            bool valid = true;
            uint8_t data[8];
            for (unsigned index = 0; index < count; ++index)
                valid = std::fread(data, 1, sizeof(data), read) == sizeof(data) &&
                        !std::memcmp(data, frame.pixels.data(), sizeof(data)) && valid;
            valid = std::fgetc(read) == EOF && !std::ferror(read) && valid;
            return std::fclose(read) == 0 && valid;
        };
        PixelCapture complete(false);
        const bool opened = complete.Open(complete_path.c_str(), 2, 2, 2);
        check(opened && complete.file, "exclusive new capture opened");
        if (opened) {
            const int fd = fileno(complete.file);
            struct stat info{};
            check(fstat(fd, &info) == 0 && S_ISREG(info.st_mode) && !(info.st_mode & 0077) &&
                  (fcntl(fd, F_GETFD) & FD_CLOEXEC), "capture is private regular close-on-exec file");
            check(!complete.Open(budget_path.c_str(), 2, 2, 1),
                  "open capture cannot replace its owned descriptor");
            check(complete.Write(frame) && complete.Write(frame) && !complete.Write(frame),
                  "capture enforces exact frame count without appending an excess frame");
            check(complete.Finish(true) && !complete.file && closed_fd(fd),
                  "successful capture closes descriptor before postflight");
            check(exact_file(complete_path, 2), "raw capture contains only exact active frames");
            PixelCapture existing(false);
            check(!existing.Open(complete_path.c_str(), 2, 2, 1) && !existing.file &&
                  exact_file(complete_path, 2), "existing capture is never truncated or overwritten");
            check(symlink("complete.raw", alias_path.c_str()) == 0,
                  "capture symlink refusal fixture created");
            PixelCapture alias(false);
            check(!alias.Open(alias_path.c_str(), 2, 2, 1) && !alias.file &&
                  exact_file(complete_path, 2), "capture refuses symlink without changing its target");
        }
        PixelCapture packed(false);
        const bool packed_opened = packed.Open(uyvy_path.c_str(), 2, 2, 1, OUTPUT_MODE422_UYVY);
        check(packed_opened && packed.output_format == OUTPUT_MODE422_UYVY,
              "UYVY file is explicitly bound to the selected format");
        if (packed_opened) {
            check(!packed.Write(frame) && packed.frames == 0 && packed.written == 0,
                  "mismatched YUY2 frame writes no bytes to UYVY capture");
            check(packed.Write(uyvy) && packed.Finish(true) && exact_file(uyvy_path, 1),
                  "UYVY capture writes its exact owned bytes and closes successfully");
        }
        PixelCapture planar(false);
        struct stat planar_info{};
        check(!planar.Open(budget_path.c_str(), 2, 2, 1, OUTPUT_MODE420) &&
              lstat(budget_path.c_str(), &planar_info) == -1 && errno == ENOENT,
              "unsupported capture format is refused before creating a file");
        check(symlink("missing.raw", dangling_path.c_str()) == 0,
              "dangling capture symlink refusal fixture created");
        PixelCapture dangling(false);
        struct stat info{};
        check(!dangling.Open(dangling_path.c_str(), 2, 2, 1) && !dangling.file &&
              lstat(missing_path.c_str(), &info) == -1 && errno == ENOENT,
              "capture refuses dangling symlink without creating its target");
        PixelCapture budget(false);
        check(!budget.Open(budget_path.c_str(), 1920, 1088, 65) && !budget.file &&
              lstat(budget_path.c_str(), &info) == -1 && errno == ENOENT,
              "over-budget capture refuses before creating any output");
        PixelCapture short_capture(false);
        const bool short_opened = short_capture.Open(short_path.c_str(), 2, 2, 2);
        check(short_opened, "short-count capture fixture opened");
        if (short_opened) {
            const int fd = fileno(short_capture.file);
            check(short_capture.Write(frame) && !short_capture.Finish(true) &&
                  closed_fd(fd) && exact_file(short_path, 1),
                  "short-count capture fails, closes and retains only its actual frame");
        }
        PixelCapture oversized(false);
        const bool oversized_opened = oversized.Open(oversized_path.c_str(), 2, 2, 1);
        check(oversized_opened, "oversized-frame capture fixture opened");
        if (oversized_opened) {
            const int fd = fileno(oversized.file);
            CapturedFrame extra = frame;
            extra.pixels.push_back(99);
            check(!oversized.Write(extra) && !oversized.Finish(false) && closed_fd(fd) &&
                  exact_file(oversized_path, 0), "oversized frame writes nothing and capture closes failed");
        }
        PixelCapture failed(false);
        const bool failed_opened = failed.Open(failed_path.c_str(), 2, 2, 1);
        check(failed_opened, "failed-run capture fixture opened");
        if (failed_opened) {
            const int fd = fileno(failed.file);
            check(failed.Write(frame) && !failed.Finish(false) && closed_fd(fd) &&
                  exact_file(failed_path, 1), "full capture remains failed when its decoder run failed");
        }
        int destructor_fd = -1;
        {
            PixelCapture abandoned(false);
            const bool abandoned_opened = abandoned.Open(destructor_path.c_str(), 2, 2, 1);
            check(abandoned_opened, "abandoned capture fixture opened");
            if (abandoned_opened) destructor_fd = fileno(abandoned.file);
        }
        check(closed_fd(destructor_fd), "capture destructor closes an unfinished descriptor");
        for (const std::string &path : {complete_path, uyvy_path, short_path, oversized_path, failed_path,
                                       destructor_path, alias_path, dangling_path})
            check(unlink(path.c_str()) == 0 || errno == ENOENT, "owned capture fixture removed");
        check(rmdir(created_directory) == 0, "owned capture filesystem test directory removed");
    }

    check(Token(0, 0) == 100000 && Token(0, 9999) < Token(1, 0) &&
          Token(998, 9999) < Token(999, 0), "generation token ranges");
    unsigned long value = 0;
    check(StatusValue("VmRSS:\t 1234 kB\n", "VmRSS", &value) && value == 1234 &&
          StatusValue("Threads:\t7\n", "Threads", &value) && value == 7 &&
          !StatusValue("VmSize:\t8 kB\n", "VmRSS", &value),
          "status parser");
    Resources resources;
    check(SampleResources(&resources) && resources.rss_kib > 0 &&
          resources.fds > 0 && resources.threads > 0, "live resource sample");

    unsigned long early = 0, late = 0;
    check(BoundedRssGrowth({1000, 1000, 1000, 1000, 1000, 1000},
                           kRssGrowthLimitKiB, &early, &late) && early == 1000 &&
          late == 1000, "stable RSS series");
    check(BoundedRssGrowth({1000, 40000, 42000, 41000, 42000, 41500,
                            42000, 41500, 42000, 42000},
                           kRssGrowthLimitKiB, &early, &late),
          "warmup then plateau RSS series");
    check(!BoundedRssGrowth({1000, 50000, 49000, 90000, 89000, 130000,
                             129000, 170000, 169000, 210000},
                            kRssGrowthLimitKiB, &early, &late),
          "sawtooth growing RSS series");
    check(!BoundedRssGrowth({1000, 41000, 81000, 121000, 161000, 201000},
                            kRssGrowthLimitKiB, &early, &late),
          "monotonic growing RSS series");
    check(FreshBeforeFlush(false) && !FreshBeforeFlush(true),
          "pre-flush EOS must belong to the current drain");

    for (unsigned selector : {2U, 3U}) {
        SclViewFixture fixture(selector);
        SclViewProbe view; view.selector = selector;
        check(fixture.Exercise(&view) && fixture.calls == 41 && fixture.valid &&
              fixture.dirty_at_write && fixture.write_calls == 2 && fixture.data_reads == 4 &&
              view.reads == 39 && view.writes == 2 && view.milestones == 3 &&
              view.restored && !view.dirty && !view.failed && !view.access_lost,
              "SCL view exact independent 39R/2W four opaque DATA script for each selector");
        const unsigned completed = fixture.calls;
        check(view.Restore(&fixture, SclViewFixture::Read, SclViewFixture::Write, false) &&
              fixture.calls == completed, "SCL view restoration never retries");
        // Changes in opaque DATA alone are accepted, with no target following.
        for (unsigned data_index : {16U, 21U, 26U, 31U}) {
            SclViewFixture changed(selector); changed.events[data_index].raw = 0x30061000U;
            SclViewProbe subject; subject.selector = selector;
            check(changed.Exercise(&subject) && changed.valid && changed.calls == 41,
                  "SCL view changed opaque DATA never becomes an address or error");
        }
        for (unsigned position = 0; position < 41; ++position) {
            for (int code = -1; code <= BC_STS_PWR_MGMT; ++code) {
                if (code == BC_STS_SUCCESS) continue;
                SclViewFixture failing(selector); failing.fail_at = position;
                failing.status = static_cast<BC_STATUS>(code);
                struct { uint32_t before = 0x12345678; SclViewProbe value;
                         uint32_t after = 0x87654321; } guarded;
                guarded.value.selector = selector;
                check(!failing.Exercise(&guarded.value) && failing.valid &&
                      failing.calls == position + 1 && guarded.value.failed && guarded.value.access_lost &&
                      guarded.value.failure == SclViewFailure::Api && guarded.value.api_status == code &&
                      guarded.before == 0x12345678 && guarded.after == 0x87654321 &&
                      failing.dirty_at_write && guarded.value.reads + guarded.value.writes == failing.calls,
                      "SCL view every API status at every R/W position stops exactly once with canaries");
                const unsigned calls = failing.calls;
                check(!guarded.value.Observe(&failing, 0, SclViewFixture::Read, false) &&
                      !guarded.value.Begin(&failing, SclViewFixture::Read, SclViewFixture::Write, false) &&
                      !guarded.value.Restore(&failing, SclViewFixture::Read, SclViewFixture::Write, false) &&
                      failing.calls == calls && guarded.value.failure == SclViewFailure::Api,
                      "SCL view API/access failure latches all later experiment I/O including blind restore");
            }
        }
        for (unsigned position = 0; position < 41; ++position) {
            SclViewFixture baseline(selector);
            const auto event = baseline.events[position];
            if (event.write || event.address == 0x540884) continue;
            std::vector<uint32_t> invalid;
            if (event.address == 0x540800) invalid = {0U, 0x81U, 0xffffU, 0x10080U, 0xffffffffU};
            else if (event.address == 0x540804) invalid = {0U, 2U, 8U, 1U, 0xffffffffU};
            else if (event.address == 0x540854) invalid = {1U, 2U, 0xffffffffU};
            else if (event.address == 0x540880) {
                for (uint32_t raw : {0U, 1U, 2U, 3U, 4U, 0xffffffffU})
                    if (raw != event.raw) invalid.push_back(raw);
            }
            else invalid = {0x100U, 0xffffffffU};
            for (uint32_t raw : invalid) {
                SclViewFixture rejected(selector); rejected.events[position].raw = raw;
                SclViewProbe subject; subject.selector = selector;
                check(!rejected.Exercise(&subject) && rejected.valid &&
                      rejected.calls == position + 1 && subject.access_lost && subject.failed,
                      "SCL view revision/reserved/selector/admission mutation stops at measured scalar");
                const unsigned calls = rejected.calls;
                check(!subject.Restore(&rejected, SclViewFixture::Read, SclViewFixture::Write, false) &&
                      rejected.calls == calls, "SCL view malformed or changed selector prohibits blind restoration");
            }
        }
        // Known error bits do not certify loss of register access. Only the
        // separate fresh cleanup guard can authorize one restoration attempt.
        for (unsigned position : {4U, 10U, 17U, 22U, 27U, 32U, 36U}) {
            for (unsigned bit = 0; bit < 8; ++bit) {
                SclViewFixture failing(selector); failing.events[position].raw = 1U << bit;
                SclViewProbe subject; subject.selector = selector;
                check(!failing.Exercise(&subject) && failing.valid && failing.calls ==
                    (position == 36 ? 41U : position + 1) && subject.failed && !subject.access_lost &&
                    subject.failure == SclViewFailure::EngineStatus,
                    "SCL view each defined status bit remains FAIL without declaring access loss");
                unsigned calls = failing.calls;
                check(!subject.Observe(&failing, 0, SclViewFixture::Read, false) &&
                    !subject.Begin(&failing, SclViewFixture::Read, SclViewFixture::Write, false) &&
                    failing.calls == calls && !subject.access_lost &&
                    subject.failure == SclViewFailure::EngineStatus,
                    "SCL view repeated failed calls preserve known-status class with zero I/O");
                if (position >= 14 && position < 34) {
                    failing.events.erase(failing.events.begin() + calls, failing.events.begin() + 34);
                    check(!subject.Restore(&failing, SclViewFixture::Read, SclViewFixture::Write, false) &&
                        subject.restored && !subject.dirty && failing.valid &&
                        failing.calls == calls + 7 && failing.write_calls == 2,
                        "SCL view guarded restore after known error succeeds but never converts failure into PASS");
                } else {
                    check(!subject.Restore(&failing, SclViewFixture::Read, SclViewFixture::Write, false) &&
                        failing.calls == calls, "SCL view pre-write status error or completed restore never adds cleanup I/O");
                }
            }
        }
        // Early native failure after selection may restore before any DATA.
        SclViewFixture early(selector); SclViewProbe subject; subject.selector = selector; early.probe = &subject;
        check(subject.Begin(&early, SclViewFixture::Read, SclViewFixture::Write, false), "SCL view early native failure setup");
        early.events.erase(early.events.begin() + 14, early.events.begin() + 34);
        check(subject.Restore(&early, SclViewFixture::Read, SclViewFixture::Write, false) &&
            early.valid && early.calls == 21 && early.data_reads == 0 && subject.reads == 19,
            "SCL view native failure finalizer has guarded restore but no extra DATA");
        for (unsigned stage = 0; stage < 2; ++stage) {
            SclViewFixture delivery(selector); SclViewProbe delivered; delivered.selector = selector;
            delivery.probe = &delivered;
            check(delivered.Begin(&delivery, SclViewFixture::Read, SclViewFixture::Write, false),
                  "SCL view delivery ordering setup");
            if (stage) check(delivered.AfterDelivered(&delivery, 1, true, true,
                SclViewFixture::Read, SclViewFixture::Write, false), "first milestone before penultimate");
            const unsigned calls = delivery.calls;
            for (unsigned frame : {0U, 2U, 178U, 180U})
                check(delivered.AfterDelivered(&delivery, frame, true, true,
                    SclViewFixture::Read, SclViewFixture::Write, false) && delivery.calls == calls,
                    "SCL view excludes every non-milestone without diagnostic I/O");
            for (const std::pair<bool, bool> &barrier : {std::make_pair(false, false),
                    std::make_pair(false, true), std::make_pair(true, false)})
                check(!delivered.AfterDelivered(&delivery, stage ? 179 : 1, barrier.first, barrier.second,
                    SclViewFixture::Read, SclViewFixture::Write, false) && delivery.calls == calls && !delivered.failed,
                    "SCL view requires release and successful owned publication before milestone I/O");
            check(delivered.AfterDelivered(&delivery, stage ? 179 : 1, true, true,
                SclViewFixture::Read, SclViewFixture::Write, false) && delivery.valid &&
                delivery.calls == (stage ? 41U : 24U) && delivered.restored == (stage != 0),
                "SCL view penultimate restores immediately, first milestone only samples");
        }
        // A native early exit may permit cleanup, but cleanup access/identity
        // losses must stop its own sequence without a restoration retry.
        for (unsigned position = 14; position < 21; ++position) {
            SclViewFixture cleanup(selector); cleanup.events.erase(cleanup.events.begin() + 14, cleanup.events.begin() + 34);
            cleanup.fail_at = position; SclViewProbe abandoned; abandoned.selector = selector; cleanup.probe = &abandoned;
            check(abandoned.Begin(&cleanup, SclViewFixture::Read, SclViewFixture::Write, false) &&
                !abandoned.Restore(&cleanup, SclViewFixture::Read, SclViewFixture::Write, false) &&
                cleanup.valid && cleanup.calls == position + 1 && abandoned.access_lost,
                "early native cleanup API loss stops without further read/write");
            const unsigned calls = cleanup.calls;
            check(!abandoned.Restore(&cleanup, SclViewFixture::Read, SclViewFixture::Write, false) &&
                cleanup.calls == calls, "early native cleanup never retries a failed restoration");
        }
        for (unsigned control : {0U, 1U, 2U, 3U, 4U, 0xffffffffU}) {
            if (control == selector) continue;
            SclViewFixture cleanup(selector); cleanup.events.erase(cleanup.events.begin() + 14, cleanup.events.begin() + 34);
            cleanup.events[15].raw = control;
            SclViewProbe abandoned; abandoned.selector = selector; cleanup.probe = &abandoned;
            check(abandoned.Begin(&cleanup, SclViewFixture::Read, SclViewFixture::Write, false) &&
                !abandoned.Restore(&cleanup, SclViewFixture::Read, SclViewFixture::Write, false) &&
                cleanup.calls == 16 && cleanup.write_calls == 1 && abandoned.access_lost && cleanup.valid,
                "fresh cleanup current CTRL not ours prohibits blind restore");
        }
        {
            SclViewFixture identity(selector); SclViewProbe subject; subject.selector = selector; identity.probe = &subject;
            check(subject.Begin(&identity, SclViewFixture::Read, SclViewFixture::Write, false), "SCL view owner setup");
            SclViewFixture other(selector);
            check(!subject.Observe(&other, 0, SclViewFixture::Read, false) &&
                !subject.Restore(&identity, SclViewFixture::Read, SclViewFixture::Write, false) &&
                other.calls == 0 && identity.calls == 14 && subject.access_lost,
                "SCL view changed handle loses access without touching either context");
        }
    }
    {
        SclViewFixture fixture;
        SclViewProbe disabled;
        check(disabled.Begin(nullptr, nullptr, nullptr, false) &&
              disabled.Observe(nullptr, 99, nullptr, false) &&
              disabled.AfterDelivered(nullptr, 1, false, false, nullptr, nullptr, false) &&
              disabled.Restore(nullptr, nullptr, nullptr, false) && fixture.calls == 0 &&
              disabled.reads == 0 && disabled.writes == 0,
              "default-off SCL view is a pure zero-I/O no-op");
        for (unsigned selector : {1U, 4U, UINT_MAX}) {
            SclViewProbe invalid; invalid.selector = selector;
            check(!invalid.Begin(&fixture, SclViewFixture::Read, SclViewFixture::Write, false) &&
                  fixture.calls == 0, "SCL view rejects invalid selectors before all I/O");
        }
        for (unsigned argument = 0; argument < 3; ++argument) {
            SclViewProbe invalid; invalid.selector = 2;
            check(!invalid.Begin(argument == 0 ? nullptr : &fixture,
                  argument == 1 ? nullptr : SclViewFixture::Read,
                  argument == 2 ? nullptr : SclViewFixture::Write, false) && fixture.calls == 0,
                  "SCL view rejects missing handle/API callbacks without reads or writes");
        }
    }
    {
        // Check the actual raw reporter, not just the counters: an API failure
        // writes a poisoned out-parameter, which must remain NOT-READ in logs.
        FILE *record = std::tmpfile();
        const int saved_stdout = dup(STDOUT_FILENO);
        std::fflush(stdout);
        const bool redirected = record && saved_stdout >= 0 &&
            dup2(fileno(record), STDOUT_FILENO) >= 0;
        bool rejected = false;
        if (redirected) {
            SclViewFixture fixture; fixture.fail_at = 0;
            SclViewProbe view; view.selector = 2; fixture.probe = &view;
            rejected = !view.Begin(&fixture, SclViewFixture::Read, SclViewFixture::Write, true) &&
                fixture.calls == 1 && view.reads == 1 && view.writes == 0;
            std::fflush(stdout);
        }
        const bool stdout_restored = saved_stdout >= 0 && dup2(saved_stdout, STDOUT_FILENO) >= 0;
        if (saved_stdout >= 0) close(saved_stdout);
        char text[4096] = {};
        size_t bytes = 0;
        if (record) {
            std::rewind(record);
            bytes = std::fread(text, 1, sizeof(text) - 1, record);
            std::fclose(record);
        }
        check(redirected && stdout_restored && rejected && bytes &&
            std::strstr(text, "measured=0") && std::strstr(text, "rev@00540800=NOT-READ") &&
            !std::strstr(text, "deadbeef") && !std::strstr(text, "raw-stable=yes"),
            "SCL view failed read output parameter is never published as measured raw or stable");
    }
    for (const char *selector : {"2", "3"}) {
        Options admitted;
        check(ParseArguments({"probe", "--hardware", "fixture", "180", "30", "1",
            "--scaler-test", "0", "--observe-scl-view", selector, "--capture-yuy2", "new"}, &admitted) &&
            admitted.observe_scl_view == static_cast<unsigned>(selector[0] - '0') &&
            !admitted.observe_scl_config && !admitted.observe_chroma,
            "SCL view admits only explicit selector two or three native unscaled capture scope");
    }
    for (const std::vector<const char *> &arguments : std::vector<std::vector<const char *>>{
        {"probe", "--self-test", "--observe-scl-view", "2"},
        {"probe", "--preflight", "fixture", "180", "--scaler-test", "0", "--observe-scl-view", "2", "--capture-yuy2", "new"},
        {"probe", "--hardware", "fixture", "179", "--scaler-test", "0", "--observe-scl-view", "2", "--capture-yuy2", "new"},
        {"probe", "--hardware", "fixture", "180", "--scaler-test", "320", "--observe-scl-view", "2", "--capture-yuy2", "new"},
        {"probe", "--hardware", "fixture", "180", "--scaler-test", "640", "--observe-scl-view", "2", "--capture-yuy2", "new"},
        {"probe", "--hardware", "fixture", "180", "--scaler-test", "0", "--observe-scl-view", "2"},
        {"probe", "--hardware", "fixture", "180", "30", "2", "--scaler-test", "0", "--observe-scl-view", "2", "--capture-yuy2", "new"},
        {"probe", "--hardware", "fixture", "180", "--scaler-test", "0", "--observe-scl-view", "2", "--capture-uyvy", "new"},
        {"probe", "--hardware", "fixture", "180", "--scaler-test", "0", "--observe-chroma", "--observe-scl-view", "2", "--capture-yuy2", "new"},
        {"probe", "--hardware", "fixture", "180", "--scaler-test", "0", "--observe-scl-config", "--observe-scl-view", "2", "--capture-yuy2", "new"},
        {"probe", "--hardware", "fixture", "180", "--scaler-test", "0", "--observe-scl-view", "2", "--observe-scl-config", "--capture-yuy2", "new"},
        {"probe", "--hardware", "fixture", "180", "--scaler-test", "0", "--mpeg1-via-mpeg2", "--observe-scl-view", "2", "--capture-yuy2", "new"},
        {"probe", "--hardware", "fixture", "180", "--scaler-test", "0", "--h263-via-divx", "--open-only", "--observe-scl-view", "2", "--capture-yuy2", "new"},
        {"probe", "--hardware", "fixture", "180", "--scaler-test", "0", "--observe-scl-view", "2", "--observe-scl-view", "3", "--capture-yuy2", "new"}}) {
        Options invalid; check(!ParseArguments(arguments, &invalid), "SCL view rejects mixed/default/alternate/repeated/invalid scopes");
    }
    for (const char *selector : {"", "0", "1", "4", "-2", "+2", "02", "2x", " 2", "4294967296"}) {
        Options invalid;
        check(!ParseArguments({"probe", "--hardware", "fixture", "180", "--scaler-test", "0",
            "--observe-scl-view", selector, "--capture-yuy2", "new"}, &invalid), "SCL view literal selector admission");
    }
    {
        Options view; view.observe_scl_view = 2; view.expected = 180;
        Input native; native.codec = AV_CODEC_ID_MPEG2VIDEO; native.subtype = BC_MSUBTYPE_MPEG2VIDEO;
        native.progressive = true; native.width = 640; native.height = 360; native.packets.resize(180);
        check(SclInputAdmitted(view, native), "SCL view exact native input shape admitted after load");
        for (unsigned field = 0; field < 7; ++field) {
            Options changed = view; Input altered = native;
            if (field == 0) altered.codec = AV_CODEC_ID_H264;
            if (field == 1) altered.subtype = BC_MSUBTYPE_H264;
            if (field == 2) altered.progressive = false;
            if (field == 3) altered.width = 638;
            if (field == 4) altered.height = 358;
            if (field == 5) altered.packets.pop_back();
            if (field == 6) changed.expected = 179;
            check(!SclInputAdmitted(changed, altered), "SCL view input codec/shape/count mismatches refuse before device open");
        }
    }
    for (unsigned stage = 0; stage < 2; ++stage) {
        for (unsigned position = 0; position < 20; ++position) {
            for (int code = -1; code <= BC_STS_PWR_MGMT; ++code) {
                if (code == BC_STS_SUCCESS) continue;
                MfdAdmissionFixture fixture; fixture.fail_at = position;
                fixture.status = static_cast<BC_STATUS>(code);
                struct { uint32_t before = 0x12345678; MfdAdmissionSnapshot value;
                         uint32_t after = 0x87654321; } guarded;
                check(!ReadMfdAdmission(&fixture, &guarded.value, MfdAdmissionFixture::Read) &&
                    fixture.valid && fixture.calls == position + 1 && guarded.value.reads == position + 1 &&
                    guarded.value.measured == position && guarded.value.status == code &&
                    guarded.value.failure == MfdAdmissionFailure::Read &&
                    guarded.before == 0x12345678 && guarded.after == 0x87654321,
                    "MFD every API failure stops the literal 20-address sequence within canaries");
                bool unpublished = true;
                for (unsigned unread = position; unread < 20; ++unread)
                    unpublished &= guarded.value.raw[unread] == 0;
                check(unpublished, "MFD poisoned failed output and all later words remain unmeasured");
                MfdAdmissionObserver subject; subject.enabled = true;
                fixture.calls = 0; fixture.fail_at = 20;
                if (stage) check(subject.Observe(&fixture, 0, MfdAdmissionFixture::Read, false),
                    "MFD second-stage fault has accepted preSTART admission");
                fixture.calls = 0; fixture.fail_at = position;
                check(!subject.Observe(&fixture, stage, MfdAdmissionFixture::Read, false) && subject.failed &&
                    fixture.valid && fixture.calls == position + 1 &&
                    subject.reads == stage * 20 + position + 1, "MFD failure latches at either stage without retries");
                MfdAdmissionFixture other;
                check(!subject.Observe(&fixture, stage, MfdAdmissionFixture::Read, false) &&
                    !subject.Observe(&other, 1, MfdAdmissionFixture::Read, false) &&
                    !subject.AfterDelivered(&fixture, 1, true, true, MfdAdmissionFixture::Read, false) &&
                    fixture.calls == position + 1 && other.calls == 0,
                    "MFD failed/repeated/changed-handle calls never read again");
            }
        }
    }
    for (unsigned position = 0; position < 20; ++position) {
        const unsigned field = position % 10;
        const unsigned widths[] = {16, 4, 24, 1, 10, 4, 16, 8, 16, 16};
        for (unsigned bit = widths[field]; bit < 32; ++bit) {
            MfdAdmissionFixture fixture; fixture.raw[position] |= 1U << bit;
            MfdAdmissionSnapshot snapshot;
            check(!ReadMfdAdmission(&fixture, &snapshot, MfdAdmissionFixture::Read) && fixture.valid &&
                fixture.calls == position + 1 && snapshot.measured == position + 1 &&
                snapshot.failure == MfdAdmissionFailure::Reserved, "MFD every reserved bit is fail-fast at both passes");
        }
        std::vector<uint32_t> invalid;
        if (field == 0 || field == 6 || field == 8 || field == 9)
            invalid = {0U, 1U, 0xffffU, (field == 0 || field == 8) ? 0x51U : 0x81U};
        if (field == 1 || field == 3 || field == 5 || field == 7)
            for (unsigned bit = 0; bit < widths[field]; ++bit)
                invalid.push_back((field == 5 ? 4U : 0U) ^ (1U << bit));
        for (uint32_t raw : invalid) {
            MfdAdmissionFixture fixture; fixture.raw[position] = raw;
            MfdAdmissionSnapshot snapshot;
            const auto failure = field == 7 ? MfdAdmissionFailure::EngineStatus :
                (field == 1 || field == 3 || field == 5) ? MfdAdmissionFailure::Mode : MfdAdmissionFailure::Revision;
            check(!ReadMfdAdmission(&fixture, &snapshot, MfdAdmissionFixture::Read) && fixture.valid &&
                fixture.calls == position + 1 && snapshot.failure == failure,
                "MFD each defined mode/status bit and wrong board revision reject without later reads");
        }
    }
    for (unsigned field : {2U, 4U}) {
        for (uint32_t raw : {0U, 1U, field == 2 ? 0xffffffU : 0x3ffU}) {
            MfdAdmissionFixture fixture; fixture.raw[field] = fixture.raw[field + 10] = raw;
            MfdAdmissionSnapshot snapshot;
            check(ReadMfdAdmission(&fixture, &snapshot, MfdAdmissionFixture::Read) && fixture.valid &&
                fixture.calls == 20 && snapshot.measured == 20, "MFD admitted colour/remap scalars remain opaque, including extremes");
            fixture.calls = 0; fixture.raw[field + 10] ^= 1;
            check(!ReadMfdAdmission(&fixture, &snapshot, MfdAdmissionFixture::Read) && fixture.valid &&
                fixture.calls == 20 && snapshot.failure == MfdAdmissionFailure::Unstable,
                "MFD admissible repeat changes reject only after the complete snapshot without retargeting");
        }
    }
    {
        MfdAdmissionFixture live;
        live.raw[2] = live.raw[12] = 0x108080; live.raw[4] = live.raw[14] = 0x108;
        MfdAdmissionSnapshot snapshot;
        check(ReadMfdAdmission(&live, &snapshot, MfdAdmissionFixture::Read) && live.valid && live.calls == 20,
            "MFD observed native colour/remap with TEST4 is admitted without interpreting or changing saturation");
        live.calls = 0; live.raw[5] = live.raw[15] = 0;
        check(!ReadMfdAdmission(&live, &snapshot, MfdAdmissionFixture::Read) && live.valid && live.calls == 6 &&
            snapshot.failure == MfdAdmissionFailure::Mode, "MFD former TEST0 profile rejects without writes or later reads");
    }
    {
        MfdAdmissionFixture fixture; MfdAdmissionObserver disabled;
        check(disabled.Observe(nullptr, 99, nullptr, false) &&
            disabled.AfterDelivered(nullptr, 1, false, false, nullptr, false) && !disabled.reads,
            "MFD default disabled observer is a pure zero-I/O no-op");
        for (unsigned argument = 0; argument < 4; ++argument) {
            MfdAdmissionObserver invalid; invalid.enabled = true;
            check(!invalid.Observe(argument == 0 ? nullptr : &fixture, argument == 2 ? 2 : argument == 3 ? 1 : 0,
                argument == 1 ? nullptr : MfdAdmissionFixture::Read, false) && fixture.calls == 0 && invalid.failed,
                "MFD invalid handle/callback/stage/order refuses before first read");
        }
        check(!ReadMfdAdmission(&fixture, nullptr, MfdAdmissionFixture::Read) && fixture.calls == 0,
            "MFD null snapshot has no I/O");
        for (bool change_handle : {false, true}) {
            MfdAdmissionObserver subject; subject.enabled = true; fixture.calls = 0;
            MfdAdmissionFixture other;
            check(subject.Observe(&fixture, 0, MfdAdmissionFixture::Read, false) &&
                !subject.Observe(change_handle ? &other : &fixture, change_handle ? 1 : 0,
                    MfdAdmissionFixture::Read, false) && subject.failed && fixture.calls == 20 && other.calls == 0,
                "MFD duplicate admission or changed output handle latches without more I/O");
        }
        MfdAdmissionObserver delivered; delivered.enabled = true; fixture.calls = 0;
        check(delivered.Observe(&fixture, 0, MfdAdmissionFixture::Read, false), "MFD delivery preSTART setup");
        fixture.calls = 0;
        for (unsigned frame : {0U, 2U, 179U, 180U})
            check(delivered.AfterDelivered(&fixture, frame, true, true, MfdAdmissionFixture::Read, false) &&
                fixture.calls == 0, "MFD never observes FMT/last-frame/EOS or other output ordinals");
        for (const auto &barrier : {std::make_pair(false, false), std::make_pair(false, true), std::make_pair(true, false)})
            check(!delivered.AfterDelivered(&fixture, 1, barrier.first, barrier.second, MfdAdmissionFixture::Read, false) &&
                fixture.calls == 0 && !delivered.failed, "MFD requires release and owned write before output observation");
        fixture.raw[2] = fixture.raw[12] = 0; fixture.raw[4] = fixture.raw[14] = 0x3ff;
        check(delivered.AfterDelivered(&fixture, 1, true, true, MfdAdmissionFixture::Read, false) &&
            fixture.valid && fixture.calls == 20 && delivered.reads == 40 && delivered.attempted == 3,
            "MFD two independent stage admissions total 40 reads, allow opaque interstage scalar changes");
        check(!delivered.AfterDelivered(&fixture, 1, true, true, MfdAdmissionFixture::Read, false) && fixture.calls == 20,
            "MFD successful first output cannot be sampled twice");
    }
    for (bool closing_revision : {false, true}) {
        FILE *record = std::tmpfile(); const int saved_stdout = dup(STDOUT_FILENO);
        std::fflush(stdout);
        const bool redirected = record && saved_stdout >= 0 && dup2(fileno(record), STDOUT_FILENO) >= 0;
        bool rejected = false;
        if (redirected) {
            MfdAdmissionFixture fixture;
            if (closing_revision) fixture.raw[19] = 0x81; else fixture.fail_at = 0;
            MfdAdmissionObserver subject; subject.enabled = true;
            rejected = !subject.Observe(&fixture, 0, MfdAdmissionFixture::Read) && fixture.valid &&
                fixture.calls == (closing_revision ? 20U : 1U);
            std::fflush(stdout);
        }
        const bool restored = saved_stdout >= 0 && dup2(saved_stdout, STDOUT_FILENO) >= 0;
        if (saved_stdout >= 0) close(saved_stdout);
        char text[8192] = {}; size_t bytes = 0;
        if (record) { std::rewind(record); bytes = std::fread(text, 1, sizeof(text) - 1, record); std::fclose(record); }
        check(redirected && restored && rejected && bytes && std::strstr(text, "result=FAIL") &&
            !std::strstr(text, "deadbeef") && (closing_revision ?
                (std::strstr(text, "measured=20") && std::strstr(text, "raw-stable=no") &&
                 std::strstr(text, "closing-scl-rev@00540800=00000081")) :
                (std::strstr(text, "measured=0") && std::strstr(text, "mfd-rev@00540000=NOT-READ") &&
                 std::strstr(text, "closing-scl-rev@00540800=NOT-READ") && !std::strstr(text, "raw-stable=yes"))),
            "MFD actual reporter suppresses poisoned unread words and computes final-failure raw stability independently");
    }
    {
        const std::vector<const char *> valid = {"probe", "--hardware", "fixture", "180", "30", "1",
            "--scaler-test", "0", "--observe-mfd-config", "--capture-yuy2", "new"};
        Options admitted;
        check(ParseArguments(valid, &admitted) && admitted.observe_mfd_config && NeedsRawIo(admitted) &&
            !admitted.observe_scl_view && !admitted.observe_scl_config && !admitted.observe_chroma && !NeedsRawIo(Options{}),
            "MFD opt-in requires raw-I/O capability before fixture/capture/device; ordinary options do not");
        for (unsigned field = 0; field < 10; ++field) {
            auto arguments = valid;
            if (field == 0) arguments[1] = "--preflight";
            if (field == 1) arguments[3] = "179";
            if (field == 2) arguments[5] = "2";
            if (field == 3) arguments[7] = "320";
            if (field == 4) arguments[7] = "640";
            if (field == 5) arguments[9] = "--capture-uyvy";
            if (field == 6) arguments.resize(9);
            if (field == 7) arguments.insert(arguments.begin() + 8, "--mpeg1-via-mpeg2");
            if (field == 8) arguments.insert(arguments.begin() + 8, "--h263-via-divx");
            if (field == 9) arguments.insert(arguments.begin() + 8, "--open-only");
            Options invalid; check(!ParseArguments(arguments, &invalid), "MFD parser rejects nonnative/scaled/repeated/noncapture scope");
        }
        for (const std::vector<const char *> &mixed : std::vector<std::vector<const char *>>{
                {"--observe-chroma"}, {"--observe-scl-config"}, {"--observe-scl-view", "2"}, {"--observe-mfd-config"}})
            for (unsigned where : {8U, 9U}) {
                auto arguments = valid; arguments.insert(arguments.begin() + where, mixed.begin(), mixed.end());
                Options invalid; check(!ParseArguments(arguments, &invalid), "MFD every observer mixture/order/duplicate is rejected");
            }
        Options invalid;
        check(!ParseArguments({"probe", "--self-test", "--observe-mfd-config"}, &invalid), "MFD self-test option mix refused");
        Input native; native.codec = AV_CODEC_ID_MPEG2VIDEO; native.subtype = BC_MSUBTYPE_MPEG2VIDEO;
        native.progressive = true; native.width = 640; native.height = 360; native.packets.resize(180);
        check(SclInputAdmitted(admitted, native), "MFD exact native input admitted before capture/device open");
        for (unsigned field = 0; field < 7; ++field) {
            Options changed = admitted; Input altered = native;
            if (field == 0) altered.codec = AV_CODEC_ID_H264;
            if (field == 1) altered.subtype = BC_MSUBTYPE_H264;
            if (field == 2) altered.progressive = false;
            if (field == 3) altered.width = 638;
            if (field == 4) altered.height = 358;
            if (field == 5) altered.packets.pop_back();
            if (field == 6) changed.expected = 179;
            check(!SclInputAdmitted(changed, altered) && SclInputAdmitted(Options{}, altered),
                "MFD codec/subtype/progressive/geometry/packet/expected mismatch rejected, default unchanged");
        }
    }
    for (unsigned stimulus : {1U, 2U}) {
        MfdColourFixture normal(stimulus);
        MfdColourProbe complete; complete.stimulus = stimulus;
        check(normal.events.size() == 81 && normal.Exercise(&complete) && normal.valid && normal.calls == 81 &&
            normal.write_calls == 4 && complete.reads == 77 && complete.writes == 4 && complete.restored &&
            !complete.colour_dirty && !complete.control_dirty && complete.saved_colour == 0x909070 &&
            complete.saved_remap == 0x108, "MFD colour independent literal normal oracle is 77R/4W and restores first-output current colour");
        check(complete.Restore(&normal, MfdColourFixture::Read, MfdColourFixture::Write, false) && normal.calls == 81,
            "MFD colour restoration finalizer never repeats successful target writes");
        for (unsigned position = 0; position < 81; ++position) {
            for (int code = -1; code <= BC_STS_PWR_MGMT; ++code) {
                if (code == BC_STS_SUCCESS) continue;
                MfdColourFixture fixture(stimulus); fixture.fail_at = position; fixture.status = static_cast<BC_STATUS>(code);
                struct { uint32_t before = 0x12345678; MfdColourProbe probe; uint32_t after = 0x87654321; } guarded;
                guarded.probe.stimulus = stimulus;
                check(!fixture.Exercise(&guarded.probe) && fixture.valid && fixture.calls == position + 1 &&
                    guarded.probe.failed && guarded.probe.access_lost && guarded.probe.api_status == code &&
                    guarded.probe.failure == MfdColourFailure::Api && guarded.before == 0x12345678 && guarded.after == 0x87654321,
                    "MFD colour all27 API failures at all81 positions stop, preserve status and canaries, dirty before writes");
                const unsigned calls = fixture.calls;
                check(!guarded.probe.Restore(&fixture, MfdColourFixture::Read, MfdColourFixture::Write, false) &&
                    !guarded.probe.PreStart(&fixture, MfdColourFixture::Read, false) &&
                    !guarded.probe.AfterDelivered(&fixture, 1, true, true, MfdColourFixture::Read, MfdColourFixture::Write, false) &&
                    !guarded.probe.AfterDelivered(&fixture, 90, true, true, MfdColourFixture::Read, MfdColourFixture::Write, false) &&
                    fixture.calls == calls, "MFD colour API loss forbids retries, future reads and blind restoration");
            }
            const auto &event = normal.events[position];
            if (event.write) continue;
            unsigned width = event.address == 0x540008 ? 24 : event.address == 0x540004 || event.address == 0x540074 ? 4 :
                event.address == 0x540044 ? 1 : event.address == 0x54004c ? 10 : event.address == 0x5408a4 ? 8 : 16;
            for (unsigned bit = width; bit < 32; ++bit) {
                MfdColourFixture fixture(stimulus); fixture.events[position].raw |= 1U << bit;
                MfdColourProbe subject; subject.stimulus = stimulus;
                check(!fixture.Exercise(&subject) && fixture.valid && fixture.calls == position + 1 && subject.access_lost &&
                    subject.failure == MfdColourFailure::Reserved &&
                    !subject.Restore(&fixture, MfdColourFixture::Read, MfdColourFixture::Write, false) && fixture.calls == position + 1,
                    "MFD colour every reserved bit at every named read closes access without cleanup I/O");
            }
            if (event.address == 0x5408a4) continue; // Known-status cleanup has its own phase oracle below.
            for (unsigned bit = 0; bit < (event.address == 0x540004 || event.address == 0x540074 ? 4U : 1U); ++bit) {
                MfdColourFixture fixture(stimulus); fixture.events[position].raw ^= 1U << bit;
                MfdColourProbe subject; subject.stimulus = stimulus;
                const unsigned stop = position < 40 && (event.address == 0x540008 || event.address == 0x54004c) ?
                    (position < 20 ? 20U : 40U) : position + 1;
                check(!fixture.Exercise(&subject) && fixture.valid && fixture.calls == stop && subject.access_lost &&
                    !subject.Restore(&fixture, MfdColourFixture::Read, MfdColourFixture::Write, false) && fixture.calls == stop,
                    "MFD colour board revision/data/test/remap/control/colour mutation stops without reassertion");
            }
        }
        for (unsigned position : {7U, 17U, 27U, 37U, 49U, 61U, 71U}) {
            for (unsigned bit = 0; bit < 8; ++bit) {
                MfdColourFixture fixture(stimulus); fixture.events[position].raw = 1U << bit;
                MfdColourProbe subject; subject.stimulus = stimulus;
                check(!fixture.Exercise(&subject) && fixture.valid && subject.failed && !subject.access_lost &&
                    subject.failure == MfdColourFailure::EngineStatus && fixture.calls == (position == 71 ? 81U : position + 1),
                    "MFD colour defined status stays FAIL, not an invented transport loss");
                const unsigned stopped = fixture.calls;
                check(!subject.AfterDelivered(&fixture, 90, true, true, MfdColourFixture::Read, MfdColourFixture::Write, false) &&
                    !subject.PreStart(&fixture, MfdColourFixture::Read, false) && fixture.calls == stopped && !subject.access_lost &&
                    subject.failure == MfdColourFailure::EngineStatus, "MFD repeated failed normal calls preserve eligible status class");
                if (position == 49 || position == 61) {
                    fixture.events.erase(fixture.events.begin() + stopped, fixture.events.begin() + 64);
                    if (position == 49) {
                        fixture.events[stopped + 1].raw = 0; // Full fresh guard expects control never attempted.
                        fixture.events.erase(fixture.events.begin() + stopped + 10, fixture.events.begin() + stopped + 12);
                    }
                    // Repeat the known bit in the fresh cleanup guard; closing revision checks still required.
                    fixture.events[stopped + 7].raw = 1U << bit;
                    check(!subject.Restore(&fixture, MfdColourFixture::Read, MfdColourFixture::Write, false) && fixture.valid &&
                        fixture.calls == fixture.events.size() && subject.restored && !subject.access_lost &&
                        subject.reads == (position == 49 ? 63U : 75U) && subject.writes == (position == 49 ? 2U : 4U),
                        "MFD fresh known-status guard allows phase-aware restore yet overall experiment remains FAIL");
                } else check(!subject.Restore(&fixture, MfdColourFixture::Read, MfdColourFixture::Write, false) &&
                    fixture.calls == stopped, "MFD status before seed/no dirty or completed restoration adds no cleanup I/O");
            }
        }
        // A native early failure, not a register failure, may perform one cleanup guard with no frame90 observation.
        for (unsigned fault = 0; fault < 17; ++fault) {
            MfdColourFixture fixture(stimulus); MfdColourProbe subject; subject.stimulus = stimulus; fixture.probe = &subject;
            check(subject.PreStart(&fixture, MfdColourFixture::Read, false) &&
                subject.AfterDelivered(&fixture, 1, true, true, MfdColourFixture::Read, MfdColourFixture::Write, false),
                "MFD early native cleanup selected setup");
            fixture.events.erase(fixture.events.begin() + 54, fixture.events.begin() + 64);
            fixture.fail_at = 54 + fault;
            check(!subject.Restore(&fixture, MfdColourFixture::Read, MfdColourFixture::Write, false) && fixture.valid &&
                fixture.calls == 55 + fault && subject.access_lost && subject.failed,
                "MFD every early native cleanup API failure stops at its own fresh guard/write/readback");
            const unsigned calls = fixture.calls;
            check(!subject.Restore(&fixture, MfdColourFixture::Read, MfdColourFixture::Write, false) && fixture.calls == calls,
                "MFD early cleanup cannot be retried");
        }
        {
            MfdColourFixture fixture(stimulus, 0x123456); MfdColourProbe subject; subject.stimulus = stimulus;
            check(fixture.Exercise(&subject) && fixture.valid && subject.saved_colour == 0x123456 && fixture.events[77].raw == 0x123456,
                "MFD colour restore uses sampled current value, not hardcoded909070 or preSTART108080");
        }
        {
            MfdColourFixture fixture(stimulus); fixture.events[76].raw ^= 1;
            MfdColourProbe subject; subject.stimulus = stimulus;
            check(!fixture.Exercise(&subject) && fixture.valid && fixture.calls == 77 && subject.writes == 3 && subject.access_lost &&
                !subject.Restore(&fixture, MfdColourFixture::Read, MfdColourFixture::Write, false) && fixture.calls == 77,
                "MFD fresh colour loss after disabling control prevents overwriting firmware's new value");
        }
        {
            MfdColourFixture fixture(stimulus); MfdColourProbe subject; subject.stimulus = stimulus; fixture.probe = &subject;
            check(subject.PreStart(&fixture, MfdColourFixture::Read, false), "MFD host publication ordering preSTART setup");
            for (unsigned frame : {0U, 2U, 89U, 91U, 179U, 180U})
                check(subject.AfterDelivered(&fixture, frame, true, true, MfdColourFixture::Read, MfdColourFixture::Write, false) &&
                    fixture.calls == 20, "MFD no I/O at FMT/EOS/last/other ordinals");
            for (const auto &barrier : {std::make_pair(false, false), std::make_pair(false, true), std::make_pair(true, false)})
                check(!subject.AfterDelivered(&fixture, 1, barrier.first, barrier.second, MfdColourFixture::Read, MfdColourFixture::Write, false) &&
                    fixture.calls == 20 && !subject.failed, "MFD cannot write before release and owned capture Write");
            check(subject.AfterDelivered(&fixture, 1, true, true, MfdColourFixture::Read, MfdColourFixture::Write, false) && fixture.calls == 54,
                "MFD first eligible owned picture alone seeds and selects colour");
            for (const auto &barrier : {std::make_pair(false, false), std::make_pair(false, true), std::make_pair(true, false)})
                check(!subject.AfterDelivered(&fixture, 90, barrier.first, barrier.second, MfdColourFixture::Read, MfdColourFixture::Write, false) &&
                    fixture.calls == 54 && !subject.failed, "MFD frame90 must also be released and stored before custom observation/restore");
            check(subject.AfterDelivered(&fixture, 90, true, true, MfdColourFixture::Read, MfdColourFixture::Write, false) && fixture.valid &&
                fixture.calls == 81 && subject.restored, "MFD restore immediately at90 before later native output/EOS");
            check(!subject.AfterDelivered(&fixture, 90, true, true, MfdColourFixture::Read, MfdColourFixture::Write, false) && fixture.calls == 81,
                "MFD repeated milestone refuses without retarget/retry");
        }
        for (unsigned stage : {0U, 1U, 2U}) {
            MfdColourFixture fixture(stimulus), other(stimulus); MfdColourProbe subject; subject.stimulus = stimulus; fixture.probe = &subject;
            if (stage) check(subject.PreStart(&fixture, MfdColourFixture::Read, false), "MFD owner setup");
            if (stage == 2) check(subject.AfterDelivered(&fixture, 1, true, true, MfdColourFixture::Read, MfdColourFixture::Write, false), "MFD selected owner setup");
            const unsigned calls = fixture.calls;
            check(!subject.AfterDelivered(&other, stage == 2 ? 90 : 1, true, true, MfdColourFixture::Read, MfdColourFixture::Write, false) &&
                !subject.Restore(&fixture, MfdColourFixture::Read, MfdColourFixture::Write, false) && other.calls == 0 && fixture.calls == calls,
                "MFD missing admission/changed HANDLE excludes all custom I/O and blind restore");
        }
    }
    for (unsigned stimulus : {1U, 2U}) {
        // The known pre-enable status failure leaves only colour dirty. This
        // independently removes all CTRL writes and the frame90 observation.
        for (unsigned position = 50; position < 65; ++position)
            for (int code = -1; code <= BC_STS_PWR_MGMT; ++code) {
                if (code == BC_STS_SUCCESS) continue;
                MfdColourFixture fixture(stimulus); fixture.events[49].raw = 1;
                fixture.events.erase(fixture.events.begin() + 50, fixture.events.begin() + 64);
                fixture.events[51].raw = 0;
                fixture.events.erase(fixture.events.begin() + 60, fixture.events.begin() + 62);
                fixture.fail_at = position; fixture.status = static_cast<BC_STATUS>(code);
                MfdColourProbe subject; subject.stimulus = stimulus;
                check(!fixture.Exercise(&subject) && fixture.valid && fixture.calls == 50 && !subject.access_lost &&
                    subject.colour_dirty && !subject.control_dirty, "MFD colour-only cleanup begins from a real known-status pre-enable rejection");
                check(!subject.Restore(&fixture, MfdColourFixture::Read, MfdColourFixture::Write, false) && fixture.valid &&
                    fixture.calls == position + 1 && subject.access_lost && subject.api_status == code && subject.failure == MfdColourFailure::Api,
                    "MFD every colour-only fresh-guard/reread/write/closing API position rejects all27 statuses without enabling control");
                const unsigned calls = fixture.calls;
                check(!subject.Restore(&fixture, MfdColourFixture::Read, MfdColourFixture::Write, false) && fixture.calls == calls,
                    "MFD failed colour-only restoration never retries");
            }
        {
            MfdColourFixture fixture(stimulus); fixture.events[49].raw = 1;
            fixture.events.erase(fixture.events.begin() + 50, fixture.events.begin() + 64);
            fixture.events[51].raw = 0; fixture.events.erase(fixture.events.begin() + 60, fixture.events.begin() + 62);
            fixture.events[60].raw ^= 1;
            MfdColourProbe subject; subject.stimulus = stimulus;
            check(!fixture.Exercise(&subject) && !subject.access_lost && fixture.calls == 50 &&
                !subject.Restore(&fixture, MfdColourFixture::Read, MfdColourFixture::Write, false) && fixture.valid &&
                fixture.calls == 61 && subject.writes == 1 && subject.access_lost,
                "MFD colour-only cleanup also fresh-checks candidate before overwriting a firmware rewrite");
        }
        {
            MfdColourFixture fixture(stimulus); MfdColourProbe subject; subject.stimulus = stimulus; fixture.probe = &subject;
            check(subject.PreStart(&fixture, MfdColourFixture::Read, false) &&
                subject.AfterDelivered(&fixture, 1, true, true, MfdColourFixture::Read, MfdColourFixture::Write, false),
                "MFD native failure cleanup success selected setup");
            fixture.events.erase(fixture.events.begin() + 54, fixture.events.begin() + 64);
            check(subject.Restore(&fixture, MfdColourFixture::Read, MfdColourFixture::Write, false) && fixture.valid &&
                fixture.calls == 71 && subject.reads == 67 && subject.writes == 4 && subject.restored && !subject.observed,
                "MFD early non-access native failure permits one fresh restoration but no fabricated frame90 sample");
        }
        {
            MfdColourFixture fixture(stimulus); fixture.events[24].raw = fixture.events[34].raw = 0x109;
            MfdColourProbe subject; subject.stimulus = stimulus;
            check(!fixture.Exercise(&subject) && fixture.valid && fixture.calls == 47 && subject.saved_remap == 0x109 &&
                subject.writes == 1 && subject.access_lost, "MFD guards compare captured current remap, not a hardcoded or preSTART value");
        }
    }
    for (unsigned position : {0U, 40U, 41U, 49U, 52U, 61U, 74U, 76U, 77U, 80U}) {
        FILE *record = std::tmpfile(); const int saved_stdout = dup(STDOUT_FILENO); std::fflush(stdout);
        const bool redirected = record && saved_stdout >= 0 && dup2(fileno(record), STDOUT_FILENO) >= 0;
        bool rejected = false;
        if (redirected) {
            MfdColourFixture fixture; fixture.fail_at = position; MfdColourProbe subject; subject.stimulus = 1;
            rejected = !fixture.Exercise(&subject, true) && fixture.valid && fixture.calls == position + 1;
            std::fflush(stdout);
        }
        const bool restored = saved_stdout >= 0 && dup2(saved_stdout, STDOUT_FILENO) >= 0;
        if (saved_stdout >= 0) close(saved_stdout);
        char text[32768] = {}; size_t bytes = 0;
        if (record) { std::rewind(record); bytes = std::fread(text, 1, sizeof(text) - 1, record); std::fclose(record); }
        const bool write_failed = position == 40 || position == 52 || position == 74 || position == 77;
        check(redirected && restored && rejected && bytes && !std::strstr(text, "deadbeef") &&
            (write_failed ? std::strstr(text, "api-status=-1 result=FAIL") != nullptr :
                (std::strstr(text, "experiment-result=FAIL") && std::strstr(text, "NOT-READ"))),
            "MFD actual reporter distinguishes failed write attempts and never publishes poisoned/unread register words");
    }
    {
        MfdColourFixture fixture; MfdColourProbe subject; subject.stimulus = 1; subject.owner = &fixture;
        struct { uint32_t before = 0x12345678; MfdColourTrace trace; uint32_t after = 0x87654321; } guarded;
        check(subject.Read(&fixture, &guarded.trace, 19, 0x540000, 0xffff, 0x50, MfdColourFixture::Read) &&
            guarded.trace.raw[19] == 0x50 && guarded.before == 0x12345678 && guarded.after == 0x87654321 && fixture.valid,
            "MFD trace final DWORD remains within independently poisoned canaries");
        check(!subject.Read(&fixture, &guarded.trace, 20, 0x540000, 0xffff, 0x50, MfdColourFixture::Read) &&
            fixture.calls == 1 && guarded.before == 0x12345678 && guarded.after == 0x87654321,
            "MFD trace index overflow rejects before API/output write");
        MfdColourProbe disabled;
        check(disabled.PreStart(nullptr, nullptr, false) && disabled.AfterDelivered(nullptr, 90, false, false, nullptr, nullptr, false) &&
            disabled.Restore(nullptr, nullptr, nullptr, false) && !disabled.reads && !disabled.writes,
            "MFD active default-off is zero-I/O and does not require host capture");
        for (unsigned stimulus : {3U, UINT_MAX}) {
            MfdColourFixture fixture; MfdColourProbe invalid; invalid.stimulus = stimulus;
            check(!invalid.PreStart(&fixture, MfdColourFixture::Read, false) && fixture.calls == 0,
                "MFD invalid stimulus refuses before diagnostic read/write");
        }
        for (int code = -1; code <= BC_STS_PWR_MGMT; ++code) {
            if (code == BC_STS_SUCCESS) continue;
            for (bool control_dirty : {false, true}) {
                PackingReadFixture fixture; fixture.status = static_cast<BC_STATUS>(code);
                MfdColourProbe colour; colour.stimulus = 1; colour.owner = &fixture; colour.seeded = true;
                colour.colour_dirty = true; colour.control_dirty = control_dirty;
                SclViewProbe view; view.selector = 2; view.owner = &fixture; view.dirty = true;
                check(!PackingState(&fixture, "injected-failure", &colour, &view, PackingReadFixture::Read, false) &&
                    fixture.valid && fixture.reads == 1 && colour.access_lost && view.access_lost &&
                    colour.api_status == code && view.api_status == code && colour.failure == MfdColourFailure::Api && view.failure == SclViewFailure::Api,
                    "Actual packing helper all27 read failures latch both enabled dirty experiments accurately");
                check(!colour.Restore(&fixture, PackingReadFixture::Read, PackingReadFixture::Write, false) &&
                    !view.Restore(&fixture, PackingReadFixture::Read, PackingReadFixture::Write, false) &&
                    !PackingState(&fixture, "no-retry", &colour, &view, PackingReadFixture::Read, false) &&
                    fixture.reads == 1 && fixture.writes == 0, "Packing access loss prevents all subsequent custom probes/restoration");
            }
        }
        PackingReadFixture success;
        check(PackingState(&success, "ordinary", nullptr, nullptr, PackingReadFixture::Read, false) && success.valid && success.reads == 1,
            "Ordinary successful packing helper remains unchanged and target-read-only");
    }
    for (const char *name : {"a", "b"}) {
        const std::vector<const char *> valid = {"probe", "--hardware", "fixture", "180", "30", "1",
            "--scaler-test", "0", "--inject-mfd-colour", name, "--capture-yuy2", "new"};
        Options admitted;
        check(ParseArguments(valid, &admitted) && admitted.inject_mfd_colour == (name[0] == 'a' ? 1U : 2U) &&
            NeedsRawIo(admitted) && !admitted.observe_mfd_config && !admitted.observe_scl_config && !admitted.observe_scl_view && !admitted.observe_chroma,
            "MFD colour literal a/b one native unscaled YUY2 capture requires CAP before fixture/device");
        for (const char *invalid : {"", "A", "B", "0", "1", "2", "c", "405ac3", "b2d32b", " a"}) {
            auto arguments = valid; arguments[9] = invalid; Options rejected;
            check(!ParseArguments(arguments, &rejected), "MFD colour accepts no arbitrary register value or alternate stimulus spelling");
        }
        for (unsigned field = 0; field < 10; ++field) {
            auto arguments = valid;
            if (field == 0) arguments[1] = "--preflight";
            if (field == 1) arguments[3] = "179";
            if (field == 2) arguments[5] = "2";
            if (field == 3) arguments[7] = "320";
            if (field == 4) arguments[7] = "640";
            if (field == 5) arguments[10] = "--capture-uyvy";
            if (field == 6) arguments.resize(10);
            if (field == 7) arguments.insert(arguments.begin() + 8, "--mpeg1-via-mpeg2");
            if (field == 8) arguments.insert(arguments.begin() + 8, "--h263-via-divx");
            if (field == 9) arguments.insert(arguments.begin() + 8, "--open-only");
            Options rejected; check(!ParseArguments(arguments, &rejected), "MFD colour excludes other native shapes/codecs/repeats/noncapture/preflight scope");
        }
        for (const std::vector<const char *> &mixed : std::vector<std::vector<const char *>>{
                {"--observe-chroma"}, {"--observe-scl-config"}, {"--observe-scl-view", "2"},
                {"--observe-mfd-config"}, {"--inject-mfd-colour", "a"}})
            for (unsigned where : {8U, 10U}) {
                auto arguments = valid; arguments.insert(arguments.begin() + where, mixed.begin(), mixed.end());
                Options rejected; check(!ParseArguments(arguments, &rejected), "MFD colour rejects every observer mixture/order/duplicate");
            }
        Options rejected;
        check(!ParseArguments({"probe", "--self-test", "--inject-mfd-colour", name}, &rejected), "MFD colour self-test mix rejects");
        Input native; native.codec = AV_CODEC_ID_MPEG2VIDEO; native.subtype = BC_MSUBTYPE_MPEG2VIDEO;
        native.progressive = true; native.width = 640; native.height = 360; native.packets.resize(180);
        check(SclInputAdmitted(admitted, native), "MFD colour native input profile admitted after load before capture/device");
        for (unsigned field = 0; field < 7; ++field) {
            Options changed = admitted; Input altered = native;
            if (field == 0) altered.codec = AV_CODEC_ID_H264;
            if (field == 1) altered.subtype = BC_MSUBTYPE_H264;
            if (field == 2) altered.progressive = false;
            if (field == 3) altered.width = 638;
            if (field == 4) altered.height = 358;
            if (field == 5) altered.packets.pop_back();
            if (field == 6) changed.expected = 179;
            check(!SclInputAdmitted(changed, altered) && SclInputAdmitted(Options{}, altered), "MFD colour profile mismatches reject while default remains unchanged");
        }
    }
    std::printf("Library drain hardware-free self-test: %u checks %s\n",
                checks, ok ? "passed" : "failed");
    return ok;
}

static bool StartCode(const uint8_t *data, size_t size)
{
    return size >= 4 && data[0] == 0 && data[1] == 0 &&
           (data[2] == 1 || (data[2] == 0 && data[3] == 1));
}

static bool Load(const char *path, unsigned expected, Deadline *deadline, Input *input,
                 bool mpeg1_via_mpeg2, bool h263_via_divx = false)
{
    struct stat file;
    if (stat(path, &file) || !S_ISREG(file.st_mode) || file.st_size <= 0 ||
        file.st_size > 64 * 1024 * 1024) {
        std::fprintf(stderr, "Require a local regular fixture of at most 64 MiB\n");
        return false;
    }
    AVFormatContext *format = avformat_alloc_context();
    if (!format) return false;
    format->interrupt_callback = AVIOInterruptCB{Deadline::Check, deadline};
    if (avformat_open_input(&format, path, nullptr, nullptr) < 0) {
        avformat_close_input(&format);
        return false;
    }
    bool ok = avformat_find_stream_info(format, nullptr) >= 0;
    const int index = ok ? av_find_best_stream(format, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0) : -1;
    ok = index >= 0;
    if (ok) {
        const AVCodecParameters *parameters = format->streams[index]->codecpar;
        const char *demuxer = format->iformat->name;
        input->codec = parameters->codec_id;
        input->subtype = InputSubtype(input->codec, demuxer, mpeg1_via_mpeg2,
                                     h263_via_divx, parameters->extradata_size);
        ok = input->subtype != BC_MSUBTYPE_INVALID && parameters->width > 0 &&
             parameters->width <= 1920 && parameters->height > 0 && parameters->height <= 1088 &&
             (parameters->field_order == AV_FIELD_UNKNOWN || parameters->field_order == AV_FIELD_PROGRESSIVE);
        input->width = parameters->width;
        input->height = parameters->height;
        input->progressive = parameters->field_order == AV_FIELD_PROGRESSIVE;
        if (ok && input->subtype == BC_MSUBTYPE_WMV3) {
            // Same normalization as gstcrystalhd-codecs.h: four STRUCT_C bytes.
            ok = parameters->extradata &&
                 (parameters->extradata_size == 4 || parameters->extradata_size == 5);
            if (ok) input->metadata.assign(parameters->extradata, parameters->extradata + 4);
        }
    }
    AVPacket *packet = av_packet_alloc();
    ok = ok && packet;
    size_t total = 0;
    int result = 0;
    while (ok && !deadline->expired() && (result = av_read_frame(format, packet)) >= 0) {
        if (packet->stream_index == index) {
            Packet next;
            next.size = packet->size > 0 ? static_cast<size_t>(packet->size) : 0;
            ok = packet->data && !(packet->flags & AV_PKT_FLAG_CORRUPT) &&
                 gst_crystalhd_input_reservation(input->subtype, next.size,
                     input->metadata.size(), &next.reservation) &&
                 (h263_via_divx ? BaselineH263Picture(packet->data, next.size,
                                                     input->width, input->height) :
                  (input->subtype == BC_MSUBTYPE_WMV3 || StartCode(packet->data, next.size))) &&
                 input->packets.size() < expected && total + next.size <= 64 * 1024 * 1024;
            if (ok) {
                // The library may inspect a full startcode even for short ASF input.
                next.data.resize(next.size + AV_INPUT_BUFFER_PADDING_SIZE, 0);
                std::memcpy(next.data.data(), packet->data, next.size);
                total += next.size;
                input->packets.push_back(std::move(next));
            }
        }
        av_packet_unref(packet);
    }
    ok = ok && !deadline->expired() && result == AVERROR_EOF && input->packets.size() == expected;
    av_packet_free(&packet);
    avformat_close_input(&format);
    if (!ok) std::fprintf(stderr, "Unsupported/corrupt framing, deadline, or packet count (got %zu, expected %u)\n",
                          input->packets.size(), expected);
    return ok;
}

struct Device {
    HANDLE handle = nullptr;
    bool opened = false, started = false;
    bool Close() {
        bool ok = true;
        const auto record = [&](const char *operation, BC_STATUS status) {
            if (status != BC_STS_SUCCESS) {
                std::fprintf(stderr, "%s failed: %d\n", operation, status);
                ok = false;
            }
        };
        if (started) record("DtsStopDecoder", DtsStopDecoder(handle));
        started = false;
        if (opened) record("DtsCloseDecoder", DtsCloseDecoder(handle));
        opened = false;
        if (handle) record("DtsDeviceClose", DtsDeviceClose(handle));
        handle = nullptr;
        return ok;
    }
    ~Device() { if (handle) Close(); }
};

static bool Status(const char *operation, BC_STATUS status)
{
    if (status == BC_STS_SUCCESS) return true;
    std::fprintf(stderr, "%s failed: %d\n", operation, status);
    return false;
}

// Named packing-control observation only; never follow device pointers.
static bool PackingState(HANDLE handle, const char *stage, MfdColourProbe *colour,
                         SclViewProbe *view, SclViewProbe::Reader reader, bool report)
{
    if ((colour && colour->Enabled() && colour->failed) || (view && view->Enabled() && view->failed)) return false;
    uint32_t control = 0;
    const BC_STATUS status = reader(handle, CRYSTALHD_FLEA_COLOR_REGISTER, &control);
    if (status != BC_STS_SUCCESS) {
        if (colour && colour->Enabled()) { colour->api_status = status; colour->Reject(MfdColourFailure::Api); }
        if (view && view->Enabled()) { view->api_status = status; view->Reject(SclViewFailure::Api); }
        if (report) Status("DtsDevRegisterRead(packing)", status);
        return false;
    }
    if (report) std::printf("Packing state: stage=%s register=0x%08x raw=0x%08x yuy2-bit=%u\n",
                stage, CRYSTALHD_FLEA_COLOR_REGISTER, control,
                (control & CRYSTALHD_FLEA_COLOR_YUY2) != 0);
    std::fflush(stdout);
    return true;
}

static bool ChromaState(HANDLE handle, const char *stage)
{
    ChromaConfiguration config;
    if (!Status("DtsDevRegisterRead(chroma configuration)", ReadChromaConfiguration(handle, &config)))
        return false;
    std::printf("Chroma state: stage=%s lac-register=0x%08x lac-raw=0x%08x "
                "sampling-register=0x%08x sampling-raw=0x%08x reposition=%u "
                "vert-position=%u interpolation=%u\n",
                stage, BCHP_MFD_LAC_CNTL, config.lac, BCHP_MFD_CHROMA_SAMPLING_CNTL,
                config.sampling,
                (config.sampling & BCHP_MFD_CHROMA_SAMPLING_CNTL_CHROMA_REPOSITION_ENABLE_MASK) >>
                    BCHP_MFD_CHROMA_SAMPLING_CNTL_CHROMA_REPOSITION_ENABLE_SHIFT,
                (config.lac & BCHP_MFD_LAC_CNTL_CHROMA_VERT_POSITION_MASK) >>
                    BCHP_MFD_LAC_CNTL_CHROMA_VERT_POSITION_SHIFT,
                (config.lac & BCHP_MFD_LAC_CNTL_CHROMA_INTERPOLATION_MASK) >>
                    BCHP_MFD_LAC_CNTL_CHROMA_INTERPOLATION_SHIFT);
    std::fflush(stdout);
    return true;
}

struct OutputLease {
    HANDLE handle;
    bool active = true;
    explicit OutputLease(HANDLE value) : handle(value) {}
    OutputLease(const OutputLease &) = delete;
    OutputLease &operator=(const OutputLease &) = delete;
    ~OutputLease() { if (active) Release(); }
    bool Release() {
        if (!active) return false;
        active = false;
        return Status("DtsReleaseOutputBuffs", DtsReleaseOutputBuffs(handle, nullptr, FALSE));
    }
};

struct Audit {
    unsigned frames = 0;
    bool marker = false, eos = false, packing_format_observed = false;
    uint32_t ready = 0;
    std::set<uint64_t> pending;
    unsigned iteration = 0;
    Phase1Progress *progress = nullptr;
    unsigned output_width = 0, output_height = 0;
    BC_OUTPUT_FORMAT output_format = OUTPUT_MODE422_YUY2;
    bool observe_chroma = false;
    unsigned expected = 0;
    SclObserver scl;
    SclViewProbe scl_view;
    MfdAdmissionObserver mfd;
    MfdColourProbe mfd_colour;
    GChecksum *pixels = nullptr;
    PixelCapture *capture = nullptr;
    ~Audit() { if (pixels) g_checksum_free(pixels); }
};

static bool Receive(Device *device, const Input &input, Audit *audit)
{
    for (unsigned attempt = 0; attempt < 32; ++attempt) {
        BC_DTS_STATUS driver = {};
        if (!Status("DtsGetDriverStatus", DtsGetDriverStatus(device->handle, &driver))) return false;
        audit->ready = driver.ReadyListCount;
        if (!audit->ready) break;
        BC_DTS_PROC_OUT output = {};
        const BC_STATUS result = DtsProcOutputNoCopy(device->handle, 0, &output);
        const bool marker = result == BC_STS_SUCCESS &&
            (output.PicInfo.flags & VDEC_FLAG_EOS) != 0;
        if (result == BC_STS_SUCCESS) {
            OutputLease lease(device->handle);
            CapturedFrame captured;
            audit->marker |= marker;
            bool valid = true;
            if (!marker) {
                const unsigned width = output.PicInfo.width, height = output.PicInfo.height;
                valid = (output.PoutFlags & BC_POUT_FLAGS_PIB_VALID) && output.Ybuff &&
                    !(output.PicInfo.flags & VDEC_FLAG_INTERLACED_SRC) &&
                    width == audit->output_width &&
                    (height == audit->output_height ||
                     (!audit->pixels && input.height == 1080 && height == 1088)) &&
                    static_cast<uint64_t>(output.YBuffDoneSz) * 4 >= static_cast<uint64_t>(width) * height * 2 &&
                    audit->pending.erase(output.PicInfo.timeStamp) == 1;
                if (valid && audit->pixels)
                    valid = HashActivePixels(audit->pixels, output, width, height, audit->output_format);
                if (valid && audit->capture)
                    valid = CopyCapturedPixels(output, audit->output_width, audit->output_height,
                                               audit->capture->frame_bytes, &captured, audit->output_format);
                if (valid) ++audit->frames;
            }
            // Every successful NoCopy fetch owns a lease, even invalid output.
            const bool released = lease.Release();
            // File I/O uses only the owned copy, after the lease was released.
            if (!marker && valid && released && audit->capture &&
                !audit->capture->Write(captured)) {
                std::fprintf(stderr, "%s capture write/budget failure\n", PackedName(audit->output_format));
                valid = false;
            }
            // Preserve an already delivered owned copy before diagnostic failure.
            if (!marker && valid && released && audit->frames == 1)
                valid = audit->scl.Observe(device->handle, SclStage::FirstReleased);
            if (!marker && valid && released && audit->frames == audit->expected)
                valid = audit->scl.Observe(device->handle, SclStage::LastReleased);
            if (!marker && valid && released)
                valid = audit->scl_view.AfterDelivered(device->handle, audit->frames,
                                                      released, audit->capture != nullptr);
            if (!marker && valid && released)
                valid = audit->mfd.AfterDelivered(device->handle, audit->frames,
                                                 released, audit->capture != nullptr);
            if (!marker && valid && released)
                valid = audit->mfd_colour.AfterDelivered(device->handle, audit->frames,
                                                        released, audit->capture != nullptr);
            if (!marker && valid && released && audit->capture && audit->frames == 1)
                valid = PackingState(device->handle, "first-output", &audit->mfd_colour, &audit->scl_view);
            if (!marker && valid && released && audit->observe_chroma && audit->frames == 1)
                valid = ChromaState(device->handle, "first-output-after-release");
            if (!marker && valid && released && audit->progress)
                phase1_progress_write(audit->progress,
                    "probe=library-drain iteration=%u frame-index=%u token=%llu\n",
                    audit->iteration, audit->frames - 1,
                    static_cast<unsigned long long>(output.PicInfo.timeStamp));
            if (!valid) {
                if (audit->mfd_colour.failed)
                    std::fprintf(stderr, "MFD colour experiment failed; guarded restoration if eligible and ordinary decoder cleanup follow\n");
                else if (audit->mfd.failed)
                    std::fprintf(stderr, "MFD passive admission failed; ordinary decoder cleanup follows\n");
                else if (audit->scl_view.failed)
                    std::fprintf(stderr, "SCL test-view experiment failed; guarded restoration if eligible and ordinary decoder cleanup follow\n");
                else if (audit->scl.failed)
                    std::fprintf(stderr, "SCL raw configuration observation failed; ordinary decoder cleanup follows\n");
                else
                    std::fprintf(stderr, "Invalid progressive picture geometry/data/token: %llu\n",
                                 static_cast<unsigned long long>(output.PicInfo.timeStamp));
                if (audit->pixels && !audit->scl.failed && !audit->scl_view.failed && !audit->mfd.failed && !audit->mfd_colour.failed)
                    std::fprintf(stderr, "Scaler picture: got=%ux%u expected=%ux%u "
                                 "flags=%x words=%u packed422=%u\n",
                                 output.PicInfo.width, output.PicInfo.height,
                                 audit->output_width, audit->output_height,
                                 output.PicInfo.flags, output.YBuffDoneSz, output.b422Mode);
            }
            if (!valid || !released) return false;
        } else if (result == BC_STS_FMT_CHANGE && audit->capture &&
                   !audit->packing_format_observed) {
            audit->packing_format_observed = true;
            // FMT_CHANGE is an API notification, not a lease/retirement proof.
            if (!audit->scl.Observe(device->handle, SclStage::FormatChange)) return false;
            if (!PackingState(device->handle, "first-format-change", &audit->mfd_colour, &audit->scl_view)) return false;
            if (audit->observe_chroma && !ChromaState(device->handle, "first-format-change")) return false;
        } else if (result != BC_STS_FMT_CHANGE && result != BC_STS_NO_DATA &&
                   result != BC_STS_BUSY && result != BC_STS_TIMEOUT) {
            return Status("DtsProcOutputNoCopy", result);
        } else if (result != BC_STS_FMT_CHANGE && !marker) {
            break;
        }
    }
    uint8_t eos = 0;
    if (!Status("DtsIsEndOfStream", DtsIsEndOfStream(device->handle, &eos))) return false;
    audit->eos = eos != 0;
    return true;
}

static bool WaitInput(Device *device, const Input &input, Audit *audit,
                      gsize reservation, const Deadline &deadline)
{
    while (!deadline.expired()) {
        if (!Receive(device, input, audit)) return false;
        if (DtsTxFreeSize(device->handle) >= reservation) return true;
        g_usleep(1000);
    }
    std::fprintf(stderr, "Input admission deadline expired\n");
    return false;
}

static bool Run(Input &input, unsigned expected, unsigned seconds,
                unsigned iteration, unsigned iterations,
                Phase1Progress *progress, const Options &options)
{
    Deadline deadline(seconds);
    Device device;
    Audit audit;
    audit.iteration = iteration;
    audit.progress = progress;
    audit.output_width = input.width;
    audit.output_height = input.height;
    audit.output_format = options.output_format;
    audit.observe_chroma = options.observe_chroma;
    audit.expected = expected;
    audit.scl.enabled = options.observe_scl_config;
    audit.scl_view.selector = options.observe_scl_view;
    audit.mfd.enabled = options.observe_mfd_config;
    audit.mfd_colour.stimulus = options.inject_mfd_colour;
    if (options.scaler_test) {
        if (!ScalerGeometry(input.width, input.height, options.scale_width,
                            &audit.output_width, &audit.output_height)) return false;
        audit.pixels = g_checksum_new(G_CHECKSUM_SHA256);
        if (!audit.pixels) return false;
    }
    PixelCapture capture;
    if (!capture.Open(options.capture_path, audit.output_width, audit.output_height, expected,
                      options.output_format))
        return false;
    audit.capture = options.capture_path ? &capture : nullptr;
    const uint32_t mode = ProbeDeviceMode(options);
    bool ok = Status("DtsDeviceOpen", DtsDeviceOpen(&device.handle, mode));
    BC_INFO_CRYSTAL version = {};
    if (ok) ok = Status("DtsCrystalHDVersion", DtsCrystalHDVersion(device.handle, &version));
    if (ok && version.device != 1) {
        std::fprintf(stderr, "This hardware probe is restricted to BCM70015\n");
        ok = false;
    }
    BC_INPUT_FORMAT format = {};
    // Match the production GStreamer input format; no AVC1/RCV conversion.
    format.Progressive = TRUE;
    format.OptFlags = 0x80000000U | vdecFrameRate59_94 | 0x40U;
    format.mSubtype = input.subtype;
    format.width = input.width;
    format.height = input.height;
    format.startCodeSz = input.subtype == BC_MSUBTYPE_H264 ? 4 : 0;
    format.pMetaData = input.metadata.empty() ? nullptr : input.metadata.data();
    format.metaDataSz = input.metadata.size();
    if (ok) ok = Status("DtsSetInputFormat", DtsSetInputFormat(device.handle, &format));
    if (ok && options.scale_width) {
        // The format setter may select its historical single-thread width.
        // Override that cache before OPEN; this is not a live channel update.
        BC_SCALING_PARAMS scaling = {};
        scaling.sWidth = options.scale_width;
        ok = Status("DtsSetScaleParams", DtsSetScaleParams(device.handle, &scaling));
    }
    if (ok) device.opened = ok = Status("DtsOpenDecoder", DtsOpenDecoder(device.handle, BC_STREAM_TYPE_ES));
    if (options.open_only) {
        const bool opened = device.opened;
        const bool closed = device.Close();
        ok = ok && opened && closed && !deadline.expired();
        std::printf("H.263 research OPEN-only: iteration=%u/%u opened=%s "
                    "cleanup=%s result=%s\n", iteration, iterations,
                    opened ? "yes" : "no", closed ? "PASS" : "FAIL", ok ? "PASS" : "FAIL");
        std::fflush(stdout);
        return ok;
    }
    if (ok) ok = Status("DtsSetColorSpace", DtsSetColorSpace(device.handle, options.output_format));
    if (ok && options.capture_path) ok = PackingState(device.handle, "selected-before-start", &audit.mfd_colour, &audit.scl_view);
    if (ok) ok = audit.scl.Observe(device.handle, SclStage::PreStart);
    if (ok) ok = audit.scl_view.Begin(device.handle);
    if (ok) ok = audit.mfd.Observe(device.handle, 0);
    if (ok) ok = audit.mfd_colour.PreStart(device.handle);
    if (ok) device.started = ok = Status("DtsStartDecoder", DtsStartDecoder(device.handle));
    if (ok && options.capture_path) ok = PackingState(device.handle, "started", &audit.mfd_colour, &audit.scl_view);
    if (ok) ok = Status("DtsStartCapture", DtsStartCapture(device.handle));
    if (ok && options.capture_path) ok = PackingState(device.handle, "capture-before-input", &audit.mfd_colour, &audit.scl_view);
    size_t packet_index = 0;
    for (Packet &packet : input.packets) {
        if (!ok) break;
        const uint64_t token = Token(iteration - 1, packet_index);
        ok = WaitInput(&device, input, &audit, packet.reservation, deadline) &&
             Status("DtsProcInput", DtsProcInput(device.handle, packet.data.data(),
                    packet.size, token, FALSE));
        if (ok) audit.pending.insert(token);
        ++packet_index;
        if (ok && options.capture_path && packet_index == 1)
            ok = PackingState(device.handle, "first-input-accepted", &audit.mfd_colour, &audit.scl_view);
    }
    if (ok && options.capture_path) ok = PackingState(device.handle, "all-input-accepted", &audit.mfd_colour, &audit.scl_view);
    if (ok) {
        ok = WaitInput(&device, input, &audit, GST_CRYSTALHD_EOS_RESERVATION,
                       deadline);
        if (ok && !FreshBeforeFlush(audit.eos)) {
            std::fprintf(stderr,
                         "DtsIsEndOfStream was already set before this drain\n");
            ok = false;
        }
        if (ok)
            ok = Status("DtsFlushInput(0)", DtsFlushInput(device.handle, 0));
        if (ok && options.capture_path) ok = PackingState(device.handle, "flush-input-returned", &audit.mfd_colour, &audit.scl_view);
    }
    // A complete frame count is deliberately NOT the termination condition.
    while (ok && !deadline.expired()) {
        ok = Receive(&device, input, &audit);
        // EOS reaching the decoder is not itself a delivery barrier: a driver
        // status poll can observe it before every accepted picture is fetched.
        if (!ok || (audit.eos && audit.ready == 0 && audit.pending.empty())) break;
        g_usleep(1000);
    }
    // Native paths derive EOS from the firmware timing marker, which need not
    // appear as a successful NoCopy lease. DIVX also has a library idle-fence
    // fallback: its EOS state alone does not prove firmware-marker consumption.
    // In either case require complete frame delivery and token retirement.
    ok = ok && !deadline.expired() && audit.eos && audit.ready == 0 &&
         audit.frames == expected && audit.pending.empty();
    // Delivery EOS barrier only, sampled before ordinary STOP/CLOSE.
    if (ok) ok = audit.scl.Observe(device.handle, SclStage::EosBarrier);
    // A failed experiment remains failed even if guarded restoration succeeds.
    // Never issue cleanup experiment I/O after an access/selector-loss latch.
    const bool view_restored = audit.scl_view.Restore(device.handle);
    const bool colour_restored = audit.mfd_colour.Restore(device.handle);
    ok = ok && view_restored && colour_restored;
    const bool closed = device.Close();
    ok = ok && closed;
    const bool captured = capture.Finish(ok);
    ok = ok && captured;
    std::printf("Library drain: iteration=%u/%u frames=%u/%u pending=%zu "
        "%s-EOS=%s output-marker=%s ready=%u cleanup=%s result=%s\n",
        iteration, iterations, audit.frames, expected, audit.pending.size(),
        options.h263_via_divx ? "library" : "firmware",
        audit.eos ? "yes" : "no", audit.marker ? "yes" : "no", audit.ready,
        closed ? "PASS" : "FAIL", ok ? "PASS" : "FAIL");
    std::fflush(stdout);
    if (audit.pixels) {
        std::printf("Scaler test: iteration=%u/%u requested-width=%u expected-output=%ux%u "
                    "%s-sha256=%s result=%s\n", iteration, iterations,
                    options.scale_width, audit.output_width, audit.output_height,
                    options.output_format == OUTPUT_MODE422_UYVY ? "requested-uyvy" : "yuy2",
                    g_checksum_get_string(audit.pixels), ok ? "PASS" : "FAIL");
        std::fflush(stdout);
    }
    return ok;
}

int main(int argc, char **argv)
{
    const std::vector<const char *> arguments(argv, argv + argc);
    Options options;
    if (!ParseArguments(arguments, &options)) {
        std::fprintf(stderr, "usage: %s --self-test | --preflight LOCAL_VIDEO "
            "EXPECTED_FRAMES [TIMEOUT_SECONDS] | --hardware LOCAL_VIDEO "
            "EXPECTED_FRAMES [TIMEOUT_SECONDS [ITERATIONS]] "
            "[--scaler-test WIDTH_OR_0] [--mpeg1-via-mpeg2 | --h263-via-divx] "
            "[--open-only] [--observe-chroma | --observe-scl-config | --observe-scl-view 2_OR_3 | --observe-mfd-config | --inject-mfd-colour a_OR_b] "
            "[--capture-yuy2 NEW_PATH | --capture-uyvy NEW_PATH]\n", argv[0]);
        return 2;
    }
    if (options.mode == Mode::SelfTest) return SelfTest() ? 0 : 1;
    if (NeedsRawIo(options) && !CanReadChromaConfiguration()) {
        if (options.inject_mfd_colour)
            std::fprintf(stderr, "--inject-mfd-colour requires CAP_SYS_RAWIO; no fixture/capture/device was opened\n");
        else if (options.observe_mfd_config)
            std::fprintf(stderr, "--observe-mfd-config requires CAP_SYS_RAWIO; no fixture/capture/device was opened\n");
        else if (options.observe_scl_view)
            std::fprintf(stderr, "--observe-scl-view requires CAP_SYS_RAWIO; no fixture/capture/device was opened\n");
        else if (options.observe_scl_config)
            std::fprintf(stderr, "--observe-scl-config requires CAP_SYS_RAWIO; no fixture/capture/device was opened\n");
        else
            std::fprintf(stderr, "--observe-chroma requires CAP_SYS_RAWIO; no device was opened\n");
        return 2;
    }
    Phase1Progress progress{};
    if (!phase1_progress_open(&progress)) {
        std::fprintf(stderr, "Could not open Phase 1 progress record\n");
        return 2;
    }
    phase1_progress_write(&progress, "probe=library-drain state=loading-fixture\n");
    std::signal(SIGINT, Interrupt);
    std::signal(SIGTERM, Interrupt);
    Deadline deadline(options.seconds);
    Input input;
    if (!Load(options.path, options.expected, &deadline, &input,
              options.mpeg1_via_mpeg2, options.h263_via_divx)) {
        phase1_progress_close(&progress);
        return 2;
    }
    if (!SclInputAdmitted(options, input)) {
        std::fprintf(stderr, "Raw observation requires native progressive MPEG2 "
                             "640x360 with 180 packets/expected frames; no device was opened\n");
        phase1_progress_close(&progress);
        return 2;
    }
    std::printf("Preflight: %ux%u subtype=%u packets=%zu metadata=%zu\n",
                input.width, input.height, input.subtype, input.packets.size(), input.metadata.size());
    if (options.mpeg1_via_mpeg2)
        std::printf("MPEG-1 research: input-codec=%s configured-algorithm=1 "
                    "route=MPEG2VIDEO selector5-not-used\n",
                    avcodec_get_name(input.codec));
    if (options.h263_via_divx)
        std::printf("H.263 research: input-codec=%s configured-algorithm=6 "
                    "route=DIVX/PES metadata=empty picture-header=baseline "
                    "packet-bytes=unchanged open-only=%s\n",
                    avcodec_get_name(input.codec), options.open_only ? "yes" : "no");
    if (options.scaler_test) {
        unsigned width = 0, height = 0;
        if (!ScalerGeometry(input.width, input.height, options.scale_width, &width, &height)) {
            std::fprintf(stderr, "Require even progressive geometry and no upscaling\n");
            phase1_progress_close(&progress);
            return 2;
        }
        std::printf("Scaler preflight: requested-width=%u expected=%ux%u\n",
                    options.scale_width, width, height);
    }
    if (options.mode == Mode::Preflight) {
        phase1_progress_close(&progress);
        return 0;
    }

    Resources baseline;
    if (!SampleResources(&baseline)) {
        std::fprintf(stderr, "Unable to sample process resources\n");
        phase1_progress_close(&progress);
        return 2;
    }
    ReportResources(0, options.iterations, baseline);
    Resources current = baseline;
    std::vector<unsigned long> rss_samples{baseline.rss_kib};
    unsigned long peak_rss = baseline.rss_kib;
    unsigned completed = 0;
    bool ok = true;
    for (unsigned iteration = 1; iteration <= options.iterations; ++iteration) {
        phase1_progress_write(&progress,
            "probe=library-drain iteration=%u/%u state=starting last-complete=%u\n",
            iteration, options.iterations, completed);
        ok = Run(input, options.expected, options.seconds, iteration,
                 options.iterations, &progress, options);
        if (!SampleResources(&current)) {
            std::fprintf(stderr, "Unable to sample resources after iteration %u\n",
                         iteration);
            ok = false;
        } else {
            rss_samples.push_back(current.rss_kib);
            if (current.rss_kib > peak_rss) peak_rss = current.rss_kib;
            if (iteration == 1 || iteration % 10 == 0 ||
                iteration == options.iterations || !ok)
                ReportResources(iteration, options.iterations, current);
            if (current.fds != baseline.fds || current.threads != baseline.threads) {
                std::fprintf(stderr, "Resource drift after iteration %u: "
                    "fds=%u/%u threads=%u/%u\n", iteration, current.fds,
                    baseline.fds, current.threads, baseline.threads);
                ok = false;
            }
        }
        if (!ok) break;
        completed = iteration;
    }
    unsigned long early_rss = 0, late_rss = 0;
    if (ok && !BoundedRssGrowth(rss_samples, kRssGrowthLimitKiB,
                                &early_rss, &late_rss)) {
        std::fprintf(stderr, "Sustained RSS growth exceeded %lu KiB: "
                     "early-window=%lu late-window=%lu samples=%zu\n",
                     kRssGrowthLimitKiB, early_rss, late_rss,
                     rss_samples.size());
        ok = false;
    }
    const long long rss_delta = static_cast<long long>(current.rss_kib) -
                                static_cast<long long>(baseline.rss_kib);
    std::printf("Library churn: completed=%u/%u rss-kib=%lu/%lu peak=%lu "
        "delta=%+lld fds=%u/%u threads=%u/%u result=%s\n", completed,
        options.iterations, current.rss_kib, baseline.rss_kib, peak_rss,
        rss_delta, current.fds, baseline.fds, current.threads,
        baseline.threads, ok && completed == options.iterations ? "PASS" : "FAIL");
    phase1_progress_close(&progress);
    return ok && completed == options.iterations ? 0 : 1;
}
