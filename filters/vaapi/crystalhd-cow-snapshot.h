// SPDX-License-Identifier: LGPL-2.1-or-later
#ifndef CRYSTALHD_COW_SNAPSHOT_H
#define CRYSTALHD_COW_SNAPSHOT_H

#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <fcntl.h>
#include <limits>
#include <linux/memfd.h>
#include <memory>
#include <mutex>
#include <new>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <utility>
#include <vector>

// Linux regular-file COW, never a DRM/MMIO mapping. The caller must serialize
// CreateSnapshot with the COMPLETE primary write window using its existing
// driver/writer reservations. PrepareWrite serializes registry/VM preparation,
// not the caller's later stores. Private clients may keep reading/writing their
// own stable pointers concurrently with PrepareWrite.
namespace crystalhd_cow_snapshot {

inline constexpr uint32_t BudgetLimit = 64U * 1024U * 1024U;
namespace detail {
inline std::atomic<uint32_t> primary_bytes{0};
inline int CapabilityError(int error) noexcept {
  return error == EINVAL || error == ENOSYS || error == EOPNOTSUPP ? ENOTSUP : error;
}
struct Charge {
  uint32_t bytes = 0;
  Charge() = default;
  Charge(const Charge &) = delete;
  Charge &operator=(const Charge &) = delete;
  ~Charge() {
    if (bytes) primary_bytes.fetch_sub(bytes, std::memory_order_relaxed);
  }
  bool Acquire(size_t wanted) noexcept {
    if (bytes || !wanted || wanted > BudgetLimit) { errno = ENOMEM; return false; }
    uint32_t current = primary_bytes.load(std::memory_order_relaxed);
    do {
      if (wanted > BudgetLimit - current) { errno = ENOMEM; return false; }
    } while (!primary_bytes.compare_exchange_weak(current, current +
        static_cast<uint32_t>(wanted), std::memory_order_relaxed));
    bytes = static_cast<uint32_t>(wanted);
    return true;
  }
};
} // namespace detail

inline uint32_t LiveBytes() noexcept {
  return detail::primary_bytes.load(std::memory_order_relaxed);
}

inline size_t AllocationBytes(size_t bytes) noexcept {
  const long page = sysconf(_SC_PAGESIZE);
  if (!bytes || page <= 0) { errno = EINVAL; return 0; }
  const size_t p = static_cast<size_t>(page);
  if (bytes > SIZE_MAX - (p - 1U)) { errno = EOVERFLOW; return 0; }
  const size_t rounded = ((bytes + p - 1U) / p) * p;
  if (rounded > static_cast<uint64_t>(std::numeric_limits<off_t>::max())) {
    errno = EOVERFLOW; return 0;
  }
  return rounded;
}

class Snapshot;
class Canvas final : public std::enable_shared_from_this<Canvas> {
 public:
  // Optional failure-only test hook before a real populate call. Returning0
  // NEVER substitutes for the real syscall. A hook may populate a prefix then
  // return-1 to exercise a genuine partial-COW/error boundary.
  using AdviceHook = int (*)(void *, size_t);
  static std::shared_ptr<Canvas> Create(size_t bytes,
                                        AdviceHook hook = nullptr) noexcept;
  ~Canvas() {
    const int saved = errno;
    if (mapping_ != MAP_FAILED) munmap(mapping_, capacity_);
    if (fd_ >= 0) close(fd_);
    errno = saved;
  }
  Canvas(const Canvas &) = delete;
  Canvas &operator=(const Canvas &) = delete;
  uint8_t *Data() const noexcept { return static_cast<uint8_t *>(mapping_); }
  size_t Size() const noexcept { return bytes_; }
  size_t Capacity() const noexcept { return capacity_; }
  static uint32_t LiveBytes() noexcept { return crystalhd_cow_snapshot::LiveBytes(); }
  static size_t AllocationBytes(size_t bytes) noexcept {
    return crystalhd_cow_snapshot::AllocationBytes(bytes);
  }
  std::shared_ptr<Snapshot> CreateSnapshot() noexcept;
  bool PrepareWrite() noexcept;

 private:
  friend class Snapshot;
  Canvas() = default;
  bool Populate(void *address, size_t bytes) noexcept {
#if defined(MADV_POPULATE_WRITE)
    if (hook_ && hook_(address, bytes) < 0) return false;
    return madvise(address, bytes, MADV_POPULATE_WRITE) == 0;
#else
    (void)address; (void)bytes; errno = ENOTSUP; return false;
#endif
  }
  bool Freeze(Snapshot &snapshot) noexcept;
  // Declared first, destroyed last: the actual primary remains charged through
  // munmap/close, including every private VMA's strong Canvas reference.
  detail::Charge charge_;
  int fd_ = -1;
  void *mapping_ = MAP_FAILED;
  size_t bytes_ = 0, capacity_ = 0;
  AdviceHook hook_ = nullptr;
  std::mutex mutex_;
  std::vector<std::weak_ptr<Snapshot>> views_;
};

class Snapshot final {
 public:
  ~Snapshot() {
    const int saved = errno;
    if (mapping_ != MAP_FAILED) munmap(mapping_, canvas_->Capacity());
    errno = saved;
  }
  Snapshot(const Snapshot &) = delete;
  Snapshot &operator=(const Snapshot &) = delete;
  uint8_t *Data() const noexcept { return static_cast<uint8_t *>(mapping_); }
  size_t Size() const noexcept { return canvas_->Size(); }
  size_t Capacity() const noexcept { return canvas_->Capacity(); }
  bool Freeze() noexcept { return canvas_->Freeze(*this); }

 private:
  friend class Canvas;
  explicit Snapshot(std::shared_ptr<Canvas> canvas) : canvas_(std::move(canvas)) {}
  // Never drop this on Freeze: even all-private file VMAs retain vm_file.
  // Private image/COW pages, like existing vector images, are NOT part of the
  // primary64MiB budget. Their primary backing ALWAYS remains charged here.
  std::shared_ptr<Canvas> canvas_;
  void *mapping_ = MAP_FAILED;
  bool frozen_ = false; // protected by canvas_->mutex_
};

inline std::shared_ptr<Canvas> Canvas::Create(size_t bytes, AdviceHook hook) noexcept {
#if defined(SYS_memfd_create) && defined(MFD_ALLOW_SEALING) && \
    defined(MFD_CLOEXEC) && defined(F_ADD_SEALS) && defined(F_SEAL_SHRINK) && \
    defined(F_SEAL_GROW) && defined(F_SEAL_SEAL) && defined(MADV_POPULATE_WRITE)
  const int saved = errno;
  const size_t capacity = AllocationBytes(bytes);
  if (!capacity) return {};
  try {
    auto result = std::shared_ptr<Canvas>(new Canvas);
    if (!result->charge_.Acquire(capacity)) return {};
    result->bytes_ = bytes; result->capacity_ = capacity; result->hook_ = hook;
    result->fd_ = static_cast<int>(syscall(SYS_memfd_create, "crystalhd-canvas",
                                          MFD_CLOEXEC | MFD_ALLOW_SEALING));
    if (result->fd_ < 0 || ftruncate(result->fd_, static_cast<off_t>(capacity)) ||
        fcntl(result->fd_, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_SEAL))
      return {};
    result->mapping_ = mmap(nullptr, capacity, PROT_READ | PROT_WRITE,
                            MAP_SHARED, result->fd_, 0);
    if (result->mapping_ == MAP_FAILED) return {};
    // Real kernel/mapping capability validation precedes ANY alias. Prefault
    // the real primary as well; ftruncate alone supplies only sparse pages.
    if (madvise(result->mapping_, capacity, MADV_POPULATE_WRITE)) {
      errno = detail::CapabilityError(errno); return {};
    }
    const size_t page = AllocationBytes(1);
    if (!page) return {};
    void *probe = mmap(nullptr, capacity, PROT_READ | PROT_WRITE, MAP_PRIVATE,
                       result->fd_, 0);
    if (probe == MAP_FAILED) return {};
    // A successful syscall return alone is insufficient (some emulators
    // silently ignore this advice). Verify actual COW across the complete
    // fresh mapping, including its last/partial logical page. NEVER write to
    // the private probe: that would force COW independently and mask a no-op.
    volatile uint8_t *shared = result->Data();
    volatile const uint8_t *private_bytes = static_cast<const uint8_t *>(probe);
    for (size_t offset = 0; offset < capacity; offset += page) shared[offset] = 0x35;
    shared[capacity - 1U] = 0x79;
    bool observed = true;
    for (size_t offset = 0; offset < capacity; offset += page)
      if (private_bytes[offset] != 0x35) observed = false;
    if (private_bytes[capacity - 1U] != 0x79) observed = false;
    const int supported = madvise(probe, capacity, MADV_POPULATE_WRITE);
    const int failure = errno;
    bool separated = false;
    if (!supported) {
      for (size_t offset = 0; offset < capacity; offset += page) shared[offset] = 0x57;
      shared[capacity - 1U] = 0x8b;
      separated = true;
      for (size_t offset = 0; offset < capacity; offset += page)
        if (private_bytes[offset] != 0x35) separated = false;
      if (private_bytes[capacity - 1U] != 0x79) separated = false;
    }
    // Fresh memfd pages were zero before this unexposed semantic probe.
    for (size_t offset = 0; offset < capacity; offset += page) shared[offset] = 0;
    shared[capacity - 1U] = 0;
    const int unmapped = munmap(probe, capacity);
    if (supported) { errno = detail::CapabilityError(failure); return {}; }
    if (unmapped) return {};
    if (!observed || !separated) { errno = ENOTSUP; return {}; }
    errno = saved;
    return result;
  } catch (const std::bad_alloc &) { errno = ENOMEM; return {}; }
#else
  (void)bytes; (void)hook; errno = ENOTSUP; return {};
#endif
}

inline std::shared_ptr<Snapshot> Canvas::CreateSnapshot() noexcept {
  const int saved = errno;
  try {
    std::lock_guard<std::mutex> guard(mutex_);
    auto self = weak_from_this().lock();
    if (!self) { errno = EINVAL; return {}; }
    auto image = std::shared_ptr<Snapshot>(new Snapshot(std::move(self)));
    // No MAP_POPULATE: eager writable prefault would lose the intended saving.
    image->mapping_ = mmap(nullptr, capacity_, PROT_READ | PROT_WRITE,
                            MAP_PRIVATE, fd_, 0);
    if (image->mapping_ == MAP_FAILED) return {};
    for (auto i = views_.begin(); i != views_.end(); ) {
      if (i->expired()) i = views_.erase(i); else ++i;
    }
    views_.emplace_back(image); // failure unmaps the unpublished image
    errno = saved;
    return image;
  } catch (const std::bad_alloc &) { errno = ENOMEM; return {}; }
}

inline bool Canvas::Freeze(Snapshot &snapshot) noexcept {
  const int saved = errno;
  std::lock_guard<std::mutex> guard(mutex_);
  if (!snapshot.frozen_) {
    if (!Populate(snapshot.mapping_, capacity_)) return false;
    snapshot.frozen_ = true;
  }
  errno = saved;
  return true;
}

inline bool Canvas::PrepareWrite() noexcept {
  const int saved = errno;
  std::lock_guard<std::mutex> guard(mutex_);
  for (auto i = views_.begin(); i != views_.end(); ) {
    const auto view = i->lock();
    if (!view) { i = views_.erase(i); continue; }
    if (!view->frozen_) {
      // Kernel COW changes PTEs without overwriting client bytes, even for
      // already mapped/privately edited images. Partial failure leaves old
      // bytes valid; the caller MUST NOT mutate the primary on false.
      if (!Populate(view->mapping_, capacity_)) return false;
      view->frozen_ = true;
    }
    ++i;
  }
  errno = saved;
  return true;
}

} // namespace crystalhd_cow_snapshot
#endif
