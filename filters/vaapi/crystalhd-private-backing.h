// SPDX-License-Identifier: LGPL-2.1-or-later
#ifndef CRYSTALHD_PRIVATE_BACKING_H
#define CRYSTALHD_PRIVATE_BACKING_H

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

// Idle private CPU bytes only: live pictures are never held by this cache.
// A Surface returns storage after its last immutable-picture reference dies.
class CrystalHDPrivateBackingPool {
 public:
  static constexpr size_t kBuffers = 4;
  static constexpr size_t kBytes = 16U * 1024U * 1024U;
  struct Usage { size_t buffers = 0, bytes = 0; };

  Usage Cached() {
    std::lock_guard<std::mutex> lock(mutex_);
    Usage usage;
    for (const auto &idle : idle_) {
      usage.buffers += idle.capacity() != 0;
      usage.bytes += idle.capacity();
    }
    return usage;
  }

  std::vector<uint8_t> Acquire(size_t bytes) {
    std::vector<uint8_t> storage;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      for (auto &idle : idle_) {
        if (!idle.empty() && idle.size() == bytes) {
          storage.swap(idle);
          break;
        }
      }
    }
    if (storage.empty())
      storage.resize(bytes);
    else
      // resize on an already-sized vector does NOT clear old crop/padding.
      std::fill(storage.begin(), storage.end(), uint8_t{0});
    return storage;
  }

  // No allocation on the final-picture destruction path. Capacity, rather
  // than visible bytes, bounds retained allocator storage; subtraction avoids
  // an overflowing sum even when a caller returns an oversized vector.
  void Recycle(std::vector<uint8_t> &&storage) {
    if (storage.empty() || storage.capacity() > kBytes)
      return;
    std::lock_guard<std::mutex> lock(mutex_);
    size_t remaining = kBytes;
    for (const auto &idle : idle_)
      remaining -= idle.capacity();
    if (storage.capacity() > remaining)
      return;
    for (auto &idle : idle_) {
      if (idle.capacity() == 0) {
        idle.swap(storage);
        return;
      }
    }
  }

 private:
  std::mutex mutex_;
  std::array<std::vector<uint8_t>, kBuffers> idle_;
};

#endif
