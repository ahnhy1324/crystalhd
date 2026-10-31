// SPDX-License-Identifier: LGPL-2.1-or-later
// Private test backend only. Never include this header in the normal driver.
#ifndef CRYSTALHD_TEST_VPP_PAYLOAD_OBSERVER_H
#define CRYSTALHD_TEST_VPP_PAYLOAD_OBSERVER_H
#include <array>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <memory>
#include <new>
#include <utility>
#include <unistd.h>

namespace crystalhd_test_vpp_payload {
constexpr unsigned kWidth = 1280, kHeight = 720, kMaxPhysicalHeight = 768, kQuota = 8;
constexpr size_t kPlaneBytes = size_t{kWidth} * kHeight * 3 / 2;
inline uint64_t NextSurface() {
  static std::atomic<uint64_t> next{1};
  return next.fetch_add(1, std::memory_order_relaxed);
}
struct FirstField {
  bool valid = false, bottom = false, bottom_first = false;
  uint64_t generation = 0, decode_identity = 0, token = 0;
  uint32_t picture_number = 0, sess_num = 0, picture_meta_payload = 0;
};
// This pair identifies a Surface storage view, not a kernel DMA-BUF.
// Pool reuse/adoption advances the epoch even at the same virtual address.
struct Stamp {
  uint64_t serial = NextSurface(), epoch = 0;
  FirstField first;
  void Installed() { ++epoch; first = {}; }
};
struct Geometry {
  uint64_t serial = 0, epoch = 0;
  unsigned width = 0, height = 0, pitch[2] = {};
  uintptr_t plane[2] = {};
  size_t storage_bytes = 0, object_count = 0;
  std::array<uint64_t, 4> object_sizes{};
  uintptr_t bo_address = 0;
  uint32_t dumb_handle = 0;
};
template <typename Surface> Geometry Describe(const Surface &surface) {
  if (surface.payload_observer_is_view)
    return surface.payload_observer_view_geometry;
  Geometry g;
  g.serial = surface.payload_observer_stamp.serial;
  g.epoch = surface.payload_observer_stamp.epoch;
  g.width = surface.width; g.height = surface.height;
  for (unsigned plane = 0; plane != 2; ++plane) {
    g.pitch[plane] = surface.pitch[plane];
    g.plane[plane] = reinterpret_cast<uintptr_t>(surface.planes[plane]);
  }
  g.storage_bytes = surface.storage.size();
  g.object_count = surface.object_sizes.size();
  for (size_t i = 0; i < g.object_count && i < 4; ++i)
    g.object_sizes[i] = surface.object_sizes[i];
  g.bo_address = reinterpret_cast<uintptr_t>(surface.bo);
  g.dumb_handle = surface.dumb_handle;
  return g;
}
struct Binding {
  uint64_t context = 0, generation = 0, token = 0, decode_identity = 0;
  uint64_t submission_ordinal = 0, surface = 0, owner = 0;
};
struct Slot {
  bool selected = false, copy_ok = false, process_success = false;
  bool committed = false, current = false, source_is_field = false, retired = false;
  int status = -1;
  unsigned stream_index = 0, parent_index = 0, parity = 0;
  uint64_t operation = 0, source_surface = 0, source_owner = 0;
  Binding binding;
  Geometry source_canonical, target_canonical, source, target;
  FirstField first;
  std::array<int, 4> source_crop{}, target_crop{};
  std::array<unsigned, 2> source_payload{}, target_payload{};
  unsigned copy_reject_reason = 0;
  size_t source_bytes = 0, target_bytes = 0;
  std::array<uint8_t, kPlaneBytes> source_pixels{}, target_pixels{};
};
struct Stream {
  bool used = false;
  bool retired = false;
  int retirement_status = -1;
  uint64_t context = 0, generation = 0;
  struct Parent {
    bool used = false;
    uint64_t token = 0, identity = 0, ordinal = 0, source_serial = 0;
  } parents[2];
};
struct Candidate {
  bool seen = false;
  uint64_t reject_mask = 0;
  Geometry source, target;
  std::array<int, 4> source_crop{}, target_crop{};
};
struct SlotStatus {
  bool selected = false, retired = false, committed = false;
  bool current = false, copied = false, process_success = false;
  uint64_t operation = 0;
  int status = -1;
  unsigned copy_reject_reason = 0, source_height = 0, target_height = 0;
};
struct Status {
  uint64_t session = 0, context = 0, generation = 0;
  int retirement_status = -1;
  bool observer_available = false, directory_open = false, sealed = false;
  int directory_errno = 0;
  uint64_t candidates = 0, rejected = 0, reject_mask = 0;
  Candidate first;
  std::array<SlotStatus, kQuota> slots{};
};
inline void PrintStatus(const Status &status);
struct State {
  uint64_t session;
  int directory = -1;
  int directory_errno = 0;
  // All mutations below are protected by the existing Driver mutex. Once
  // sealed, only the claimed caller-side publisher touches independent state.
  bool sealed = false;
  std::atomic<bool> publication_started{false};
  uint64_t candidates = 0, rejected = 0, reject_mask = 0;
  Candidate first_candidate;
  std::array<Stream, 2> streams{};
  std::array<Slot, kQuota> slots{};
  explicit State(uint64_t instance) : session(instance) {
    const char *path = std::getenv("CRYSTALHD_TEST_VPP_PAYLOAD_DIR");
    if (session != 0 && path != nullptr && path[0] == '/')
      directory = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    else
      directory_errno = EINVAL;
    if (directory < 0 && directory_errno == 0) directory_errno = errno;
  }
  ~State() { if (directory >= 0) close(directory); }
  template <typename Fields, typename Surface, typename Rectangle, typename Deinterlace>
  Slot *Select(const Fields &fields, const Surface &canonical,
               const Surface &source_owner, const Surface &target,
               const Rectangle &source_region, const Rectangle &output_region,
               const Deinterlace &deinterlace, uint64_t sequence);
  void Commit(uint64_t sequence, int status, bool successful, bool current) {
    if (sealed) return;
    for (auto &slot : slots) {
      if (slot.selected && slot.operation == sequence) {
        slot.status = status; slot.current = current;
        slot.committed = successful && current && slot.copy_ok && slot.process_success;
        slot.retired = true;
        return;
      }
    }
  }
  bool Retire(uint64_t context, uint64_t generation, int status) {
    if (sealed) return false;
    bool matched = false;
    for (auto &stream : streams) {
      if (stream.used && stream.context == context && stream.generation == generation) {
        stream.retired = true;
        stream.retirement_status = status;
        matched = true;
      }
    }
    if (!matched) return false;
    for (const auto &stream : streams)
      if (!stream.used || !stream.retired || stream.retirement_status != 0)
        return false;
    for (const auto &slot : slots)
      // committed is protected by Driver::mutex and becomes true only after
      // the existing unlocked leaf has retired and the mutex was reacquired.
      // Short-circuit before reading fields an active leaf may still write.
      if (!slot.selected || !slot.committed || !slot.copy_ok ||
          !slot.process_success || !slot.current || slot.status != 0)
        return false;
    // Selected writes have already retired via their ordinary commit path.
    // No producer can replace any of the eight fixed slots after this claim.
    sealed = true;
    return true;
  }
  Status Snapshot(uint64_t context, uint64_t generation, int retirement_status) const {
    Status result;
    result.session = session; result.context = context; result.generation = generation;
    result.retirement_status = retirement_status;
    result.observer_available = true;
    result.directory_open = directory >= 0; result.sealed = sealed;
    result.directory_errno = directory_errno;
    result.candidates = candidates; result.rejected = rejected;
    result.reject_mask = reject_mask; result.first = first_candidate;
    for (unsigned i = 0; i != kQuota; ++i) {
      const auto &slot = slots[i]; auto &out = result.slots[i];
      out.selected = slot.selected; out.retired = slot.retired;
      out.committed = slot.committed; out.operation = slot.operation;
      out.source_height = slot.source_canonical.height;
      out.target_height = slot.target_canonical.height;
      // Effective leaf fields are read only after ordinary retirement/relock.
      if (slot.retired) {
        out.current = slot.current; out.copied = slot.copy_ok;
        out.process_success = slot.process_success; out.status = slot.status;
        out.copy_reject_reason = slot.copy_reject_reason;
      }
    }
    return result;
  }
  void Flush();
};
// Declare before the existing caller's unique_lock. C++ destruction order
// releases that lock and performs its normal bookkeeping before this I/O.
struct PublishOnReturn {
  std::shared_ptr<State> state;
  bool has_status = false;
  Status status;
  ~PublishOnReturn() {
    if (has_status) PrintStatus(status);
    if (state) state->Flush();
  }
};
inline thread_local Slot *active = nullptr;
struct Scope {
  Slot *previous = active;
  template <typename... Arguments> Scope(State *state, Arguments &&...arguments) {
    active = state ? state->Select(std::forward<Arguments>(arguments)...) : nullptr;
  }
  ~Scope() { active = previous; }
  Scope(const Scope &) = delete;
  Scope &operator=(const Scope &) = delete;
};
inline void ProcessSuccess() { if (active) active->process_success = true; }

template <typename Rectangle>
bool AdmittedCrops(const Rectangle &source, const Rectangle &target,
                    unsigned source_height, unsigned target_height) {
  return source.x == 0 && source.y == 0 && target.x == 0 && target.y == 0 &&
      source.width == kWidth && target.width == kWidth &&
      source.height >= kHeight && source.height <= kMaxPhysicalHeight &&
      source.height % 4 == 0 && target.height == source.height &&
      source.height <= source_height && target.height <= target_height;
}

template <typename Fields, typename Surface, typename Rectangle, typename Deinterlace>
Slot *State::Select(const Fields &fields, const Surface &canonical,
                    const Surface &source_owner, const Surface &target,
                    const Rectangle &source_region, const Rectangle &output_region,
                    const Deinterlace &deinterlace, uint64_t sequence) {
  if (sealed) return nullptr;
  ++candidates;
  uint64_t why = 0;
  if (directory < 0) why |= 1;
  if (!deinterlace.bob) why |= 2;
  if (canonical.destroyed || canonical.failed || target.destroyed) why |= 4;
  if (fields.operation != sequence || fields.surface != target.id ||
      fields.context == 0 || fields.generation == 0 || fields.token == 0 ||
      fields.decode_identity == 0 || fields.submission_ordinal == 0) why |= 8;
  if (canonical.fourcc != 0x3231564eU || target.fourcc != 0x3231564eU) why |= 16;
  if (canonical.direct_backing || canonical.centered_geometry.enabled) why |= 32;
  if (canonical.width != kWidth || canonical.height < kHeight ||
      canonical.height > kMaxPhysicalHeight || canonical.height % 4 != 0 ||
      target.width != kWidth || target.height < kHeight ||
      target.height > kMaxPhysicalHeight || target.height % 4 != 0) why |= 64;
  if (!AdmittedCrops(source_region, output_region, canonical.height, target.height)) why |= 128;
  if (canonical.payload_observer_stamp.epoch == 0 ||
      target.payload_observer_stamp.epoch == 0 ||
      !canonical.planes[0] || !canonical.planes[1]) why |= 256;
  const unsigned parity = static_cast<unsigned>(deinterlace.field);
  if (parity > 1) why |= 512;
  if (!first_candidate.seen && deinterlace.bob) {
    first_candidate.seen = true; first_candidate.reject_mask = why;
    first_candidate.source = Describe(canonical); first_candidate.target = Describe(target);
    first_candidate.source_crop = {source_region.x, source_region.y, source_region.width, source_region.height};
    first_candidate.target_crop = {output_region.x, output_region.y, output_region.width, output_region.height};
  }
  if (why) { ++rejected; reject_mask |= why; return nullptr; }
  unsigned stream_index = 2;
  for (unsigned i = 0; i != 2; ++i) {
    if (streams[i].used && streams[i].context == fields.context &&
        streams[i].generation == fields.generation) {
      stream_index = i; break;
    }
  }
  if (stream_index == 2) {
    for (unsigned i = 0; i != 2; ++i) {
      if (!streams[i].used) {
        stream_index = i; streams[i].used = true;
        streams[i].context = fields.context;
        streams[i].generation = fields.generation;
        break;
      }
    }
  }
  if (stream_index == 2) { ++rejected; reject_mask |= 1024; return nullptr; }
  auto &parents = streams[stream_index].parents;
  unsigned parent_index = 2;
  for (unsigned i = 0; i != 2; ++i) {
    if (parents[i].used && parents[i].token == fields.token &&
        parents[i].identity == fields.decode_identity &&
        parents[i].ordinal == fields.submission_ordinal &&
        parents[i].source_serial == canonical.payload_observer_stamp.serial) {
      parent_index = i; break;
    }
  }
  if (parent_index == 2) {
    for (unsigned i = 0; i != 2; ++i) {
      if (!parents[i].used) {
        parent_index = i;
        parents[i] = {true, fields.token, fields.decode_identity,
                      fields.submission_ordinal, canonical.payload_observer_stamp.serial};
        break;
      }
    }
  }
  if (parent_index == 2) { ++rejected; reject_mask |= 1024; return nullptr; }
  Slot &slot = slots[stream_index * 4 + parent_index * 2 + parity];
  // Preserve a failed first selection; never retry into an evidence slot.
  if (slot.selected) { ++rejected; reject_mask |= 2048; return nullptr; }
  slot.selected = true; slot.stream_index = stream_index;
  slot.parent_index = parent_index; slot.parity = parity;
  slot.operation = sequence;
  slot.binding = {fields.context, fields.generation, fields.token,
                  fields.decode_identity, fields.submission_ordinal,
                  fields.surface, fields.owner};
  slot.source_surface = canonical.id;
  slot.source_owner = source_owner.id;
  slot.source_canonical = Describe(canonical);
  slot.target_canonical = Describe(target);
  slot.source_crop = {source_region.x, source_region.y,
                      source_region.width, source_region.height};
  slot.target_crop = {output_region.x, output_region.y,
                      output_region.width, output_region.height};
  return &slot;
}

template <typename Surface, typename Rectangle, typename Deinterlace>
void Copy(const Surface &source, const Surface &target,
          const Rectangle &source_region, const Rectangle &output_region,
          const Deinterlace &deinterlace, bool compact) {
  Slot *slot = active;
  if (!slot || slot->copy_ok) return;
  slot->source = Describe(source); slot->target = Describe(target);
  slot->source_is_field = compact;
  slot->first = source.payload_observer_stamp.first;
  slot->copy_reject_reason = 1;
  if (!deinterlace.bob ||
      static_cast<unsigned>(deinterlace.field) != slot->parity ||
      source.fourcc != 0x3231564eU || target.fourcc != 0x3231564eU ||
      source.width != kWidth ||
      source.height != slot->source_canonical.height / (compact ? 2 : 1) ||
      target.width != kWidth || target.height != slot->target_canonical.height ||
      !AdmittedCrops(source_region, output_region,
                      slot->source_canonical.height, slot->target_canonical.height) ||
      source.payload_observer_stamp.epoch == 0 ||
      target.payload_observer_stamp.serial != slot->target_canonical.serial ||
      target.payload_observer_stamp.epoch != slot->target_canonical.epoch)
    return;
  slot->copy_reject_reason = 2;
  if (compact && (!slot->first.valid ||
      slot->first.bottom != (slot->parity == 1) ||
      slot->first.bottom_first != slot->first.bottom ||
      slot->first.generation != slot->binding.generation ||
      slot->first.decode_identity != slot->binding.decode_identity ||
      slot->first.token != slot->binding.token))
    return;
  slot->copy_reject_reason = 3;
  if (!compact &&
      (source.payload_observer_stamp.serial != slot->source_canonical.serial ||
       source.payload_observer_stamp.epoch != slot->source_canonical.epoch))
    return;
  slot->copy_reject_reason = 4;
  for (unsigned plane = 0; plane != 2; ++plane) {
    if (!source.planes[plane] || !target.planes[plane] ||
        source.pitch[plane] < kWidth || target.pitch[plane] < kWidth)
      return;
  }
  slot->source_payload = {kWidth, compact ? kHeight / 2 : kHeight};
  slot->target_payload = {kWidth, kHeight};
  size_t input_offset = 0, output_offset = 0;
  for (unsigned plane = 0; plane != 2; ++plane) {
    // Equal admitted Bob rectangles preserve the prefix row mapping exactly.
    // Keep physical coded/VA padding in metadata; copy only top active rows.
    const unsigned input_rows = slot->source_payload[1] / (plane == 0 ? 1 : 2);
    const unsigned output_rows = slot->target_payload[1] / (plane == 0 ? 1 : 2);
    for (unsigned row = 0; row != input_rows; ++row) {
      std::memcpy(slot->source_pixels.data() + input_offset,
                  source.planes[plane] + size_t{row} * source.pitch[plane], kWidth);
      input_offset += kWidth;
    }
    for (unsigned row = 0; row != output_rows; ++row) {
      std::memcpy(slot->target_pixels.data() + output_offset,
                  target.planes[plane] + size_t{row} * target.pitch[plane], kWidth);
      output_offset += kWidth;
    }
  }
  slot->source_bytes = input_offset; slot->target_bytes = output_offset;
  slot->copy_reject_reason = 0;
  slot->copy_ok = true;
}

inline bool WriteAll(int fd, const uint8_t *bytes, size_t size) {
  while (size != 0) {
    const ssize_t count = write(fd, bytes, size);
    if (count > 0) { bytes += count; size -= static_cast<size_t>(count); }
    else if (count < 0 && errno == EINTR) continue;
    else return false;
  }
  return true;
}
inline void PrintStatus(const Status &s) {
  char slots[2048];
  size_t offset = 0;
  unsigned selected = 0, retired = 0, committed = 0;
  for (unsigned i = 0; i != kQuota; ++i) {
    const auto &slot = s.slots[i];
    selected += slot.selected; retired += slot.retired; committed += slot.committed;
    const int count = std::snprintf(slots + offset, sizeof(slots) - offset,
        "%s{\"slot\":%u,\"operation\":%llu,\"selected\":%s,\"retired\":%s,"
        "\"committed\":%s,\"current\":%s,\"copied\":%s,\"process_success\":%s,"
        "\"status\":%d,\"copy_reject_reason\":%u,\"source_height\":%u,\"target_height\":%u}",
        i ? "," : "", i, static_cast<unsigned long long>(slot.operation),
        slot.selected ? "true" : "false", slot.retired ? "true" : "false",
        slot.committed ? "true" : "false",
        slot.retired ? (slot.current ? "true" : "false") : "null",
        slot.retired ? (slot.copied ? "true" : "false") : "null",
        slot.retired ? (slot.process_success ? "true" : "false") : "null",
        slot.status, slot.copy_reject_reason, slot.source_height, slot.target_height);
    if (count < 0 || static_cast<size_t>(count) >= sizeof(slots) - offset) return;
    offset += static_cast<size_t>(count);
  }
  const auto &f = s.first;
  char line[4096];
  const int count = std::snprintf(line, sizeof(line),
      "{\"schema\":\"crystalhd-vpp-payload-status-v1\",\"pid\":%lld,\"driver_instance\":%llu,"
      "\"context\":%llu,\"generation\":%llu,\"retirement_status\":%d,"
      "\"observer_available\":%s,\"directory_open\":%s,\"directory_errno\":%d,"
      "\"after_driver_unlock\":true,\"sealed\":%s,\"candidates\":%llu,\"rejected\":%llu,"
      "\"reject_mask\":%llu,\"selected_slots\":%u,\"retired_slots\":%u,\"committed_slots\":%u,"
      "\"first_candidate\":{\"seen\":%s,\"reject_mask\":%llu,\"source_physical\":[%u,%u],"
      "\"target_physical\":[%u,%u],\"source_crop\":[%d,%d,%d,%d],\"target_crop\":[%d,%d,%d,%d]},"
      "\"slots\":[%s]}\n", static_cast<long long>(getpid()),
      static_cast<unsigned long long>(s.session), static_cast<unsigned long long>(s.context),
      static_cast<unsigned long long>(s.generation), s.retirement_status,
      s.observer_available ? "true" : "false", s.directory_open ? "true" : "false",
      s.directory_errno, s.sealed ? "true" : "false", static_cast<unsigned long long>(s.candidates),
      static_cast<unsigned long long>(s.rejected), static_cast<unsigned long long>(s.reject_mask),
      selected, retired, committed, f.seen ? "true" : "false",
      static_cast<unsigned long long>(f.reject_mask), f.source.width, f.source.height,
      f.target.width, f.target.height, f.source_crop[0], f.source_crop[1], f.source_crop[2],
      f.source_crop[3], f.target_crop[0], f.target_crop[1], f.target_crop[2], f.target_crop[3], slots);
  if (count > 0 && static_cast<size_t>(count) < sizeof(line))
    WriteAll(STDERR_FILENO, reinterpret_cast<const uint8_t *>(line), static_cast<size_t>(count));
}
inline bool WriteBlob(int directory, const char *name, const uint8_t *bytes, size_t size) {
  const int fd = openat(directory, name,
      O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
  if (fd < 0) return false;
  const bool good = WriteAll(fd, bytes, size);
  return close(fd) == 0 && good;
}
inline void PrintGeometry(FILE *out, const Geometry &g) {
  std::fprintf(out, "{\"surface_serial\":%llu,\"storage_epoch\":%llu,"
      "\"width\":%u,\"height\":%u,\"pitch\":[%u,%u],"
      "\"plane_address\":[%llu,%llu],\"fourcc\":\"NV12\","
      "\"storage_bytes\":%zu,\"bo_address\":%llu,\"dumb_handle\":%u,"
      "\"object_count\":%zu,\"object_sizes\":[%llu,%llu,%llu,%llu]}",
      static_cast<unsigned long long>(g.serial), static_cast<unsigned long long>(g.epoch),
      g.width, g.height, g.pitch[0], g.pitch[1],
      static_cast<unsigned long long>(g.plane[0]), static_cast<unsigned long long>(g.plane[1]),
      g.storage_bytes, static_cast<unsigned long long>(g.bo_address), g.dumb_handle, g.object_count,
      static_cast<unsigned long long>(g.object_sizes[0]),
      static_cast<unsigned long long>(g.object_sizes[1]),
      static_cast<unsigned long long>(g.object_sizes[2]),
      static_cast<unsigned long long>(g.object_sizes[3]));
}
inline void State::Flush() {
  if (!sealed || directory < 0 || publication_started.exchange(true)) return;
  // The existing successful selected decoder-context retirement caller has
  // released Driver::mutex. All eight arrays were sealed under that lock;
  // they retain no Surface, allocation, Driver, worker or external-client lease.
  char prefix[96], manifest_name[128];
  std::snprintf(prefix, sizeof(prefix), "vpp-payload-%lld-%llu",
      static_cast<long long>(getpid()), static_cast<unsigned long long>(session));
  std::snprintf(manifest_name, sizeof(manifest_name), "%s.json", prefix);
  std::array<std::array<char, 128>, kQuota> source_names{}, target_names{};
  bool good = true;
  for (unsigned i = 0; i != kQuota; ++i) {
    const Slot &slot = slots[i];
    if (!slot.committed) continue;
    std::snprintf(source_names[i].data(), source_names[i].size(), "%s-%u-source.nv12", prefix, i);
    std::snprintf(target_names[i].data(), target_names[i].size(), "%s-%u-target.nv12", prefix, i);
    good = WriteBlob(directory, source_names[i].data(), slot.source_pixels.data(), slot.source_bytes) && good;
    good = WriteBlob(directory, target_names[i].data(), slot.target_pixels.data(), slot.target_bytes) && good;
  }
  if (!good) {
    std::fprintf(stderr, "crystalhd-vpp-payload: blob write failed; no manifest\n");
    return;
  }
  const int fd = openat(directory, manifest_name,
      O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
  if (fd < 0) return;
  FILE *out = fdopen(fd, "w");
  if (!out) { close(fd); return; }
  std::fprintf(out, "{\"schema\":\"crystalhd-vpp-payload-v1\",\"pid\":%lld,"
      "\"driver_instance\":%llu,\"quota\":%u,\"publication\":{"
      "\"kind\":\"selected-decoder-context-retirement\",\"after_driver_unlock\":true,"
      "\"all_slots_frozen\":true,\"retirements\":[",
      static_cast<long long>(getpid()), static_cast<unsigned long long>(session), kQuota);
  for (unsigned i = 0; i != streams.size(); ++i) {
    const auto &stream = streams[i];
    std::fprintf(out, "%s{\"stream_index\":%u,\"context\":%llu,\"generation\":%llu,\"status\":%d}",
        i ? "," : "", i, static_cast<unsigned long long>(stream.context),
        static_cast<unsigned long long>(stream.generation), stream.retirement_status);
  }
  std::fputs("]},\"slots\":[", out);
  bool comma = false;
  for (unsigned i = 0; i != kQuota; ++i) {
    const Slot &s = slots[i];
    if (!s.selected) continue;
    const auto &b = s.binding; const auto &f = s.first;
    std::fprintf(out, "%s{\"stream_index\":%u,\"parent_index\":%u,"
        "\"operation\":%llu,\"binding\":{\"context\":%llu,\"generation\":%llu,"
        "\"token\":%llu,\"decode_identity\":%llu,\"submission_ordinal\":%llu,"
        "\"surface\":%llu,\"owner\":%llu},\"source_surface\":%llu,"
        "\"source_owner\":%llu,\"requested_parity\":%u,\"source_is_field\":%s,"
        "\"source_crop\":[%d,%d,%d,%d],\"target_crop\":[%d,%d,%d,%d],"
        "\"source_payload\":[%u,%u],\"target_payload\":[%u,%u],"
        "\"copy_ok\":%s,\"process_success\":%s,\"committed\":%s,"
        "\"current\":%s,\"status\":%d,\"source_file\":\"%s\","
        "\"target_file\":\"%s\",\"source_bytes\":%zu,\"target_bytes\":%zu,"
        "\"compact_first_field\":{\"valid\":%s,\"bottom\":%s,\"bottom_first\":%s,"
        "\"generation\":%llu,\"decode_identity\":%llu,\"token\":%llu,"
        "\"picture_number\":%u,\"sess_num\":%u,\"picture_meta_payload\":%u},"
        "\"source_canonical\":", comma ? "," : "", s.stream_index, s.parent_index,
        static_cast<unsigned long long>(s.operation),
        static_cast<unsigned long long>(b.context), static_cast<unsigned long long>(b.generation),
        static_cast<unsigned long long>(b.token), static_cast<unsigned long long>(b.decode_identity),
        static_cast<unsigned long long>(b.submission_ordinal),
        static_cast<unsigned long long>(b.surface), static_cast<unsigned long long>(b.owner),
        static_cast<unsigned long long>(s.source_surface), static_cast<unsigned long long>(s.source_owner),
        s.parity, s.source_is_field ? "true" : "false", s.source_crop[0], s.source_crop[1],
        s.source_crop[2], s.source_crop[3], s.target_crop[0], s.target_crop[1],
        s.target_crop[2], s.target_crop[3], s.source_payload[0], s.source_payload[1],
        s.target_payload[0], s.target_payload[1], s.copy_ok ? "true" : "false",
        s.process_success ? "true" : "false", s.committed ? "true" : "false",
        s.current ? "true" : "false", s.status, source_names[i].data(), target_names[i].data(),
        s.source_bytes, s.target_bytes, f.valid ? "true" : "false", f.bottom ? "true" : "false",
        f.bottom_first ? "true" : "false", static_cast<unsigned long long>(f.generation),
        static_cast<unsigned long long>(f.decode_identity), static_cast<unsigned long long>(f.token),
        f.picture_number, f.sess_num, f.picture_meta_payload);
    PrintGeometry(out, s.source_canonical);
    std::fputs(",\"target_canonical\":", out); PrintGeometry(out, s.target_canonical);
    std::fputs(",\"source\":", out); PrintGeometry(out, s.source);
    std::fputs(",\"target\":", out); PrintGeometry(out, s.target);
    std::fputc('}', out); comma = true;
  }
  std::fputs("]}\n", out);
  const bool stream_good = std::ferror(out) == 0;
  const bool closed = std::fclose(out) == 0;
  if (!stream_good || !closed)
    std::fprintf(stderr, "crystalhd-vpp-payload: manifest write failed\n");
}

}  // namespace crystalhd_test_vpp_payload
#endif
