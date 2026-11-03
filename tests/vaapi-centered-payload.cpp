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
std::string ReadManifest(centered_payload::State *state, uint64_t context, uint64_t generation) {
  char name[192];
  ArtifactName(name, sizeof(name), state->session, context, generation, ".json");
  const int fd = openat(state->directory, name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  Require(fd >= 0, "open independently published CPU manifest");
  std::array<char, 16384> bytes{};
  size_t size = 0;
  bool good = true;
  for (;;) {
    const ssize_t got = read(fd, bytes.data() + size, bytes.size() - size);
    if (got < 0 && errno == EINTR) continue;
    if (got < 0) { good = false; break; }
    if (got == 0) break;
    size += static_cast<size_t>(got);
    if (size == bytes.size()) { good = false; break; }
  }
  const bool closed = close(fd) == 0;
  Require(good && closed && size != 0, "published manifest is bounded, readable and complete");
  return std::string(bytes.data(), size);
}
void CheckPublicationSchema(centered_payload::State *state, uint64_t context,
                            uint64_t generation, bool reset, uint64_t next_generation = 0) {
  const std::string document = ReadManifest(state, context, generation);
  Require(document.find(reset ? "\"schema\":\"crystalhd-centered-payload-v2\"" :
                                "\"schema\":\"crystalhd-centered-payload-v1\"") != std::string::npos,
          "only generation reset publishes v2; ordinary context retirement preserves v1");
  char publication[512];
  if (reset)
    std::snprintf(publication, sizeof(publication),
        "\"publication\":{\"kind\":\"selected-decoder-generation-reset\","
        "\"context\":%llu,\"generation\":%llu,\"status\":0,\"next_generation\":%llu,"
        "\"close_status\":0,\"device_closed\":true,\"after_driver_unlock\":true,\"all_slots_frozen\":true}",
        static_cast<unsigned long long>(context), static_cast<unsigned long long>(generation),
        static_cast<unsigned long long>(next_generation));
  else
    std::snprintf(publication, sizeof(publication),
        "\"publication\":{\"kind\":\"selected-decoder-context-retirement\","
        "\"context\":%llu,\"generation\":%llu,\"status\":0,"
        "\"after_driver_unlock\":true,\"all_slots_frozen\":true}",
        static_cast<unsigned long long>(context), static_cast<unsigned long long>(generation));
  Require(document.find(publication) != std::string::npos &&
          document.find("\"slots_required\":2,\"media_pts_observed\":false") != std::string::npos,
          "publication records exact old/new lifetime, close result and unchanged payload scope");
  if (!reset)
    Require(document.find("\"next_generation\"") == std::string::npos &&
            document.find("\"close_status\"") == std::string::npos &&
            document.find("\"device_closed\"") == std::string::npos,
            "ordinary v1 retirement does not acquire reset-only metadata");
}
void CheckResetStopReopenPublication() {
  using namespace centered_payload;
  Require(kStreams == 3 && kSlots == 2, "controls observer reserves exactly three two-binding lifetimes");
  std::shared_ptr<State> state(new State(100));
  Populate(state.get(), 3, 4);
  Require(state->RetireReset(9, 4, 5, 0, 0, true) == -1 &&
          state->RetireReset(3, 5, 6, 0, 0, true) == -1 && !state->streams[0].sealed,
          "unselected context or new generation cannot retire the selected old generation");
  std::mutex caller_mutex;
  char manifest[192];
  ArtifactName(manifest, sizeof(manifest), 100, 3, 4, ".json");
  {
    std::array<PublishOnReturn, kStreams> publications;
    std::unique_lock<std::mutex> lock(caller_mutex);
    publications[0].state = state;
    publications[0].index = state->RetireReset(3, 4, 5, 0, 0, true);
    Require(publications[0].index == 0 && state->Complete(state->streams[0]) &&
            state->streams[0].generation == 4 && state->streams[0].next_generation == 5 &&
            state->streams[0].generation_reset && !state->streams[0].publication_started &&
            faccessat(state->directory, manifest, F_OK, 0) != 0,
            "successful reset freezes old generation under caller lock without publishing");
  }
  Require(caller_mutex.try_lock(), "reset publisher returns after ordinary caller unlock");
  caller_mutex.unlock();
  Require(state.use_count() == 1 && state->streams[0].manifest_written &&
          state->streams[0].status_printed && !state->streams[1].used,
          "reset publishes first frozen stream while independent State owner remains alive");
  CheckPublicationSchema(state.get(), 3, 4, true, 5);
  Require(state->RetireReset(3, 4, 5, 0, 0, true) == -1 && state->Retire(3, 4, 0) == -1,
          "reset retirement cannot publish again through either retirement API");
  {
    CenteredFixture stale(8, 1088, 1080, 3, 4);
    Require(ComposeObserved(state.get(), &stale, false) && CopyObserved(state.get(), &stale),
            "old-generation ordinary operations remain unaffected by sealed diagnostics");
    state->Materialized(stale.Fields());
    Require(state->streams[0].slots[0].binding.token == 1001 &&
            state->streams[0].slots[1].binding.token == 1002,
            "late old-generation observations cannot replace either frozen selected binding");
  }
  Populate(state.get(), 3, 5);
  {
    std::array<PublishOnReturn, kStreams> publications;
    std::unique_lock<std::mutex> lock(caller_mutex);
    publications[1].state = state;
    publications[1].index = state->Retire(3, 5, 0);
    Require(publications[1].index == 1 && !state->streams[1].publication_started &&
            !state->streams[1].generation_reset, "post-seek Stop retains ordinary consuming retirement");
  }
  Require(state->streams[1].manifest_written && state->streams[0].next_generation == 5,
          "post-seek Stop publishes separately without modifying the reset record");
  CheckPublicationSchema(state.get(), 3, 5, false);
  Populate(state.get(), 4, 1);
  {
    std::array<PublishOnReturn, kStreams> publications;
    std::unique_lock<std::mutex> lock(caller_mutex);
    publications[2].state = state;
    publications[2].index = state->Retire(4, 1, 0);
    Require(publications[2].index == 2 && !state->streams[2].publication_started,
            "same-State reopened decoder uses the third fixed lifetime");
  }
  Require(state.use_count() == 1 && state->streams[2].manifest_written,
          "third reopened lifetime publishes without growing observer storage");
  CheckPublicationSchema(state.get(), 4, 1, false);
  {
    CenteredFixture overflow(9, 1088, 1080, 99, 1);
    Require(ComposeObserved(state.get(), &overflow, false) && CopyObserved(state.get(), &overflow),
            "fourth-lifetime production composition/copy continue normally beyond diagnostic bound");
    state->Materialized(overflow.Fields());
    Require(state->Find(99, 1) == nullptr && state->Retire(99, 1, 0) == -1 &&
            state->RetireReset(99, 1, 2, 0, 0, true) == -1,
            "fourth lifetime cannot replace sealed selections or expand the three-stream bound");
  }
  for (unsigned i = 0; i != kStreams; ++i) {
    Require(state->streams[i].slots[0].binding.token == 1001 &&
            state->streams[i].slots[1].binding.token == 1002,
            "all three lifetimes preserve their first two distinct source bindings");
    CheckPackedCanvas(state->streams[i].slots[0].composed_pixels.data(), 1088, 1);
    CheckPackedCanvas(state->streams[i].slots[0].public_pixels.data(), 1080, 1);
  }
  state->Flush(0);
  Require(state->streams[0].manifest_written && !state->streams[0].publication_errno,
          "repeated reset flush never rewrites exclusive artifacts");
  RemoveArtifacts(state.get(), 3, 4); RemoveArtifacts(state.get(), 3, 5);
  RemoveArtifacts(state.get(), 4, 1);
}
void CheckResetBatchPublication() {
  using namespace centered_payload;
  std::shared_ptr<State> state(new State(101));
  for (unsigned i = 0; i != kStreams; ++i) Populate(state.get(), 10 + i, 20 + i);
  std::mutex caller_mutex;
  {
    std::array<PublishOnReturn, kStreams> publications;
    std::unique_lock<std::mutex> lock(caller_mutex);
    for (unsigned i = 0; i != kStreams; ++i) {
      publications[i].state = state;
      publications[i].index = state->RetireReset(10 + i, 20 + i, 21 + i, i == 1 ? 1 : 0, 0, true);
      Require(publications[i].index == static_cast<int>(i) &&
              !state->streams[i].publication_started,
              "one caller freezes multiple selected old streams without in-lock publication");
    }
  }
  Require(caller_mutex.try_lock(), "all reset publication guards follow the one ordinary unlock");
  caller_mutex.unlock();
  Require(state.use_count() == 1, "batch publishers retain no State pins after caller return");
  for (unsigned i = 0; i != kStreams; ++i) {
    Require(state->streams[i].sealed && state->streams[i].status_printed &&
            state->streams[i].manifest_written == (i != 1) &&
            state->streams[i].publication_started == (i != 1),
            "one decoder reset failure cannot poison or qualify another decoder's independent result");
    if (i != 1) {
      CheckPublicationSchema(state.get(), 10 + i, 20 + i, true, 21 + i);
      RemoveArtifacts(state.get(), 10 + i, 20 + i);
    }
  }
}
void CheckFailedResets() {
  using namespace centered_payload;
  struct Case {
    uint64_t previous, next;
    int status, close_status;
    bool closed;
    uint64_t reason;
  };
  const Case failures[] = {
    {4, 5, 1, 0, true, RetirementFailure},
    {4, 5, 0, 1, true, ResetCloseFailure},
    {4, 5, 0, -1, true, ResetCloseFailure},
    {4, 5, 0, 0, false, ResetDeviceOpen},
    {4, 4, 0, 0, true, ResetGenerationMismatch},
    {4, 3, 0, 0, true, ResetGenerationMismatch},
    {4, 6, 0, 0, true, ResetGenerationMismatch},
    {uint64_t(-1), 0, 0, 0, true, ResetGenerationMismatch}
  };
  unsigned number = 0;
  for (const auto &failure : failures) {
    std::unique_ptr<State> state(new State(110 + number++));
    Populate(state.get(), 3, failure.previous);
    const int index = state->RetireReset(3, failure.previous, failure.next,
        failure.status, failure.close_status, failure.closed);
    Require(index == 0 && state->streams[0].sealed && state->streams[0].generation_reset &&
            state->streams[0].generation == failure.previous &&
            state->streams[0].next_generation == failure.next &&
            state->streams[0].close_status == failure.close_status &&
            state->streams[0].device_closed == failure.closed &&
            (state->streams[0].reject_mask & failure.reason) && !state->Complete(state->streams[0]),
            "failed VA reset, close, live device or invalid generation step seals with its original reason");
    state->PrintStatus(index); state->Flush(index);
    char manifest[192];
    ArtifactName(manifest, sizeof(manifest), state->session, 3, failure.previous, ".json");
    Require(state->streams[0].status_printed && !state->streams[0].publication_started &&
            faccessat(state->directory, manifest, F_OK, 0) != 0,
            "failed reset emits bounded status and cannot publish qualifying manifest");
    Require(state->RetireReset(3, failure.previous, failure.previous + 1, 0, 0, true) == -1 &&
            state->Retire(3, failure.previous, 0) == -1 &&
            state->streams[0].retirement_status == failure.status &&
            state->streams[0].next_generation == failure.next &&
            state->streams[0].close_status == failure.close_status &&
            state->streams[0].device_closed == failure.closed &&
            (state->streams[0].reject_mask & failure.reason),
            "later successful attempt cannot relabel a failed selected reset as passing");
    state->Flush(index);
    Require(!state->streams[0].publication_started, "failed reset remains permanently nonpublishing");
  }
  std::unique_ptr<State> state(new State(119));
  CenteredFixture incomplete(1, 1088, 1088);
  Require(ComposeObserved(state.get(), &incomplete, true), "incomplete reset fixture composes normally");
  const int index = state->RetireReset(3, 4, 5, 0, 0, true);
  state->Materialized(incomplete.Fields());
  Require(index == 0 && state->streams[0].sealed &&
          (state->streams[0].reject_mask & Incomplete) &&
          (state->streams[0].reject_mask & MissingMaterialized) &&
          !state->streams[0].slots[0].materialized && !state->Complete(state->streams[0]),
          "successful reset metadata cannot qualify missing slot or late materialized evidence");
  state->PrintStatus(index); state->Flush(index);
  Require(!state->streams[0].publication_started && state->RetireReset(3, 4, 5, 0, 0, true) == -1,
          "incomplete reset stays sealed and cannot be retried into qualification");
}
enum class TargetMutation { None, SourceCase, Magic, Complement, Crc, Replica, Threshold, Spread };
void FillTargetSource(CenteredFixture *fixture, unsigned ordinal,
                      TargetMutation mutation = TargetMutation::None,
                      unsigned low = 16, unsigned high = 235, bool coded_vertical = false) {
  // Construct full scaled source rectangles independently of the observer's
  // 3x3 patch lookup. Code bytes follow native-picture-fixture.py, including
  // a freshly computed CRC; wrong case/magic controls keep a valid CRC.
  uint8_t code[] = {0xc5, 0x3d, 2, static_cast<uint8_t>(ordinal),
                    static_cast<uint8_t>(ordinal ^ 255), 0};
  if (mutation == TargetMutation::SourceCase) code[2] = 3;
  if (mutation == TargetMutation::Magic) code[0] ^= 1;
  unsigned crc = 0;
  for (unsigned byte = 0; byte != 4; ++byte) {
    crc ^= code[byte];
    for (unsigned bit = 0; bit != 8; ++bit)
      crc = ((crc << 1) ^ ((crc & 128) ? 7 : 0)) & 255;
  }
  code[5] = static_cast<uint8_t>(crc);
  if (mutation == TargetMutation::Complement) code[4] ^= 1;
  if (mutation == TargetMutation::Crc) code[5] ^= 1;
  Surface &small = *fixture->small;
  for (unsigned row = 0; row != 360; ++row)
    memset(small.planes[1] + size_t{row} * small.pitch[1], 128, 1280);
  for (unsigned replica = 0; replica != 2; ++replica) {
    const unsigned source_y = replica == 0 ? 16 : 64;
    const unsigned top = coded_vertical ? source_y * 45 / 68 : source_y * 2 / 3;
    const unsigned bottom = coded_vertical ? (source_y + 32) * 45 / 68 : (source_y + 32) * 2 / 3;
    for (unsigned bit = 0; bit != 48; ++bit) {
      bool value = (code[bit / 8] >> (7 - bit % 8)) & 1;
      if (mutation == TargetMutation::Replica && replica == 1 && bit == 0) value = !value;
      const unsigned source_x = 240 + bit * 30;
      const unsigned left = source_x * 2 / 3, right = (source_x + 30) * 2 / 3;
      for (unsigned row = top; row != bottom; ++row)
        memset(small.planes[0] + size_t{row} * small.pitch[0] + left, value ? high : low, right - left);
    }
  }
  if (mutation == TargetMutation::Threshold)
    for (unsigned row = 20; row != 23; ++row)
      memset(small.planes[0] + size_t{row} * small.pitch[0] + 169, 128, 3);
  if (mutation == TargetMutation::Spread)
    small.planes[0][size_t{20} * small.pitch[0] + 169] = static_cast<uint8_t>(high - 16);
}
std::unique_ptr<centered_payload::State> TargetState(uint64_t session) {
  Require(setenv("CRYSTALHD_TEST_CENTERED_PAYLOAD_SELECT", "case2-90-91", 1) == 0,
          "set explicit fixed fixture target selector");
  std::unique_ptr<centered_payload::State> state(new centered_payload::State(session));
  Require(unsetenv("CRYSTALHD_TEST_CENTERED_PAYLOAD_SELECT") == 0,
          "restore environment before any target worker observation");
  Require(state->selection == centered_payload::Selection::Case2_90_91,
          "fixed target selection is read once at State initialization");
  return state;
}
void CheckPackedSurface(const uint8_t *bytes, size_t length, const Surface &surface) {
  size_t offset = 0;
  bool equal = length == size_t{surface.width} * surface.height * 3 / 2;
  for (unsigned plane = 0; plane != 2; ++plane)
    for (unsigned row = 0; row != (plane == 0 ? surface.height : surface.height / 2); ++row) {
      equal = (memcmp(bytes + offset, surface.planes[plane] + size_t{row} * surface.pitch[plane],
                      surface.width) == 0) && equal;
      offset += surface.width;
    }
  Require(equal && offset == length, "target observation retains every active source/canvas byte without pitch tails");
}
void CheckTargetRoute(bool direct, unsigned physical_height, unsigned public_height,
                      unsigned session, bool coded_vertical = false) {
  using namespace centered_payload;
  auto state = TargetState(session);
  for (unsigned content : {0U, 1U, 89U, 92U, 179U}) {
    CenteredFixture ignored(10 + content, physical_height, public_height);
    FillTargetSource(&ignored, content);
    Require(ComposeObserved(state.get(), &ignored, direct), "non-target production composition remains functional");
    if (!direct) Require(CopyObserved(state.get(), &ignored), "non-target public copy remains functional");
    state->Materialized(ignored.Fields());
    Require(!state->streams[0].used, "unrelated fixture ordinals cannot reserve target slots or lifetimes");
  }
  for (unsigned content : {90U, 91U}) {
    CenteredFixture fixture(content, physical_height, public_height);
    FillTargetSource(&fixture, content, TargetMutation::None, 16, 235, coded_vertical);
    Surface &canvas = direct ? fixture.public_canvas : fixture.picture;
    Require(state->Select(fixture.picture, canvas, *fixture.small, false, direct) == nullptr,
            "target mode cannot select or inspect content before existing BEGINs");
    Require(ComposeObserved(state.get(), &fixture, direct), "target production composition succeeds under original leases");
    auto &slot = state->streams[0].slots[content - 90];
    Require(slot.selected && slot.compose_committed && slot.binding.token == fixture.picture.expected_timestamp,
            "source90/91 reserves only its corresponding fixed slot");
    if (!direct) Require(CopyObserved(state.get(), &fixture), "target exact decoded-public copy succeeds");
    state->Materialized(fixture.Fields());
    Require(State::Complete(slot), "target still requires successful ENDs and exact public materialization");
    CheckPackedSurface(slot.small_pixels.data(), slot.small_bytes, *fixture.small);
    CheckPackedSurface(slot.composed_pixels.data(), slot.composed_bytes, canvas);
    CheckPackedSurface(slot.public_pixels.data(), slot.public_bytes, fixture.public_canvas);
    Require(slot.composed.height == physical_height && slot.public_canvas.height == public_height &&
            SameGeometry(slot.composed, slot.copy_source), "target mode preserves actual private/public geometry and epoch");
    CheckTails(*fixture.small); CheckTails(canvas); CheckTails(fixture.public_canvas);
  }
  const int index = state->Retire(3, 4, 0);
  Require(index == 0 && state->Complete(state->streams[0]), "two complete fixed targets qualify consuming retirement");
  state->PrintStatus(index); state->Flush(index);
  Require(state->streams[0].manifest_written, "fixed target payload publishes its independent arrays");
  const std::string document = ReadManifest(state.get(), 3, 4);
  Require(document.find("\"selection\":\"case2-90-91\"") != std::string::npos &&
          document.find("\"schema\":\"crystalhd-centered-payload-v1\"") != std::string::npos,
          "only explicit target output carries its selection label");
  RemoveArtifacts(state.get(), 3, 4);
}
void CheckTargetRejectionsAndPosition() {
  using namespace centered_payload;
  auto state = TargetState(310);
  unsigned ordinal = 1;
  for (TargetMutation mutation : {TargetMutation::SourceCase, TargetMutation::Magic,
      TargetMutation::Complement, TargetMutation::Crc, TargetMutation::Replica,
      TargetMutation::Threshold, TargetMutation::Spread}) {
    CenteredFixture fixture(ordinal++, 1088, 1088);
    FillTargetSource(&fixture, 90, mutation);
    Require(ComposeObserved(state.get(), &fixture, true), "bad codeword never changes ordinary composition success");
    Require(!state->streams[0].used, "wrong case, magic, complement, CRC, replica, threshold or spread cannot select a target");
  }
  CenteredFixture ninety_one(20, 1088, 1088);
  FillTargetSource(&ninety_one, 91, TargetMutation::None, 64, 192);
  ninety_one.small->planes[0][size_t{20} * ninety_one.small->pitch[0] + 169] = 200;
  Require(ComposeObserved(state.get(), &ninety_one, true), "threshold and spread boundary target composes normally");
  state->Materialized(ninety_one.Fields());
  auto &stream = state->streams[0];
  Require(!stream.slots[0].selected && State::Complete(stream.slots[1]) &&
          stream.slots[1].binding.token == ninety_one.picture.expected_timestamp,
          "source91 arriving alone uses slot1 and cannot stand in for missing source90");
  CenteredFixture repeat(21, 1088, 1088);
  FillTargetSource(&repeat, 91);
  Require(ComposeObserved(state.get(), &repeat, true), "duplicate target remains an ordinary successful composition");
  state->Materialized(repeat.Fields());
  Require(stream.slots[1].binding.token == ninety_one.picture.expected_timestamp && !stream.slots[0].selected,
          "another binding with source91 cannot replace its fixed slot or fill source90");
  const int index = state->Retire(3, 4, 0);
  state->Flush(index);
  Require(index == 0 && !state->Complete(stream) && !stream.publication_started,
          "one fixed target is incomplete regardless of successful repeated source91");
}
void CheckTargetFailedEnd(bool direct, EndFailure failure, unsigned session) {
  using namespace centered_payload;
  auto state = TargetState(session);
  CenteredFixture first(1, 1088, 1088);
  FillTargetSource(&first, 90);
  const bool compose_failure = failure == EndFailure::ComposeWrite || failure == EndFailure::ComposeRead;
  Require(ComposeObserved(state.get(), &first, direct, failure) == !compose_failure,
          "target observation preserves the original composition END failure");
  auto &failed = state->streams[0].slots[0];
  if (!compose_failure)
    Require(!direct && !CopyObserved(state.get(), &first, failure),
            "target observation preserves the original decoded-public END failure");
  state->Materialized(first.Fields());
  Require(failed.selected && failed.failed && !State::Complete(failed) && !failed.materialized,
          "first selected target remains tentative after either existing END fails");
  CenteredFixture retry(2, 1088, 1088), unrelated(3, 1088, 1088), other(4, 1088, 1088);
  FillTargetSource(&retry, 90); FillTargetSource(&unrelated, 92); FillTargetSource(&other, 91);
  for (CenteredFixture *fixture : {&retry, &unrelated, &other}) {
    Require(ComposeObserved(state.get(), fixture, direct), "later target/unrelated production composition succeeds");
    if (!direct) Require(CopyObserved(state.get(), fixture), "later target/unrelated production copy succeeds");
    state->Materialized(fixture->Fields());
  }
  Require(failed.failed && failed.binding.token == first.picture.expected_timestamp &&
          !State::Complete(failed) && State::Complete(state->streams[0].slots[1]),
          "later correct target or other frames cannot replace the failed fixed ordinal");
  const int index = state->Retire(3, 4, 0); state->Flush(index);
  Require(index == 0 && !state->streams[0].publication_started,
          "one failed fixed target prevents a qualifying manifest");
}
void CheckTargetPublicMismatch(bool epoch) {
  using namespace centered_payload;
  auto state = TargetState(epoch ? 330 : 331);
  CenteredFixture first(1);
  FillTargetSource(&first, 90);
  Require(ComposeObserved(state.get(), &first, false), "public mismatch target composes in private storage");
  const uint64_t original_epoch = first.picture.payload_observer_stamp.epoch;
  if (epoch) first.picture.payload_observer_stamp.Installed();
  else ++first.public_canvas.trace_decode_identity;
  Require(CopyObserved(state.get(), &first), "mismatched target public copy preserves ordinary production behavior");
  auto &slot = state->streams[0].slots[0];
  Require(slot.failed && slot.public_started && !slot.public_committed,
          "target public copy still requires exact source epoch and public binding");
  first.picture.payload_observer_stamp.epoch = original_epoch;
  first.public_canvas.trace_decode_identity = first.public_canvas.decode_identity;
  Require(CopyObserved(state.get(), &first), "restored target public copy succeeds normally");
  state->Materialized(first.Fields());
  CenteredFixture replacement(2);
  FillTargetSource(&replacement, 90);
  Require(ComposeObserved(state.get(), &replacement, false) && CopyObserved(state.get(), &replacement),
          "later target composition and public copy remain functional");
  state->Materialized(replacement.Fields());
  Require(slot.failed && !slot.public_copied && !slot.materialized &&
          slot.binding.token == first.picture.expected_timestamp,
          "restored or later bindings cannot replace the first rejected target public attempt");
}
void CheckTargetBeginLimit(bool fail_read) {
  using namespace centered_payload;
  auto state = TargetState(fail_read ? 340 : 341);
  CenteredFixture fixture(1, 1088, 1088);
  FillTargetSource(&fixture, 90);
  fixture.small->object_fds.push_back(777); fixture.public_canvas.object_fds.push_back(888);
  failing_fd = fail_read ? 777 : 888;
  failing_flags = DMA_BUF_SYNC_START | (fail_read ? DMA_BUF_SYNC_READ : DMA_BUF_SYNC_WRITE);
  const unsigned calls = centered_ioctl_calls;
  bool result, allocation_unused;
  {
    Scope scope(state.get()); allocation_fault::fail_next = true;
    result = ComposeCenteredCanvas(fixture.picture, &fixture.public_canvas, false, true);
    allocation_unused = allocation_fault::fail_next; allocation_fault::fail_next = false;
  }
  failing_fd = -1; fixture.small->object_fds.clear(); fixture.public_canvas.object_fds.clear();
  Require(!result && allocation_unused && centered_ioctl_calls == calls + (fail_read ? 1U : 3U) &&
          !state->streams[0].used,
          "failed original BEGIN prevents recognition; target mode adds no acquisition or premature selection");
  Require(ComposeObserved(state.get(), &fixture, true) && state->streams[0].slots[0].selected,
          "later leased observation is first recognizable target, not evidence about failed-BEGIN pixels");
}
void CheckSelectionInitialization() {
  using namespace centered_payload;
  std::unique_ptr<State> ordinary(new State(350));
  Require(ordinary->selection == Selection::FirstTwo, "unset selector retains ordinary first-two mode");
  Require(setenv("CRYSTALHD_TEST_CENTERED_PAYLOAD_SELECT", "case2-90-91", 1) == 0,
          "change selector environment after default State initialization");
  Populate(ordinary.get(), 3, 4);
  Require(unsetenv("CRYSTALHD_TEST_CENTERED_PAYLOAD_SELECT") == 0,
          "restore selector environment after immutable default-mode control");
  const int index = ordinary->Retire(3, 4, 0); ordinary->Flush(index);
  const std::string document = ReadManifest(ordinary.get(), 3, 4);
  Require(ordinary->streams[0].manifest_written && document.find("\"selection\"") == std::string::npos &&
          ordinary->streams[0].slots[0].binding.token == 1001 &&
          ordinary->streams[0].slots[1].binding.token == 1002,
          "default mode does not reread environment or add target metadata to v1 output");
  RemoveArtifacts(ordinary.get(), 3, 4);
  ordinary.reset();
  for (const char *invalid : {"", "first-two", "case2-90-92", "CASE2-90-91"}) {
    Require(setenv("CRYSTALHD_TEST_CENTERED_PAYLOAD_SELECT", invalid, 1) == 0,
            "set invalid private selection control");
    std::unique_ptr<State> state(new State(351));
    Require(unsetenv("CRYSTALHD_TEST_CENTERED_PAYLOAD_SELECT") == 0,
            "restore environment before invalid-mode worker control");
    CenteredFixture fixture(1, 1088, 1088); FillTargetSource(&fixture, 90);
    Require(ComposeObserved(state.get(), &fixture, true) && state->selection == Selection::Invalid &&
            !state->streams[0].used && state->Retire(3, 4, 0) == -1,
            "invalid explicit selector fails closed without changing ordinary composition behavior");
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
  if (!mkdtemp(directory) || setenv("CRYSTALHD_TEST_CENTERED_PAYLOAD_DIR", directory, 1) != 0 ||
      unsetenv("CRYSTALHD_TEST_CENTERED_PAYLOAD_SELECT") != 0) return 1;
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
    CheckResetStopReopenPublication();
    CheckResetBatchPublication();
    CheckFailedResets();
    CheckTargetRoute(true, 1080, 1080, 300);
    CheckTargetRoute(true, 1088, 1088, 301);
    CheckTargetRoute(false, 1088, 1080, 302);
    CheckTargetRoute(false, 1088, 1088, 303);
    CheckTargetRoute(false, 1088, 1080, 304, true);
    CheckTargetRejectionsAndPosition();
    unsigned failure_session = 320;
    for (bool direct : {false, true})
      for (EndFailure failure : {EndFailure::ComposeWrite, EndFailure::ComposeRead})
        CheckTargetFailedEnd(direct, failure, failure_session++);
    for (EndFailure failure : {EndFailure::PublicWrite, EndFailure::PublicRead})
      CheckTargetFailedEnd(false, failure, failure_session++);
    CheckTargetPublicMismatch(false); CheckTargetPublicMismatch(true);
    CheckTargetBeginLimit(false); CheckTargetBeginLimit(true);
    CheckSelectionInitialization();
    Require(centered_payload::active == nullptr, "no active observer survives CPU controls");
    std::printf("VA centered payload CPU controls: %u checks PASS\n", checks);
  } catch (const std::exception &error) {
    allocation_fault::fail_next = false;
    std::fprintf(stderr, "VA centered payload CPU FAIL: %s\n", error.what());
    result = 1;
  }
  unsetenv("CRYSTALHD_TEST_CENTERED_PAYLOAD_SELECT");
  unsetenv("CRYSTALHD_TEST_CENTERED_PAYLOAD_DIR");
  if (rmdir(directory) != 0) result = 1;
  return result;
}
