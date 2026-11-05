// SPDX-License-Identifier: LGPL-2.1-or-later
// Real Linux VM controls only: no decoder, DRM device, external SDK or media.
#include "../filters/vaapi/crystalhd-cow-snapshot.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <stdexcept>
#include <string>
#include <thread>

namespace cow = crystalhd_cow_snapshot;
static unsigned checks = 0;
static void Require(bool condition, const char *message) {
  ++checks;
  if (!condition) throw std::runtime_error(message);
}
static bool Filled(const uint8_t *data, size_t bytes, uint8_t value) {
  return std::all_of(data, data + bytes, [value](uint8_t c) { return c == value; });
}
static int FdCount() {
  DIR *directory = opendir("/proc/self/fd");
  if (!directory) return -1;
  int count = 0;
  while (const auto *entry = readdir(directory))
    if (entry->d_name[0] != '.') ++count;
  closedir(directory);
  return count;
}
static void AllocationAndSeals(size_t page) {
  const int fds = FdCount();
  Require(fds >= 0 && cow::LiveBytes() == 0, "start with no owned primary or descriptor");
  errno = E2BIG;
  auto canvas = cow::Canvas::Create(page + 7);
  Require(canvas && errno == E2BIG && canvas->Size() == page + 7 &&
              canvas->Capacity() == page * 2 && cow::LiveBytes() == page * 2 &&
              Filled(canvas->Data(), canvas->Size(), 0),
          "real shared primary is zero, rounded, charged and success preserves errno");
  DIR *directory = opendir("/proc/self/fd");
  Require(directory != nullptr, "inspect only this process's newly owned descriptor");
  unsigned found = 0;
  while (const auto *entry = readdir(directory)) {
    if (entry->d_name[0] == '.') continue;
    const std::string path = std::string("/proc/self/fd/") + entry->d_name;
    char name[128] = {};
    const ssize_t n = readlink(path.c_str(), name, sizeof(name) - 1);
    if (n <= 0 || !strstr(name, "memfd:crystalhd-canvas")) continue;
    const int fd = std::atoi(entry->d_name);
    const int seals = fcntl(fd, F_GET_SEALS);
    Require(seals >= 0 && (seals & (F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL)) ==
                (F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL) &&
                (fcntl(fd, F_GETFD) & FD_CLOEXEC),
            "internal fd is close-on-exec with immutable size/seal policy");
    Require(ftruncate(fd, static_cast<off_t>(page)) == -1 && errno == EPERM &&
                ftruncate(fd, static_cast<off_t>(page * 3)) == -1 && errno == EPERM,
            "shrink/grow cannot invalidate private VMAs or expose a new tail");
    ++found;
  }
  closedir(directory);
  Require(found == 1, "one primary descriptor, no public helper fd accessor");
  canvas.reset();
  Require(cow::LiveBytes() == 0 && FdCount() == fds, "last primary releases real mapping/fd/charge");
  Require(!cow::Canvas::Create(0) && !cow::Canvas::Create(SIZE_MAX) &&
              !cow::Canvas::Create(static_cast<size_t>(cow::BudgetLimit) + 1),
          "zero/overflow/over-budget requests conservatively return unavailable");
  Require(cow::LiveBytes() == 0 && FdCount() == fds, "failed creation leaks no primary or fd");
}

static void PrivateOnlyNegative(size_t page) {
#if defined(SYS_memfd_create) && defined(MFD_CLOEXEC)
  const int fd = static_cast<int>(syscall(SYS_memfd_create, "cow-negative", MFD_CLOEXEC));
  Require(fd >= 0 && ftruncate(fd, static_cast<off_t>(page)) == 0,
          "create an independent raw MAP_PRIVATE-only negative control");
  void *shared = mmap(nullptr, page, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  void *image = mmap(nullptr, page, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
  Require(shared != MAP_FAILED && image != MAP_FAILED, "map negative control without populate");
  memset(shared, 0x21, page);
  volatile uint8_t *private_bytes = static_cast<uint8_t *>(image);
  Require(private_bytes[0] == 0x21, "negative view first observes original file page");
  memset(shared, 0x43, page);
  const bool changed = private_bytes[0] == 0x43;
  munmap(image, page); munmap(shared, page); close(fd);
  Require(changed, "on this Linux kernel plain MAP_PRIVATE fails the retained-byte oracle");
#else
  (void)page;
#endif
}

static void EditedMappedAndUnmappedViews(size_t page) {
  auto canvas = cow::Canvas::Create(page * 2 + 13);
  Require(bool(canvas), "create real COW canvas");
  memset(canvas->Data(), 0x31, canvas->Size());
  auto one = canvas->CreateSnapshot(), two = canvas->CreateSnapshot();
  Require(one && two && one->Data() != two->Data() &&
              one->Size() == canvas->Size() && one->Capacity() == canvas->Capacity(),
          "each logical image owns a distinct lazy private VMA with exact logical bytes");
  auto *first = one->Data(), *second = two->Data();
  first[0] = 0x81; first[page + 1] = 0x82;
  second[one->Size() - 1] = 0x83;
  Require(canvas->Data()[0] == 0x31 && second[0] == 0x31,
          "legacy/RW private image edits never write primary or another image");
  errno = EALREADY;
  Require(canvas->PrepareWrite() && errno == EALREADY,
          "freeze all surviving views without asking whether API map_count is zero");
  memset(canvas->Data(), 0x52, canvas->Size());
  Require(one->Data() == first && two->Data() == second,
          "source reuse preserves every borrowed pointer");
  for (size_t i = 0; i < one->Size(); ++i) {
    Require(first[i] == (i == 0 ? 0x81 : i == page + 1 ? 0x82 : 0x31),
            "mapped/private-edited first image preserves every original byte across source reuse");
    Require(second[i] == (i + 1 == two->Size() ? 0x83 : 0x31),
            "retained unmapped image preserves its own edits, including partial last page");
  }
  auto fresh = canvas->CreateSnapshot();
  Require(fresh && Filled(fresh->Data(), fresh->Size(), 0x52),
          "a later derivation observes the actual new complete primary");
  Require(canvas->PrepareWrite(), "a second reuse also freezes newly registered views");
  memset(canvas->Data(), 0x64, canvas->Size());
  Require(Filled(fresh->Data(), fresh->Size(), 0x52) && first[0] == 0x81,
          "multiple retained generations remain independent");
}

static unsigned advice_calls = 0, fail_call = 0;
static bool partial_failure = false;
static int FaultAdvice(void *address, size_t bytes) {
  if (++advice_calls != fail_call) return 0; // the helper STILL calls real MADV
#if defined(MADV_POPULATE_WRITE)
  if (partial_failure) {
    const size_t page = cow::AllocationBytes(1);
    if (madvise(address, std::min(page, bytes), MADV_POPULATE_WRITE)) return -1;
  }
#else
  (void)address; (void)bytes;
#endif
  errno = ENOMEM;
  return -1;
}
static void PartialAndMultipleViewFailure(size_t page) {
  for (bool partial : {false, true}) {
    advice_calls = 0; fail_call = 2; partial_failure = partial;
    auto canvas = cow::Canvas::Create(page * 3, FaultAdvice);
    Require(bool(canvas), "failure injection cannot fake kernel capability preflight");
    memset(canvas->Data(), 0x24, canvas->Size());
    auto one = canvas->CreateSnapshot(), two = canvas->CreateSnapshot();
    Require(one && two, "register two real private mappings before simulated allocation failure");
    two->Data()[page + 7] = 0xa4;
    uint8_t *stable = two->Data();
    Require(!canvas->PrepareWrite() && errno == ENOMEM && advice_calls == 2 &&
                Filled(canvas->Data(), canvas->Size(), 0x24) &&
                Filled(one->Data(), one->Size(), 0x24) && stable[page + 7] == 0xa4,
            "partial/second-view failure preserves all old bytes before any caller store");
    fail_call = 0;
    Require(canvas->PrepareWrite() && advice_calls == 3,
            "retry skips the fully frozen view but completes the unfinished view");
    memset(canvas->Data(), 0x35, canvas->Size());
    Require(two->Data() == stable && Filled(one->Data(), one->Size(), 0x24),
            "retry does not remap borrowed pointers or overwrite an earlier image");
    for (size_t i = 0; i < two->Size(); ++i)
      Require(stable[i] == (i == page + 7 ? 0xa4 : 0x24),
              "retry preserves both private edits and previously shared pages");
  }
  advice_calls = 0; fail_call = 1; partial_failure = false;
  auto canvas = cow::Canvas::Create(page, FaultAdvice);
  auto retired = canvas->CreateSnapshot();
  Require(bool(retired), "create a soon-destroyed image");
  retired.reset();
  Require(canvas->PrepareWrite() && advice_calls == 0,
          "destroyed image requires neither COW nor a stale registry callback");
}

static void ConcurrentPrivateClient(size_t page) {
  auto canvas = cow::Canvas::Create(page * 32);
  Require(bool(canvas), "create concurrent COW canvas");
  memset(canvas->Data(), 0x16, canvas->Size());
  auto image = canvas->CreateSnapshot();
  Require(bool(image), "create actual privately mapped client image");
  std::atomic<bool> started{false}, stop{false};
  volatile uint8_t *p = image->Data();
  std::thread client([&] {
    do {
      for (size_t row = 0; row < 32; ++row) {
        p[row * page + 17] = 0x78;
        if (row == 0) started.store(true, std::memory_order_release);
      }
    } while (!stop.load(std::memory_order_acquire));
  });
  while (!started.load(std::memory_order_acquire)) std::this_thread::yield();
  const bool frozen = canvas->PrepareWrite();
  if (frozen) memset(canvas->Data(), 0x27, canvas->Size());
  stop.store(true, std::memory_order_release);
  client.join();
  Require(frozen, "kernel preparation succeeds while client stores to its own private VMA");
  for (size_t i = 0; i < image->Size(); ++i)
    Require(image->Data()[i] == (i % page == 17 ? 0x78 : 0x16),
            "VM COW preserves concurrent private edits without self-stores or mapped-BUSY");
}

static void RetainedLifetimeAndPrimaryBudget(size_t page) {
  const int fds = FdCount();
  auto canvas = cow::Canvas::Create(page + 1);
  Require(bool(canvas), "create lifetime canvas");
  memset(canvas->Data(), 0x49, canvas->Size());
  auto image = canvas->CreateSnapshot();
  Require(bool(image) && image->Freeze(), "detach actual view before dropping the original owner");
  std::weak_ptr<cow::Canvas> weak = canvas;
  canvas.reset();
  Require(!weak.expired() && cow::LiveBytes() == page * 2 &&
              Filled(image->Data(), image->Size(), 0x49),
          "even frozen private file VMA retains backing fd/mapping and primary charge after source retirement");
  image.reset();
  Require(weak.expired() && cow::LiveBytes() == 0 && FdCount() == fds,
          "last private VMA releases primary charge and fd after its own unmap");
  auto first = cow::Canvas::Create(cow::BudgetLimit / 2);
  auto second = cow::Canvas::Create(cow::BudgetLimit / 2);
  Require(first && second && cow::LiveBytes() == cow::BudgetLimit &&
              !cow::Canvas::Create(1) && errno == ENOMEM,
          "actual page-rounded primary allocations share the process64MiB ceiling");
  auto outside_primary = first->CreateSnapshot();
  Require(outside_primary && cow::LiveBytes() == cow::BudgetLimit,
          "private image pages stay in normal image allocation scope, not a new derived-image limit");
  first.reset(); second.reset();
  Require(cow::LiveBytes() == cow::BudgetLimit / 2,
          "an image pins only its real primary through last reference");
  outside_primary.reset();
  Require(cow::LiveBytes() == 0 && FdCount() == fds, "budget pressure controls release every resource");
}

int main() {
  try {
    const size_t page = cow::AllocationBytes(1);
    auto available = cow::Canvas::Create(page);
    if (!available) {
      const int failure = errno;
      if (failure == ENOTSUP || failure == ENOSYS || failure == EOPNOTSUPP) {
        std::printf("COW real-kernel controls UNAVAILABLE (errno=%d); optional route must use existing fallback. No kernel-positive PASS claimed.\n", failure);
        return 77;
      }
      std::fprintf(stderr, "COW real-kernel preflight FAIL: operational error errno=%d, not capability-unavailable\n", failure);
      return 1;
    }
    available.reset();
    AllocationAndSeals(page);
    PrivateOnlyNegative(page);
    EditedMappedAndUnmappedViews(page);
    PartialAndMultipleViewFailure(page);
    ConcurrentPrivateClient(page);
    RetainedLifetimeAndPrimaryBudget(page);
    std::printf("COW real-kernel controls PASS (%u checks): plain-private counterexample, edited/mapped reuse, concurrency, partial failures, lifetime/fd and primary-only64MiB budget\n", checks);
    return 0;
  } catch (const std::exception &e) {
    std::fprintf(stderr, "COW control FAIL after %u checks: %s\n", checks, e.what());
    return 1;
  }
}
