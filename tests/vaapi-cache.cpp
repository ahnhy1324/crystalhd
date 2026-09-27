// SPDX-License-Identifier: LGPL-2.1-or-later
// Actual backend cache/fence helpers, with no graphics or decoder library at
// link/run time. Section GC drops unrelated APIs; unused GBM/decoder cleanup
// (also retained by some sanitizer instrumentation) aborts if ever reached.
#include "../filters/vaapi/crystalhd_drv_video.cpp"

#include <stdexcept>
#include <string>

extern "C" void gbm_bo_unmap(gbm_bo *, void *) { abort(); }
extern "C" void gbm_bo_destroy(gbm_bo *) { abort(); }
extern "C" BC_STATUS DtsDeviceClose(HANDLE) { abort(); }
extern "C" BC_STATUS DtsStopDecoder(HANDLE) { abort(); }
extern "C" BC_STATUS DtsCloseDecoder(HANDLE) { abort(); }

namespace {

unsigned int checks = 0;

void Require(bool value, const char *message) {
  ++checks;
  if (!value)
    throw std::runtime_error(message);
}

struct Protocol {
  static constexpr int timeline = 701;
  static constexpr int fence = 702;
  std::vector<std::string> events;
  std::vector<uintptr_t> flushed;
  unsigned int owned = 0;
  unsigned int imports = 0;
  bool timeline_open = false;
  bool fence_open = false;
  int status = 0;
  int fail_import = -1;
  bool fail_signal = false;
  bool require_barrier = true;
};

Protocol *protocol = nullptr;

struct Fixture {
  Protocol mock;
  alignas(128) unsigned char pixels[512] = {};
  Surface surface;
  Fixture() {
    Require(protocol == nullptr, "non-nested protocol fixture");
    protocol = &mock;
    surface.object_fds = {101, 102};
    surface.object_maps = {pixels};
    surface.object_sizes = {sizeof(pixels)};
  }
  ~Fixture() {
    // These are borrowed CPU arrays and fake descriptors, not mapped objects.
    surface.object_fds.clear();
    surface.object_maps.clear();
    surface.object_sizes.clear();
    protocol = nullptr;
  }
};

void FlushLine(const void *address) {
  Require(protocol != nullptr, "flush requires fixture");
  protocol->events.emplace_back("flush");
  protocol->flushed.push_back(reinterpret_cast<uintptr_t>(address));
}

void Barrier() {
  Require(protocol != nullptr, "barrier requires fixture");
  protocol->events.emplace_back("barrier");
}

CpuCacheFlushOps MockOps(size_t line_bytes = 64) {
  return {line_bytes, FlushLine, Barrier};
}

void Capabilities() {
  constexpr unsigned int clflush = 1U << 19;
  constexpr unsigned int sse2 = 1U << 26;
  for (unsigned int bytes : {32U, 64U, 128U}) {
    const unsigned int ebx = (bytes / 8) << 8;
    Require(CpuCacheLineBytes(ebx, clflush | sse2) == bytes,
            "CPUID supplies the flush line size");
    for (unsigned int missing : {0U, clflush, sse2})
      Require(CpuCacheLineBytes(ebx, missing) == 0,
              "CLFLUSH and SSE2 must both be present");
  }
  for (unsigned int units : {0U, 3U, 5U, 255U})
    Require(CpuCacheLineBytes(units << 8, clflush | sse2) == 0,
            "zero or malformed CPUID line size must fail closed");
}

void LineCoverage() {
  for (size_t bytes : {32U, 64U, 128U}) {
    for (size_t offset : {0U, 1U, 31U, 32U, 63U, 64U, 127U}) {
      for (size_t length : {1U, 31U, 32U, 33U, 63U, 64U, 65U,
                            127U, 128U, 129U}) {
        Fixture fixture;
        fixture.surface.object_maps = {fixture.pixels + offset};
        fixture.surface.object_sizes = {length};
        Require(fixture.surface.FlushCpuWrites(MockOps(bytes)),
                "flush a valid mapped extent");
        const uintptr_t begin = reinterpret_cast<uintptr_t>(fixture.pixels + offset);
        const uintptr_t first = begin & ~(bytes - 1);
        const uintptr_t last = (begin + length - 1) & ~(bytes - 1);
        std::vector<uintptr_t> expected;
        for (uintptr_t address = first; address <= last; address += bytes)
          expected.push_back(address);
        Require(fixture.mock.flushed == expected,
                "flush every intersecting line exactly once");
        Require(fixture.mock.events.back() == "barrier" &&
                    std::count(fixture.mock.events.begin(),
                               fixture.mock.events.end(), "barrier") == 1,
                "one barrier follows all lines");
      }
    }
  }
  Fixture fixture;
  fixture.surface.object_maps = {fixture.pixels, nullptr, MAP_FAILED,
                                 fixture.pixels + 128, fixture.pixels + 256};
  fixture.surface.object_sizes = {1, 16, 16, 0, 65};
  Require(fixture.surface.FlushCpuWrites(MockOps()), "multiple mapped objects");
  const uintptr_t base = reinterpret_cast<uintptr_t>(fixture.pixels);
  Require(fixture.mock.flushed == std::vector<uintptr_t>{base, base + 256, base + 320},
          "skip unmapped and empty objects, flush every real object");
  fixture.mock.events.clear();
  fixture.mock.flushed.clear();
  // The mocked line operation records addresses without dereferencing them.
  fixture.surface.object_maps = {reinterpret_cast<void *>(UINTPTR_MAX - 31)};
  fixture.surface.object_sizes = {32};
  Require(fixture.surface.FlushCpuWrites(MockOps()) &&
              fixture.mock.flushed == std::vector<uintptr_t>{UINTPTR_MAX - 63},
          "an extent ending at UINTPTR_MAX must terminate without wrapping");
}

void RejectedBeforeOwnership() {
  const CpuCacheFlushOps missing_line = {64, nullptr, Barrier};
  const CpuCacheFlushOps missing_barrier = {64, FlushLine, nullptr};
  for (const auto &operations : {CpuCacheFlushOps{}, MockOps(0), MockOps(24),
                                 missing_line, missing_barrier}) {
    Fixture fixture;
    Require(BeginFencedWrite(&fixture.surface, operations) < 0,
            "unsupported cache operations reject async writes");
    Require(fixture.mock.events.empty(),
            "rejection precedes CPU START, timeline open and fence import");
    Require(!fixture.surface.FlushCpuWrites(operations) &&
                fixture.mock.events.empty(),
            "unavailable flush never pretends to commit pixels");
    Require(fixture.surface.BeginCpuWrite() && fixture.surface.EndCpuWrite(),
            "synchronous kernel-managed CPU access remains functional");
    Require(fixture.mock.owned == 0 && fixture.mock.imports == 0,
            "synchronous access does not create an asynchronous fence");
  }
}

void CompletionOrder() {
  Fixture fixture;
  const int timeline = BeginFencedWrite(&fixture.surface, MockOps());
  Require(timeline == Protocol::timeline && fixture.mock.status == 0 &&
              fixture.mock.owned == 2 && fixture.mock.imports == 2,
          "all imported objects remain protected while the writer runs");
  Require(EndFencedWrite(&fixture.surface, timeline, MockOps()),
          "supported completion succeeds");
  Require(fixture.mock.status == 1 && fixture.mock.owned == 0 &&
              !fixture.mock.timeline_open && !fixture.mock.fence_open,
          "successful completion consumes all ownership");
  const auto &events = fixture.mock.events;
  const auto signal = std::find(events.begin(), events.end(), "signal");
  Require(signal != events.end() && signal != events.begin() &&
              *(signal - 1) == "barrier" && *(signal + 1) == "end",
          "flush barrier precedes signaling, which precedes CPU END");
}

void FailedFlushAborts() {
  for (bool unsupported : {false, true}) {
    Fixture fixture;
    const int timeline = BeginFencedWrite(&fixture.surface, MockOps());
    Require(timeline == Protocol::timeline, "establish pending fence");
    if (!unsupported) {
      fixture.surface.object_maps = {fixture.pixels,
                                     reinterpret_cast<void *>(UINTPTR_MAX - 31)};
      fixture.surface.object_sizes = {64, 64};
    }
    Require(!EndFencedWrite(&fixture.surface, timeline,
                           unsupported ? CpuCacheFlushOps{} : MockOps()),
            "unsupported or overflowing flush cannot complete successfully");
    Require(fixture.mock.status == -ENOENT && fixture.mock.owned == 0 &&
                fixture.mock.flushed.empty() && !fixture.mock.timeline_open &&
                std::find(fixture.mock.events.begin(), fixture.mock.events.end(),
                          "signal") == fixture.mock.events.end(),
            "failed flush closes the timeline with error before releasing CPU ownership");
  }
  Fixture fixture;
  fixture.surface.object_sizes.clear();
  Require(!fixture.surface.FlushCpuWrites(MockOps()) && fixture.mock.events.empty(),
          "mismatched map metadata must not be indexed");
}

void AbortAndImportFailure() {
  for (int failure : {-1, 101, 102}) {
    Fixture fixture;
    fixture.mock.fail_import = failure;
    const int timeline = BeginFencedWrite(&fixture.surface, MockOps());
    if (failure < 0)
      Require(AbortFencedWrite(&fixture.surface, timeline, MockOps()),
              "cancellation aborts a supported writer");
    else
      Require(timeline < 0, "partial import fails");
    Require(fixture.mock.status == -ENOENT && fixture.mock.owned == 0 &&
                !fixture.mock.timeline_open && !fixture.mock.fence_open &&
                std::find(fixture.mock.events.begin(), fixture.mock.events.end(),
                          "signal") == fixture.mock.events.end(),
            "cancellation or partial import cannot signal successful pixels");
  }
  Fixture fixture;
  const int timeline = BeginFencedWrite(&fixture.surface, MockOps());
  fixture.mock.fail_signal = true;
  Require(!EndFencedWrite(&fixture.surface, timeline, MockOps()) &&
              fixture.mock.status == -ENOENT && fixture.mock.owned == 0,
          "failed signaling still aborts before CPU END");
}

void DefaultCapability() {
  Fixture fixture;
  fixture.mock.require_barrier = false;  // real instructions, not event callbacks
  const bool supported = CpuCacheFlush().Available();
#if !defined(__SSE2__)
  Require(!supported, "strict legacy build compiles out the asynchronous cache path");
#else
  unsigned int eax = 0, ebx = 0, ecx = 0, edx = 0;
  const size_t expected = __get_cpuid(1, &eax, &ebx, &ecx, &edx)
                              ? CpuCacheLineBytes(ebx, edx) : 0;
  Require(CpuCacheFlush().line_bytes == expected,
          "default runtime capability matches the actual CPU");
#endif
  const int timeline = BeginFencedWrite(&fixture.surface);
  if (supported) {
    Require(timeline == Protocol::timeline && EndFencedWrite(&fixture.surface, timeline),
            "default supported CPU executes the real cache flush instructions");
    Require(fixture.mock.status == 1, "real supported cache completion succeeds");
  } else {
    Require(timeline < 0 && fixture.mock.events.empty(),
            "real legacy default rejects before CPU ownership or fence creation");
    Require(fixture.surface.BeginCpuWrite() && fixture.surface.EndCpuWrite(),
            "real legacy default retains synchronous CPU access");
  }
}

}  // namespace

extern "C" int __wrap_open(const char *path, int flags, ...) {
  Require(protocol && strcmp(path, kSwSyncPath) == 0 &&
              flags == (O_RDWR | O_CLOEXEC) && !protocol->timeline_open,
          "only the mocked sw_sync timeline may be opened");
  protocol->events.emplace_back("open");
  protocol->timeline_open = true;
  return Protocol::timeline;
}

extern "C" int __wrap_close(int fd) {
  Require(protocol != nullptr, "close requires fixture");
  if (fd == Protocol::timeline) {
    Require(protocol->timeline_open, "timeline closes once");
    protocol->events.emplace_back("close-timeline");
    protocol->timeline_open = false;
    if (protocol->status == 0)
      protocol->status = -ENOENT;
  } else {
    Require(fd == Protocol::fence && protocol->fence_open, "only owned fence closes");
    protocol->events.emplace_back("close-fence");
    protocol->fence_open = false;
  }
  return 0;
}

extern "C" int __wrap_ioctl(int fd, unsigned long request, ...) {
  Require(protocol != nullptr, "ioctl requires fixture");
  va_list arguments;
  va_start(arguments, request);
  void *argument = va_arg(arguments, void *);
  va_end(arguments);
  if (request == DMA_BUF_IOCTL_SYNC) {
    Require(fd == 101 || fd == 102, "sync only the known fake DMA objects");
    const auto flags = static_cast<dma_buf_sync *>(argument)->flags;
    if (flags == (DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE)) {
      protocol->events.emplace_back("start");
      ++protocol->owned;
    } else {
      Require(flags == (DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE) &&
                  protocol->owned != 0 &&
                  (protocol->imports == 0 || protocol->status != 0),
              "CPU END cannot wait on its own unsignaled fence");
      protocol->events.emplace_back("end");
      --protocol->owned;
    }
  } else if (request == SW_SYNC_IOC_CREATE_FENCE) {
    Require(fd == Protocol::timeline && protocol->timeline_open,
            "fence creation requires an owned timeline");
    auto *create = static_cast<SwSyncCreateFenceData *>(argument);
    Require(create->value == 1, "fence completion point is one");
    create->fence = Protocol::fence;
    protocol->fence_open = true;
    protocol->events.emplace_back("create");
  } else if (request == DMA_BUF_IOCTL_IMPORT_SYNC_FILE) {
    const auto *import = static_cast<dma_buf_import_sync_file *>(argument);
    Require((fd == 101 || fd == 102) && protocol->owned == 2 &&
                protocol->fence_open && import->fd == Protocol::fence &&
                import->flags == DMA_BUF_SYNC_WRITE,
            "import only a valid fence after CPU ownership");
    protocol->events.emplace_back("import");
    if (fd == protocol->fail_import) {
      errno = EIO;
      return -1;
    }
    ++protocol->imports;
  } else if (request == SW_SYNC_IOC_INC) {
    Require(fd == Protocol::timeline && protocol->timeline_open &&
                protocol->status == 0 && *static_cast<uint32_t *>(argument) == 1 &&
                (!protocol->require_barrier ||
                 (!protocol->events.empty() && protocol->events.back() == "barrier")),
            "signaling requires a preceding cache barrier");
    protocol->events.emplace_back("signal");
    if (protocol->fail_signal) {
      errno = EIO;
      return -1;
    }
    protocol->status = 1;
  } else {
    Require(false, "unexpected ioctl: no real device access is permitted");
  }
  return 0;
}

int main() {
  try {
    Capabilities();
    LineCoverage();
    RejectedBeforeOwnership();
    CompletionOrder();
    FailedFlushAborts();
    AbortAndImportFailure();
    DefaultCapability();
  } catch (const std::exception &error) {
    fprintf(stderr, "VA-API cache protocol failed: %s\n", error.what());
    return 1;
  }
  printf("VA-API cache protocol: 7 groups, %u checks PASS (default async cache: %s)\n",
         checks, CpuCacheFlush().Available() ? "enabled" : "unavailable");
  return 0;
}
