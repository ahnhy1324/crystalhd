/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef CRYSTALHD_TESTS_RX_OUTPUT_RECEIPTS_H
#define CRYSTALHD_TESTS_RX_OUTPUT_RECEIPTS_H

#include <cstddef>
#include <cstdint>
#include <sys/types.h>
#include <vector>

#include "crystalhd_fw_research.h"

namespace rx_output_receipts {

/* This is a library-drain/research-driver evidence join, not a new generic
 * libcrystalhd ABI or a timestamp contract for other applications.
 */

enum class Failure {
    None,
    Argument,
    Path,
    Allocation,
    DeviceOpen,
    Arm,
    HostOverflow,
    HostClock,
    Seal,
    Read,
    ResearchClose,
    DriverOverflow,
    DriverRecord,
    ClockInversion,
    EosMismatch,
    Timestamp,
    Ambiguous,
    Unmatched,
    NativeRun,
    OutputOpen,
    OutputMode,
    OutputWrite,
    OutputClose,
};

const char *FailureName(Failure failure);

struct HostReceipt {
    uint64_t ordinal = 0;
    uint64_t return_ns = 0;
    uint64_t token = 0;
    uint32_t ready_before = 0;
    uint32_t picture_number = 0;
    uint32_t session_number = 0;
    uint32_t picture_flags = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t y_done_words = 0;
    uint32_t uv_done_words = 0;
    uint32_t pout_flags = 0;
    uint32_t b422_mode = 0;
    uint32_t eos = 0;
};

struct JoinedReceipt {
    HostReceipt host;
    crystalhd_fw_research_rx_receipt driver;
    uint64_t normalized_firmware_timestamp = 0;
};

struct DeviceOps {
    void *opaque = nullptr;
    int (*open_device)(void *, const char *, int) = nullptr;
    int (*ioctl_device)(void *, int, unsigned long, void *) = nullptr;
    int (*close_device)(void *, int) = nullptr;
};

struct OutputOps {
    void *opaque = nullptr;
    ssize_t (*write_file)(void *, int, const void *, size_t) = nullptr;
};

bool AbsoluteNewPathSyntax(const char *path);
bool OutputPathAvailable(const char *path);
bool NormalizeFirmwareTimestamp(uint64_t raw, uint64_t *normalized);
uint32_t DriverCapacity(unsigned expected_outputs);
size_t HostCapacity(unsigned expected_outputs);

bool JoinReceipts(const HostReceipt *host, size_t host_count,
                  const crystalhd_fw_research_rx_receipt *driver,
                  size_t driver_count, JoinedReceipt *joined,
                  size_t joined_capacity, uint8_t *driver_matched,
                  size_t matched_capacity, size_t *joined_count,
                  Failure *failure);

bool WriteTsv(const char *path, const JoinedReceipt *rows, size_t row_count,
              uint64_t generation, uint64_t run_id, const OutputOps *ops,
              Failure *failure);

class Probe {
public:
    Probe(const DeviceOps *device_ops = nullptr,
          const OutputOps *output_ops = nullptr);
    Probe(const Probe &) = delete;
    Probe &operator=(const Probe &) = delete;
    ~Probe();

    bool Prepare(const char *output_path, unsigned expected_outputs);
    bool Arm();
    bool Record(const HostReceipt &receipt);
    bool Finish(bool native_run_ok);

    Failure failure() const { return failure_; }
    size_t host_count() const { return host_count_; }
    size_t driver_count() const { return driver_count_; }
    size_t joined_count() const { return joined_count_; }
    uint64_t generation() const { return arm_.generation; }
    uint64_t run_id() const { return arm_.run_id; }

private:
    bool SealAndRead();
    bool CloseDevice();
    void Fail(Failure failure);

    DeviceOps device_ops_;
    OutputOps output_ops_;
    const char *output_path_ = nullptr;
    int fd_ = -1;
    bool prepared_ = false;
    bool armed_ = false;
    Failure failure_ = Failure::None;
    crystalhd_fw_research_rx_receipt_arm arm_ = {};
    std::vector<HostReceipt> host_;
    std::vector<crystalhd_fw_research_rx_receipt> driver_;
    std::vector<JoinedReceipt> joined_;
    std::vector<uint8_t> driver_matched_;
    size_t host_count_ = 0;
    size_t driver_count_ = 0;
    size_t joined_count_ = 0;
};

} // namespace rx_output_receipts

#endif
