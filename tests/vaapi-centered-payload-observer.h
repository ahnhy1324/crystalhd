// SPDX-License-Identifier: LGPL-2.1-or-later
// Private diagnostic backend only; never included by the ordinary driver.
#ifndef CRYSTALHD_TEST_CENTERED_PAYLOAD_OBSERVER_H
#define CRYSTALHD_TEST_CENTERED_PAYLOAD_OBSERVER_H
#include "vaapi-vpp-payload-observer.h"

namespace crystalhd_test_centered_payload {
using crystalhd_test_vpp_payload::Geometry;
using crystalhd_test_vpp_payload::Describe;
using crystalhd_test_vpp_payload::PrintGeometry;
using crystalhd_test_vpp_payload::WriteAll;
using crystalhd_test_vpp_payload::WriteBlob;
constexpr unsigned kStreams = 2, kSlots = 2;
constexpr unsigned kSmallWidth = 1280, kSmallHeight = 720;
constexpr unsigned kCanvasWidth = 1920, kMaxCanvasHeight = 1088;
constexpr size_t kSmallBytes = size_t{kSmallWidth} * kSmallHeight * 3 / 2;
constexpr size_t kCanvasBytes = size_t{kCanvasWidth} * kMaxCanvasHeight * 3 / 2;
enum Reject : uint64_t {
  Directory = 1, GeometryScope = 2, BindingMismatch = 4, StorageMismatch = 8,
  ComposeFailure = 16, PublicFailure = 32, MissingMaterialized = 64,
  Incomplete = 128, RetirementFailure = 256, RepeatedBinding = 512
};
struct Binding {
  uint64_t context = 0, generation = 0, token = 0, decode_identity = 0;
  uint64_t submission_ordinal = 0, surface = 0, owner = 0;
};
struct Slot {
  bool selected = false, direct_public = false, failed = false;
  bool compose_copied = false, compose_committed = false, compose_ended = false;
  bool public_started = false, public_copied = false, public_committed = false;
  bool public_ended = false, materialized = false;
  unsigned stream_index = 0;
  uint64_t reject_mask = 0;
  Binding binding;
  Geometry small, composed, copy_source, public_canvas;
  size_t small_bytes = 0, composed_bytes = 0, public_bytes = 0;
  std::array<uint8_t, kSmallBytes> small_pixels{};
  std::array<uint8_t, kCanvasBytes> composed_pixels{}, public_pixels{};
};
struct Stream {
  bool used = false, sealed = false, status_printed = false;
  bool publication_started = false, manifest_written = false;
  int retirement_status = -1, publication_errno = 0;
  uint64_t context = 0, generation = 0, candidates = 0, rejected = 0, reject_mask = 0;
  std::array<Slot, kSlots> slots{};
};
inline bool SameGeometry(const Geometry &a, const Geometry &b) {
  return a.serial == b.serial && a.epoch == b.epoch && a.width == b.width &&
      a.height == b.height && a.pitch[0] == b.pitch[0] && a.pitch[1] == b.pitch[1] &&
      a.plane[0] == b.plane[0] && a.plane[1] == b.plane[1];
}
template <typename Surface> uint64_t Owner(const Surface &s) {
  return s.backing_owner == uint32_t(-1) ? s.id : s.backing_owner;
}
template <typename Surface> Binding DescribeBinding(const Surface &s) {
  return {s.trace_context, s.trace_generation, s.expected_timestamp,
      s.trace_decode_identity, s.trace_submission_ordinal, s.id, Owner(s)};
}
inline bool ValidBinding(const Binding &b) {
  return b.context && b.generation && b.token && b.decode_identity &&
      b.submission_ordinal && b.surface && b.surface != uint32_t(-1) &&
      b.owner && b.owner != uint32_t(-1);
}
template <typename Surface> bool SameSourceBinding(const Binding &b, const Surface &s) {
  return b.context == s.trace_context && b.generation == s.trace_generation &&
      b.token == s.expected_timestamp && b.decode_identity == s.decode_identity &&
      b.decode_identity == s.trace_decode_identity &&
      b.submission_ordinal == s.trace_submission_ordinal;
}
template <typename Surface> bool AdmittedStorage(const Surface &s, unsigned width,
                                                 unsigned height) {
  const Geometry g = Describe(s);
  return s.fourcc == 0x3231564eU && !s.destroyed && !s.failed &&
      s.width == width && s.height == height && g.width == width && g.height == height &&
      g.serial && g.epoch && s.planes[0] && s.planes[1] &&
      s.pitch[0] >= width && s.pitch[1] >= width;
}
template <typename Surface, size_t Capacity>
bool Pack(const Surface &s, std::array<uint8_t, Capacity> &pixels, size_t *bytes) {
  const size_t length = size_t{s.width} * s.height * 3 / 2;
  if (length > Capacity || !s.planes[0] || !s.planes[1] ||
      (s.height & 1U) || s.pitch[0] < s.width || s.pitch[1] < s.width) return false;
  size_t offset = 0;
  for (unsigned plane = 0; plane != 2; ++plane)
    for (unsigned row = 0; row != (plane == 0 ? s.height : s.height / 2); ++row) {
      std::memcpy(pixels.data() + offset, s.planes[plane] + size_t{row} * s.pitch[plane], s.width);
      offset += s.width;
    }
  *bytes = offset;
  return true;
}
struct State {
  const uint64_t session;
  int directory = -1, directory_errno = 0;
  std::array<Stream, kStreams> streams{};
  explicit State(uint64_t instance) : session(instance) {
    const char *path = std::getenv("CRYSTALHD_TEST_CENTERED_PAYLOAD_DIR");
    if (session && path && path[0] == '/')
      directory = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    else directory_errno = EINVAL;
    if (directory < 0 && !directory_errno) directory_errno = errno;
  }
  ~State() { if (directory >= 0) close(directory); }
  State(const State &) = delete;
  State &operator=(const State &) = delete;
  Stream *Find(uint64_t context, uint64_t generation, bool reserve = false) {
    for (auto &s : streams)
      if (s.used && s.context == context && s.generation == generation) return &s;
    if (reserve) for (auto &s : streams) if (!s.used) {
      s.used = true; s.context = context; s.generation = generation;
      return &s;
    }
    return nullptr;
  }
  bool Writable(const Slot *s) const {
    return s && s->selected && s->stream_index < streams.size() &&
        !streams[s->stream_index].sealed;
  }
  void Fail(Slot *slot, uint64_t why) {
    if (!Writable(slot)) return;
    slot->failed = true; slot->reject_mask |= why;
    streams[slot->stream_index].reject_mask |= why;
  }
  template <typename Surface>
  Slot *Select(const Surface &picture, const Surface &canvas, const Surface &small,
               bool compact, bool decoded_public) {
    const Binding binding = DescribeBinding(picture);
    if (!ValidBinding(binding)) return nullptr;
    const auto &g = picture.centered_geometry;
    uint64_t why = 0;
    if (compact || !g.enabled || g.full_range || g.canvas_width != kCanvasWidth ||
        (g.canvas_height != 1080 && g.canvas_height != 1088) ||
        g.visible_width != kCanvasWidth || g.visible_height != 1080 ||
        g.capture_width != kSmallWidth || g.capture_height != kSmallHeight ||
        g.x != 320 || g.y != 180 || small.centered_geometry.enabled ||
        !AdmittedStorage(small, kSmallWidth, kSmallHeight) ||
        !AdmittedStorage(canvas, kCanvasWidth, g.canvas_height) ||
        picture.width != kCanvasWidth || picture.height != g.canvas_height ||
        picture.fourcc != 0x3231564eU || picture.destroyed || picture.failed ||
        !small.ready || small.frame_timestamp != binding.token) why |= GeometryScope;
    if (!SameSourceBinding(binding, picture) || !SameSourceBinding(binding, small) ||
        !SameSourceBinding(binding, canvas) || canvas.id != binding.surface ||
        Owner(canvas) != binding.owner) why |= BindingMismatch;
    // Unsupported geometry cannot consume one of the two eligible lifetimes.
    Stream *stream = Find(binding.context, binding.generation, why == 0);
    if (!stream || stream->sealed) return nullptr;
    ++stream->candidates;
    if (why) { ++stream->rejected; stream->reject_mask |= why; return nullptr; }
    for (const auto &slot : stream->slots)
      if (slot.selected && (slot.binding.token == binding.token ||
          slot.binding.decode_identity == binding.decode_identity ||
          slot.binding.submission_ordinal == binding.submission_ordinal)) {
        ++stream->rejected; stream->reject_mask |= RepeatedBinding; return nullptr;
      }
    for (auto &slot : stream->slots) if (!slot.selected) {
      slot.selected = true; slot.direct_public = decoded_public;
      slot.stream_index = static_cast<unsigned>(stream - streams.data());
      slot.binding = binding; slot.small = Describe(small); slot.composed = Describe(canvas);
      if (directory < 0) Fail(&slot, Directory);
      return &slot;
    }
    return nullptr;
  }
  template <typename Surface>
  void CaptureCompose(Slot *slot, const Surface &small, const Surface &canvas) {
    if (!Writable(slot) || slot->compose_ended || slot->compose_copied || slot->failed) return;
    if (!SameGeometry(slot->small, Describe(small)) ||
        !SameGeometry(slot->composed, Describe(canvas)) ||
        !SameSourceBinding(slot->binding, small) || !SameSourceBinding(slot->binding, canvas)) {
      Fail(slot, StorageMismatch); return;
    }
    slot->compose_copied = Pack(small, slot->small_pixels, &slot->small_bytes) &&
        Pack(canvas, slot->composed_pixels, &slot->composed_bytes);
    if (!slot->compose_copied) { Fail(slot, ComposeFailure); return; }
    if (slot->direct_public) {
      slot->public_started = true;
      slot->copy_source = slot->public_canvas = slot->composed;
      slot->public_copied = Pack(canvas, slot->public_pixels, &slot->public_bytes);
      if (!slot->public_copied) Fail(slot, PublicFailure);
    }
  }
  void EndCompose(Slot *slot, bool committed, bool released) {
    if (!Writable(slot) || slot->compose_ended) return;
    slot->compose_ended = true;
    slot->compose_committed = !slot->failed && slot->compose_copied && committed && released;
    if (!slot->compose_committed) Fail(slot, ComposeFailure);
    if (slot->direct_public) {
      slot->public_ended = true;
      slot->public_committed = slot->compose_committed && slot->public_copied;
    }
  }
  template <typename Surface>
  Slot *SelectPublic(const Surface &source, const Surface &destination) {
    const Geometry source_geometry = Describe(source);
    Slot *candidate = nullptr;
    unsigned best_rank = 0;
    for (auto &stream : streams) {
      if (!stream.used || stream.sealed) continue;
      for (auto &slot : stream.slots) {
        if (!slot.selected || slot.direct_public) continue;
        const auto &binding = slot.binding;
        const bool same_lifetime = binding.context == source.trace_context &&
            binding.generation == source.trace_generation;
        const bool related_binding = same_lifetime &&
            (binding.token == source.expected_timestamp ||
             binding.decode_identity == source.decode_identity ||
             binding.submission_ordinal == source.trace_submission_ordinal);
        // A private Surface can be reused at a later storage epoch. Its old
        // completed slot must never hide the current source binding merely
        // because the serial or virtual addresses are shared. Prefer binding
        // identity, then its partial mismatch, then the exact storage view;
        // serial alone is only the final fail-closed association.
        const unsigned rank = SameSourceBinding(binding, source) ? 4U :
            related_binding ? 3U : SameGeometry(slot.composed, source_geometry) ? 2U :
            source_geometry.serial == slot.composed.serial ? 1U : 0U;
        if (rank > best_rank) { candidate = &slot; best_rank = rank; }
      }
    }
    if (!candidate) return nullptr;
    auto &slot = *candidate;
    const auto &binding = slot.binding;
    if (slot.public_started || !slot.compose_committed || slot.failed) return nullptr;
    // The first attempted publication of this selected binding is fixed even
    // when its storage/binding is rejected or an existing CPU END fails.
    slot.public_started = true;
    if (!SameGeometry(slot.composed, source_geometry) ||
        !SameSourceBinding(binding, source) ||
        !SameSourceBinding(binding, destination) ||
        source.id != binding.surface || destination.id != binding.surface ||
        Owner(destination) != binding.owner ||
        !AdmittedStorage(destination, kCanvasWidth, destination.height) ||
        (destination.height != 1080 && destination.height != 1088)) {
      Fail(&slot, StorageMismatch | BindingMismatch); return nullptr;
    }
    slot.copy_source = source_geometry; slot.public_canvas = Describe(destination);
    return &slot;
  }
  template <typename Surface> void CapturePublic(Slot *slot, const Surface &destination) {
    if (!Writable(slot) || slot->direct_public || !slot->public_started ||
        slot->public_ended || slot->public_copied || slot->failed) return;
    if (!SameGeometry(slot->public_canvas, Describe(destination)) ||
        !SameSourceBinding(slot->binding, destination)) { Fail(slot, StorageMismatch); return; }
    slot->public_copied = Pack(destination, slot->public_pixels, &slot->public_bytes);
    if (!slot->public_copied) Fail(slot, PublicFailure);
  }
  void EndPublic(Slot *slot, bool committed, bool released) {
    if (!Writable(slot) || slot->public_ended) return;
    slot->public_ended = true;
    slot->public_committed = !slot->failed && slot->compose_committed &&
        slot->public_copied && committed && released;
    if (!slot->public_committed) Fail(slot, PublicFailure);
  }
  template <typename Fields> void Materialized(const Fields &f) {
    Stream *stream = Find(f.context, f.generation);
    if (!stream || stream->sealed) return;
    for (auto &slot : stream->slots) {
      const auto &b = slot.binding;
      if (slot.selected && slot.public_committed && !slot.failed &&
          b.token == f.token && b.decode_identity == f.decode_identity &&
          b.submission_ordinal == f.submission_ordinal &&
          b.surface == f.surface && b.owner == f.owner) slot.materialized = true;
    }
  }
  static bool Complete(const Slot &s) {
    return s.selected && !s.failed && s.compose_copied && s.compose_committed &&
        s.compose_ended && s.public_started && s.public_copied && s.public_committed &&
        s.public_ended && s.materialized;
  }
  bool Complete(const Stream &s) const {
    if (!s.sealed || s.retirement_status != 0 || directory < 0) return false;
    for (const auto &slot : s.slots) if (!Complete(slot)) return false;
    return true;
  }
  int Retire(uint64_t context, uint64_t generation, int status) {
    Stream *stream = Find(context, generation);
    if (!stream || stream->sealed) return -1;
    stream->sealed = true; stream->retirement_status = status;
    if (status != 0) stream->reject_mask |= RetirementFailure;
    if (directory < 0) stream->reject_mask |= Directory;
    for (const auto &slot : stream->slots) {
      if (!Complete(slot)) stream->reject_mask |= Incomplete;
      if (slot.selected && !slot.materialized) stream->reject_mask |= MissingMaterialized;
    }
    return static_cast<int>(stream - streams.data());
  }
  void PrintStatus(int index);
  void Flush(int index);
};
inline thread_local State *active = nullptr;
struct Scope {
  State *previous = active;
  explicit Scope(State *state) { active = state; }
  ~Scope() { active = previous; }
  Scope(const Scope &) = delete;
  Scope &operator=(const Scope &) = delete;
};
// Declare before the existing Driver lock, so publication follows its unlock.
struct PublishOnReturn {
  std::shared_ptr<State> state;
  int index = -1;
  ~PublishOnReturn() { if (state && index >= 0) { state->PrintStatus(index); state->Flush(index); } }
};
inline void State::PrintStatus(int index) {
  if (index < 0 || static_cast<size_t>(index) >= streams.size()) return;
  auto &s = streams[index];
  if (!s.sealed || s.status_printed) return;
  s.status_printed = true;
  unsigned selected = 0, composed = 0, published = 0, materialized = 0;
  for (const auto &slot : s.slots) {
    selected += slot.selected; composed += slot.compose_committed;
    published += slot.public_committed; materialized += slot.materialized;
  }
  char line[1024];
  const int count = std::snprintf(line, sizeof(line),
      "{\"schema\":\"crystalhd-centered-payload-status-v1\",\"pid\":%lld,"
      "\"driver_instance\":%llu,\"context\":%llu,\"generation\":%llu,"
      "\"retirement_status\":%d,\"sealed\":true,\"after_driver_unlock\":true,"
      "\"directory_open\":%s,\"directory_errno\":%d,\"candidates\":%llu,"
      "\"rejected\":%llu,\"reject_mask\":%llu,\"selected_slots\":%u,"
      "\"compose_committed_slots\":%u,\"public_committed_slots\":%u,"
      "\"materialized_slots\":%u,\"eligible\":%s}\n",
      static_cast<long long>(getpid()), static_cast<unsigned long long>(session),
      static_cast<unsigned long long>(s.context), static_cast<unsigned long long>(s.generation),
      s.retirement_status, directory >= 0 ? "true" : "false", directory_errno,
      static_cast<unsigned long long>(s.candidates), static_cast<unsigned long long>(s.rejected),
      static_cast<unsigned long long>(s.reject_mask), selected, composed, published, materialized,
      Complete(s) ? "true" : "false");
  if (count > 0 && static_cast<size_t>(count) < sizeof(line))
    WriteAll(STDERR_FILENO, reinterpret_cast<const uint8_t *>(line), static_cast<size_t>(count));
}
inline void State::Flush(int index) {
  if (index < 0 || static_cast<size_t>(index) >= streams.size()) return;
  auto &stream = streams[index];
  if (!Complete(stream) || stream.publication_started) return;
  stream.publication_started = true;
  // Only this sealed stream is accessed here. Other decoder contexts may
  // continue producing under Driver::mutex without racing these arrays.
  char prefix[160], manifest_name[192];
  std::snprintf(prefix, sizeof(prefix), "centered-payload-%lld-%llu-%llu-%llu",
      static_cast<long long>(getpid()), static_cast<unsigned long long>(session),
      static_cast<unsigned long long>(stream.context), static_cast<unsigned long long>(stream.generation));
  std::snprintf(manifest_name, sizeof(manifest_name), "%s.json", prefix);
  std::array<std::array<std::array<char, 192>, 3>, kSlots> names{};
  bool good = true;
  for (unsigned i = 0; i != kSlots; ++i) {
    const auto &s = stream.slots[i];
    const char *kinds[] = {"small", "composed", "public"};
    for (unsigned kind = 0; kind != 3; ++kind)
      std::snprintf(names[i][kind].data(), names[i][kind].size(), "%s-s%u-%s.nv12", prefix, i, kinds[kind]);
    good = WriteBlob(directory, names[i][0].data(), s.small_pixels.data(), s.small_bytes) && good;
    good = WriteBlob(directory, names[i][1].data(), s.composed_pixels.data(), s.composed_bytes) && good;
    good = WriteBlob(directory, names[i][2].data(), s.public_pixels.data(), s.public_bytes) && good;
  }
  if (!good) {
    stream.publication_errno = errno ? errno : EIO;
    std::fprintf(stderr, "crystalhd-centered-payload: blob write failed; no manifest (errno=%d)\n", stream.publication_errno);
    return;
  }
  const int fd = openat(directory, manifest_name, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
  if (fd < 0) { stream.publication_errno = errno; return; }
  FILE *out = fdopen(fd, "w");
  if (!out) { stream.publication_errno = errno; close(fd); return; }
  std::fprintf(out, "{\"schema\":\"crystalhd-centered-payload-v1\",\"pid\":%lld,"
      "\"driver_instance\":%llu,\"slots_required\":2,\"media_pts_observed\":false,"
      "\"publication\":{\"kind\":\"selected-decoder-context-retirement\","
      "\"context\":%llu,\"generation\":%llu,\"status\":%d,"
      "\"after_driver_unlock\":true,\"all_slots_frozen\":true},\"slots\":[",
      static_cast<long long>(getpid()), static_cast<unsigned long long>(session),
      static_cast<unsigned long long>(stream.context), static_cast<unsigned long long>(stream.generation),
      stream.retirement_status);
  for (unsigned i = 0; i != kSlots; ++i) {
    const auto &s = stream.slots[i]; const auto &b = s.binding;
    std::fprintf(out, "%s{\"binding\":{\"context\":%llu,\"generation\":%llu,\"token\":%llu,"
        "\"decode_identity\":%llu,\"submission_ordinal\":%llu,\"surface\":%llu,\"owner\":%llu},"
        "\"roi\":[320,180,1280,720],\"visible\":[1920,1080],\"limited_range\":true,"
        "\"direct_public\":%s,\"compose_copied\":true,\"compose_committed\":true,"
        "\"public_copied\":true,\"public_committed\":true,\"materialized\":true,\"small\":",
        i ? "," : "", static_cast<unsigned long long>(b.context), static_cast<unsigned long long>(b.generation),
        static_cast<unsigned long long>(b.token), static_cast<unsigned long long>(b.decode_identity),
        static_cast<unsigned long long>(b.submission_ordinal), static_cast<unsigned long long>(b.surface),
        static_cast<unsigned long long>(b.owner), s.direct_public ? "true" : "false");
    PrintGeometry(out, s.small);
    std::fputs(",\"composed\":", out); PrintGeometry(out, s.composed);
    std::fputs(",\"copy_source\":", out); PrintGeometry(out, s.copy_source);
    std::fputs(",\"public_canvas\":", out); PrintGeometry(out, s.public_canvas);
    std::fprintf(out, ",\"small_file\":\"%s\",\"composed_file\":\"%s\",\"public_file\":\"%s\","
        "\"small_bytes\":%zu,\"composed_bytes\":%zu,\"public_bytes\":%zu}",
        names[i][0].data(), names[i][1].data(), names[i][2].data(),
        s.small_bytes, s.composed_bytes, s.public_bytes);
  }
  std::fputs("]}\n", out);
  const bool written = std::ferror(out) == 0;
  const bool closed = std::fclose(out) == 0;
  stream.manifest_written = written && closed;
  if (!stream.manifest_written) {
    stream.publication_errno = errno ? errno : EIO;
    std::fprintf(stderr, "crystalhd-centered-payload: manifest write failed (errno=%d)\n", stream.publication_errno);
  }
}
}  // namespace crystalhd_test_centered_payload
#endif
