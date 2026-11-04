// SPDX-License-Identifier: LGPL-2.1-or-later
// OWN private diagnostic only. No ordinary-backend state or per-read file I/O.
#ifndef CRYSTALHD_TEST_CENTERED_READBACK_OBSERVER_H
#define CRYSTALHD_TEST_CENTERED_READBACK_OBSERVER_H
#include <array>
#include <atomic>
#include <cerrno>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <limits.h>
#include <mutex>
#include <pthread.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>
#include <va/va.h>

namespace crystalhd_test_centered_readback {
constexpr unsigned kSlots = 2, kWidth = 1920, kHeight = 1088;
constexpr size_t kBytes = size_t{kWidth} * kHeight * 3 / 2;
enum class Route : unsigned { ImmutableSmall, CpuPrimaryFullCanvas };
inline const char* RouteName(Route route) noexcept {
  return route == Route::ImmutableSmall ? "IMMUTABLE_SMALL_RECONSTRUCTION"
                                       : "CPU_PRIMARY_FULL_CANVAS";
}
struct Slot {
  Route route = Route::ImmutableSmall;
  uint64_t call = 0, driver = 0, context = 0, generation = 0, token = 0;
  uint64_t identity = 0, ordinal = 0, surface = 0, owner = 0, monotonic_ns = 0;
  pid_t tid = 0;
  uint32_t image = 0, buffer = 0, width = 0, height = 0, bytes = 0;
  uint32_t pitch[2]{}, offset[2]{};
  std::array<uint8_t, kBytes> pixels{};
};
class State;
inline State* fork_state = nullptr;
class State {
 public:
  std::array<Slot, kSlots> slots{};
  std::mutex gate;
  std::atomic<bool> fork_seen{false};
  uint64_t calls = 0, valid_calls = 0, invalid = 0, clock_failures = 0;
  std::array<uint64_t, 2> route_calls{}, route_valid{}, route_unbound{};
  unsigned captured = 0;
  pid_t pid = 0;
  bool enabled = false, publishing = false;
  char prefix[PATH_MAX]{};
  State() noexcept {
    const int saved = errno;
    pid = getpid();
    const char* marker = std::getenv("CRYSTALHD_TEST_REPLAY_DISPOSITION");
    const char* path = std::getenv("CRYSTALHD_TEST_REPLAY_DISPOSITION_PREFIX");
    if (marker && std::strcmp(marker, "v1") == 0 && path && path[0] == '/' &&
        std::strlen(path) < sizeof(prefix)) {
      const char* slash = std::strrchr(path, '/');
      const size_t directory = static_cast<size_t>(slash - path) + 1;
      constexpr char name[] = "centered-readback";
      if (directory + sizeof(name) <= sizeof(prefix)) {
        std::memcpy(prefix, path, directory);
        std::memcpy(prefix + directory, name, sizeof(name));
        enabled = true;
        fork_state = this;
        if (pthread_atfork(ForkPrepare, nullptr, nullptr)) ++invalid;
      }
    }
    errno = saved;
  }
  State(const State&) = delete;
  State& operator=(const State&) = delete;
  bool Active() const noexcept { return enabled && pid == getpid(); }
  static void ForkPrepare() noexcept {
    if (fork_state && fork_state->Active()) fork_state->fork_seen = true;
  }
  template<typename Fields>
  void Capture(Route route, uint64_t driver, const Fields& f, const VAImage& image,
               const uint8_t* bytes) noexcept {
    const int saved = errno;
    if (!Active()) { errno = saved; return; }
    std::lock_guard<std::mutex> lock(gate);
    if (publishing) { errno = saved; return; }
    const unsigned route_index = static_cast<unsigned>(route);
    if (route_index >= route_calls.size()) { ++invalid; errno = saved; return; }
    if (calls == UINT64_MAX) { ++invalid; errno = saved; return; }
    ++calls;  // Successful API reads, never a source/M/presentation count.
    ++route_calls[route_index];
    const uint64_t length = uint64_t{image.width} * image.height * 3 / 2;
    if (!bytes ||
        image.format.fourcc != VA_FOURCC_NV12 || image.num_planes != 2 ||
        !image.width || image.width > kWidth || (image.width & 1) ||
        !image.height || image.height > kHeight || (image.height & 1) ||
        image.pitches[0] != image.width || image.pitches[1] != image.width ||
        image.offsets[0] != 0 || image.offsets[1] != uint64_t{image.width} * image.height ||
        image.data_size != length || length > kBytes) {
      ++invalid; errno = saved; return;
    }
    if (!driver || !f.context || !f.generation || !f.token || !f.decode_identity ||
        !f.submission_ordinal || !f.surface || !f.owner) {
      // Legal cold/unbound full-image API reads are counted, but cannot
      // qualify an original or supply a selected decoded-pixel snapshot.
      ++route_unbound[route_index]; errno = saved; return;
    }
    ++valid_calls;
    ++route_valid[route_index];
    if (captured == kSlots) { errno = saved; return; }
    timespec now{};
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0 || now.tv_sec < 0 ||
        now.tv_nsec < 0 || now.tv_nsec >= 1000000000) {
      ++clock_failures; errno = saved; return;
    }
    Slot& slot = slots[captured];
    slot.route = route;
    slot.call = calls; slot.driver = driver; slot.context = f.context;
    slot.generation = f.generation; slot.token = f.token;
    slot.identity = f.decode_identity; slot.ordinal = f.submission_ordinal;
    slot.surface = f.surface; slot.owner = f.owner;
    slot.monotonic_ns = static_cast<uint64_t>(now.tv_sec) * 1000000000ULL + static_cast<uint64_t>(now.tv_nsec);
    slot.tid = static_cast<pid_t>(syscall(SYS_gettid));
    slot.image = image.image_id; slot.buffer = image.buf;
    slot.width = image.width; slot.height = image.height; slot.bytes = static_cast<uint32_t>(length);
    slot.pitch[0] = image.pitches[0]; slot.pitch[1] = image.pitches[1];
    slot.offset[0] = image.offsets[0]; slot.offset[1] = image.offsets[1];
    // bytes belongs to the independent completed image Buffer. No GTT/surface
    // planes are touched after their original READ leases have ended.
    std::memcpy(slot.pixels.data(), bytes, static_cast<size_t>(length));
    ++captured;
    errno = saved;
  }
  static bool WriteAll(int fd, const char* data, size_t length) noexcept {
    while (length) {
      const ssize_t count = write(fd, data, length);
      if (count < 0 && errno == EINTR) continue;
      if (count <= 0) return false;
      data += count; length -= static_cast<size_t>(count);
    }
    return true;
  }
  static bool Blob(const char* path, const Slot& slot) noexcept {
    const int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) return false;
    const bool written = WriteAll(fd, reinterpret_cast<const char*>(slot.pixels.data()), slot.bytes);
    return close(fd) == 0 && written;
  }
  void Publish() noexcept {
    const int saved = errno;
    if (!Active()) { errno = saved; return; }
    std::lock_guard<std::mutex> lock(gate);
    if (publishing) { errno = saved; return; }
    publishing = true;
    char final_path[PATH_MAX], temporary[PATH_MAX], line[3072];
    const int name = std::snprintf(final_path, sizeof(final_path), "%s-%ld.jsonl", prefix, static_cast<long>(pid));
    const int temp = std::snprintf(temporary, sizeof(temporary), "%s-%ld.jsonl.partial", prefix, static_cast<long>(pid));
    if (name <= 0 || static_cast<size_t>(name) >= sizeof(final_path) ||
        temp <= 0 || static_cast<size_t>(temp) >= sizeof(temporary)) { errno = saved; return; }
    const int fd = open(temporary, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) { errno = saved; return; }
    int n = std::snprintf(line, sizeof(line),
        "{\"type\":\"header\",\"schema\":\"crystalhd-centered-readback-v2\",\"pid\":%ld,"
        "\"fork_policy\":\"PARENT_PID_ONLY_CHILD_FORWARD_ONLY\",\"snapshot_capacity\":%u,\"max_snapshot_bytes\":%zu,"
        "\"selection\":\"FIRST_TWO_VALID_FULL_BINDING_API_READS_ALL_ROUTES_NOT_COMPOSE_SLOT_SELECTOR\","
        "\"capture_point\":\"AFTER_FULL_IMAGE_COPY_AND_READ_END_BEFORE_API_RETURN\","
        "\"count_kind\":\"SUCCESSFUL_API_READS_BY_ROUTE_NEVER_SOURCE_OR_M\","
        "\"unbound_scope\":\"COUNTED_NOT_SELECTED_NOT_ORIGINAL_QUALIFICATION\","
        "\"publication_scope\":\"OWNER_DSO_OR_PROCESS_EXIT_INDEPENDENT_ARRAYS\","
        "\"driver_retirement_observed\":false,\"pixel_comparison\":\"REQUIRES_OFFLINE_EXACT_BINDING_JOIN\"}\n",
        static_cast<long>(pid), kSlots, kBytes);
    bool stream_ok = n > 0 && static_cast<size_t>(n) < sizeof(line) && WriteAll(fd, line, static_cast<size_t>(n));
    bool blobs_ok = true;
    for (unsigned i = 0; i != captured && stream_ok; ++i) {
      const Slot& slot = slots[i]; char path[PATH_MAX], relative[96]{};
      const int size = std::snprintf(path, sizeof(path), "%s-%ld-s%u.nv12", prefix, static_cast<long>(pid), i);
      const int rel = std::snprintf(relative, sizeof(relative), "centered-readback-%ld-s%u.nv12", static_cast<long>(pid), i);
      const bool written = size > 0 && static_cast<size_t>(size) < sizeof(path) &&
                           rel > 0 && static_cast<size_t>(rel) < sizeof(relative) && Blob(path, slot);
      blobs_ok = blobs_ok && written;
      n = std::snprintf(line, sizeof(line),
          "{\"type\":\"snapshot\",\"slot\":%u,\"route\":\"%s\",\"call_ordinal\":%" PRIu64 ",\"pid\":%ld,\"tid\":%ld,\"monotonic_ns\":%" PRIu64 ","
          "\"driver_instance\":%" PRIu64 ",\"context\":%" PRIu64 ",\"generation\":%" PRIu64 ",\"token\":%" PRIu64 ","
          "\"decode_identity\":%" PRIu64 ",\"submission_ordinal\":%" PRIu64 ",\"surface\":%" PRIu64 ",\"owner\":%" PRIu64 ","
          "\"image_id\":%u,\"buffer_id\":%u,\"fourcc\":%u,\"width\":%u,\"height\":%u,\"num_planes\":2,"
          "\"pitches\":[%u,%u],\"offsets\":[%u,%u],\"bytes\":%u,\"file\":\"%s\",\"written\":%s}\n",
          i, RouteName(slot.route), slot.call, static_cast<long>(pid), static_cast<long>(slot.tid), slot.monotonic_ns,
          slot.driver, slot.context, slot.generation, slot.token, slot.identity, slot.ordinal,
          slot.surface, slot.owner, slot.image, slot.buffer, static_cast<unsigned>(VA_FOURCC_NV12), slot.width, slot.height,
          slot.pitch[0], slot.pitch[1], slot.offset[0], slot.offset[1], slot.bytes, relative,
          written ? "true" : "false");
      stream_ok = n > 0 && static_cast<size_t>(n) < sizeof(line) && WriteAll(fd, line, static_cast<size_t>(n));
    }
    const bool complete = stream_ok && blobs_ok && !invalid && !clock_failures;
    n = std::snprintf(line, sizeof(line),
        "{\"type\":\"footer\",\"successful_api_read_calls\":%" PRIu64 ","
        "\"successful_fast_api_read_calls\":%" PRIu64 ",\"successful_cpu_primary_full_canvas_api_read_calls\":%" PRIu64 ","
        "\"valid_fast_full_binding_calls\":%" PRIu64 ",\"valid_cpu_primary_full_binding_calls\":%" PRIu64 ","
        "\"unbound_fast_api_read_calls\":%" PRIu64 ",\"unbound_cpu_primary_api_read_calls\":%" PRIu64 ","
        "\"valid_full_binding_calls\":%" PRIu64 ","
        "\"snapshots\":%u,\"invalid_binding_or_layout\":%" PRIu64 ",\"clock_failures\":%" PRIu64 ",\"fork_seen\":%s,"
        "\"count_window\":\"BEFORE_OWNER_EXIT_CAPTURE_GATE_CLOSE\",\"complete\":%s}\n",
        calls, route_calls[0], route_calls[1], route_valid[0], route_valid[1],
        route_unbound[0], route_unbound[1], valid_calls, captured, invalid,
        clock_failures, fork_seen ? "true" : "false", complete ? "true" : "false");
    stream_ok = stream_ok && n > 0 && static_cast<size_t>(n) < sizeof(line) && WriteAll(fd, line, static_cast<size_t>(n));
    const bool closed = close(fd) == 0;
    // Publish without replacing any earlier receipt. Failed/partial writes are
    // retained under .partial and cannot masquerade as the final manifest.
    if (stream_ok && closed && link(temporary, final_path) == 0) unlink(temporary);
    errno = saved;
  }
  ~State() { Publish(); if (fork_state == this) fork_state = nullptr; }
};
// Bind ownership at private DSO load, not at a child's first inherited hook.
inline State state;
template<typename Fields>
inline void Capture(uint64_t driver, const Fields& fields, const VAImage& image,
                    const uint8_t* bytes) noexcept {
  state.Capture(Route::ImmutableSmall, driver, fields, image, bytes);
}
template<typename Fields>
inline void CaptureCpuPrimary(uint64_t driver, const Fields& fields,
                              const VAImage& image, const uint8_t* bytes) noexcept {
  state.Capture(Route::CpuPrimaryFullCanvas, driver, fields, image, bytes);
}
} // namespace crystalhd_test_centered_readback
#endif
