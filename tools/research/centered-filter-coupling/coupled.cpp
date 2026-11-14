// SPDX-License-Identifier: LGPL-2.1-or-later
// Isolated OWN CPU study; unchanged original App filter loaded through C bridge.
// Long synthetic streams submit actual fixture IDRs through normal admission.
static bool coupled_periodic_idr=true;
// OWN observer condition only; default full-byte correctness coverage remains.
static bool coupled_full_pixel_checks=true;
#include "retained-lifetime-bench.inc"
#include "filter-bridge.h"
#include <cerrno>

namespace coupled {
constexpr unsigned Inputs=900;
constexpr size_t VisibleBytes=1920U*1080U*3U/2U;
enum Extra { Derive,Allocate,Copy,Retire,Add,Get0,Get1,Get2,Verify,Count };
const char *const Names[Count]={"derive_map","client_allocate","client_visible_copy",
    "image_retire","filter_add_wrapper","filter_get0_wrapper","filter_get1_wrapper",
    "filter_get2_wrapper","pixel_oracles"};
struct Field { int64_t pts=0,best=0; uint64_t ready=0; unsigned reference=0; };
struct Row {
  unsigned index=0,overlap=0,fields=0;uint64_t begin=0,end=0,due=0,token=0;
  uint64_t cpu_begin=0,cpu_end=0,epoch_before=0,epoch_after=0;
  Sample owned;std::array<Span,Count> spans{};std::array<Field,2> output{};
};
unsigned active_index=0;
int64_t Pts(unsigned i){return 66734+int64_t((uint64_t(i)*1001000+15)/30);}
uint64_t Due(uint64_t epoch,unsigned i){return epoch+uint64_t(i)*1001000000/30;}
void Wait(uint64_t due){timespec ts{time_t(due/1000000000),long(due%1000000000)};int r;
  do{r=clock_nanosleep(CLOCK_MONOTONIC,TIMER_ABSTIME,&ts,nullptr);}while(r==EINTR);
  Require(r==0,"absolute indexed pacing succeeds without rebasing late inputs");}
void CopyVisible(const uint8_t *y,const uint8_t *uv,unsigned yp,unsigned uvp,CoupledFrame &frame){
  Require(frame.width==1920 && frame.height==1080 && y && uv && yp>=1920 && uvp>=1920,
          "actual public and owned filter geometry agree");
  for(unsigned p=0;p<2;++p)for(unsigned row=0;row<(p?540U:1080U);++row)
    memcpy(frame.planes[p]+size_t(row)*frame.pitches[p],(p?uv:y)+size_t(row)*(p?uvp:yp),1920);
}
std::vector<uint8_t> Tight(const CoupledFrame &frame){
  std::vector<uint8_t> bytes(VisibleBytes);
  for(unsigned p=0;p<2;++p)for(unsigned row=0;row<(p?540U:1080U);++row)
    memcpy(bytes.data()+(p?1920U*1080U:0)+size_t(row)*1920,
           frame.planes[p]+size_t(row)*frame.pitches[p],1920);
  return bytes;
}
void Equal(const CoupledFrame &frame,const std::vector<uint8_t> &expected){
  Require(expected.size()==VisibleBytes,"complete active reference available");
  for(unsigned p=0;p<2;++p)for(unsigned row=0;row<(p?540U:1080U);++row)
    Require(memcmp(frame.planes[p]+size_t(row)*frame.pitches[p],
                   expected.data()+(p?1920U*1080U:0)+size_t(row)*1920,1920)==0,
            "all actual filter/input Y and UV bytes match independent scalar-input reference");
}
unsigned Reference(unsigned input,unsigned field){
  Require(input>0 && field<2,"output belongs to previous lookahead input");
  if(input==1)return field;
  const unsigned source=(input-1)%3;
  return (source==0?6U:source==1?2U:4U)+field;
}
std::array<std::vector<uint8_t>,8> ReferencePixels(const std::array<Prepared,3> &patterns){
  std::array<std::vector<uint8_t>,8> expected;
  unsigned outputs=0;
  for(unsigned i=0;i<8;++i){
    const auto &pattern=patterns[i%3];
    auto in=coupled_input(i%3,Pts(i),pattern.bottom_first);
    CopyVisible(pattern.public_oracle.data(),pattern.public_oracle.data()+1920U*1088U,1920,1920,in);
    Require(coupled_add(&in)==0,"independent scalar-input graph accepts whole picture");
    unsigned fields=0;
    for(unsigned g=0;g<3;++g){CoupledFrame out{};int ret=coupled_get(&out);
      if(ret==-EAGAIN)break;
      Require(ret==0 && i>0 && fields<2,"bounded independent graph output");
      const unsigned ref=Reference(i,fields);
      if(expected[ref].empty())expected[ref]=Tight(out);else Equal(out,expected[ref]);
      ++fields;++outputs;coupled_release();
    }
    Require(fields==(i?2U:0U),"reference graph has exactly one-input lookahead");
  }
  Require(outputs==14,"independent periodic reference emits fourteen full outputs");
  for(const auto &pixels:expected)Require(pixels.size()==VisibleBytes,"all startup/cyclic references present");
  coupled_reset();return expected;
}
void PrintSpan(const Span &s){
  printf("{\"executed\":%s,\"wall_begin_ns\":%" PRIu64 ",\"wall_end_ns\":%" PRIu64
         ",\"thread_cpu_begin_ns\":%" PRIu64 ",\"thread_cpu_end_ns\":%" PRIu64 "}",
         s.executed?"true":"false",s.wall_begin,s.wall_end,s.cpu_begin,s.cpu_end);
}
int HistoryControl(){
  RequireCowAvailable();
  const uint32_t baseline=crystalhd_cow_snapshot::LiveBytes();
  unsigned rejected=0,accepted=0,prunes=0;size_t maximum=0;
  for(bool periodic:{false,true}){
    coupled_periodic_idr=periodic;
    CenteredDumbScope io;CenteredFixture f(1920,1088,1080,1280,720,1088,false,true);
    const auto surface=InstallCenteredDumb(&f,&io.state);RecreateCenteredContext(&f);
    std::array<Prepared,3> patterns{Prepare(f,0),Prepare(f,1),Prepare(f,2)};
    size_t previous=0;
    for(unsigned i=0;i<(periodic?540U:513U);++i){
      auto &in=patterns[i%3];in.token=uint64_t(i+1)*kTimestampStep;
      for(auto &out:in.output){out.PicInfo.timeStamp=in.token;out.PicInfo.picture_number=i+1;}
      bool failed=false;
      try{Produce(f,surface,in,nullptr);}catch(const std::exception &e){
        Require(!periodic && i==512 && !strcmp(e.what(),"real EndPicture/SubmitPicture") &&
                f.decode->replay.cached_pictures()==512 && f.mock.inputs.size()==512,
                "single-IDR negative control fails only at original512-picture history bound");
        failed=true;++rejected;
      }
      if(failed)break;
      const size_t current=f.decode->replay.cached_pictures();
      if(periodic){++accepted;maximum=std::max(maximum,current);if(current<previous)++prunes;}
      previous=current;
    }
  }
  Require(rejected==1 && accepted==540 && maximum<=31 && prunes>=17 &&
          crystalhd_cow_snapshot::LiveBytes()==baseline,"periodic actual IDRs reclaim completed history without bypass");
  printf("{\"status\":\"PASS_SCAFFOLD_IDR_HISTORY_CAUSE_CONTROL\",\"single_idr_rejected_ordinal\":512,"
         "\"single_idr_admitted\":512,\"periodic_idr_interval\":30,\"periodic_admitted\":%u,"
         "\"maximum_cached_pictures\":%zu,\"observed_history_prunes\":%u,\"final_cow_live_bytes\":%u}\n",
         accepted,maximum,prunes,crystalhd_cow_snapshot::LiveBytes());
  return 0;
}
}
int main(int argc,char **argv){
  try{
    if(argc==2 && !strcmp(argv[1],"--history-admission-control"))return coupled::HistoryControl();
    bool hold=false,control=false,timing=false;std::vector<char *> arguments{argv[0]};
    for(int i=1;i<argc;++i){
      if(!strcmp(argv[i],"--hold-image")){Require(!hold,"one image-hold option");hold=true;}
      else if(!strcmp(argv[i],"--bounded-control")){Require(!control,"one bounded-control option");control=true;}
      else if(!strcmp(argv[i],"--timing-observer-condition")){Require(!timing,"one timing observer condition");timing=true;}
      else arguments.push_back(argv[i]);
    }
    Require(control==hold,"only fixed900 ordinary or fixed8 held-image control");
    Require(!timing || !control,"timing condition is fixed900 ordinary lifetime only");
    arguments.push_back(nullptr);
    Require(coupled_original_oracle(int(arguments.size()-1),arguments.data())==0,
            "unchanged original captured3-to4 oracle passes first");
    RequireCowAvailable();const uint32_t baseline=crystalhd_cow_snapshot::LiveBytes();
    const unsigned count=control?8:coupled::Inputs;
    std::vector<coupled::Row> rows(count);unsigned outputs=0,overlaps=0;
    uint64_t epoch=0,trial_begin=0,trial_end=0,process_begin=0,process_end=0;
    {
      CenteredDumbScope io;CenteredFixture f(1920,1088,1080,1280,720,1088,false,true);
      const auto surface=InstallCenteredDumb(&f,&io.state);RecreateCenteredContext(&f);
      Require(CpuPrimaryStorageMatches(*surface),"current maintained callbacks select canonical CPU canvas");
      std::array<Prepared,3> patterns{Prepare(f,0),Prepare(f,1),Prepare(f,2)};
      const auto references=coupled::ReferencePixels(patterns);
      // Captured3->4 and scalar-input14-output qualifications above ALWAYS
      // compare full bytes. Only repeated checks inside this paced loop vary.
      coupled_full_pixel_checks=!timing;
      const auto *backing=surface->cpu_primary_canvas.get();
      VAImage previous{};void *previous_map=nullptr;unsigned previous_pattern=0;
      auto retire=[&]{Require(previous_map && UnmapBuffer(&f.context,previous.buf)==VA_STATUS_SUCCESS &&
              DestroyImage(&f.context,previous.image_id)==VA_STATUS_SUCCESS,"actual mapped image retires");
        previous_map=nullptr;};
      trial_begin=Clock(CLOCK_MONOTONIC);process_begin=Clock(CLOCK_PROCESS_CPUTIME_ID);
      epoch=trial_begin+50000000;
      for(unsigned i=0;i<count;++i){
        coupled::active_index=i;
        auto &row=rows[i];row.index=i;row.due=coupled::Due(epoch,i);coupled::Wait(row.due);
        row.begin=Clock(CLOCK_MONOTONIC);row.cpu_begin=Clock(CLOCK_THREAD_CPUTIME_ID);
        auto &input=patterns[i%3];input.token=uint64_t(i+1)*kTimestampStep;row.token=input.token;
        for(auto &out:input.output){out.PicInfo.timeStamp=input.token;out.PicInfo.picture_number=i+1;}
        row.overlap=previous_map?1:0;overlaps+=row.overlap;
        Require(f.driver.images.size()==row.overlap,"only measured actual VA images survive publication");
        row.epoch_before=surface->cpu_primary_canvas->WriteEpoch();
        Produce(f,surface,input,&row.owned);
        row.epoch_after=surface->cpu_primary_canvas->WriteEpoch();
        Require(surface->cpu_primary_canvas.get()==backing && row.epoch_after!=row.epoch_before,
                "every actual producer writes the same canonical backing and advances its epoch");
        if(previous_map){
          CheckImage(previous,previous_map,patterns[previous_pattern].public_oracle);
          Measure(row.spans[coupled::Retire],retire);
        }
        VAStatus derived=VA_STATUS_ERROR_OPERATION_FAILED,mapped=VA_STATUS_ERROR_OPERATION_FAILED;
        Measure(row.spans[coupled::Derive],[&]{derived=DeriveImage(&f.context,1,&previous);
          if(derived==VA_STATUS_SUCCESS)mapped=MapBuffer(&f.context,previous.buf,&previous_map);});
        Require(derived==VA_STATUS_SUCCESS && mapped==VA_STATUS_SUCCESS && previous_map &&
                f.driver.buffers.at(previous.buf).cow_snapshot,"actual current public COW image is mapped");
        previous_pattern=i%3;CheckImageLayout(previous,previous_map,input.public_oracle);
        CoupledFrame filter_in{};
        Measure(row.spans[coupled::Allocate],[&]{filter_in=coupled_input(i%3,coupled::Pts(i),input.bottom_first);});
        Require(filter_in.planes[0]!=previous_map,"filter owns independent client storage");
        Measure(row.spans[coupled::Copy],[&]{coupled::CopyVisible(
          static_cast<const uint8_t *>(previous_map)+previous.offsets[0],
          static_cast<const uint8_t *>(previous_map)+previous.offsets[1],
          previous.pitches[0],previous.pitches[1],filter_in);});
        // First mapped-byte touch occurs above, in the actual timed client copy.
        Span input_oracle;
        if(!timing)Measure(input_oracle,[&]{
          for(unsigned p=0;p<2;++p)for(unsigned y=0;y<(p?540U:1080U);++y)
            Require(memcmp(filter_in.planes[p]+size_t(y)*filter_in.pitches[p],
              input.public_oracle.data()+(p?1920U*1088U:0)+size_t(y)*1920,1920)==0,
              "every client byte matches independent full-canvas conversion oracle");
        });
        row.spans[coupled::Verify]=input_oracle;
        if(!hold)Measure(row.spans[coupled::Retire],retire);
        {
          const auto small=f.decode->decoded_frames.at(input.token)->centered_pixels;
          Require(small && !small->ready && small->centered_deferred.fields==3U,
                  "ordinary public BWDIF consumer does not request optional deferred Small");
        }
        int added=-1;Measure(row.spans[coupled::Add],[&]{added=coupled_add(&filter_in);});
        Require(added==0 && !filter_in.owner,"original filter accepts copied owned frame exactly once");
        unsigned fields=0;
        for(unsigned g=0;g<3;++g){
          CoupledFrame out{};int ret=-1;
          Measure(row.spans[coupled::Get0+g],[&]{ret=coupled_get(&out);});
          if(ret==-EAGAIN)break;
          Require(ret==0 && i>0 && fields<2,"coupled graph returns bounded previous-source fields");
          const int64_t expected_pts=fields?coupled::Pts(i-1)+coupled::Pts(i):2*coupled::Pts(i-1);
          Require(out.pts==expected_pts && out.best==coupled::Pts(i-1) &&
                  out.flags==(patterns[(i-1)%3].bottom_first?0:16),"every field's PTS and parity metadata agrees");
          const unsigned reference=coupled::Reference(i,fields);
          row.output[fields]={out.pts,out.best,row.spans[coupled::Get0+g].wall_end,reference};
          if(!timing)coupled::Equal(out,references[reference]);
          coupled_release();++fields;++outputs;
        }
        Require(fields==(i?2U:0U),"all fields produced with unchanged unflushed lookahead");
        row.fields=fields;row.cpu_end=Clock(CLOCK_THREAD_CPUTIME_ID);row.end=Clock(CLOCK_MONOTONIC);
      }
      if(previous_map)retire();
      trial_end=Clock(CLOCK_MONOTONIC);process_end=Clock(CLOCK_PROCESS_CPUTIME_ID);
      Require(f.driver.images.empty(),"all actual public images retire before context cleanup");
      coupled_close();
    }
    Require(outputs==2*(count-1) && overlaps==(hold?count-1:0) &&
            crystalhd_cow_snapshot::LiveBytes()==baseline,"exact outputs, overlap and final resource census");
    printf("{\"type\":\"coupled_header\",\"schema\":\"crystalhd-centered-filter-coupled-v1\","
      "\"validation_mode\":\"%s\",\"fixture_idr_interval\":30,\"replay_limit_unchanged\":512,"
      "\"inputs\":%u,\"outputs\":%u,\"hold_image_control\":%s,\"same_canvas_overlap_writes\":%u,"
      "\"epoch_ns\":%" PRIu64 ",\"trial_begin_ns\":%" PRIu64 ",\"trial_end_ns\":%" PRIu64
      ",\"process_cpu_begin_ns\":%" PRIu64 ",\"process_cpu_end_ns\":%" PRIu64
      ",\"source_revision\":\"1427d2de0825706e20137f16e17dcb42366bac97\",\"distinct_patterns\":3,"
      "\"independent_scalar_input_oracle_outputs\":14,\"cached_output_references\":8,"
      "\"client_visible_bytes\":3110400,\"coded_canvas_bytes\":3133440,\"small_demand\":false,"
      "\"source_eof_injected\":false,\"stock_or_release_qualification\":false,\"final_cow_live_bytes\":%u}\n",
      timing?"TIMING_AFTER_SEPARATE_FULL_QUALIFICATION":"FULL_BYTES_EACH_MEASURED_INPUT_AND_OUTPUT",
      count,outputs,hold?"true":"false",overlaps,epoch,trial_begin,trial_end,process_begin,process_end,
      crystalhd_cow_snapshot::LiveBytes());
    for(const auto &row:rows){
      printf("{\"type\":\"coupled_cycle\",\"index\":%u,\"token\":%" PRIu64
        ",\"due_ns\":%" PRIu64 ",\"begin_ns\":%" PRIu64 ",\"end_ns\":%" PRIu64
        ",\"thread_cpu_begin_ns\":%" PRIu64 ",\"thread_cpu_end_ns\":%" PRIu64
        ",\"live_images_at_write\":%u,\"write_epoch_before\":%" PRIu64 ",\"write_epoch_after\":%" PRIu64
        ",\"owned_stages\":{",row.index,row.token,row.due,row.begin,row.end,row.cpu_begin,row.cpu_end,
        row.overlap,row.epoch_before,row.epoch_after);
      for(unsigned s=kAdmission;s<=kComplement;++s){if(s!=kAdmission)putchar(',');printf("\"%s\":",kNames[s]);coupled::PrintSpan(row.owned.stages[s]);}
      printf("},\"stages\":{");for(unsigned s=0;s<coupled::Count;++s){if(s)putchar(',');printf("\"%s\":",coupled::Names[s]);coupled::PrintSpan(row.spans[s]);}
      printf("},\"fields\":[");for(unsigned f=0;f<row.fields;++f){if(f)putchar(',');const auto &out=row.output[f];
        printf("{\"pts\":%" PRId64 ",\"best\":%" PRId64 ",\"ready_ns\":%" PRIu64 ",\"reference\":%u,\"all_pixels_equal\":%s}",out.pts,out.best,out.ready,out.reference,timing?"null":"true");}
      puts("]}");
    }
    puts("{\"type\":\"coupled_footer\",\"status\":\"PASS_SCOPED_COMBINED_CURRENT_OWN_AND_ORIGINAL_FILTER\",\"complete\":true,\"graphs_live\":0,\"images_live\":0}");
    return 0;
  }catch(const CowUnavailable &){fprintf(stderr,"coupled COW unavailable\n");return 77;}
   catch(const std::exception &e){fprintf(stderr,"coupled FAIL at input%u: %s\n",coupled::active_index,e.what());return 1;}
}
