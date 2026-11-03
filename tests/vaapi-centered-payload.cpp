// SPDX-License-Identifier: LGPL-2.1-or-later
// CPU controls execute maintained ComposeCenteredCanvas/CopyNv12Surface.
// The existing Bob harness aborts every unexpected device or DIL call.
#define CRYSTALHD_VAAPI_TEST_CENTERED_PAYLOAD 1
#define main CrystalhdExistingBobMain
#define __wrap_ioctl CrystalhdExistingBobIoctl
#include "vaapi-bob-vpp.cpp"
#undef __wrap_ioctl
#undef main
#include <sys/stat.h>

namespace {
namespace centered_payload = crystalhd_test_centered_payload;
int failing_fd = -1;
uint64_t failing_flags = 0;
unsigned centered_ioctl_calls = 0;

void PaddedCpu(Surface *surface, unsigned width, unsigned height) {
  Require(surface->AllocateCpuNv12(width, height), "centered CPU allocation");
  surface->pitch[0] = width + 16;
  surface->pitch[1] = width + 32;
  const size_t y_bytes = size_t{surface->pitch[0]} * height;
  surface->storage.assign(y_bytes + size_t{surface->pitch[1]} * height / 2, 0xa5);
  surface->planes[0] = surface->storage.data();
  surface->planes[1] = surface->storage.data() + y_bytes;
  surface->payload_observer_stamp.Installed();
  surface->failed = false;
}
uint8_t Sample(unsigned seed, unsigned plane, unsigned row, unsigned column) {
  return static_cast<uint8_t>(seed * 13 + row * 29 + column * 7 + plane * 83);
}
struct CenteredFixture {
  Surface picture, public_canvas;
  std::shared_ptr<Surface> small = std::make_shared<Surface>();
  unsigned seed;
  CenteredFixture(unsigned ordinal, unsigned physical_height = 1088,
                  unsigned public_height = 1080, uint64_t context = 3,
                  uint64_t generation = 4) : seed(ordinal) {
    PaddedCpu(&picture, 1920, physical_height);
    PaddedCpu(&public_canvas, 1920, public_height);
    PaddedCpu(small.get(), 1280, 720);
    picture.id = public_canvas.id = 50 + ordinal;
    picture.centered_pixels = small;
    auto &g = picture.centered_geometry;
    g.enabled = true; g.full_range = false;
    g.canvas_width = 1920; g.canvas_height = physical_height;
    g.visible_width = 1920; g.visible_height = 1080;
    g.capture_width = 1280; g.capture_height = 720;
    g.x = 320; g.y = 180;
    for (Surface *surface : {&picture, &public_canvas, small.get()}) {
      surface->expected_timestamp = 1000 + ordinal;
      surface->decode_identity = surface->trace_decode_identity = 2000 + ordinal;
      surface->trace_context = context; surface->trace_generation = generation;
      surface->trace_submission_ordinal = ordinal;
    }
    small->ready = true;
    small->frame_timestamp = small->expected_timestamp;
    for (unsigned plane = 0; plane != 2; ++plane)
      for (unsigned row = 0; row != (plane == 0 ? 720U : 360U); ++row)
        for (unsigned x = 0; x != 1280; ++x)
          small->planes[plane][size_t{row} * small->pitch[plane] + x] = Sample(seed, plane, row, x);
  }
  crystalhd_vaapi_trace::Fields Fields() const {
    crystalhd_vaapi_trace::Fields f;
    f.context = public_canvas.trace_context; f.generation = public_canvas.trace_generation;
    f.token = public_canvas.expected_timestamp; f.decode_identity = public_canvas.trace_decode_identity;
    f.submission_ordinal = public_canvas.trace_submission_ordinal;
    f.surface = public_canvas.id; f.owner = public_canvas.id;
    return f;
  }
  ~CenteredFixture() {
    small->object_fds.clear(); picture.object_fds.clear(); public_canvas.object_fds.clear();
  }
};
enum class EndFailure { None, ComposeWrite, ComposeRead, PublicWrite, PublicRead };
void SetFailure(EndFailure failure, bool compose) {
  failing_fd = -1;
  if ((compose && failure == EndFailure::ComposeWrite) ||
      (!compose && failure == EndFailure::PublicWrite)) {
    failing_fd = 888; failing_flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE;
  } else if ((compose && failure == EndFailure::ComposeRead) ||
             (!compose && failure == EndFailure::PublicRead)) {
    failing_fd = 777; failing_flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ;
  }
}
bool ComposeObserved(centered_payload::State *state, CenteredFixture *fixture,
                     bool direct, EndFailure failure = EndFailure::None) {
  fixture->small->object_fds.push_back(777);
  Surface *canvas = direct ? &fixture->public_canvas : &fixture->picture;
  canvas->object_fds.push_back(888);
  SetFailure(failure, true);
  const unsigned calls = centered_ioctl_calls;
  bool result;
  {
    centered_payload::Scope scope(state);
    // Production composition and its observer must perform no allocation.
    allocation_fault::fail_next = true;
    result = ComposeCenteredCanvas(fixture->picture, canvas, false, direct);
    const bool allocation_unused = allocation_fault::fail_next;
    allocation_fault::fail_next = false;
    Require(allocation_unused, "composition observation does not allocate on the worker");
  }
  failing_fd = -1;
  fixture->small->object_fds.clear(); canvas->object_fds.clear();
  Require(centered_ioctl_calls == calls + 4, "composition uses exactly its original two CPU leases");
  return result;
}
bool CopyObserved(centered_payload::State *state, CenteredFixture *fixture,
                  EndFailure failure = EndFailure::None,
                  CopyPurpose purpose = CopyPurpose::DecodedPublic) {
  fixture->picture.object_fds.push_back(777);
  fixture->public_canvas.object_fds.push_back(888);
  SetFailure(failure, false);
  const unsigned calls = centered_ioctl_calls;
  bool result;
  {
    centered_payload::Scope scope(state);
    allocation_fault::fail_next = true;
    result = CopyNv12Surface(fixture->picture, &fixture->public_canvas, purpose);
    const bool allocation_unused = allocation_fault::fail_next;
    allocation_fault::fail_next = false;
    Require(allocation_unused, "public-copy observation does not allocate on the worker");
  }
  failing_fd = -1;
  fixture->picture.object_fds.clear(); fixture->public_canvas.object_fds.clear();
  Require(centered_ioctl_calls == calls + 4, "public observation uses exactly its original two CPU leases");
  return result;
}
void CheckPackedCanvas(const uint8_t *pixels, unsigned height, unsigned seed) {
  bool equal = true;
  size_t offset = 0;
  for (unsigned plane = 0; plane != 2; ++plane) {
    const unsigned top = plane == 0 ? 180 : 90;
    const unsigned active = plane == 0 ? 720 : 360;
    for (unsigned row = 0; row != (plane == 0 ? height : height / 2); ++row)
      for (unsigned x = 0; x != 1920; ++x) {
        const uint8_t expected = row >= top && row < top + active && x >= 320 && x < 1600
            ? Sample(seed, plane, row - top, x - 320) : (plane == 0 ? 16 : 128);
        equal = (pixels[offset++] == expected) && equal;
      }
  }
  Require(equal, "every active Y/UV byte equals independent centered ROI or limited black");
}
void CheckTails(const Surface &surface) {
  bool intact = true;
  for (unsigned plane = 0; plane != 2; ++plane)
    for (unsigned row = 0; row != (plane == 0 ? surface.height : surface.height / 2); ++row)
      for (unsigned x = surface.width; x != surface.pitch[plane]; ++x)
        intact = (surface.planes[plane][size_t{row} * surface.pitch[plane] + x] == 0xa5) && intact;
  Require(intact, "observer and production composition preserve pitch tails");
}
void CheckSuccessfulRoute(bool direct, unsigned physical_height, unsigned public_height) {
  using namespace centered_payload;
  std::unique_ptr<State> state(new State(10));
  CenteredFixture fixture(1, physical_height, public_height);
  Require(state->directory >= 0 && ComposeObserved(state.get(), &fixture, direct),
          "production centered composition succeeds");
  auto &slot = state->streams[0].slots[0];
  Require(slot.selected && slot.compose_copied && slot.compose_committed &&
          !slot.materialized, "successful compose is still awaiting original materialized binding");
  if (!direct) {
    Require(!slot.public_started, "private composition does not pretend to be public");
    Require(CopyObserved(state.get(), &fixture), "production decoded-public copy succeeds");
  }
  const auto &canvas = direct ? fixture.public_canvas : fixture.picture;
  Require(slot.small.serial == fixture.small->payload_observer_stamp.serial &&
          slot.composed.serial == canvas.payload_observer_stamp.serial &&
          SameGeometry(slot.composed, slot.copy_source) &&
          slot.public_canvas.serial == fixture.public_canvas.payload_observer_stamp.serial &&
          slot.composed.height == physical_height && slot.public_canvas.height == public_height,
          "metadata retains exact small, composed, copy-source and actual public storage views");
  Require(slot.small_bytes == kSmallBytes &&
          slot.composed_bytes == size_t{1920} * physical_height * 3 / 2 &&
          slot.public_bytes == size_t{1920} * public_height * 3 / 2,
          "packed lengths use each physical geometry and omit pitch tails");
  bool small_equal = true;
  size_t offset = 0;
  for (unsigned plane = 0; plane != 2; ++plane)
    for (unsigned row = 0; row != (plane == 0 ? 720U : 360U); ++row)
      for (unsigned x = 0; x != 1280; ++x)
        small_equal = (slot.small_pixels[offset++] == Sample(fixture.seed, plane, row, x)) && small_equal;
  Require(small_equal, "entire small NV12 payload retains its nonuniform source identity");
  CheckPackedCanvas(slot.composed_pixels.data(), physical_height, fixture.seed);
  CheckPackedCanvas(slot.public_pixels.data(), public_height, fixture.seed);
  CheckTails(*fixture.small); CheckTails(canvas); CheckTails(fixture.public_canvas);
  auto fields = fixture.Fields();
  ++fields.decode_identity; state->Materialized(fields);
  Require(!slot.materialized, "unrelated materialized identity cannot qualify a snapshot");
  fields = fixture.Fields(); ++fields.owner; state->Materialized(fields);
  Require(!slot.materialized, "unrelated public owner cannot qualify a snapshot");
  state->Materialized(fixture.Fields());
  Require(State::Complete(slot), "successful exact public binding qualifies only after materialized");
  Require(state->Select(fixture.picture, canvas, *fixture.small, false, direct) == nullptr &&
          slot.binding.token == fixture.picture.expected_timestamp,
          "selected source binding cannot be overwritten by a repeat");
}
void CheckFailedEnd(bool direct, EndFailure failure) {
  using namespace centered_payload;
  std::unique_ptr<State> state(new State(11));
  CenteredFixture fixture(1, 1088, 1088);
  const bool compose_failure = failure == EndFailure::ComposeWrite || failure == EndFailure::ComposeRead;
  Require(ComposeObserved(state.get(), &fixture, direct, failure) == !compose_failure,
          "existing composition END failure remains a production failure");
  auto &slot = state->streams[0].slots[0];
  if (!compose_failure) {
    Require(!direct && !CopyObserved(state.get(), &fixture, failure),
            "existing decoded-public END failure remains a production failure");
    Require(slot.public_copied && !slot.public_committed,
            "public bytes remain tentative after either existing END fails");
  } else Require(slot.compose_copied && !slot.compose_committed,
                 "composed bytes remain tentative after either existing END fails");
  state->Materialized(fixture.Fields());
  Require(slot.failed && !slot.materialized && !State::Complete(slot),
          "END failure can never qualify by a later materialized event");
  Require(ComposeObserved(state.get(), &fixture, direct), "production retry can proceed normally");
  Require(slot.failed && slot.compose_committed == !compose_failure,
          "successful retry never replaces the first failed selected observation");
  if (!direct) {
    Require(CopyObserved(state.get(), &fixture), "ordinary public retry remains functional");
    Require(!slot.public_committed, "observer does not replace failed public snapshot with retry");
  }
  CenteredFixture second(2, 1088, 1088), third(3, 1088, 1088);
  Require(ComposeObserved(state.get(), &second, true) && ComposeObserved(state.get(), &third, true),
          "later production compositions remain functional after observer quota");
  Require(state->streams[0].slots[1].binding.token == second.picture.expected_timestamp &&
          state->streams[0].slots[0].binding.token == fixture.picture.expected_timestamp,
          "first two distinct selected bindings stay fixed after a failed first sample");
  const int index = state->Retire(3, 4, 0);
  Require(index == 0 && state->streams[0].sealed && !state->Complete(state->streams[0]),
          "incomplete failed stream is permanently sealed at consuming retirement");
  state->Flush(index);
  Require(!state->streams[0].publication_started && state->Retire(3, 4, 0) == -1,
          "failed stream cannot publish or claim a second retirement");
}
void CheckRejectedPublic(unsigned mutation) {
  using namespace centered_payload;
  std::unique_ptr<State> state(new State(12));
  CenteredFixture fixture(1);
  Require(ComposeObserved(state.get(), &fixture, false), "rejection fixture composition");
  auto &slot = state->streams[0].slots[0];
  const uint64_t epoch = fixture.picture.payload_observer_stamp.epoch;
  const uint64_t identity = fixture.public_canvas.trace_decode_identity;
  const uint64_t context = fixture.picture.trace_context;
  if (mutation == 0) fixture.picture.payload_observer_stamp.Installed();
  if (mutation == 1) ++fixture.public_canvas.trace_decode_identity;
  if (mutation == 2) ++fixture.picture.trace_context;
  Require(CopyObserved(state.get(), &fixture), "rejected diagnostic copy does not change production return");
  Require(slot.failed && slot.public_started && !slot.public_committed,
          "storage epoch or binding mismatch fails closed for selected source");
  fixture.picture.payload_observer_stamp.epoch = epoch;
  fixture.public_canvas.trace_decode_identity = identity;
  fixture.picture.trace_context = context;
  Require(CopyObserved(state.get(), &fixture), "restored ordinary public copy succeeds");
  Require(!slot.public_copied && !slot.public_committed,
          "restored storage cannot replace the first rejected public observation");
}
void CheckReusedComposedStorage(unsigned rejected_second) {
  using namespace centered_payload;
  std::unique_ptr<State> state(new State(17 + rejected_second));
  CenteredFixture fixture(1);
  Require(ComposeObserved(state.get(), &fixture, false) && CopyObserved(state.get(), &fixture),
          "first binding uses production private composition and public copy");
  state->Materialized(fixture.Fields());
  auto &first = state->streams[0].slots[0];
  Require(State::Complete(first), "first binding is complete before storage reuse");
  const auto original = first.composed;
  // Model a recycled private Surface at the same address/serial, with the
  // storage epoch and source/public binding advanced for another picture.
  fixture.seed = 2;
  for (Surface *surface : {&fixture.picture, &fixture.public_canvas, fixture.small.get()}) {
    surface->payload_observer_stamp.Installed();
    surface->expected_timestamp = 1002;
    surface->decode_identity = surface->trace_decode_identity = 2002;
    surface->trace_submission_ordinal = 2;
    surface->ready = false; surface->failed = false;
    surface->frame_timestamp = 0;
  }
  fixture.small->ready = true;
  fixture.small->frame_timestamp = fixture.small->expected_timestamp;
  for (unsigned plane = 0; plane != 2; ++plane)
    for (unsigned row = 0; row != (plane == 0 ? 720U : 360U); ++row)
      for (unsigned x = 0; x != 1280; ++x)
        fixture.small->planes[plane][size_t{row} * fixture.small->pitch[plane] + x] =
            Sample(fixture.seed, plane, row, x);
  Require(ComposeObserved(state.get(), &fixture, false), "second binding composes into recycled storage");
  auto &second = state->streams[0].slots[1];
  Require(second.selected && second.compose_committed &&
          second.composed.serial == original.serial && second.composed.epoch != original.epoch &&
          second.composed.plane[0] == original.plane[0],
          "distinct selected binding retains reused serial/address and its new epoch");
  if (rejected_second == 1) ++fixture.public_canvas.trace_decode_identity;
  if (rejected_second == 2) ++fixture.picture.trace_context;
  Require(CopyObserved(state.get(), &fixture), "recycled source ordinary public copy succeeds");
  Require(State::Complete(first) && first.reject_mask == 0 &&
          first.composed.epoch == original.epoch,
          "reused storage cannot alter or select the older completed binding");
  if (rejected_second) {
    Require(second.failed && second.public_started && !second.public_committed,
            "mismatch poisons its own selected recycled binding rather than the older serial match");
    fixture.public_canvas.trace_decode_identity = fixture.public_canvas.decode_identity;
    fixture.picture.trace_context = fixture.public_canvas.trace_context;
    Require(CopyObserved(state.get(), &fixture), "restored recycled source ordinary copy remains functional");
    state->Materialized(fixture.Fields());
    Require(second.failed && !second.public_copied && !second.materialized,
            "later correct copy cannot replace the first failed selected recycled binding");
  } else {
    state->Materialized(fixture.Fields());
    Require(State::Complete(second) && SameGeometry(second.composed, second.copy_source),
            "exact current source binding wins over older slot sharing its serial");
    CheckPackedCanvas(second.composed_pixels.data(), 1088, 2);
    CheckPackedCanvas(second.public_pixels.data(), 1080, 2);
  }
  CheckPackedCanvas(first.composed_pixels.data(), 1088, 1);
  CheckPackedCanvas(first.public_pixels.data(), 1080, 1);
  Require(state->Retire(3, 4, 0) == 0 &&
          state->Complete(state->streams[0]) == (rejected_second == 0),
          "retirement qualifies both reused bindings only when each original public attempt completed");
}
void CheckScopeAndGenericCopy() {
  using namespace centered_payload;
  std::unique_ptr<State> state(new State(13));
  CenteredFixture fixture(1);
  const auto saved = fixture.picture.centered_geometry;
  for (unsigned mutation = 0; mutation != 10; ++mutation) {
    fixture.picture.centered_geometry = saved;
    if (mutation == 0) fixture.picture.centered_geometry.full_range = true;
    if (mutation == 1) fixture.picture.centered_geometry.enabled = false;
    if (mutation == 2) fixture.picture.centered_geometry.x = 318;
    if (mutation == 3) fixture.picture.centered_geometry.visible_height = 1088;
    if (mutation == 4) fixture.picture.centered_geometry.capture_height = 704;
    if (mutation == 5) ++fixture.small->trace_decode_identity;
    if (mutation == 6) ++fixture.picture.trace_decode_identity;
    if (mutation == 8) fixture.picture.centered_geometry.canvas_height = 1084;
    if (mutation == 9) fixture.picture.centered_geometry.capture_width = 1264;
    Require(state->Select(fixture.picture, fixture.picture, *fixture.small,
                          mutation == 7, false) == nullptr,
            "full-range, noncentered, compact and unrelated bindings are outside bounded scope");
    fixture.small->trace_decode_identity = fixture.small->decode_identity;
    fixture.picture.trace_decode_identity = fixture.picture.decode_identity;
  }
  fixture.picture.centered_geometry = saved;
  Require(ComposeObserved(state.get(), &fixture, false), "eligible route is selectable after rejected scope");
  Require(CopyObserved(state.get(), &fixture, EndFailure::None, CopyPurpose::Generic),
          "generic production copy succeeds");
  Require(!state->streams[0].slots[0].public_started,
          "generic copy is never mislabeled as decoded-public materialization");
  Require(CopyObserved(state.get(), &fixture), "later real decoded-public copy succeeds");
  Require(state->streams[0].slots[0].public_committed, "real decoded-public copy is observed");
  State *outer = active;
  {
    Scope scope(state.get()); Require(active == state.get(), "scope installs active independent observer");
    { Scope disabled(nullptr); Require(active == nullptr, "nested disabled scope masks observation"); }
    Require(active == state.get(), "nested scope restores previous observer");
  }
  Require(active == outer, "scope never retains worker state after return");
}
void Populate(centered_payload::State *state, uint64_t context, uint64_t generation) {
  for (unsigned ordinal = 1; ordinal != 3; ++ordinal) {
    CenteredFixture fixture(ordinal, 1088, 1080, context, generation);
    Require(ComposeObserved(state, &fixture, false) && CopyObserved(state, &fixture),
            "retirement fixture executes both production stages");
    state->Materialized(fixture.Fields());
  }
}
void ArtifactName(char *name, size_t size, uint64_t instance, uint64_t context,
                  uint64_t generation, const char *suffix) {
  std::snprintf(name, size, "centered-payload-%lld-%llu-%llu-%llu%s",
      static_cast<long long>(getpid()), static_cast<unsigned long long>(instance),
      static_cast<unsigned long long>(context), static_cast<unsigned long long>(generation), suffix);
}
void RemoveArtifacts(centered_payload::State *state, uint64_t context, uint64_t generation) {
  char name[192], suffix[40];
  ArtifactName(name, sizeof(name), state->session, context, generation, ".json");
  Require(unlinkat(state->directory, name, 0) == 0, "remove CPU diagnostic manifest");
  for (unsigned slot = 0; slot != 2; ++slot)
    for (const char *kind : {"small", "composed", "public"}) {
      std::snprintf(suffix, sizeof(suffix), "-s%u-%s.nv12", slot, kind);
      ArtifactName(name, sizeof(name), state->session, context, generation, suffix);
      Require(unlinkat(state->directory, name, 0) == 0, "remove CPU diagnostic packed blob");
    }
}
void CheckRetirementPublication() {
  using namespace centered_payload;
  std::shared_ptr<State> state(new State(99));
  Populate(state.get(), 3, 4);
  // A second lifetime is still live when first natural EOF publishes.
  Populate(state.get(), 3, 5);
  Require(state->Retire(3, 6, 0) == -1, "unselected generation cannot retire another lifetime");
  char manifest[192];
  ArtifactName(manifest, sizeof(manifest), 99, 3, 4, ".json");
  std::mutex caller_mutex;
  {
    PublishOnReturn publication;
    std::unique_lock<std::mutex> lock(caller_mutex);
    publication.state = state;
    publication.index = state->Retire(3, 4, 0);
    Require(publication.index == 0 && state->Complete(state->streams[0]) &&
            !state->streams[1].sealed && !state->streams[0].publication_started &&
            faccessat(state->directory, manifest, F_OK, 0) != 0,
            "first selected consuming EOF seals its stream without I/O or second-lifetime retirement");
  }
  Require(caller_mutex.try_lock(), "caller mutex has unlocked at publication return");
  caller_mutex.unlock();
  Require(state.use_count() == 1 && state->streams[0].manifest_written &&
          !state->streams[1].publication_started &&
          faccessat(state->directory, manifest, F_OK, 0) == 0,
          "return guard publishes only frozen first stream while State owner remains alive");
  Require(state->Retire(3, 4, 0) == -1, "one selected retirement publishes at most once");
  const auto selected = state->streams[0].slots[0].binding;
  CenteredFixture late(7, 1088, 1080, 3, 4);
  Require(ComposeObserved(state.get(), &late, false), "late ordinary composition remains functional");
  Require(state->streams[0].slots[0].binding.token == selected.token,
          "sealed stream ignores subsequent worker observations");
  state->Flush(0);
  Require(state->streams[0].manifest_written && !state->streams[0].publication_errno,
          "second flush performs no exclusive-file rewrite");
  {
    PublishOnReturn publication;
    std::unique_lock<std::mutex> lock(caller_mutex);
    publication.state = state; publication.index = state->Retire(3, 5, 0);
  }
  Require(state->streams[1].manifest_written, "second lifetime independently publishes its own frozen arrays");
  struct stat metadata{};
  char blob[192];
  ArtifactName(blob, sizeof(blob), 99, 3, 4, "-s0-public.nv12");
  Require(fstatat(state->directory, blob, &metadata, AT_SYMLINK_NOFOLLOW) == 0 &&
          metadata.st_size == static_cast<off_t>(size_t{1920} * 1080 * 3 / 2),
          "written public blob has its actual 1080-row active packed length");
  RemoveArtifacts(state.get(), 3, 4); RemoveArtifacts(state.get(), 3, 5);
}
void CheckExclusivePublicationFailure() {
  using namespace centered_payload;
  std::unique_ptr<State> state(new State(98));
  Populate(state.get(), 3, 4);
  char blob[192], manifest[192];
  ArtifactName(blob, sizeof(blob), 98, 3, 4, "-s0-small.nv12");
  ArtifactName(manifest, sizeof(manifest), 98, 3, 4, ".json");
  const int fd = openat(state->directory, blob,
      O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
  const uint8_t marker = 0x7b;
  Require(fd >= 0 && write(fd, &marker, 1) == 1 && close(fd) == 0,
          "create preserved preexisting diagnostic artifact");
  const int index = state->Retire(3, 4, 0);
  state->Flush(index);
  struct stat metadata{};
  Require(state->streams[0].publication_started && !state->streams[0].manifest_written &&
          state->streams[0].publication_errno &&
          faccessat(state->directory, manifest, F_OK, 0) != 0 &&
          fstatat(state->directory, blob, &metadata, AT_SYMLINK_NOFOLLOW) == 0 && metadata.st_size == 1,
          "exclusive blob collision preserves prior artifact and withholds qualifying manifest");
  const int input = openat(state->directory, blob, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  uint8_t found = 0;
  Require(input >= 0 && read(input, &found, 1) == 1 && close(input) == 0 && found == marker,
          "exclusive output failure never overwrites old blob contents");
  state->Flush(index);
  Require(!state->streams[0].manifest_written &&
          faccessat(state->directory, manifest, F_OK, 0) != 0,
          "failed publication is claimed once and cannot retry partial outputs");
  for (unsigned slot = 0; slot != 2; ++slot)
    for (const char *kind : {"small", "composed", "public"}) {
      char suffix[40];
      std::snprintf(suffix, sizeof(suffix), "-s%u-%s.nv12", slot, kind);
      ArtifactName(blob, sizeof(blob), 98, 3, 4, suffix);
      Require(unlinkat(state->directory, blob, 0) == 0, "remove preserved CPU partial-output fixture");
    }
}
void CheckIncompleteAndUnavailable() {
  using namespace centered_payload;
  {
    std::unique_ptr<State> state(new State(14));
    CenteredFixture fixture(1, 1088, 1088);
    Require(ComposeObserved(state.get(), &fixture, true), "incomplete retirement fixture");
    const int index = state->Retire(3, 4, 0);
    state->Materialized(fixture.Fields());
    Require(index == 0 && !state->streams[0].slots[0].materialized &&
            (state->streams[0].reject_mask & (Incomplete | MissingMaterialized)) != 0,
            "missing materialized or second slot seals with explicit incomplete status");
    state->PrintStatus(index); state->Flush(index);
    Require(!state->streams[0].publication_started, "incomplete retirement emits status but no manifest");
  }
  {
    std::unique_ptr<State> state(new State(15));
    Populate(state.get(), 3, 4);
    const int index = state->Retire(3, 4, 1);
    state->Flush(index);
    Require(index == 0 && (state->streams[0].reject_mask & RetirementFailure) &&
            !state->streams[0].publication_started,
            "failed consuming retirement cannot qualify otherwise complete slots");
  }
  {
    // Constructor rejects missing/relative directories without altering any
    // environment while workers could be observing the original State.
    const char *existing = std::getenv("CRYSTALHD_TEST_CENTERED_PAYLOAD_DIR");
    const std::string original = existing ? existing : "";
    Require(setenv("CRYSTALHD_TEST_CENTERED_PAYLOAD_DIR", "relative-directory", 1) == 0,
            "set unavailable private output fixture");
    std::unique_ptr<State> state(new State(16));
    Require(setenv("CRYSTALHD_TEST_CENTERED_PAYLOAD_DIR", original.c_str(), 1) == 0,
            "restore private output directory");
    CenteredFixture fixture(1, 1088, 1088);
    Require(ComposeObserved(state.get(), &fixture, true), "unavailable observer preserves production success");
    const int index = state->Retire(3, 4, 0);
    state->PrintStatus(index); state->Flush(index);
    Require(state->directory < 0 && state->directory_errno == EINVAL && index == 0 &&
            (state->streams[0].reject_mask & Directory) && !state->streams[0].publication_started,
            "unavailable directory is a bounded explicit rejection, never qualifying output");
  }
}
}  // namespace

extern "C" int __wrap_ioctl(int fd, unsigned long request, ...) {
  va_list args; va_start(args, request);
  const auto *sync = va_arg(args, const dma_buf_sync *);
  va_end(args);
  ++centered_ioctl_calls;
  if (fd == failing_fd && request == DMA_BUF_IOCTL_SYNC && sync->flags == failing_flags) {
    errno = EIO; return -1;
  }
  return CrystalhdExistingBobIoctl(fd, request, sync);
}

int main() {
  char directory[] = "/tmp/crystalhd-centered-payload-cpu-XXXXXX";
  if (!mkdtemp(directory) || setenv("CRYSTALHD_TEST_CENTERED_PAYLOAD_DIR", directory, 1) != 0) return 1;
  int result = 0;
  try {
    CheckSuccessfulRoute(true, 1080, 1080);
    CheckSuccessfulRoute(true, 1088, 1088);
    CheckSuccessfulRoute(false, 1088, 1080);
    CheckSuccessfulRoute(false, 1088, 1088);
    for (bool direct : {false, true})
      for (EndFailure failure : {EndFailure::ComposeWrite, EndFailure::ComposeRead})
        CheckFailedEnd(direct, failure);
    for (EndFailure failure : {EndFailure::PublicWrite, EndFailure::PublicRead})
      CheckFailedEnd(false, failure);
    for (unsigned mutation = 0; mutation != 3; ++mutation) CheckRejectedPublic(mutation);
    for (unsigned mutation = 0; mutation != 3; ++mutation) CheckReusedComposedStorage(mutation);
    CheckScopeAndGenericCopy();
    CheckRetirementPublication();
    CheckExclusivePublicationFailure();
    CheckIncompleteAndUnavailable();
    Require(centered_payload::active == nullptr, "no active observer survives CPU controls");
    std::printf("VA centered payload CPU controls: %u checks PASS\n", checks);
  } catch (const std::exception &error) {
    allocation_fault::fail_next = false;
    std::fprintf(stderr, "VA centered payload CPU FAIL: %s\n", error.what());
    result = 1;
  }
  unsetenv("CRYSTALHD_TEST_CENTERED_PAYLOAD_DIR");
  if (rmdir(directory) != 0) result = 1;
  return result;
}
