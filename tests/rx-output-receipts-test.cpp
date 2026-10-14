/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "rx-output-receipts.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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

    std::vector<crystalhd_fw_research_rx_receipt> records;
    std::string events;
    bool valid = true;
    bool close_ok = true;
    bool sealed = false;
    SealFault seal_fault = SealFault::None;
    bool malformed_seal_reply = false;
    bool malformed_read_reply = false;
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
            if (!recover && fake->malformed_seal_reply)
                seal->generation++;
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
            if (fake->malformed_read_reply)
                read->generation++;
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

static bool Exists(const char *path, off_t *size = nullptr, mode_t *mode = nullptr)
{
    struct stat status = {};
    if (lstat(path, &status)) return false;
    if (size) *size = status.st_size;
    if (mode) *mode = status.st_mode & 0777;
    return true;
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
    char output[512], native_failure[512], partial[512], raced[512];
    char multi[512], overflow[512];
    std::snprintf(output, sizeof(output), "%s/receipts.tsv", directory);
    std::snprintf(native_failure, sizeof(native_failure), "%s/native-fail.tsv",
                  directory);
    std::snprintf(partial, sizeof(partial), "%s/partial.tsv", directory);
    std::snprintf(raced, sizeof(raced), "%s/raced.tsv", directory);
    std::snprintf(multi, sizeof(multi), "%s/multi.tsv", directory);
    std::snprintf(overflow, sizeof(overflow), "%s/overflow.tsv", directory);

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
        off_t bytes = 0;
        mode_t mode = 0;
        Check(Exists(output, &bytes, &mode) && bytes > 100 && mode == 0600,
              "successful joined evidence is a nonempty mode-0600 file");
        FILE *file = std::fopen(output, "rb");
        char text[4096] = {};
        const size_t read = file ? std::fread(text, 1, sizeof(text) - 1, file) : 0;
        if (file) std::fclose(file);
        Check(read && std::strstr(text, "receipt_generation\treceipt_run_id") &&
              std::strstr(text, "normalized_firmware_timestamp") &&
              std::strstr(text, "driver_picture_number\tdriver_session_number\t") &&
              std::strstr(text, "driver_width\tdriver_height\t") &&
              std::strstr(text, "driver_y_done_words\tdriver_uv_done_words\t") &&
              std::strstr(text, "7\t9\t0\t") &&
              std::strstr(text, "\t100000\t"),
              "TSV preserves sealed-run provenance and normalized token without claiming a generic ABI");
        Probe collision(&ops);
        Check(!collision.Prepare(output, 1) &&
              collision.failure() == Failure::Path,
              "an existing output is rejected before research-device open");
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
        char malformed_seal[512];
        std::snprintf(malformed_seal, sizeof(malformed_seal),
                      "%s/malformed-seal.tsv", directory);
        FakeDevice fake;
        fake.records.push_back(Driver(0, 6, 12));
        fake.malformed_seal_reply = true;
        DeviceOps ops = fake.Ops();
        Probe probe(&ops);
        Check(probe.Prepare(malformed_seal, 1) && probe.Arm() &&
              probe.Record(Host(fake.records[0])) && !probe.Finish(true) &&
              probe.failure() == Failure::Seal && fake.valid && fake.sealed &&
              fake.exact_seal_calls == 2 && fake.events == "OASSC" &&
              !Exists(malformed_seal),
              "malformed exact SEAL reply fails and retries only the owned run ID");
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
        char malformed_read[512];
        std::snprintf(malformed_read, sizeof(malformed_read),
                      "%s/malformed-read.tsv", directory);
        FakeDevice fake;
        fake.records.push_back(Driver(0, 6, 12));
        fake.malformed_read_reply = true;
        DeviceOps ops = fake.Ops();
        Probe probe(&ops);
        Check(probe.Prepare(malformed_read, 1) && probe.Arm() &&
              probe.Record(Host(fake.records[0])) && !probe.Finish(true) &&
              probe.failure() == Failure::Read && fake.valid &&
              fake.events == "OASRC" && !Exists(malformed_read),
              "malformed READ reply fails without exporting evidence");
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
        off_t bytes = 0;
        Check(Exists(partial, &bytes) && bytes == 8,
              "a failed export preserves its partial file for diagnosis");
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
              "a final-component symlink race is not followed or overwritten");
        struct stat status = {};
        Check(lstat(raced, &status) == 0 && S_ISLNK(status.st_mode),
              "failed symlink race leaves the link untouched");
    }

    unlink(raced);
    unlink(partial);
    unlink(native_failure);
    unlink(overflow);
    unlink(multi);
    unlink(output);
    rmdir(directory);

    std::printf("RX output receipt tests: %u checks %s\n", checks,
                okay ? "passed" : "failed");
    return okay ? 0 : 1;
}
