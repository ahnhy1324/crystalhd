/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "rx-output-receipts.h"

#include <bc_dts_defs.h>

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

using namespace rx_output_receipts;

static unsigned checks;
static bool okay = true;

static void Check(bool value, const char *message)
{
    ++checks;
    if (!value) {
        okay = false;
        std::fprintf(stderr, "FAIL: %s\n", message);
    }
}

static crystalhd_fw_research_rx_receipt Driver(uint64_t ordinal,
                                                uint32_t raw_timestamp,
                                                uint32_t picture,
                                                bool eos = false)
{
    crystalhd_fw_research_rx_receipt record = {};
    record.ordinal = ordinal;
    record.capture_epoch = 3;
    record.fire_return_ns = 100 + ordinal * 100;
    record.irq_entry_ns = 110 + ordinal * 100;
    record.completion_ns = 120 + ordinal * 100;
    record.parse_ns = 130 + ordinal * 100;
    record.disposition_ns = 140 + ordinal * 100;
    record.firmware_timestamp = raw_timestamp;
    record.rx_sequence = 10 + ordinal;
    record.list_index = ordinal % 2;
    record.packet_tag = 20 + ordinal;
    record.channel_id = 1;
    record.completion_status = 0;
    record.y_done_words = 320 * 180;
    record.uv_done_words = 0;
    record.y_descriptor_count = 4;
    record.picture_number = picture;
    record.picture_flags = eos ? 0x0004U : 0;
    record.picture_width = 640;
    record.picture_height = 360;
    record.row_width = 640;
    record.pib_line = 360;
    record.session_number = 4;
    record.packet_flags = 6;
    record.stage_flags =
        CRYSTALHD_FW_RESEARCH_RX_STAGE_RESERVED |
        CRYSTALHD_FW_RESEARCH_RX_STAGE_FIRE_RETURN |
        CRYSTALHD_FW_RESEARCH_RX_STAGE_IRQ_ENTRY |
        CRYSTALHD_FW_RESEARCH_RX_STAGE_COMPLETE |
        CRYSTALHD_FW_RESEARCH_RX_STAGE_PARSED |
        CRYSTALHD_FW_RESEARCH_RX_STAGE_DISPOSITION;
    record.disposition = CRYSTALHD_FW_RESEARCH_RX_DISPOSITION_OUTPUT;
    record.eos_trailer = eos;
    return record;
}

static HostReceipt Host(const crystalhd_fw_research_rx_receipt &driver)
{
    HostReceipt host;
    host.return_ns = driver.disposition_ns + 20;
    host.token = driver.firmware_timestamp == UINT32_MAX
        ? UINT64_MAX : driver.firmware_timestamp * 20000U;
    host.ready_before = 2;
    host.picture_number = driver.picture_number;
    host.session_number = driver.session_number;
    host.picture_flags = driver.picture_flags;
    host.n_drop = 17;
    host.width = driver.picture_width;
    host.height = driver.picture_height;
    host.y_done_words = driver.y_done_words;
    host.uv_done_words = driver.uv_done_words;
    host.pout_flags = 0x20000;
    host.b422_mode = 2;
    host.eos = driver.eos_trailer != 0;
    return host;
}

struct FakeDevice {
    enum class SealFault { None, RejectBeforeSeal, LoseReply };
    enum class ArmReplyFault { None, Reserved, ZeroIdentity };
    enum class SealReplyFault { None, Generation, Sealed, Reserved };
    enum class ReadReplyFault { None, Generation, Start, Sealed, Reserved,
                                Ordinal };

    std::vector<crystalhd_fw_research_rx_receipt> records;
    std::string events;
    bool valid = true;
    bool close_ok = true;
    bool sealed = false;
    SealFault seal_fault = SealFault::None;
    ArmReplyFault arm_reply_fault = ArmReplyFault::None;
    SealReplyFault seal_reply_fault = SealReplyFault::None;
    ReadReplyFault read_reply_fault = ReadReplyFault::None;
    unsigned exact_seal_calls = 0;
    uint32_t overflow_count = 0;
    uint32_t capacity = 0;

    static int Open(void *opaque, const char *path, int flags)
    {
        FakeDevice *fake = static_cast<FakeDevice *>(opaque);
        fake->events += "O";
        fake->valid &= !std::strcmp(path, "/dev/crystalhd-fw-research") &&
            (flags & O_RDWR) && (flags & O_CLOEXEC) && (flags & O_NOFOLLOW);
        return 91;
    }

    static int Ioctl(void *opaque, int fd, unsigned long command, void *argument)
    {
        FakeDevice *fake = static_cast<FakeDevice *>(opaque);
        fake->valid &= fd == 91;
        if (command == CRYSTALHD_FW_RESEARCH_RX_RECEIPT_ARM) {
            fake->events += "A";
            crystalhd_fw_research_rx_receipt_arm *arm =
                static_cast<crystalhd_fw_research_rx_receipt_arm *>(argument);
            fake->valid &= arm->version ==
                    CRYSTALHD_FW_RESEARCH_RX_RECEIPT_VERSION &&
                arm->size == sizeof(*arm) && arm->capacity && !arm->flags &&
                !arm->generation && !arm->run_id;
            fake->capacity = arm->capacity;
            arm->generation = 7;
            arm->run_id = 9;
            if (fake->arm_reply_fault == ArmReplyFault::Reserved)
                arm->reserved[0] = 1;
            if (fake->arm_reply_fault == ArmReplyFault::ZeroIdentity) {
                arm->generation = 0;
                arm->run_id = 0;
            }
            return 0;
        }
        if (command == CRYSTALHD_FW_RESEARCH_RX_RECEIPT_SEAL) {
            crystalhd_fw_research_rx_receipt_seal *seal =
                static_cast<crystalhd_fw_research_rx_receipt_seal *>(argument);
            const bool recover = seal->flags ==
                CRYSTALHD_FW_RESEARCH_RX_RECEIPT_SEAL_CURRENT;
            fake->events += recover ? "K" : "S";
            fake->valid &= seal->version ==
                    CRYSTALHD_FW_RESEARCH_RX_RECEIPT_VERSION &&
                seal->size == sizeof(*seal) &&
                ((!recover && !seal->flags && seal->run_id == 9) ||
                 (recover && seal->flags ==
                      CRYSTALHD_FW_RESEARCH_RX_RECEIPT_SEAL_CURRENT &&
                  !seal->run_id)) &&
                !seal->generation && !seal->sealed;
            if (!recover) ++fake->exact_seal_calls;
            if (!recover && fake->sealed) {
                errno = EALREADY;
                return -1;
            }
            if (!recover && fake->exact_seal_calls == 1 &&
                fake->seal_fault == SealFault::RejectBeforeSeal) {
                errno = EIO;
                return -1;
            }
            fake->sealed = true;
            if (!recover && fake->exact_seal_calls == 1 &&
                fake->seal_fault == SealFault::LoseReply) {
                errno = EFAULT;
                return -1;
            }
            seal->generation = 7;
            seal->run_id = 9;
            seal->sealed = 1;
            seal->record_count = fake->records.size();
            seal->overflow_count = fake->overflow_count;
            if (!recover &&
                fake->seal_reply_fault == SealReplyFault::Generation)
                seal->generation++;
            if (!recover && fake->seal_reply_fault == SealReplyFault::Sealed)
                seal->sealed = 2;
            if (!recover &&
                fake->seal_reply_fault == SealReplyFault::Reserved)
                seal->reserved[0] = 1;
            return 0;
        }
        if (command == CRYSTALHD_FW_RESEARCH_RX_RECEIPT_READ) {
            fake->events += "R";
            crystalhd_fw_research_rx_receipt_read *read =
                static_cast<crystalhd_fw_research_rx_receipt_read *>(argument);
            const uint32_t start = read->start;
            fake->valid &= read->version ==
                    CRYSTALHD_FW_RESEARCH_RX_RECEIPT_VERSION &&
                read->size == sizeof(*read) && !read->flags &&
                read->run_id == 9 && !read->generation && !read->returned &&
                fake->sealed && start <= fake->records.size();
            read->generation = 7;
            read->sealed = 1;
            read->record_count = fake->records.size();
            read->overflow_count = fake->overflow_count;
            read->returned = std::min<size_t>(
                CRYSTALHD_FW_RESEARCH_RX_RECEIPT_READ_RECORDS,
                fake->records.size() - start);
            for (uint32_t index = 0; index < read->returned; ++index)
                read->records[index] = fake->records[start + index];
            if (fake->read_reply_fault == ReadReplyFault::Generation)
                read->generation++;
            if (fake->read_reply_fault == ReadReplyFault::Start)
                read->start++;
            if (fake->read_reply_fault == ReadReplyFault::Sealed)
                read->sealed = 2;
            if (fake->read_reply_fault == ReadReplyFault::Reserved)
                read->reserved[0] = 1;
            if (fake->read_reply_fault == ReadReplyFault::Ordinal &&
                read->returned)
                read->records[0].ordinal++;
            return 0;
        }
        fake->valid = false;
        errno = ENOTTY;
        return -1;
    }

    static int Close(void *opaque, int fd)
    {
        FakeDevice *fake = static_cast<FakeDevice *>(opaque);
        fake->events += "C";
        fake->valid &= fd == 91;
        return fake->close_ok ? 0 : -1;
    }

    DeviceOps Ops()
    {
        DeviceOps ops;
        ops.opaque = this;
        ops.open_device = Open;
        ops.ioctl_device = Ioctl;
        ops.close_device = Close;
        return ops;
    }
};

struct FailingWriter {
    unsigned calls = 0;
    static ssize_t Write(void *opaque, int fd, const void *data, size_t bytes)
    {
        FailingWriter *writer = static_cast<FailingWriter *>(opaque);
        ++writer->calls;
        if (writer->calls == 1)
            return write(fd, data, std::min<size_t>(bytes, 8));
        errno = EIO;
        return -1;
    }
    OutputOps Ops()
    {
        OutputOps ops;
        ops.opaque = this;
        ops.write_file = Write;
        return ops;
    }
};

struct RecoveringWriter {
    unsigned calls = 0;
    static ssize_t Write(void *opaque, int fd, const void *data, size_t bytes)
    {
        RecoveringWriter *writer = static_cast<RecoveringWriter *>(opaque);
        ++writer->calls;
        if (writer->calls == 1) {
            errno = EINTR;
            return -1;
        }
        if (writer->calls == 2)
            return write(fd, data, std::min<size_t>(bytes, 7));
        return write(fd, data, bytes);
    }
    OutputOps Ops()
    {
        OutputOps ops;
        ops.opaque = this;
        ops.write_file = Write;
        return ops;
    }
};

struct PublishRacer {
    const char *path = nullptr;
    const char *target = nullptr;
    bool raced = false;
    static ssize_t Write(void *opaque, int fd, const void *data, size_t bytes)
    {
        PublishRacer *writer = static_cast<PublishRacer *>(opaque);
        if (!writer->raced) {
            writer->raced = true;
            if (symlink(writer->target, writer->path)) return -1;
        }
        return write(fd, data, bytes);
    }
    OutputOps Ops()
    {
        OutputOps ops;
        ops.opaque = this;
        ops.write_file = Write;
        return ops;
    }
};

static bool Exists(const char *path, off_t *size = nullptr, mode_t *mode = nullptr)
{
    struct stat status = {};
    if (lstat(path, &status)) return false;
    if (size) *size = status.st_size;
    if (mode) *mode = status.st_mode & 0777;
    return true;
}

static unsigned CountPrefix(const char *directory, const char *prefix)
{
    DIR *stream = opendir(directory);
    if (!stream) return UINT_MAX;
    unsigned count = 0;
    const size_t length = std::strlen(prefix);
    while (const dirent *entry = readdir(stream))
        if (!std::strncmp(entry->d_name, prefix, length)) ++count;
    closedir(stream);
    return count;
}

int main()
{
    Check(AbsoluteNewPathSyntax("/tmp/result.tsv") &&
          !AbsoluteNewPathSyntax("result.tsv") &&
          !AbsoluteNewPathSyntax("/") &&
          !AbsoluteNewPathSyntax("/tmp/.") &&
          !AbsoluteNewPathSyntax("/tmp/.."),
          "absolute new-path syntax is strict");

    uint64_t timestamp = 1;
    const uint64_t max_safe_raw = UINT64_MAX / 20000U;
    Check(NormalizeFirmwareTimestamp(0, &timestamp) && timestamp == 0 &&
          NormalizeFirmwareTimestamp(17, &timestamp) && timestamp == 340000 &&
          NormalizeFirmwareTimestamp(UINT32_MAX, &timestamp) &&
              timestamp == UINT64_MAX &&
          NormalizeFirmwareTimestamp(static_cast<uint64_t>(UINT32_MAX) + 1,
                                      &timestamp) &&
              timestamp == (static_cast<uint64_t>(UINT32_MAX) + 1) * 20000U &&
          NormalizeFirmwareTimestamp(max_safe_raw, &timestamp) &&
              timestamp == max_safe_raw * 20000U &&
          !NormalizeFirmwareTimestamp(max_safe_raw + 1, &timestamp) &&
          !NormalizeFirmwareTimestamp(1, nullptr),
          "firmware timestamp normalization exactly mirrors the Flea library rule");
    Check(HostCapacity(1) == 33 && DriverCapacity(1) == 68 &&
          DriverCapacity(10000) == 40064,
          "bounded preallocation capacities cover successful outputs and RX churn");
    uint64_t dma_sum = 0;
    Check(CheckedDmaByteAdd(UINT64_MAX - 3U, 3U, &dma_sum) &&
          dma_sum == UINT64_MAX &&
          !CheckedDmaByteAdd(UINT64_MAX - 3U, 4U, &dma_sum) &&
          !CheckedDmaByteAdd(0, 0, nullptr),
          "driver DMA byte accumulation accepts its exact limit and rejects overflow/null output");

    {
        crystalhd_fw_research_rx_receipt full = Driver(0, 5, 10);
        DriverDmaAudit audit;
        Failure audit_failure = Failure::None;
        Check(AuditDriverDma(&full, 1, 640, 360, 320 * 180, 0,
                             &audit, &audit_failure) &&
              audit.records == 1 && audit.completed == 1 &&
              audit.full_size == 1 && audit.output_full_size == 1 &&
              !audit.non_output_full_size && !audit.partial_size &&
              audit.full_size_bytes == 320U * 180U * 4U &&
              audit_failure == Failure::None,
              "all-driver DMA audit counts exact completed native output bytes");
        crystalhd_fw_research_rx_receipt zero = Driver(0, 5, 10, true);
        zero.y_done_words = 0;
        Check(AuditDriverDma(&zero, 1, 640, 360, 320 * 180, 0,
                             &audit, &audit_failure) &&
              audit.records == 1 && audit.completed == 1 &&
              audit.eos_control == 1 && !audit.eos_control_bytes &&
              audit.zero_size == 1 && !audit.full_size &&
              !audit.partial_size && !audit.full_size_bytes,
              "all-driver DMA audit keeps a fully staged zero-size EOS control separate from picture DMA bytes");
        zero.eos_trailer = 0;
        zero.picture_flags &= ~0x0004U;
        Check(!AuditDriverDma(&zero, 1, 640, 360, 320 * 180, 0,
                              &audit, &audit_failure) &&
              audit_failure == Failure::DriverRecord,
              "all-driver DMA audit rejects an unexplained zero-size non-EOS record");
        crystalhd_fw_research_rx_receipt short_eos =
            Driver(0, 5, 10, true);
        short_eos.y_done_words = 16;
        Check(AuditDriverDma(&short_eos, 1, 640, 360, 320 * 180, 0,
                             &audit, &audit_failure) &&
              audit.eos_control == 1 && audit.eos_control_bytes == 64U &&
              !audit.zero_size && !audit.partial_size && !audit.full_size,
              "all-driver DMA audit accounts a bounded short EOS control without calling it partial picture DMA");
        short_eos.y_done_words = 320 * 180 + 1;
        Check(!AuditDriverDma(&short_eos, 1, 640, 360, 320 * 180, 0,
                              &audit, &audit_failure) &&
              audit_failure == Failure::DriverRecord,
              "all-driver DMA audit rejects an EOS control larger than one full native picture");
        crystalhd_fw_research_rx_receipt exact_controls[3] = {
            Driver(0, 5, 10), Driver(1, 6, 11, true),
            Driver(2, 7, 12, true)
        };
        exact_controls[1].y_done_words = 16;
        exact_controls[2].y_done_words = 0;
        Check(AuditDriverDma(exact_controls, 3, 640, 360, 320 * 180, 0,
                             &audit, &audit_failure) &&
              audit.records == 3 && audit.completed == 3 &&
              audit.eos_control == 2 && audit.zero_size == 1 &&
              audit.full_size == 1 && audit.output_full_size == 1 &&
              audit.eos_control_bytes == 64U &&
              audit.full_size_bytes == 320U * 180U * 4U,
              "all-driver DMA audit separates the exact two EOS controls and byte totals from one full picture");
        full.disposition =
            CRYSTALHD_FW_RESEARCH_RX_DISPOSITION_RECYCLE_ZERO;
        Check(AuditDriverDma(&full, 1, 640, 360, 320 * 180, 0,
                             &audit, &audit_failure) &&
              audit.full_size == 1 && !audit.output_full_size &&
              audit.non_output_full_size == 1,
              "all-driver DMA audit exposes a full-size non-output receipt");
        full = Driver(0, 5, 10);
        --full.y_done_words;
        Check(AuditDriverDma(&full, 1, 640, 360, 320 * 180, 0,
                             &audit, &audit_failure) &&
              !audit.full_size && audit.partial_size == 1 &&
              !audit.full_size_bytes,
              "all-driver DMA audit never counts a partial transfer as full-size");
        full = Driver(0, 5, 10);
        full.completion_status = BC_STS_ERROR;
        Check(!AuditDriverDma(&full, 1, 640, 360, 320 * 180, 0,
                              &audit, &audit_failure) &&
              audit.error == 1 && audit_failure == Failure::DriverRecord,
              "all-driver DMA audit fails a completion error without using stale sizes");
        full = Driver(0, 5, 10);
        full.stage_flags &= ~CRYSTALHD_FW_RESEARCH_RX_STAGE_COMPLETE;
        Check(!AuditDriverDma(&full, 1, 640, 360, 320 * 180, 0,
                              &audit, &audit_failure) &&
              audit_failure == Failure::DriverRecord,
              "all-driver DMA audit fails an incomplete stage record");
    }

    crystalhd_fw_research_rx_receipt drivers[3] = {
        Driver(0, 5, 10), Driver(1, UINT32_MAX, 11, true), Driver(2, 7, 12)
    };
    drivers[2].disposition =
        CRYSTALHD_FW_RESEARCH_RX_DISPOSITION_RECYCLE_ZERO;
    HostReceipt hosts[2] = {Host(drivers[1]), Host(drivers[0])};
    hosts[1].return_ns = hosts[0].return_ns + 20;
    JoinedReceipt joined[2] = {};
    uint8_t matched[3] = {};
    size_t joined_count = 0;
    Failure failure = Failure::None;
    Check(JoinReceipts(hosts, 2, drivers, 3, joined, 2, matched, 3,
                       &joined_count, &failure) &&
          joined_count == 2 && joined[0].driver.ordinal == 1 &&
          joined[0].normalized_firmware_timestamp == UINT64_MAX &&
          joined[1].driver.ordinal == 0 && matched[0] && matched[1] &&
          !matched[2] && failure == Failure::None,
          "join is order-independent and ignores explicitly recycled RX records");

    {
        crystalhd_fw_research_rx_receipt controls[2] = {
            Driver(0, 5, 10), Driver(1, UINT32_MAX, UINT32_MAX, true)
        };
        HostReceipt picture[1] = {Host(controls[0])};
        Check(JoinReceipts(picture, 1, controls, 2, joined, 2, matched, 3,
                           &joined_count, &failure) && joined_count == 1 &&
              joined[0].driver.ordinal == 0,
              "an unmatched firmware EOS control is not misreported as a successful NoCopy result");
        controls[1].parse_ns = 0;
        Check(!JoinReceipts(picture, 1, controls, 2, joined, 2, matched, 3,
                            &joined_count, &failure) &&
              failure == Failure::DriverRecord,
              "an unmatched EOS control must still be a valid output receipt");
    }

    {
        crystalhd_fw_research_rx_receipt duplicate[2] = {
            Driver(0, 5, 10), Driver(1, 5, 10)
        };
        duplicate[1].fire_return_ns += 1000;
        duplicate[1].irq_entry_ns += 1000;
        duplicate[1].completion_ns += 1000;
        duplicate[1].parse_ns += 1000;
        duplicate[1].disposition_ns += 1000;
        HostReceipt repeated[2] = {Host(duplicate[0]), Host(duplicate[0])};
        Check(!JoinReceipts(repeated, 2, duplicate, 2, joined, 2, matched, 3,
                            &joined_count, &failure) &&
              failure == Failure::Ambiguous,
              "duplicate full join keys fail as ambiguous");
    }
    {
        HostReceipt changed[2] = {hosts[0], hosts[1]};
        changed[0].session_number++;
        Check(!JoinReceipts(changed, 2, drivers, 3, joined, 2, matched, 3,
                            &joined_count, &failure) &&
              failure == Failure::Unmatched,
              "missing exact session identity fails as unmatched");
        changed[0] = hosts[0];
        changed[1] = hosts[1];
        changed[1].return_ns = changed[0].return_ns - 1;
        Check(!JoinReceipts(changed, 2, drivers, 3, joined, 2, matched, 3,
                            &joined_count, &failure) &&
              failure == Failure::ClockInversion,
              "host CLOCK_MONOTONIC samples must not run backwards");
        changed[0] = hosts[0];
        changed[1] = hosts[1];
        changed[0].eos = 0;
        Check(!JoinReceipts(changed, 2, drivers, 3, joined, 2, matched, 3,
                            &joined_count, &failure) &&
              failure == Failure::EosMismatch,
              "host and driver EOS disagreement has a distinct failure");
    }
    {
        crystalhd_fw_research_rx_receipt inverted[3] = {
            drivers[0], drivers[1], drivers[2]
        };
        inverted[2].stage_flags |=
            CRYSTALHD_FW_RESEARCH_RX_STAGE_CLOCK_INVERTED;
        Check(!JoinReceipts(hosts, 2, inverted, 3, joined, 2, matched, 3,
                            &joined_count, &failure) &&
              failure == Failure::ClockInversion,
              "clock inversion fails even on a non-output receipt");
    }
    {
        crystalhd_fw_research_rx_receipt malformed[1] = {Driver(0, 5, 10)};
        HostReceipt host[1] = {Host(malformed[0])};
        malformed[0].completion_ns = malformed[0].irq_entry_ns - 1;
        Check(!JoinReceipts(host, 1, malformed, 1, joined, 2, matched, 3,
                            &joined_count, &failure) &&
              failure == Failure::DriverRecord,
              "non-monotonic driver stages fail closed");
        malformed[0] = Driver(0, 5, 10);
        malformed[0].stage_flags |=
            CRYSTALHD_FW_RESEARCH_RX_STAGE_PARSE_FAILED;
        Check(!JoinReceipts(host, 1, malformed, 1, joined, 2, matched, 3,
                            &joined_count, &failure) &&
              failure == Failure::DriverRecord,
              "PARSED and PARSE_FAILED together fail closed");
        malformed[0] = Driver(0, 5, 10);
        malformed[0].firmware_timestamp = max_safe_raw + 1;
        host[0].token = 5 * 20000U;
        Check(!JoinReceipts(host, 1, malformed, 1, joined, 2, matched, 3,
                            &joined_count, &failure) &&
              failure == Failure::Timestamp,
              "raw firmware timestamp multiplication overflow fails closed");
    }

    char directory[] = "/tmp/crystalhd-rx-output-test.XXXXXX";
    Check(mkdtemp(directory) != nullptr, "temporary output directory created");
    char output[512], native_failure[512], preserved_failure[512];
    char invalid_failure[512], semantic_failure[512], empty_failure[512];
    char eos_only_failure[512], partial[512], raced[512];
    char publish_race[512], recovered[512], multi[512], overflow[512];
    char arm_reserved[512], arm_unknown[512];
    std::snprintf(output, sizeof(output), "%s/receipts.tsv", directory);
    std::snprintf(native_failure, sizeof(native_failure), "%s/native-fail.tsv",
                  directory);
    std::snprintf(preserved_failure, sizeof(preserved_failure),
                  "%s/preserved-native-fail.tsv", directory);
    std::snprintf(invalid_failure, sizeof(invalid_failure),
                  "%s/invalid-native-fail.tsv", directory);
    std::snprintf(semantic_failure, sizeof(semantic_failure),
                  "%s/semantic-native-fail.tsv", directory);
    std::snprintf(empty_failure, sizeof(empty_failure),
                  "%s/empty-native-fail.tsv", directory);
    std::snprintf(eos_only_failure, sizeof(eos_only_failure),
                  "%s/eos-only-native-fail.tsv", directory);
    std::snprintf(partial, sizeof(partial), "%s/partial.tsv", directory);
    std::snprintf(raced, sizeof(raced), "%s/raced.tsv", directory);
    std::snprintf(publish_race, sizeof(publish_race), "%s/publish-race.tsv",
                  directory);
    std::snprintf(recovered, sizeof(recovered), "%s/recovered.tsv", directory);
    std::snprintf(multi, sizeof(multi), "%s/multi.tsv", directory);
    std::snprintf(overflow, sizeof(overflow), "%s/overflow.tsv", directory);
    std::snprintf(arm_reserved, sizeof(arm_reserved), "%s/arm-reserved.tsv",
                  directory);
    std::snprintf(arm_unknown, sizeof(arm_unknown), "%s/arm-unknown.tsv",
                  directory);

    {
        FakeDevice fake;
        fake.records.push_back(Driver(0, 5, 10));
        DeviceOps ops = fake.Ops();
        Probe probe(&ops);
        HostReceipt host = Host(fake.records[0]);
        Check(probe.Prepare(output, 1) && !Exists(output) && probe.Arm() &&
              probe.generation() == 7 && probe.run_id() == 9 &&
              probe.Record(host) && probe.Finish(true) && fake.valid &&
              fake.events == "OASRC" && probe.host_count() == 1 &&
              probe.driver_count() == 1 && probe.joined_count() == 1,
              "probe arms before the session boundary then seals, reads, closes and exports");
        DriverDmaAudit dma_audit;
        Failure dma_failure = Failure::None;
        Check(probe.AuditDma(640, 360, 320 * 180, 0,
                             &dma_audit, &dma_failure) &&
              dma_audit.full_size == 1 &&
              dma_audit.full_size_bytes == 320U * 180U * 4U &&
              dma_failure == Failure::None,
              "finished probe retains every sealed driver record for DMA audit");
        off_t bytes = 0;
        mode_t mode = 0;
        Check(Exists(output, &bytes, &mode) && bytes > 100 && mode == 0600,
              "successful joined evidence is a nonempty mode-0600 file");
        FILE *file = std::fopen(output, "rb");
        char text[4096] = {};
        const size_t read = file ? std::fread(text, 1, sizeof(text) - 1, file) : 0;
        if (file) std::fclose(file);
        static const char expected[] =
            "receipt_generation\treceipt_run_id\thost_ordinal\thost_return_ns\t"
            "ready_before\ttoken\tpicture_number\tsession_number\t"
            "host_picture_flags\thost_n_drop\twidth\theight\ty_done_words\tuv_done_words\t"
            "pout_flags\tb422_mode\teos\tdriver_ordinal\tcapture_epoch\t"
            "fire_return_ns\tirq_entry_ns\tcompletion_ns\tparse_ns\t"
            "disposition_ns\traw_firmware_timestamp\t"
            "normalized_firmware_timestamp\tdriver_picture_number\t"
            "driver_session_number\tdriver_width\tdriver_height\t"
            "driver_y_done_words\tdriver_uv_done_words\trx_sequence\t"
            "list_index\tpacket_tag\tchannel_id\tcompletion_status\t"
            "y_descriptor_count\tuv_descriptor_count\tdriver_picture_flags\t"
            "row_width\tpib_line\tpacket_flags\tstage_flags\tdisposition\t"
            "eos_trailer\tfire_to_irq_ns\tirq_to_completion_ns\t"
            "disposition_to_host_ns\n"
            "7\t9\t0\t160\t2\t100000\t10\t4\t0\t17\t640\t360\t57600\t0\t"
            "131072\t2\t0\t0\t3\t100\t110\t120\t130\t140\t5\t100000\t"
            "10\t4\t640\t360\t57600\t0\t10\t0\t20\t1\t0\t4\t0\t0\t"
            "640\t360\t6\t95\t1\t0\t10\t10\t20\n";
        Check(read == sizeof(expected) - 1 &&
              !std::memcmp(text, expected, sizeof(expected) - 1),
              "successful TSV preserves the exact ordered 49-column schema and golden row");
        Probe collision(&ops);
        Check(!collision.Prepare(output, 1) &&
              collision.failure() == Failure::Path,
              "an existing output is rejected before research-device open");
    }
    {
        FakeDevice fake;
        fake.arm_reply_fault = FakeDevice::ArmReplyFault::Reserved;
        DeviceOps ops = fake.Ops();
        Probe probe(&ops);
        Check(probe.Prepare(arm_reserved, 1) && !probe.Arm() &&
              probe.failure() == Failure::Arm && fake.valid && fake.sealed &&
              fake.exact_seal_calls == 1 && fake.events == "OASC" &&
              !Exists(arm_reserved),
              "malformed ARM reply with an identity retries only that exact run");
    }
    {
        FakeDevice fake;
        fake.arm_reply_fault = FakeDevice::ArmReplyFault::ZeroIdentity;
        DeviceOps ops = fake.Ops();
        Probe probe(&ops);
        Check(probe.Prepare(arm_unknown, 1) && !probe.Arm() &&
              probe.failure() == Failure::Arm && fake.valid && !fake.sealed &&
              fake.exact_seal_calls == 0 && fake.events == "OAC" &&
              !Exists(arm_unknown),
              "malformed ARM reply without an identity never seals a current run");
    }
    {
        FakeDevice fake;
        for (unsigned index = 0; index < 33; ++index)
            fake.records.push_back(Driver(index, 100 + index, index + 1));
        DeviceOps ops = fake.Ops();
        Probe probe(&ops);
        bool recorded = probe.Prepare(multi, 33) && probe.Arm();
        for (const crystalhd_fw_research_rx_receipt &driver : fake.records)
            recorded = probe.Record(Host(driver)) && recorded;
        Check(recorded && probe.Finish(true) && fake.valid &&
              fake.events == "OASRRC" && probe.host_count() == 33 &&
              probe.driver_count() == 33 && probe.joined_count() == 33 &&
              Exists(multi),
              "READ pagination and exact 33-row join complete without hot-path allocation");
    }
    {
        FakeDevice fake;
        DeviceOps ops = fake.Ops();
        Probe probe(&ops);
        bool accepted = probe.Prepare(overflow, 1) && probe.Arm();
        HostReceipt receipt = Host(Driver(0, 9, 15));
        for (size_t index = 0; index < HostCapacity(1); ++index)
            accepted = probe.Record(receipt) && accepted;
        Check(accepted && !probe.Record(receipt) && !probe.Finish(true) &&
              probe.failure() == Failure::HostOverflow &&
              fake.events == "OASRC" && !Exists(overflow),
              "host receipt overflow seals the run and fails without publishing output");
    }
    {
        FakeDevice fake;
        fake.records.push_back(Driver(0, 6, 12));
        DeviceOps ops = fake.Ops();
        Probe probe(&ops);
        Check(probe.Prepare(native_failure, 1) && probe.Arm() &&
              probe.Record(Host(fake.records[0])) && !probe.Finish(false) &&
              probe.failure() == Failure::NativeRun && !Exists(native_failure) &&
              fake.events == "OASRC",
              "failed native decode still seals evidence but never publishes a TSV");
    }
    {
        FakeDevice fake;
        fake.records.push_back(Driver(0, 6, 12));
        DeviceOps ops = fake.Ops();
        Probe probe(&ops);
        DmaExpectation expected_dma;
        expected_dma.width = 640;
        expected_dma.height = 360;
        expected_dma.y_done_words = 320 * 180;
        Check(probe.Prepare(preserved_failure, 1) && probe.Arm() &&
              probe.Record(Host(fake.records[0])) &&
              !probe.Finish(false, &expected_dma) &&
              probe.failure() == Failure::NativeRun &&
              probe.failed_evidence_published() && fake.valid &&
              fake.events == "OASRC" && probe.host_count() == 1 &&
              probe.driver_count() == 1 && probe.joined_count() == 1 &&
              Exists(preserved_failure),
              "explicit failed-run policy validates, joins and atomically publishes evidence while returning native FAIL");
        DriverDmaAudit audit;
        Failure audit_failure = Failure::None;
        Check(probe.AuditDma(640, 360, 320 * 180, 0,
                             &audit, &audit_failure) &&
              audit.records == 1 && audit.full_size == 1 &&
              audit_failure == Failure::None,
              "published failed-run evidence retains every sealed driver record for audit");
        FILE *file = std::fopen(preserved_failure, "rb");
        char text[4096] = {};
        const size_t read = file ?
            std::fread(text, 1, sizeof(text) - 1, file) : 0;
        if (file) std::fclose(file);
        Check(read && std::strstr(text,
                  "disposition_to_host_ns\tnative_run_result\n") &&
              std::strstr(text, "\tFAIL\n") &&
              !std::strstr(text, "\tPASS\n"),
              "failed evidence identifies native FAIL in-band and cannot masquerade as a passing run");
        Check(!probe.Finish(false, &expected_dma) &&
              !probe.failed_evidence_published(),
              "a duplicate Finish call cannot expose stale failed-evidence publication state");
    }
    {
        FakeDevice fake;
        fake.records.push_back(Driver(0, 6, 12));
        fake.records[0].y_done_words = 0;
        DeviceOps ops = fake.Ops();
        Probe probe(&ops);
        DmaExpectation expected_dma;
        expected_dma.width = 640;
        expected_dma.height = 360;
        expected_dma.y_done_words = 320 * 180;
        Check(probe.Prepare(invalid_failure, 1) && probe.Arm() &&
              probe.Record(Host(fake.records[0])) &&
              !probe.Finish(false, &expected_dma) &&
              probe.failure() == Failure::DriverRecord &&
              !probe.failed_evidence_published() && fake.valid &&
              fake.events == "OASRC" && !Exists(invalid_failure),
              "failed-run preservation refuses malformed DMA receipts before atomic publication");
    }
    {
        FakeDevice fake;
        fake.records.push_back(Driver(0, 6, 12));
        --fake.records[0].y_done_words;
        DeviceOps ops = fake.Ops();
        Probe probe(&ops);
        DmaExpectation expected_dma;
        expected_dma.width = 640;
        expected_dma.height = 360;
        expected_dma.y_done_words = 320 * 180;
        Check(probe.Prepare(semantic_failure, 1) && probe.Arm() &&
              probe.Record(Host(fake.records[0])) &&
              !probe.Finish(false, &expected_dma) &&
              probe.failure() == Failure::NativeRun &&
              probe.failed_evidence_published() &&
              Exists(semantic_failure),
              "semantic short-picture evidence remains inspectable but cannot turn native failure into PASS");
        DriverDmaAudit audit;
        Failure audit_failure = Failure::None;
        Check(probe.AuditDma(640, 360, 320 * 180, 0,
                             &audit, &audit_failure) &&
              audit.records == 1 && audit.partial_size == 1 &&
              !audit.full_size && audit_failure == Failure::None,
              "failed evidence distinguishes hypothesis anomalies from malformed receipt integrity");
    }
    {
        FakeDevice fake;
        DeviceOps ops = fake.Ops();
        Probe probe(&ops);
        DmaExpectation expected_dma;
        expected_dma.width = 640;
        expected_dma.height = 360;
        expected_dma.y_done_words = 320 * 180;
        Check(probe.Prepare(empty_failure, 1) && probe.Arm() &&
              !probe.Finish(false, &expected_dma) &&
              probe.failure() == Failure::NativeRun &&
              !probe.failed_evidence_published() && fake.valid &&
              fake.events == "OASRC" && !probe.host_count() &&
              !probe.driver_count() && !probe.joined_count() &&
              !Exists(empty_failure),
              "zero-host zero-driver native failure cannot publish a header-only evidence TSV");
    }
    {
        FakeDevice fake;
        fake.records.push_back(Driver(0, 6, 12, true));
        fake.records.back().y_done_words = 16;
        fake.records.push_back(Driver(1, 7, 13, true));
        fake.records.back().y_done_words = 0;
        DeviceOps ops = fake.Ops();
        Probe probe(&ops);
        DmaExpectation expected_dma;
        expected_dma.width = 640;
        expected_dma.height = 360;
        expected_dma.y_done_words = 320 * 180;
        Check(probe.Prepare(eos_only_failure, 1) && probe.Arm() &&
              !probe.Finish(false, &expected_dma) &&
              probe.failure() == Failure::NativeRun &&
              !probe.failed_evidence_published() && fake.valid &&
              fake.events == "OASRC" && !probe.host_count() &&
              probe.driver_count() == 2 && !probe.joined_count() &&
              !Exists(eos_only_failure),
              "EOS-control-only seal cannot publish failure evidence without one exact joined row");
    }
    {
        const FakeDevice::SealFault faults[] = {
            FakeDevice::SealFault::RejectBeforeSeal,
            FakeDevice::SealFault::LoseReply,
        };
        for (unsigned index = 0; index < 2; ++index) {
            char seal_failure[512];
            std::snprintf(seal_failure, sizeof(seal_failure),
                          "%s/seal-fail-%u.tsv", directory, index);
            FakeDevice fake;
            fake.records.push_back(Driver(0, 6, 12));
            fake.seal_fault = faults[index];
            DeviceOps ops = fake.Ops();
            Probe probe(&ops);
            Check(probe.Prepare(seal_failure, 1) && probe.Arm() &&
                  probe.Record(Host(fake.records[0])) && !probe.Finish(true) &&
                  probe.failure() == Failure::Seal && fake.valid &&
                  fake.sealed && fake.exact_seal_calls == 2 &&
                  fake.events == "OASSC" && !Exists(seal_failure),
                  "failed or lost exact SEAL reply gets one identity-safe exact retry");
        }
    }
    {
        const FakeDevice::SealReplyFault faults[] = {
            FakeDevice::SealReplyFault::Generation,
            FakeDevice::SealReplyFault::Sealed,
            FakeDevice::SealReplyFault::Reserved,
        };
        for (unsigned index = 0; index < 3; ++index) {
            char malformed_seal[512];
            std::snprintf(malformed_seal, sizeof(malformed_seal),
                          "%s/malformed-seal-%u.tsv", directory, index);
            FakeDevice fake;
            fake.records.push_back(Driver(0, 6, 12));
            fake.seal_reply_fault = faults[index];
            DeviceOps ops = fake.Ops();
            Probe probe(&ops);
            Check(probe.Prepare(malformed_seal, 1) && probe.Arm() &&
                  probe.Record(Host(fake.records[0])) && !probe.Finish(true) &&
                  probe.failure() == Failure::Seal && fake.valid &&
                  fake.sealed && fake.exact_seal_calls == 2 &&
                  fake.events == "OASSC" && !Exists(malformed_seal),
                  "malformed exact SEAL reply fails and retries only the owned run ID");
        }
    }
    {
        char overflow_path[512];
        std::snprintf(overflow_path, sizeof(overflow_path),
                      "%s/driver-overflow.tsv", directory);
        FakeDevice fake;
        fake.records.push_back(Driver(0, 6, 12));
        fake.overflow_count = 1;
        DeviceOps ops = fake.Ops();
        Probe probe(&ops);
        Check(probe.Prepare(overflow_path, 1) && probe.Arm() &&
              probe.Record(Host(fake.records[0])) && !probe.Finish(true) &&
              probe.failure() == Failure::DriverOverflow && fake.valid &&
              fake.events == "OASC" && !Exists(overflow_path),
              "driver receipt overflow fails before READ or export");
    }
    {
        const FakeDevice::ReadReplyFault faults[] = {
            FakeDevice::ReadReplyFault::Generation,
            FakeDevice::ReadReplyFault::Start,
            FakeDevice::ReadReplyFault::Sealed,
            FakeDevice::ReadReplyFault::Reserved,
            FakeDevice::ReadReplyFault::Ordinal,
        };
        for (unsigned index = 0; index < 5; ++index) {
            char malformed_read[512];
            std::snprintf(malformed_read, sizeof(malformed_read),
                          "%s/malformed-read-%u.tsv", directory, index);
            FakeDevice fake;
            fake.records.push_back(Driver(0, 6, 12));
            fake.read_reply_fault = faults[index];
            DeviceOps ops = fake.Ops();
            Probe probe(&ops);
            Check(probe.Prepare(malformed_read, 1) && probe.Arm() &&
                  probe.Record(Host(fake.records[0])) && !probe.Finish(true) &&
                  probe.failure() == Failure::Read && fake.valid &&
                  fake.events == "OASRC" && !Exists(malformed_read),
                  "malformed READ reply fails without exporting evidence");
        }
    }
    {
        char close_failure[512];
        std::snprintf(close_failure, sizeof(close_failure), "%s/close-fail.tsv",
                      directory);
        FakeDevice fake;
        fake.records.push_back(Driver(0, 6, 12));
        fake.close_ok = false;
        DeviceOps ops = fake.Ops();
        Probe probe(&ops);
        Check(probe.Prepare(close_failure, 1) && probe.Arm() &&
              probe.Record(Host(fake.records[0])) && !probe.Finish(true) &&
              probe.failure() == Failure::ResearchClose &&
              fake.events == "OASRC" && !Exists(close_failure),
              "research-device close failure is distinct and prevents export");
    }
    {
        FakeDevice fake;
        fake.records.push_back(Driver(0, 7, 13));
        DeviceOps ops = fake.Ops();
        FailingWriter writer;
        OutputOps output_ops = writer.Ops();
        Probe probe(&ops, &output_ops);
        Check(probe.Prepare(partial, 1) && probe.Arm() &&
              probe.Record(Host(fake.records[0])) && !probe.Finish(true) &&
              probe.failure() == Failure::OutputWrite && writer.calls == 2,
              "short write followed by failure is reported");
        Check(!Exists(partial) &&
              CountPrefix(directory, "partial.tsv.partial.") == 0,
              "a failed export leaves neither a truncated final TSV nor staging file");
    }
    {
        FakeDevice fake;
        fake.records.push_back(Driver(0, 7, 13));
        DeviceOps ops = fake.Ops();
        RecoveringWriter writer;
        OutputOps output_ops = writer.Ops();
        Probe probe(&ops, &output_ops);
        Check(probe.Prepare(recovered, 1) && probe.Arm() &&
              probe.Record(Host(fake.records[0])) && probe.Finish(true) &&
              writer.calls >= 4 && Exists(recovered) &&
              CountPrefix(directory, "recovered.tsv.partial.") == 0,
              "EINTR and short writes recover before atomic final publication");
    }
    {
        FakeDevice fake;
        fake.records.push_back(Driver(0, 8, 14));
        DeviceOps ops = fake.Ops();
        Probe probe(&ops);
        Check(probe.Prepare(raced, 1) && probe.Arm() &&
              probe.Record(Host(fake.records[0])) &&
              symlink(output, raced) == 0 && !probe.Finish(true) &&
              probe.failure() == Failure::Path,
              "a pre-existing final-component symlink is not followed or overwritten");
        struct stat status = {};
        Check(lstat(raced, &status) == 0 && S_ISLNK(status.st_mode),
              "rejected final symlink remains untouched");
    }
    {
        FakeDevice fake;
        fake.records.push_back(Driver(0, 8, 14));
        DeviceOps ops = fake.Ops();
        PublishRacer writer;
        writer.path = publish_race;
        writer.target = output;
        OutputOps output_ops = writer.Ops();
        Probe probe(&ops, &output_ops);
        Check(probe.Prepare(publish_race, 1) && probe.Arm() &&
              probe.Record(Host(fake.records[0])) && !probe.Finish(true) &&
              probe.failure() == Failure::OutputPublish && writer.raced &&
              CountPrefix(directory, "publish-race.tsv.partial.") == 0,
              "a final-path race fails atomic no-replace publication and cleans staging");
        struct stat status = {};
        Check(lstat(publish_race, &status) == 0 && S_ISLNK(status.st_mode),
              "atomic publication never overwrites the racing final symlink");
    }

    unlink(publish_race);
    unlink(raced);
    unlink(recovered);
    unlink(partial);
    unlink(eos_only_failure);
    unlink(empty_failure);
    unlink(semantic_failure);
    unlink(invalid_failure);
    unlink(preserved_failure);
    unlink(native_failure);
    unlink(overflow);
    unlink(multi);
    unlink(output);
    rmdir(directory);

    std::printf("RX output receipt tests: %u checks %s\n", checks,
                okay ? "passed" : "failed");
    return okay ? 0 : 1;
}
