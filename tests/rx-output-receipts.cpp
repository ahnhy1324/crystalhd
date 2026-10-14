/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "rx-output-receipts.h"

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <inttypes.h>
#include <limits>
#include <new>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "bc_dts_types.h"
#include "bc_dts_defs.h"

namespace rx_output_receipts {
namespace {

const char kResearchDevice[] = "/dev/crystalhd-fw-research";
const uint32_t kRequiredStages =
    CRYSTALHD_FW_RESEARCH_RX_STAGE_RESERVED |
    CRYSTALHD_FW_RESEARCH_RX_STAGE_FIRE_RETURN |
    CRYSTALHD_FW_RESEARCH_RX_STAGE_IRQ_ENTRY |
    CRYSTALHD_FW_RESEARCH_RX_STAGE_COMPLETE |
    CRYSTALHD_FW_RESEARCH_RX_STAGE_PARSED |
    CRYSTALHD_FW_RESEARCH_RX_STAGE_DISPOSITION;

int OpenDevice(void *, const char *path, int flags)
{
    return open(path, flags);
}

int IoctlDevice(void *, int fd, unsigned long request, void *argument)
{
    return ioctl(fd, request, argument);
}

int CloseDeviceFd(void *, int fd)
{
    return close(fd);
}

ssize_t WriteFile(void *, int fd, const void *buffer, size_t bytes)
{
    return write(fd, buffer, bytes);
}

bool WriteAll(int fd, const char *data, size_t bytes, const OutputOps &ops)
{
    while (bytes) {
        const ssize_t written = ops.write_file(ops.opaque, fd, data, bytes);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0 || static_cast<size_t>(written) > bytes) return false;
        data += written;
        bytes -= static_cast<size_t>(written);
    }
    return true;
}

bool DriverOutputRecord(const crystalhd_fw_research_rx_receipt &record,
                        uint64_t *normalized, Failure *failure)
{
    if (record.stage_flags & CRYSTALHD_FW_RESEARCH_RX_STAGE_CLOCK_INVERTED) {
        *failure = Failure::ClockInversion;
        return false;
    }
    if (record.disposition != CRYSTALHD_FW_RESEARCH_RX_DISPOSITION_OUTPUT)
        return false;
    if ((record.stage_flags & kRequiredStages) != kRequiredStages ||
        (record.stage_flags & CRYSTALHD_FW_RESEARCH_RX_STAGE_LIST_REPLACED) ||
        (record.stage_flags & CRYSTALHD_FW_RESEARCH_RX_STAGE_PARSE_FAILED) ||
        record.completion_status != BC_STS_SUCCESS ||
        !record.fire_return_ns || !record.irq_entry_ns ||
        !record.completion_ns || !record.parse_ns ||
        !record.disposition_ns ||
        record.fire_return_ns > record.irq_entry_ns ||
        record.irq_entry_ns > record.completion_ns ||
        record.completion_ns > record.parse_ns ||
        record.parse_ns > record.disposition_ns) {
        *failure = Failure::DriverRecord;
        return false;
    }
    if (!NormalizeFirmwareTimestamp(record.firmware_timestamp, normalized)) {
        *failure = Failure::Timestamp;
        return false;
    }
    return true;
}

bool MatchKey(const HostReceipt &host,
              const crystalhd_fw_research_rx_receipt &driver,
              uint64_t normalized)
{
    return host.token == normalized &&
        host.picture_number == driver.picture_number &&
        host.session_number == driver.session_number &&
        host.width == driver.picture_width &&
        host.height == driver.picture_height &&
        host.y_done_words == driver.y_done_words &&
        host.uv_done_words == driver.uv_done_words;
}

bool DriverEosControl(const crystalhd_fw_research_rx_receipt &driver)
{
    return driver.eos_trailer || (driver.picture_flags & 0x0004U);
}

} // namespace

const char *FailureName(Failure failure)
{
    static const char *const names[] = {
        "none", "argument", "output-path", "allocation", "research-device-open",
        "receipt-arm", "host-receipt-overflow", "host-clock", "receipt-seal",
        "receipt-read", "research-device-close", "driver-receipt-overflow",
        "invalid-driver-record", "clock-inversion", "eos-mismatch",
        "firmware-timestamp", "ambiguous-join", "unmatched-receipt", "native-run",
        "output-open", "output-mode", "output-write", "output-close"
    };
    const size_t index = static_cast<size_t>(failure);
    return index < sizeof(names) / sizeof(names[0]) ? names[index] : "unknown";
}

bool AbsoluteNewPathSyntax(const char *path)
{
    if (!path || path[0] != '/' || !path[1]) return false;
    const char *base = std::strrchr(path, '/');
    return base && base[1] && std::strcmp(base + 1, ".") &&
        std::strcmp(base + 1, "..");
}

bool OutputPathAvailable(const char *path)
{
    if (!AbsoluteNewPathSyntax(path)) return false;
    struct stat status = {};
    if (!lstat(path, &status)) return false;
    return errno == ENOENT;
}

bool NormalizeFirmwareTimestamp(uint64_t raw, uint64_t *normalized)
{
    if (!normalized) return false;
    if (raw == UINT32_MAX) {
        *normalized = UINT64_MAX;
        return true;
    }
    const uint64_t scale = 20000U;
    if (raw > UINT64_MAX / scale) return false;
    *normalized = raw * scale;
    return true;
}

uint32_t DriverCapacity(unsigned expected_outputs)
{
    const uint64_t capacity = static_cast<uint64_t>(expected_outputs) * 4U + 64U;
    return static_cast<uint32_t>(std::min<uint64_t>(
        capacity, CRYSTALHD_FW_RESEARCH_RX_RECEIPT_MAX_RECORDS));
}

size_t HostCapacity(unsigned expected_outputs)
{
    return static_cast<size_t>(expected_outputs) + 32U;
}

bool JoinReceipts(const HostReceipt *host, size_t host_count,
                  const crystalhd_fw_research_rx_receipt *driver,
                  size_t driver_count, JoinedReceipt *joined,
                  size_t joined_capacity, uint8_t *driver_matched,
                  size_t matched_capacity, size_t *joined_count,
                  Failure *failure)
{
    if (failure) *failure = Failure::None;
    if (joined_count) *joined_count = 0;
    if ((!host && host_count) || (!driver && driver_count) ||
        (!joined && host_count) || joined_capacity < host_count ||
        (!driver_matched && driver_count) || matched_capacity < driver_count ||
        !joined_count || !failure) {
        if (failure) *failure = Failure::Argument;
        return false;
    }
    if (driver_count)
        std::memset(driver_matched, 0, driver_count);
    for (size_t index = 0; index < driver_count; ++index) {
        if (driver[index].stage_flags &
            CRYSTALHD_FW_RESEARCH_RX_STAGE_CLOCK_INVERTED) {
            *failure = Failure::ClockInversion;
            return false;
        }
        if (driver[index].disposition ==
                CRYSTALHD_FW_RESEARCH_RX_DISPOSITION_OUTPUT) {
            uint64_t normalized = 0;
            Failure record_failure = Failure::None;
            if (!DriverOutputRecord(driver[index], &normalized,
                                    &record_failure)) {
                *failure = record_failure == Failure::None
                    ? Failure::DriverRecord : record_failure;
                return false;
            }
        }
    }
    for (size_t host_index = 0; host_index < host_count; ++host_index) {
        if (!host[host_index].return_ns ||
            (host_index && host[host_index].return_ns <
                host[host_index - 1].return_ns)) {
            *failure = Failure::ClockInversion;
            return false;
        }
        size_t match = driver_count;
        unsigned matches = 0;
        uint64_t match_timestamp = 0;
        for (size_t driver_index = 0; driver_index < driver_count;
             ++driver_index) {
            uint64_t normalized = 0;
            Failure record_failure = Failure::None;
            const bool output = DriverOutputRecord(driver[driver_index],
                                                   &normalized,
                                                   &record_failure);
            if (!output) {
                if (record_failure != Failure::None) {
                    *failure = record_failure;
                    return false;
                }
                continue;
            }
            if (MatchKey(host[host_index], driver[driver_index], normalized)) {
                ++matches;
                match = driver_index;
                match_timestamp = normalized;
            }
        }
        if (matches > 1) {
            *failure = Failure::Ambiguous;
            return false;
        }
        if (matches != 1 || driver_matched[match]) {
            *failure = Failure::Unmatched;
            return false;
        }
        const bool driver_eos = DriverEosControl(driver[match]);
        if (!host[host_index].return_ns ||
            host[host_index].return_ns < driver[match].disposition_ns) {
            *failure = Failure::ClockInversion;
            return false;
        }
        if ((host[host_index].eos != 0) != driver_eos) {
            *failure = Failure::EosMismatch;
            return false;
        }
        driver_matched[match] = 1;
        joined[host_index].host = host[host_index];
        joined[host_index].driver = driver[match];
        joined[host_index].normalized_firmware_timestamp = match_timestamp;
    }
    for (size_t index = 0; index < driver_count; ++index)
        if (driver[index].disposition ==
                CRYSTALHD_FW_RESEARCH_RX_DISPOSITION_OUTPUT &&
            !driver_matched[index] && !DriverEosControl(driver[index])) {
            *failure = Failure::Unmatched;
            return false;
        }
    *joined_count = host_count;
    return true;
}

bool WriteTsv(const char *path, const JoinedReceipt *rows, size_t row_count,
              uint64_t generation, uint64_t run_id, const OutputOps *in_ops,
              Failure *failure)
{
    if (failure) *failure = Failure::None;
    if (!failure || !OutputPathAvailable(path) || (!rows && row_count)) {
        if (failure) *failure = Failure::Path;
        return false;
    }
    OutputOps ops = in_ops ? *in_ops : OutputOps{};
    if (!ops.write_file) ops.write_file = WriteFile;
    const int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC |
                       O_NOFOLLOW, 0600);
    if (fd < 0) {
        *failure = Failure::OutputOpen;
        return false;
    }
    bool ok = fchmod(fd, 0600) == 0;
    if (!ok) *failure = Failure::OutputMode;
    static const char header[] =
        "receipt_generation\treceipt_run_id\thost_ordinal\thost_return_ns\t"
        "ready_before\ttoken\tpicture_number\t"
        "session_number\thost_picture_flags\twidth\theight\ty_done_words\t"
        "uv_done_words\tpout_flags\tb422_mode\teos\tdriver_ordinal\t"
        "capture_epoch\tfire_return_ns\tirq_entry_ns\tcompletion_ns\tparse_ns\t"
        "disposition_ns\traw_firmware_timestamp\tnormalized_firmware_timestamp\t"
        "driver_picture_number\tdriver_session_number\tdriver_width\t"
        "driver_height\tdriver_y_done_words\tdriver_uv_done_words\t"
        "rx_sequence\tlist_index\tpacket_tag\tchannel_id\tcompletion_status\t"
        "y_descriptor_count\tuv_descriptor_count\tdriver_picture_flags\t"
        "row_width\tpib_line\tpacket_flags\tstage_flags\tdisposition\t"
        "eos_trailer\tfire_to_irq_ns\tirq_to_completion_ns\t"
        "disposition_to_host_ns\n";
    if (ok && !WriteAll(fd, header, sizeof(header) - 1, ops)) {
        ok = false;
        *failure = Failure::OutputWrite;
    }
    for (size_t index = 0; ok && index < row_count; ++index) {
        const HostReceipt &host = rows[index].host;
        const crystalhd_fw_research_rx_receipt &driver = rows[index].driver;
        char line[2048];
        const int length = std::snprintf(line, sizeof(line),
            "%llu\t%llu\t%llu\t%llu\t%u\t%llu\t%u\t%u\t%u\t%u\t%u\t"
            "%u\t%u\t%u\t%u\t%u\t%llu\t%llu\t%llu\t"
            "%llu\t%llu\t%llu\t%llu\t%llu\t"
            "%llu\t%u\t%u\t%u\t%u\t%u\t%u\t"
            "%u\t%u\t%u\t%u\t%d\t%u\t%u\t%u\t%u\t%u\t%u\t%u\t"
            "%u\t%u\t%llu\t%llu\t%llu\n",
            static_cast<unsigned long long>(generation),
            static_cast<unsigned long long>(run_id),
            static_cast<unsigned long long>(host.ordinal),
            static_cast<unsigned long long>(host.return_ns),
            host.ready_before,
            static_cast<unsigned long long>(host.token),
            host.picture_number, host.session_number, host.picture_flags,
            host.width, host.height, host.y_done_words, host.uv_done_words,
            host.pout_flags, host.b422_mode, host.eos,
            static_cast<unsigned long long>(driver.ordinal),
            static_cast<unsigned long long>(driver.capture_epoch),
            static_cast<unsigned long long>(driver.fire_return_ns),
            static_cast<unsigned long long>(driver.irq_entry_ns),
            static_cast<unsigned long long>(driver.completion_ns),
            static_cast<unsigned long long>(driver.parse_ns),
            static_cast<unsigned long long>(driver.disposition_ns),
            static_cast<unsigned long long>(driver.firmware_timestamp),
            static_cast<unsigned long long>(
                rows[index].normalized_firmware_timestamp),
            driver.picture_number, driver.session_number,
            driver.picture_width, driver.picture_height,
            driver.y_done_words, driver.uv_done_words, driver.rx_sequence,
            driver.list_index, driver.packet_tag, driver.channel_id,
            driver.completion_status, driver.y_descriptor_count,
            driver.uv_descriptor_count, driver.picture_flags, driver.row_width,
            driver.pib_line, driver.packet_flags, driver.stage_flags,
            driver.disposition, driver.eos_trailer,
            static_cast<unsigned long long>(
                driver.irq_entry_ns - driver.fire_return_ns),
            static_cast<unsigned long long>(
                driver.completion_ns - driver.irq_entry_ns),
            static_cast<unsigned long long>(
                host.return_ns - driver.disposition_ns));
        if (length < 0 || static_cast<size_t>(length) >= sizeof(line) ||
            !WriteAll(fd, line, static_cast<size_t>(length), ops)) {
            ok = false;
            *failure = Failure::OutputWrite;
        }
    }
    if (close(fd) && ok) {
        ok = false;
        *failure = Failure::OutputClose;
    }
    return ok;
}

Probe::Probe(const DeviceOps *device_ops, const OutputOps *output_ops)
{
    if (device_ops) device_ops_ = *device_ops;
    if (!device_ops_.open_device) device_ops_.open_device = OpenDevice;
    if (!device_ops_.ioctl_device) device_ops_.ioctl_device = IoctlDevice;
    if (!device_ops_.close_device) device_ops_.close_device = CloseDeviceFd;
    if (output_ops) output_ops_ = *output_ops;
    if (!output_ops_.write_file) output_ops_.write_file = WriteFile;
}

Probe::~Probe()
{
    if (fd_ >= 0) (void)device_ops_.close_device(device_ops_.opaque, fd_);
}

void Probe::Fail(Failure failure)
{
    if (failure_ == Failure::None) failure_ = failure;
}

bool Probe::Prepare(const char *output_path, unsigned expected_outputs)
{
    if (prepared_ || !expected_outputs || !OutputPathAvailable(output_path)) {
        Fail(Failure::Path);
        return false;
    }
    const size_t host_capacity = HostCapacity(expected_outputs);
    const uint32_t driver_capacity = DriverCapacity(expected_outputs);
    if (!host_capacity || !driver_capacity) {
        Fail(Failure::Argument);
        return false;
    }
    try {
        host_.resize(host_capacity);
        driver_.resize(driver_capacity);
        joined_.resize(host_capacity);
        driver_matched_.resize(driver_capacity);
    } catch (const std::bad_alloc &) {
        Fail(Failure::Allocation);
        return false;
    }
    output_path_ = output_path;
    arm_.version = CRYSTALHD_FW_RESEARCH_RX_RECEIPT_VERSION;
    arm_.size = sizeof(arm_);
    arm_.capacity = driver_capacity;
    prepared_ = true;
    return true;
}

bool Probe::Arm()
{
    if (!prepared_ || armed_ || fd_ >= 0 || failure_ != Failure::None) {
        Fail(Failure::Argument);
        return false;
    }
    fd_ = device_ops_.open_device(device_ops_.opaque, kResearchDevice,
                                  O_RDWR | O_CLOEXEC | O_NOFOLLOW);
    if (fd_ < 0) {
        Fail(Failure::DeviceOpen);
        return false;
    }
    if (device_ops_.ioctl_device(device_ops_.opaque, fd_,
            CRYSTALHD_FW_RESEARCH_RX_RECEIPT_ARM, &arm_) < 0) {
        Fail(Failure::Arm);
        (void)CloseDevice();
        return false;
    }
    armed_ = true;
    if (arm_.version != CRYSTALHD_FW_RESEARCH_RX_RECEIPT_VERSION ||
        arm_.size != sizeof(arm_) || !arm_.generation || !arm_.run_id ||
        arm_.capacity != driver_.size() || arm_.flags) {
        /* Successful ARM with corrupt copyout still created a store. Seal the
         * current idle store so a subsequent exact ARM can replace it.
         */
        crystalhd_fw_research_rx_receipt_seal recovery = {};
        recovery.version = CRYSTALHD_FW_RESEARCH_RX_RECEIPT_VERSION;
        recovery.size = sizeof(recovery);
        recovery.flags = CRYSTALHD_FW_RESEARCH_RX_RECEIPT_SEAL_CURRENT;
        (void)device_ops_.ioctl_device(device_ops_.opaque, fd_,
            CRYSTALHD_FW_RESEARCH_RX_RECEIPT_SEAL, &recovery);
        Fail(Failure::Arm);
        armed_ = false;
        (void)CloseDevice();
        return false;
    }
    return true;
}

bool Probe::Record(const HostReceipt &receipt)
{
    if (!armed_ || failure_ != Failure::None) return false;
    if (!receipt.return_ns) {
        Fail(Failure::HostClock);
        return false;
    }
    if (host_count_ &&
        receipt.return_ns < host_[host_count_ - 1].return_ns) {
        Fail(Failure::ClockInversion);
        return false;
    }
    if (host_count_ >= host_.size()) {
        Fail(Failure::HostOverflow);
        return false;
    }
    host_[host_count_] = receipt;
    host_[host_count_].ordinal = host_count_;
    ++host_count_;
    return true;
}

bool Probe::CloseDevice()
{
    if (fd_ < 0) return true;
    const int fd = fd_;
    fd_ = -1;
    return device_ops_.close_device(device_ops_.opaque, fd) == 0;
}

bool Probe::SealAndRead()
{
    crystalhd_fw_research_rx_receipt_seal seal = {};
    seal.version = CRYSTALHD_FW_RESEARCH_RX_RECEIPT_VERSION;
    seal.size = sizeof(seal);
    seal.run_id = arm_.run_id;
    const bool seal_failed = device_ops_.ioctl_device(device_ops_.opaque, fd_,
            CRYSTALHD_FW_RESEARCH_RX_RECEIPT_SEAL, &seal) < 0 ||
        seal.version != CRYSTALHD_FW_RESEARCH_RX_RECEIPT_VERSION ||
        seal.size != sizeof(seal) || seal.flags || !seal.sealed ||
        seal.generation != arm_.generation || seal.run_id != arm_.run_id ||
        seal.record_count > driver_.size();
    if (seal_failed) {
        /* A failed ioctl can mean the exact SEAL never ran or that its reply
         * was lost after it ran.  Retry only our exact run ID: SEAL_CURRENT
         * could affect a concurrently replaced run.  A successful retry
         * releases an armed store; EALREADY means the first call sealed it.
         * Either way the original evidence failure remains fatal.
         */
        crystalhd_fw_research_rx_receipt_seal recovery = {};
        recovery.version = CRYSTALHD_FW_RESEARCH_RX_RECEIPT_VERSION;
        recovery.size = sizeof(recovery);
        recovery.run_id = arm_.run_id;
        (void)device_ops_.ioctl_device(device_ops_.opaque, fd_,
            CRYSTALHD_FW_RESEARCH_RX_RECEIPT_SEAL, &recovery);
        Fail(Failure::Seal);
        return false;
    }
    if (seal.overflow_count) {
        Fail(Failure::DriverOverflow);
        return false;
    }
    uint32_t start = 0;
    bool first = true;
    while (first || start < seal.record_count) {
        first = false;
        crystalhd_fw_research_rx_receipt_read read = {};
        read.version = CRYSTALHD_FW_RESEARCH_RX_RECEIPT_VERSION;
        read.size = sizeof(read);
        read.start = start;
        read.run_id = arm_.run_id;
        if (device_ops_.ioctl_device(device_ops_.opaque, fd_,
                CRYSTALHD_FW_RESEARCH_RX_RECEIPT_READ, &read) < 0 ||
            read.version != CRYSTALHD_FW_RESEARCH_RX_RECEIPT_VERSION ||
            read.size != sizeof(read) || read.flags ||
            read.generation != arm_.generation || read.run_id != arm_.run_id ||
            !read.sealed || read.record_count != seal.record_count ||
            read.overflow_count ||
            read.returned > CRYSTALHD_FW_RESEARCH_RX_RECEIPT_READ_RECORDS ||
            read.returned > read.record_count - start ||
            (!read.returned && start < read.record_count)) {
            Fail(Failure::Read);
            return false;
        }
        std::copy(read.records, read.records + read.returned,
                  driver_.begin() + start);
        start += read.returned;
    }
    driver_count_ = seal.record_count;
    return true;
}

bool Probe::Finish(bool native_run_ok)
{
    if (!armed_ || fd_ < 0) {
        Fail(Failure::Argument);
        return false;
    }
    bool ok = failure_ == Failure::None;
    if (!SealAndRead()) ok = false;
    if (!CloseDevice()) {
        Fail(Failure::ResearchClose);
        ok = false;
    }
    armed_ = false;
    if (!native_run_ok) {
        Fail(Failure::NativeRun);
        ok = false;
    }
    Failure join_failure = Failure::None;
    if (ok && !JoinReceipts(host_.data(), host_count_, driver_.data(),
            driver_count_, joined_.data(), joined_.size(),
            driver_matched_.data(), driver_matched_.size(), &joined_count_,
            &join_failure)) {
        Fail(join_failure);
        ok = false;
    }
    Failure write_failure = Failure::None;
    if (ok && !WriteTsv(output_path_, joined_.data(), joined_count_,
                        arm_.generation, arm_.run_id, &output_ops_,
                        &write_failure)) {
        Fail(write_failure);
        ok = false;
    }
    return ok;
}

} // namespace rx_output_receipts
