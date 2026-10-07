// SPDX-License-Identifier: LGPL-2.1-or-later
// Optional direct-library drain probe, not a pixel-quality benchmark.
// Use an external timeout as well: a userspace deadline cannot bound a stuck
// kernel ioctl or device close. --preflight never opens the CrystalHD device.
#include <bc_dts_types.h>
#include <libcrystalhd_if.h>
#include "crystalhd_ioctl_limits.h"
#include "flea/70015/magnum/basemodules/chp/70015/rdb/a0/bchp_mfd.h"
#include "flea/70015/magnum/basemodules/chp/70015/rdb/a0/bchp_scl_hd.h"
#include "flea/70015/magnum/basemodules/chp/70015/rdb/a0/bchp_sun_gisb_arb.h"
#include "flea/70015/magnum/basemodules/chp/70015/rdb/a0/bchp_decode_cpuimem_0.h"
#include "flea/70015/magnum/basemodules/chp/70015/rdb/a0/bchp_decode_cpudmem_0.h"
#include "flea/70015/magnum/basemodules/chp/70015/rdb/a0/bchp_decode_cpuimem2_0.h"
#include "flea/70015/magnum/basemodules/chp/70015/rdb/a0/bchp_decode_cpudmem2_0.h"
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
extern "C" BC_STATUS DtsDevMemRd(HANDLE handle, uint32_t *buffer, uint32_t bytes, uint32_t offset);

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

// Read-only native-path map, not an active-bank, arithmetic or transport
// certificate. The fixed whitelist excludes TEST/DATA, CLEAR and scratch.
// Both sequential tuples are retained; matching cannot prove atomicity.
static const unsigned kSclFilterMapFields = 216;
enum class SclFilterMapFailure {
    None, Argument, Read, Revision, Reserved, Unavailable, Admission, Order, Unstable
};
static uint32_t SclFilterMapAddress(unsigned field)
{
    if (field < 22) return BCHP_SCL_HD_REVISION_ID + field * 4;
    if (field == 22) return BCHP_SCL_HD_BVB_IN_STATUS;
    if (field < 215) return BCHP_SCL_HD_VERT_FIR_COEFF_PHASE0_00_01 + (field - 23) * 4;
    return field == 215 ? BCHP_SCL_HD_REVISION_ID : 0;
}
static uint32_t SclFilterMapMask(unsigned field)
{
    // Nonreserved RDB fields, in REV..ENABLE address order.
    static const uint32_t control_masks[22] = {
        0x0000ffffU, 0x0000000eU, 0x000001f7U, 0x000000ffU,
        0x07ff07ffU, 0x07ff07ffU, 0x07ff07ffU, 0x07ff07ffU,
        0x003f0000U, 0xfffffff8U, 0x03fffff8U, 0x001fc000U,
        0xffffc000U, 0xffffc000U, 0xffffffffU, 0x7fffffffU,
        0x3ffffffcU, 0x3ffffffcU, 0x07ff0000U, 0x07ff0000U,
        0x07ff0000U, 0x00000001U
    };
    if (field < 22) return control_masks[field];
    if (field == 22) return 0xffU;
    if (field < 215)
        return BCHP_SCL_HD_VERT_FIR_COEFF_PHASE0_00_01_COEFF_0_MASK |
               BCHP_SCL_HD_VERT_FIR_COEFF_PHASE0_00_01_COEFF_1_MASK;
    return field == 215 ? 0xffffU : 0;
}
static unsigned SclFilterMapEven(uint32_t raw)
{
    return (raw & BCHP_SCL_HD_VERT_FIR_COEFF_PHASE0_00_01_COEFF_0_MASK) >>
        BCHP_SCL_HD_VERT_FIR_COEFF_PHASE0_00_01_COEFF_0_SHIFT;
}
static unsigned SclFilterMapOdd(uint32_t raw)
{
    return (raw & BCHP_SCL_HD_VERT_FIR_COEFF_PHASE0_00_01_COEFF_1_MASK) >>
        BCHP_SCL_HD_VERT_FIR_COEFF_PHASE0_00_01_COEFF_1_SHIFT;
}
static SclFilterMapFailure SclFilterMapScalarFailure(unsigned field, uint32_t raw)
{
    if (field >= kSclFilterMapFields) return SclFilterMapFailure::Argument;
    // Even a full-width field cannot distinguish all-ones from an inaccessible
    // read portal. This is conservative admission, not a claimed illegal value.
    if (raw == 0xffffffffU) return SclFilterMapFailure::Unavailable;
    if (field == 0 || field == 215)
        return raw == 0x80U ? SclFilterMapFailure::None : SclFilterMapFailure::Revision;
    return raw & ~SclFilterMapMask(field) ?
        SclFilterMapFailure::Reserved : SclFilterMapFailure::None;
}
struct SclFilterMapSnapshot {
    uint32_t raw[2][kSclFilterMapFields] = {};
    unsigned reads = 0, measured = 0;
    BC_STATUS status = BC_STS_SUCCESS;
    SclFilterMapFailure failure = SclFilterMapFailure::None;
    bool Complete() const { return measured == 2 * kSclFilterMapFields; }
    bool Stable() const {
        return Complete() && !std::memcmp(raw[0], raw[1], sizeof(raw[0]));
    }
    bool StatusObserved() const {
        return (measured > 22 && raw[0][22]) ||
               (measured > kSclFilterMapFields + 22 && raw[1][22]);
    }
};
static bool ReadSclFilterMap(HANDLE handle, SclFilterMapSnapshot *snapshot,
    BC_STATUS (*reader)(HANDLE, uint32_t, uint32_t *) = DtsDevRegisterRead)
{
    if (!snapshot) return false;
    *snapshot = SclFilterMapSnapshot{};
    if (!handle || !reader) {
        snapshot->failure = SclFilterMapFailure::Argument;
        return false;
    }
    for (unsigned pass = 0; pass < 2; ++pass) {
        for (unsigned field = 0; field < kSclFilterMapFields; ++field) {
            uint32_t raw = 0;
            ++snapshot->reads;
            snapshot->status = reader(handle, SclFilterMapAddress(field), &raw);
            if (snapshot->status != BC_STS_SUCCESS) {
                snapshot->failure = SclFilterMapFailure::Read;
                return false; // A callback's poisoned output is NOT-READ.
            }
            snapshot->raw[pass][field] = raw;
            ++snapshot->measured;
            snapshot->failure = SclFilterMapScalarFailure(field, raw);
            if (snapshot->failure != SclFilterMapFailure::None) return false;
        }
    }
    return true;
}
struct SclFilterMapObserver {
    typedef BC_STATUS (*Reader)(HANDLE, uint32_t, uint32_t *);
    bool enabled = false, attempted = false, failed = false, fatal = false;
    HANDLE owner = nullptr;
    SclFilterMapSnapshot snapshot;
    bool Reject(SclFilterMapFailure why) {
        failed = fatal = true;
        snapshot.failure = why;
        return false;
    }
    void Report() const {
        const char *const names[22] = {"rev", "top", "vert", "horiz", "bvb-size",
            "pic-offset", "src-size", "dest-size", "vpan", "v-offset", "v-step",
            "hpan", "hy-offset", "hc-offset", "h-phase", "h-step", "region0-delta",
            "region2-delta", "region0-end", "region1-end", "region2-end", "enable"};
        const char *const failures[] = {"none", "argument", "read-status", "revision",
            "reserved-bits", "all-ones-unavailable", "admission", "order", "unequal-tuples"};
        std::printf("SCL filter map: stage=first-output-after-release-and-owned-write "
            "attempted=%s reads=%u measured=%u api-status=%d failure=%s raw-stable=%s "
            "observed-status=%s diagnostic=%s fatal=%s target-writes=0 "
            "atomic=no transport-certified=no active-bank/source/lease/arithmetic-certified=no\n",
            attempted ? "yes" : "no", snapshot.reads, snapshot.measured, snapshot.status,
            failures[static_cast<unsigned>(snapshot.failure)],
            snapshot.Complete() ? (snapshot.Stable() ? "yes" : "no") : "NOT-READ",
            snapshot.StatusObserved() ? "yes" : "no", failed ? "FAIL" : "PASS", fatal ? "yes" : "no");
        for (unsigned pass = 0; pass < 2; ++pass) {
            for (unsigned field = 0; field < kSclFilterMapFields; ++field) {
                std::printf("SCL filter raw: pass=%u field=%u address=%08x ",
                    pass, field, SclFilterMapAddress(field));
                if (field < 22) std::printf("name=%s ", names[field]);
                else if (field == 22 || field == 215)
                    std::printf("name=%s ", field == 22 ? "status" : "closing-rev");
                else {
                    const unsigned word = field - 23;
                    const unsigned bank = word < 32 ? 0 : word < 64 ? 1 : word < 128 ? 2 : 3;
                    const unsigned local = word - (bank == 0 ? 0 : bank == 1 ? 32 : bank == 2 ? 64 : 128);
                    const unsigned pairs = bank < 2 ? 4 : 8;
                    const char *const banks[] = {"VY", "VC", "HY", "HC"};
                    std::printf("bank=%s phase=%u even-tap=%u ", banks[bank], local / pairs,
                        (local % pairs) * 2);
                }
                if (pass * kSclFilterMapFields + field < snapshot.measured) {
                    const uint32_t raw = snapshot.raw[pass][field];
                    std::printf("raw=%08x", raw);
                    if (field >= 23 && field < 215)
                        std::printf(" even12=%u odd12=%u", SclFilterMapEven(raw), SclFilterMapOdd(raw));
                } else std::printf("raw=NOT-READ");
                std::printf("\n");
            }
        }
        std::fflush(stdout);
    }
    bool AfterDelivered(HANDLE handle, unsigned frame, bool released, bool owned_written,
        Reader reader = DtsDevRegisterRead, bool report = true) {
        if (!enabled) return true;
        if (fatal) return false;
        if (!reader) return Reject(SclFilterMapFailure::Argument);
        if (!handle || !owner || handle != owner || !released || !owned_written)
            return Reject(SclFilterMapFailure::Admission);
        if (!frame || (!attempted && frame != 1) || (attempted && frame == 1))
            return Reject(SclFilterMapFailure::Order);
        if (attempted) return true; // No retries or later-stage reads.
        attempted = true;
        const bool read_ok = ReadSclFilterMap(handle, &snapshot, reader);
        fatal = !read_ok;
        failed = fatal || !snapshot.Stable() || snapshot.StatusObserved();
        if (read_ok && !snapshot.Stable()) snapshot.failure = SclFilterMapFailure::Unstable;
        if (report) Report();
        return !fatal; // Status/inequality cannot erase delivered native frames.
    }
    bool Finish(bool native_ok, bool report = true) const {
        if (!enabled) return native_ok;
        const bool diagnostic_ok = attempted && !failed && !fatal;
        if (report) {
            std::printf("SCL filter finish: native-result=%s diagnostic-result=%s reads=%u "
                "target-writes=0 standalone/arithmetic-certified=no\n",
                native_ok ? "PASS" : "FAIL", diagnostic_ok ? "PASS" : "FAIL", snapshot.reads);
            std::fflush(stdout);
        }
        return native_ok && diagnostic_ok;
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

// One query on the already initialized native owner. GET_VERSION sends a
// command/doorbell; it is not a passive snapshot or a firmware side-effect
// certificate. No extra download, INIT or target-configuration write is added.
struct RuntimeInventoryObserver {
    using Query = BC_STATUS (*)(HANDLE, uint32_t *, uint32_t *, uint32_t *, char *, uint32_t);
    bool enabled = false, attempted = false, failed = false, measured = false;
    unsigned queries = 0;
    HANDLE owner = nullptr;
    BC_STATUS status = BC_STS_SUCCESS;
    uint32_t raw[3] = {};
    bool Observe(const HANDLE *current, Query query = DtsGetFWVersion, bool report = true) {
        if (!enabled) return true;
        if (failed) return false;
        attempted = true;
        if (!current || !*current || !query || queries) {
            failed = true;
        } else {
            owner = *current;
            uint32_t reply[3] = {};
            ++queries;
            status = query(owner, &reply[0], &reply[1], &reply[2], nullptr, 1);
            failed = status != BC_STS_SUCCESS || !*current || *current != owner;
            if (!failed) {
                std::memcpy(raw, reply, sizeof(raw));
                measured = true;
            }
        }
        if (report) {
            std::printf("Runtime inventory: stage=after-OPEN/pre-START queries=%u api-status=%d result=%s "
                "source=firmware-reported-GET_VERSION passive=no target-config-writes=0 "
                "extra-download/INIT=0 reply-header-match/transport-certified=no\n",
                queries, status, failed ? "FAIL" : "PASS");
            const char *const names[] = {"stream-sw", "decoder-sw", "chip-hw"};
            for (unsigned field = 0; field < 3; ++field) {
                std::printf("Runtime inventory raw: %s=", names[field]);
                if (measured && !failed) std::printf("%08x", raw[field]);
                else std::printf("NOT-READ");
                std::printf("\n");
            }
            std::fflush(stdout);
        }
        return !failed;
    }
    bool Finish(bool native_ok, const MfdAdmissionObserver &mfd, bool report = true) const {
        if (!enabled) return native_ok;
        const bool diagnostic_ok = attempted && !failed && measured && queries == 1 &&
            mfd.enabled && !mfd.failed && mfd.attempted == 3 && mfd.reads == 40;
        if (report) {
            std::printf("Runtime inventory finish: native-result=%s diagnostic-result=%s queries=%u/1 "
                "MFD/SCL-status-reads=%u/40 standalone/source-lease/completion-certified=no\n",
                native_ok ? "PASS" : "FAIL", diagnostic_ok ? "PASS" : "FAIL", queries, mfd.reads);
            std::fflush(stdout);
        }
        return native_ok && diagnostic_ok;
    }
};

struct RuntimeInventoryFixture {
    HANDLE current = this;
    unsigned queries = 0;
    BC_STATUS status = BC_STS_SUCCESS;
    bool valid = true, lose_owner = false;
    HANDLE replacement = nullptr;
    uint32_t raw[3] = {0x01360000, 0x02030004, 0x00007015};
    static BC_STATUS Query(HANDLE handle, uint32_t *stream, uint32_t *decoder,
                          uint32_t *chip, char *filename, uint32_t flag) {
        auto *fixture = static_cast<RuntimeInventoryFixture *>(handle);
        ++fixture->queries;
        fixture->valid &= stream && decoder && chip && !filename && flag == 1 && fixture->queries == 1;
        *stream = fixture->raw[0]; *decoder = fixture->raw[1]; *chip = fixture->raw[2];
        if (fixture->lose_owner) fixture->current = fixture->replacement;
        return fixture->status;
    }
};

// Debug-address calibration only. The fixed tuple excludes TEST_PORT_DATA and
// all target writes. Matching sequential words cannot establish atomicity,
// selector ownership, pixel-source identity, admission or freshness.
enum class MfdAddressFailure {
    None, Argument, Read, Revision, Reserved, Unavailable, Owner, Order, Publication, Unstable
};
struct MfdAddressSnapshot {
    uint32_t raw[6] = {};
    unsigned reads = 0, measured = 0;
    BC_STATUS status = BC_STS_SUCCESS;
    MfdAddressFailure failure = MfdAddressFailure::None;
    bool Stable() const {
        return measured == 6 && !std::memcmp(raw, raw + 3, 3 * sizeof(uint32_t));
    }
};
static bool ReadMfdAddress(const HANDLE *current, HANDLE owner, MfdAddressSnapshot *snapshot,
    BC_STATUS (*reader)(HANDLE, uint32_t, uint32_t *) = DtsDevRegisterRead)
{
    if (!snapshot) return false;
    *snapshot = MfdAddressSnapshot{};
    if (!current || !*current || !owner || !reader) {
        snapshot->failure = MfdAddressFailure::Argument; return false;
    }
    const uint32_t addresses[3] = {
        BCHP_MFD_REVISION_ID, BCHP_MFD_TEST_PORT_CNTL, BCHP_MFD_REVISION_ID
    };
    for (unsigned index = 0; index < 6; ++index) {
        if (!*current || *current != owner) {
            snapshot->failure = MfdAddressFailure::Owner; return false;
        }
        uint32_t raw = 0;
        ++snapshot->reads;
        snapshot->status = reader(owner, addresses[index % 3], &raw);
        if (snapshot->status != BC_STS_SUCCESS) {
            snapshot->failure = MfdAddressFailure::Read; return false;
        }
        if (!*current || *current != owner) {
            snapshot->failure = MfdAddressFailure::Owner; return false;
        }
        snapshot->raw[index] = raw;
        ++snapshot->measured;
        if (raw == 0xffffffffU) snapshot->failure = MfdAddressFailure::Unavailable;
        else if (index % 3 != 1 && raw != 0x50U)
            snapshot->failure = MfdAddressFailure::Revision;
        else if (index % 3 == 1 && (raw & 0xfffffff0U))
            snapshot->failure = MfdAddressFailure::Reserved;
        else if (index >= 3 && raw != snapshot->raw[index - 3])
            snapshot->failure = MfdAddressFailure::Unstable;
        if (snapshot->failure != MfdAddressFailure::None) return false;
    }
    return true;
}
struct MfdAddressObserver {
    typedef BC_STATUS (*Reader)(HANDLE, uint32_t, uint32_t *);
    bool enabled = false, failed = false;
    HANDLE owner = nullptr;
    unsigned attempted = 0, reads = 0, last_frame = 0;
    MfdAddressSnapshot snapshot;
    bool Reject(MfdAddressFailure why) {
        failed = true; snapshot.failure = why; return false;
    }
    void Report(unsigned stage) const {
        const char *name = stage ? "first-output-after-release-and-owned-write" : "after-OPEN/pre-START";
        const char *const failures[] = {"none", "argument", "read-status", "revision",
            "reserved-bits", "all-ones-unavailable", "current-handle-loss", "order",
            "release-or-owned-write", "unequal-tuples"};
        const char *const fields[] = {"rev", "test-port-cntl", "closing-rev"};
        const uint32_t addresses[] = {BCHP_MFD_REVISION_ID, BCHP_MFD_TEST_PORT_CNTL, BCHP_MFD_REVISION_ID};
        std::printf("MFD debug address: stage=%s reads=%u total-reads=%u measured=%u "
            "api-status=%d failure=%s result=%s raw-stable=%s target-writes=0 data-reads=0 "
            "atomic=no address-source-enum-only=yes "
            "pixel-source/view-identity/admission/ownership/freshness-certified=no\n",
            name, snapshot.reads, reads, snapshot.measured, snapshot.status,
            failures[static_cast<unsigned>(snapshot.failure)], failed ? "FAIL" : "PASS",
            snapshot.measured == 6 ? (snapshot.Stable() ? "yes" : "no") : "NOT-READ");
        for (unsigned index = 0; index < 6; ++index) {
            std::printf("MFD debug address raw: stage=%s pass=%u %s@%08x=",
                name, index / 3, fields[index % 3], addresses[index % 3]);
            if (index < snapshot.measured) {
                const uint32_t raw = snapshot.raw[index];
                std::printf("%08x", raw);
                if (index % 3 == 1 && !(raw & 0xfffffff0U)) {
                    const unsigned source = (raw >> 3) & 1U;
                    std::printf(" ADDR_SEL=%u address-source-enum=%s TP_ADDR=%u",
                        source, source ? "SOFT_INPUT" : "PIN_INPUT", raw & 7U);
                }
            } else std::printf("NOT-READ");
            std::printf("\n");
        }
        std::fflush(stdout);
    }
    bool Observe(const HANDLE *current, unsigned stage, bool released, bool written,
        Reader reader = DtsDevRegisterRead, bool report = true) {
        if (!enabled) return true;
        if (failed) return false;
        if (!current || !*current || !reader) return Reject(MfdAddressFailure::Argument);
        if (stage > 1 || (stage == 0 ? attempted != 0 : attempted != 1))
            return Reject(MfdAddressFailure::Order);
        if (stage == 1 && (!released || !written)) return Reject(MfdAddressFailure::Publication);
        if (stage == 0 && (released || written)) return Reject(MfdAddressFailure::Order);
        if (stage == 0) owner = *current;
        if (*current != owner) return Reject(MfdAddressFailure::Owner);
        attempted |= 1U << stage;
        const bool ok = ReadMfdAddress(current, owner, &snapshot, reader);
        reads += snapshot.reads;
        failed = !ok;
        if (report) Report(stage);
        return ok;
    }
    bool PreStart(const HANDLE *current, Reader reader = DtsDevRegisterRead, bool report = true) {
        return Observe(current, 0, false, false, reader, report);
    }
    bool AfterDelivered(const HANDLE *current, unsigned frame, bool released, bool written,
        Reader reader = DtsDevRegisterRead, bool report = true) {
        if (!enabled) return true;
        if (failed) return false;
        if (!current || !*current || !reader) return Reject(MfdAddressFailure::Argument);
        if (!owner || *current != owner) return Reject(MfdAddressFailure::Owner);
        if (!released || !written) return Reject(MfdAddressFailure::Publication);
        if (frame == 0 || frame > 180 || frame != last_frame + 1 ||
            (frame == 1 ? attempted != 1 : attempted != 3)) return Reject(MfdAddressFailure::Order);
        if (frame == 1 && !Observe(current, 1, released, written, reader, report)) return false;
        last_frame = frame;
        return true;
    }
    bool Finish(bool native_ok, bool report = true) const {
        if (!enabled) return native_ok;
        const bool diagnostic_ok = !failed && attempted == 3 && reads == 12 && last_frame == 180;
        if (report) {
            std::printf("MFD debug address finish: native-result=%s diagnostic-result=%s "
                "stages=%u/2 reads=%u/12 target-writes=0 data-reads=0 "
                "pixel-source/view-identity/admission/ownership/freshness-certified=no\n",
                native_ok ? "PASS" : "FAIL", diagnostic_ok ? "PASS" : "FAIL",
                (attempted & 1U) + ((attempted >> 1) & 1U), reads);
            std::fflush(stdout);
        }
        return native_ok && diagnostic_ok;
    }
};
struct MfdAddressFixture {
    uint32_t raw[12] = {0x50, 0, 0x50, 0x50, 0, 0x50,
                       0x50, 15, 0x50, 0x50, 15, 0x50};
    unsigned calls = 0, fail_at = 12, lose_at = 12;
    BC_STATUS status = BC_STS_ERROR;
    bool valid = true;
    HANDLE current = this, replacement = nullptr;
    static BC_STATUS Read(HANDLE handle, uint32_t address, uint32_t *value) {
        auto *fixture = static_cast<MfdAddressFixture *>(handle);
        const unsigned index = fixture->calls++;
        const uint32_t expected[] = {0x00540000, 0x0054007c, 0x00540000};
        if (index >= 12 || !value) { fixture->valid = false; return BC_STS_ERROR; }
        fixture->valid &= address == expected[index % 3];
        *value = index == fixture->fail_at ? 0xdeadbeefU : fixture->raw[index];
        if (index == fixture->lose_at) fixture->current = fixture->replacement;
        return index == fixture->fail_at ? fixture->status : BC_STS_SUCCESS;
    }
    bool Exercise(MfdAddressObserver *observer, bool report = false) {
        observer->enabled = true;
        if (!observer->PreStart(&current, Read, report)) return false;
        for (unsigned frame = 1; frame <= 180; ++frame)
            if (!observer->AfterDelivered(&current, frame, true, true, Read, report)) return false;
        return observer->Finish(true, report);
    }
};

// Fixed ARM working-slot scalars only. No observed word selects an address;
// DtsDevMemRd may use its normal window selector, but no target is written.
// Cache pairs and cached source-record words have separate firmware stores.
enum class ArmMetadataFailure { None, Argument, Owner, Order, Barrier, Budget, Read };
struct ArmMetadataObserver {
    typedef BC_STATUS (*Reader)(HANDLE, uint32_t *, uint32_t, uint32_t);
    bool enabled = false, failed = false, source_shape = false, owner_source_shape = false;
    HANDLE owner = nullptr;
    unsigned next_stage = 0, reads = 0, bytes = 0, measured = 0;
    uint32_t raw[2][4][20] = {};
    BC_STATUS status = BC_STS_SUCCESS;
    ArmMetadataFailure failure = ArmMetadataFailure::None;
    unsigned SpanCount() const { return source_shape ? 10 : 5; }
    unsigned StageReads() const { return 8 * SpanCount(); }
    unsigned TotalBytes() const { return source_shape ? 1920 : 768; }
    bool Stable() const { return measured == StageReads() && !std::memcmp(raw[0], raw[1], sizeof(raw[0])); }
    bool Reject(ArmMetadataFailure why) { failed = true; failure = why; return false; }
    void Report(unsigned stage) const {
        const char *const stages[] = {"after-OPEN/pre-START", "first-output-after-release-and-owned-write", "delivery-EOS-before-STOP"};
        const char *const failures[] = {"none", "argument", "current-handle-loss", "order", "delivery-barrier", "budget", "read-status"};
        const char *label = source_shape ? "ARM source-shape" : "ARM metadata";
        std::printf("%s: stage=%s reads=%u/%u bytes=%u/%u measured=%u/%u api-status=%d failure=%s "
            "passes=%s cached-source-is-current-input=unproven exact-frame/lease/generation/cache-ready/all-consumers-certified=no\n",
            label, stages[stage], reads, StageReads() * 3, bytes, TotalBytes(), measured, StageReads(), status,
            failures[static_cast<unsigned>(failure)],
            measured == StageReads() ? (Stable() ? "observed-stable,non-atomic" : "observed-different,non-atomic") : "INCOMPLETE");
        const char *const metadata_names[] = {"c4", "d0", "cached-meta-virtual", "cached-meta-physical", "e8-opaque",
            "cached-source+34", "cached-source+38", "180-acquire/reuse"};
        const char *const shape_names[] = {"c4", "d0", "cached-meta-virtual", "cached-meta-physical", "e8-opaque",
            "cached-picture+08", "cached-picture+14", "cached-picture+18", "cached-picture+24", "cached-picture+28",
            "cached-picture+2c", "cached-picture+30", "cached-source+34", "cached-source+38",
            "cached-picture+54", "cached-picture+58", "cached-picture+5c", "cached-picture+6c", "cached-picture+70",
            "180-acquire/reuse"};
        const unsigned metadata_spans[] = {0, 1, 2, 2, 2, 3, 3, 4};
        const unsigned shape_spans[] = {0, 1, 2, 2, 2, 3, 4, 4, 5, 5, 5, 5, 6, 6, 7, 7, 7, 8, 8, 9};
        const char *const *names = source_shape ? shape_names : metadata_names;
        const unsigned *spans = source_shape ? shape_spans : metadata_spans;
        const unsigned words = source_shape ? 20 : 8;
        for (unsigned pass = 0; pass < 2; ++pass) for (unsigned slot = 0; slot < 4; ++slot) {
            std::printf("%s raw: stage=%s pass=%u slot=%u", label, stages[stage], pass, slot);
            for (unsigned word = 0; word < words; ++word) {
                std::printf(" %s=", names[word]);
                if (pass * 4 * SpanCount() + slot * SpanCount() + spans[word] < measured) std::printf("%08x", raw[pass][slot][word]);
                else std::printf("NOT-READ");
            }
            if (pass * 4 * SpanCount() + slot * SpanCount() + SpanCount() - 1 < measured) {
                const uint32_t *words = raw[pass][slot];
                const unsigned adjacent = (words[0] >> 8) & 255U, branch = words[source_shape ? 19 : 7] & 255U;
                std::printf(" active=%u adjacent-c5=%u started=%u branch-byte=%u branch-selector-if-active-and-started=%s",
                    words[0] & 255U, adjacent, (words[1] >> 16) & 255U, branch,
                    branch == 1 || adjacent == 0 ? "FRESH-dequeue" : "CACHE-copy");
                if (source_shape) std::printf(" mode-byte=%u format-byte=%u field-byte=%u table-selector-byte=%u",
                    words[5] & 255U, words[8] >> 24, words[9] & 255U, words[16] & 255U);
            }
            std::printf("\n");
        }
        std::fflush(stdout);
    }
    bool Observe(const HANDLE *current, unsigned stage, bool barrier,
        Reader reader = DtsDevMemRd, bool report = true) {
        if (!enabled) return true;
        if (failed) return false;
        if (!current || !*current || !reader) return Reject(ArmMetadataFailure::Argument);
        if (next_stage && source_shape != owner_source_shape) return Reject(ArmMetadataFailure::Argument);
        if (stage > 2 || stage != next_stage) return Reject(ArmMetadataFailure::Order);
        if (barrier != (stage != 0)) return Reject(ArmMetadataFailure::Barrier);
        if (reads > StageReads() * 2) return Reject(ArmMetadataFailure::Budget);
        if (stage == 0) { owner = *current; owner_source_shape = source_shape; }
        if (!owner || *current != owner) return Reject(ArmMetadataFailure::Owner);
        ++next_stage; // Each stage is attempted once, including failed reads.
        measured = 0; std::memset(raw, 0, sizeof(raw));
        const uint32_t metadata_offsets[] = {0xc4, 0xd0, 0xe0, 0x120, 0x180};
        const unsigned metadata_counts[] = {1, 1, 3, 2, 1};
        // P is the separately cached 140-byte picture at slot+ec. These
        // aligned reads never dereference P's source/metadata address words.
        const uint32_t shape_offsets[] = {0xc4, 0xd0, 0xe0, 0xf4, 0x100, 0x110, 0x120, 0x140, 0x158, 0x180};
        const unsigned shape_counts[] = {1, 1, 3, 1, 2, 4, 2, 3, 2, 1};
        const uint32_t *offsets = source_shape ? shape_offsets : metadata_offsets;
        const unsigned *counts = source_shape ? shape_counts : metadata_counts;
        bool ok = true;
        for (unsigned pass = 0; pass < 2 && ok; ++pass) for (unsigned slot = 0; slot < 4 && ok; ++slot) {
            unsigned word = 0;
            for (unsigned span = 0; span < SpanCount() && ok; ++span) {
                if (!*current || *current != owner) { ok = Reject(ArmMetadataFailure::Owner); break; }
                uint32_t values[4] = {};
                ++reads; bytes += counts[span] * 4;
                status = reader(owner, values, counts[span] * 4, 0xd3a00U + slot * 0x1ccU + offsets[span]);
                if (!*current || *current != owner) ok = Reject(ArmMetadataFailure::Owner);
                else if (status != BC_STS_SUCCESS) ok = Reject(ArmMetadataFailure::Read);
                if (!ok) break;
                std::memcpy(raw[pass][slot] + word, values, counts[span] * sizeof(uint32_t));
                word += counts[span]; ++measured;
            }
        }
        if (report) Report(stage);
        return ok;
    }
    bool Finish(bool native_ok, bool report = true) const {
        if (!enabled) return native_ok;
        const bool complete = !failed && source_shape == owner_source_shape && next_stage == 3 &&
            reads == StageReads() * 3 && bytes == TotalBytes();
        if (report) {
            std::printf("%s finish: native-result=%s observation-result=%s stages=%u/3 reads=%u/%u bytes=%u/%u\n",
                source_shape ? "ARM source-shape" : "ARM metadata", native_ok ? "PASS" : "FAIL",
                complete ? "PASS" : "FAIL", next_stage, reads, StageReads() * 3, bytes, TotalBytes());
            std::fflush(stdout);
        }
        return native_ok && complete;
    }
};
struct ArmMetadataFixture {
    HANDLE current = this, replacement = nullptr;
    unsigned calls = 0, fail_at = 120, lose_at = 120;
    bool valid = true, different = false, all_ones = false, source_shape = false;
    static BC_STATUS Read(HANDLE handle, uint32_t *values, uint32_t bytes, uint32_t address) {
        auto *fixture = static_cast<ArmMetadataFixture *>(handle);
        const unsigned count = fixture->source_shape ? 10 : 5, per_stage = count * 8;
        const unsigned position = fixture->calls++, span = position % count, slot = position % (count * 4) / count;
        const uint32_t metadata_starts[] = {0xd3ac4, 0xd3ad0, 0xd3ae0, 0xd3b20, 0xd3b80};
        const unsigned metadata_lengths[] = {4, 4, 12, 8, 4}, metadata_first[] = {0, 1, 2, 5, 7};
        const uint32_t shape_starts[] = {0xd3ac4, 0xd3ad0, 0xd3ae0, 0xd3af4, 0xd3b00,
            0xd3b10, 0xd3b20, 0xd3b40, 0xd3b58, 0xd3b80};
        const unsigned shape_lengths[] = {4, 4, 12, 4, 8, 16, 8, 12, 8, 4};
        const unsigned shape_first[] = {0, 1, 2, 5, 6, 8, 12, 14, 17, 19};
        const uint32_t *starts = fixture->source_shape ? shape_starts : metadata_starts;
        const unsigned *lengths = fixture->source_shape ? shape_lengths : metadata_lengths;
        const unsigned *first = fixture->source_shape ? shape_first : metadata_first;
        if (position >= per_stage * 3 || !values || bytes != lengths[span]) { fixture->valid = false; return BC_STS_ERROR; }
        fixture->valid &= address == starts[span] + slot * 460 &&
            reinterpret_cast<uintptr_t>(values) % alignof(uint32_t) == 0;
        for (unsigned word = 0; word < bytes / 4; ++word)
            values[word] = fixture->all_ones ? 0xffffffffU : position < per_stage ? 0 :
                ((position / per_stage) << 24) | (slot << 8) | (first[span] + word + 1);
        if (fixture->different && position % per_stage >= count * 4) values[0] ^= 0x80000000U;
        if (position == fixture->lose_at) fixture->current = fixture->replacement;
        return position == fixture->fail_at ? BC_STS_ERROR : BC_STS_SUCCESS;
    }
    bool Exercise(ArmMetadataObserver *observer) {
        observer->enabled = true;
        for (unsigned stage = 0; stage < 3; ++stage)
            if (!observer->Observe(&current, stage, stage != 0, Read, false)) return false;
        return observer->Finish(true, false);
    }
};

// Default-only rooted ARC-context diagnostics. Every object is bounded by
// its stock declaration before reading; pool/bank words never select a target.
// Saved scalars and separate fixed metadata prefixes do not certify a lease.
struct PpbContextGraph { uint32_t words[39] = {}; };
enum class PpbContextFailure { None, Argument, Owner, Order, Budget, Read, Object, Profile, Map, Slice, Changed };
struct PpbContextObserver {
    typedef BC_STATUS (*Reader)(HANDLE, uint32_t *, uint32_t, uint32_t);
    bool enabled = false, post_stop = false, metadata_pool = false, failed = false, admitted = false;
    bool owner_post_stop = false, owner_metadata_pool = false;
    bool return_header = false, owner_return_header = false;
    bool video_prefix = false, owner_video_prefix = false;
    bool video_graph = false, owner_video_graph = false;
    bool video_staging = false, owner_video_staging = false;
    HANDLE owner = nullptr;
    unsigned next_stage = 0, reads = 0, bytes = 0, measured = 0;
    unsigned active_stage = 0, stage_reads = 0, stage_bytes = 0;
    BC_STATUS status = BC_STS_SUCCESS;
    PpbContextFailure failure = PpbContextFailure::None;
    unsigned changed_word = 39;
    uint32_t changed_expected = 0, changed_observed = 0;
    PpbContextGraph authority, graph[2];
    uint32_t raw[2][60] = {};
    uint32_t pool_raw[2][620] = {}, route_raw[2][2] = {};
    bool complete[2] = {};
    unsigned Stages() const { return (video_staging || owner_video_staging || video_graph || owner_video_graph || video_prefix || owner_video_prefix) ? 3U : post_stop ? 4U : 3U; }
    unsigned StageReads(unsigned stage = 0) const {
        return owner_video_staging ? (stage ? 62U : 126U) : owner_video_graph ? 62U : owner_video_prefix ? 126U :
            video_staging ? (stage ? 62U : 126U) : video_graph ? 62U : video_prefix ? 126U : return_header ? 204U : metadata_pool ? 196U : 140U;
    }
    unsigned StageBytes(unsigned stage = 0) const {
        return owner_video_staging ? (stage ? 312U : 880U) : owner_video_graph ? 312U : owner_video_prefix ? 880U :
            video_staging ? (stage ? 312U : 880U) : video_graph ? 312U : video_prefix ? 880U : return_header ? 5600U : metadata_pool ? 5552U : 1104U;
    }
    unsigned ReadLimit() const { return owner_video_staging || (!owner_video_graph && !owner_video_prefix && video_staging) ? 250U : Stages() * StageReads(); }
    unsigned ByteLimit() const { return owner_video_staging || (!owner_video_graph && !owner_video_prefix && video_staging) ? 1504U : Stages() * StageBytes(); }
    bool OnlyGraphStage(unsigned stage) const { return owner_video_graph || (owner_video_staging && stage != 0); }
    static bool PoolSpan(unsigned span, uint32_t *offset, unsigned *count, bool bracket = false) {
        if (span >= (bracket ? 39U : 36U) || !offset || !count) return false;
        if (bracket) {
            // Four consecutive requests compare grouping/timing only: the
            // 8-byte request itself uses two sequential 32-bit device reads.
            const uint32_t offsets[] = {0x15678,0x15778,0x1577c,0x1577c,0x15778};
            const unsigned counts[] = {2,2,1,1,2};
            *offset = span < 5 ? offsets[span] : 0x15878U + (span - 5) * 0xe4U;
            *count = span < 5 ? counts[span] : 18U;
            return true;
        }
        *offset = span < 2 ? 0x15678U + span * 0x100U : 0x15878U + (span - 2) * 0xe4U;
        *count = span < 2 ? 2U : 18U;
        return true;
    }
    static bool PoolWindow(const PpbContextGraph &g) {
        const uint64_t p = g.words[14], skip = (0U - g.words[14]) & 3U, d = p + skip;
        const uint64_t submitted_end = p + g.words[15], video_end = static_cast<uint64_t>(g.words[22]) + g.words[23];
        // Guard both complete 256-byte rings and the entire 34*228-byte pool,
        // not merely the 72-byte prefixes selected for this observation.
        return g.words[15] >= skip + 0x177ccU && d >= g.words[22] &&
            d + 0x176c0U <= submitted_end && submitted_end <= video_end && video_end <= 0x3ffc000U;
    }
    static bool PrefixWindow(const PpbContextGraph &g) {
        const uint64_t base = g.words[22], end = base + g.words[23];
        const uint64_t p = g.words[14], d = p + ((0U - g.words[14]) & 3U);
        // Only the allocation's fixed front 128 bytes. No picture/pool word
        // selects this target; the span does not establish live pixel layout.
        return base && !(base & 4095U) && base == g.words[21] && g.words[23] >= 128U &&
            base + 128U <= d && d <= end && p >= base &&
            p + g.words[15] <= end && d + 0x5bcU <= p + g.words[15] && end <= 0x3ffc000U;
    }
    static bool Object(uint32_t pointer, unsigned kind) {
        const uint32_t sizes[] = {0xa84c, 0x378, 0x1f4, 0x64};
        return kind < 4 && !(pointer & 3U) && pointer >= 0xd53dcU &&
            static_cast<uint64_t>(pointer) + sizes[kind] <= 0x116000U;
    }
    static bool Disjoint(const uint32_t *pointers, unsigned count) {
        const uint32_t sizes[] = {0xa84c, 0x378, 0x1f4, 0x64};
        if (!pointers || count > 4) return false;
        for (unsigned i = 0; i < count; ++i) {
            if (!Object(pointers[i], i)) return false;
            for (unsigned j = 0; j < i; ++j)
                if (pointers[i] < static_cast<uint64_t>(pointers[j]) + sizes[j] &&
                    pointers[j] < static_cast<uint64_t>(pointers[i]) + sizes[i]) return false;
        }
        return true;
    }
    static bool Equal(const PpbContextGraph &a, const PpbContextGraph &b) {
        // Working flags are phase-changing qualifiers, not allocator authority.
        if (a.words[0] != b.words[0]) return false;
        for (unsigned slot = 0; slot < 4; ++slot)
            if (a.words[1 + slot * 3] != b.words[1 + slot * 3]) return false;
        return !std::memcmp(a.words + 13, b.words + 13, 26 * sizeof(uint32_t));
    }
    bool Reject(PpbContextFailure why) { failed = true; failure = why; return false; }
    bool RejectChanged(const PpbContextGraph &observed, const PpbContextGraph &frozen,
                       unsigned first, unsigned count) {
        // Diagnose only the range already collected and compared. This never
        // issues another read or uses a changed value as a pointer target.
        for (unsigned word = first; word < first + count && word < 39; ++word) {
            if (observed.words[word] == frozen.words[word]) continue;
            changed_word = word;
            changed_expected = frozen.words[word];
            changed_observed = observed.words[word];
            break;
        }
        return Reject(PpbContextFailure::Changed);
    }
    bool VideoModeReady() const {
        return (!video_prefix && !owner_video_prefix && !video_graph && !owner_video_graph && !video_staging && !owner_video_staging) ||
            (enabled && video_prefix == owner_video_prefix && video_graph == owner_video_graph && video_staging == owner_video_staging &&
             static_cast<unsigned>(video_prefix) + static_cast<unsigned>(video_graph) + static_cast<unsigned>(video_staging) == 1U &&
             !post_stop && !metadata_pool && !return_header &&
             !owner_post_stop && !owner_metadata_pool && !owner_return_header);
    }
    bool Read(const HANDLE *current, Reader reader, uint32_t address, unsigned count, uint32_t *values) {
        if (failed) return false;
        if (!VideoModeReady()) return Reject(PpbContextFailure::Argument);
        if (!current || !*current || *current != owner) return Reject(PpbContextFailure::Owner);
        if (!reader || !values || !count || count > 36 || (address & 3U) ||
            static_cast<uint64_t>(address) + count * 4U > 0x4000000U) return Reject(PpbContextFailure::Argument);
        if (reads >= ReadLimit() || bytes > ByteLimit() - count * 4U) return Reject(PpbContextFailure::Budget);
        if (owner_video_staging && (stage_reads >= StageReads(active_stage) ||
            stage_bytes > StageBytes(active_stage) - count * 4U)) return Reject(PpbContextFailure::Budget);
        ++reads; bytes += count * 4U;
        ++stage_reads; stage_bytes += count * 4U;
        status = reader(owner, values, count * 4U, address);
        if (!*current || *current != owner) return Reject(PpbContextFailure::Owner);
        if (!VideoModeReady()) return Reject(PpbContextFailure::Argument);
        if (status != BC_STS_SUCCESS) return Reject(PpbContextFailure::Read);
        ++measured; return true;
    }
    bool Graph(const HANDLE *current, unsigned stage, Reader reader, PpbContextGraph *out,
               const PpbContextGraph *frozen) {
        uint32_t *v = out->words;
        if (!Read(current, reader, 0xd3a08, 1, v)) return false;
        for (unsigned slot = 0; slot < 4; ++slot) {
            const uint32_t base = 0xd3a00U + slot * 0x1ccU;
            if (!Read(current, reader, base + 0x20, 1, v + 1 + slot * 3) ||
                !Read(current, reader, base + 0xc4, 1, v + 2 + slot * 3) ||
                !Read(current, reader, base + 0xd0, 1, v + 3 + slot * 3)) return false;
            if ((v[2 + slot * 3] & 255U) != (slot == 0 ? 1U : 0U)) return Reject(PpbContextFailure::Profile);
            if (frozen && v[1 + slot * 3] != frozen->words[1 + slot * 3]) return RejectChanged(*out, *frozen, 1 + slot * 3, 1);
        }
        if ((v[3] & 255U) || ((v[3] >> 16) & 255U) != ((stage == 1 || stage == 2) ? 1U : 0U))
            return Reject(PpbContextFailure::Profile);
        if (frozen && v[0] != frozen->words[0]) return RejectChanged(*out, *frozen, 0, 1);
        const uint32_t h = frozen ? frozen->words[1] : v[1], c = frozen ? frozen->words[0] : v[0];
        uint32_t objects[] = {h, c, 0, 0};
        if (!Disjoint(objects, 2)) return Reject(PpbContextFailure::Object);
        if (!Read(current, reader, h, 1, v + 13) || !Read(current, reader, h + 8, 2, v + 14) ||
            !Read(current, reader, h + 0x64, 1, v + 16) || !Read(current, reader, h + 0xcc, 1, v + 17) ||
            !Read(current, reader, h + 0x224, 1, v + 18)) return false;
        if (frozen && std::memcmp(v + 13, frozen->words + 13, 6 * sizeof(uint32_t))) return RejectChanged(*out, *frozen, 13, 6);
        if (v[13] || v[15] != 0x3f940U || v[16] != c || v[17] || v[18] != 0x116004U) return Reject(PpbContextFailure::Profile);
        if (!Read(current, reader, c + 8, 1, v + 19) || !Read(current, reader, c + 0x1a0, 1, v + 20) ||
            !Read(current, reader, c + 0x1d4, 3, v + 21)) return false;
        if (frozen && std::memcmp(v + 19, frozen->words + 19, 5 * sizeof(uint32_t))) return RejectChanged(*out, *frozen, 19, 5);
        if (v[19] != 0x116004U) return Reject(PpbContextFailure::Profile);
        objects[2] = frozen ? frozen->words[20] : v[20];
        if (!Disjoint(objects, 3)) return Reject(PpbContextFailure::Object);
        const uint32_t q = objects[2];
        if (!Read(current, reader, q, 1, v + 24) || !Read(current, reader, q + 8, 1, v + 25) ||
            !Read(current, reader, q + 0x10, 2, v + 26)) return false;
        if (frozen && std::memcmp(v + 24, frozen->words + 24, 4 * sizeof(uint32_t))) return RejectChanged(*out, *frozen, 24, 4);
        if (v[24] != c || v[26] != v[23] || v[27] != v[21]) return Reject(PpbContextFailure::Map);
        objects[3] = frozen ? frozen->words[25] : v[25];
        if (!Disjoint(objects, 4)) return Reject(PpbContextFailure::Object);
        const uint32_t m = objects[3];
        if (!Read(current, reader, m + 0x18, 2, v + 28) || !Read(current, reader, m + 0x28, 1, v + 30) ||
            !Read(current, reader, m + 0x30, 2, v + 31) || !Read(current, reader, m + 0x40, 1, v + 33) ||
            !Read(current, reader, 0x11601c, 2, v + 34) || !Read(current, reader, 0x11602c, 1, v + 36) ||
            !Read(current, reader, 0x116034, 2, v + 37)) return false;
        if (frozen && !Equal(*out, *frozen)) return RejectChanged(*out, *frozen, 13, 26);
        const uint32_t defaults[] = {0x116068, 0x3ffc000, 0x116004, 0x116004, 0x3ffc000 - 0x116004};
        const uint64_t video_end = static_cast<uint64_t>(v[21]) + v[23];
        if (std::memcmp(v + 34, defaults, sizeof(defaults)) || !v[23] || (v[21] & 4095U) || (v[23] & 3U) ||
            v[21] != v[22] || v[21] < defaults[0] || video_end > defaults[1] ||
            v[28] != v[21] || v[29] != video_end || v[30] != v[21] || v[31] != v[22] ||
            v[32] != v[23] || v[33] != 1) return Reject(PpbContextFailure::Map);
        const uint64_t submitted_end = static_cast<uint64_t>(v[14]) + v[15];
        const uint32_t skip = (0U - v[14]) & 3U;
        if (!v[14] || v[14] < v[22] || submitted_end > video_end || v[15] < skip ||
            v[15] - skip < 0x177ccU || static_cast<uint64_t>(v[14]) + skip + 0x5bcU > submitted_end)
            return Reject(PpbContextFailure::Slice);
        return true;
    }
    void Report(unsigned stage) const {
        const char *const stages[] = {"after-OPEN/pre-START", "first-output-after-release-and-owned-write",
            "delivery-EOS-before-STOP", "host-STOP-returned-before-CLOSE"};
        const char *const failures[] = {"none", "argument", "current-handle-loss", "order", "budget", "read-status",
            "object-envelope/alias", "default-profile", "map-tuple", "context-slice", "authority-changed"};
        if (owner_video_staging) {
            std::printf("Video allocation staging: stage=%s reads=%u/%u bytes=%u/%u failure=%s api-status=%d\n",
                stages[stage], reads, ReadLimit(), bytes, ByteLimit(), failures[static_cast<unsigned>(failure)], status);
            if (failure == PpbContextFailure::Changed && changed_word < 39)
                std::printf("Video allocation staging authority delta: word=%u frozen=%08x observed=%08x "
                    "already-collected-first-difference only; no retry or changed-target read\n",
                    changed_word, changed_expected, changed_observed);
            for (unsigned pass = 0; pass < 2; ++pass) {
                const char *kind = stage ? "graph" : "raw";
                if (!complete[pass]) {
                    std::printf("Video allocation staging %s: stage=%s pass=%u INCOMPLETE\n", kind, stages[stage], pass);
                    continue;
                }
                std::printf("Video allocation staging %s: stage=%s pass=%u", kind, stages[stage], pass);
                if (!stage) std::printf(" address=%08x bytes=128", authority.words[22]);
                std::printf(" words=");
                for (unsigned word = 0; word < (stage ? 39U : 32U); ++word)
                    std::printf("%s%08x", word ? "," : "", stage ? graph[pass].words[word] : raw[pass][word]);
                std::printf("\n");
            }
            std::printf("Video allocation staging scope: pre-START image-staging bytes only; "
                "post-START prefix/context/pool/ring/plane-target-reads=0; "
                "non-atomic pixel-layout/lease/generation/completion/cause-certified=no\n");
            std::fflush(stdout);
            return;
        }
        if (owner_video_graph) {
            std::printf("Video allocation graph: stage=%s reads=%u/%u bytes=%u/%u failure=%s api-status=%d\n",
                stages[stage], reads, ReadLimit(), bytes, ByteLimit(), failures[static_cast<unsigned>(failure)], status);
            if (failure == PpbContextFailure::Changed && changed_word < 39)
                std::printf("Video allocation graph authority delta: word=%u frozen=%08x observed=%08x "
                    "already-collected-first-difference only; no retry or changed-target read\n",
                    changed_word, changed_expected, changed_observed);
            for (unsigned pass = 0; pass < 2; ++pass) {
                if (!complete[pass]) {
                    std::printf("Video allocation graph raw: stage=%s pass=%u INCOMPLETE\n", stages[stage], pass);
                    continue;
                }
                std::printf("Video allocation graph raw: stage=%s pass=%u words=", stages[stage], pass);
                for (unsigned word = 0; word < 39; ++word)
                    std::printf("%s%08x", word ? "," : "", graph[pass].words[word]);
                std::printf("\n");
            }
            std::printf("Video allocation graph scope: rooted graph only; prefix/context/pool/ring/plane-target-reads=0; "
                "non-atomic allocator-integrity/pixel-layout/lease/generation/completion-certified=no\n");
            std::fflush(stdout);
            return;
        }
        if (owner_video_prefix) {
            std::printf("Video allocation prefix: stage=%s reads=%u/%u bytes=%u/%u failure=%s api-status=%d\n",
                stages[stage], reads, ReadLimit(), bytes, ByteLimit(), failures[static_cast<unsigned>(failure)], status);
            if (failure == PpbContextFailure::Changed && changed_word < 39)
                std::printf("Video allocation prefix authority delta: word=%u frozen=%08x observed=%08x "
                    "already-collected-first-difference only; no retry or changed-target read\n",
                    changed_word, changed_expected, changed_observed);
            for (unsigned pass = 0; pass < 2; ++pass) {
                if (!complete[pass]) {
                    std::printf("Video allocation prefix raw: stage=%s pass=%u INCOMPLETE\n", stages[stage], pass);
                    continue;
                }
                std::printf("Video allocation prefix raw: stage=%s pass=%u address=%08x bytes=128 words=",
                    stages[stage], pass, authority.words[22]);
                for (unsigned word = 0; word < 32; ++word)
                    std::printf("%s%08x", word ? "," : "", raw[pass][word]);
                std::printf("\n");
            }
            std::printf("Video allocation prefix scope: non-atomic raw bytes; pixel-layout/lease/generation/completion-certified=no\n");
            std::fflush(stdout);
            return;
        }
        if (metadata_pool) {
            const char *label = return_header ? "PPB return header" : "PPB fixed metadata";
            std::printf("%s: stage=%s reads=%u/%u bytes=%u/%u measured=%u/%u failure=%s api-status=%d "
                "non-atomic current-state/allocator/extent/lease/generation/ARC-completion-certified=no\n",
                label, stages[stage], reads, ReadLimit(), bytes, ByteLimit(), measured, StageReads(), failures[static_cast<unsigned>(failure)], status);
            for (unsigned pass = 0; pass < 2; ++pass) {
                if (!complete[pass]) { std::printf("%s raw: stage=%s pass=%u INCOMPLETE\n", label, stages[stage], pass); continue; }
                const uint32_t *g = graph[pass].words, *r = pool_raw[pass];
                const uint32_t d = g[14] + ((0U - g[14]) & 3U);
                std::printf("%s raw: stage=%s pass=%u C=%08x H=%08x Q=%08x M=%08x P=%08x N=%08x "
                    "D=%08x video-base=%08x video-bytes=%08x delivery-read=%08x delivery-write=%08x "
                    "return-read=%08x return-write=%08x", label, stages[stage], pass, g[0], g[1], g[20], g[25],
                    g[14], g[15], d, g[22], g[23], r[0], r[1], r[2], r[3]);
                if (return_header) std::printf(" route-acquire=%08x route-return=%08x return-write-single0=%08x "
                    "return-write-single1=%08x return-read-after=%08x return-write-after=%08x",
                    route_raw[pass][0], route_raw[pass][1], r[4], r[5], r[6], r[7]);
                std::printf("\n");
                for (unsigned slot = 0; slot < 34; ++slot) {
                    std::printf("%s slot: stage=%s pass=%u slot=%u fixed-address=%08x words=",
                        label, stages[stage], pass, slot, d + 0x15878U + slot * 0xe4U);
                    for (unsigned word = 0; word < 18; ++word)
                        std::printf("%s%08x", word ? "," : "", r[(return_header ? 8U : 4U) + slot * 18 + word]);
                    std::printf("\n");
                }
            }
            if (complete[0] && complete[1]) std::printf("%s comparison: stage=%s fields=%s,non-atomic "
                "queued-return-is-consumption=unproven slot-address-is-generation=no\n", label, stages[stage],
                std::memcmp(pool_raw[0], pool_raw[1], (return_header ? 620U : 616U) * sizeof(uint32_t)) ||
                (return_header && std::memcmp(route_raw[0], route_raw[1], sizeof(route_raw[0]))) ? "observed-different" : "observed-stable");
            if (return_header) std::printf("PPB return header scope: request-grouping/timing-only atomic8B/routing-identity/zero-cause-certified=no\n");
            std::fflush(stdout); return;
        }
        std::printf("PPB saved context: stage=%s reads=%u/%u bytes=%u/%u measured=%u/140 failure=%s api-status=%d "
            "saved-copy-is-current=unproven atomic/allocator-integrity/lease/generation/cache-ready/all-consumers-certified=no\n",
            stages[stage], reads, ReadLimit(), bytes, ByteLimit(), measured, failures[static_cast<unsigned>(failure)], status);
        for (unsigned pass = 0; pass < 2; ++pass) {
            if (!complete[pass]) { std::printf("PPB saved context raw: stage=%s pass=%u INCOMPLETE\n", stages[stage], pass); continue; }
            const uint32_t *g = graph[pass].words, *r = raw[pass];
            std::printf("PPB saved context raw: stage=%s pass=%u C=%08x H=%08x Q=%08x M=%08x P=%08x N=%08x "
                "D=%08x video-base=%08x video-bytes=%08x %s=%08x word80=%08x producer-pool=%08x "
                "metadata-pool=%08x bank-bytes=%08x %s=%08x reader-pool=%08x",
                stages[stage], pass, g[0], g[1], g[20], g[25], g[14], g[15], g[14] + ((0U - g[14]) & 3U),
                g[22], g[23], post_stop ? "saved-core-word0" : "core-flags", r[0], r[1], r[2], r[3],
                r[57], post_stop ? "bank-word490" : "bank-count", r[58], r[59]);
            // Keep the older profile's raw labels stable. The STOP profile
            // distinguishes the count byte from its adjacent disposition byte.
            if (post_stop) std::printf(" bank-count-u8=%u adjacent-disposition-u8=%u", r[58] & 255U, (r[58] >> 8) & 255U);
            std::printf(" flags=");
            for (unsigned word = 4; word < 21; ++word) std::printf("%s%08x", word == 4 ? "" : ",", r[word]);
            std::printf(" banks=");
            for (unsigned word = 21; word < 57; ++word) std::printf("%s%08x", word == 21 ? "" : ",", r[word]);
            std::printf("\n");
        }
        if (complete[0] && complete[1]) std::printf("PPB saved context comparison: stage=%s saved-fields=%s,non-atomic\n",
            stages[stage], std::memcmp(raw[0], raw[1], sizeof(raw[0])) ? "observed-different" : "observed-stable");
        std::fflush(stdout);
    }
    bool Observe(const HANDLE *current, unsigned stage, bool barrier, Reader reader = DtsDevMemRd, bool report = true) {
        if (failed) return false;
        if (!enabled) return (owner_video_prefix || owner_video_graph || owner_video_staging) ? Reject(PpbContextFailure::Argument) : true;
        if (!current || !*current || !reader) return Reject(PpbContextFailure::Argument);
        if ((metadata_pool && !post_stop) || (return_header && !metadata_pool) ||
            ((video_prefix || video_graph || video_staging) && (post_stop || metadata_pool || return_header)) ||
            (static_cast<unsigned>(video_prefix) + static_cast<unsigned>(video_graph) + static_cast<unsigned>(video_staging) > 1U) || (next_stage &&
            (post_stop != owner_post_stop || metadata_pool != owner_metadata_pool || return_header != owner_return_header ||
             video_prefix != owner_video_prefix || video_graph != owner_video_graph || video_staging != owner_video_staging)))
            return Reject(PpbContextFailure::Argument);
        if (stage >= Stages() || stage != next_stage || barrier != (stage != 0)) return Reject(PpbContextFailure::Order);
        if (reads > ReadLimit() - StageReads(stage) || bytes > ByteLimit() - StageBytes(stage)) return Reject(PpbContextFailure::Budget);
        if (!stage) { owner = *current; owner_post_stop = post_stop; owner_metadata_pool = metadata_pool; owner_return_header = return_header; owner_video_prefix = video_prefix; owner_video_graph = video_graph; owner_video_staging = video_staging; }
        if (!owner || *current != owner) return Reject(PpbContextFailure::Owner);
        ++next_stage; measured = 0; std::memset(raw, 0, sizeof(raw)); complete[0] = complete[1] = false;
        active_stage = stage; stage_reads = stage_bytes = 0;
        std::memset(pool_raw, 0, sizeof(pool_raw));
        std::memset(route_raw, 0, sizeof(route_raw));
        const uint32_t offsets[] = {0, 0x80, 0x21c, 0x33c, 0x354, 0x3fc, 0x48c, 0x530};
        const unsigned counts[] = {1, 1, 1, 1, 17, 36, 2, 1};
        bool ok = true;
        for (unsigned pass = 0; pass < 2 && ok; ++pass) {
            graph[pass] = PpbContextGraph{};
            ok = Graph(current, stage, reader, &graph[pass], admitted ? &authority : nullptr);
            if (!ok) break;
            if (!admitted) { authority = graph[pass]; admitted = true; }
            // This separate mode stops at the rooted graph. In particular it
            // never reads allocation prefixes, saved context, rings or planes.
            if (OnlyGraphStage(stage)) { complete[pass] = true; continue; }
            const uint32_t d = authority.words[14] + ((0U - authority.words[14]) & 3U);
            unsigned word = 0;
            if (metadata_pool && !PoolWindow(authority)) { ok = Reject(PpbContextFailure::Slice); break; }
            // The route operands are informational, addressed only from H;
            // they never select a ring/pool target. Read before fixed spans.
            if (return_header) ok = Read(current, reader, authority.words[1] + 0x250U, 2, route_raw[pass]);
            if (video_prefix || owner_video_staging) {
                ok = PrefixWindow(authority) ? Read(current, reader, authority.words[22], 32, raw[pass]) :
                    Reject(PpbContextFailure::Slice);
            }
            for (unsigned span = 0; span < ((video_prefix || owner_video_staging) ? 0U : return_header ? 39U : metadata_pool ? 36U : 8U) && ok; ++span) {
                uint32_t offset = offsets[metadata_pool ? 0 : span];
                unsigned count = counts[metadata_pool ? 0 : span];
                if (metadata_pool && !PoolSpan(span, &offset, &count, return_header)) { ok = Reject(PpbContextFailure::Argument); break; }
                ok = Read(current, reader, d + offset, count, (metadata_pool ? pool_raw[pass] : raw[pass]) + word);
                word += count;
            }
            PpbContextGraph after;
            if (ok) ok = Graph(current, stage, reader, &after, &authority);
            complete[pass] = ok;
        }
        if (report) Report(stage);
        return ok;
    }
    bool Finish(bool native_ok, bool report = true) const {
        if (!enabled && !owner_video_prefix && !owner_video_graph && !owner_video_staging) return native_ok;
        const bool ok = !failed && admitted && VideoModeReady() && owner_post_stop == post_stop && owner_metadata_pool == metadata_pool && owner_return_header == return_header &&
            owner_video_prefix == video_prefix && owner_video_graph == video_graph && owner_video_staging == video_staging &&
            next_stage == Stages() && reads == ReadLimit() && bytes == ByteLimit();
        if (report) std::printf("PPB %s finish: native-result=%s observation-result=%s stages=%u/%u reads=%u/%u bytes=%u/%u\n",
            owner_video_staging ? "video staging" : owner_video_graph ? "video graph" : owner_video_prefix ? "video prefix" : return_header ? "return header" : metadata_pool ? "fixed metadata" : "saved context", native_ok ? "PASS" : "FAIL", ok ? "PASS" : "FAIL",
            next_stage, Stages(), reads, ReadLimit(), bytes, ByteLimit());
        return native_ok && ok;
    }
};
struct PpbContextFixture {
    HANDLE current = this, replacement = nullptr;
    unsigned calls = 0, fail_at = 420, lose_at = 420, change_at = 420;
    uint32_t changed_address = 0, changed_value = 0;
    uint32_t physical = 0x1000000;
    bool valid = true, different = false, all_ones = false, post_stop = false, metadata_pool = false, return_header = false;
    bool video_prefix = false, prefix_zero = false, video_graph = false, video_staging = false;
    uint32_t Address(unsigned at) const {
        const uint32_t graph[] = {0xd3a08,
            0xd3a20,0xd3ac4,0xd3ad0,0xd3bec,0xd3c90,0xd3c9c,0xd3db8,0xd3e5c,0xd3e68,0xd3f84,0xd4028,0xd4034,
            0xd6000,0xd6008,0xd6064,0xd60cc,0xd6224,0xd5408,0xd55a0,0xd55d4,
            0xd5800,0xd5808,0xd5810,0xd5a18,0xd5a28,0xd5a30,0xd5a40,0x11601c,0x11602c,0x116034};
        const uint32_t core[] = {0,0x80,0x21c,0x33c,0x354,0x3fc,0x48c,0x530};
        if (video_graph || (video_staging && calls > 126)) return graph[at];
        if (video_prefix || video_staging) return at < 31 ? graph[at] : at == 31 ? 0x200000U : graph[at - 32];
        if (return_header) {
            if (at < 31) return graph[at];
            if (at >= 71) return graph[at - 71];
            if (at == 31) return 0xd6250;
            const uint32_t headers[] = {0x15678,0x15778,0x1577c,0x1577c,0x15778};
            const uint32_t offset = at < 37 ? headers[at - 32] : 0x15878U + (at - 37) * 228U;
            return physical + ((0U - physical) & 3U) + offset;
        }
        if (metadata_pool) {
            if (at < 31) return graph[at];
            if (at >= 67) return graph[at - 67];
            const uint32_t offset = at < 33 ? 0x15678U + (at - 31) * 256U : 0x15878U + (at - 33) * 228U;
            return physical + ((0U - physical) & 3U) + offset;
        }
        return at < 31 ? graph[at] : at < 39 ? physical + ((0U - physical) & 3U) + core[at - 31] : graph[at - 39];
    }
    static unsigned Count(unsigned at, bool metadata = false, bool bracket = false, bool prefix = false, bool graph_only = false) {
        const unsigned graph[] = {1,1,1,1,1,1,1,1,1,1,1,1,1,1,2,1,1,1,1,1,3,1,1,2,2,1,2,1,2,1,2};
        const unsigned core[] = {1,1,1,1,17,36,2,1};
        if (graph_only) return graph[at];
        if (prefix) return at < 31 ? graph[at] : at == 31 ? 32U : graph[at - 32];
        if (bracket) {
            if (at < 31) return graph[at];
            if (at >= 71) return graph[at - 71];
            const unsigned headers[] = {2,2,2,1,1,2};
            return at < 37 ? headers[at - 31] : 18;
        }
        if (metadata) {
            if (at < 31) return graph[at];
            if (at >= 67) return graph[at - 67];
            return at < 33 ? 2 : 18;
        }
        return at < 31 ? graph[at] : at < 39 ? core[at - 31] : graph[at - 39];
    }
    uint32_t Word(uint32_t address, unsigned position) const {
        if (position >= change_at && address == changed_address) return changed_value;
        if (address == 0xd3a08) return 0xd5400;
        for (unsigned slot = 0; slot < 4; ++slot) {
            const uint32_t base = 0xd3a00 + slot * 0x1cc;
            if (address == base + 0x20) return slot ? 0 : 0xd6000;
            if (address == base + 0xc4) return slot ? 0 : 0x101;
            if (address == base + 0xd0) return slot ? 0 : 0x200 |
                ((video_staging ? position >= 126 :
                  (position / (video_graph ? 62U : video_prefix ? 126U : return_header ? 204U : metadata_pool ? 196U : 140U) == 1 ||
                   position / (video_graph ? 62U : video_prefix ? 126U : return_header ? 204U : metadata_pool ? 196U : 140U) == 2)) ? 0x10000 : 0);
        }
        const uint32_t locations[] = {0xd6000,0xd6008,0xd600c,0xd6064,0xd60cc,0xd6224,
            0xd5408,0xd55a0,0xd55d4,0xd55d8,0xd55dc,0xd5800,0xd5808,0xd5810,0xd5814,
            0xd5a18,0xd5a1c,0xd5a28,0xd5a30,0xd5a34,0xd5a40,0x11601c,0x116020,0x11602c,0x116034,0x116038};
        const uint32_t values[] = {0,physical,0x3f940,0xd5400,0,0x116004,
            0x116004,0xd5800,0x200000,0x200000,0x1000000,0xd5400,0xd5a00,0x1000000,0x200000,
            0x200000,0x1200000,0x200000,0x200000,0x1000000,1,0x116068,0x3ffc000,0x116004,0x116004,0x3ee5ffc};
        for (unsigned word = 0; word < sizeof(locations) / sizeof(locations[0]); ++word)
            if (address == locations[word]) return values[word];
        if ((video_prefix || video_staging) && prefix_zero && address >= 0x200000U && address < 0x200080U) return 0;
        const unsigned stage_reads = video_graph ? 62U : video_prefix ? 126U : return_header ? 204U : metadata_pool ? 196U : 140U;
        return all_ones ? 0xffffffffU : 0x5a000000U | ((position / stage_reads) << 16) | (address & 65535U) |
            (different && position % stage_reads >= stage_reads / 2 ? 0x80000000U : 0);
    }
    static BC_STATUS Read(HANDLE handle, uint32_t *values, uint32_t bytes, uint32_t address) {
        auto *f = static_cast<PpbContextFixture *>(handle);
        const unsigned position = f->calls++, at = f->video_staging ? (position < 126 ? position % 63 : (position - 126) % 31) :
            position % (f->video_graph ? 31U : f->video_prefix ? 63U : f->return_header ? 102U : f->metadata_pool ? 98U : 70U);
        f->valid &= position < (f->video_staging ? 250U : f->video_graph ? 186U : f->video_prefix ? 378U : f->return_header ? 816U : f->metadata_pool ? 784U : f->post_stop ? 560U : 420U) && values &&
            address == f->Address(at) && bytes == Count(at, f->metadata_pool, f->return_header,
                f->video_prefix || (f->video_staging && position < 126), f->video_graph || (f->video_staging && position >= 126)) * 4U &&
            reinterpret_cast<uintptr_t>(values) % alignof(uint32_t) == 0;
        if (!f->valid) return BC_STS_ERROR;
        for (unsigned word = 0; word < bytes / 4; ++word) values[word] = f->Word(address + word * 4, position);
        if (position == f->lose_at) f->current = f->replacement;
        return position == f->fail_at ? BC_STS_BUSY : BC_STS_SUCCESS;
    }
};

// Native framing observations only: the revision pin is an observed board
// profile, not a universal reset value. Sequential passes may differ; sync,
// line counts and EOL/EOF are raw fields, not accepted-transfer/completion proof.
// Fixed named memory-window front DWORDs only. The register backend has no
// independent bus-ACK result; zero GISB guards do not authenticate a RAM alias.
enum class AvdMemoryFailure { None, Argument, Mode, Owner, Order, Barrier, Budget, Api, Revision, Gisb };
static uint32_t AvdMemoryAddress(unsigned field)
{
    const uint32_t addresses[] = {BCHP_MFD_REVISION_ID, BCHP_SUN_GISB_ARB_ERR_CAP_STATUS,
        BCHP_DECODE_CPUIMEM_0_CPUIMEM_REG, BCHP_DECODE_CPUDMEM_0_CPUDMEM_REG,
        BCHP_DECODE_CPUIMEM2_0_CPUIMEM_REG, BCHP_DECODE_CPUDMEM2_0_CPUDMEM_REG,
        BCHP_SUN_GISB_ARB_ERR_CAP_STATUS, BCHP_MFD_REVISION_ID};
    return field < 8 ? addresses[field] : 0;
}
struct AvdMemoryObserver {
    typedef BC_STATUS (*Reader)(HANDLE, uint32_t, uint32_t *);
    bool enabled = false, conflicting_mode = false, failed = false;
    unsigned reads = 0, bytes = 0, next_stage = 0;
    uint32_t raw[3][2][8] = {};
    unsigned measured[3][2] = {};
    bool complete[3][2] = {};
    BC_STATUS api_status = BC_STS_SUCCESS;
    AvdMemoryFailure failure = AvdMemoryFailure::None;
    uint32_t rejected_address = 0, rejected_raw = 0;
    bool rejected_raw_valid = false;
private:
    HANDLE owner = nullptr;
    bool sealed = false;
    bool ModeReady() const { return sealed && enabled && !conflicting_mode; }
    bool Reject(AvdMemoryFailure why) {
        if (!failed) failure = why;
        failed = true;
        return false;
    }
public:
    void Report(unsigned stage) const {
        if (stage > 2) return;
        const char *const stages[] = {"after-OPEN/pre-START", "first-output-after-release-and-owned-write", "delivery-EOS-before-STOP"};
        const char *const failures[] = {"none", "argument", "mode", "current-handle-loss", "order", "native-barrier", "budget", "api-status", "revision", "GISB-error"};
        std::printf("AVD memory inventory: stage=%s reads=%u/48 bytes=%u/192 api-status=%d failure=%s\n",
            stages[stage], reads, bytes, api_status, failures[static_cast<unsigned>(failure)]);
        for (unsigned pass = 0; pass < 2; ++pass) {
            std::printf("AVD memory inventory raw: stage=%s pass=%u", stages[stage], pass);
            if (complete[stage][pass]) {
                for (unsigned field = 0; field < 8; ++field)
                    std::printf(" %08x=%08x", AvdMemoryAddress(field), raw[stage][pass][field]);
            } else std::printf(" INCOMPLETE measured=%u/8", measured[stage][pass]);
            std::printf("\n");
        }
        if (rejected_raw_valid) std::printf("AVD memory inventory rejected guard: address=%08x raw=%08x\n", rejected_address, rejected_raw);
        std::printf("AVD memory inventory scope: named-front-DWORDs only; target-writes=0 error-clear-writes=0 pointer-follow=0 "
            "atomic/alias/current-context/pixels/lease/completion-certified=no\n");
        std::fflush(stdout);
    }
    bool Observe(const HANDLE *current, unsigned stage, bool barrier,
        Reader reader = DtsDevRegisterRead, bool report = true) {
        if (failed) return false;
        if (!enabled && !sealed) return true;
        if (!enabled || conflicting_mode) return Reject(AvdMemoryFailure::Mode);
        if (!current || !*current || !reader) return Reject(AvdMemoryFailure::Argument);
        if (stage > 2 || stage != next_stage) return Reject(AvdMemoryFailure::Order);
        if (barrier != (stage != 0)) return Reject(AvdMemoryFailure::Barrier);
        if (reads != stage * 16U || bytes != stage * 64U) return Reject(AvdMemoryFailure::Budget);
        if (!sealed) { owner = *current; sealed = true; }
        if (*current != owner) return Reject(AvdMemoryFailure::Owner);
        bool ok = true;
        for (unsigned pass = 0; pass < 2 && ok; ++pass) {
            for (unsigned field = 0; field < 8 && ok; ++field) {
                if (!ModeReady()) { ok = Reject(AvdMemoryFailure::Mode); break; }
                if (!*current || *current != owner) { ok = Reject(AvdMemoryFailure::Owner); break; }
                if (reads >= 48 || bytes > 192U - 4U) { ok = Reject(AvdMemoryFailure::Budget); break; }
                uint32_t value = 0;
                ++reads; bytes += 4;
                api_status = reader(owner, AvdMemoryAddress(field), &value);
                if (api_status != BC_STS_SUCCESS) { ok = Reject(AvdMemoryFailure::Api); break; }
                if (!ModeReady()) { ok = Reject(AvdMemoryFailure::Mode); break; }
                if (!*current || *current != owner) { ok = Reject(AvdMemoryFailure::Owner); break; }
                const unsigned ordinal = stage * 16U + pass * 8U + field + 1U;
                if (reads != ordinal || bytes != ordinal * 4U) { ok = Reject(AvdMemoryFailure::Budget); break; }
                raw[stage][pass][field] = value;
                ++measured[stage][pass];
                if ((field == 0 || field == 7) ? value != 0x50U : (field == 1 || field == 6) && value != 0) {
                    rejected_address = AvdMemoryAddress(field); rejected_raw = value; rejected_raw_valid = true;
                    ok = Reject(field == 0 || field == 7 ? AvdMemoryFailure::Revision : AvdMemoryFailure::Gisb);
                }
            }
            if (ok) complete[stage][pass] = true;
        }
        if (ok) ++next_stage;
        if (report) Report(stage);
        return ok;
    }
    bool Finish(bool native_ok, bool report = true) const {
        if (!sealed && !enabled && !failed) return native_ok;
        bool observed = !failed && ModeReady() && next_stage == 3 && reads == 48 && bytes == 192;
        for (unsigned stage = 0; stage < 3; ++stage) for (unsigned pass = 0; pass < 2; ++pass)
            observed = observed && complete[stage][pass] && measured[stage][pass] == 8;
        if (report) {
            std::printf("AVD memory inventory finish: native-result=%s observation-result=%s stages=%u/3 reads=%u/48 bytes=%u/192 "
                "alias/current-context/lease/completion-certified=no\n", native_ok ? "PASS" : "FAIL", observed ? "PASS" : "FAIL", next_stage, reads, bytes);
            std::fflush(stdout);
        }
        return native_ok && observed;
    }
};

enum class MfdFramingFailure { None, Argument, Read, Revision, Reserved, Unavailable, Owner, Order, Publication };
struct MfdFramingSnapshot {
    uint32_t raw[10] = {};
    unsigned reads = 0, measured = 0;
    BC_STATUS status = BC_STS_SUCCESS;
    MfdFramingFailure failure = MfdFramingFailure::None;
    bool Equal() const { return measured == 10 && !std::memcmp(raw, raw + 5, 5 * sizeof(uint32_t)); }
};
static uint32_t MfdFramingAddress(unsigned field)
{
    const uint32_t addresses[] = {BCHP_MFD_REVISION_ID, BCHP_MFD_BVB_SAMPLE_DATA,
        BCHP_MFD_FEED_STATUS, BCHP_MFD_FEEDER_BVB_STATUS, BCHP_MFD_REVISION_ID};
    return field < 5 ? addresses[field] : 0;
}
static uint32_t MfdFramingReserved(unsigned field)
{
    const uint32_t masks[] = {0xffff0000U, 0xfc000000U, 0xdfffe000U, 0xfffffffcU, 0xffff0000U};
    return field < 5 ? masks[field] : 0xffffffffU;
}
static bool ReadMfdFraming(const HANDLE *current, HANDLE owner, MfdFramingSnapshot *snapshot,
    BC_STATUS (*reader)(HANDLE, uint32_t, uint32_t *) = DtsDevRegisterRead)
{
    if (!snapshot) return false;
    *snapshot = MfdFramingSnapshot{};
    if (!current || !*current || !owner || !reader) {
        snapshot->failure = MfdFramingFailure::Argument; return false;
    }
    for (unsigned index = 0; index < 10; ++index) {
        if (!*current || *current != owner) {
            snapshot->failure = MfdFramingFailure::Owner; return false;
        }
        uint32_t raw = 0;
        ++snapshot->reads;
        snapshot->status = reader(owner, MfdFramingAddress(index % 5), &raw);
        if (snapshot->status != BC_STS_SUCCESS) {
            snapshot->failure = MfdFramingFailure::Read; return false;
        }
        if (!*current || *current != owner) {
            snapshot->failure = MfdFramingFailure::Owner; return false;
        }
        snapshot->raw[index] = raw;
        ++snapshot->measured;
        if (raw == 0xffffffffU) snapshot->failure = MfdFramingFailure::Unavailable;
        else if ((index % 5 == 0 || index % 5 == 4) && raw != 0x50U)
            snapshot->failure = MfdFramingFailure::Revision;
        else if (raw & MfdFramingReserved(index % 5)) snapshot->failure = MfdFramingFailure::Reserved;
        if (snapshot->failure != MfdFramingFailure::None) return false;
    }
    return true;
}
struct MfdFramingObserver {
    typedef BC_STATUS (*Reader)(HANDLE, uint32_t, uint32_t *);
    bool enabled = false, failed = false;
    HANDLE owner = nullptr;
    unsigned attempted = 0, reads = 0, last_frame = 0;
    MfdFramingSnapshot snapshot;
    bool Reject(MfdFramingFailure why) { failed = true; snapshot.failure = why; return false; }
    void Report(unsigned stage) const {
        const char *const stages[] = {"after-OPEN/pre-START", "frame1-after-release-and-owned-write",
            "frame90-after-release-and-owned-write"};
        const char *const failures[] = {"none", "argument", "read-status", "revision", "reserved-bits",
            "all-ones-unavailable", "current-handle-loss", "order", "release-or-owned-write"};
        const char *const fields[] = {"rev", "bvb-sample", "feed-status", "bvb-status", "closing-rev"};
        std::printf("MFD framing: stage=%s api-reads=%u total-api-reads=%u measured=%u api-status=%d "
            "failure=%s result=%s raw-equal=%s target-writes=0 test-port-data-reads=0 "
            "atomic/admission/lease/completion-certified=no\n", stages[stage], snapshot.reads, reads,
            snapshot.measured, snapshot.status, failures[static_cast<unsigned>(snapshot.failure)],
            failed ? "FAIL" : "PASS", snapshot.measured == 10 ? (snapshot.Equal() ? "yes" : "no") : "NOT-READ");
        for (unsigned index = 0; index < 10; ++index) {
            const unsigned field = index % 5;
            std::printf("MFD framing raw: stage=%s pass=%u %s@%08x=", stages[stage], index / 5,
                fields[field], MfdFramingAddress(field));
            if (index < snapshot.measured) {
                const uint32_t raw = snapshot.raw[index];
                std::printf("%08x", raw);
                if (!(raw & MfdFramingReserved(field))) {
                    if (field == 1) std::printf(" PICTURE_SYNC=%u LINE_SYNC=%u COLOUR_SYNC=%u LUMA=%u CHROMA=%u",
                        (raw >> 24) & 3U, (raw >> 22) & 3U, (raw >> 20) & 3U, (raw >> 10) & 1023U, raw & 1023U);
                    if (field == 2) std::printf(" LAST_LINE=%u LINE_COUNT=%u", (raw >> 29) & 1U, raw & 8191U);
                    if (field == 3) std::printf(" EOF=%u EOL=%u", (raw >> 1) & 1U, raw & 1U);
                }
            } else std::printf("NOT-READ");
            std::printf("\n");
        }
        std::fflush(stdout);
    }
    bool Observe(const HANDLE *current, unsigned stage, bool released, bool written,
        Reader reader = DtsDevRegisterRead, bool report = true) {
        if (!enabled) return true;
        if (failed) return false;
        if (!current || !*current || !reader) return Reject(MfdFramingFailure::Argument);
        if (stage > 2 || attempted != (stage == 0 ? 0U : stage == 1 ? 1U : 3U) ||
            last_frame != (stage == 2 ? 89U : 0U)) return Reject(MfdFramingFailure::Order);
        if (stage && (!released || !written)) return Reject(MfdFramingFailure::Publication);
        if (!stage && (released || written)) return Reject(MfdFramingFailure::Order);
        if (!stage) owner = *current;
        if (*current != owner) return Reject(MfdFramingFailure::Owner);
        attempted |= 1U << stage;
        const bool ok = ReadMfdFraming(current, owner, &snapshot, reader);
        reads += snapshot.reads;
        failed = !ok;
        if (report) Report(stage);
        return ok;
    }
    bool PreStart(const HANDLE *current, Reader reader = DtsDevRegisterRead, bool report = true) {
        return Observe(current, 0, false, false, reader, report);
    }
    bool AfterDelivered(const HANDLE *current, unsigned frame, bool released, bool written,
        Reader reader = DtsDevRegisterRead, bool report = true) {
        if (!enabled) return true;
        if (failed) return false;
        if (!current || !*current || !reader) return Reject(MfdFramingFailure::Argument);
        if (!owner || *current != owner) return Reject(MfdFramingFailure::Owner);
        if (!released || !written) return Reject(MfdFramingFailure::Publication);
        if (!frame || frame > 180 || frame != last_frame + 1 ||
            attempted != (frame == 1 ? 1U : frame <= 90 ? 3U : 7U)) return Reject(MfdFramingFailure::Order);
        if ((frame == 1 || frame == 90) && !Observe(current, frame == 1 ? 1 : 2, released, written, reader, report))
            return false;
        last_frame = frame;
        return true;
    }
    bool Finish(bool native_ok, bool report = true) const {
        if (!enabled) return native_ok;
        const bool diagnostic_ok = !failed && attempted == 7 && reads == 30 && last_frame == 180;
        if (report) {
            std::printf("MFD framing finish: native-result=%s diagnostic-result=%s stages=%u/3 "
                "api-reads=%u/30 target-writes=0 test-port-data-reads=0 "
                "atomic/admission/lease/completion-certified=no\n", native_ok ? "PASS" : "FAIL",
                diagnostic_ok ? "PASS" : "FAIL", (attempted & 1U) + ((attempted >> 1) & 1U) +
                ((attempted >> 2) & 1U), reads);
            std::fflush(stdout);
        }
        return native_ok && diagnostic_ok;
    }
};
struct MfdFramingFixture {
    uint32_t raw[30] = {};
    unsigned calls = 0, fail_at = 30, lose_at = 30;
    BC_STATUS status = BC_STS_ERROR;
    bool valid = true;
    HANDLE current = this, replacement = nullptr;
    MfdFramingFixture() {
        for (unsigned index = 0; index < 30; ++index) {
            const uint32_t values[] = {0x50, 0x03ffffff, 0x20001fff, 3, 0x50};
            raw[index] = values[index % 5];
        }
    }
    static BC_STATUS Read(HANDLE handle, uint32_t address, uint32_t *value) {
        auto *fixture = static_cast<MfdFramingFixture *>(handle);
        const unsigned index = fixture->calls++;
        const uint32_t expected[] = {0x00540000, 0x00540078, 0x00540050, 0x00540070, 0x00540000};
        if (index >= 30 || !value) { fixture->valid = false; return BC_STS_ERROR; }
        fixture->valid &= address == expected[index % 5];
        *value = index == fixture->fail_at ? 0xdeadbeefU : fixture->raw[index];
        if (index == fixture->lose_at) fixture->current = fixture->replacement;
        return index == fixture->fail_at ? fixture->status : BC_STS_SUCCESS;
    }
    bool Exercise(MfdFramingObserver *observer, bool report = false) {
        observer->enabled = true;
        if (!observer->PreStart(&current, Read, report)) return false;
        for (unsigned frame = 1; frame <= 180; ++frame)
            if (!observer->AfterDelivered(&current, frame, true, true, Read, report)) return false;
        return observer->Finish(true, report);
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

// Empirical status-clear test only. The RDB names CLEAR and its bit masks,
// but does not specify W1C timing, concurrent set/clear priority or persistence.
enum class SclStatusFailure { None, Argument, Api, Revision, Reserved, Profile, Unstable };
struct SclStatusTrace { uint32_t raw[6] = {}; unsigned measured = 0, reads = 0; };
struct SclStatusTest {
    unsigned mode = 0, stages = 0, reads = 0, writes = 0;
    uint32_t pre_status = 0;
    bool fatal = false, diagnostic_failed = false, clear_attempted = false;
    HANDLE owner = nullptr;
    BC_STATUS api_status = BC_STS_SUCCESS;
    SclStatusFailure failure = SclStatusFailure::None;
    bool Enabled() const { return mode != 0; }
    bool Reject(SclStatusFailure why) { fatal = diagnostic_failed = true; failure = why; return false; }
    bool Sample(HANDLE handle, unsigned stage, SclViewProbe::Reader reader, bool report) {
        if (!Enabled()) return true;
        if (fatal) return false;
        if (!handle || !reader || mode > 2 || stage > 2 || stages != ((1U << stage) - 1) ||
            (stage && handle != owner)) return Reject(SclStatusFailure::Argument);
        if (!stage) owner = handle;
        stages |= 1U << stage; // One attempt, before the first API call.
        SclStatusTrace trace; bool ok = true;
        const uint32_t addresses[] = {BCHP_SCL_HD_REVISION_ID, BCHP_SCL_HD_BVB_IN_STATUS, BCHP_SCL_HD_REVISION_ID};
        const char *const names[] = {"rev", "status", "closing-rev"};
        for (unsigned index = 0; index < 6 && ok; ++index) {
            uint32_t raw = 0; ++reads; ++trace.reads;
            api_status = reader(handle, addresses[index % 3], &raw);
            if (api_status != BC_STS_SUCCESS) ok = Reject(SclStatusFailure::Api);
            else {
                trace.raw[index] = raw; ++trace.measured;
                if (raw & ~(index % 3 == 1 ? 0xffU : 0xffffU)) ok = Reject(SclStatusFailure::Reserved);
                else if (index % 3 != 1) { if (raw != 0x80) ok = Reject(SclStatusFailure::Revision); }
                else {
                    diagnostic_failed |= raw != 0;
                    if ((!stage && raw != 0 && raw != 1 && raw != 5) ||
                        (stage && (raw & ~pre_status))) ok = Reject(SclStatusFailure::Profile);
                }
            }
        }
        if (ok && !stage && std::memcmp(trace.raw, trace.raw + 3, 3 * sizeof(uint32_t)))
            ok = Reject(SclStatusFailure::Unstable);
        if (ok && !stage) pre_status = trace.raw[1];
        if (report) {
            const char *const phases[] = {"last-output-after-release-and-owned-write/pre", "immediate", "complete-EOS-delivery-barrier"};
            const char *const failures[] = {"none", "argument", "api-status", "revision", "reserved-bits", "unadmitted-status", "pre-unstable"};
            std::printf("SCL status test: mode=%s stage=%s reads=%u total-reads=%u target-write-attempts=%u "
                "measured=%u api-status=%d failure=%s diagnostic=%s raw-stable=%s "
                "board-profile=REV80 API-success-not-transport-certificate=yes non-atomic=yes clear-semantics-unproved=yes\n",
                mode == 1 ? "observe" : "clear", phases[stage], trace.reads, reads, writes, trace.measured,
                api_status, failures[static_cast<unsigned>(failure)], diagnostic_failed ? "FAIL" : "INCOMPLETE",
                trace.measured == 6 ? (std::memcmp(trace.raw, trace.raw + 3, 3 * sizeof(uint32_t)) ? "no" : "yes") : "NOT-READ");
            for (unsigned index = 0; index < 6; ++index) {
                std::printf("SCL status raw: stage=%s pass=%u %s@%08x=", phases[stage], index / 3, names[index % 3], addresses[index % 3]);
                if (index < trace.measured) std::printf("%08x", trace.raw[index]); else std::printf("NOT-READ");
                std::printf("\n");
            }
            std::fflush(stdout);
        }
        return ok;
    }
    bool AfterDelivered(HANDLE handle, unsigned frame, bool released, bool written,
        SclViewProbe::Reader reader = DtsDevRegisterRead, SclViewProbe::Writer writer = DtsDevRegisterWr, bool report = true) {
        if (!Enabled()) return true;
        if (fatal) return false;
        if (frame != 180) return true;
        if (!released || !written) return Reject(SclStatusFailure::Argument);
        if (mode > 2 || (mode == 2 && !writer)) return Reject(SclStatusFailure::Argument);
        if (!Sample(handle, 0, reader, report)) return false;
        if (mode == 2 && pre_status) {
            clear_attempted = true; ++writes; // A failed return does not prove no write occurred.
            if (report) { std::printf("SCL status clear attempt: address=005408a0 value=%08x attempts=%u\n", pre_status, writes); std::fflush(stdout); }
            api_status = writer(handle, BCHP_SCL_HD_BVB_IN_STATUS_CLEAR, pre_status);
            if (report) { std::printf("SCL status clear return: api-status=%d\n", api_status); std::fflush(stdout); }
            if (api_status != BC_STS_SUCCESS) return Reject(SclStatusFailure::Api);
        } else if (report) {
            std::printf("SCL status action: %s pre-status=%08x empirical-clear-success=no\n", mode == 2 ? "NO_ACTION" : "OBSERVE_ONLY", pre_status);
            std::fflush(stdout);
        }
        return Sample(handle, 1, reader, report);
    }
    bool Eos(HANDLE handle, bool complete, SclViewProbe::Reader reader = DtsDevRegisterRead, bool report = true) {
        if (!Enabled()) return true;
        return complete && !fatal && Sample(handle, 2, reader, report);
    }
    bool DiagnosticOkay() const { return !Enabled() || (stages == 7 && !fatal && !diagnostic_failed); }
    bool Finish(bool native_success, bool report = true) const {
        const bool aggregate = native_success && DiagnosticOkay();
        if (Enabled() && report) {
            std::printf("SCL status final: native-full-delivery/EOS/capture/cleanup=%s diagnostic=%s aggregate=%s "
                "reads=%u target-write-attempts=%u action=%s fresh-work/stale-only/source-lease/completion-certified=no\n",
                native_success ? "PASS" : "FAIL", DiagnosticOkay() ? "PASS" : "FAIL", aggregate ? "PASS" : "FAIL",
                reads, writes, clear_attempted ? "ONE_ATTEMPT" : mode == 2 ? "NO_ACTION" : "OBSERVE_ONLY");
            std::fflush(stdout);
        }
        return aggregate;
    }
};
struct SclStatusFixture {
    struct Event { bool write; uint32_t address, raw; };
    std::vector<Event> events;
    unsigned calls = 0, write_calls = 0;
    size_t fail_at = SIZE_MAX;
    BC_STATUS status = BC_STS_ERROR;
    bool valid = true;
    SclStatusTest *probe = nullptr;
    SclStatusFixture(unsigned mode = 2, uint32_t pre = 1) {
        Tuple(pre, pre);
        if (mode == 2 && pre) events.push_back({true, 0x5408a0, pre});
        Tuple(0, 0); Tuple(0, 0);
    }
    void Tuple(uint32_t first, uint32_t second) {
        for (uint32_t raw : {first, second}) {
            events.push_back({false, 0x540800, 0x80}); events.push_back({false, 0x5408a4, raw});
            events.push_back({false, 0x540800, 0x80});
        }
    }
    static BC_STATUS Read(HANDLE handle, uint32_t address, uint32_t *value) {
        auto *fixture = static_cast<SclStatusFixture *>(handle); const unsigned index = fixture->calls++;
        if (index >= fixture->events.size()) { fixture->valid = false; return BC_STS_ERROR; }
        const auto &event = fixture->events[index]; fixture->valid &= !event.write && address == event.address && value;
        *value = index == fixture->fail_at ? 0xdeadbeefU : event.raw;
        return index == fixture->fail_at ? fixture->status : BC_STS_SUCCESS;
    }
    static BC_STATUS Write(HANDLE handle, uint32_t address, uint32_t raw) {
        auto *fixture = static_cast<SclStatusFixture *>(handle); const unsigned index = fixture->calls++; ++fixture->write_calls;
        if (index >= fixture->events.size()) { fixture->valid = false; return BC_STS_ERROR; }
        const auto &event = fixture->events[index]; fixture->valid &= event.write && address == event.address && raw == event.raw &&
            fixture->probe && fixture->probe->clear_attempted && fixture->probe->writes == 1;
        return index == fixture->fail_at ? fixture->status : BC_STS_SUCCESS;
    }
    bool Exercise(SclStatusTest *subject, bool report = false) {
        probe = subject;
        return subject->AfterDelivered(this, 180, true, true, Read, Write, report) &&
               subject->Eos(this, true, Read, report);
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
    bool observe_scl_filter_map = false;
    unsigned observe_scl_view = 0;
    bool observe_mfd_config = false;
    bool observe_mfd_address = false;
    bool observe_mfd_framing = false;
    bool observe_runtime_inventory = false;
    bool observe_arm_metadata = false;
    bool observe_arm_source_shape = false;
    bool observe_ppb_context = false;
    bool observe_ppb_stop = false;
    bool observe_ppb_metadata = false;
    bool observe_ppb_return = false;
    bool observe_video_prefix = false;
    bool observe_video_graph = false;
    bool observe_video_staging = false;
    bool observe_avd_memory = false;
    unsigned inject_mfd_colour = 0;
    unsigned scl_status_test = 0;
    BC_OUTPUT_FORMAT output_format = OUTPUT_MODE422_YUY2;
};

static bool AvdMemoryConflicts(const Options &o)
{
    return o.observe_chroma || o.observe_scl_config || o.observe_scl_filter_map || o.observe_scl_view ||
        o.observe_mfd_config || o.observe_mfd_address || o.observe_mfd_framing || o.observe_runtime_inventory ||
        o.observe_arm_metadata || o.observe_arm_source_shape || o.observe_ppb_context || o.observe_ppb_stop ||
        o.observe_ppb_metadata || o.observe_ppb_return || o.observe_video_prefix || o.observe_video_graph ||
        o.observe_video_staging || o.inject_mfd_colour || o.scl_status_test || o.open_only || o.mpeg1_via_mpeg2 || o.h263_via_divx;
}

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
    if (arguments.size() >= 2 && (!std::strcmp(arguments.back(), "--observe-arm-metadata") ||
                                 !std::strcmp(arguments.back(), "--observe-arm-source-shape") ||
                                 !std::strcmp(arguments.back(), "--observe-ppb-context") ||
                                 !std::strcmp(arguments.back(), "--observe-ppb-stop") ||
                                 !std::strcmp(arguments.back(), "--observe-ppb-metadata") ||
                                 !std::strcmp(arguments.back(), "--observe-ppb-return") ||
                                 !std::strcmp(arguments.back(), "--observe-video-prefix") ||
                                 !std::strcmp(arguments.back(), "--observe-video-graph") ||
                                 !std::strcmp(arguments.back(), "--observe-video-staging"))) {
        options->observe_video_prefix = !std::strcmp(arguments.back(), "--observe-video-prefix");
        options->observe_video_graph = !std::strcmp(arguments.back(), "--observe-video-graph");
        options->observe_video_staging = !std::strcmp(arguments.back(), "--observe-video-staging");
        options->observe_ppb_return = !std::strcmp(arguments.back(), "--observe-ppb-return");
        options->observe_ppb_metadata = options->observe_ppb_return || !std::strcmp(arguments.back(), "--observe-ppb-metadata");
        options->observe_ppb_stop = options->observe_ppb_metadata || !std::strcmp(arguments.back(), "--observe-ppb-stop");
        options->observe_ppb_context = options->observe_video_staging || options->observe_video_graph || options->observe_video_prefix || options->observe_ppb_stop || !std::strcmp(arguments.back(), "--observe-ppb-context");
        options->observe_arm_source_shape = !std::strcmp(arguments.back(), "--observe-arm-source-shape");
        options->observe_arm_metadata = !options->observe_arm_source_shape && !options->observe_ppb_context;
        arguments.pop_back();
    }
    if (arguments.size() >= 2 && !std::strcmp(arguments.back(), "--observe-avd-memory")) {
        options->observe_avd_memory = true;
        arguments.pop_back();
    }
    if (arguments.size() >= 2 && !std::strcmp(arguments.back(), "--observe-runtime-inventory")) {
        options->observe_runtime_inventory = true;
        arguments.pop_back();
    }
    if (arguments.size() >= 2 && !std::strcmp(arguments.back(), "--observe-mfd-framing")) {
        options->observe_mfd_framing = true;
        arguments.pop_back();
    }
    if (arguments.size() >= 2 && !std::strcmp(arguments.back(), "--observe-mfd-address")) {
        options->observe_mfd_address = true;
        arguments.pop_back();
    }
    if (arguments.size() >= 2 && !std::strcmp(arguments.back(), "--observe-scl-filter-map")) {
        options->observe_scl_filter_map = true;
        arguments.pop_back();
    }
    if (arguments.size() >= 2 && !std::strcmp(arguments.back(), "--observe-scl-config")) {
        options->observe_scl_config = true;
        arguments.pop_back();
    }
    if (arguments.size() >= 3 && !std::strcmp(arguments[arguments.size() - 2], "--scl-status-test")) {
        if (std::strcmp(arguments.back(), "observe") && std::strcmp(arguments.back(), "clear")) return false;
        options->scl_status_test = !std::strcmp(arguments.back(), "observe") ? 1 : 2;
        arguments.resize(arguments.size() - 2);
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
        if (options->observe_avd_memory || options->capture_path || options->observe_arm_metadata || options->observe_arm_source_shape || options->observe_ppb_context || options->observe_runtime_inventory || options->observe_chroma || options->observe_scl_config || options->observe_scl_filter_map || options->observe_scl_view || options->observe_mfd_config || options->observe_mfd_address || options->observe_mfd_framing || options->inject_mfd_colour || options->scl_status_test || options->scaler_test || options->mpeg1_via_mpeg2 ||
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
    if (options->observe_avd_memory && (!hardware || !options->capture_path || !options->scaler_test ||
        options->scale_width || options->expected != 180 || options->iterations != 1 ||
        options->output_format != OUTPUT_MODE422_YUY2 || AvdMemoryConflicts(*options))) return false;
    if ((options->observe_arm_metadata || options->observe_arm_source_shape || options->observe_ppb_context) &&
        (!hardware || !options->capture_path || !options->scaler_test || options->scale_width ||
         options->expected != 180 || options->iterations != 1 || options->output_format != OUTPUT_MODE422_YUY2 ||
         options->observe_chroma || options->observe_scl_config || options->observe_scl_filter_map ||
         options->observe_scl_view || options->observe_mfd_config || options->observe_mfd_address ||
         options->observe_mfd_framing || options->observe_runtime_inventory || options->inject_mfd_colour ||
         options->scl_status_test || options->mpeg1_via_mpeg2 || options->h263_via_divx || options->open_only)) return false;
    if (options->observe_mfd_framing &&
        (!hardware || !options->capture_path || !options->scaler_test || options->scale_width ||
         options->expected != 180 || options->iterations != 1 ||
         options->output_format != OUTPUT_MODE422_YUY2 || options->observe_chroma ||
         options->observe_scl_config || options->observe_scl_filter_map || options->observe_scl_view ||
         options->observe_mfd_config || options->observe_mfd_address || options->observe_runtime_inventory ||
         options->inject_mfd_colour || options->scl_status_test || options->mpeg1_via_mpeg2 ||
         options->h263_via_divx || options->open_only)) return false;
    if (options->observe_runtime_inventory &&
        (!hardware || !options->capture_path || !options->scaler_test || options->scale_width ||
         options->expected != 180 || options->iterations != 1 ||
         options->output_format != OUTPUT_MODE422_YUY2 || options->observe_chroma ||
         options->observe_scl_config || options->observe_scl_filter_map || options->observe_scl_view ||
         options->observe_mfd_config || options->observe_mfd_address || options->inject_mfd_colour ||
         options->scl_status_test || options->mpeg1_via_mpeg2 || options->h263_via_divx || options->open_only)) return false;
    if (options->observe_mfd_address &&
        (!hardware || !options->capture_path || !options->scaler_test || options->scale_width ||
         options->expected != 180 || options->iterations != 1 ||
         options->output_format != OUTPUT_MODE422_YUY2 || options->observe_chroma ||
         options->observe_scl_config || options->observe_scl_filter_map || options->observe_scl_view ||
         options->observe_mfd_config || options->inject_mfd_colour || options->scl_status_test ||
         options->mpeg1_via_mpeg2 || options->h263_via_divx || options->open_only)) return false;
    if (options->observe_scl_filter_map &&
        (!hardware || !options->capture_path || !options->scaler_test ||
         options->expected != 180 || options->iterations != 1 ||
         options->output_format != OUTPUT_MODE422_YUY2 ||
         options->mpeg1_via_mpeg2 || options->h263_via_divx || options->open_only ||
         options->observe_chroma || options->observe_scl_config || options->observe_scl_view ||
         options->observe_mfd_config || options->inject_mfd_colour || options->scl_status_test ||
         (options->scale_width != 0 && options->scale_width != 320 && options->scale_width != 640)))
        return false;
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
    if (options->scl_status_test &&
        (!hardware || !options->capture_path || !options->scaler_test || options->scale_width != 320 ||
         options->expected != 180 || options->iterations != 1 || options->output_format != OUTPUT_MODE422_YUY2 ||
         options->observe_mfd_config || options->inject_mfd_colour || options->observe_scl_config || options->observe_scl_view ||
         options->observe_chroma || options->mpeg1_via_mpeg2 || options->h263_via_divx || options->open_only)) return false;
    return !options->capture_path ||
        (hardware && options->scaler_test && !options->open_only && options->iterations == 1);
}

static bool SclInputAdmitted(const Options &options, const Input &input)
{
    return (!options.observe_runtime_inventory && !options.observe_scl_config && !options.observe_scl_filter_map && !options.observe_scl_view && !options.observe_mfd_config && !options.observe_mfd_address && !options.observe_mfd_framing && !options.inject_mfd_colour && !options.scl_status_test) ||
        (options.expected == 180 && input.codec == AV_CODEC_ID_MPEG2VIDEO &&
         input.subtype == BC_MSUBTYPE_MPEG2VIDEO && input.progressive &&
         input.width == 640 && input.height == 360 && input.packets.size() == 180);
}

static bool NeedsRawIo(const Options &options)
{
    return options.observe_avd_memory || options.observe_arm_metadata || options.observe_arm_source_shape || options.observe_ppb_context || options.observe_runtime_inventory || options.observe_chroma || options.observe_scl_config || options.observe_scl_filter_map ||
        options.observe_scl_view || options.observe_mfd_config || options.observe_mfd_address || options.observe_mfd_framing || options.inject_mfd_colour || options.scl_status_test;
}

static bool SubmittedPacketDigestMatches(const Input &input, size_t expected_bytes, const char *expected_sha)
{
    if (!expected_sha || !expected_bytes || expected_bytes > 124832 || input.packets.size() > 180) return false;
    GChecksum *checksum = g_checksum_new(G_CHECKSUM_SHA256);
    if (!checksum) return false;
    size_t total = 0;
    bool ok = true;
    for (const Packet &packet : input.packets) {
        if (!packet.size || packet.size > packet.data.size() || packet.size > expected_bytes - total) { ok = false; break; }
        g_checksum_update(checksum, packet.data.data(), static_cast<gssize>(packet.size));
        total += packet.size;
    }
    ok = ok && total == expected_bytes && !std::strcmp(g_checksum_get_string(checksum), expected_sha);
    g_checksum_free(checksum);
    return ok;
}
static bool ArmMetadataInputShape(const Options &options, const Input &input)
{
    return options.expected == 180 && input.codec == AV_CODEC_ID_H264 && input.subtype == BC_MSUBTYPE_H264 &&
        input.progressive && input.width == 256 && input.height == 96 && input.packets.size() == 180;
}
static bool ArmMetadataInputAdmitted(const Options &options, const Input &input)
{
    return (!options.observe_arm_metadata && !options.observe_arm_source_shape && !options.observe_ppb_context) || (ArmMetadataInputShape(options, input) &&
        SubmittedPacketDigestMatches(input, 124832, "1363a87c8f59fab6187cd13653a3ba8a41fd994066d30c24be1c2b09d675666e"));
}
static bool AvdMemoryInputShape(const Options &options, const Input &input)
{
    return ArmMetadataInputShape(options, input) && input.metadata.empty();
}
static bool AvdMemoryInputAdmitted(const Options &options, const Input &input)
{
    return !options.observe_avd_memory || (AvdMemoryInputShape(options, input) &&
        SubmittedPacketDigestMatches(input, 124832, "1363a87c8f59fab6187cd13653a3ba8a41fd994066d30c24be1c2b09d675666e"));
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

#include "library-scl-filter-map-test.h"

template<class Check> static void MfdAddressSelfTest(const Check &check)
{
    check(BCHP_MFD_REVISION_ID == 0x00540000U && BCHP_MFD_TEST_PORT_CNTL == 0x0054007cU &&
        BCHP_MFD_TEST_PORT_CNTL_reserved0_MASK == 0xfffffff0U &&
        BCHP_MFD_TEST_PORT_CNTL_ADDR_SEL_MASK == 8U && BCHP_MFD_TEST_PORT_CNTL_ADDR_SEL_SHIFT == 3 &&
        BCHP_MFD_TEST_PORT_CNTL_TP_ADDR_MASK == 7U && BCHP_MFD_TEST_PORT_CNTL_TP_ADDR_SHIFT == 0 &&
        BCHP_MFD_TEST_PORT_CNTL_ADDR_SEL_PIN_INPUT == 0 &&
        BCHP_MFD_TEST_PORT_CNTL_ADDR_SEL_SOFT_INPUT == 1,
        "MFD debug address whitelist and numeric address-source fields match independent literals");
    for (uint32_t control = 0; control < 16; ++control) {
        MfdAddressFixture fixture;
        fixture.raw[1] = fixture.raw[4] = control;
        fixture.raw[7] = fixture.raw[10] = 15U - control;
        MfdAddressObserver subject;
        check(fixture.Exercise(&subject) && fixture.valid && fixture.calls == 12 &&
            subject.reads == 12 && subject.attempted == 3 && subject.last_frame == 180 &&
            subject.owner == &fixture && !subject.failed && subject.snapshot.Stable() &&
            !subject.Finish(false, false),
            "MFD debug address all 16 controls at both stages allow opaque interstage changes and require full native success");
    }
    for (unsigned position = 0; position < 12; ++position) {
        for (int code = -1; code <= BC_STS_PWR_MGMT; ++code) {
            if (code == BC_STS_SUCCESS) continue;
            MfdAddressFixture fixture; fixture.fail_at = position; fixture.status = static_cast<BC_STATUS>(code);
            MfdAddressObserver subject; subject.enabled = true;
            if (position >= 6) check(subject.PreStart(&fixture.current, MfdAddressFixture::Read, false),
                "MFD debug address second-stage API fault setup");
            const bool accepted = position < 6 ? subject.PreStart(&fixture.current, MfdAddressFixture::Read, false) :
                subject.AfterDelivered(&fixture.current, 1, true, true, MfdAddressFixture::Read, false);
            check(!accepted && subject.failed && fixture.valid && fixture.calls == position + 1 &&
                subject.reads == position + 1 && subject.snapshot.reads == position % 6 + 1 &&
                subject.snapshot.measured == position % 6 && subject.snapshot.status == code &&
                subject.snapshot.failure == MfdAddressFailure::Read && !subject.Finish(true, false),
                "MFD debug address all 27 failing API statuses at all 12 read ordinals stop at the exact read");
            bool unread_zero = true;
            for (unsigned unread = position % 6; unread < 6; ++unread)
                unread_zero &= subject.snapshot.raw[unread] == 0;
            check(unread_zero, "MFD debug address failed API poison and unread words remain unpublished");
            MfdAddressFixture other;
            check(!subject.PreStart(&fixture.current, MfdAddressFixture::Read, false) &&
                !subject.AfterDelivered(&fixture.current, 1, true, true, MfdAddressFixture::Read, false) &&
                !subject.AfterDelivered(&other.current, 2, true, true, MfdAddressFixture::Read, false) &&
                fixture.calls == position + 1 && other.calls == 0,
                "MFD debug address API failure is sticky with no retry, later-stage or changed-owner I/O");
        }
        std::vector<uint32_t> invalid = {0xffffffffU};
        if (position % 3 == 1) {
            for (unsigned bit = 4; bit < 32; ++bit) invalid.push_back(1U << bit);
        } else {
            invalid.insert(invalid.end(), {0U, 1U, 0xffffU, 0x51U});
            for (unsigned bit = 0; bit < 32; ++bit) invalid.push_back(0x50U ^ (1U << bit));
        }
        for (uint32_t raw : invalid) {
            MfdAddressFixture fixture; fixture.raw[position] = raw;
            MfdAddressObserver subject; subject.enabled = true;
            if (position >= 6) check(subject.PreStart(&fixture.current, MfdAddressFixture::Read, false),
                "MFD debug address second-stage scalar fault setup");
            const bool accepted = position < 6 ? subject.PreStart(&fixture.current, MfdAddressFixture::Read, false) :
                subject.AfterDelivered(&fixture.current, 1, true, true, MfdAddressFixture::Read, false);
            const auto expected = raw == 0xffffffffU ? MfdAddressFailure::Unavailable :
                position % 3 == 1 ? MfdAddressFailure::Reserved : MfdAddressFailure::Revision;
            check(!accepted && subject.failed && fixture.valid && fixture.calls == position + 1 &&
                subject.reads == position + 1 && subject.snapshot.measured == position % 6 + 1 &&
                subject.snapshot.failure == expected && !subject.Finish(true, false),
                "MFD debug address all-ones/every reserved bit/every revision bit stops at each ordinal");
            check(!subject.AfterDelivered(&fixture.current, 1, true, true, MfdAddressFixture::Read, false) &&
                !subject.PreStart(&fixture.current, MfdAddressFixture::Read, false) && fixture.calls == position + 1,
                "MFD debug address scalar failure prevents all subsequent diagnostic I/O");
        }
        for (bool change : {false, true}) {
            MfdAddressFixture fixture, other; fixture.lose_at = position;
            fixture.replacement = change ? static_cast<HANDLE>(&other) : nullptr;
            MfdAddressObserver subject; subject.enabled = true;
            if (position >= 6) check(subject.PreStart(&fixture.current, MfdAddressFixture::Read, false),
                "MFD debug address second-stage current-handle fault setup");
            const bool accepted = position < 6 ? subject.PreStart(&fixture.current, MfdAddressFixture::Read, false) :
                subject.AfterDelivered(&fixture.current, 1, true, true, MfdAddressFixture::Read, false);
            check(!accepted && subject.failed && fixture.valid && fixture.calls == position + 1 &&
                subject.reads == position + 1 && subject.snapshot.measured == position % 6 &&
                subject.snapshot.failure == MfdAddressFailure::Owner && other.calls == 0,
                "MFD debug address null or replaced current HANDLE during every callback excludes the next read");
            fixture.current = &fixture;
            check(!subject.PreStart(&fixture.current, MfdAddressFixture::Read, false) &&
                !subject.AfterDelivered(&fixture.current, 1, true, true, MfdAddressFixture::Read, false) &&
                fixture.calls == position + 1 && other.calls == 0,
                "MFD debug address current-handle loss remains sticky after apparent handle recovery");
        }
    }
    for (unsigned position : {1U, 4U, 7U, 10U}) {
        for (unsigned bit = 0; bit < 4; ++bit) {
            MfdAddressFixture fixture; fixture.raw[position] ^= 1U << bit;
            MfdAddressObserver subject; subject.enabled = true;
            if (position >= 6) check(subject.PreStart(&fixture.current, MfdAddressFixture::Read, false),
                "MFD debug address second-stage unequal tuple setup");
            const bool accepted = position < 6 ? subject.PreStart(&fixture.current, MfdAddressFixture::Read, false) :
                subject.AfterDelivered(&fixture.current, 1, true, true, MfdAddressFixture::Read, false);
            const unsigned stopped = position < 6 ? 5 : 11;
            check(!accepted && subject.failed && fixture.valid && fixture.calls == stopped &&
                subject.reads == stopped && subject.snapshot.measured == 5 &&
                subject.snapshot.failure == MfdAddressFailure::Unstable && !subject.snapshot.Stable(),
                "MFD debug address every unreserved bit at either control ordinal rejects at the first differing repeat");
            check(!subject.AfterDelivered(&fixture.current, 1, true, true, MfdAddressFixture::Read, false) &&
                fixture.calls == stopped && !subject.Finish(true, false),
                "MFD debug address unequal tuples cause no closing or followup diagnostic read");
        }
    }
    {
        MfdAddressFixture fixture;
        struct { uint32_t before = 0x12345678; MfdAddressSnapshot value;
                 uint32_t after = 0x87654321; } guarded;
        check(ReadMfdAddress(&fixture.current, &fixture, &guarded.value, MfdAddressFixture::Read) &&
            fixture.valid && fixture.calls == 6 && guarded.value.Stable() &&
            guarded.before == 0x12345678 && guarded.after == 0x87654321,
            "MFD debug address complete six-word tuple stays within poisoned canaries");
        fixture.calls = 0;
        HANDLE missing = nullptr;
        check(!ReadMfdAddress(&fixture.current, &fixture, nullptr, MfdAddressFixture::Read) &&
            !ReadMfdAddress(nullptr, &fixture, &guarded.value, MfdAddressFixture::Read) &&
            !ReadMfdAddress(&missing, &fixture, &guarded.value, MfdAddressFixture::Read) &&
            !ReadMfdAddress(&fixture.current, nullptr, &guarded.value, MfdAddressFixture::Read) &&
            !ReadMfdAddress(&fixture.current, &fixture, &guarded.value, nullptr) && fixture.calls == 0,
            "MFD debug address null snapshot/current/handle/owner/callback has zero I/O");
        MfdAddressObserver disabled;
        check(disabled.Observe(nullptr, UINT_MAX, false, false, nullptr, false) &&
            disabled.AfterDelivered(nullptr, UINT_MAX, false, false, nullptr, false) &&
            disabled.Finish(true, false) && !disabled.Finish(false, false) && !disabled.reads,
            "MFD debug address disabled mode is a zero-I/O no-op preserving native result");
        for (unsigned invalid = 0; invalid < 8; ++invalid) {
            MfdAddressObserver subject; subject.enabled = true;
            check(!subject.Observe(invalid == 0 ? nullptr : invalid == 1 ? &missing : &fixture.current,
                    invalid == 3 ? 2U : invalid == 4 ? UINT_MAX : invalid == 5 ? 1U : 0U,
                    invalid == 6, invalid == 7, invalid == 2 ? nullptr : MfdAddressFixture::Read, false) &&
                subject.failed && fixture.calls == 0 &&
                !subject.PreStart(&fixture.current, MfdAddressFixture::Read, false) && fixture.calls == 0,
                "MFD debug address invalid initial arguments/stage/order latch before I/O");
        }
        for (unsigned phase = 0; phase < 3; ++phase) {
            for (unsigned invalid = 0; invalid < 10; ++invalid) {
                MfdAddressFixture owner, other; MfdAddressObserver subject; subject.enabled = true;
                if (phase) check(subject.PreStart(&owner.current, MfdAddressFixture::Read, false),
                    "MFD debug address delivery admission failure setup");
                if (phase == 2) check(subject.AfterDelivered(&owner.current, 1, true, true, MfdAddressFixture::Read, false),
                    "MFD debug address post-sample failure setup");
                const unsigned stopped = owner.calls;
                HANDLE missing_handle = nullptr;
                const HANDLE *current = invalid == 0 ? nullptr : invalid == 1 ? &missing_handle :
                    invalid == 2 ? &other.current : &owner.current;
                const unsigned frame = invalid == 5 ? 0 : invalid == 6 ? 181 :
                    invalid == 7 ? 3 : invalid == 8 && phase == 2 ? 1 : phase == 2 ? 2 : 1;
                const bool accepted = invalid == 8 && phase != 2 ?
                    subject.Observe(&owner.current, phase ? 0 : 1, false, false, MfdAddressFixture::Read, false) :
                    subject.AfterDelivered(current, frame, invalid != 3, invalid != 4,
                        invalid == 9 ? nullptr : MfdAddressFixture::Read, false);
                check(!accepted && subject.failed && owner.calls == stopped && other.calls == 0 &&
                    !subject.AfterDelivered(&owner.current, phase == 2 ? 2 : 1, true, true, MfdAddressFixture::Read, false) &&
                    owner.calls == stopped && !subject.Finish(true, false),
                    "MFD debug address missing owner/current/release/write/ordinal/duplicate rejection is sticky at every phase");
            }
        }
        for (const auto &barrier : {std::make_pair(false, false), std::make_pair(false, true), std::make_pair(true, false)}) {
            MfdAddressFixture owner; MfdAddressObserver subject; subject.enabled = true;
            check(subject.PreStart(&owner.current, MfdAddressFixture::Read, false) &&
                !subject.AfterDelivered(&owner.current, 1, barrier.first, barrier.second, MfdAddressFixture::Read, false) &&
                subject.failed && subject.snapshot.failure == MfdAddressFailure::Publication && owner.calls == 6 &&
                !subject.AfterDelivered(&owner.current, 1, true, true, MfdAddressFixture::Read, false) && owner.calls == 6,
                "MFD debug address every incomplete release/write barrier latches without any output-stage I/O");
        }
        MfdAddressFixture partial; MfdAddressObserver subject; subject.enabled = true;
        check(!subject.Finish(true, false) && subject.PreStart(&partial.current, MfdAddressFixture::Read, false) &&
            !subject.Finish(true, false) &&
            subject.AfterDelivered(&partial.current, 1, true, true, MfdAddressFixture::Read, false) &&
            !subject.Finish(true, false) && partial.calls == 12,
            "MFD debug address finish requires both stages and full 180 delivery without performing I/O");
    }
    for (unsigned scenario = 0; scenario < 18; ++scenario) {
        FILE *record = std::tmpfile(); const int saved_stdout = dup(STDOUT_FILENO);
        std::fflush(stdout);
        const bool redirected = record && saved_stdout >= 0 && dup2(fileno(record), STDOUT_FILENO) >= 0;
        bool expected_result = false;
        if (redirected) {
            MfdAddressFixture fixture; MfdAddressObserver subject; subject.enabled = true;
            fixture.raw[1] = fixture.raw[4] = scenario < 16 ? scenario : 0;
            if (scenario == 16) fixture.fail_at = 1;
            if (scenario == 17) fixture.raw[4] = 1;
            expected_result = subject.PreStart(&fixture.current, MfdAddressFixture::Read) == (scenario < 16);
            std::fflush(stdout);
        }
        const bool restored = saved_stdout >= 0 && dup2(saved_stdout, STDOUT_FILENO) >= 0;
        if (saved_stdout >= 0) close(saved_stdout);
        char text[8192] = {}; size_t bytes = 0;
        if (record) { std::rewind(record); bytes = std::fread(text, 1, sizeof(text) - 1, record); std::fclose(record); }
        char fields[96] = {};
        std::snprintf(fields, sizeof(fields), "ADDR_SEL=%u address-source-enum=%s TP_ADDR=%u",
            scenario >> 3, scenario < 8 ? "PIN_INPUT" : "SOFT_INPUT", scenario & 7U);
        check(redirected && restored && expected_result && bytes &&
            std::strstr(text, "target-writes=0 data-reads=0") &&
            std::strstr(text, "pixel-source/view-identity/admission/ownership/freshness-certified=no") &&
            !std::strstr(text, "deadbeef") && !std::strstr(text, "selected-active-view") &&
            (scenario < 16 ? (std::strstr(text, fields) && std::strstr(text, "reads=6") &&
                std::strstr(text, "raw-stable=yes")) : (std::strstr(text, "result=FAIL") &&
                std::strstr(text, "closing-rev@00540000=NOT-READ") &&
                std::strstr(text, scenario == 16 ? "failure=read-status" : "failure=unequal-tuples"))),
            "MFD debug address actual report exposes numeric fields/address-source-only labels and suppresses poison/unread words");
    }
    {
        const std::vector<const char *> valid = {"probe", "--hardware", "fixture", "180", "30", "1",
            "--scaler-test", "0", "--observe-mfd-address", "--capture-yuy2", "new"};
        Options admitted;
        check(ParseArguments(valid, &admitted) && admitted.observe_mfd_address && NeedsRawIo(admitted) &&
            admitted.iterations == 1 && !NeedsRawIo(Options{}),
            "MFD debug address opt-in native capture requires CAP_SYS_RAWIO before any fixture/progress/capture/device action");
        Options implicit;
        check(ParseArguments({"probe", "--hardware", "fixture", "180", "--scaler-test", "0",
            "--observe-mfd-address", "--capture-yuy2", "new"}, &implicit) && implicit.iterations == 1,
            "MFD debug address implicit single iteration remains admitted");
        for (unsigned field = 0; field < 16; ++field) {
            auto arguments = valid;
            if (field == 0) arguments[1] = "--preflight";
            if (field == 1) arguments[3] = "179";
            if (field == 2) arguments[5] = "2";
            if (field == 3) arguments[7] = "320";
            if (field == 4) arguments[7] = "640";
            if (field == 5) arguments[9] = "--capture-uyvy";
            if (field == 6) arguments.resize(9);
            if (field == 7) arguments[10] = "";
            if (field == 8) arguments[10] = "-";
            if (field == 9) arguments.erase(arguments.begin() + 6, arguments.begin() + 8);
            if (field == 10) arguments.insert(arguments.begin() + 8, "--mpeg1-via-mpeg2");
            if (field == 11) arguments.insert(arguments.begin() + 8, "--h263-via-divx");
            if (field == 12) arguments.insert(arguments.begin() + 8, "--open-only");
            if (field == 13) std::swap(arguments[8], arguments[9]);
            if (field == 14) arguments.resize(10);
            if (field == 15) arguments.insert(arguments.end(), {"--capture-yuy2", "another"});
            Options rejected;
            check(!ParseArguments(arguments, &rejected), "MFD debug address rejects other modes/counts/scaling/capture paths/placement before actions");
        }
        for (const auto &mixed : std::vector<std::vector<const char *>>{
                {"--observe-chroma"}, {"--observe-scl-config"}, {"--observe-scl-filter-map"},
                {"--observe-scl-view", "2"}, {"--observe-scl-view", "3"}, {"--observe-mfd-config"},
                {"--observe-mfd-address"}, {"--inject-mfd-colour", "a"}, {"--inject-mfd-colour", "b"},
                {"--scl-status-test", "observe"}, {"--scl-status-test", "clear"}}) {
            for (unsigned where : {8U, 9U}) {
                auto arguments = valid; arguments.insert(arguments.begin() + where, mixed.begin(), mixed.end());
                Options rejected; check(!ParseArguments(arguments, &rejected),
                    "MFD debug address every other diagnostic/duplicate is rejected in both orders");
            }
        }
        Options rejected;
        check(!ParseArguments({"probe", "--self-test", "--observe-mfd-address"}, &rejected),
            "MFD debug address option cannot be combined with self-test");
        Input native; native.codec = AV_CODEC_ID_MPEG2VIDEO; native.subtype = BC_MSUBTYPE_MPEG2VIDEO;
        native.progressive = true; native.width = 640; native.height = 360; native.packets.resize(180);
        check(SclInputAdmitted(admitted, native), "MFD debug address exact native progressive 640x360/180 packet profile admitted");
        for (unsigned field = 0; field < 8; ++field) {
            Options changed = admitted; Input altered = native;
            if (field == 0) altered.codec = AV_CODEC_ID_H264;
            if (field == 1) altered.subtype = BC_MSUBTYPE_H264;
            if (field == 2) altered.progressive = false;
            if (field == 3) altered.width = 638;
            if (field == 4) altered.height = 358;
            if (field == 5) altered.packets.pop_back();
            if (field == 6) altered.packets.emplace_back();
            if (field == 7) changed.expected = 179;
            check(!SclInputAdmitted(changed, altered) && SclInputAdmitted(Options{}, altered),
                "MFD debug address rejects codec/subtype/progressive/shape/packet/expected mismatch while default admission remains intact");
        }
    }
}

template<class Check> static void ArmMetadataSelfTest(const Check &check)
{
    for (unsigned variant = 0; variant < 3; ++variant) {
        ArmMetadataFixture fixture; fixture.different = variant == 1; fixture.all_ones = variant == 2;
        ArmMetadataObserver subject; subject.enabled = true;
        for (unsigned stage = 0; stage < 3; ++stage) {
            check(subject.Observe(&fixture.current, stage, stage != 0, ArmMetadataFixture::Read, false) &&
                fixture.valid && fixture.calls == (stage + 1) * 40 && subject.reads == fixture.calls &&
                subject.bytes == (stage + 1) * 256 && subject.measured == 40 && subject.Stable() == !fixture.different &&
                subject.raw[0][0][0] == (fixture.all_ones ? 0xffffffffU : stage ? (stage << 24) | 1U : 0),
                "ARM fixed four-slot/two-pass tuples retain distinct stages, zero/stale/all-ones and unequal observations without rejection");
        }
        check(subject.Finish(true, false) && !subject.Finish(false, false) && fixture.calls == 120,
            "ARM observations require all three stages and native success; finish adds no I/O");
    }
    for (unsigned position = 0; position < 120; ++position) for (unsigned fault = 0; fault < 3; ++fault) {
        ArmMetadataFixture fixture, other;
        fixture.fail_at = fault == 0 ? position : 120;
        fixture.lose_at = fault ? position : 120;
        fixture.replacement = fault == 2 ? static_cast<HANDLE>(&other) : nullptr;
        ArmMetadataObserver subject;
        check(!fixture.Exercise(&subject) && fixture.valid && fixture.calls == position + 1 &&
            subject.reads == position + 1 && subject.measured == position % 40 &&
            subject.failure == (fault ? ArmMetadataFailure::Owner : ArmMetadataFailure::Read) &&
            !subject.Observe(&fixture.current, 0, false, ArmMetadataFixture::Read, false) &&
            !subject.Observe(&other.current, 2, true, ArmMetadataFixture::Read, false) &&
            fixture.calls == position + 1 && other.calls == 0,
            "ARM every API failure/current-handle-null/replacement ordinal stops exactly and latches zero future I/O");
    }
    for (unsigned prior = 0; prior < 4; ++prior) {
        ArmMetadataFixture fixture; ArmMetadataObserver subject; subject.enabled = true;
        for (unsigned stage = 0; stage < prior; ++stage)
            check(subject.Observe(&fixture.current, stage, stage != 0, ArmMetadataFixture::Read, false), "ARM ordered admission setup");
        check(!subject.Observe(&fixture.current, prior ? prior - 1 : 1, prior != 1, ArmMetadataFixture::Read, false) &&
            subject.failure == ArmMetadataFailure::Order && fixture.calls == prior * 40,
            "ARM duplicate/out-of-order stages refuse without reads, including after the full budget");
    }
    for (unsigned invalid = 0; invalid < 7; ++invalid) {
        ArmMetadataFixture fixture, other; ArmMetadataObserver subject; subject.enabled = true;
        HANDLE missing = nullptr;
        if (invalid == 3) subject.reads = 81;
        if (invalid >= 5) check(subject.Observe(&fixture.current, 0, false, ArmMetadataFixture::Read, false), "ARM first barrier setup");
        if (invalid == 6) fixture.current = &other;
        check(!subject.Observe(invalid == 0 ? nullptr : invalid == 1 ? &missing : &fixture.current,
            invalid >= 5 ? 1 : 0, invalid == 4 || invalid == 6,
            invalid == 2 ? nullptr : ArmMetadataFixture::Read, false) && subject.failed &&
            fixture.calls == (invalid >= 5 ? 40U : 0U),
            "ARM null arguments, budget, wrong publication barriers and interstage owner change refuse before reads");
    }
    ArmMetadataObserver disabled;
    check(disabled.Observe(nullptr, 999, true, nullptr, false) && disabled.Finish(true, false) &&
        !disabled.Finish(false, false) && disabled.reads == 0 && disabled.next_stage == 0,
        "ARM default-disabled has zero calls and leaves ordinary native success unchanged");
    {
        ArmMetadataFixture fixture; ArmMetadataObserver subject; subject.enabled = true;
        check(subject.Observe(&fixture.current, 0, false, ArmMetadataFixture::Read, false) &&
            subject.Observe(&fixture.current, 1, true, ArmMetadataFixture::Read, false) &&
            !subject.Observe(&fixture.current, 2, false, ArmMetadataFixture::Read, false) &&
            subject.failure == ArmMetadataFailure::Barrier && fixture.calls == 80 && !subject.Finish(true, false),
            "ARM final sample requires a complete delivery EOS barrier and performs no I/O without it");
    }
    {
        FILE *record = std::tmpfile(); const int saved = dup(STDOUT_FILENO);
        std::fflush(stdout);
        const bool redirected = record && saved >= 0 && dup2(fileno(record), STDOUT_FILENO) >= 0;
        if (redirected) {
            ArmMetadataFixture fixture; ArmMetadataObserver subject; subject.enabled = true;
            subject.Observe(&fixture.current, 0, false, ArmMetadataFixture::Read);
            fixture.different = true;
            subject.Observe(&fixture.current, 1, true, ArmMetadataFixture::Read);
        }
        std::fflush(stdout);
        const bool restored = saved >= 0 && dup2(saved, STDOUT_FILENO) >= 0;
        if (saved >= 0) close(saved);
        char text[16384] = {}; size_t bytes = 0;
        if (record) { std::rewind(record); bytes = std::fread(text, 1, sizeof(text) - 1, record); std::fclose(record); }
        check(redirected && restored && bytes && std::strstr(text, "observed-stable,non-atomic") &&
            std::strstr(text, "observed-different,non-atomic") && std::strstr(text, "cached-meta-virtual=") &&
            std::strstr(text, "e8-opaque=") && std::strstr(text, "cached-source+34=") &&
            std::strstr(text, "cached-source-is-current-input=unproven") && std::strstr(text, "started=0") &&
            std::strstr(text, "branch-selector-if-active-and-started=FRESH-dequeue") &&
            std::strstr(text, "branch-selector-if-active-and-started=CACHE-copy"),
            "ARM compact reports distinguish cached metadata/source/opaque/branch scalars and informational non-atomic stability");
    }
    const std::vector<const char *> valid = {"probe", "--hardware", "fixture", "180", "30", "1",
        "--scaler-test", "0", "--observe-arm-metadata", "--capture-yuy2", "new"};
    Options admitted;
    check(ParseArguments(valid, &admitted) && admitted.observe_arm_metadata && NeedsRawIo(admitted) && !NeedsRawIo(Options{}),
        "ARM explicit hardware/native/unscaled/one YUY2 capture requires raw-I/O capability");
    Options implicit;
    check(ParseArguments({"probe", "--hardware", "fixture", "180", "--scaler-test", "0", "--observe-arm-metadata",
        "--capture-yuy2", "new"}, &implicit) && implicit.iterations == 1, "ARM implicit single iteration admits the same bounded profile");
    for (unsigned field = 0; field < 12; ++field) {
        auto arguments = valid;
        if (field == 0) arguments[1] = "--preflight";
        if (field == 1) arguments[3] = "179";
        if (field == 2) arguments[5] = "2";
        if (field == 3) arguments[7] = "320";
        if (field == 4) arguments[9] = "--capture-uyvy";
        if (field == 5) arguments.resize(9);
        if (field == 6) arguments.erase(arguments.begin() + 6, arguments.begin() + 8);
        if (field == 7) arguments[10] = "";
        if (field == 8) arguments[10] = "-";
        if (field == 9) arguments[8] = "--observe-arm-metadata=0";
        if (field == 10) arguments.insert(arguments.begin() + 9, "0xd3a00");
        if (field == 11) arguments.insert(arguments.end(), {"--capture-yuy2", "another"});
        Options rejected; check(!ParseArguments(arguments, &rejected), "ARM parser rejects unsupported mode/count/scaling/capture/slot/address syntax before actions");
    }
    for (const auto &mixed : std::vector<std::vector<const char *>>{{"--observe-chroma"}, {"--observe-scl-config"},
        {"--observe-scl-filter-map"}, {"--observe-scl-view", "2"}, {"--observe-mfd-config"}, {"--observe-mfd-address"},
        {"--observe-mfd-framing"}, {"--observe-runtime-inventory"}, {"--observe-arm-metadata"}, {"--inject-mfd-colour", "a"},
        {"--scl-status-test", "observe"}, {"--mpeg1-via-mpeg2"}, {"--h263-via-divx"}, {"--open-only"}})
        for (unsigned where : {8U, 9U}) {
            auto arguments = valid; arguments.insert(arguments.begin() + where, mixed.begin(), mixed.end());
            Options rejected; check(!ParseArguments(arguments, &rejected), "ARM excludes all other diagnostics/injection/legacy/open-only and duplicates in both orders");
        }
    Options rejected;
    check(!ParseArguments({"probe", "--self-test", "--observe-arm-metadata"}, &rejected), "ARM observer cannot mix with self-test");
    Input native; native.codec = AV_CODEC_ID_H264; native.subtype = BC_MSUBTYPE_H264;
    native.progressive = true; native.width = 256; native.height = 96; native.packets.resize(180);
    check(ArmMetadataInputShape(admitted, native) && SclInputAdmitted(admitted, native) &&
        !ArmMetadataInputAdmitted(admitted, native), "ARM shape admission is separate from old MPEG2-only gates and requires the fixed input digest before capture/device");
    for (unsigned field = 0; field < 8; ++field) {
        Input altered = native; Options changed = admitted;
        if (field == 0) altered.codec = AV_CODEC_ID_MPEG2VIDEO;
        if (field == 1) altered.subtype = BC_MSUBTYPE_MPEG2VIDEO;
        if (field == 2) altered.progressive = false;
        if (field == 3) altered.width = 254;
        if (field == 4) altered.height = 94;
        if (field == 5) altered.packets.pop_back();
        if (field == 6) altered.packets.emplace_back();
        if (field == 7) changed.expected = 179;
        check(!ArmMetadataInputShape(changed, altered) && !ArmMetadataInputAdmitted(changed, altered) &&
            ArmMetadataInputAdmitted(Options{}, altered) && SclInputAdmitted(Options{}, altered), "ARM shape mismatches refuse while disabled/default admission is unchanged");
    }
    for (unsigned packet = 0; packet < 180; ++packet) {
        native.packets[packet].size = packet ? 1 : 124653;
        native.packets[packet].data.resize(native.packets[packet].size, 0);
    }
    check(!ArmMetadataInputAdmitted(admitted, native), "ARM exact shape/byte count with a noncanonical digest refuses before capture/device");
    Input canonical; canonical.packets.resize(2);
    canonical.packets[0].data = {'a', 0xee}; canonical.packets[0].size = 1;
    canonical.packets[1].data = {'b', 'c', 0xff}; canonical.packets[1].size = 2;
    const char *abc = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
    check(SubmittedPacketDigestMatches(canonical, 3, abc), "ARM digest hashes concatenated submitted packet.size bytes, excluding padding, against canonical abc SHA256");
    check(!SubmittedPacketDigestMatches(canonical, 4, abc) && !SubmittedPacketDigestMatches(canonical, 3, nullptr),
        "ARM digest requires exact submitted total and a supplied private-test digest");
    for (unsigned fault = 0; fault < 6; ++fault) {
        Input changed = canonical;
        if (fault == 0) changed.packets[0].size = 0;
        if (fault == 1) changed.packets[0].size = 3;
        if (fault == 2) changed.packets[0].size = 2;
        if (fault == 3) changed.packets[0].data[0] = 'x';
        if (fault == 4) changed.packets.resize(181);
        check(!SubmittedPacketDigestMatches(changed, fault == 5 ? 124833 : 3, abc), "ARM digest rejects zero/out-of-storage/excess/changed bytes, packet and byte budgets");
    }
}

template<class Check> static void ArmSourceShapeSelfTest(const Check &check)
{
    ArmMetadataObserver disabled; disabled.source_shape = true;
    check(disabled.Observe(nullptr, 9, true, nullptr, false) && disabled.Finish(true, false) && !disabled.reads,
        "ARM source-shape disabled path adds no I/O or owner/barrier constraints");
    for (unsigned variant = 0; variant < 3; ++variant) {
        ArmMetadataFixture fixture; fixture.source_shape = true; fixture.fail_at = fixture.lose_at = 240;
        fixture.different = variant == 1; fixture.all_ones = variant == 2;
        ArmMetadataObserver subject; subject.enabled = subject.source_shape = true;
        for (unsigned stage = 0; stage < 3; ++stage) {
            check(subject.Observe(&fixture.current, stage, stage != 0, ArmMetadataFixture::Read, false) &&
                fixture.valid && fixture.calls == (stage + 1) * 80 && subject.reads == fixture.calls &&
                subject.bytes == (stage + 1) * 640 && subject.measured == 80 && subject.Stable() == !fixture.different,
                "ARM source-shape reads exactly ten fixed aligned spans/four slots/two non-atomic passes without rejecting raw values");
            for (unsigned word = 0; word < 20; ++word)
                check(subject.raw[0][0][word] == (fixture.all_ones ? 0xffffffffU : stage ? (stage << 24) | (word + 1) : 0),
                    "ARM source-shape raw words preserve all twenty separate field locations and stage values");
        }
        check(subject.Finish(true, false) && !subject.Finish(false, false) && fixture.calls == 240,
            "ARM source-shape completion requires three barriers/240reads/1920bytes and native success, without additional I/O");
    }
    for (unsigned position = 0; position < 240; ++position) for (unsigned fault = 0; fault < 3; ++fault) {
        ArmMetadataFixture fixture, other; fixture.source_shape = true;
        fixture.fail_at = fault == 0 ? position : 240; fixture.lose_at = fault ? position : 240;
        fixture.replacement = fault == 2 ? other.current : nullptr;
        ArmMetadataObserver subject; subject.enabled = subject.source_shape = true;
        bool result = true;
        for (unsigned stage = 0; stage < 3 && result; ++stage)
            result = subject.Observe(&fixture.current, stage, stage != 0, ArmMetadataFixture::Read, false);
        check(!result && fixture.valid && subject.failed && fixture.calls == position + 1 &&
            subject.failure == (fault ? ArmMetadataFailure::Owner : ArmMetadataFailure::Read) &&
            !subject.Observe(&fixture.current, 0, false, ArmMetadataFixture::Read, false) &&
            !subject.Observe(&other.current, 2, true, ArmMetadataFixture::Read, false) &&
            !subject.Finish(true, false) && fixture.calls == position + 1 && !other.calls,
            "ARM source-shape all read/handle-loss/replacement positions fail sticky and never use another owner's device");
    }
    for (unsigned invalid = 0; invalid < 7; ++invalid) {
        ArmMetadataFixture fixture; fixture.source_shape = true; fixture.fail_at = fixture.lose_at = 240;
        ArmMetadataObserver subject; subject.enabled = subject.source_shape = true;
        if (invalid >= 4) check(subject.Observe(&fixture.current, 0, false, ArmMetadataFixture::Read, false),
            "ARM source-shape invalid-stage setup");
        if (invalid == 3) subject.reads = 161;
        if (invalid == 6) subject.source_shape = false;
        check(!subject.Observe(&fixture.current, invalid == 0 ? 3 : invalid == 4 ? 0 : invalid >= 5 ? 1 : 0,
            invalid == 1 || invalid == 6, invalid == 2 ? nullptr : ArmMetadataFixture::Read, false) &&
            subject.failed && fixture.calls == (invalid >= 4 ? 80U : 0U),
            "ARM source-shape rejects wrong order/barrier/null reader/budget and mid-session plan changes before I/O");
    }
    const std::vector<const char *> valid = {"probe", "--hardware", "fixture", "180", "30", "1",
        "--scaler-test", "0", "--observe-arm-source-shape", "--capture-yuy2", "new"};
    Options admitted;
    check(ParseArguments(valid, &admitted) && admitted.observe_arm_source_shape && !admitted.observe_arm_metadata && NeedsRawIo(admitted),
        "ARM source-shape is separately opted in and requires CAP_SYS_RAWIO before fixture/progress/capture/device");
    const std::vector<std::vector<const char *>> forbidden = {{"--observe-arm-metadata"}, {"--observe-arm-source-shape"},
        {"--observe-runtime-inventory"}, {"--observe-mfd-framing"}, {"--observe-mfd-config"}, {"--observe-mfd-address"},
        {"--observe-scl-config"}, {"--observe-scl-filter-map"}, {"--observe-scl-view", "2"}, {"--observe-chroma"},
        {"--inject-mfd-colour", "a"}, {"--scl-status-test", "observe"}, {"--open-only"}, {"--mpeg1-via-mpeg2"}, {"--h263-via-divx"}};
    for (const auto &extra : forbidden) for (unsigned order = 0; order < 2; ++order) {
        auto arguments = valid; const auto where = order ? arguments.end() - 2 : arguments.begin() + 8;
        arguments.insert(where, extra.begin(), extra.end()); Options rejected;
        check(!ParseArguments(arguments, &rejected), "ARM source-shape refuses all mixed/duplicate diagnostics in either order");
    }
    for (unsigned invalid = 0; invalid < 9; ++invalid) {
        auto arguments = valid;
        if (invalid == 0) arguments[1] = "--preflight";
        if (invalid == 1) arguments[3] = "179";
        if (invalid == 2) arguments[5] = "2";
        if (invalid == 3) arguments[7] = "128";
        if (invalid == 4) arguments[9] = "--capture-uyvy";
        if (invalid == 5) arguments[10] = "-";
        if (invalid == 6) arguments.resize(9);
        if (invalid == 7) arguments[8] = "--observe-arm-source-shape=0";
        if (invalid == 8) arguments.erase(arguments.begin() + 6, arguments.begin() + 8);
        Options rejected;
        check(!ParseArguments(arguments, &rejected), "ARM source-shape refuses preflight/unpinned frame count/repeat/scaling/packing/missing capture or scaler");
    }
    Input native; native.codec = AV_CODEC_ID_H264; native.subtype = BC_MSUBTYPE_H264;
    native.progressive = true; native.width = 256; native.height = 96; native.packets.resize(180);
    check(ArmMetadataInputShape(admitted, native) && !ArmMetadataInputAdmitted(admitted, native) &&
        ArmMetadataInputAdmitted(Options{}, native) && SclInputAdmitted(admitted, native),
        "ARM source-shape shares exact H264 digest admission without broadening other observers or the default path");
    for (unsigned field = 0; field < 7; ++field) {
        Input altered = native; Options changed = admitted;
        if (field == 0) altered.width = 128;
        if (field == 1) altered.height = 48;
        if (field == 2) altered.progressive = false;
        if (field == 3) altered.codec = AV_CODEC_ID_MPEG2VIDEO;
        if (field == 4) altered.subtype = BC_MSUBTYPE_MPEG2VIDEO;
        if (field == 5) altered.packets.pop_back();
        if (field == 6) changed.expected = 179;
        check(!ArmMetadataInputShape(changed, altered) && !ArmMetadataInputAdmitted(changed, altered),
            "ARM source-shape shape mismatches refuse before capture and device");
    }
    FILE *record = std::tmpfile(); const int saved = dup(STDOUT_FILENO);
    std::fflush(stdout);
    const bool redirected = record && saved >= 0 && dup2(fileno(record), STDOUT_FILENO) >= 0;
    if (redirected) {
        ArmMetadataFixture fixture; fixture.source_shape = fixture.all_ones = true; fixture.fail_at = fixture.lose_at = 240;
        ArmMetadataObserver subject; subject.enabled = subject.source_shape = true;
        subject.Observe(&fixture.current, 0, false, ArmMetadataFixture::Read);
        fixture.fail_at = 83;
        subject.Observe(&fixture.current, 1, true, ArmMetadataFixture::Read);
    }
    std::fflush(stdout);
    const bool restored = saved >= 0 && dup2(saved, STDOUT_FILENO) >= 0;
    if (saved >= 0) close(saved);
    char text[32768] = {}; size_t bytes = 0;
    if (record) { std::rewind(record); bytes = std::fread(text, 1, sizeof(text) - 1, record); std::fclose(record); }
    check(redirected && restored && bytes && std::strstr(text, "ARM source-shape raw:") &&
        std::strstr(text, "bytes=640/1920") && std::strstr(text, "measured=3/80") &&
        std::strstr(text, "cached-picture+14=ffffffff") && std::strstr(text, "cached-picture+54=ffffffff") &&
        std::strstr(text, "cached-picture+08=NOT-READ") && std::strstr(text, "e8-opaque=ffffffff") &&
        std::strstr(text, "mode-byte=255 format-byte=255 field-byte=255 table-selector-byte=255") &&
        std::strstr(text, "cached-source-is-current-input=unproven") &&
        std::strstr(text, "lease/generation/cache-ready/all-consumers-certified=no"),
        "ARM source-shape reports raw/cache fields separately, marks incomplete spans and never certifies source identity or lease");
}

template<class Check> static void PpbContextSelfTest(const Check &check)
{
    PpbContextObserver disabled;
    check(disabled.Observe(nullptr, 9, true, nullptr, false) && disabled.Finish(true, false) && !disabled.reads,
        "PPB saved context default disabled path adds no I/O or owner constraints");
    const uint32_t sizes[] = {0xa84c,0x378,0x1f4,0x64};
    for (unsigned kind = 0; kind < 4; ++kind) {
        check(PpbContextObserver::Object(0xd53dc, kind) && PpbContextObserver::Object(0x116000 - sizes[kind], kind),
            "PPB complete declared object allows exact lower/last-exclusive heap boundaries");
        for (uint32_t pointer : {0U,0xd53d8U,0xd53ddU,0x116004U - sizes[kind],0xfffffffcU,0xa40000U})
            check(!PpbContextObserver::Object(pointer, kind), "PPB object guards refuse null/misaligned/crossed/wrapped/source-plane candidates");
    }
    check(!PpbContextObserver::Object(0xd6000,4) && !PpbContextObserver::Disjoint(nullptr,4), "PPB malformed object kinds/null storage refuse");
    for (unsigned variant = 0; variant < 3; ++variant) {
        PpbContextFixture fixture; fixture.different = variant == 1; fixture.all_ones = variant == 2;
        PpbContextObserver subject; subject.enabled = true;
        for (unsigned stage = 0; stage < 3; ++stage) {
            check(subject.Observe(&fixture.current,stage,stage != 0,PpbContextFixture::Read,false) && fixture.valid &&
                fixture.calls == (stage + 1) * 140 && subject.reads == fixture.calls &&
                subject.bytes == (stage + 1) * 1104 && subject.measured == 140 && subject.complete[0] && subject.complete[1],
                "PPB rooted default graph/exact fixed saved-core spans/frozen rechecks consume140calls1104B per ordered stage");
            check((!std::memcmp(subject.raw[0],subject.raw[1],sizeof(subject.raw[0]))) == !fixture.different,
                "PPB saved core permits stale/all-ones/different data without promoting it to an active allocation certificate");
        }
        check(subject.Finish(true,false) && !subject.Finish(false,false) && fixture.calls == 420,
            "PPB completion requires all420calls3312bytes and genuine native success without extra reads");
    }
    for (uint32_t physical : {0x1000000U,0x1000001U,0x1000002U,0x1000003U,0x200000U,0x11c06c0U}) {
        PpbContextFixture fixture; fixture.physical = physical;
        PpbContextObserver subject; subject.enabled = true; bool result = true;
        for (unsigned stage = 0; stage < 3 && result; ++stage)
            result = subject.Observe(&fixture.current,stage,stage != 0,PpbContextFixture::Read,false);
        check(result && fixture.valid && subject.Finish(true,false) && fixture.calls == 420 &&
            subject.authority.words[14] == physical,
            "PPB all four upward-alignment residues and complete submitted slice at exact video boundaries keep only normalized fixed targets");
    }
    for (unsigned position = 0; position < 420; ++position) for (unsigned fault = 0; fault < 3; ++fault) {
        PpbContextFixture fixture,other;
        fixture.fail_at = fault ? 420 : position; fixture.lose_at = fault ? position : 420;
        fixture.replacement = fault == 2 ? other.current : nullptr;
        PpbContextObserver subject; subject.enabled = true;
        bool result = true;
        for (unsigned stage = 0; stage < 3 && result; ++stage)
            result = subject.Observe(&fixture.current,stage,stage != 0,PpbContextFixture::Read,false);
        check(!result && fixture.valid && subject.failed && fixture.calls == position + 1 &&
            subject.failure == (fault ? PpbContextFailure::Owner : PpbContextFailure::Read) &&
            !subject.Observe(&fixture.current,0,false,PpbContextFixture::Read,false) &&
            !subject.Observe(&other.current,2,true,PpbContextFixture::Read,false) && !subject.Finish(true,false) &&
            fixture.calls == position + 1 && !other.calls,
            "PPB every busy/error/owner-loss/replacement position is sticky and cannot read a foreign owner");
        uint32_t ignored = 0;
        check(!subject.Read(&other.current,PpbContextFixture::Read,0xd3a08,1,&ignored) && !other.calls,
            "PPB direct internal read helper also refuses after sticky failure");
    }
    const uint32_t links[] = {0xd3a08,0xd3a20,0xd6008,0xd600c,0xd6064,0xd60cc,0xd6224,
        0xd5408,0xd55a0,0xd55d4,0xd55d8,0xd55dc,0xd5800,0xd5808,0xd5810,0xd5814,
        0xd5a18,0xd5a1c,0xd5a28,0xd5a30,0xd5a34,0xd5a40,0x11601c,0x116020,0x11602c,0x116034,0x116038};
    for (uint32_t address : links) for (unsigned change : {39U,70U,140U,280U}) {
        PpbContextFixture fixture; fixture.changed_address = address; fixture.change_at = change;
        fixture.changed_value = fixture.Word(address,0) ^ 4U;
        PpbContextObserver subject; subject.enabled = true; bool result = true;
        for (unsigned stage = 0; stage < 3 && result; ++stage)
            result = subject.Observe(&fixture.current,stage,stage != 0,PpbContextFixture::Read,false);
        check(!result && fixture.valid && subject.failed && subject.failure == PpbContextFailure::Changed &&
            fixture.calls < 420 && !subject.Finish(true,false),
            "PPB every authority field change during frozen recheck/later pass/stage refuses without retargeting");
    }
    const uint32_t invalid[][2] = {
        {0xd3a08,0},{0xd3a20,0xa40000},{0xd3ac4,0},{0xd3ad0,0x10200},{0xd3c90,1},
        {0xd3a20,0xd5400},{0xd6000,1},{0xd6008,0},{0xd6008,0xfffffffc},{0xd6008,0x1ffffc},
        {0xd6008,0x11fff00},{0xd600c,0x3f93f},{0xd6064,0xd5800},{0xd60cc,1},{0xd6224,0xa40000},
        {0xd5408,0xd5800},{0xd55a0,0xd6000},{0xd55d4,0x200004},{0xd55d8,0x300000},
        {0xd55dc,0},{0xd55dc,0xfffffff0},{0xd55dc,0x1000001},{0xd5800,0xd6000},
        {0xd5808,0xd5800},{0xd5810,0x1000004},{0xd5814,0x300000},{0xd5a18,0x200004},
        {0xd5a1c,0x1200004},{0xd5a28,0x300000},{0xd5a30,0x300000},{0xd5a34,0x1000004},
        {0xd5a40,0},{0x11601c,0x116004},{0x116020,0x4000000},{0x11602c,0x116068},
        {0x116034,0x200000},{0x116038,0x3ee6000}};
    for (const auto &changed : invalid) {
        PpbContextFixture fixture; fixture.change_at = 0; fixture.changed_address = changed[0]; fixture.changed_value = changed[1];
        PpbContextObserver subject; subject.enabled = true;
        check(!subject.Observe(&fixture.current,0,false,PpbContextFixture::Read,false) && fixture.valid && subject.failed &&
            fixture.calls <= 31 && !subject.complete[0] && !subject.Finish(true,false),
            "PPB invalid rooted profiles/object aliases/map geometry/full context slices refuse before all saved-core reads");
    }
    for (unsigned invalid_stage = 0; invalid_stage < 7; ++invalid_stage) {
        PpbContextFixture fixture; PpbContextObserver subject; subject.enabled = true;
        if (invalid_stage >= 4) check(subject.Observe(&fixture.current,0,false,PpbContextFixture::Read,false),"PPB stage refusal setup");
        if (invalid_stage == 2) subject.reads = 281;
        if (invalid_stage == 3) subject.bytes = 2209;
        check(!subject.Observe(&fixture.current,invalid_stage == 0 ? 3 : invalid_stage == 4 ? 0 : invalid_stage >= 5 ? 1 : 0,
            invalid_stage == 1 || invalid_stage == 6,invalid_stage == 6 ? nullptr : PpbContextFixture::Read,false) &&
            subject.failed && fixture.calls == (invalid_stage >= 4 ? 140U : 0U),
            "PPB stage order/barrier/budget/null reader rejects before reads");
    }
    const std::vector<const char *> valid = {"probe","--hardware","fixture","180","30","1","--scaler-test","0",
        "--observe-ppb-context","--capture-yuy2","new"};
    Options admitted;
    check(ParseArguments(valid,&admitted) && admitted.observe_ppb_context && !admitted.observe_arm_metadata &&
        !admitted.observe_arm_source_shape && NeedsRawIo(admitted),"PPB observer is separately opted in with CAP_SYS_RAWIO admission");
    const std::vector<std::vector<const char *>> forbidden = {{"--observe-ppb-context"},{"--observe-arm-metadata"},{"--observe-arm-source-shape"},
        {"--observe-runtime-inventory"},{"--observe-mfd-framing"},{"--observe-mfd-config"},{"--observe-mfd-address"},
        {"--observe-scl-config"},{"--observe-scl-filter-map"},{"--observe-scl-view","2"},{"--observe-chroma"},
        {"--inject-mfd-colour","a"},{"--scl-status-test","observe"},{"--open-only"},{"--mpeg1-via-mpeg2"},{"--h263-via-divx"}};
    for (const auto &extra : forbidden) for (unsigned order = 0; order < 2; ++order) {
        auto arguments = valid; arguments.insert(order ? arguments.end()-2 : arguments.begin()+8,extra.begin(),extra.end()); Options rejected;
        check(!ParseArguments(arguments,&rejected),"PPB observer refuses all mixed or duplicate experiments in either order");
    }
    for (unsigned fault = 0; fault < 9; ++fault) {
        auto arguments = valid;
        if (fault == 0) arguments[1] = "--preflight";
        if (fault == 1) arguments[3] = "179";
        if (fault == 2) arguments[5] = "2";
        if (fault == 3) arguments[7] = "128";
        if (fault == 4) arguments[9] = "--capture-uyvy";
        if (fault == 5) arguments[10] = "-";
        if (fault == 6) arguments.resize(9);
        if (fault == 7) arguments[8] = "--observe-ppb-context=0";
        if (fault == 8) arguments.erase(arguments.begin()+6,arguments.begin()+8);
        Options rejected;
        check(!ParseArguments(arguments,&rejected),"PPB observer refuses preflight/framecount/repeat/scaling/packing/capture and ambiguous option syntax");
    }
    Input native; native.codec = AV_CODEC_ID_H264; native.subtype = BC_MSUBTYPE_H264; native.progressive = true;
    native.width = 256; native.height = 96; native.packets.resize(180);
    check(ArmMetadataInputShape(admitted,native) && !ArmMetadataInputAdmitted(admitted,native) &&
        ArmMetadataInputAdmitted(Options{},native),"PPB exact shape still requires original submitted-byte SHA before capture/device");
    FILE *record = std::tmpfile(); const int saved = dup(STDOUT_FILENO);
    std::fflush(stdout);
    const bool redirected = record && saved >= 0 && dup2(fileno(record),STDOUT_FILENO) >= 0;
    if (redirected) {
        PpbContextFixture fixture; fixture.all_ones = true;
        PpbContextObserver subject; subject.enabled = true;
        subject.Observe(&fixture.current,0,false,PpbContextFixture::Read);
        fixture.fail_at = 141;
        subject.Observe(&fixture.current,1,true,PpbContextFixture::Read);
    }
    std::fflush(stdout);
    const bool restored = saved >= 0 && dup2(saved,STDOUT_FILENO) >= 0;
    if (saved >= 0) close(saved);
    char text[16384] = {}; size_t length = 0;
    if (record) { std::rewind(record); length = std::fread(text,1,sizeof(text)-1,record); std::fclose(record); }
    check(redirected && restored && length && std::strstr(text,"reads=140/420 bytes=1104/3312 measured=140/140") &&
        std::strstr(text,"bank-count=ffffffff") && std::strstr(text,"P=01000000 N=0003f940 D=01000000") &&
        std::strstr(text,"saved-copy-is-current=unproven") && std::strstr(text,"allocator-integrity/lease/generation") &&
        std::strstr(text,"saved-fields=observed-stable,non-atomic") && std::strstr(text,"pass=0 INCOMPLETE"),
        "PPB reports frozen graph/raw saved fields only after complete passes and marks failed snapshots incomplete without lifetime certification");
}

template<class Check> static void PpbStopContextSelfTest(const Check &check)
{
    PpbContextObserver disabled; disabled.post_stop = true;
    check(disabled.Observe(nullptr, 3, true, nullptr, false) && disabled.Finish(true, false) && !disabled.reads,
        "PPB post-STOP configuration alone is disabled and adds no device I/O");
    for (unsigned variant = 0; variant < 3; ++variant) {
        PpbContextFixture fixture; fixture.post_stop = true;
        fixture.fail_at = fixture.lose_at = fixture.change_at = 560;
        fixture.different = variant == 1; fixture.all_ones = variant == 2;
        PpbContextObserver subject; subject.enabled = subject.post_stop = true;
        check(subject.Stages() == 4 && subject.ReadLimit() == 560 && subject.ByteLimit() == 4416,
            "PPB explicit post-STOP profile adds exactly one140read1104byte stage");
        for (unsigned stage = 0; stage < 4; ++stage) {
            check(subject.Observe(&fixture.current, stage, stage != 0, PpbContextFixture::Read, false) && fixture.valid &&
                fixture.calls == (stage + 1) * 140 && subject.bytes == (stage + 1) * 1104 &&
                subject.complete[0] && subject.complete[1] && subject.measured == 140,
                "PPB fourth stage keeps frozen graph/D only and admits the stopped qualifier without certifying refresh");
            check(subject.Finish(true, false) == (stage == 3), "PPB post-STOP finish requires all four stages, not EOS alone");
        }
        check(!subject.Finish(false, false) && fixture.calls == 560,
            "PPB complete saved observation cannot override native decode/cleanup failure");
    }
    for (unsigned position = 0; position < 560; ++position) for (unsigned fault = 0; fault < 3; ++fault) {
        PpbContextFixture fixture, other; fixture.post_stop = true;
        fixture.change_at = 560; fixture.fail_at = fault ? 560 : position; fixture.lose_at = fault ? position : 560;
        fixture.replacement = fault == 2 ? other.current : nullptr;
        PpbContextObserver subject; subject.enabled = subject.post_stop = true;
        bool result = true;
        for (unsigned stage = 0; stage < 4 && result; ++stage)
            result = subject.Observe(&fixture.current, stage, stage != 0, PpbContextFixture::Read, false);
        uint32_t ignored = 0;
        check(!result && fixture.valid && subject.failed && fixture.calls == position + 1 &&
            subject.failure == (fault ? PpbContextFailure::Owner : PpbContextFailure::Read) &&
            !subject.Observe(&fixture.current, 3, true, PpbContextFixture::Read, false) &&
            !subject.Read(&other.current, PpbContextFixture::Read, 0xd3a08, 1, &ignored) &&
            fixture.calls == position + 1 && !other.calls && !subject.Finish(true, false),
            "PPB all560 read/owner failures including post-STOP latch and never retry or retarget");
    }
    const uint32_t links[] = {0xd3a08,0xd3a20,0xd6000,0xd6008,0xd600c,0xd6064,0xd60cc,0xd6224,
        0xd5408,0xd55a0,0xd55d4,0xd55d8,0xd55dc,0xd5800,0xd5808,0xd5810,0xd5814,
        0xd5a18,0xd5a1c,0xd5a28,0xd5a30,0xd5a34,0xd5a40,0x11601c,0x116020,0x11602c,0x116034,0x116038};
    for (uint32_t address : links) for (unsigned position : {420U, 459U, 490U, 529U}) {
        PpbContextFixture fixture; fixture.post_stop = true;
        fixture.fail_at = fixture.lose_at = 560; fixture.change_at = position; fixture.changed_address = address;
        fixture.changed_value = 0xa40000;
        PpbContextObserver subject; subject.enabled = subject.post_stop = true; bool result = true;
        for (unsigned stage = 0; stage < 4 && result; ++stage)
            result = subject.Observe(&fixture.current, stage, stage != 0, PpbContextFixture::Read, false);
        check(!result && fixture.valid && subject.failed && subject.failure == PpbContextFailure::Changed &&
            fixture.calls <= position + 31 && !subject.Finish(true, false),
            "PPB every authority edge rejects post-STOP loss before following any changed pointer");
    }
    for (unsigned fault = 0; fault < 8; ++fault) {
        PpbContextFixture fixture; fixture.post_stop = true; fixture.fail_at = fixture.lose_at = fixture.change_at = 560;
        PpbContextObserver subject; subject.enabled = subject.post_stop = true;
        for (unsigned stage = 0; stage < 3; ++stage)
            check(subject.Observe(&fixture.current, stage, stage != 0, PpbContextFixture::Read, false), "PPB post-STOP refusal setup");
        if (fault == 0 || fault == 1) {
            fixture.change_at = 420; fixture.changed_address = fault ? 0xd3ad0 : 0xd3ac4;
            fixture.changed_value = fault ? 0x10200 : 0;
        }
        if (fault == 4) subject.reads = 421;
        if (fault == 5) subject.bytes = 3313;
        const PpbContextFailure reasons[] = {PpbContextFailure::Profile,PpbContextFailure::Profile,
            PpbContextFailure::Order,PpbContextFailure::Order,PpbContextFailure::Budget,PpbContextFailure::Budget,
            PpbContextFailure::Order,PpbContextFailure::Argument};
        check(!subject.Observe(&fixture.current, fault == 2 ? 2 : fault == 3 ? 4 : 3, fault != 6,
            fault == 7 ? nullptr : PpbContextFixture::Read, false) && subject.failed && subject.failure == reasons[fault] &&
            fixture.calls == (fault < 2 ? fault ? 433U : 424U : 420U),
            "PPB stopped-stage active/started qualifiers, order, budget, barrier and reader are enforced");
    }
    std::vector<const char *> valid = {"probe","--hardware","fixture","180","30","1","--scaler-test","0",
        "--observe-ppb-stop","--capture-yuy2","new"};
    Options admitted;
    check(ParseArguments(valid, &admitted) && admitted.observe_ppb_stop && admitted.observe_ppb_context &&
        !admitted.observe_arm_metadata && !admitted.observe_arm_source_shape && NeedsRawIo(admitted),
        "PPB host-STOP observation requires its own explicit CAP-gated profile");
    for (const auto &extra : std::vector<std::vector<const char *>>{{"--observe-ppb-stop"},{"--observe-ppb-context"},
            {"--observe-arm-metadata"},{"--observe-arm-source-shape"},{"--observe-runtime-inventory"},{"--observe-mfd-framing"},
            {"--observe-mfd-config"},{"--observe-mfd-address"},{"--observe-scl-config"},{"--observe-scl-filter-map"},
            {"--observe-scl-view","2"},{"--observe-chroma"},{"--inject-mfd-colour","a"},{"--scl-status-test","observe"},
            {"--open-only"},{"--mpeg1-via-mpeg2"},{"--h263-via-divx"}})
        for (unsigned order = 0; order < 2; ++order) {
            auto arguments = valid; arguments.insert(order ? arguments.end() - 2 : arguments.begin() + 8, extra.begin(), extra.end());
            Options rejected; check(!ParseArguments(arguments, &rejected), "PPB post-STOP refuses duplicate/mixed experiments in either order");
        }
    for (unsigned fault = 0; fault < 8; ++fault) {
        auto arguments = valid;
        if (fault == 0) arguments[1] = "--preflight";
        if (fault == 1) arguments[3] = "179";
        if (fault == 2) arguments[5] = "2";
        if (fault == 3) arguments[7] = "128";
        if (fault == 4) arguments[9] = "--capture-uyvy";
        if (fault == 5) arguments[10] = "-";
        if (fault == 6) arguments.resize(9);
        if (fault == 7) arguments[8] = "--observe-ppb-stop=0";
        Options rejected; check(!ParseArguments(arguments, &rejected), "PPB post-STOP keeps exact native-input/capture preconditions");
    }
    Input native; native.codec = AV_CODEC_ID_H264; native.subtype = BC_MSUBTYPE_H264; native.progressive = true;
    native.width = 256; native.height = 96; native.packets.resize(180);
    check(ArmMetadataInputShape(admitted, native) && !ArmMetadataInputAdmitted(admitted, native),
        "PPB post-STOP cannot open capture/device with shape-only input missing its original submitted-byte digest");
    FILE *record = std::tmpfile(); const int saved = dup(STDOUT_FILENO);
    std::fflush(stdout);
    const bool redirected = record && saved >= 0 && dup2(fileno(record), STDOUT_FILENO) >= 0;
    if (redirected) {
        PpbContextFixture fixture; fixture.post_stop = fixture.all_ones = true;
        fixture.fail_at = fixture.lose_at = fixture.change_at = 560;
        PpbContextObserver subject; subject.enabled = subject.post_stop = true;
        for (unsigned stage = 0; stage < 3; ++stage)
            subject.Observe(&fixture.current, stage, stage != 0, PpbContextFixture::Read, false);
        subject.Observe(&fixture.current, 3, true, PpbContextFixture::Read);
        subject.Finish(true);
    }
    std::fflush(stdout);
    const bool restored = saved >= 0 && dup2(saved, STDOUT_FILENO) >= 0;
    if (saved >= 0) close(saved);
    char text[16384] = {}; size_t length = 0;
    if (record) { std::rewind(record); length = std::fread(text, 1, sizeof(text) - 1, record); std::fclose(record); }
    check(redirected && restored && length && std::strstr(text, "stage=host-STOP-returned-before-CLOSE reads=560/560 bytes=4416/4416") &&
        std::strstr(text, "stages=4/4 reads=560/560 bytes=4416/4416") && std::strstr(text, "bank-word490=ffffffff") &&
        std::strstr(text, "saved-core-word0=ffffffff") && std::strstr(text, "bank-count-u8=255 adjacent-disposition-u8=255") &&
        std::strstr(text, "saved-copy-is-current=unproven") && std::strstr(text, "atomic/allocator-integrity/lease/generation/cache-ready/all-consumers-certified=no"),
        "PPB stopped-stage report pins the560read4416byte profile while retaining saved-copy and lifetime limitations");
}

template<class Check> static void PpbFixedMetadataSelfTest(const Check &check)
{
    PpbContextObserver disabled; disabled.metadata_pool = disabled.post_stop = true;
    check(disabled.Observe(nullptr, 9, true, nullptr, false) && disabled.Finish(true, false) && !disabled.reads,
        "PPB fixed metadata configuration alone has no I/O");
    unsigned calls = 0, total = 0;
    for (unsigned span = 0; span < 36; ++span) {
        uint32_t offset = 0; unsigned count = 0;
        check(PpbContextObserver::PoolSpan(span, &offset, &count) &&
            offset == (span < 2 ? 0x15678U + span * 256U : 0x15878U + (span - 2) * 228U) &&
            count == (span < 2 ? 2U : 18U) && !(offset & 3U) && offset + count * 4U <= 0x176c0U,
            "PPB fixed spans are two index headers and34 scalar prefixes, never pool-selected targets");
        ++calls; total += count * 4;
    }
    uint32_t offset = 0; unsigned count = 0;
    check(calls == 36 && total == 2464 && !PpbContextObserver::PoolSpan(36, &offset, &count) &&
        !PpbContextObserver::PoolSpan(0, nullptr, &count) && !PpbContextObserver::PoolSpan(0, &offset, nullptr),
        "PPB span count and byte cap exclude whole records, source planes and malformed destinations");
    for (unsigned variant = 0; variant < 3; ++variant) {
        PpbContextFixture fixture; fixture.post_stop = fixture.metadata_pool = true;
        fixture.fail_at = fixture.lose_at = fixture.change_at = 784;
        fixture.different = variant == 1; fixture.all_ones = variant == 2;
        PpbContextObserver subject; subject.enabled = subject.post_stop = subject.metadata_pool = true;
        check(subject.Stages() == 4 && subject.ReadLimit() == 784 && subject.ByteLimit() == 22208,
            "PPB fixed metadata profile has exactly784reads22208bytes");
        for (unsigned stage = 0; stage < 4; ++stage) {
            check(subject.Observe(&fixture.current, stage, stage != 0, PpbContextFixture::Read, false) && fixture.valid &&
                fixture.calls == (stage + 1) * 196 && subject.bytes == (stage + 1) * 5552 && subject.measured == 196 &&
                subject.complete[0] && subject.complete[1] && PpbContextObserver::PoolWindow(subject.authority),
                "PPB fixed metadata uses graph before/after every pass and a whole pool envelope");
            check((!std::memcmp(subject.pool_raw[0], subject.pool_raw[1], sizeof(subject.pool_raw[0]))) == !fixture.different &&
                subject.Finish(true, false) == (stage == 3),
                "PPB uninitialized/all-ones/different pool values remain informative, not a lifetime gate");
        }
        check(!subject.Finish(false, false), "PPB complete metadata sampling cannot override failed native cleanup");
    }
    for (uint32_t physical : {0x1000000U,0x1000001U,0x1000002U,0x1000003U,0x200000U,0x11c06c0U}) {
        PpbContextFixture fixture; fixture.physical = physical; fixture.post_stop = fixture.metadata_pool = true;
        fixture.fail_at = fixture.lose_at = fixture.change_at = 784;
        PpbContextObserver subject; subject.enabled = subject.post_stop = subject.metadata_pool = true; bool result = true;
        for (unsigned stage = 0; stage < 4 && result; ++stage)
            result = subject.Observe(&fixture.current, stage, stage != 0, PpbContextFixture::Read, false);
        check(result && fixture.valid && subject.Finish(true, false),
            "PPB every normalization residue and exact video slice boundaries keep fixed metadata addresses");
    }
    PpbContextGraph envelope;
    envelope.words[14] = envelope.words[22] = 0x200000;
    envelope.words[15] = envelope.words[23] = 0x177cc;
    check(PpbContextObserver::PoolWindow(envelope), "PPB full metadata envelope fits exact minimum remaining slice");
    for (unsigned fault = 0; fault < 5; ++fault) {
        PpbContextGraph bad = envelope;
        if (fault == 0) --bad.words[15];
        if (fault == 1) --bad.words[23];
        if (fault == 2) ++bad.words[22];
        if (fault == 3) bad.words[14] = 0xfffffff0U;
        if (fault == 4) bad.words[23] = 0xffffffffU;
        check(!PpbContextObserver::PoolWindow(bad), "PPB complete pool range rejects crossed/wrapped or short declared windows");
    }
    for (unsigned position = 0; position < 784; ++position) for (unsigned fault = 0; fault < 3; ++fault) {
        PpbContextFixture fixture, other; fixture.post_stop = fixture.metadata_pool = true;
        fixture.change_at = 784; fixture.fail_at = fault ? 784 : position; fixture.lose_at = fault ? position : 784;
        fixture.replacement = fault == 2 ? other.current : nullptr;
        PpbContextObserver subject; subject.enabled = subject.post_stop = subject.metadata_pool = true; bool result = true;
        for (unsigned stage = 0; stage < 4 && result; ++stage)
            result = subject.Observe(&fixture.current, stage, stage != 0, PpbContextFixture::Read, false);
        check(!result && fixture.valid && subject.failed && fixture.calls == position + 1 &&
            subject.failure == (fault ? PpbContextFailure::Owner : PpbContextFailure::Read) &&
            !subject.Observe(&fixture.current, 3, true, PpbContextFixture::Read, false) &&
            !subject.Observe(&other.current, 0, false, PpbContextFixture::Read, false) &&
            fixture.calls == position + 1 && !other.calls && !subject.Finish(true, false),
            "PPB all784 read/owner-loss/replacement failures latch without retry or foreign-owner I/O");
    }
    const uint32_t links[] = {0xd3a08,0xd3a20,0xd6000,0xd6008,0xd600c,0xd6064,0xd60cc,0xd6224,
        0xd5408,0xd55a0,0xd55d4,0xd55d8,0xd55dc,0xd5800,0xd5808,0xd5810,0xd5814,
        0xd5a18,0xd5a1c,0xd5a28,0xd5a30,0xd5a34,0xd5a40,0x11601c,0x116020,0x11602c,0x116034,0x116038};
    for (uint32_t address : links) for (unsigned position : {67U,98U,165U,196U,392U,588U,655U,686U,753U}) {
        PpbContextFixture fixture; fixture.post_stop = fixture.metadata_pool = true;
        fixture.fail_at = fixture.lose_at = 784; fixture.change_at = position; fixture.changed_address = address;
        fixture.changed_value = 0xa40000;
        PpbContextObserver subject; subject.enabled = subject.post_stop = subject.metadata_pool = true; bool result = true;
        for (unsigned stage = 0; stage < 4 && result; ++stage)
            result = subject.Observe(&fixture.current, stage, stage != 0, PpbContextFixture::Read, false);
        check(!result && fixture.valid && subject.failed && subject.failure == PpbContextFailure::Changed &&
            fixture.calls <= position + 31 && !subject.Finish(true, false),
            "PPB every authority edge mutation across all passes/stages refuses without pool retargeting");
    }
    for (unsigned fault = 0; fault < 8; ++fault) {
        PpbContextFixture fixture; fixture.post_stop = fixture.metadata_pool = true;
        fixture.fail_at = fixture.lose_at = fixture.change_at = 784;
        PpbContextObserver subject; subject.enabled = subject.post_stop = subject.metadata_pool = true;
        check(subject.Observe(&fixture.current, 0, false, PpbContextFixture::Read, false), "PPB fixed metadata refusal setup");
        if (fault == 0) subject.metadata_pool = false;
        if (fault == 1) subject.post_stop = false;
        if (fault == 2) subject.reads = 589;
        if (fault == 3) subject.bytes = 16657;
        const PpbContextFailure reasons[] = {PpbContextFailure::Argument,PpbContextFailure::Argument,
            PpbContextFailure::Budget,PpbContextFailure::Budget,PpbContextFailure::Order,PpbContextFailure::Order,
            PpbContextFailure::Order,PpbContextFailure::Argument};
        check(!subject.Observe(&fixture.current, fault == 4 ? 0 : fault == 5 ? 4 : 1, fault != 6,
            fault == 7 ? nullptr : PpbContextFixture::Read, false) && subject.failed && subject.failure == reasons[fault] && fixture.calls == 196,
            "PPB frozen profile/order/barrier/budget/reader constraints fail before further I/O");
    }
    const std::vector<const char *> valid = {"probe","--hardware","fixture","180","30","1","--scaler-test","0",
        "--observe-ppb-metadata","--capture-yuy2","new"};
    Options admitted;
    check(ParseArguments(valid, &admitted) && admitted.observe_ppb_metadata && admitted.observe_ppb_stop &&
        admitted.observe_ppb_context && !admitted.observe_arm_metadata && NeedsRawIo(admitted),
        "PPB fixed metadata requires a separate exact-input CAP-gated native profile");
    for (const auto &extra : std::vector<std::vector<const char *>>{{"--observe-ppb-metadata"},{"--observe-ppb-stop"},
            {"--observe-ppb-context"},{"--observe-arm-metadata"},{"--observe-arm-source-shape"},{"--observe-runtime-inventory"},
            {"--observe-mfd-framing"},{"--observe-mfd-config"},{"--observe-mfd-address"},{"--observe-scl-config"},
            {"--observe-scl-filter-map"},{"--observe-scl-view","2"},{"--observe-chroma"},{"--inject-mfd-colour","a"},
            {"--scl-status-test","observe"},{"--open-only"},{"--mpeg1-via-mpeg2"},{"--h263-via-divx"}})
        for (unsigned order = 0; order < 2; ++order) {
            auto arguments = valid; arguments.insert(order ? arguments.end() - 2 : arguments.begin() + 8, extra.begin(), extra.end());
            Options rejected; check(!ParseArguments(arguments, &rejected), "PPB fixed metadata refuses duplicate/mixed experiments in either order");
        }
    for (unsigned fault = 0; fault < 9; ++fault) {
        auto arguments = valid;
        if (fault == 0) arguments[1] = "--preflight";
        if (fault == 1) arguments[3] = "179";
        if (fault == 2) arguments[5] = "2";
        if (fault == 3) arguments[7] = "128";
        if (fault == 4) arguments[9] = "--capture-uyvy";
        if (fault == 5) arguments[10] = "-";
        if (fault == 6) arguments.resize(9);
        if (fault == 7) arguments[8] = "--observe-ppb-metadata=0";
        if (fault == 8) arguments.erase(arguments.begin() + 6, arguments.begin() + 8);
        Options rejected; check(!ParseArguments(arguments, &rejected), "PPB fixed metadata preserves scaler/input/capture admission before device I/O");
    }
    FILE *record = std::tmpfile(); const int saved = dup(STDOUT_FILENO);
    std::fflush(stdout);
    const bool redirected = record && saved >= 0 && dup2(fileno(record), STDOUT_FILENO) >= 0;
    if (redirected) {
        PpbContextFixture fixture; fixture.post_stop = fixture.metadata_pool = fixture.all_ones = true;
        fixture.fail_at = fixture.lose_at = fixture.change_at = 784;
        PpbContextObserver subject; subject.enabled = subject.post_stop = subject.metadata_pool = true;
        for (unsigned stage = 0; stage < 4; ++stage)
            subject.Observe(&fixture.current, stage, stage != 0, PpbContextFixture::Read, stage == 3);
        subject.Finish(true);
    }
    std::fflush(stdout);
    const bool restored = saved >= 0 && dup2(saved, STDOUT_FILENO) >= 0;
    if (saved >= 0) close(saved);
    char text[32768] = {}; size_t length = 0;
    if (record) { std::rewind(record); length = std::fread(text, 1, sizeof(text) - 1, record); std::fclose(record); }
    check(redirected && restored && length && std::strstr(text, "reads=784/784 bytes=22208/22208 measured=196/196") &&
        std::strstr(text, "slot=33 fixed-address=010175dc words=ffffffff") &&
        std::strstr(text, "return-read=ffffffff return-write=ffffffff") &&
        std::strstr(text, "fields=observed-stable,non-atomic") && std::strstr(text, "slot-address-is-generation=no") &&
        std::strstr(text, "current-state/allocator/extent/lease/generation/ARC-completion-certified=no") &&
        std::strstr(text, "stages=4/4 reads=784/784 bytes=22208/22208"),
        "PPB report exposes fixed raw words/indexheaders without ownership/generation or ARC completion claims");
}

template<class Check> static void PpbReturnHeaderSelfTest(const Check &check)
{
    PpbContextObserver disabled; disabled.return_header = disabled.metadata_pool = disabled.post_stop = true;
    check(disabled.Observe(nullptr, 9, true, nullptr, false) && disabled.Finish(true, false) && !disabled.reads,
        "PPB return-header configuration alone performs no I/O");
    unsigned total = 0;
    const uint32_t headers[] = {0x15678,0x15778,0x1577c,0x1577c,0x15778};
    const unsigned counts[] = {2,2,1,1,2};
    for (unsigned span = 0; span < 39; ++span) {
        uint32_t offset = 0; unsigned count = 0;
        check(PpbContextObserver::PoolSpan(span, &offset, &count, true) &&
            offset == (span < 5 ? headers[span] : 0x15878U + (span - 5) * 228U) &&
            count == (span < 5 ? counts[span] : 18U) && offset + count * 4U <= 0x176c0U,
            "PPB return-header request order is8B/4B/4B/8B followed by34 fixed scalar prefixes");
        total += count * 4;
    }
    uint32_t offset = 0; unsigned count = 0;
    check(total == 2480 && !PpbContextObserver::PoolSpan(39, &offset, &count, true) &&
        !PpbContextObserver::PoolSpan(0, nullptr, &count, true) && !PpbContextObserver::PoolSpan(0, &offset, nullptr, true),
        "PPB return-header fixed spans are39 requests2480B excluding record-selected targets");
    for (unsigned variant = 0; variant < 5; ++variant) {
        PpbContextFixture fixture; fixture.post_stop = fixture.metadata_pool = fixture.return_header = true;
        fixture.fail_at = fixture.lose_at = fixture.change_at = 816;
        fixture.different = variant == 1; fixture.all_ones = variant == 2;
        if (variant == 3) { fixture.change_at = 0; fixture.changed_address = 0xd6250; fixture.changed_value = 0xffffffffU; }
        if (variant == 4) { fixture.change_at = 34; fixture.changed_address = 0x101577c; fixture.changed_value = 0; }
        PpbContextObserver subject; subject.enabled = subject.post_stop = subject.metadata_pool = subject.return_header = true;
        check(subject.ReadLimit() == 816 && subject.ByteLimit() == 22400,
            "PPB return-header profile has exactly816calls22400B including two rooted handle scalars");
        for (unsigned stage = 0; stage < 4; ++stage) {
            check(subject.Observe(&fixture.current, stage, stage != 0, PpbContextFixture::Read, false) && fixture.valid &&
                fixture.calls == (stage + 1) * 204 && subject.bytes == (stage + 1) * 5600 && subject.measured == 204 &&
                subject.complete[0] && subject.complete[1] && subject.Finish(true, false) == (stage == 3),
                "PPB rooted route pair precedes fixed spans with graph guards before and after each pass");
            if (variant == 1) check(std::memcmp(subject.route_raw[0], subject.route_raw[1], sizeof(subject.route_raw[0])) != 0 &&
                std::memcmp(subject.pool_raw[0], subject.pool_raw[1], sizeof(subject.pool_raw[0])) != 0,
                "PPB route and metadata differences are preserved without routing identity or coherence gating");
            if (variant == 2) check(subject.route_raw[0][0] == 0xffffffffU && subject.pool_raw[0][7] == 0xffffffffU,
                "PPB all-ones route and all six return header words remain informational");
            if (variant == 3) check(subject.route_raw[0][0] == 0xffffffffU && subject.route_raw[0][1] != 0xffffffffU,
                "PPB route operands neither select targets nor require equality");
            if (variant == 4) check(subject.pool_raw[0][4] == 0 && subject.pool_raw[0][5] == 0 && subject.pool_raw[0][7] == 0 &&
                (stage || subject.pool_raw[0][3] != 0),
                "PPB grouped and single return samples are separately retained without zero normalization");
        }
        check(!subject.Finish(false, false), "PPB return observations cannot override native failure");
    }
    for (uint32_t physical : {0x1000000U,0x1000001U,0x1000002U,0x1000003U,0x200000U,0x11c06c0U}) {
        PpbContextFixture fixture; fixture.physical = physical;
        fixture.post_stop = fixture.metadata_pool = fixture.return_header = true;
        fixture.fail_at = fixture.lose_at = fixture.change_at = 816;
        PpbContextObserver subject; subject.enabled = subject.post_stop = subject.metadata_pool = subject.return_header = true;
        bool result = true;
        for (unsigned stage = 0; stage < 4 && result; ++stage)
            result = subject.Observe(&fixture.current, stage, stage != 0, PpbContextFixture::Read, false);
        check(result && fixture.valid && subject.Finish(true, false),
            "PPB return schedule admits all physical alignment residues and exact declared envelope limits");
    }
    for (unsigned position = 0; position < 816; ++position) for (unsigned fault = 0; fault < 3; ++fault) {
        PpbContextFixture fixture, other; fixture.post_stop = fixture.metadata_pool = fixture.return_header = true;
        fixture.change_at = 816; fixture.fail_at = fault ? 816 : position; fixture.lose_at = fault ? position : 816;
        fixture.replacement = fault == 2 ? other.current : nullptr;
        PpbContextObserver subject; subject.enabled = subject.post_stop = subject.metadata_pool = subject.return_header = true;
        bool result = true;
        for (unsigned stage = 0; stage < 4 && result; ++stage)
            result = subject.Observe(&fixture.current, stage, stage != 0, PpbContextFixture::Read, false);
        check(!result && fixture.valid && subject.failed && fixture.calls == position + 1 &&
            subject.failure == (fault ? PpbContextFailure::Owner : PpbContextFailure::Read) &&
            !subject.Observe(&fixture.current, 3, true, PpbContextFixture::Read, false) &&
            !subject.Observe(&other.current, 0, false, PpbContextFixture::Read, false) &&
            fixture.calls == position + 1 && !other.calls && !subject.Finish(true, false),
            "PPB all816 read/owner-loss/replacement faults latch with no retry or foreign I/O");
    }
    const uint32_t links[] = {0xd3a08,0xd3a20,0xd6000,0xd6008,0xd600c,0xd6064,0xd60cc,0xd6224,
        0xd5408,0xd55a0,0xd55d4,0xd55d8,0xd55dc,0xd5800,0xd5808,0xd5810,0xd5814,
        0xd5a18,0xd5a1c,0xd5a28,0xd5a30,0xd5a34,0xd5a40,0x11601c,0x116020,0x11602c,0x116034,0x116038};
    for (uint32_t address : links) for (unsigned position : {71U,102U,173U,204U,408U,612U,683U,714U,785U}) {
        PpbContextFixture fixture; fixture.post_stop = fixture.metadata_pool = fixture.return_header = true;
        fixture.fail_at = fixture.lose_at = 816; fixture.change_at = position; fixture.changed_address = address; fixture.changed_value = 0xa40000;
        PpbContextObserver subject; subject.enabled = subject.post_stop = subject.metadata_pool = subject.return_header = true;
        bool result = true;
        for (unsigned stage = 0; stage < 4 && result; ++stage)
            result = subject.Observe(&fixture.current, stage, stage != 0, PpbContextFixture::Read, false);
        check(!result && fixture.valid && subject.failed && subject.failure == PpbContextFailure::Changed &&
            fixture.calls <= position + 31 && !subject.Finish(true, false),
            "PPB route/header observations cannot bypass any frozen authority-edge change");
    }
    for (unsigned fault = 0; fault < 9; ++fault) {
        PpbContextFixture fixture; fixture.post_stop = fixture.metadata_pool = fixture.return_header = true;
        fixture.fail_at = fixture.lose_at = fixture.change_at = 816;
        PpbContextObserver subject; subject.enabled = subject.post_stop = subject.metadata_pool = subject.return_header = true;
        check(subject.Observe(&fixture.current, 0, false, PpbContextFixture::Read, false), "PPB return-header refusal setup");
        if (fault == 0) subject.return_header = false;
        if (fault == 1) subject.metadata_pool = false;
        if (fault == 2) subject.post_stop = false;
        if (fault == 3) subject.reads = 613;
        if (fault == 4) subject.bytes = 16801;
        const PpbContextFailure reasons[] = {PpbContextFailure::Argument,PpbContextFailure::Argument,PpbContextFailure::Argument,
            PpbContextFailure::Budget,PpbContextFailure::Budget,PpbContextFailure::Order,PpbContextFailure::Order,
            PpbContextFailure::Order,PpbContextFailure::Argument};
        check(!subject.Observe(&fixture.current, fault == 5 ? 0 : fault == 6 ? 4 : 1, fault != 7,
            fault == 8 ? nullptr : PpbContextFixture::Read, false) && subject.failed && subject.failure == reasons[fault] && fixture.calls == 204,
            "PPB return profile cannot mutate schedule or bypass order/barrier/budget/reader checks");
    }
    const std::vector<const char *> valid = {"probe","--hardware","fixture","180","30","1","--scaler-test","0",
        "--observe-ppb-return","--capture-yuy2","new"};
    Options admitted;
    check(ParseArguments(valid, &admitted) && admitted.observe_ppb_return && admitted.observe_ppb_metadata && admitted.observe_ppb_stop &&
        admitted.observe_ppb_context && !admitted.observe_arm_metadata && NeedsRawIo(admitted),
        "PPB return flag implies metadata/STOP/rooted authority and CAP-gated exact-input native profile");
    for (const auto &extra : std::vector<std::vector<const char *>>{{"--observe-ppb-return"},{"--observe-ppb-metadata"},{"--observe-ppb-stop"},
            {"--observe-ppb-context"},{"--observe-arm-metadata"},{"--observe-arm-source-shape"},{"--observe-runtime-inventory"},
            {"--observe-mfd-framing"},{"--observe-mfd-config"},{"--observe-mfd-address"},{"--observe-scl-config"},
            {"--observe-scl-filter-map"},{"--observe-scl-view","2"},{"--observe-chroma"},{"--inject-mfd-colour","a"},
            {"--scl-status-test","observe"},{"--open-only"},{"--mpeg1-via-mpeg2"},{"--h263-via-divx"}})
        for (unsigned order = 0; order < 2; ++order) {
            auto arguments = valid; arguments.insert(order ? arguments.end() - 2 : arguments.begin() + 8, extra.begin(), extra.end());
            Options rejected; check(!ParseArguments(arguments, &rejected), "PPB return-header refuses duplicate/mixed experiments in either order");
        }
    for (unsigned fault = 0; fault < 9; ++fault) {
        auto arguments = valid;
        if (fault == 0) arguments[1] = "--preflight";
        if (fault == 1) arguments[3] = "179";
        if (fault == 2) arguments[5] = "2";
        if (fault == 3) arguments[7] = "128";
        if (fault == 4) arguments[9] = "--capture-uyvy";
        if (fault == 5) arguments[10] = "-";
        if (fault == 6) arguments.resize(9);
        if (fault == 7) arguments[8] = "--observe-ppb-return=0";
        if (fault == 8) arguments.erase(arguments.begin() + 6, arguments.begin() + 8);
        Options rejected; check(!ParseArguments(arguments, &rejected), "PPB return-header admission rejects altered input/scaler/capture before I/O");
    }
    FILE *record = std::tmpfile(); const int saved = dup(STDOUT_FILENO);
    std::fflush(stdout);
    const bool redirected = record && saved >= 0 && dup2(fileno(record), STDOUT_FILENO) >= 0;
    if (redirected) {
        PpbContextFixture fixture; fixture.post_stop = fixture.metadata_pool = fixture.return_header = fixture.all_ones = true;
        fixture.fail_at = fixture.lose_at = fixture.change_at = 816;
        PpbContextObserver subject; subject.enabled = subject.post_stop = subject.metadata_pool = subject.return_header = true;
        for (unsigned stage = 0; stage < 4; ++stage) subject.Observe(&fixture.current, stage, stage != 0, PpbContextFixture::Read, stage == 3);
        subject.Finish(true);
    }
    std::fflush(stdout);
    const bool restored = saved >= 0 && dup2(saved, STDOUT_FILENO) >= 0;
    if (saved >= 0) close(saved);
    char text[32768] = {}; size_t length = 0;
    if (record) { std::rewind(record); length = std::fread(text, 1, sizeof(text) - 1, record); std::fclose(record); }
    check(redirected && restored && length && std::strstr(text, "reads=816/816 bytes=22400/22400 measured=204/204") &&
        std::strstr(text, "route-acquire=ffffffff route-return=ffffffff return-write-single0=ffffffff return-write-single1=ffffffff") &&
        std::strstr(text, "return-read-after=ffffffff return-write-after=ffffffff") &&
        std::strstr(text, "slot=33 fixed-address=010175dc words=ffffffff") &&
        std::strstr(text, "atomic8B/routing-identity/zero-cause-certified=no") &&
        std::strstr(text, "PPB return header finish: native-result=PASS observation-result=PASS stages=4/4 reads=816/816 bytes=22400/22400"),
        "PPB return report retains every independent word and states request grouping is not atomicity or cause proof");
}

template<class Check> static void VideoPrefixSelfTest(const Check &check)
{
    PpbContextObserver disabled; disabled.video_prefix = true;
    PpbContextFixture untouched; untouched.video_prefix = true;
    check(disabled.Observe(&untouched.current, 99, false, PpbContextFixture::Read, false) &&
        disabled.Finish(true, false) && !untouched.calls, "video prefix opt-in configuration alone adds no I/O");
    for (unsigned values = 0; values < 3; ++values) {
        PpbContextFixture fixture; fixture.video_prefix = true;
        fixture.all_ones = values == 1; fixture.prefix_zero = values == 2;
        PpbContextObserver subject; subject.enabled = subject.video_prefix = true;
        check(subject.StageReads() == 126 && subject.StageBytes() == 880 && subject.Stages() == 3 &&
            subject.ReadLimit() == 378 && subject.ByteLimit() == 2640, "video prefix freezes six target reads in three stages");
        for (unsigned stage = 0; stage < 3; ++stage) {
            check(subject.Observe(&fixture.current, stage, stage != 0, PpbContextFixture::Read, false) && fixture.valid &&
                fixture.calls == (stage + 1) * 126 && subject.bytes == (stage + 1) * 880 &&
                subject.complete[0] && subject.complete[1], "video prefix graph brackets each fixed 128-byte request");
            if (values) for (unsigned pass = 0; pass < 2; ++pass)
                for (unsigned word = 0; word < 32; ++word)
                    check(subject.raw[pass][word] == (values == 1 ? 0xffffffffU : 0U),
                        "video prefix preserves zero and all-ones as raw words, not pixel certificates");
        }
        check(subject.Finish(true, false) && !subject.Finish(false, false), "video prefix cannot override native pixel/EOS/cleanup failure");
    }
    PpbContextGraph boundary;
    boundary.words[21] = boundary.words[22] = 0x200000;
    boundary.words[23] = 128 + 0x5bc;
    boundary.words[14] = 0x200080; boundary.words[15] = 0x5bc;
    check(PpbContextObserver::PrefixWindow(boundary), "video prefix allows exact span/context boundary without following a plane word");
    for (unsigned skip = 0; skip < 4; ++skip) {
        PpbContextGraph aligned = boundary;
        aligned.words[14] -= skip; aligned.words[15] += skip;
        check(PpbContextObserver::PrefixWindow(aligned), "video prefix safely normalizes the context's four alignment residues");
        --aligned.words[15];
        check(!PpbContextObserver::PrefixWindow(aligned), "video prefix requires the full normalized saved-core extent");
    }
    for (unsigned fault = 0; fault < 7; ++fault) {
        PpbContextGraph changed = boundary;
        if (fault == 0) changed.words[21] = changed.words[22] = 0;
        if (fault == 1) ++changed.words[22];
        if (fault == 2) changed.words[23] = 127;
        if (fault == 3) --changed.words[14];
        if (fault == 4) changed.words[23] = 0xffffffffU;
        if (fault == 5) changed.words[14] = 0xffffffffU;
        if (fault == 6) ++changed.words[21];
        check(!PpbContextObserver::PrefixWindow(changed), "video prefix rejects crossed, wrapped, unaligned and foreign envelope values");
    }
    for (unsigned kind = 0; kind < 2; ++kind) for (unsigned at = 0; at < 378; ++at) {
        PpbContextFixture fixture; fixture.video_prefix = true;
        if (kind) fixture.lose_at = at; else fixture.fail_at = at;
        PpbContextObserver subject; subject.enabled = subject.video_prefix = true;
        bool ok = true;
        for (unsigned stage = 0; stage < 3 && ok; ++stage)
            ok = subject.Observe(&fixture.current, stage, stage != 0, PpbContextFixture::Read, false);
        const unsigned calls = fixture.calls;
        check(!ok && subject.failed && calls == at + 1 && fixture.valid &&
            !subject.Finish(true, false) && !subject.Observe(&fixture.current, 0, false, PpbContextFixture::Read, false) &&
            fixture.calls == calls, "video prefix every read and owner loss is sticky with no subsequent I/O");
        check(!subject.complete[(at % 126) / 63], "video prefix never publishes a partially bracketed pass");
    }
    struct ModeFixture : PpbContextFixture {
        PpbContextObserver *subject = nullptr;
        unsigned at = 0, field = 0;
        static BC_STATUS Read(HANDLE handle, uint32_t *values, uint32_t bytes, uint32_t address) {
            auto *fixture = static_cast<ModeFixture *>(handle);
            const BC_STATUS status = PpbContextFixture::Read(handle, values, bytes, address);
            if (fixture->calls == fixture->at + 1) {
                if (fixture->field == 0) fixture->subject->video_prefix = false;
                if (fixture->field == 1) fixture->subject->post_stop = true;
                if (fixture->field == 2) fixture->subject->metadata_pool = true;
                if (fixture->field == 3) fixture->subject->return_header = true;
                if (fixture->field == 4) fixture->subject->video_graph = true;
            }
            return status;
        }
    };
    for (unsigned field = 0; field < 5; ++field) for (unsigned at : {0U,30U,31U,32U,62U,63U,94U,125U,126U,251U,377U}) {
        ModeFixture fixture; fixture.video_prefix = true; fixture.at = at; fixture.field = field;
        PpbContextObserver subject; subject.enabled = subject.video_prefix = true; fixture.subject = &subject;
        bool ok = true;
        for (unsigned stage = 0; stage < 3 && ok; ++stage)
            ok = subject.Observe(&fixture.current, stage, stage != 0, ModeFixture::Read, false);
        check(!ok && subject.failure == PpbContextFailure::Argument && fixture.calls == at + 1 &&
            !subject.complete[(at % 126) / 63] && subject.owner_video_prefix &&
            subject.StageReads() == 126 && subject.StageBytes() == 880 && subject.Stages() == 3,
            "video prefix mode cannot change inside a reader callback or relabel its frozen scope");
    }
    PpbContextFixture changed; changed.video_prefix = true; changed.change_at = 32;
    changed.changed_address = 0xd55d8; changed.changed_value = 0x200004;
    PpbContextObserver guarded; guarded.enabled = guarded.video_prefix = true;
    check(!guarded.Observe(&changed.current, 0, false, PpbContextFixture::Read, false) &&
        guarded.failure == PpbContextFailure::Changed && !guarded.complete[0] && !guarded.complete[1] &&
        guarded.changed_word == 22 && guarded.changed_expected == 0x200000 && guarded.changed_observed == 0x200004,
        "video prefix post-read graph mismatch discards collected data before publication");
    const uint32_t authority_addresses[] = {0xd3a08,0xd3a20,0xd3bec,0xd3db8,0xd3f84,
        0xd6000,0xd6008,0xd600c,0xd6064,0xd60cc,0xd6224,
        0xd5408,0xd55a0,0xd55d4,0xd55d8,0xd55dc,0xd5800,0xd5808,0xd5810,0xd5814,
        0xd5a18,0xd5a1c,0xd5a28,0xd5a30,0xd5a34,0xd5a40,0x11601c,0x116020,0x11602c,0x116034,0x116038};
    const unsigned authority_words[] = {0,1,4,7,10,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,
        28,29,30,31,32,33,34,35,36,37,38};
    for (unsigned edge = 0; edge < 31; ++edge) for (unsigned position : {32U,126U}) {
        PpbContextFixture fixture; fixture.video_prefix = true;
        fixture.change_at = position; fixture.changed_address = authority_addresses[edge];
        const uint32_t expected = fixture.Word(fixture.changed_address, 0);
        fixture.changed_value = expected ^ 4U;
        PpbContextObserver subject; subject.enabled = subject.video_prefix = true;
        if (position == 126)
            check(subject.Observe(&fixture.current, 0, false, PpbContextFixture::Read, false),
                "video prefix changed-authority diagnosis setup preserves the initial bracket");
        const unsigned stage = position == 126 ? 1U : 0U;
        check(!subject.Observe(&fixture.current, stage, stage != 0, PpbContextFixture::Read, false) &&
            fixture.valid && subject.failure == PpbContextFailure::Changed &&
            subject.changed_word == authority_words[edge] && subject.changed_expected == expected &&
            subject.changed_observed == fixture.changed_value && fixture.calls <= position + 31 &&
            !subject.complete[0] && !subject.complete[1],
            "video prefix records only an already-collected authority difference without another target read");
        const unsigned calls = fixture.calls;
        check(!subject.Observe(&fixture.current, stage, stage != 0, PpbContextFixture::Read, false) &&
            fixture.calls == calls && subject.changed_word == authority_words[edge] &&
            subject.changed_expected == expected && subject.changed_observed == fixture.changed_value,
            "video prefix failure and its diagnostic stay frozen without retries");
    }
    FILE *record = std::tmpfile(); const int saved = dup(STDOUT_FILENO);
    std::fflush(stdout);
    const bool redirected = record && saved >= 0 && dup2(fileno(record), STDOUT_FILENO) >= 0;
    if (redirected) guarded.Report(0);
    std::fflush(stdout);
    const bool restored = saved >= 0 && dup2(saved, STDOUT_FILENO) >= 0;
    if (saved >= 0) close(saved);
    char diagnostic[2048] = {}; size_t diagnostic_length = 0;
    if (record) { std::rewind(record); diagnostic_length = std::fread(diagnostic, 1, sizeof(diagnostic) - 1, record); std::fclose(record); }
    check(redirected && restored && diagnostic_length &&
        std::strstr(diagnostic, "authority delta: word=22 frozen=00200000 observed=00200004") &&
        std::strstr(diagnostic, "already-collected-first-difference only; no retry or changed-target read") &&
        std::strstr(diagnostic, "pass=0 INCOMPLETE") && std::strstr(diagnostic, "pass=1 INCOMPLETE") &&
        !std::strstr(diagnostic, " bytes=128 words="),
        "video prefix report identifies a rejected field without publishing an incomplete target or claiming its cause");
    const std::vector<const char *> valid = {"probe","--hardware","fixture","180","30","1","--scaler-test","0",
        "--observe-video-prefix","--capture-yuy2","new"};
    Options admitted;
    check(ParseArguments(valid, &admitted) && admitted.observe_video_prefix && admitted.observe_ppb_context &&
        !admitted.observe_ppb_stop && !admitted.observe_ppb_metadata && !admitted.observe_ppb_return &&
        !admitted.observe_arm_metadata && NeedsRawIo(admitted), "video prefix uses the pinned native input and raw-I/O authority gate");
    for (const char *extra : {"--observe-video-prefix", "--observe-ppb-context", "--observe-ppb-return", "--observe-chroma", "--observe-mfd-config"}) {
        for (unsigned order = 0; order < 2; ++order) {
            auto arguments = valid; arguments.insert(arguments.begin() + (order ? 9 : 8), extra);
            Options rejected;
            check(!ParseArguments(arguments, &rejected), "video prefix rejects duplicate/mixed experiments in both orders");
        }
    }
    for (unsigned fault = 0; fault < 7; ++fault) {
        auto arguments = valid;
        if (fault == 0) arguments[1] = "--preflight";
        if (fault == 1) arguments[3] = "179";
        if (fault == 2) arguments[5] = "2";
        if (fault == 3) arguments[7] = "128";
        if (fault == 4) arguments[8] = "--observe-video-prefix=0";
        if (fault == 5) arguments[9] = "--capture-uyvy";
        if (fault == 6) arguments.resize(9);
        Options rejected;
        check(!ParseArguments(arguments, &rejected), "video prefix rejects altered preflight/count/repeat/scaler/packing/capture syntax");
    }
}

template<class Check> static void VideoGraphSelfTest(const Check &check)
{
    PpbContextObserver disabled; disabled.video_graph = true;
    PpbContextFixture untouched; untouched.video_graph = true;
    check(disabled.Observe(nullptr, 99, true, nullptr, false) && disabled.Finish(true, false) &&
        !disabled.reads && !untouched.calls, "video graph default-off mode adds no I/O or owner constraints");
    const uint32_t addresses[] = {0xd3a08,
        0xd3a20,0xd3ac4,0xd3ad0,0xd3bec,0xd3c90,0xd3c9c,0xd3db8,0xd3e5c,0xd3e68,0xd3f84,0xd4028,0xd4034,
        0xd6000,0xd6008,0xd6064,0xd60cc,0xd6224,0xd5408,0xd55a0,0xd55d4,
        0xd5800,0xd5808,0xd5810,0xd5a18,0xd5a28,0xd5a30,0xd5a40,0x11601c,0x11602c,0x116034};
    const unsigned counts[] = {1,1,1,1,1,1,1,1,1,1,1,1,1,1,2,1,1,1,1,1,3,1,1,2,2,1,2,1,2,1,2};
    const uint32_t expected[] = {0xd5400,0xd6000,0x101,0x200,0,0,0,0,0,0,0,0,0,
        0,0x1000000,0x3f940,0xd5400,0,0x116004,0x116004,0xd5800,0x200000,0x200000,0x1000000,
        0xd5400,0xd5a00,0x1000000,0x200000,0x200000,0x1200000,0x200000,0x200000,0x1000000,1,
        0x116068,0x3ffc000,0x116004,0x116004,0x3ee5ffc};
    struct TraceFixture : PpbContextFixture {
        uint32_t addresses[186] = {}, lengths[186] = {};
        static BC_STATUS Read(HANDLE handle, uint32_t *values, uint32_t bytes, uint32_t address) {
            auto *fixture = static_cast<TraceFixture *>(handle);
            if (fixture->calls < 186) { fixture->addresses[fixture->calls] = address; fixture->lengths[fixture->calls] = bytes; }
            return PpbContextFixture::Read(handle, values, bytes, address);
        }
    };
    TraceFixture clean; clean.video_graph = true;
    PpbContextObserver stable; stable.enabled = stable.video_graph = true;
    check(stable.Stages() == 3 && stable.StageReads() == 62 && stable.StageBytes() == 312 &&
        stable.ReadLimit() == 186 && stable.ByteLimit() == 936 && !stable.Finish(true, false),
        "video graph has exactly two 31-read/156-byte passes per stage and no target spans");
    for (unsigned stage = 0; stage < 3; ++stage) {
        check(stable.Observe(&clean.current, stage, stage != 0, TraceFixture::Read, false) && clean.valid &&
            clean.calls == (stage + 1) * 62 && stable.reads == clean.calls && stable.bytes == (stage + 1) * 312 &&
            stable.measured == 62 && stable.complete[0] && stable.complete[1],
            "video graph publishes two complete rooted passes only after ordered native barriers");
        for (unsigned pass = 0; pass < 2; ++pass) for (unsigned word = 0; word < 39; ++word)
            check(stable.graph[pass].words[word] == (word == 3 && stage ? 0x10200U : expected[word]),
                "video graph retains all 39 graph words, including phase flags, in literal field order");
        for (unsigned pass = 0; pass < 2; ++pass) {
            for (uint32_t word : stable.raw[pass]) check(!word, "video graph never fills prefix or saved-context storage");
            for (uint32_t word : stable.pool_raw[pass]) check(!word, "video graph never fills pool or ring storage");
            for (uint32_t word : stable.route_raw[pass]) check(!word, "video graph never reads route operands");
        }
    }
    unsigned total = 0;
    for (unsigned at = 0; at < 186; ++at) {
        total += clean.lengths[at];
        check(clean.addresses[at] == addresses[at % 31] && clean.lengths[at] == counts[at % 31] * 4U,
            "video graph exact 186-callback trace contains only qualified rooted object and fixed heap words");
    }
    check(total == 936 && stable.Finish(true, false) && !stable.Finish(false, false) && clean.calls == 186,
        "video graph finish requires every pass and native output/EOS/cleanup success without extra I/O");
    for (unsigned fault = 0; fault < 3; ++fault) {
        PpbContextObserver altered = stable;
        if (fault == 0) altered.enabled = false;
        if (fault == 1) altered.video_graph = false;
        if (fault == 2) altered.video_prefix = true;
        check(!altered.Finish(true, false) && altered.ReadLimit() == 186 && altered.ByteLimit() == 936,
            "video graph finish cannot bypass its frozen mode or relabel its read budget");
    }
    struct StatusFixture : PpbContextFixture {
        BC_STATUS result = BC_STS_ERROR;
        static BC_STATUS Read(HANDLE handle, uint32_t *values, uint32_t bytes, uint32_t address) {
            auto *fixture = static_cast<StatusFixture *>(handle);
            const BC_STATUS status = PpbContextFixture::Read(handle, values, bytes, address);
            if (fixture->calls == fixture->fail_at + 1) {
                for (unsigned word = 0; word < bytes / 4; ++word) values[word] = 0xdeadbeefU;
                return fixture->result;
            }
            return status;
        }
    };
    for (int code = -1; code <= BC_STS_PWR_MGMT; ++code) {
        if (code == BC_STS_SUCCESS) continue;
        for (unsigned at = 0; at < 186; ++at) {
            StatusFixture fixture; fixture.video_graph = true; fixture.fail_at = at; fixture.result = static_cast<BC_STATUS>(code);
            PpbContextObserver subject; subject.enabled = subject.video_graph = true;
            bool ok = true;
            for (unsigned stage = 0; stage < 3 && ok; ++stage)
                ok = subject.Observe(&fixture.current, stage, stage != 0, StatusFixture::Read, false);
            const unsigned calls = fixture.calls;
            check(!ok && subject.failed && fixture.valid && calls == at + 1 && subject.measured == at % 62 &&
                subject.failure == PpbContextFailure::Read && subject.status == code && !subject.complete[(at % 62) / 31] &&
                !subject.Finish(true, false) && !subject.Observe(&fixture.current, 0, false, StatusFixture::Read, false) &&
                fixture.calls == calls, "video graph all 27 API failures at every callback are unpublished, exact and sticky");
        }
    }
    for (unsigned kind = 0; kind < 2; ++kind) for (unsigned at = 0; at < 186; ++at) {
        PpbContextFixture fixture, foreign; fixture.video_graph = true; fixture.lose_at = at;
        fixture.replacement = kind ? foreign.current : nullptr;
        PpbContextObserver subject; subject.enabled = subject.video_graph = true;
        bool ok = true;
        for (unsigned stage = 0; stage < 3 && ok; ++stage)
            ok = subject.Observe(&fixture.current, stage, stage != 0, PpbContextFixture::Read, false);
        const unsigned calls = fixture.calls;
        check(!ok && subject.failure == PpbContextFailure::Owner && calls == at + 1 && fixture.valid &&
            !subject.complete[(at % 62) / 31] && !subject.Finish(true, false) &&
            !subject.Observe(&foreign.current, 0, false, PpbContextFixture::Read, false) && fixture.calls == calls && !foreign.calls,
            "video graph every current-handle null/replacement stops without foreign reads or publication");
    }
    struct ModeFixture : PpbContextFixture {
        PpbContextObserver *subject = nullptr;
        unsigned at = 0, field = 0;
        static BC_STATUS Read(HANDLE handle, uint32_t *values, uint32_t bytes, uint32_t address) {
            auto *fixture = static_cast<ModeFixture *>(handle);
            const BC_STATUS status = PpbContextFixture::Read(handle, values, bytes, address);
            if (fixture->calls == fixture->at + 1) {
                if (fixture->field == 0) fixture->subject->video_graph = false;
                if (fixture->field == 1) fixture->subject->video_prefix = true;
                if (fixture->field == 2) fixture->subject->post_stop = true;
                if (fixture->field == 3) fixture->subject->metadata_pool = true;
                if (fixture->field == 4) fixture->subject->return_header = true;
                if (fixture->field == 5) fixture->subject->enabled = false;
            }
            return status;
        }
    };
    for (unsigned field = 0; field < 6; ++field) for (unsigned at = 0; at < 186; ++at) {
        ModeFixture fixture; fixture.video_graph = true; fixture.at = at; fixture.field = field;
        PpbContextObserver subject; subject.enabled = subject.video_graph = true; fixture.subject = &subject;
        bool ok = true;
        for (unsigned stage = 0; stage < 3 && ok; ++stage)
            ok = subject.Observe(&fixture.current, stage, stage != 0, ModeFixture::Read, false);
        const unsigned calls = fixture.calls;
        check(!ok && subject.failure == PpbContextFailure::Argument && calls == at + 1 && subject.owner_video_graph &&
            !subject.complete[(at % 62) / 31] && subject.StageReads() == 62 && subject.StageBytes() == 312 &&
            subject.Stages() == 3 && !subject.Finish(true, false) &&
            !subject.Observe(&fixture.current, 0, false, ModeFixture::Read, false) && fixture.calls == calls,
            "video graph callbacks cannot disable observation, mix targets or change its frozen scope/budget");
    }
    const uint32_t authority_addresses[] = {0xd3a08,0xd3a20,0xd3bec,0xd3db8,0xd3f84,
        0xd6000,0xd6008,0xd600c,0xd6064,0xd60cc,0xd6224,
        0xd5408,0xd55a0,0xd55d4,0xd55d8,0xd55dc,0xd5800,0xd5808,0xd5810,0xd5814,
        0xd5a18,0xd5a1c,0xd5a28,0xd5a30,0xd5a34,0xd5a40,0x11601c,0x116020,0x11602c,0x116034,0x116038};
    const unsigned authority_words[] = {0,1,4,7,10,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,
        28,29,30,31,32,33,34,35,36,37,38};
    PpbContextObserver rejected;
    for (unsigned edge = 0; edge < 31; ++edge) for (unsigned position : {31U,62U}) {
        PpbContextFixture fixture; fixture.video_graph = true; fixture.change_at = position;
        fixture.changed_address = authority_addresses[edge];
        const uint32_t original = fixture.Word(fixture.changed_address, 0);
        fixture.changed_value = original ^ 4U;
        PpbContextObserver subject; subject.enabled = subject.video_graph = true;
        if (position == 62) check(subject.Observe(&fixture.current, 0, false, PpbContextFixture::Read, false),
            "video graph authority diagnosis begins from complete frozen initial passes");
        const unsigned stage = position == 62 ? 1U : 0U;
        check(!subject.Observe(&fixture.current, stage, stage != 0, PpbContextFixture::Read, false) && fixture.valid &&
            subject.failure == PpbContextFailure::Changed && subject.changed_word == authority_words[edge] &&
            subject.changed_expected == original && subject.changed_observed == fixture.changed_value &&
            fixture.calls <= position + 31 && !subject.complete[1] && subject.complete[0] == (position == 31),
            "video graph each frozen authority edge refuses in next pass or next stage using only collected words");
        const unsigned calls = fixture.calls;
        check(!subject.Finish(true, false) && !subject.Observe(&fixture.current, stage, stage != 0, PpbContextFixture::Read, false) &&
            fixture.calls == calls && subject.changed_word == authority_words[edge],
            "video graph rejected authority is never retried or used as a replacement target");
        if (edge == 14 && position == 62) rejected = subject;
    }
    for (unsigned fault = 0; fault < 11; ++fault) {
        PpbContextFixture fixture; fixture.video_graph = true;
        PpbContextObserver subject; subject.enabled = subject.video_graph = true;
        if (fault >= 6) check(subject.Observe(&fixture.current, 0, false, PpbContextFixture::Read, false), "video graph ordering setup");
        if (fault == 2) subject.reads = 125;
        if (fault == 3) subject.bytes = 625;
        if (fault == 4) subject.video_prefix = true;
        if (fault == 5) subject.metadata_pool = subject.post_stop = true;
        if (fault == 10) subject.enabled = false;
        const unsigned stage = fault == 0 ? 3U : fault == 6 ? 0U : fault >= 7 ? 1U : 0U;
        const bool barrier = fault == 1 || fault == 8 || fault == 9;
        check(!subject.Observe(fault == 9 ? nullptr : &fixture.current, stage, barrier,
            fault == 8 ? nullptr : PpbContextFixture::Read, false) && fixture.calls == (fault >= 6 ? 62U : 0U) &&
            !subject.Finish(true, false), "video graph malformed stage/barrier/budget/null/mixed modes reject before I/O");
    }
    FILE *record = std::tmpfile(); const int saved = dup(STDOUT_FILENO);
    std::fflush(stdout);
    const bool redirected = record && saved >= 0 && dup2(fileno(record), STDOUT_FILENO) >= 0;
    if (redirected) {
        stable.Report(2); stable.Finish(true);
        rejected.Report(1); rejected.video_graph = false; rejected.Report(1); rejected.Finish(true);
    }
    std::fflush(stdout);
    const bool restored = saved >= 0 && dup2(saved, STDOUT_FILENO) >= 0;
    if (saved >= 0) close(saved);
    char text[8192] = {}; size_t length = 0;
    if (record) { std::rewind(record); length = std::fread(text, 1, sizeof(text) - 1, record); std::fclose(record); }
    check(redirected && restored && length &&
        std::strstr(text, "Video allocation graph raw: stage=delivery-EOS-before-STOP pass=0 words=000d5400,000d6000,00000101,00010200") &&
        std::strstr(text, "authority delta: word=22 frozen=00200000 observed=00200004") &&
        std::strstr(text, "already-collected-first-difference only; no retry or changed-target read") &&
        std::strstr(text, "Video allocation graph raw: stage=first-output-after-release-and-owned-write pass=0 INCOMPLETE") &&
        std::strstr(text, "prefix/context/pool/ring/plane-target-reads=0") &&
        std::strstr(text, "PPB video graph finish: native-result=PASS observation-result=PASS stages=3/3 reads=186/186 bytes=936/936") &&
        std::strstr(text, "observation-result=FAIL") && !std::strstr(text, "Video allocation prefix") &&
        !std::strstr(text, "PPB saved context") && !std::strstr(text, " bytes=128 words="),
        "video graph report stays graph-only/frozen, hides incomplete words and separates native success from observational failure");
    const std::vector<const char *> valid = {"probe","--hardware","fixture","180","30","1","--scaler-test","0",
        "--observe-video-graph","--capture-yuy2","new"};
    Options admitted;
    check(ParseArguments(valid, &admitted) && admitted.observe_video_graph && admitted.observe_ppb_context &&
        !admitted.observe_video_prefix && !admitted.observe_ppb_stop && !admitted.observe_ppb_metadata && !admitted.observe_ppb_return &&
        !admitted.observe_arm_metadata && !admitted.observe_arm_source_shape && NeedsRawIo(admitted) && !NeedsRawIo(Options{}),
        "video graph has a separate exact option requiring the pre-action CAP_SYS_RAWIO gate");
    const std::vector<std::vector<const char *>> forbidden = {{"--observe-video-graph"},{"--observe-video-prefix"},
        {"--observe-ppb-context"},{"--observe-ppb-stop"},{"--observe-ppb-metadata"},{"--observe-ppb-return"},
        {"--observe-arm-metadata"},{"--observe-arm-source-shape"},{"--observe-runtime-inventory"},{"--observe-mfd-framing"},
        {"--observe-mfd-config"},{"--observe-mfd-address"},{"--observe-scl-config"},{"--observe-scl-filter-map"},
        {"--observe-scl-view","2"},{"--observe-chroma"},{"--inject-mfd-colour","a"},{"--scl-status-test","observe"},
        {"--open-only"},{"--mpeg1-via-mpeg2"},{"--h263-via-divx"},{"--scaler-test","0"}};
    for (const auto &extra : forbidden) for (unsigned order = 0; order < 2; ++order) {
        auto arguments = valid;
        arguments.insert(order ? arguments.end() - 2 : arguments.begin() + 8, extra.begin(), extra.end());
        Options invalid;
        check(!ParseArguments(arguments, &invalid), "video graph duplicates and all other experiments reject in both orders");
    }
    for (unsigned fault = 0; fault < 10; ++fault) {
        auto arguments = valid;
        if (fault == 0) arguments[1] = "--preflight";
        if (fault == 1) arguments[1] = "--self-test";
        if (fault == 2) arguments[3] = "179";
        if (fault == 3) arguments[5] = "2";
        if (fault == 4) arguments[7] = "128";
        if (fault == 5) arguments[8] = "--observe-video-graph=0";
        if (fault == 6) arguments[9] = "--capture-uyvy";
        if (fault == 7) arguments[10] = "-";
        if (fault == 8) arguments[10] = "";
        if (fault == 9) arguments.resize(9);
        Options invalid;
        check(!ParseArguments(arguments, &invalid), "video graph requires native hardware/count/one iteration/unscaled fresh YUY2 capture syntax");
    }
    Input native; native.codec = AV_CODEC_ID_H264; native.subtype = BC_MSUBTYPE_H264;
    native.progressive = true; native.width = 256; native.height = 96; native.packets.resize(180);
    check(ArmMetadataInputShape(admitted, native) && !ArmMetadataInputAdmitted(admitted, native) &&
        ArmMetadataInputAdmitted(Options{}, native), "video graph shape alone cannot bypass the exact frozen input byte digest");
}

template<class Check> static void VideoStagingSelfTest(const Check &check)
{
    PpbContextObserver disabled; disabled.video_staging = true;
    check(disabled.Observe(nullptr, 99, true, nullptr, false) && disabled.Finish(true, false) && !disabled.reads,
        "video staging default-off adds no I/O or owner requirements");
    const uint32_t graph_addresses[] = {0xd3a08,
        0xd3a20,0xd3ac4,0xd3ad0,0xd3bec,0xd3c90,0xd3c9c,0xd3db8,0xd3e5c,0xd3e68,0xd3f84,0xd4028,0xd4034,
        0xd6000,0xd6008,0xd6064,0xd60cc,0xd6224,0xd5408,0xd55a0,0xd55d4,
        0xd5800,0xd5808,0xd5810,0xd5a18,0xd5a28,0xd5a30,0xd5a40,0x11601c,0x11602c,0x116034};
    const unsigned graph_counts[] = {1,1,1,1,1,1,1,1,1,1,1,1,1,1,2,1,1,1,1,1,3,1,1,2,2,1,2,1,2,1,2};
    const unsigned accumulated_reads[] = {126,188,250}, accumulated_bytes[] = {880,1192,1504};
    struct TraceFixture : PpbContextFixture {
        uint32_t addresses[250] = {}, lengths[250] = {};
        static BC_STATUS Read(HANDLE handle, uint32_t *values, uint32_t bytes, uint32_t address) {
            auto *fixture = static_cast<TraceFixture *>(handle);
            if (fixture->calls < 250) { fixture->addresses[fixture->calls] = address; fixture->lengths[fixture->calls] = bytes; }
            return PpbContextFixture::Read(handle, values, bytes, address);
        }
    };
    PpbContextObserver initial, stable;
    for (unsigned variant = 0; variant < 4; ++variant) {
        TraceFixture fixture; fixture.video_staging = true;
        fixture.all_ones = variant == 1; fixture.prefix_zero = variant == 2; fixture.different = variant == 3;
        PpbContextObserver subject; subject.enabled = subject.video_staging = true;
        check(subject.Stages() == 3 && subject.ReadLimit() == 250 && subject.ByteLimit() == 1504 &&
            subject.StageReads(0) == 126 && subject.StageBytes(0) == 880 &&
            subject.StageReads(1) == 62 && subject.StageBytes(1) == 312 &&
            subject.StageReads(2) == 62 && subject.StageBytes(2) == 312,
            "video staging pins two pre-START targets and graph-only subsequent phase budgets");
        for (unsigned stage = 0; stage < 3; ++stage) {
            check(subject.Observe(&fixture.current, stage, stage != 0, TraceFixture::Read, false) && fixture.valid &&
                subject.reads == accumulated_reads[stage] && fixture.calls == subject.reads &&
                subject.bytes == accumulated_bytes[stage] && subject.stage_reads == (stage ? 62U : 126U) &&
                subject.stage_bytes == (stage ? 312U : 880U) && subject.measured == subject.stage_reads &&
                subject.complete[0] && subject.complete[1],
                "video staging enforces pre-START brackets then exact graph-only passes after native barriers");
            for (unsigned pass = 0; pass < 2; ++pass) {
                for (unsigned word = 0; word < 60; ++word)
                    if (stage || word >= 32 || variant == 1 || variant == 2)
                        check(subject.raw[pass][word] == (!stage && word < 32 && variant == 1 ? 0xffffffffU : 0U),
                            "video staging retains raw zero/all-ones only before START and never collects saved context");
                for (uint32_t word : subject.pool_raw[pass]) check(!word, "video staging never reads rings or a metadata pool");
                for (uint32_t word : subject.route_raw[pass]) check(!word, "video staging never reads route operands");
            }
            if (!stage && !variant) initial = subject;
            if (!stage && variant == 3) check(std::memcmp(subject.raw[0], subject.raw[1], 32 * sizeof(uint32_t)) != 0,
                "video staging permits different complete pre-START raw passes without claiming atomicity or image identity");
        }
        unsigned total = 0, targets = 0;
        for (unsigned at = 0; at < 250; ++at) {
            const unsigned ordinal = at < 126 ? at % 63 : (at - 126) % 31;
            const bool target = at < 126 && ordinal == 31;
            const unsigned field = at < 126 && ordinal > 31 ? ordinal - 32 : ordinal;
            total += fixture.lengths[at]; targets += target;
            check(fixture.addresses[at] == (target ? 0x200000U : graph_addresses[field]) &&
                fixture.lengths[at] == (target ? 128U : graph_counts[field] * 4U),
                "video staging literal250 trace permits precisely two pre-START front targets and no post-START targets");
        }
        check(total == 1504 && targets == 2 && subject.Finish(true, false) && !subject.Finish(false, false) && fixture.calls == 250,
            "video staging observation cannot override native pixel/EOS/cleanup failure or issue finishing I/O");
        if (!variant) stable = subject;
    }
    struct StatusFixture : PpbContextFixture {
        BC_STATUS result = BC_STS_ERROR;
        static BC_STATUS Read(HANDLE handle, uint32_t *values, uint32_t bytes, uint32_t address) {
            auto *fixture = static_cast<StatusFixture *>(handle);
            const BC_STATUS status = PpbContextFixture::Read(handle, values, bytes, address);
            if (fixture->calls == fixture->fail_at + 1) {
                for (unsigned word = 0; word < bytes / 4; ++word) values[word] = 0xdeadbeefU;
                return fixture->result;
            }
            return status;
        }
    };
    for (int code = -1; code <= BC_STS_PWR_MGMT; ++code) {
        if (code == BC_STS_SUCCESS) continue;
        for (unsigned at = 0; at < 250; ++at) {
            StatusFixture fixture; fixture.video_staging = true; fixture.fail_at = at; fixture.result = static_cast<BC_STATUS>(code);
            PpbContextObserver subject; subject.enabled = subject.video_staging = true;
            bool ok = true;
            for (unsigned stage = 0; stage < 3 && ok; ++stage)
                ok = subject.Observe(&fixture.current, stage, stage != 0, StatusFixture::Read, false);
            const unsigned ordinal = at < 126 ? at % 126 : (at - 126) % 62;
            const unsigned pass = ordinal / (at < 126 ? 63U : 31U), calls = fixture.calls;
            check(!ok && subject.failed && fixture.valid && calls == at + 1 && subject.status == code &&
                subject.failure == PpbContextFailure::Read && subject.measured == ordinal && !subject.complete[pass] &&
                !subject.Finish(true, false) && !subject.Observe(&fixture.current, 0, false, StatusFixture::Read, false) &&
                fixture.calls == calls, "video staging all27 API statuses at every250 callback reject poison and stay sticky");
        }
    }
    for (unsigned kind = 0; kind < 2; ++kind) for (unsigned at = 0; at < 250; ++at) {
        PpbContextFixture fixture, foreign; fixture.video_staging = true; fixture.lose_at = at;
        fixture.replacement = kind ? foreign.current : nullptr;
        PpbContextObserver subject; subject.enabled = subject.video_staging = true;
        bool ok = true;
        for (unsigned stage = 0; stage < 3 && ok; ++stage)
            ok = subject.Observe(&fixture.current, stage, stage != 0, PpbContextFixture::Read, false);
        const unsigned calls = fixture.calls;
        const unsigned pass = at < 126 ? at % 126 / 63 : (at - 126) % 62 / 31;
        check(!ok && subject.failure == PpbContextFailure::Owner && fixture.valid && calls == at + 1 &&
            !subject.complete[pass] && !subject.Finish(true, false) &&
            !subject.Observe(&foreign.current, 0, false, PpbContextFixture::Read, false) && !foreign.calls && fixture.calls == calls,
            "video staging every current-handle null/replacement prevents publication and foreign/future I/O");
    }
    struct ModeFixture : PpbContextFixture {
        PpbContextObserver *subject = nullptr;
        unsigned at = 0, field = 0;
        static BC_STATUS Read(HANDLE handle, uint32_t *values, uint32_t bytes, uint32_t address) {
            auto *fixture = static_cast<ModeFixture *>(handle);
            const BC_STATUS status = PpbContextFixture::Read(handle, values, bytes, address);
            if (fixture->calls == fixture->at + 1) {
                if (fixture->field == 0) fixture->subject->video_staging = false;
                if (fixture->field == 1) fixture->subject->video_prefix = true;
                if (fixture->field == 2) fixture->subject->video_graph = true;
                if (fixture->field == 3) fixture->subject->post_stop = true;
                if (fixture->field == 4) fixture->subject->metadata_pool = true;
                if (fixture->field == 5) fixture->subject->return_header = true;
                if (fixture->field == 6) fixture->subject->enabled = false;
                if (fixture->field == 7) fixture->subject->video_staging = true;
            }
            return status;
        }
    };
    for (unsigned field = 0; field < 7; ++field) for (unsigned at = 0; at < 250; ++at) {
        ModeFixture fixture; fixture.video_staging = true; fixture.at = at; fixture.field = field;
        PpbContextObserver subject; subject.enabled = subject.video_staging = true; fixture.subject = &subject;
        bool ok = true;
        for (unsigned stage = 0; stage < 3 && ok; ++stage)
            ok = subject.Observe(&fixture.current, stage, stage != 0, ModeFixture::Read, false);
        const unsigned calls = fixture.calls;
        check(!ok && subject.failure == PpbContextFailure::Argument && fixture.valid && calls == at + 1 && subject.owner_video_staging &&
            subject.ReadLimit() == 250 && subject.ByteLimit() == 1504 && subject.StageReads(0) == 126 &&
            subject.StageReads(1) == 62 && subject.StageBytes(0) == 880 && subject.StageBytes(2) == 312 &&
            !subject.Finish(true, false) && !subject.Observe(&fixture.current, 0, false, ModeFixture::Read, false) && fixture.calls == calls,
            "video staging every callback mode mutation fails without relabelling phases or relaxing frozen budgets");
    }
    for (bool graph : {false, true}) {
        ModeFixture fixture; fixture.video_graph = graph; fixture.video_prefix = !graph; fixture.field = 7;
        PpbContextObserver subject; subject.enabled = true; subject.video_graph = graph; subject.video_prefix = !graph; fixture.subject = &subject;
        check(!subject.Observe(&fixture.current, 0, false, ModeFixture::Read, false) && fixture.calls == 1 &&
            subject.failure == PpbContextFailure::Argument && subject.ReadLimit() == (graph ? 186U : 378U) &&
            subject.ByteLimit() == (graph ? 936U : 2640U), "existing graph/prefix owners cannot be switched into staging inside callbacks");
    }
    const uint32_t authority_addresses[] = {0xd3a08,0xd3a20,0xd3bec,0xd3db8,0xd3f84,
        0xd6000,0xd6008,0xd600c,0xd6064,0xd60cc,0xd6224,
        0xd5408,0xd55a0,0xd55d4,0xd55d8,0xd55dc,0xd5800,0xd5808,0xd5810,0xd5814,
        0xd5a18,0xd5a1c,0xd5a28,0xd5a30,0xd5a34,0xd5a40,0x11601c,0x116020,0x11602c,0x116034,0x116038};
    const unsigned authority_words[] = {0,1,4,7,10,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,
        28,29,30,31,32,33,34,35,36,37,38};
    PpbContextObserver rejected;
    for (unsigned edge = 0; edge < 31; ++edge) for (unsigned position : {32U,63U,126U,188U}) {
        TraceFixture fixture; fixture.video_staging = true; fixture.change_at = position; fixture.changed_address = authority_addresses[edge];
        const uint32_t original = fixture.Word(fixture.changed_address, 0); fixture.changed_value = original ^ 4U;
        PpbContextObserver subject; subject.enabled = subject.video_staging = true;
        const unsigned stage = position < 126 ? 0U : position == 126 ? 1U : 2U;
        for (unsigned before = 0; before < stage; ++before)
            check(subject.Observe(&fixture.current, before, before != 0, TraceFixture::Read, false), "video staging frozen authority setup");
        check(!subject.Observe(&fixture.current, stage, stage != 0, TraceFixture::Read, false) && fixture.valid &&
            subject.failure == PpbContextFailure::Changed && subject.changed_word == authority_words[edge] &&
            subject.changed_expected == original && subject.changed_observed == fixture.changed_value &&
            fixture.calls <= position + 31 && !subject.complete[1] && subject.complete[0] == (position == 63),
            "video staging all31 authority edges stop at pre-START bracket/pass or later graph without retargeting");
        unsigned targets = 0;
        for (unsigned call = 0; call < fixture.calls; ++call) {
            if (fixture.addresses[call] == 0x200000U) { ++targets; check(call == 31 || call == 94, "video staging rejected graph never enables a later target"); }
        }
        check(targets == (position < 126 ? 1U : 2U) && !subject.Finish(true, false), "video staging authority failure retains only admitted pre-START reads");
        const unsigned calls = fixture.calls;
        check(!subject.Observe(&fixture.current, stage, stage != 0, TraceFixture::Read, false) && fixture.calls == calls,
            "video staging first buffered authority diagnostic is sticky without retries");
        if (edge == 14 && position == 32) rejected = subject;
    }
    for (unsigned stage = 0; stage < 3; ++stage) for (bool byte_budget : {false, true}) {
        PpbContextFixture fixture; fixture.video_staging = true;
        PpbContextObserver subject; subject.enabled = subject.video_staging = true;
        for (unsigned before = 0; before < stage; ++before)
            check(subject.Observe(&fixture.current, before, before != 0, PpbContextFixture::Read, false), "video staging total budget setup");
        if (byte_budget) subject.bytes = subject.ByteLimit() - subject.StageBytes(stage) + 1;
        else subject.reads = subject.ReadLimit() - subject.StageReads(stage) + 1;
        const unsigned calls = fixture.calls;
        check(!subject.Observe(&fixture.current, stage, stage != 0, PpbContextFixture::Read, false) &&
            subject.failure == PpbContextFailure::Budget && fixture.calls == calls, "video staging each phase reserves its complete total budget before I/O");
    }
    for (unsigned stage = 0; stage < 3; ++stage) for (bool byte_budget : {false, true}) {
        PpbContextFixture fixture; fixture.video_staging = true;
        PpbContextObserver subject; subject.enabled = subject.video_staging = subject.owner_video_staging = true;
        subject.owner = fixture.current; subject.active_stage = stage;
        if (byte_budget) subject.stage_bytes = subject.StageBytes(stage) - 3;
        else subject.stage_reads = subject.StageReads(stage);
        uint32_t value = 0xa5a5a5a5U;
        check(!subject.Read(&fixture.current, PpbContextFixture::Read, 0xd3a08, 1, &value) &&
            subject.failure == PpbContextFailure::Budget && !fixture.calls && value == 0xa5a5a5a5U,
            "video staging per-phase read/byte limits refuse even when the overall budget remains unused");
    }
    for (unsigned fault = 0; fault < 8; ++fault) {
        PpbContextFixture fixture; fixture.video_staging = true;
        PpbContextObserver subject; subject.enabled = subject.video_staging = true;
        if (fault >= 3) check(subject.Observe(&fixture.current, 0, false, PpbContextFixture::Read, false), "video staging order/mode setup");
        if (fault == 5) subject.video_staging = false;
        if (fault == 6) subject.enabled = false;
        if (fault == 7) subject.video_prefix = true;
        check(!subject.Observe(fault == 2 ? nullptr : &fixture.current, fault == 0 ? 3U : fault == 3 ? 0U : fault >= 4 ? 1U : 0U,
            fault == 1 || fault >= 5, PpbContextFixture::Read, false) && fixture.calls == (fault >= 3 ? 126U : 0U) &&
            !subject.Finish(true, false), "video staging stage/barrier/current/mode changes refuse without extra I/O");
    }
    FILE *record = std::tmpfile(); const int saved = dup(STDOUT_FILENO);
    std::fflush(stdout);
    const bool redirected = record && saved >= 0 && dup2(fileno(record), STDOUT_FILENO) >= 0;
    if (redirected) { initial.video_staging = false; initial.Report(0); stable.Report(2); stable.Finish(true); stable.Finish(false); rejected.Report(0); }
    std::fflush(stdout);
    const bool restored = saved >= 0 && dup2(saved, STDOUT_FILENO) >= 0;
    if (saved >= 0) close(saved);
    char text[8192] = {}; size_t length = 0;
    if (record) { std::rewind(record); length = std::fread(text, 1, sizeof(text) - 1, record); std::fclose(record); }
    check(redirected && restored && length &&
        std::strstr(text, "Video allocation staging raw: stage=after-OPEN/pre-START pass=0 address=00200000 bytes=128 words=") &&
        std::strstr(text, "Video allocation staging graph: stage=delivery-EOS-before-STOP pass=1 words=") &&
        !std::strstr(text, "Video allocation staging raw: stage=delivery-EOS-before-STOP") &&
        std::strstr(text, "authority delta: word=22 frozen=00200000 observed=00200004") &&
        std::strstr(text, "pass=0 INCOMPLETE") && std::strstr(text, "post-START prefix/context/pool/ring/plane-target-reads=0") &&
        std::strstr(text, "non-atomic pixel-layout/lease/generation/completion/cause-certified=no") &&
        std::strstr(text, "PPB video staging finish: native-result=PASS observation-result=PASS stages=3/3 reads=250/250 bytes=1504/1504") &&
        std::strstr(text, "native-result=FAIL observation-result=PASS") && !std::strstr(text, "Video allocation prefix") &&
        !std::strstr(text, "PPB saved context"), "video staging frozen report separates complete pre-START bytes/later graph/failure and certifies neither pixels nor cause");
    const std::vector<const char *> valid = {"probe","--hardware","fixture","180","30","1","--scaler-test","0",
        "--observe-video-staging","--capture-yuy2","new"};
    Options admitted;
    check(ParseArguments(valid, &admitted) && admitted.observe_video_staging && admitted.observe_ppb_context &&
        !admitted.observe_video_prefix && !admitted.observe_video_graph && !admitted.observe_ppb_stop && !admitted.observe_ppb_metadata &&
        !admitted.observe_ppb_return && !admitted.observe_arm_metadata && NeedsRawIo(admitted),
        "video staging has a separate exact opt-in and CAP_SYS_RAWIO gate before any fixture/capture/device action");
    const std::vector<std::vector<const char *>> forbidden = {{"--observe-video-staging"},{"--observe-video-prefix"},{"--observe-video-graph"},
        {"--observe-ppb-context"},{"--observe-ppb-stop"},{"--observe-ppb-metadata"},{"--observe-ppb-return"},{"--observe-arm-metadata"},
        {"--observe-arm-source-shape"},{"--observe-runtime-inventory"},{"--observe-mfd-framing"},{"--observe-mfd-config"},{"--observe-mfd-address"},
        {"--observe-scl-config"},{"--observe-scl-filter-map"},{"--observe-scl-view","2"},{"--observe-chroma"},{"--inject-mfd-colour","a"},
        {"--scl-status-test","observe"},{"--open-only"},{"--mpeg1-via-mpeg2"},{"--h263-via-divx"},{"--scaler-test","0"},{"--capture-yuy2","other"}};
    for (const auto &extra : forbidden) for (unsigned order = 0; order < 2; ++order) {
        auto arguments = valid; arguments.insert(order ? arguments.end() - 2 : arguments.begin() + 8, extra.begin(), extra.end());
        Options invalid;
        check(!ParseArguments(arguments, &invalid), "video staging all duplicate/mixed profiles reject in either order");
    }
    for (unsigned fault = 0; fault < 10; ++fault) {
        auto arguments = valid;
        if (fault == 0) arguments[1] = "--preflight";
        if (fault == 1) arguments[1] = "--self-test";
        if (fault == 2) arguments[3] = "179";
        if (fault == 3) arguments[5] = "2";
        if (fault == 4) arguments[7] = "128";
        if (fault == 5) arguments[8] = "--observe-video-staging=0";
        if (fault == 6) arguments[9] = "--capture-uyvy";
        if (fault == 7) arguments[10] = "-";
        if (fault == 8) arguments[10] = "";
        if (fault == 9) arguments.resize(9);
        Options invalid;
        check(!ParseArguments(arguments, &invalid), "video staging requires exact native/unscaled/one-pass fresh YUY2 syntax");
    }
    Input native; native.codec = AV_CODEC_ID_H264; native.subtype = BC_MSUBTYPE_H264;
    native.progressive = true; native.width = 256; native.height = 96; native.packets.resize(180);
    check(ArmMetadataInputShape(admitted, native) && !ArmMetadataInputAdmitted(admitted, native) && ArmMetadataInputAdmitted(Options{}, native),
        "video staging retains the old exact-input digest, not just geometry/packet count admission");
}

struct AvdMemoryFixture {
    uint32_t raw[48] = {};
    unsigned calls = 0, fail_at = 48, lose_at = 48, mutate_at = 48, mutation = 0;
    BC_STATUS status = BC_STS_ERROR;
    bool valid = true;
    HANDLE current = this, replacement = nullptr;
    AvdMemoryObserver *subject = nullptr;
    AvdMemoryFixture() {
        for (unsigned index = 0; index < 48; ++index)
            raw[index] = index % 8 == 0 || index % 8 == 7 ? 0x50U :
                index % 8 == 1 || index % 8 == 6 ? 0U : 0x12000000U + index;
    }
    static BC_STATUS Read(HANDLE handle, uint32_t address, uint32_t *value) {
        auto *f = static_cast<AvdMemoryFixture *>(handle);
        const unsigned index = f->calls++;
        const uint32_t expected[] = {0x540000, 0x4000d4, 0x846000, 0x848000, 0x856000, 0x858000, 0x4000d4, 0x540000};
        if (index >= 48 || !value) { f->valid = false; return BC_STS_ERROR; }
        f->valid &= address == expected[index % 8];
        *value = index == f->fail_at ? 0xdeadbeefU : f->raw[index];
        if (index == f->lose_at) f->current = f->replacement;
        if (index == f->mutate_at && f->subject) {
            if (f->mutation == 0) f->subject->enabled = false;
            if (f->mutation == 1) f->subject->conflicting_mode = true;
            if (f->mutation == 2) ++f->subject->reads;
            if (f->mutation == 3) ++f->subject->bytes;
        }
        return index == f->fail_at ? f->status : BC_STS_SUCCESS;
    }
    bool Exercise(AvdMemoryObserver *o) {
        subject = o; o->enabled = true;
        for (unsigned stage = 0; stage < 3; ++stage)
            if (!o->Observe(&current, stage, stage != 0, Read, false)) return false;
        return o->Finish(true, false);
    }
};

template<class Check> static void AvdMemorySelfTest(const Check &check)
{
    const uint32_t addresses[] = {0x540000, 0x4000d4, 0x846000, 0x848000, 0x856000, 0x858000, 0x4000d4, 0x540000};
    for (unsigned field = 0; field < 8; ++field)
        check(AvdMemoryAddress(field) == addresses[field] && addresses[field] != 0x4000c8,
            "AVD inventory independent eight-word whitelist excludes error-clear and adjacent/window-END words");
    check(AvdMemoryAddress(8) == 0 && BCHP_SUN_GISB_ARB_ERR_CAP_CLR_clear_MASK == 1 &&
        BCHP_SUN_GISB_ARB_ERR_CAP_STATUS_valid_MASK == 1 && BCHP_SUN_GISB_ARB_ERR_CAP_STATUS_timeout_MASK == 0x1000 &&
        BCHP_DECODE_CPUIMEM_0_CPUIMEM_REG_Addr_MASK == 0xffffffffU &&
        BCHP_DECODE_CPUDMEM_0_CPUDMEM_REG_Addr_MASK == 0xffffffffU,
        "AVD inventory named RDB field receipts do not turn opaque words into pointers or native ACKs");
    AvdMemoryObserver disabled;
    check(disabled.Observe(nullptr, 99, true, nullptr, false) && disabled.Finish(true, false) &&
        !disabled.Finish(false, false) && disabled.reads == 0 && disabled.bytes == 0,
        "AVD inventory defaults off with zero observer I/O and preserves native failure");
    AvdMemoryObserver stable;
    for (unsigned variant = 0; variant < 3; ++variant) {
        struct Canary { uint32_t before = 0x12345678; AvdMemoryFixture fixture; AvdMemoryObserver observer; uint32_t after = 0x87654321; } c;
        for (unsigned index = 0; index < 48; ++index) if (index % 8 >= 2 && index % 8 <= 5)
            c.fixture.raw[index] = variant == 0 ? 0U : variant == 1 ? 0xffffffffU : 0x80000000U ^ (index * 0x1020301U);
        check(c.fixture.Exercise(&c.observer) && c.fixture.valid && c.fixture.calls == 48 && c.observer.reads == 48 &&
            c.observer.bytes == 192 && c.observer.next_stage == 3 && !c.observer.failed &&
            c.before == 0x12345678 && c.after == 0x87654321 && !c.observer.Finish(false, false),
            "AVD inventory literal 48/192 trace admits zero/all-ones/changing opaque memory and preserves canaries/native result");
        for (unsigned stage = 0; stage < 3; ++stage) for (unsigned pass = 0; pass < 2; ++pass) {
            check(c.observer.complete[stage][pass] && c.observer.measured[stage][pass] == 8 &&
                !std::memcmp(c.observer.raw[stage][pass], c.fixture.raw + stage * 16 + pass * 8, 32),
                "AVD inventory complete tuple is published only after both closing guards");
        }
        if (variant == 2) stable = c.observer;
    }
    for (unsigned at = 0; at < 48; ++at) {
        const unsigned stage = at / 16, pass = at % 16 / 8, field = at % 8;
        for (int code = -1; code <= BC_STS_PWR_MGMT; ++code) {
            if (code == BC_STS_SUCCESS) continue;
            AvdMemoryFixture f; f.fail_at = at; f.status = static_cast<BC_STATUS>(code);
            AvdMemoryObserver o;
            check(!f.Exercise(&o) && f.valid && f.calls == at + 1 && o.reads == at + 1 && o.bytes == (at + 1) * 4 &&
                o.failure == AvdMemoryFailure::Api && o.api_status == code && o.measured[stage][pass] == field &&
                !o.complete[stage][pass] && o.raw[stage][pass][field] == 0 && !o.Finish(true, false),
                "AVD inventory every non-success API status at all 48 callbacks excludes poison/unread/partial publications");
            o.enabled = false;
            check(!o.Observe(&f.current, 0, false, AvdMemoryFixture::Read, false) && f.calls == at + 1 &&
                !o.Finish(true, false), "AVD inventory API failure remains sticky even after mode disable");
        }
        for (bool replace : {false, true}) {
            AvdMemoryFixture f, other; f.lose_at = at; f.replacement = replace ? static_cast<HANDLE>(&other) : nullptr;
            AvdMemoryObserver o;
            check(!f.Exercise(&o) && f.valid && f.calls == at + 1 && other.calls == 0 &&
                o.failure == AvdMemoryFailure::Owner && o.measured[stage][pass] == field &&
                o.raw[stage][pass][field] == 0 && !o.complete[stage][pass],
                "AVD inventory current handle null/replacement during each callback is never published or followed");
            f.current = &f;
            check(!o.Observe(&f.current, stage, stage != 0, AvdMemoryFixture::Read, false) && f.calls == at + 1,
                "AVD inventory restored handle cannot retry a lost-owner observation");
        }
        for (unsigned mutation = 0; mutation < 4; ++mutation) {
            AvdMemoryFixture f; f.mutate_at = at; f.mutation = mutation; AvdMemoryObserver o;
            check(!f.Exercise(&o) && f.valid && f.calls == at + 1 && o.failed &&
                o.failure == (mutation < 2 ? AvdMemoryFailure::Mode : AvdMemoryFailure::Budget) &&
                o.measured[stage][pass] == field && !o.complete[stage][pass] && o.raw[stage][pass][field] == 0,
                "AVD inventory callback mode/conflicting-profile/counter mutation fails before value publication");
        }
        if (field == 1 || field == 6) for (unsigned bit = 0; bit < 32; ++bit) {
            AvdMemoryFixture f; f.raw[at] = 1U << bit; AvdMemoryObserver o;
            check(!f.Exercise(&o) && f.valid && f.calls == at + 1 && o.failure == AvdMemoryFailure::Gisb &&
                o.rejected_raw_valid && o.rejected_address == 0x4000d4 && o.rejected_raw == (1U << bit) &&
                o.measured[stage][pass] == field + 1 && !o.complete[stage][pass] && !o.Finish(true, false),
                "AVD inventory any GISB bit including reserved bits is fatal raw diagnosis without clear or retry");
        }
        if (field == 0 || field == 7) for (uint32_t value : {0U, 0x51U, 0x10050U, 0xffffffffU}) {
            AvdMemoryFixture f; f.raw[at] = value; AvdMemoryObserver o;
            check(!f.Exercise(&o) && f.valid && f.calls == at + 1 && o.failure == AvdMemoryFailure::Revision &&
                o.rejected_raw_valid && o.rejected_raw == value && !o.complete[stage][pass],
                "AVD inventory both revisions in every pass require exact 50 including reserved bits");
        }
    }
    for (unsigned fault = 0; fault < 9; ++fault) {
        AvdMemoryFixture f; AvdMemoryObserver o; o.enabled = true;
        bool ok = false;
        if (fault == 0) ok = o.Observe(nullptr, 0, false, AvdMemoryFixture::Read, false);
        if (fault == 1) ok = o.Observe(&f.current, 0, false, nullptr, false);
        if (fault == 2) ok = o.Observe(&f.current, 3, true, AvdMemoryFixture::Read, false);
        if (fault == 3) ok = o.Observe(&f.current, 1, true, AvdMemoryFixture::Read, false);
        if (fault == 4) ok = o.Observe(&f.current, 0, true, AvdMemoryFixture::Read, false);
        if (fault == 5) { o.reads = 1; ok = o.Observe(&f.current, 0, false, AvdMemoryFixture::Read, false); }
        if (fault == 6) { o.bytes = 1; ok = o.Observe(&f.current, 0, false, AvdMemoryFixture::Read, false); }
        if (fault == 7) { o.bytes = UINT_MAX; ok = o.Observe(&f.current, 0, false, AvdMemoryFixture::Read, false); }
        if (fault == 8) { o.conflicting_mode = true; ok = o.Observe(&f.current, 0, false, AvdMemoryFixture::Read, false); }
        check(!ok && o.failed && f.calls == 0 && !o.Finish(true, false),
            "AVD inventory bad initial argument/order/barrier/budget/mixed profile performs no observer I/O");
    }
    for (unsigned fault = 0; fault < 5; ++fault) {
        AvdMemoryFixture f; AvdMemoryObserver o; o.enabled = true;
        check(o.Observe(&f.current, 0, false, AvdMemoryFixture::Read, false), "AVD inventory stage0 setup");
        if (fault == 0) o.enabled = false;
        if (fault == 1) o.conflicting_mode = true;
        if (fault == 2) o.reads = 48;
        if (fault == 3) o.bytes = 192;
        check(!o.Observe(&f.current, 1, fault != 4, AvdMemoryFixture::Read, false) && f.calls == 16 &&
            !o.Finish(true, false), "AVD inventory sealed mode/budget/first-release-and-owned-write barrier cannot be bypassed");
    }
    AvdMemoryFixture rejected_fixture; rejected_fixture.fail_at = 10;
    AvdMemoryObserver rejected; check(!rejected_fixture.Exercise(&rejected), "AVD inventory reporter API failure setup");
    FILE *log = std::tmpfile(); const int saved = log ? dup(STDOUT_FILENO) : -1;
    const bool redirected = saved >= 0 && dup2(fileno(log), STDOUT_FILENO) >= 0;
    if (redirected) { stable.enabled = false; stable.Report(0); stable.Report(2); stable.enabled = true;
        stable.Finish(true); stable.Finish(false); rejected.Report(0); }
    std::fflush(stdout);
    if (saved >= 0) { (void)dup2(saved, STDOUT_FILENO); close(saved); }
    char text[8192] = {}; size_t length = 0;
    if (log) { std::rewind(log); length = std::fread(text, 1, sizeof(text) - 1, log); std::fclose(log); }
    check(redirected && length && std::strstr(text, "00846000=") && std::strstr(text, "pass=1 INCOMPLETE measured=2/8") &&
        !std::strstr(text, "deadbeef") && std::strstr(text, "native-result=PASS observation-result=PASS stages=3/3 reads=48/48 bytes=192/192") &&
        std::strstr(text, "native-result=FAIL observation-result=PASS") && std::strstr(text, "error-clear-writes=0 pointer-follow=0") &&
        std::strstr(text, "atomic/alias/current-context/pixels/lease/completion-certified=no"),
        "AVD inventory frozen reporter publishes only complete passes and separates native failure without bus/alias/lease claims");
    const std::vector<const char *> valid = {"probe","--hardware","fixture","180","30","1","--scaler-test","0",
        "--observe-avd-memory","--capture-yuy2","new"};
    Options admitted;
    check(ParseArguments(valid, &admitted) && admitted.observe_avd_memory && !AvdMemoryConflicts(admitted) &&
        NeedsRawIo(admitted) && !NeedsRawIo(Options{}), "AVD inventory exact separate opt-in uses CAP_SYS_RAWIO before all actions");
    const std::vector<std::vector<const char *>> forbidden = {{"--observe-avd-memory"},{"--observe-video-staging"},{"--observe-video-prefix"},{"--observe-video-graph"},
        {"--observe-ppb-context"},{"--observe-ppb-stop"},{"--observe-ppb-metadata"},{"--observe-ppb-return"},{"--observe-arm-metadata"},
        {"--observe-arm-source-shape"},{"--observe-runtime-inventory"},{"--observe-mfd-framing"},{"--observe-mfd-config"},{"--observe-mfd-address"},
        {"--observe-scl-config"},{"--observe-scl-filter-map"},{"--observe-scl-view","2"},{"--observe-chroma"},{"--inject-mfd-colour","a"},
        {"--scl-status-test","observe"},{"--open-only"},{"--mpeg1-via-mpeg2"},{"--h263-via-divx"},{"--scaler-test","0"},{"--capture-yuy2","other"}};
    for (const auto &extra : forbidden) for (unsigned order = 0; order < 2; ++order) {
        auto args = valid; args.insert(order ? args.end() - 2 : args.begin() + 8, extra.begin(), extra.end()); Options o;
        check(!ParseArguments(args, &o), "AVD inventory all duplicate/mixed options reject in either order");
    }
    for (unsigned fault = 0; fault < 10; ++fault) {
        auto args = valid;
        if (fault == 0) args[1] = "--preflight";
        if (fault == 1) args[1] = "--self-test";
        if (fault == 2) args[3] = "179";
        if (fault == 3) args[5] = "2";
        if (fault == 4) args[7] = "128";
        if (fault == 5) args[8] = "--observe-avd-memory=0";
        if (fault == 6) args[9] = "--capture-uyvy";
        if (fault == 7) args[10] = "-";
        if (fault == 8) args[10] = "";
        if (fault == 9) args.resize(9);
        Options o; check(!ParseArguments(args, &o), "AVD inventory requires native/unscaled/180/one-iteration/fresh YUY2 syntax");
    }
    Input native; native.codec = AV_CODEC_ID_H264; native.subtype = BC_MSUBTYPE_H264;
    native.progressive = true; native.width = 256; native.height = 96; native.packets.resize(180);
    check(AvdMemoryInputShape(admitted, native) && !AvdMemoryInputAdmitted(admitted, native) &&
        AvdMemoryInputAdmitted(Options{}, native), "AVD inventory admits neither geometry-only nor unpinned submitted bytes");
    for (unsigned fault = 0; fault < 7; ++fault) {
        Input wrong; wrong.codec = AV_CODEC_ID_H264; wrong.subtype = BC_MSUBTYPE_H264;
        wrong.progressive = true; wrong.width = 256; wrong.height = 96; wrong.packets.resize(180);
        Options o = admitted;
        if (fault == 0) wrong.codec = AV_CODEC_ID_MPEG2VIDEO;
        if (fault == 1) wrong.subtype = BC_MSUBTYPE_MPEG2VIDEO;
        if (fault == 2) wrong.progressive = false;
        if (fault == 3) wrong.width = 128;
        if (fault == 4) wrong.height = 48;
        if (fault == 5) wrong.packets.resize(179);
        if (fault == 6) o.expected = 179;
        check(!AvdMemoryInputShape(o, wrong), "AVD inventory rejects hidden codec/interlace/geometry/packet-count changes");
    }
    native.metadata.push_back(0);
    check(!AvdMemoryInputShape(admitted, native), "AVD inventory pinned native source has no extra metadata");
}

template<class Check> static void MfdFramingSelfTest(const Check &check)
{
    const uint32_t addresses[] = {0x00540000, 0x00540078, 0x00540050, 0x00540070, 0x00540000};
    const uint32_t reserved[] = {0xffff0000U, 0xfc000000U, 0xdfffe000U, 0xfffffffcU, 0xffff0000U};
    for (unsigned field = 0; field < 5; ++field)
        check(MfdFramingAddress(field) == addresses[field] && MfdFramingReserved(field) == reserved[field],
            "MFD framing whitelist addresses and reserved masks match independent literals");
    check(BCHP_MFD_BVB_SAMPLE_DATA_reserved0_MASK == 0xfc000000U &&
        (BCHP_MFD_FEED_STATUS_reserved0_MASK | BCHP_MFD_FEED_STATUS_reserved1_MASK) == 0xdfffe000U &&
        BCHP_MFD_FEEDER_BVB_STATUS_reserved0_MASK == 0xfffffffcU,
        "MFD framing masks agree with the primary RDB, not a debug-port format");
    for (bool unequal : {false, true}) {
        MfdFramingFixture fixture;
        if (unequal) for (unsigned stage = 0; stage < 3; ++stage) {
            fixture.raw[stage * 10 + 6] ^= 0x00ffffffU;
            fixture.raw[stage * 10 + 7] ^= 0x20001fffU;
            fixture.raw[stage * 10 + 8] ^= 3U;
        }
        MfdFramingObserver subject;
        check(fixture.Exercise(&subject) && fixture.valid && fixture.calls == 30 && subject.reads == 30 &&
            subject.attempted == 7 && subject.last_frame == 180 && subject.owner == &fixture &&
            !subject.failed && subject.snapshot.Equal() == !unequal && !subject.Finish(false, false),
            "MFD framing exact 30-read trace admits sync3/count8191/opaque payload/EOL/EOF and unequal dynamic passes");
    }
    for (unsigned position = 0; position < 30; ++position) {
        for (int code = -1; code <= BC_STS_PWR_MGMT; ++code) {
            if (code == BC_STS_SUCCESS) continue;
            MfdFramingFixture fixture; fixture.fail_at = position; fixture.status = static_cast<BC_STATUS>(code);
            MfdFramingObserver subject;
            check(!fixture.Exercise(&subject) && fixture.valid && fixture.calls == position + 1 &&
                subject.failed && subject.reads == position + 1 && subject.snapshot.reads == position % 10 + 1 &&
                subject.snapshot.measured == position % 10 && subject.snapshot.status == code &&
                subject.snapshot.failure == MfdFramingFailure::Read && !subject.Finish(true, false),
                "MFD framing all 27 non-success statuses at every read stop at the exact callback");
            bool excluded = true;
            for (unsigned unread = position % 10; unread < 10; ++unread) excluded &= subject.snapshot.raw[unread] == 0;
            check(excluded, "MFD framing API poison and unread outputs are excluded from measured raw values");
            MfdFramingFixture other;
            check(!subject.PreStart(&fixture.current, MfdFramingFixture::Read, false) &&
                !subject.AfterDelivered(&other.current, 90, true, true, MfdFramingFixture::Read, false) &&
                fixture.calls == position + 1 && other.calls == 0,
                "MFD framing API failure is sticky without retries or replacement-handle I/O");
        }
        for (bool replaced : {false, true}) {
            MfdFramingFixture fixture, other; fixture.lose_at = position;
            fixture.replacement = replaced ? static_cast<HANDLE>(&other) : nullptr;
            MfdFramingObserver subject;
            check(!fixture.Exercise(&subject) && fixture.valid && fixture.calls == position + 1 &&
                subject.reads == position + 1 && subject.snapshot.measured == position % 10 &&
                subject.snapshot.failure == MfdFramingFailure::Owner && other.calls == 0,
                "MFD framing current HANDLE null/replacement during every callback excludes its output and later reads");
            fixture.current = &fixture;
            check(!subject.PreStart(&fixture.current, MfdFramingFixture::Read, false) &&
                !subject.AfterDelivered(&fixture.current, 1, true, true, MfdFramingFixture::Read, false) &&
                fixture.calls == position + 1,
                "MFD framing apparent HANDLE recovery cannot clear the failure latch");
        }
        std::vector<uint32_t> invalid = {0xffffffffU};
        const unsigned field = position % 5;
        for (unsigned bit = 0; bit < 32; ++bit) {
            if (field == 0 || field == 4) invalid.push_back(0x50U ^ (1U << bit));
            else if (reserved[field] & (1U << bit)) invalid.push_back(1U << bit);
        }
        for (uint32_t value : invalid) {
            MfdFramingFixture fixture; fixture.raw[position] = value;
            MfdFramingObserver subject;
            const auto expected = value == 0xffffffffU ? MfdFramingFailure::Unavailable :
                field == 0 || field == 4 ? MfdFramingFailure::Revision : MfdFramingFailure::Reserved;
            check(!fixture.Exercise(&subject) && fixture.valid && fixture.calls == position + 1 &&
                subject.snapshot.measured == position % 10 + 1 && subject.snapshot.raw[position % 10] == value &&
                subject.snapshot.failure == expected && !subject.Finish(true, false),
                "MFD framing every reserved/revision bit and all-ones scalar retains successful raw but fails closed");
            check(!subject.PreStart(&fixture.current, MfdFramingFixture::Read, false) &&
                !subject.AfterDelivered(&fixture.current, 90, true, true, MfdFramingFixture::Read, false) &&
                fixture.calls == position + 1,
                "MFD framing successful but invalid scalar permits no future observer I/O");
        }
    }
    {
        MfdFramingFixture fixture, other;
        struct { uint32_t before = 0x12345678; MfdFramingSnapshot value; uint32_t after = 0x87654321; } guarded;
        check(ReadMfdFraming(&fixture.current, &fixture, &guarded.value, MfdFramingFixture::Read) &&
            guarded.value.measured == 10 && guarded.value.Equal() && fixture.valid && fixture.calls == 10 &&
            guarded.before == 0x12345678 && guarded.after == 0x87654321,
            "MFD framing ten-word snapshot remains within canaries");
        fixture.calls = 0; HANDLE missing = nullptr;
        check(!ReadMfdFraming(nullptr, &fixture, &guarded.value, MfdFramingFixture::Read) &&
            !ReadMfdFraming(&missing, &fixture, &guarded.value, MfdFramingFixture::Read) &&
            !ReadMfdFraming(&fixture.current, nullptr, &guarded.value, MfdFramingFixture::Read) &&
            !ReadMfdFraming(&fixture.current, &fixture, nullptr, MfdFramingFixture::Read) &&
            !ReadMfdFraming(&fixture.current, &fixture, &guarded.value, nullptr) &&
            !ReadMfdFraming(&other.current, &fixture, &guarded.value, MfdFramingFixture::Read) &&
            fixture.calls == 0 && other.calls == 0,
            "MFD framing null arguments or initial owner mismatch cause zero reads");
        MfdFramingObserver disabled;
        check(disabled.Observe(nullptr, UINT_MAX, false, false, nullptr, false) &&
            disabled.AfterDelivered(nullptr, UINT_MAX, false, false, nullptr, false) &&
            disabled.Finish(true, false) && !disabled.Finish(false, false) && disabled.reads == 0,
            "MFD framing default-off is a pure zero-I/O no-op preserving the native result");
    }
    for (unsigned cutoff : {0U, 1U, 89U, 90U, 179U, 180U}) {
        for (unsigned invalid = 0; invalid < 10; ++invalid) {
            MfdFramingFixture fixture, other; MfdFramingObserver subject; subject.enabled = true;
            check(subject.PreStart(&fixture.current, MfdFramingFixture::Read, false), "MFD framing order-fault preSTART");
            for (unsigned frame = 1; frame <= cutoff; ++frame)
                (void)subject.AfterDelivered(&fixture.current, frame, true, true, MfdFramingFixture::Read, false);
            const unsigned stopped = fixture.calls;
            HANDLE missing = nullptr;
            const HANDLE *current = invalid == 0 ? nullptr : invalid == 1 ? &missing :
                invalid == 2 ? &other.current : &fixture.current;
            const unsigned frame = invalid == 5 ? 0 : invalid == 6 ? 181 : invalid == 7 ? cutoff :
                invalid == 8 ? cutoff + 2 : cutoff + 1;
            check(!subject.AfterDelivered(current, frame, invalid != 3, invalid != 4,
                    invalid == 9 ? nullptr : MfdFramingFixture::Read, false) && subject.failed &&
                !subject.AfterDelivered(&fixture.current, cutoff + 1, true, true, MfdFramingFixture::Read, false) &&
                fixture.calls == stopped && other.calls == 0 && !subject.Finish(true, false),
                "MFD framing order/current/release/owned-write/duplicate loss is sticky at each milestone and Finish");
        }
    }
    for (unsigned invalid = 0; invalid < 8; ++invalid) {
        MfdFramingFixture fixture; MfdFramingObserver subject; subject.enabled = true;
        HANDLE missing = nullptr;
        check(!subject.Observe(invalid == 0 ? nullptr : invalid == 1 ? &missing : &fixture.current,
                invalid == 3 ? 3U : invalid == 4 ? UINT_MAX : invalid == 5 ? 1U : 0U,
                invalid == 6, invalid == 7, invalid == 2 ? nullptr : MfdFramingFixture::Read, false) &&
            subject.failed && !subject.PreStart(&fixture.current, MfdFramingFixture::Read, false) && fixture.calls == 0,
            "MFD framing invalid initial argument/stage/order rejects before I/O");
    }
    {
        MfdFramingFixture fixture; MfdFramingObserver subject; subject.enabled = true;
        check(!subject.Finish(true, false) && subject.PreStart(&fixture.current, MfdFramingFixture::Read, false) &&
            subject.attempted == 1 && !subject.Finish(true, false), "MFD framing Finish cannot invent a missing delivery stage");
        bool sequence = true;
        for (unsigned frame = 1; frame <= 180; ++frame) {
            sequence &= subject.AfterDelivered(&fixture.current, frame, true, true, MfdFramingFixture::Read, false);
            sequence &= fixture.calls == (frame < 90 ? 20U : 30U) &&
                subject.attempted == (frame < 90 ? 3U : 7U) && subject.Finish(true, false) == (frame == 180);
        }
        check(sequence && subject.Finish(true, false) && !subject.Finish(false, false),
            "MFD framing exact frame1..180 masks1/3/7 and 30 reads require the independent full native/EOS/capture/close result");
        MfdFramingFixture other; MfdFramingObserver direct; direct.enabled = true;
        check(direct.PreStart(&other.current, MfdFramingFixture::Read, false) &&
            !direct.Observe(&other.current, 2, true, true, MfdFramingFixture::Read, false) && other.calls == 10,
            "MFD framing cannot directly bypass first output or frame89 to observe frame90");
    }
    for (unsigned scenario = 0; scenario < 5; ++scenario) {
        FILE *record = std::tmpfile(); const int saved_stdout = dup(STDOUT_FILENO);
        std::fflush(stdout);
        const bool redirected = record && saved_stdout >= 0 && dup2(fileno(record), STDOUT_FILENO) >= 0;
        bool result = false;
        if (redirected) {
            MfdFramingFixture fixture; MfdFramingObserver subject;
            if (scenario == 1) fixture.raw[26] ^= 1U;
            if (scenario == 2) fixture.fail_at = 1;
            if (scenario == 3) fixture.lose_at = 1;
            if (scenario == 4) fixture.raw[1] = 0x04000000;
            result = fixture.Exercise(&subject, true) == (scenario < 2);
            std::fflush(stdout);
        }
        const bool restored = saved_stdout >= 0 && dup2(saved_stdout, STDOUT_FILENO) >= 0;
        if (saved_stdout >= 0) close(saved_stdout);
        char text[16384] = {}; size_t bytes = 0;
        if (record) { std::rewind(record); bytes = std::fread(text, 1, sizeof(text) - 1, record); std::fclose(record); }
        check(redirected && restored && result && bytes && !std::strstr(text, "deadbeef") &&
            std::strstr(text, "atomic/admission/lease/completion-certified=no") &&
            (scenario < 2 ? (std::strstr(text, "frame90-after-release-and-owned-write") &&
                std::strstr(text, "bvb-sample@00540078=03ffffff PICTURE_SYNC=3 LINE_SYNC=3 COLOUR_SYNC=3 LUMA=1023 CHROMA=1023") &&
                std::strstr(text, "feed-status@00540050=20001fff LAST_LINE=1 LINE_COUNT=8191") &&
                std::strstr(text, "bvb-status@00540070=00000003 EOF=1 EOL=1") &&
                std::strstr(text, scenario ? "raw-equal=no" : "raw-equal=yes") &&
                std::strstr(text, "api-reads=30/30")) :
                (std::strstr(text, "bvb-status@00540070=NOT-READ") &&
                 std::strstr(text, scenario == 4 ? "bvb-sample@00540078=04000000" : "bvb-sample@00540078=NOT-READ"))),
            "MFD framing actual reporter publishes separate pass fields/scalar errors, not API poison/unread/lost-handle outputs");
    }
    const std::vector<const char *> valid = {"probe", "--hardware", "fixture", "180", "30", "1",
        "--scaler-test", "0", "--observe-mfd-framing", "--capture-yuy2", "new"};
    Options admitted;
    check(ParseArguments(valid, &admitted) && admitted.observe_mfd_framing && NeedsRawIo(admitted) &&
        admitted.mode == Mode::Hardware && !NeedsRawIo(Options{}),
        "MFD framing explicit native capture requires the CAP_SYS_RAWIO gate before any action");
    Options implicit;
    check(ParseArguments({"probe", "--hardware", "fixture", "180", "--scaler-test", "0",
        "--observe-mfd-framing", "--capture-yuy2", "new"}, &implicit) && implicit.iterations == 1,
        "MFD framing implicit one iteration is admitted");
    for (unsigned field = 0; field < 16; ++field) {
        auto arguments = valid;
        if (field == 0) arguments[1] = "--preflight";
        if (field == 1) arguments[3] = "179";
        if (field == 2) arguments[5] = "2";
        if (field == 3) arguments[7] = "320";
        if (field == 4) arguments[7] = "640";
        if (field == 5) arguments[9] = "--capture-uyvy";
        if (field == 6) arguments.resize(9);
        if (field == 7) arguments[10] = "";
        if (field == 8) arguments[10] = "-";
        if (field == 9) arguments.erase(arguments.begin() + 6, arguments.begin() + 8);
        if (field == 10) arguments.insert(arguments.begin() + 8, "--mpeg1-via-mpeg2");
        if (field == 11) arguments.insert(arguments.begin() + 8, "--h263-via-divx");
        if (field == 12) arguments.insert(arguments.begin() + 8, "--open-only");
        if (field == 13) std::swap(arguments[8], arguments[9]);
        if (field == 14) arguments.resize(10);
        if (field == 15) arguments.insert(arguments.end(), {"--capture-yuy2", "another"});
        Options rejected;
        check(!ParseArguments(arguments, &rejected), "MFD framing malformed/mixed native profile refuses before actions");
    }
    for (const auto &mixed : std::vector<std::vector<const char *>>{
            {"--observe-chroma"}, {"--observe-scl-config"}, {"--observe-scl-filter-map"},
            {"--observe-scl-view", "2"}, {"--observe-scl-view", "3"}, {"--observe-mfd-config"},
            {"--observe-mfd-address"}, {"--observe-runtime-inventory"}, {"--observe-mfd-framing"},
            {"--inject-mfd-colour", "a"}, {"--inject-mfd-colour", "b"},
            {"--scl-status-test", "observe"}, {"--scl-status-test", "clear"},
            {"--scaler-test", "0"}, {"--self-test"}, {"--preflight"}, {"--hardware"}}) {
        for (unsigned where : {8U, 9U}) {
            auto arguments = valid; arguments.insert(arguments.begin() + where, mixed.begin(), mixed.end());
            Options rejected; check(!ParseArguments(arguments, &rejected),
                "MFD framing all diagnostics/duplicates/hidden options refuse in both orders");
        }
    }
    Options rejected;
    check(!ParseArguments({"probe", "--self-test", "--observe-mfd-framing"}, &rejected) &&
        !ParseArguments({"probe", "--observe-mfd-framing", "--self-test"}, &rejected),
        "MFD framing cannot mix with self-test in either order");
    Input native; native.codec = AV_CODEC_ID_MPEG2VIDEO; native.subtype = BC_MSUBTYPE_MPEG2VIDEO;
    native.progressive = true; native.width = 640; native.height = 360; native.packets.resize(180);
    check(SclInputAdmitted(admitted, native), "MFD framing exact native progressive MPEG2 input admitted");
    for (unsigned field = 0; field < 8; ++field) {
        Options changed = admitted; Input altered = native;
        if (field == 0) altered.codec = AV_CODEC_ID_H264;
        if (field == 1) altered.subtype = BC_MSUBTYPE_H264;
        if (field == 2) altered.progressive = false;
        if (field == 3) altered.width = 638;
        if (field == 4) altered.height = 358;
        if (field == 5) altered.packets.pop_back();
        if (field == 6) altered.packets.emplace_back();
        if (field == 7) changed.expected = 179;
        check(!SclInputAdmitted(changed, altered) && SclInputAdmitted(Options{}, altered),
            "MFD framing shape/codec/subtype/packet/expected gates do not alter default input admission");
    }
}

template<class Check> static void RuntimeInventorySelfTest(const Check &check)
{
    const BC_STATUS errors[] = {BC_STS_ERROR, BC_STS_IO_ERROR, BC_STS_FW_CMD_ERR,
        BC_STS_TIMEOUT, BC_STS_INV_ARG, BC_STS_BUSY, BC_STS_ERR_USAGE};
    MfdAdmissionObserver complete;
    complete.enabled = true; complete.attempted = 3; complete.reads = 40;
    for (unsigned scenario = 0; scenario < 11; ++scenario) {
        RuntimeInventoryFixture fixture;
        RuntimeInventoryFixture other_owner;
        if (scenario < 7) fixture.status = errors[scenario];
        fixture.lose_owner = scenario == 7 || scenario == 8;
        if (scenario == 8) fixture.replacement = &other_owner;
        if (scenario == 9) std::memset(fixture.raw, 0, sizeof(fixture.raw));
        if (scenario == 10) fixture.raw[1] = 0xffffffffU;
        RuntimeInventoryObserver observer; observer.enabled = true;
        const bool ok = observer.Observe(&fixture.current, RuntimeInventoryFixture::Query, false);
        check(ok == (scenario >= 9) && fixture.valid && fixture.queries == 1 && observer.queries == 1 &&
            observer.measured == ok && observer.failed == !ok &&
            (ok ? !std::memcmp(observer.raw, fixture.raw, sizeof(fixture.raw)) :
                observer.raw[0] == 0 && observer.raw[1] == 0 && observer.raw[2] == 0),
            "runtime inventory publishes only a successful current-owner reply; opaque words are not capability proof");
        check(observer.Finish(true, complete, false) == ok && !observer.Finish(false, complete, false),
            "runtime inventory finish requires native recovery/delivery as well as all diagnostic stages");
        auto incomplete = complete; incomplete.reads = 39;
        check(!observer.Finish(true, incomplete, false), "runtime inventory requires all forty existing MFD/SCL reads");
        for (unsigned field = 0; field < 3; ++field) {
            incomplete = complete;
            if (field == 0) incomplete.failed = true;
            if (field == 1) incomplete.attempted = 1;
            if (field == 2) incomplete.enabled = false;
            check(!observer.Finish(true, incomplete, false), "runtime inventory cannot erase MFD/SCL failure or missing stage");
        }
        RuntimeInventoryFixture other;
        check(!observer.Observe(&fixture.current, RuntimeInventoryFixture::Query, false) &&
            !observer.Observe(&other.current, RuntimeInventoryFixture::Query, false) &&
            fixture.queries == 1 && other.queries == 0 && !observer.Finish(true, complete, false),
            "runtime inventory duplicate or failure is sticky; no later query on original or changed owner");
    }
    for (unsigned argument = 0; argument < 3; ++argument) {
        RuntimeInventoryFixture fixture;
        if (argument == 1) fixture.current = nullptr;
        RuntimeInventoryObserver observer; observer.enabled = true;
        check(!observer.Observe(argument == 0 ? nullptr : &fixture.current,
            argument == 2 ? nullptr : RuntimeInventoryFixture::Query, false) && observer.failed &&
            observer.attempted && !observer.queries && !fixture.queries,
            "runtime inventory validates handle and query before device action");
        fixture.current = &fixture;
        check(!observer.Observe(&fixture.current, RuntimeInventoryFixture::Query, false) && !fixture.queries,
            "runtime inventory cannot retry rejected arguments");
    }
    RuntimeInventoryObserver disabled;
    check(disabled.Observe(nullptr, nullptr, false) && disabled.Finish(true, complete, false) &&
        !disabled.Finish(false, complete, false) && !disabled.attempted && !disabled.queries,
        "runtime inventory is default-off and does not alter ordinary native success/failure");
    for (unsigned scenario = 0; scenario < 3; ++scenario) {
        RuntimeInventoryFixture fixture;
        if (scenario == 1) fixture.status = BC_STS_IO_ERROR;
        if (scenario == 2) fixture.lose_owner = true;
        if (scenario) for (auto &raw : fixture.raw) raw = 0xdeadbeefU;
        RuntimeInventoryObserver observer; observer.enabled = true;
        FILE *record = std::tmpfile();
        std::fflush(stdout);
        const int saved_stdout = dup(STDOUT_FILENO);
        const bool redirected = record && saved_stdout >= 0 && dup2(fileno(record), STDOUT_FILENO) >= 0;
        bool result = false;
        if (redirected) {
            result = observer.Observe(&fixture.current, RuntimeInventoryFixture::Query);
            std::fflush(stdout);
        }
        const bool restored = saved_stdout >= 0 && dup2(saved_stdout, STDOUT_FILENO) >= 0;
        if (saved_stdout >= 0) close(saved_stdout);
        char text[2048] = {}; size_t bytes = 0;
        if (record) { std::rewind(record); bytes = std::fread(text, 1, sizeof(text) - 1, record); std::fclose(record); }
        check(redirected && restored && bytes && fixture.valid && fixture.queries == 1 &&
            result == (scenario == 0) && !std::strstr(text, "deadbeef") &&
            std::strstr(text, "source=firmware-reported-GET_VERSION passive=no target-config-writes=0") &&
            std::strstr(text, "extra-download/INIT=0 reply-header-match/transport-certified=no") &&
            (scenario == 0 ? (std::strstr(text, "result=PASS") && std::strstr(text, "stream-sw=01360000") &&
                std::strstr(text, "decoder-sw=02030004") && std::strstr(text, "chip-hw=00007015")) :
                (std::strstr(text, "result=FAIL") && std::strstr(text, "stream-sw=NOT-READ") &&
                 std::strstr(text, "decoder-sw=NOT-READ") && std::strstr(text, "chip-hw=NOT-READ"))),
            "runtime inventory actual report distinguishes query provenance and suppresses failed/owner-lost reply poison");
    }
    const std::vector<const char *> valid = {"probe", "--hardware", "fixture", "180", "30", "1",
        "--scaler-test", "0", "--observe-runtime-inventory", "--capture-yuy2", "new"};
    Options admitted;
    check(ParseArguments(valid, &admitted) && admitted.observe_runtime_inventory && NeedsRawIo(admitted),
        "runtime inventory uses exact opt-in native capture profile with capability gate before any action");
    for (unsigned field = 0; field < 12; ++field) {
        auto arguments = valid;
        if (field == 0) arguments[1] = "--preflight";
        if (field == 1) arguments[3] = "179";
        if (field == 2) arguments[5] = "2";
        if (field == 3) arguments[7] = "320";
        if (field == 4) arguments[9] = "--capture-uyvy";
        if (field == 5) arguments.resize(9);
        if (field == 6) arguments[10] = "";
        if (field == 7) arguments[10] = "-";
        if (field == 8) arguments.erase(arguments.begin() + 6, arguments.begin() + 8);
        if (field == 9) std::swap(arguments[8], arguments[9]);
        if (field == 10) arguments.resize(10);
        if (field == 11) arguments.insert(arguments.end(), {"--capture-yuy2", "another"});
        Options rejected;
        check(!ParseArguments(arguments, &rejected), "runtime inventory rejects incompatible mode/count/scaling/capture before action");
    }
    for (const auto &mixed : std::vector<std::vector<const char *>>{
            {"--observe-chroma"}, {"--observe-scl-config"}, {"--observe-scl-filter-map"},
            {"--observe-scl-view", "2"}, {"--observe-scl-view", "3"}, {"--observe-mfd-config"},
            {"--observe-mfd-address"}, {"--observe-runtime-inventory"}, {"--inject-mfd-colour", "a"},
            {"--inject-mfd-colour", "b"}, {"--scl-status-test", "observe"}, {"--scl-status-test", "clear"},
            {"--mpeg1-via-mpeg2"}, {"--h263-via-divx"}, {"--open-only"}}) {
        for (unsigned where : {8U, 9U}) {
            auto arguments = valid; arguments.insert(arguments.begin() + where, mixed.begin(), mixed.end());
            Options rejected;
            check(!ParseArguments(arguments, &rejected), "runtime inventory cannot combine or duplicate diagnostic/hidden-codec options");
        }
    }
    Options rejected;
    check(!ParseArguments({"probe", "--self-test", "--observe-runtime-inventory"}, &rejected),
        "runtime inventory cannot access hardware in self-test mode");
    Input native; native.codec = AV_CODEC_ID_MPEG2VIDEO; native.subtype = BC_MSUBTYPE_MPEG2VIDEO;
    native.progressive = true; native.width = 640; native.height = 360; native.packets.resize(180);
    check(SclInputAdmitted(admitted, native), "runtime inventory exact native fixture admitted");
    for (unsigned field = 0; field < 8; ++field) {
        Options changed = admitted; Input altered = native;
        if (field == 0) altered.codec = AV_CODEC_ID_H264;
        if (field == 1) altered.subtype = BC_MSUBTYPE_H264;
        if (field == 2) altered.progressive = false;
        if (field == 3) altered.width = 638;
        if (field == 4) altered.height = 358;
        if (field == 5) altered.packets.pop_back();
        if (field == 6) altered.packets.emplace_back();
        if (field == 7) changed.expected = 179;
        check(!SclInputAdmitted(changed, altered) && SclInputAdmitted(Options{}, altered),
            "runtime inventory cannot broaden native fixture admission or change default behavior");
    }
}

template<class Check> static void PpbStopLifecycleSelfTest(const Check &check);

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

    SclFilterMapSelfTest(check);
    MfdAddressSelfTest(check);
    ArmMetadataSelfTest(check);
    ArmSourceShapeSelfTest(check);
    PpbContextSelfTest(check);
    PpbStopContextSelfTest(check);
    PpbFixedMetadataSelfTest(check);
    PpbReturnHeaderSelfTest(check);
    VideoPrefixSelfTest(check);
    VideoGraphSelfTest(check);
    VideoStagingSelfTest(check);
    AvdMemorySelfTest(check);
    PpbStopLifecycleSelfTest(check);
    MfdFramingSelfTest(check);
    RuntimeInventorySelfTest(check);

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
    for (unsigned mode : {1U, 2U}) for (uint32_t pre : {0U, 1U, 5U}) {
        SclStatusFixture fixture(mode, pre); SclStatusTest subject; subject.mode = mode;
        check(fixture.Exercise(&subject) && fixture.valid && subject.stages == 7 && subject.reads == 18 &&
            subject.writes == (mode == 2 && pre ? 1U : 0U) && subject.pre_status == pre &&
            subject.diagnostic_failed == (pre != 0) && !subject.fatal && subject.Finish(true, false) == (pre == 0),
            "SCL status independent literal18R/optional1W oracle preserves sticky diagnostic failure and native success separation");
        for (unsigned position = 0; position < fixture.events.size(); ++position) {
            for (int code = -1; code <= BC_STS_PWR_MGMT; ++code) {
                if (code == BC_STS_SUCCESS) continue;
                SclStatusFixture failing(mode, pre); failing.fail_at = position; failing.status = static_cast<BC_STATUS>(code);
                struct { uint32_t before = 0x12345678; SclStatusTest value; uint32_t after = 0x87654321; } guarded;
                guarded.value.mode = mode;
                check(!failing.Exercise(&guarded.value) && failing.valid && failing.calls == position + 1 &&
                    guarded.value.fatal && guarded.value.failure == SclStatusFailure::Api && guarded.value.api_status == code &&
                    guarded.before == 0x12345678 && guarded.after == 0x87654321 &&
                    guarded.value.writes == failing.write_calls && guarded.value.clear_attempted == (failing.write_calls != 0),
                    "SCL status every19/18 API position and all27 failures stop exactly once, discard poison, preserve canaries/attempt status");
                check(!guarded.value.AfterDelivered(&failing, 180, true, true, SclStatusFixture::Read, SclStatusFixture::Write, false) &&
                    !guarded.value.Eos(&failing, true, SclStatusFixture::Read, false) &&
                    !guarded.value.Sample(&failing, 2, SclStatusFixture::Read, false) && failing.calls == position + 1 &&
                    !guarded.value.Finish(true, false), "SCL status access loss forbids all later probe reads/writes, cleanup probes and retries");
            }
            if (fixture.events[position].write) continue;
            const bool status = fixture.events[position].address == 0x5408a4;
            for (unsigned bit = status ? 8 : 16; bit < 32; ++bit) {
                SclStatusFixture invalid(mode, pre); invalid.events[position].raw |= 1U << bit;
                SclStatusTest value; value.mode = mode;
                check(!invalid.Exercise(&value) && invalid.valid && invalid.calls == position + 1 &&
                    value.failure == SclStatusFailure::Reserved && value.fatal &&
                    !value.Eos(&invalid, true, SclStatusFixture::Read, false) && invalid.calls == position + 1,
                    "SCL status every reserved bit at every read position is fatal, without additional custom I/O");
            }
            if (!status) for (uint32_t revision : {0U, 0x7fU, 0x81U, 0xffffU}) {
                SclStatusFixture invalid(mode, pre); invalid.events[position].raw = revision;
                SclStatusTest value; value.mode = mode;
                check(!invalid.Exercise(&value) && invalid.valid && invalid.calls == position + 1 &&
                    value.failure == SclStatusFailure::Revision, "SCL status every opening/closing observed-board revision is pinned80");
            }
            if (status) for (unsigned bit = 0; bit < 8; ++bit) {
                if ((pre & (1U << bit)) && position >= 6) continue;
                if (position < 6 && (1U << bit) == 1) continue;
                SclStatusFixture invalid(mode, pre); invalid.events[position].raw = 1U << bit;
                SclStatusTest value; value.mode = mode;
                check(!invalid.Exercise(&value) && invalid.valid && invalid.calls == position + 1 &&
                    value.failure == SclStatusFailure::Profile && value.api_status == BC_STS_SUCCESS,
                    "SCL status unsupported pre bits or newly unseeded post/EOS bits are profile loss, not invented transport errors");
            }
        }
        if (pre) {
            SclStatusFixture mixed(mode, pre); const unsigned offset = mode == 2 ? 1 : 0;
            mixed.events[7 + offset].raw = pre; mixed.events[10 + offset].raw = 0;
            mixed.events[13 + offset].raw = pre == 5 ? 4 : 0; mixed.events[16 + offset].raw = pre;
            SclStatusTest value; value.mode = mode;
            check(mixed.Exercise(&value) && mixed.valid && !value.fatal && value.pre_status == pre &&
                value.diagnostic_failed && !value.Finish(true, false),
                "SCL post/EOS subsets may differ; pre bitmask immutable and original diagnosticFAIL cannot be absolved by zero");
        }
        for (uint32_t immediate0 : {0U, 1U, 4U, 5U}) for (uint32_t immediate1 : {0U, 1U, 4U, 5U})
            for (uint32_t eos0 : {0U, 1U, 4U, 5U}) for (uint32_t eos1 : {0U, 1U, 4U, 5U}) {
                if ((immediate0 | immediate1 | eos0 | eos1) & ~pre) continue;
                SclStatusFixture mixed(mode, pre); const unsigned offset = mode == 2 && pre ? 1 : 0;
                mixed.events[7 + offset].raw = immediate0; mixed.events[10 + offset].raw = immediate1;
                mixed.events[13 + offset].raw = eos0; mixed.events[16 + offset].raw = eos1;
                SclStatusTest value; value.mode = mode;
                check(mixed.Exercise(&value) && mixed.valid && !value.fatal && value.pre_status == pre &&
                    value.Finish(true, false) == (pre == 0), "Every allowed immediate/EOS subset combination remains numeric data, never an error-absolution rule");
            }
    }
    for (uint32_t first : {0U, 1U, 5U}) for (uint32_t second : {0U, 1U, 5U}) {
        if (first == second) continue;
        SclStatusFixture unstable(2, first); unstable.events[4].raw = second; SclStatusTest value; value.mode = 2;
        check(!unstable.Exercise(&value) && unstable.valid && unstable.calls == 6 && !unstable.write_calls &&
            value.failure == SclStatusFailure::Unstable && value.fatal,
            "SCL status pre repeat mismatch rejects after complete bracket, before any clear write");
    }
    for (uint32_t pre : {2U, 3U, 4U, 6U, 7U, 8U, 0xffU}) {
        SclStatusFixture invalid(2, pre); SclStatusTest value; value.mode = 2;
        check(!invalid.Exercise(&value) && invalid.calls == 2 && !invalid.write_calls &&
            value.failure == SclStatusFailure::Profile, "SCL pre profile admits exactly0/1/5, never a cached or arbitrary clear mask");
    }
    {
        SclStatusFixture fixture; SclStatusTest value; value.mode = 2; fixture.probe = &value;
        for (unsigned frame : {0U, 1U, 90U, 179U, 181U})
            check(value.AfterDelivered(&fixture, frame, true, true, SclStatusFixture::Read, SclStatusFixture::Write, false) && !fixture.calls,
                "SCL status no nonlast/FMT/EOS-marker frame attempts");
        for (const auto &barrier : {std::pair<bool, bool>{false, false}, {false, true}, {true, false}}) {
            SclStatusFixture invalid; SclStatusTest rejected; rejected.mode = 2; invalid.probe = &rejected;
            check(!rejected.AfterDelivered(&invalid, 180, barrier.first, barrier.second, SclStatusFixture::Read, SclStatusFixture::Write, false) &&
                !invalid.calls && !rejected.stages && rejected.fatal &&
                !rejected.AfterDelivered(&invalid, 180, true, true, SclStatusFixture::Read, SclStatusFixture::Write, false) && !invalid.calls,
                "SCL final output release/ownedWrite loss is sticky before admission, with no later retry on that probe");
        }
        check(!value.Eos(&fixture, false, SclStatusFixture::Read, false) && !fixture.calls,
            "SCL incomplete native barrier cannot fabricate an EOS snapshot");
        check(value.AfterDelivered(&fixture, 180, true, true, SclStatusFixture::Read, SclStatusFixture::Write, false) && fixture.calls == 13 &&
            !value.Eos(&fixture, false, SclStatusFixture::Read, false) && fixture.calls == 13 && value.stages == 3,
            "SCL status ordinary deadline/input/output failure after clear causes no EOS or cleanup probe I/O");
        SclStatusFixture other;
        check(!value.Eos(&other, true, SclStatusFixture::Read, false) && !other.calls && fixture.calls == 13 && value.fatal,
            "SCL status wrong-handle EOS refuses before hardware and latches all future custom I/O");
        check(!value.Eos(&fixture, true, SclStatusFixture::Read, false) && fixture.calls == 13, "SCL status owner loss never retries original owner");
    }
    for (unsigned phase : {0U, 1U, 2U}) {
        SclStatusFixture fixture; SclStatusTest value; value.mode = 2; fixture.probe = &value;
        if (phase) check(value.AfterDelivered(&fixture, 180, true, true, SclStatusFixture::Read, SclStatusFixture::Write, false), "SCL order test setup");
        if (phase == 2) check(value.Eos(&fixture, true, SclStatusFixture::Read, false), "SCL completed EOS test setup");
        const unsigned calls = fixture.calls;
        check(!(phase ? value.AfterDelivered(&fixture, 180, true, true, SclStatusFixture::Read, SclStatusFixture::Write, false) :
            value.Eos(&fixture, true, SclStatusFixture::Read, false)) && fixture.calls == calls && value.fatal,
            "SCL status wrong phase/repeated last/repeated completed observation refuses without repeat clear");
    }
    {
        SclStatusFixture fixture; SclStatusTest value; value.mode = 2;
        check(fixture.Exercise(&value) && !value.Eos(&fixture, true, SclStatusFixture::Read, false) && fixture.calls == 19 && value.fatal,
            "SCL status repeated actual EOS refuses without further reads");
        for (unsigned invalid : {0U, 1U, 2U}) {
            SclStatusFixture untouched; SclStatusTest rejected; rejected.mode = 2; untouched.probe = &rejected;
            check(!rejected.AfterDelivered(invalid == 0 ? nullptr : &untouched, 180, true, true,
                invalid == 1 ? nullptr : SclStatusFixture::Read,
                invalid == 2 ? nullptr : SclStatusFixture::Write, false) &&
                !untouched.calls && rejected.fatal,
                "SCL status null handle/reader/writer rejects before effects");
        }
    }
    {
        SclStatusTest disabled;
        check(disabled.AfterDelivered(nullptr, 180, false, false, nullptr, nullptr, false) &&
            disabled.Eos(nullptr, false, nullptr, false) && disabled.Sample(nullptr, 99, nullptr, false) &&
            disabled.Finish(true, false) && !disabled.Finish(false, false) && !disabled.reads && !disabled.writes,
            "Default SCL status path is a pure no-op and preserves native result");
        for (unsigned mode : {3U, UINT_MAX}) {
            SclStatusFixture fixture; SclStatusTest invalid; invalid.mode = mode;
            check(!invalid.AfterDelivered(&fixture, 180, true, true, SclStatusFixture::Read, SclStatusFixture::Write, false) && !fixture.calls,
                "SCL status invalid modes refuse before any effect");
        }
    }
    for (unsigned position = 0; position <= 19; ++position) {
        FILE *record = std::tmpfile(); const int saved_stdout = dup(STDOUT_FILENO); std::fflush(stdout);
        const bool redirected = record && saved_stdout >= 0 && dup2(fileno(record), STDOUT_FILENO) >= 0;
        bool accepted = false;
        if (redirected) {
            SclStatusFixture fixture; fixture.fail_at = position; SclStatusTest value; value.mode = 2;
            accepted = fixture.Exercise(&value, true);
            (void)value.Finish(true); std::fflush(stdout);
        }
        const bool restored = saved_stdout >= 0 && dup2(saved_stdout, STDOUT_FILENO) >= 0;
        if (saved_stdout >= 0) close(saved_stdout);
        char text[8192] = {}; size_t bytes = 0;
        if (record) { std::rewind(record); bytes = std::fread(text, 1, sizeof(text) - 1, record); std::fclose(record); }
        check(redirected && restored && bytes && !std::strstr(text, "deadbeef") &&
            std::strstr(text, "native-full-delivery/EOS/capture/cleanup=PASS diagnostic=FAIL aggregate=FAIL") &&
            (position == 19 ? accepted : !accepted) && (position == 6 || position == 19 || std::strstr(text, "NOT-READ")),
            "SCL actual raw reporter never prints poison/unread zeros and separates complete native success from sticky diagnosticFAIL");
    }
    for (const char *name : {"observe", "clear"}) {
        const std::vector<const char *> valid = {"probe", "--hardware", "fixture", "180", "30", "1",
            "--scaler-test", "320", "--scl-status-test", name, "--capture-yuy2", "new"};
        Options admitted;
        check(ParseArguments(valid, &admitted) && admitted.scl_status_test == (!std::strcmp(name, "observe") ? 1U : 2U) && NeedsRawIo(admitted),
            "SCL status literal observe/clear native320 YUY2 onecapture admission requires CAP before fixture/progress/device");
        for (const char *invalid : {"", "0", "1", "2", "Observe", "CLEAR", "all", "5", " clear"}) {
            auto arguments = valid; arguments[9] = invalid; Options rejected;
            check(!ParseArguments(arguments, &rejected), "SCL status no arbitrary mask/alternate mode spelling");
        }
        for (unsigned field = 0; field < 10; ++field) {
            auto arguments = valid;
            if (field == 0) arguments[1] = "--preflight";
            if (field == 1) arguments[3] = "179";
            if (field == 2) arguments[5] = "2";
            if (field == 3) arguments[7] = "0";
            if (field == 4) arguments[7] = "640";
            if (field == 5) arguments[10] = "--capture-uyvy";
            if (field == 6) arguments.resize(10);
            if (field == 7) arguments.insert(arguments.begin() + 8, "--mpeg1-via-mpeg2");
            if (field == 8) arguments.insert(arguments.begin() + 8, "--h263-via-divx");
            if (field == 9) arguments.insert(arguments.begin() + 8, "--open-only");
            Options rejected; check(!ParseArguments(arguments, &rejected), "SCL status strict320/native/hardware180/oneYUY2capture scope only");
        }
        for (const std::vector<const char *> &mixed : std::vector<std::vector<const char *>>{
                {"--observe-chroma"}, {"--observe-scl-config"}, {"--observe-scl-view", "2"},
                {"--observe-mfd-config"}, {"--inject-mfd-colour", "a"}, {"--scl-status-test", "observe"}})
            for (unsigned where : {8U, 10U}) {
                auto arguments = valid; arguments.insert(arguments.begin() + where, mixed.begin(), mixed.end()); Options rejected;
                check(!ParseArguments(arguments, &rejected), "SCL status excludes every other observer/injection, both orders and duplicate mode");
            }
        Options rejected;
        check(!ParseArguments({"probe", "--self-test", "--scl-status-test", name}, &rejected), "SCL status self-test mix rejects");
        Input native; native.codec = AV_CODEC_ID_MPEG2VIDEO; native.subtype = BC_MSUBTYPE_MPEG2VIDEO;
        native.progressive = true; native.width = 640; native.height = 360; native.packets.resize(180);
        check(SclInputAdmitted(admitted, native), "SCL status exact native input shape admitted before capture/device");
        for (unsigned field = 0; field < 7; ++field) {
            Input changed = native; Options options = admitted;
            if (field == 0) changed.codec = AV_CODEC_ID_H264;
            if (field == 1) changed.subtype = BC_MSUBTYPE_H264;
            if (field == 2) changed.progressive = false;
            if (field == 3) changed.width = 638;
            if (field == 4) changed.height = 358;
            if (field == 5) changed.packets.pop_back();
            if (field == 6) options.expected = 179;
            check(!SclInputAdmitted(options, changed) && SclInputAdmitted(Options{}, changed), "SCL status input refusal leaves default admission unchanged");
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
    BC_STATUS Stop(BC_STATUS (*stopper)(HANDLE) = DtsStopDecoder) {
        if (!started) return BC_STS_SUCCESS;
        if (!handle || !stopper) return BC_STS_INV_ARG;
        // Match ordinary Close: even a failed STOP is consumed once. The
        // optional observation must not add a second STOP during cleanup.
        started = false;
        return stopper(handle);
    }
    bool Close() {
        bool ok = true;
        const auto record = [&](const char *operation, BC_STATUS status) {
            if (status != BC_STS_SUCCESS) {
                std::fprintf(stderr, "%s failed: %d\n", operation, status);
                ok = false;
            }
        };
        if (started) record("DtsStopDecoder", Stop());
        started = false;
        if (opened) record("DtsCloseDecoder", DtsCloseDecoder(handle));
        opened = false;
        if (handle) record("DtsDeviceClose", DtsDeviceClose(handle));
        handle = nullptr;
        return ok;
    }
    ~Device() { if (handle) Close(); }
};

template<class Check> static void PpbStopLifecycleSelfTest(const Check &check)
{
    struct Fixture {
        unsigned calls = 0;
        BC_STATUS result = BC_STS_SUCCESS;
        static BC_STATUS Stop(HANDLE handle) { auto *f = static_cast<Fixture *>(handle); ++f->calls; return f->result; }
    };
    for (BC_STATUS result : {BC_STS_SUCCESS, BC_STS_BUSY, BC_STS_ERROR}) {
        Fixture fixture; fixture.result = result;
        Device device; device.handle = &fixture; device.started = true;
        check(device.Stop(Fixture::Stop) == result && fixture.calls == 1 && !device.started &&
            device.Stop(Fixture::Stop) == BC_STS_SUCCESS && fixture.calls == 1,
            "PPB explicit host STOP forwards success/failure once without retry or an implicit ARC ACK");
        device.handle = nullptr;
        check(device.Close() && fixture.calls == 1, "PPB STOP consumption leaves ordinary cleanup no duplicate STOP");
    }
    Fixture fixture; Device idle; idle.handle = &fixture;
    check(idle.Stop(Fixture::Stop) == BC_STS_SUCCESS && !fixture.calls, "PPB unopened/idle STOP helper adds no call");
    idle.started = true;
    check(idle.Stop(nullptr) == BC_STS_INV_ARG && idle.started && !fixture.calls,
        "PPB malformed STOP callback is refused before consuming the started state");
    idle.started = false; idle.handle = nullptr;
}

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
    SclFilterMapObserver scl_filter_map;
    SclViewProbe scl_view;
    MfdAdmissionObserver mfd;
    RuntimeInventoryObserver runtime_inventory;
    ArmMetadataObserver arm_metadata;
    PpbContextObserver ppb_context;
    AvdMemoryObserver avd_memory;
    MfdAddressObserver mfd_address;
    MfdFramingObserver mfd_framing;
    MfdColourProbe mfd_colour;
    SclStatusTest scl_status;
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
            bool owned_written = false;
            // File I/O uses only the owned copy, after the lease was released.
            if (!marker && valid && released && audit->capture) {
                owned_written = audit->capture->Write(captured);
                if (!owned_written) {
                    std::fprintf(stderr, "%s capture write/budget failure\n", PackedName(audit->output_format));
                    valid = false;
                }
            }
            // Preserve an already delivered owned copy before diagnostic failure.
            if (!marker && valid && released && audit->frames == 1)
                valid = audit->arm_metadata.Observe(&device->handle, 1, released && owned_written);
            if (!marker && valid && released && audit->frames == 1)
                valid = audit->ppb_context.Observe(&device->handle, 1, released && owned_written);
            if (!marker && valid && released && audit->frames == 1)
                valid = audit->avd_memory.Observe(&device->handle, 1, released && owned_written);
            if (!marker && valid && released)
                valid = audit->scl_filter_map.AfterDelivered(device->handle, audit->frames,
                                                           released, audit->capture != nullptr);
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
                valid = audit->mfd_address.AfterDelivered(&device->handle, audit->frames,
                                                         released, owned_written);
            if (!marker && valid && released)
                valid = audit->mfd_framing.AfterDelivered(&device->handle, audit->frames,
                                                         released, owned_written);
            if (!marker && valid && released)
                valid = audit->mfd_colour.AfterDelivered(device->handle, audit->frames,
                                                        released, audit->capture != nullptr);
            if (!marker && valid && released)
                valid = audit->scl_status.AfterDelivered(device->handle, audit->frames,
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
                if (audit->scl_filter_map.fatal)
                    std::fprintf(stderr, "SCL filter map lost admission; no further map I/O, ordinary decoder cleanup follows\n");
                else if (audit->scl_status.fatal)
                    std::fprintf(stderr, "SCL status test lost admission; no further probe I/O, ordinary decoder cleanup follows\n");
                else if (audit->mfd_colour.failed)
                    std::fprintf(stderr, "MFD colour experiment failed; guarded restoration if eligible and ordinary decoder cleanup follow\n");
                else if (audit->mfd.failed)
                    std::fprintf(stderr, "MFD passive admission failed; ordinary decoder cleanup follows\n");
                else if (audit->mfd_address.failed)
                    std::fprintf(stderr, "MFD debug address observation failed; no further observer I/O, ordinary decoder cleanup follows\n");
                else if (audit->mfd_framing.failed)
                    std::fprintf(stderr, "MFD framing observation failed; no further observer I/O, ordinary decoder cleanup follows\n");
                else if (audit->avd_memory.failed)
                    std::fprintf(stderr, "AVD memory inventory failed; no further observer I/O or error clearing, ordinary decoder cleanup follows\n");
                else if (audit->ppb_context.failed)
                    std::fprintf(stderr, "PPB saved context observation failed; no new pointer targets or retries, ordinary decoder cleanup follows\n");
                else if (audit->arm_metadata.failed)
                    std::fprintf(stderr, "ARM metadata observation failed; no further observer I/O, ordinary decoder cleanup follows\n");
                else if (audit->scl_view.failed)
                    std::fprintf(stderr, "SCL test-view experiment failed; guarded restoration if eligible and ordinary decoder cleanup follow\n");
                else if (audit->scl.failed)
                    std::fprintf(stderr, "SCL raw configuration observation failed; ordinary decoder cleanup follows\n");
                else
                    std::fprintf(stderr, "Invalid progressive picture geometry/data/token: %llu\n",
                                 static_cast<unsigned long long>(output.PicInfo.timeStamp));
                if (audit->pixels && !audit->avd_memory.failed && !audit->scl.failed && !audit->scl_filter_map.fatal && !audit->scl_view.failed && !audit->mfd.failed && !audit->mfd_address.failed && !audit->mfd_framing.failed && !audit->arm_metadata.failed && !audit->ppb_context.failed && !audit->mfd_colour.failed && !audit->scl_status.fatal)
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
    audit.scl_filter_map.enabled = options.observe_scl_filter_map;
    audit.scl_view.selector = options.observe_scl_view;
    audit.mfd.enabled = options.observe_mfd_config || options.observe_runtime_inventory;
    audit.runtime_inventory.enabled = options.observe_runtime_inventory;
    audit.arm_metadata.enabled = options.observe_arm_metadata || options.observe_arm_source_shape;
    audit.arm_metadata.source_shape = options.observe_arm_source_shape;
    audit.ppb_context.enabled = options.observe_ppb_context;
    audit.ppb_context.post_stop = options.observe_ppb_stop;
    audit.ppb_context.metadata_pool = options.observe_ppb_metadata;
    audit.ppb_context.return_header = options.observe_ppb_return;
    audit.ppb_context.video_prefix = options.observe_video_prefix;
    audit.ppb_context.video_graph = options.observe_video_graph;
    audit.ppb_context.video_staging = options.observe_video_staging;
    audit.avd_memory.enabled = options.observe_avd_memory;
    audit.avd_memory.conflicting_mode = AvdMemoryConflicts(options);
    audit.mfd_address.enabled = options.observe_mfd_address;
    audit.mfd_framing.enabled = options.observe_mfd_framing;
    audit.mfd_colour.stimulus = options.inject_mfd_colour;
    audit.scl_status.mode = options.scl_status_test;
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
    if (ok && audit.scl_filter_map.enabled) audit.scl_filter_map.owner = device.handle;
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
    if (ok) ok = audit.runtime_inventory.Observe(&device.handle);
    if (ok) ok = audit.mfd.Observe(device.handle, 0);
    if (ok) ok = audit.mfd_address.PreStart(&device.handle);
    if (ok) ok = audit.mfd_framing.PreStart(&device.handle);
    if (ok) ok = audit.arm_metadata.Observe(&device.handle, 0, false);
    if (ok) ok = audit.ppb_context.Observe(&device.handle, 0, false);
    if (ok) ok = audit.avd_memory.Observe(&device.handle, 0, false);
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
    if (ok) ok = audit.arm_metadata.Observe(&device.handle, 2, true);
    if (ok) ok = audit.ppb_context.Observe(&device.handle, 2, true);
    if (ok) ok = audit.avd_memory.Observe(&device.handle, 2, true);
    if (ok) ok = audit.scl.Observe(device.handle, SclStage::EosBarrier);
    // Only an actual native delivery barrier admits the last status sample.
    // Its diagnostic result cannot retroactively erase delivered native data.
    if (ok) (void)audit.scl_status.Eos(device.handle, true);
    // A failed experiment remains failed even if guarded restoration succeeds.
    // Never issue cleanup experiment I/O after an access/selector-loss latch.
    const bool view_restored = audit.scl_view.Restore(device.handle);
    const bool colour_restored = audit.mfd_colour.Restore(device.handle);
    ok = ok && view_restored && colour_restored;
    if (ok && audit.ppb_context.post_stop) {
        // ARM can discard internal STOP failures. This barrier means only that
        // the host API returned success; neither refresh nor ARC ACK is proven.
        ok = !deadline.expired() && device.handle && device.opened && device.started;
        if (ok) {
            const BC_STATUS stopped = device.Stop();
            std::printf("PPB host STOP: api-status=%d inner-ARC-completion=unproven saved-refresh=unproven\n", stopped);
            ok = Status("DtsStopDecoder", stopped);
            if (ok) ok = !deadline.expired() && audit.ppb_context.Observe(&device.handle, 3, true);
        }
    }
    const bool closed = device.Close();
    ok = ok && closed;
    const bool captured = capture.Finish(ok);
    ok = ok && captured;
    ok = audit.scl_filter_map.Finish(ok);
    ok = audit.scl_status.Finish(ok);
    ok = audit.mfd_address.Finish(ok);
    ok = audit.mfd_framing.Finish(ok);
    ok = audit.arm_metadata.Finish(ok);
    ok = audit.ppb_context.Finish(ok);
    ok = audit.avd_memory.Finish(ok);
    ok = audit.runtime_inventory.Finish(ok, audit.mfd);
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
            "[--open-only] [--observe-chroma | --observe-scl-config | --observe-scl-filter-map | --observe-scl-view 2_OR_3 | --observe-mfd-config | --observe-mfd-address | --observe-mfd-framing | --observe-runtime-inventory | --observe-arm-metadata | --observe-arm-source-shape | --observe-ppb-context | --observe-ppb-stop | --observe-ppb-metadata | --observe-ppb-return | --observe-video-prefix | --observe-video-graph | --observe-video-staging | --observe-avd-memory | --inject-mfd-colour a_OR_b | --scl-status-test observe_OR_clear] "
            "[--capture-yuy2 NEW_PATH | --capture-uyvy NEW_PATH]\n", argv[0]);
        return 2;
    }
    if (options.mode == Mode::SelfTest) return SelfTest() ? 0 : 1;
    if (NeedsRawIo(options) && !CanReadChromaConfiguration()) {
        if (options.observe_avd_memory)
            std::fprintf(stderr, "--observe-avd-memory requires CAP_SYS_RAWIO; no fixture/progress/capture/device was opened\n");
        else if (options.observe_video_staging)
            std::fprintf(stderr, "--observe-video-staging requires CAP_SYS_RAWIO; no fixture/progress/capture/device was opened\n");
        else if (options.observe_video_graph)
            std::fprintf(stderr, "--observe-video-graph requires CAP_SYS_RAWIO; no fixture/progress/capture/device was opened\n");
        else if (options.observe_video_prefix)
            std::fprintf(stderr, "--observe-video-prefix requires CAP_SYS_RAWIO; no fixture/progress/capture/device was opened\n");
        else if (options.observe_ppb_return)
            std::fprintf(stderr, "--observe-ppb-return requires CAP_SYS_RAWIO; no fixture/progress/capture/device was opened\n");
        else if (options.observe_ppb_metadata)
            std::fprintf(stderr, "--observe-ppb-metadata requires CAP_SYS_RAWIO; no fixture/progress/capture/device was opened\n");
        else if (options.observe_ppb_stop)
            std::fprintf(stderr, "--observe-ppb-stop requires CAP_SYS_RAWIO; no fixture/progress/capture/device was opened\n");
        else if (options.observe_ppb_context)
            std::fprintf(stderr, "--observe-ppb-context requires CAP_SYS_RAWIO; no fixture/progress/capture/device was opened\n");
        else if (options.observe_arm_source_shape)
            std::fprintf(stderr, "--observe-arm-source-shape requires CAP_SYS_RAWIO; no fixture/progress/capture/device was opened\n");
        else if (options.observe_arm_metadata)
            std::fprintf(stderr, "--observe-arm-metadata requires CAP_SYS_RAWIO; no fixture/progress/capture/device was opened\n");
        else if (options.observe_runtime_inventory)
            std::fprintf(stderr, "--observe-runtime-inventory requires CAP_SYS_RAWIO; no fixture/progress/capture/device was opened\n");
        else if (options.observe_mfd_framing)
            std::fprintf(stderr, "--observe-mfd-framing requires CAP_SYS_RAWIO; no fixture/progress/capture/device was opened\n");
        else if (options.observe_mfd_address)
            std::fprintf(stderr, "--observe-mfd-address requires CAP_SYS_RAWIO; no fixture/progress/capture/device was opened\n");
        else if (options.observe_scl_filter_map)
            std::fprintf(stderr, "--observe-scl-filter-map requires CAP_SYS_RAWIO; no fixture/progress/capture/device was opened\n");
        else if (options.scl_status_test)
            std::fprintf(stderr, "--scl-status-test requires CAP_SYS_RAWIO; no fixture/progress/capture/device was opened\n");
        else if (options.inject_mfd_colour)
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
    if (!ArmMetadataInputAdmitted(options, input)) {
        std::fprintf(stderr, "ARM metadata observation requires the pinned progressive H264 256x96/180-packet input: "
            "124832 submitted bytes and fixed SHA256; no capture or device was opened\n");
        phase1_progress_close(&progress);
        return 2;
    }
    if (!AvdMemoryInputAdmitted(options, input)) {
        std::fprintf(stderr, "AVD memory inventory requires the pinned progressive H264 256x96/180-packet input: "
            "124832 submitted bytes and fixed SHA256; no capture or device was opened\n");
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
