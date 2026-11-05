// SPDX-License-Identifier: LGPL-2.1-or-later
// Private scalar probe on product base50c4dce60773d12a7d6fc46217571a625c85805b.
// Ordinary backends exclude this header; API return status is not observed.
#ifndef CRYSTALHD_TEST_API_ADMISSION_OBSERVER_H
#define CRYSTALHD_TEST_API_ADMISSION_OBSERVER_H
#include <array>
#include <atomic>
#include <cerrno>
#include <cinttypes>
#include <cstdint>
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

namespace crystalhd_test_api_admission {
constexpr unsigned kCapacity = 32768;
enum class Api { BeginPicture, RenderPicture, EndPicture };
inline const char *Name(Api api) noexcept {
  switch (api) {
    case Api::BeginPicture: return "BeginPicture";
    case Api::RenderPicture: return "RenderPicture";
    case Api::EndPicture: return "EndPicture";
  }
  return "invalid";
}
struct Event {
  uint64_t driver = 0, context_argument = 0;
  uint64_t wall_begin = 0, wall_end = 0, cpu_begin = 0, cpu_end = 0;
  pid_t tid = 0, end_tid = 0;
  Api api = Api::BeginPicture;
  bool lock_acquired = false;
  std::atomic<bool> ready{false};
};
class State;
inline State *fork_state = nullptr;
class State {
 public:
  std::array<Event, kCapacity> events{};
  std::mutex gate;
  uint64_t attempted = 0, dropped = 0, drivers_opened = 0, drivers_closed = 0;
  unsigned live_drivers = 0;
  std::atomic<unsigned> inflight{0}, clock_failures{0}, lifecycle_failures{0};
  std::atomic<bool> fork_seen{false};
  pid_t pid = 0;
  int directory = -1;
  bool publishing = false;
  State() noexcept {
    const int saved = errno;
    pid = getpid();
    // Reuse the existing payload directory, with no new runtime option. Bind
    // the parent when this private DSO loads, before a child's first hook.
    const char *path = std::getenv("CRYSTALHD_TEST_CENTERED_PAYLOAD_DIR");
    if (path && path[0] == '/' && std::strlen(path) < PATH_MAX) {
      directory = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
      if (directory >= 0) {
        fork_state = this;
        if (pthread_atfork(ForkPrepare, nullptr, nullptr)) ++lifecycle_failures;
      }
    }
    errno = saved;
  }
  State(const State &) = delete;
  State &operator=(const State &) = delete;
  bool Active() const noexcept { return directory >= 0 && pid == getpid(); }
  static void ForkPrepare() noexcept {
    if (fork_state && fork_state->Active()) fork_state->fork_seen = true;
  }
  uint64_t Clock(clockid_t clock) noexcept {
    timespec value{};
    if (clock_gettime(clock, &value) || value.tv_sec < 0 || value.tv_nsec < 0 ||
        value.tv_nsec >= 1000000000 || static_cast<uint64_t>(value.tv_sec) >
        (UINT64_MAX - static_cast<uint64_t>(value.tv_nsec)) / UINT64_C(1000000000)) {
      ++clock_failures; return 0;
    }
    return static_cast<uint64_t>(value.tv_sec) * UINT64_C(1000000000) +
           static_cast<uint64_t>(value.tv_nsec);
  }
  void DriverOpened() noexcept {
    const int saved = errno;
    if (Active()) {
      std::lock_guard<std::mutex> hold(gate);
      if (publishing) ++lifecycle_failures;
      else { ++drivers_opened; ++live_drivers; }
    }
    errno = saved;
  }
  void DriverClosed() noexcept {
    const int saved = errno;
    if (Active()) {
      std::lock_guard<std::mutex> hold(gate);
      if (publishing || !live_drivers) ++lifecycle_failures;
      else { ++drivers_closed; --live_drivers; }
    }
    errno = saved;
  }
  bool Reserve(unsigned *index) noexcept {
    if (!Active()) return false; // No inherited-child observer mutex access.
    std::lock_guard<std::mutex> hold(gate);
    if (publishing) { ++lifecycle_failures; return false; }
    const uint64_t slot = attempted++;
    if (slot >= events.size()) { ++dropped; return false; }
    *index = static_cast<unsigned>(slot);
    ++inflight;
    return true;
  }
  static bool WriteAll(int fd, const char *data, size_t bytes) noexcept {
    while (bytes) {
      const ssize_t done = write(fd, data, bytes);
      if (done < 0 && errno == EINTR) continue;
      if (done <= 0) return false;
      data += done; bytes -= static_cast<size_t>(done);
    }
    return true;
  }
  void Publish() noexcept {
    const int saved = errno;
    if (!Active()) { errno = saved; return; }
    uint64_t total, lost, opened, closed;
    unsigned busy;
    {
      std::lock_guard<std::mutex> hold(gate);
      if (publishing) { errno = saved; return; }
      publishing = true;
      // Do not perform observer IO while a Driver owner survives. Missing
      // output means unavailable evidence, never successful retirement.
      if (live_drivers) { errno = saved; return; }
      total = attempted; lost = dropped; opened = drivers_opened; closed = drivers_closed;
      busy = inflight.load(std::memory_order_acquire);
    }
    char name[96], partial[112];
    const int length = std::snprintf(name, sizeof(name), "api-admission-%ld.jsonl", static_cast<long>(pid));
    if (length <= 0 || static_cast<size_t>(length) >= sizeof(name)) { errno = saved; return; }
    const int temporary = std::snprintf(partial, sizeof(partial), "%s.partial", name);
    if (temporary <= 0 || static_cast<size_t>(temporary) >= sizeof(partial)) { errno = saved; return; }
    const int fd = openat(directory, partial, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) { errno = saved; return; }
    char line[1536];
    int n = std::snprintf(line, sizeof(line),
        "{\"type\":\"header\",\"schema\":\"crystalhd-api-admission-v1\",\"pid\":%ld,"
        "\"fork_policy\":\"PARENT_PID_ONLY_CHILD_FORWARD_ONLY\",\"scope\":\"OWN_PUBLIC_API_MUTEX_ACQUISITION_ONLY\","
        "\"capacity\":%u,\"product_base_revision\":\"50c4dce60773d12a7d6fc46217571a625c85805b\","
        "\"clock_names\":{\"wall\":\"CLOCK_MONOTONIC\",\"cpu\":\"CLOCK_THREAD_CPUTIME_ID\"},"
        "\"context_identity\":\"UNVALIDATED_API_ARGUMENT\",\"api_return_status\":\"NOT_OBSERVED\","
        "\"publication\":\"DSO_OR_PROCESS_TEARDOWN_AFTER_ALL_OBSERVED_DRIVERS_RETIRED\"}\n",
        static_cast<long>(pid), kCapacity);
    bool ok = n > 0 && static_cast<size_t>(n) < sizeof(line) && WriteAll(fd, line, static_cast<size_t>(n));
    const unsigned count = total < events.size() ? static_cast<unsigned>(total) : kCapacity;
    unsigned written = 0, unpublished = 0;
    for (unsigned index = 0; index < count; ++index) {
      const Event &e = events[index];
      if (!e.ready.load(std::memory_order_acquire)) { ++unpublished; continue; }
      if (!ok) continue;
      n = std::snprintf(line, sizeof(line),
          "{\"type\":\"event\",\"seq\":%u,\"id\":%u,\"pid\":%ld,\"tid\":%ld,\"end_tid\":%ld,"
          "\"api\":\"%s\",\"driver_instance\":%" PRIu64 ",\"context_argument\":%" PRIu64 ","
          "\"wall_begin_ns\":%" PRIu64 ",\"wall_end_ns\":%" PRIu64 ","
          "\"cpu_begin_ns\":%" PRIu64 ",\"cpu_end_ns\":%" PRIu64 ","
          "\"lock_acquired\":%s,\"api_return_status\":null}\n",
          index, index + 1, static_cast<long>(pid), static_cast<long>(e.tid), static_cast<long>(e.end_tid),
          Name(e.api), e.driver, e.context_argument, e.wall_begin, e.wall_end, e.cpu_begin, e.cpu_end,
          e.lock_acquired ? "true" : "false");
      ok = n > 0 && static_cast<size_t>(n) < sizeof(line) && WriteAll(fd, line, static_cast<size_t>(n));
      if (ok) ++written;
    }
    const unsigned bad_clock = clock_failures.load(), bad_life = lifecycle_failures.load();
    const bool complete = ok && total && opened && opened == closed && !lost && !busy &&
                          !unpublished && !bad_clock && !bad_life;
    n = std::snprintf(line, sizeof(line),
        "{\"type\":\"footer\",\"events\":%u,\"attempted_events\":%" PRIu64 ",\"dropped\":%" PRIu64 ","
        "\"inflight\":%u,\"unpublished\":%u,\"clock_failures\":%u,\"lifecycle_failures\":%u,"
        "\"drivers_opened\":%" PRIu64 ",\"drivers_closed\":%" PRIu64 ",\"live_drivers\":0,"
        "\"fork_seen\":%s,\"complete\":%s}\n",
        written, total, lost, busy, unpublished, bad_clock, bad_life, opened, closed,
        fork_seen.load() ? "true" : "false", complete ? "true" : "false");
    ok = ok && n > 0 && static_cast<size_t>(n) < sizeof(line) &&
         WriteAll(fd, line, static_cast<size_t>(n));
    const bool stream_closed = close(fd) == 0; // Consuming close is never retried.
    // Only a successful write AND close can publish the final, without
    // replacing earlier evidence. Every failure preserves its partial file.
    if (ok && stream_closed && linkat(directory, partial, directory, name, 0) == 0)
      unlinkat(directory, partial, 0);
    errno = saved;
  }
  ~State() {
    const int saved = errno;
    Publish();
    if (fork_state == this) fork_state = nullptr;
    if (directory >= 0) close(directory);
    errno = saved;
  }
};
inline State state;
class Admission {
  Event *event_ = nullptr;
 public:
  Admission(Api api, uint64_t context_argument) noexcept {
    const int saved = errno;
    unsigned index;
    if (state.Reserve(&index)) {
      event_ = &state.events[index];
      event_->api = api; event_->context_argument = context_argument;
      event_->tid = static_cast<pid_t>(syscall(SYS_gettid));
      event_->wall_begin = state.Clock(CLOCK_MONOTONIC);
      event_->cpu_begin = state.Clock(CLOCK_THREAD_CPUTIME_ID);
    }
    errno = saved;
  }
  Admission(const Admission &) = delete;
  Admission &operator=(const Admission &) = delete;
  void Finish(bool acquired) noexcept {
    const int saved = errno;
    if (!event_ || !state.Active()) { errno = saved; return; }
    event_->cpu_end = state.Clock(CLOCK_THREAD_CPUTIME_ID);
    event_->wall_end = state.Clock(CLOCK_MONOTONIC);
    event_->end_tid = static_cast<pid_t>(syscall(SYS_gettid));
    event_->lock_acquired = acquired;
    if (!acquired || !event_->driver || event_->tid <= 0 || event_->tid != event_->end_tid ||
        event_->wall_end < event_->wall_begin || event_->cpu_end < event_->cpu_begin)
      ++state.lifecycle_failures;
    // Independent clocks may give CPU > wall on a short call. Preserve raw
    // endpoints; no clamp/tolerance or derived pure-lock/IO time is emitted.
    event_->ready.store(true, std::memory_order_release);
    state.inflight.fetch_sub(1, std::memory_order_release);
    event_ = nullptr; errno = saved;
  }
  void Acquired(uint64_t driver) noexcept {
    if (event_ && state.Active()) event_->driver = driver;
    Finish(true);
  }
  ~Admission() { Finish(false); }
};
} // namespace crystalhd_test_api_admission
#define CH_API_ADMISSION_SCOPE(name, api, context) \
  crystalhd_test_api_admission::Admission name( \
      crystalhd_test_api_admission::Api::api, (context))
#define CH_API_ADMISSION_ACQUIRED(name, driver) (name).Acquired((driver))
#define CH_API_ADMISSION_DRIVER_OPENED() crystalhd_test_api_admission::state.DriverOpened()
#define CH_API_ADMISSION_DRIVER_CLOSED() crystalhd_test_api_admission::state.DriverClosed()
#endif
