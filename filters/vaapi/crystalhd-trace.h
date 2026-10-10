// SPDX-License-Identifier: LGPL-2.1-or-later
#ifndef CRYSTALHD_VAAPI_TRACE_H_
#define CRYSTALHD_VAAPI_TRACE_H_

#include <atomic>
#include <chrono>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

#include <unistd.h>

namespace crystalhd_vaapi_trace {

enum class Event {
  SubmitBind,
  BindingEnd,
  InputSent,
  PrefeedSent,
  OutputDequeued,
  Materialized,
  SyncEnter,
  SyncExit,
  ExportEnter,
  ExportExit,
  ReadExportReuseImplicitFenceProbe,
  VppCapture,
  VppCommit,
  VppCancel,
  ContextDestroy,
  SurfaceDestroy,
  ContextReset,
  SessionDestroy,
};

struct Fields {
  uint64_t context = 0;
  uint64_t generation = 0;
  uint64_t surface = 0;
  uint64_t token = 0;
  uint64_t decode_identity = 0;
  uint64_t owner = 0;
  uint64_t submission_ordinal = 0;
  uint64_t operation = 0;
  uint64_t duration_ns = 0;
  int64_t outcome = 0;
  // ReadExportReuseImplicitFenceProbe calls poll(POLLOUT, timeout=0) at the
  // next client reuse boundary. It does not wait for fence signaling, but the
  // kernel may sleep while acquiring a contended dma_resv lock; duration_ns
  // measures that diagnostic perturbation. It neither proves export-time
  // reader completion nor covers explicit-sync or unfenced access. The
  // operation is the latest successful explicit read-only export; repeated
  // read exports observe aggregate implicit fences on the same backing.
  uint64_t identified_exported_object_count = 0;
  uint64_t pollout_count = 0;
  uint64_t not_pollout_count = 0;
  uint64_t probe_error_count = 0;
  int64_t probe_errno = 0;
};

// Opt-in diagnostic output only. With CRYSTALHD_VAAPI_TRACE unset (or not
// exactly "1"), Emit is one predictable branch and produces no output.
class Trace {
 public:
  explicit Trace(int output_fd = STDERR_FILENO)
      : enabled_(EnvironmentEnabled()), output_fd_(output_fd),
        session_(enabled_ ? NextSession() : 0) {}

  Trace(const Trace &) = delete;
  Trace &operator=(const Trace &) = delete;

  bool enabled() const { return enabled_; }
  uint64_t session() const { return session_; }

  void Emit(Event event, const Fields &fields = {}) {
    if (!enabled_)
      return;

    // Serialize formatting and the single write so driver worker/frontend
    // threads cannot splice JSON records. Capture the clock under the same
    // lock, making sequence and timestamp order agree within one process.
    std::lock_guard<std::mutex> lock(OutputMutex());
    const uint64_t sequence = ++sequence_;
    const uint64_t timestamp_ns = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
    char line[768];
    const int length = std::snprintf(
        line, sizeof(line),
        "{\"schema\":\"crystalhd-vaapi-trace-v2\","
        "\"ts_monotonic_ns\":%llu,\"session\":%llu,"
        "\"driver_instance\":%llu,\"pid\":%lld,\"seq\":%llu,"
        "\"event\":\"%s\",\"context\":%llu,\"generation\":%llu,"
        "\"surface\":%llu,\"token\":%llu,\"decode_identity\":%llu,"
        "\"owner\":%llu,\"submission_ordinal\":%llu,"
        "\"operation\":%llu,\"duration_ns\":%llu,"
        "\"outcome\":%lld,\"identified_exported_object_count\":%llu,"
        "\"pollout_count\":%llu,\"not_pollout_count\":%llu,"
        "\"probe_error_count\":%llu,\"probe_errno\":%lld}\n",
        static_cast<unsigned long long>(timestamp_ns),
        static_cast<unsigned long long>(session_),
        static_cast<unsigned long long>(session_),
        static_cast<long long>(getpid()),
        static_cast<unsigned long long>(sequence), EventName(event),
        static_cast<unsigned long long>(fields.context),
        static_cast<unsigned long long>(fields.generation),
        static_cast<unsigned long long>(fields.surface),
        static_cast<unsigned long long>(fields.token),
        static_cast<unsigned long long>(fields.decode_identity),
        static_cast<unsigned long long>(fields.owner),
        static_cast<unsigned long long>(fields.submission_ordinal),
        static_cast<unsigned long long>(fields.operation),
        static_cast<unsigned long long>(fields.duration_ns),
        static_cast<long long>(fields.outcome),
        static_cast<unsigned long long>(
            fields.identified_exported_object_count),
        static_cast<unsigned long long>(fields.pollout_count),
        static_cast<unsigned long long>(fields.not_pollout_count),
        static_cast<unsigned long long>(fields.probe_error_count),
        static_cast<long long>(fields.probe_errno));
    if (length <= 0 || static_cast<size_t>(length) >= sizeof(line))
      return;
    size_t written = 0;
    while (written < static_cast<size_t>(length)) {
      const ssize_t result = write(output_fd_, line + written,
                                   static_cast<size_t>(length) - written);
      if (result > 0) {
        written += static_cast<size_t>(result);
        continue;
      }
      if (result < 0 && errno == EINTR)
        continue;
      break;
    }
  }

 private:
  static bool EnvironmentEnabled() {
    const char *value = std::getenv("CRYSTALHD_VAAPI_TRACE");
    return value != nullptr && std::strcmp(value, "1") == 0;
  }

  static uint64_t NextSession() {
    static std::atomic<uint64_t> next{1};
    uint64_t result = next.fetch_add(1, std::memory_order_relaxed);
    if (result == 0)
      result = next.fetch_add(1, std::memory_order_relaxed);
    return result;
  }

  static std::mutex &OutputMutex() {
    static std::mutex mutex;
    return mutex;
  }

  static const char *EventName(Event event) {
    switch (event) {
      case Event::SubmitBind: return "submit_bind";
      case Event::BindingEnd: return "binding_end";
      case Event::InputSent: return "input_sent";
      case Event::PrefeedSent: return "prefeed_sent";
      case Event::OutputDequeued: return "output_dequeued";
      case Event::Materialized: return "materialized";
      case Event::SyncEnter: return "sync_enter";
      case Event::SyncExit: return "sync_exit";
      case Event::ExportEnter: return "export_enter";
      case Event::ExportExit: return "export_exit";
      case Event::ReadExportReuseImplicitFenceProbe:
        return "read_export_reuse_implicit_fence_probe";
      case Event::VppCapture: return "vpp_capture";
      case Event::VppCommit: return "vpp_commit";
      case Event::VppCancel: return "vpp_cancel";
      case Event::ContextDestroy: return "context_destroy";
      case Event::SurfaceDestroy: return "surface_destroy";
      case Event::ContextReset: return "context_reset";
      case Event::SessionDestroy: return "session_destroy";
    }
    return "unknown";
  }

  bool enabled_ = false;
  int output_fd_ = STDERR_FILENO;
  uint64_t session_ = 0;
  uint64_t sequence_ = 0;
};

}  // namespace crystalhd_vaapi_trace

#endif  // CRYSTALHD_VAAPI_TRACE_H_
