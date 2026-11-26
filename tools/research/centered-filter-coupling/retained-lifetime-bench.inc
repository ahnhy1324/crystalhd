// SPDX-License-Identifier: LGPL-2.1-or-later
// Isolated OWN CPU consumer-lifetime scaffold. Root alone builds/runs.
// Reuses the old deferred-source-prefetch benchmark's fixture and scalar
// oracles, but includes only the explicitly selected latest maintained source.
#include <algorithm>
#include <array>
#include <cinttypes>
#include <cstdio>
#include <cstdint>
#include <time.h>
#include <vector>
#ifndef CRYSTALHD_BENCH_SCALING_SOURCE
#error "Set CRYSTALHD_BENCH_SCALING_SOURCE to latest absolute vaapi-scaling.cpp"
#endif
#if !defined(CRYSTALHD_BENCH_DEFERRED) || CRYSTALHD_BENCH_DEFERRED != 1
#error "This complete-lifetime study requires actual deferred Small"
#endif
#define main crystalhd_scaling_fixture_main
#include CRYSTALHD_BENCH_SCALING_SOURCE
#undef main

namespace {
constexpr unsigned kWarmup = 8, kSamples = 100;
enum Stage { kDeriveMap, kClientCopy, kSmallDemand, kAdmission, kFirst, kComplement, kRetire, kStageCount };
const char *const kNames[kStageCount] = {
    "derive_map", "client_copy", "materialize_small", "next_admission_with_fixture_overhead",
    "next_first_field", "next_complement_field", "image_retire"};
struct Span {
  bool executed = false;
  uint64_t wall_begin = 0, wall_end = 0, cpu_begin = 0, cpu_end = 0;
  uint64_t Wall() const { return executed ? wall_end-wall_begin : 0; }
  uint64_t Cpu() const { return executed ? cpu_end-cpu_begin : 0; }
};
struct Sample {
  uint64_t read_token = 0, write_token = 0;
  bool bottom_first = false, small_before = false, small_after = false;
  std::array<Span,kStageCount> stages{};
  uint64_t product_wall = 0, product_cpu = 0, all_wall = 0, all_cpu = 0;
};
uint64_t Clock(clockid_t clock) {
  timespec value{};
  Require(clock_gettime(clock,&value) == 0 && value.tv_sec >= 0 &&
          value.tv_nsec >= 0 && value.tv_nsec < 1000000000,"lifetime clock succeeds");
  return uint64_t(value.tv_sec)*UINT64_C(1000000000)+uint64_t(value.tv_nsec);
}
template<class Function> void Measure(Span &span,Function function) {
  Require(!span.executed,"each selected callback scope executes once");
  span.executed = true;
  span.wall_begin = Clock(CLOCK_MONOTONIC); span.cpu_begin = Clock(CLOCK_THREAD_CPUTIME_ID);
  function();
  span.cpu_end = Clock(CLOCK_THREAD_CPUTIME_ID); span.wall_end = Clock(CLOCK_MONOTONIC);
  Require(span.wall_end >= span.wall_begin && span.cpu_end >= span.cpu_begin,
          "independent lifetime clocks do not regress");
}
std::vector<uint8_t> SmallOracle(const CenteredFixture &f,const std::vector<uint8_t> &packed) {
  const unsigned w = f.capture_width,h = f.capture_height;
  std::vector<uint8_t> result(size_t{w}*h*3U/2U);
  for (unsigned y = 0;y < h;++y) for (unsigned x = 0;x < w;++x)
    result[size_t{y}*w+x] = packed[(size_t{y}*w+x)*2U];
  for (unsigned y = 0;y < h/2U;++y) {
    const unsigned first = (y/2U)*4U+y%2U;
    for (unsigned x = 0;x < w;x += 2U) for (unsigned component = 0;component < 2;++component) {
      const size_t a = (size_t{first}*w+x)*2U+1U+component*2U;
      result[size_t{w}*h+size_t{y}*w+x+component] =
          static_cast<uint8_t>((unsigned(packed[a])+packed[a+size_t{w}*4U]+1U)/2U);
    }
  }
  return result;
}
void CheckPlaneBytes(const Surface &surface,const std::vector<uint8_t> &expected,
                     unsigned width,unsigned height) {
  Require(surface.width == width && surface.height == height && surface.fourcc == VA_FOURCC_NV12 &&
          expected.size() == size_t{width}*height*3U/2U,"whole NV12 oracle geometry agrees");
  for (unsigned plane = 0;plane < 2;++plane) {
    const unsigned rows = plane ? height/2U : height;
    const size_t base = plane ? size_t{width}*height : 0;
    for (unsigned y = 0;y < rows;++y)
      Require(surface.planes[plane] && surface.pitch[plane] >= width &&
              memcmp(surface.planes[plane]+size_t{y}*surface.pitch[plane],
                     expected.data()+base+size_t{y}*width,width) == 0,
              "every active public/Small Y and UV byte matches independent scalar oracle");
  }
}
struct Prepared {
  uint64_t token = 0;
  bool bottom_first = false;
  std::vector<uint8_t> public_oracle,small_oracle;
  std::array<std::vector<uint8_t>,2> fields;
  std::array<BC_DTS_PROC_OUT,2> output{};
};
Prepared Prepare(const CenteredFixture &f,unsigned ordinal) {
  Prepared result; result.token = uint64_t(ordinal+1U)*kTimestampStep;
  result.bottom_first = (ordinal&1U) != 0;
  const auto packed = f.Packed(17U+ordinal*31U);
  result.public_oracle = f.Canvas(packed); result.small_oracle = SmallOracle(f,packed);
  for (unsigned parity = 0;parity < 2;++parity) {
    result.fields[parity].resize(packed.size()/2U);
    for (unsigned y = 0;y < f.capture_height/2U;++y)
      memcpy(result.fields[parity].data()+size_t{y}*f.capture_width*2U,
             packed.data()+size_t{y*2U+parity}*f.capture_width*2U,size_t{f.capture_width}*2U);
    auto &out = result.output[parity];
    out.Ybuff = result.fields[parity].data(); out.YBuffDoneSz = result.fields[parity].size()/4U;
    out.PoutFlags = BC_POUT_FLAGS_PIB_VALID; out.b422Mode = OUTPUT_MODE422_YUY2;
    out.PicInfo.timeStamp = result.token; out.PicInfo.width = f.capture_width;
    out.PicInfo.height = f.capture_height; out.PicInfo.picture_number = result.token/kTimestampStep;
    out.PicInfo.flags = VDEC_FLAG_INTERLACED_SRC |
        (parity ? VDEC_FLAG_BOTTOMFIELD : VDEC_FLAG_TOPFIELD) |
        (result.bottom_first ? VDEC_FLAG_BOTTOM_FIRST : 0);
  }
  return result;
}
void Produce(CenteredFixture &f,const std::shared_ptr<Surface> &surface,Prepared &input,
             Sample *sample) {
  // Pixel arrays/scalar references are prepared before these scopes. Submit
  // includes real admission PLUS fixture validation and is reported separately.
  if (sample)
    Measure(sample->stages[kAdmission],[&] { f.Submit(1,input.token == kTimestampStep || (coupled_periodic_idr && (input.token/kTimestampStep-1U)%30U == 0)); });
  else
    f.Submit(1,input.token == kTimestampStep || (coupled_periodic_idr && (input.token/kTimestampStep-1U)%30U == 0));
  Require(surface->expected_timestamp == input.token,"actual Submit token increases exactly");
  const std::weak_ptr<Surface> picture_binding = f.decode->decoded_frames.at(input.token);
  auto small = f.decode->decoded_frames.at(input.token)->centered_pixels;
  const auto geometry = f.decode->decoded_frames.at(input.token)->centered_geometry;
  Require(small && CenteredReadbackCpuPixels(*small) && CpuPrimaryStorageMatches(*surface),
          "actual map-only admission supplies Small and canonical full CPU canvas");
  const auto untouched = small->storage;
  const uint64_t before_copy = centered_probe::roi_copy_bytes;
  VAStatus first = VA_STATUS_ERROR_OPERATION_FAILED,second = VA_STATUS_ERROR_OPERATION_FAILED;
  Span untimed_first,untimed_second;
  auto &first_span = sample ? sample->stages[kFirst] : untimed_first;
  auto &second_span = sample ? sample->stages[kComplement] : untimed_second;
  Measure(first_span,[&] { first = ProcessDecodedOutput(&f.driver,f.decode.get(),input.output[input.bottom_first ? 1 : 0]); });
  const auto &held = f.decode->h264_first_field;
  Require(first == VA_STATUS_SUCCESS && held.valid && held.nv12_small.lock() == small &&
          held.nv12_deferred && !held.nv12_staged && !small->ready && !surface->ready &&
          small->storage == untouched && small->frame_timestamp == 0 && surface->frame_timestamp == 0 &&
          DeferredCenteredOwnFields(*surface,*small,geometry,input.bottom_first ? 2U : 1U),
          "first parity is genuinely deferred and cannot publish a complete picture");
  Measure(second_span,[&] { second = ProcessDecodedOutput(&f.driver,f.decode.get(),input.output[input.bottom_first ? 0 : 1]); });
  {
    const auto picture = picture_binding.lock(); // Only after BOTH actual field calls.
    Require(second == VA_STATUS_SUCCESS && picture && picture == f.decode->decoded_frames.at(input.token) &&
            picture->centered_pixels == small && picture->ready && surface->ready &&
            !small->failed && !surface->failed && surface->frame_timestamp == input.token &&
            !f.decode->h264_first_field.valid && centered_probe::roi_copy_bytes == before_copy,
            "completed pair publishes exact full pixels without final ROI copies");
  }
  Require(!small->ready && small->frame_timestamp == 0 && small->storage == untouched &&
          small->centered_deferred.fields == 3U && small->centered_deferred.small_fields == 0,
          "public completion truthfully leaves optional Small stores deferred");
  if (coupled_full_pixel_checks) CheckPlaneBytes(*surface,input.public_oracle,1920,1088);
  // All test strong Small/picture refs die before the next invocation's Submit.
}
void CheckImageLayout(const VAImage &image,const void *mapped,const std::vector<uint8_t> &expected) {
  Require(mapped && image.format.fourcc == VA_FOURCC_NV12 && image.width == 1920 && image.height == 1088 &&
          expected.size() == 3133440U,"mapped image uses the complete coded public canvas");
  for (unsigned plane = 0;plane < 2;++plane) {
    const unsigned rows = plane ? image.height/2U : image.height;
    for (unsigned y = 0;y < rows;++y)
      Require(image.pitches[plane] >= image.width &&
              size_t{image.offsets[plane]}+size_t{y}*image.pitches[plane]+image.width <= image.data_size,
              "public image rows remain inside the actual mapped extent");
  }
}
void CheckImage(const VAImage &image,const void *mapped,const std::vector<uint8_t> &expected) {
  CheckImageLayout(image,mapped,expected);
  for (unsigned plane = 0;plane < 2;++plane) {
    const unsigned rows = plane ? image.height/2U : image.height;
    const size_t base = plane ? size_t{image.width}*image.height : 0;
    for (unsigned y = 0;y < rows;++y)
      Require(memcmp(static_cast<const uint8_t *>(mapped)+image.offsets[plane]+size_t{y}*image.pitches[plane],
                     expected.data()+base+size_t{y}*image.width,image.width) == 0,
              "all retained snapshot Y/UV bytes remain the original image");
  }
}
void SumScopes(Sample &sample) {
  std::vector<const Span *> ordered;
  for (unsigned stage = 0;stage < kStageCount;++stage) if (sample.stages[stage].executed) {
    const auto &span = sample.stages[stage]; ordered.push_back(&span);
    sample.all_wall += span.Wall(); sample.all_cpu += span.Cpu();
    if (stage != kClientCopy && stage != kAdmission) {
      sample.product_wall += span.Wall(); sample.product_cpu += span.Cpu();
    }
  }
  std::sort(ordered.begin(),ordered.end(),[](const Span *a,const Span *b) { return a->wall_begin < b->wall_begin; });
  for (size_t i = 1;i < ordered.size();++i)
    Require(ordered[i-1]->wall_end <= ordered[i]->wall_begin && ordered[i-1]->cpu_end <= ordered[i]->cpu_begin,
            "selected same-thread scopes do not overlap or double count");
}
Sample Cycle(CenteredFixture &f,const std::shared_ptr<Surface> &surface,const Prepared &current,
             Prepared &next,std::vector<uint8_t> &client,bool retain,bool public_only) {
  Sample sample; sample.read_token = current.token; sample.write_token = next.token;
  sample.bottom_first = next.bottom_first;
  VAImage image{}; void *mapped = nullptr;
  VAStatus derived = VA_STATUS_ERROR_OPERATION_FAILED,mapped_status = VA_STATUS_ERROR_OPERATION_FAILED;
  Measure(sample.stages[kDeriveMap],[&] {
    derived = DeriveImage(&f.context,1,&image);
    if (derived == VA_STATUS_SUCCESS) mapped_status = MapBuffer(&f.context,image.buf,&mapped);
  });
  Require(derived == VA_STATUS_SUCCESS && mapped_status == VA_STATUS_SUCCESS && mapped &&
          f.driver.buffers.at(image.buf).cow_snapshot && f.driver.buffers.at(image.buf).data.empty(),
          "public callback creates/maps a real retained COW image, not an eager replacement");
  // No mapped pixel is touched before the timed first client read: mapping
  // faults must not be hidden in an untimed preliminary oracle comparison.
  CheckImageLayout(image,mapped,current.public_oracle);
  Require(client.size() == current.public_oracle.size(),"preallocated client storage covers all active coded NV12");
  Measure(sample.stages[kClientCopy],[&] {
    for (unsigned plane = 0;plane < 2;++plane) {
      const unsigned rows = plane ? image.height/2U : image.height;
      const size_t base = plane ? size_t{image.width}*image.height : 0;
      for (unsigned y = 0;y < rows;++y)
        memcpy(client.data()+base+size_t{y}*image.width,
               static_cast<const uint8_t *>(mapped)+image.offsets[plane]+size_t{y}*image.pitches[plane],image.width);
    }
  });
  Require(client == current.public_oracle,"client's every copied Y/UV byte matches the original public image");
  {
    auto small = f.decode->decoded_frames.at(current.token)->centered_pixels;
    Require(small && !small->ready && small->centered_deferred.fields == 3U,
            "public Derive/Map/copy does not force an unnecessary private Small");
    sample.small_before = small->ready;
    if (!public_only) {
      bool materialized = false;
      Measure(sample.stages[kSmallDemand],[&] { materialized = MaterializeDeferredCenteredSmall(small.get()); });
      Require(materialized && small->ready && small->frame_timestamp == current.token,
              "explicit private consumer pays actual full Small materialization");
      CheckPlaneBytes(*small,current.small_oracle,1280,720);
    }
    sample.small_after = small->ready;
    Require(sample.small_after == !public_only,"only the explicitly selected consumer requests Small");
  } // No extra old Small or picture test owner survives the next Submit.
  auto retire = [&] {
    VAStatus unmapped = VA_STATUS_ERROR_OPERATION_FAILED,destroyed = VA_STATUS_ERROR_OPERATION_FAILED;
    Measure(sample.stages[kRetire],[&] {
      unmapped = UnmapBuffer(&f.context,image.buf);
      if (unmapped == VA_STATUS_SUCCESS) destroyed = DestroyImage(&f.context,image.image_id);
    });
    Require(unmapped == VA_STATUS_SUCCESS && destroyed == VA_STATUS_SUCCESS,
            "ordinary public Unmap/Destroy retires this image completely");
  };
  if (!retain) retire();
  const auto *const backing = surface->cpu_primary_canvas.get();
  const uint64_t epoch = surface->cpu_primary_canvas->WriteEpoch();
  Produce(f,surface,next,&sample);
  Require(surface->cpu_primary_canvas.get() == backing && surface->cpu_primary_canvas->WriteEpoch() != epoch,
          "next actual field processing writes the same canonical canvas");
  if (retain) { CheckImage(image,mapped,current.public_oracle); retire(); }
  Require(client == current.public_oracle,"the independent preallocated client copy survives the next original");
  SumScopes(sample);
  return sample;
}
void Metric(const char *name,std::vector<uint64_t> values) {
  std::sort(values.begin(),values.end()); uint64_t sum = 0;
  for (uint64_t value : values) sum += value;
  std::printf("\"%s\":{\"count\":%zu,\"sum_ns\":%" PRIu64 ",\"p50_ns\":%" PRIu64 ",\"p95_ns\":%" PRIu64 ",\"max_ns\":%" PRIu64 "}",name,values.size(),sum,
              values[(values.size()-1U)/2U],values[(values.size()*95U+99U)/100U-1U],values.back());
}
}
int retained_lifetime_benchmark_main(int argc,char **argv) {
  try {
    bool retain = false,public_only = false;
    for (int i = 1;i < argc;++i) {
      if (std::strcmp(argv[i],"--retain-image") == 0) { Require(!retain,"one retention flag"); retain = true; }
      else if (std::strcmp(argv[i],"--public-only") == 0) { Require(!public_only,"one public-only flag"); public_only = true; }
      else Require(false,"only --retain-image and --public-only are accepted");
    }
    RequireCowAvailable();
    const uint32_t baseline = crystalhd_cow_snapshot::LiveBytes();
    std::array<Sample,kSamples> samples{};
    {
      CenteredDumbScope io;
      CenteredFixture f(1920,1088,1080,1280,720,1088,false,true);
      const auto surface = InstallCenteredDumb(&f,&io.state); RecreateCenteredContext(&f);
      Require(CpuPrimaryStorageMatches(*surface),"latest actual context selects canonical memfd CPU canvas");
      std::vector<uint8_t> client(3133440U);
      auto current = Prepare(f,0); Produce(f,surface,current,nullptr);
      for (unsigned i = 0;i < kWarmup+kSamples;++i) {
        auto next = Prepare(f,i+1U);
        const auto sample = Cycle(f,surface,current,next,client,retain,public_only);
        if (i >= kWarmup) samples[i-kWarmup] = sample;
        current = std::move(next);
      }
      Require(f.driver.images.empty(),"all measured and warmup image lifetimes fully retire");
    }
    Require(crystalhd_cow_snapshot::LiveBytes() == baseline,"final COW primary charges return exactly to baseline");
    std::printf("{\"schema\":\"crystalhd-centered-consumer-lifetime-v1\",\"status\":\"PASS_SCOPED_OWN_CPU_CONSUMER_LIFETIMES\",\"fixture_source\":\"%s\",\"deferred_small_enabled\":true,\"retain_image_across_next_write\":%s,\"small_demand_requested\":%s,\"public_geometry\":[1920,1088],\"visible_geometry\":[1920,1080],\"small_geometry\":[1280,720],\"client_copy_bytes_per_sample\":3133440,\"small_bytes_if_requested\":1382400,\"bootstrap_pairs\":1,\"warmup_samples\":%u,\"measured_samples\":%u,\"producer_pairs_total\":%u,\"completed_consumer_lifetimes_total\":%u,\"sample_alignment\":\"READ_CURRENT_TOKEN_AND_PRODUCE_NEXT_TOKEN; NOT_SAME_PICTURE_CONVERSION_TO_READ_LATENCY\",\"pixel_compaction_and_scalar_oracles_in_timing\":false,\"next_admission_in_timing\":true,\"next_admission_scope\":\"Submit_PLUS_FIXTURE_VALIDATION_NOT_PURE_PRODUCT_CPU\",\"final_unconsumed_producer_cleanup_in_timing\":false,\"no_extra_old_small_or_picture_owner_before_submit\":true,\"final_cow_live_bytes\":%u,\"baseline_cow_live_bytes\":%u,\"summary\":{",
                CRYSTALHD_BENCH_SCALING_SOURCE,retain?"true":"false",public_only?"false":"true",
                kWarmup,kSamples,kWarmup+kSamples+1U,kWarmup+kSamples,
                crystalhd_cow_snapshot::LiveBytes(),baseline);
    std::vector<uint64_t> values; values.reserve(kSamples);
    for (unsigned kind = 0;kind < 4;++kind) {
      values.clear(); for (const auto &sample : samples)
        values.push_back(kind == 0 ? sample.product_wall : kind == 1 ? sample.product_cpu :
                         kind == 2 ? sample.all_wall : sample.all_cpu);
      if (kind) std::putchar(',');
      Metric(kind == 0 ? "product_callback_wall_union" : kind == 1 ? "product_callback_thread_cpu_union" :
             kind == 2 ? "all_selected_wall_union" : "all_selected_thread_cpu_union",values);
    }
    std::printf("},\"stage_metrics\":{");
    for (unsigned stage = 0;stage < kStageCount;++stage) {
      if (stage) std::putchar(',');
      std::printf("\"%s\":{\"executed\":%s,",kNames[stage],
          stage == kSmallDemand && public_only ? "false" : "true");
      values.clear(); for (const auto &sample : samples) values.push_back(sample.stages[stage].Wall());
      Metric("wall",values); std::putchar(',');
      values.clear(); for (const auto &sample : samples) values.push_back(sample.stages[stage].Cpu());
      Metric("thread_cpu",values); std::putchar('}');
    }
    std::printf("},\"samples\":[");
    for (unsigned i = 0;i < kSamples;++i) {
      if (i) std::putchar(',');
      const auto &sample = samples[i];
      std::printf("{\"sample\":%u,\"read_token\":%" PRIu64 ",\"write_token\":%" PRIu64 ",\"next_bottom_first\":%s,\"small_ready_before_demand\":%s,\"small_ready_after_demand\":%s,\"product_wall_union_ns\":%" PRIu64 ",\"product_thread_cpu_union_ns\":%" PRIu64 ",\"all_wall_union_ns\":%" PRIu64 ",\"all_thread_cpu_union_ns\":%" PRIu64 ",\"stages\":{",
                  i,sample.read_token,sample.write_token,sample.bottom_first?"true":"false",
                  sample.small_before?"true":"false",sample.small_after?"true":"false",
                  sample.product_wall,sample.product_cpu,sample.all_wall,sample.all_cpu);
      for (unsigned stage = 0;stage < kStageCount;++stage) {
        if (stage) std::putchar(',');
        const auto &span = sample.stages[stage];
        std::printf("\"%s\":{\"executed\":%s,\"wall_begin_ns\":%" PRIu64 ",\"wall_end_ns\":%" PRIu64 ",\"thread_cpu_begin_ns\":%" PRIu64 ",\"thread_cpu_end_ns\":%" PRIu64 "}",
                    kNames[stage],span.executed?"true":"false",span.wall_begin,span.wall_end,span.cpu_begin,span.cpu_end);
      }
      std::printf("}}");
    }
    std::puts("],\"limits\":\"Selected synchronous OWN host CPU consumer lifetimes only; no DIL receive/release, hardware decode, DMA coherence, physical graphics, natural field gap, filter processing, export/promotion/allocation-wide claim, stock/source900 or release qualification. Public coded1088 copy differs from prior visible1080 filter pattern. Forced Small demand is an explicit optional diagnostic consumer, not ordinary BWDIF. Independent pixel oracles/compaction excluded; Submit plus fixture validation is a separate observed scope excluded from pure product union but included in all-selected union. Bootstrap/final unused producer are explicit excluded boundaries.\"}");
    return 0;
  } catch (const CowUnavailable &) {
    std::puts("{\"status\":\"NOT_EXERCISED_COW_UNAVAILABLE\"}"); return 77;
  } catch (const std::exception &error) {
    std::fprintf(stderr,"centered-consumer-lifetime-bench FAIL: %s\n",error.what()); return 1;
  }
}
