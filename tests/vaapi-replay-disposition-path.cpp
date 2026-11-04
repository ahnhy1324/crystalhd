// SPDX-License-Identifier: LGPL-2.1-or-later
// OWN production replay/field-pair path, linked to the existing DIL CPU mocks.
// These are explicit software scaffolds, never stock/hardware qualification.
#define CRYSTALHD_VAAPI_TEST_MAINTAINED_COST 1
#define CRYSTALHD_VAAPI_TEST_REPLAY_DISPOSITION 1
#define main CrystalHDExistingReplayMain
// The existing harness defines global fixture structs containing the driver's
// anonymous-namespace types. Inclusion in this single CPU-only TU makes GCC
// diagnose their linkage; no fixture is exported or shared between TUs.
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsubobject-linkage"
#endif
#include "vaapi-replay.cpp"
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif
#undef main

static VAStatus ReceiveField(H264FieldFixture &f,DecodeIoMock &io,
                             const BC_DTS_PROC_OUT &out) {
  io.override_output=true;io.overridden_output=out;
  bool activity=false;
  const VAStatus status=ReceiveOne(&f.driver,&f.decoder,0,&activity);
  Require(activity,"actual ReceiveOne acquired the mocked public field");
  return status;
}
static void BindScaffold(Driver &driver,DecodeContext &decode,
                         const std::shared_ptr<Surface> &surface) {
  // Explicit fixture admission provenance; the production observer never
  // synthesizes this history. Real cases use EndPicture's existing SubmitBind.
  driver.trace.Emit(crystalhd_vaapi_trace::Event::SubmitBind,
                    TraceFields(&decode,surface.get()));
}
static void RunDispositionPath(const std::string &mode) {
  DecodeIoMock io;MockDecodeScope mocks(io);io.allow_device_open=true;
  H264FieldFixture f;auto &d=f.decoder;
  f.private_frame->id=1;
  d.device=io.handle();d.decoder_open=true;d.decoder_started=true;
  d.replay=CrystalHDDecodeReplay();
  Require(d.replay.Append(1,true,{1}),"fresh fixture has one unsubmitted original");
  BindScaffold(f.driver,d,f.public_frame);
  io.input_busy=true;
  Require(PumpDecodeInput(&f.driver,&d,1)==VA_STATUS_SUCCESS &&
              d.replay.NextInput() && !d.replay.IsOutstanding(1),
          "busy original attempt cannot become a logical input commit");
  io.input_busy=false;
  Require(PumpDecodeInput(&f.driver,&d,1)==VA_STATUS_SUCCESS &&
              d.replay.IsOutstanding(1) && d.replay.Seal(),
          "actual Pump accepts original logical input before sealed field drain");
  auto first=f.Output(false,false,&f.top);
  auto second=f.Output(true,false,&f.bottom);
  Require(ReceiveField(f,io,first)==VA_STATUS_SUCCESS &&
              d.h264_first_field.valid && !f.public_frame->ready &&
              ReceiveField(f,io,second)==VA_STATUS_SUCCESS &&
              f.public_frame->ready && d.replay.outstanding()==0 &&
              d.replay.EndOfSequence(),
          "actual new pair materializes once and retains its sealed history");
  f.CheckPixels(*f.public_frame);
  const auto old_pixels=f.public_frame->storage;
  const auto private_pixels=f.private_frame->storage;
  auto continuation=std::make_shared<Surface>();
  auto next_public=std::make_shared<Surface>();
  Require(continuation->AllocateInternal(nullptr,-1,16,32,VA_FOURCC_NV12) &&
              next_public->AllocateInternal(nullptr,-1,16,32,VA_FOURCC_NV12),
          "allocate distinct continuation fixture backing");
  continuation->id=2;continuation->expected_timestamp=2;continuation->decode_identity=42;
  continuation->trace_decode_identity=42;continuation->trace_context=d.id;
  continuation->trace_generation=d.generation;continuation->trace_submission_ordinal=2;
  continuation->h264_crop_unit_y=4;
  next_public->id=2;next_public->expected_timestamp=2;next_public->decode_identity=42;
  next_public->h264_crop_unit_y=4;InheritTraceIdentity(next_public.get(),*continuation);
  next_public->decode_picture=continuation;
  f.driver.surfaces[2]=next_public;d.pending[2]=2;d.decoded_frames[2]=continuation;
  d.surface_timestamps[next_public.get()]=2;
  BindScaffold(f.driver,d,next_public);
  Require(d.replay.Append(2,false,{2}),"accepted continuation requires actual retained-prefix restart");
  if(mode=="retired-backing") {
    EndTraceBinding(&f.driver,f.public_frame.get());
    d.decoded_frames.erase(1);
    // Same public object now owns another transaction. No old identity is
    // restored by the observer; old replay output must preserve raw zeros.
    f.public_frame->expected_timestamp=3;f.public_frame->decode_identity=99;
    std::fill(f.public_frame->storage.begin(),f.public_frame->storage.end(),0x7b);
  }
  const auto protected_pixels=f.public_frame->storage;
  Require(PumpDecodeInput(&f.driver,&d,1)==VA_STATUS_SUCCESS &&
              d.replay_restart_ordinal==1 && d.replay.IsOutstanding(1) &&
              !d.replay.IsOutstanding(2) && io.decoder_open_calls==1,
          "actual Pump close/open/Restarted emits one completed-history resend");
  std::fill(f.top.begin(),f.top.end(),0);std::fill(f.bottom.begin(),f.bottom.end(),0);
  Require(ReceiveField(f,io,first)==VA_STATUS_SUCCESS &&
              d.h264_first_field.valid && d.h264_first_field.replay_duplicate &&
              d.h264_first_field.pixels.empty() && !d.h264_first_field.picture,
          "actual duplicate first field keeps metadata only, never replacement pixels");
  if(mode=="wrong-parity") second.PicInfo.flags=first.PicInfo.flags;
  if(mode=="release-failure") io.output_release_status=BC_STS_ERROR;
  const VAStatus status=ReceiveField(f,io,second);
  const bool negative=mode=="wrong-parity" || mode=="release-failure";
  Require(status==(negative?VA_STATUS_ERROR_DECODING_ERROR:VA_STATUS_SUCCESS) &&
              !d.h264_first_field.valid && f.public_frame->storage==protected_pixels &&
              f.private_frame->storage==private_pixels && io.released_outputs==4 && !io.invalid,
          "actual pair and both releases preserve completed/rebound pixels; rejection stays explicit");
  if(!negative) {
    Require(!d.replay.IsOutstanding(1) &&
                PumpDecodeInput(&f.driver,&d,1)==VA_STATUS_SUCCESS &&
                d.replay.IsOutstanding(2),
            "only validated replay pair retires its own replay debt");
    first.PicInfo.timeStamp=second.PicInfo.timeStamp=2;
    ++first.PicInfo.picture_number;++second.PicInfo.picture_number;
    H264FieldFixture::Fill(f.top,16,20,40,60);
    H264FieldFixture::Fill(f.bottom,16,100,140,160);
    Require(ReceiveField(f,io,first)==VA_STATUS_SUCCESS &&
                ReceiveField(f,io,second)==VA_STATUS_SUCCESS && next_public->ready &&
                next_public->frame_timestamp==2 && d.replay.outstanding()==0 &&
                io.released_outputs==6 && f.public_frame->storage==protected_pixels,
            "new continuation retains its own identity, pixels and exactly-once materialization");
    f.CheckPixels(*next_public);
    if(mode!="retired-backing") Require(f.public_frame->storage==old_pixels,"original public pixels remain exact");
  } else Require(d.replay.failed(),"broken pair or release remains terminal");
  io.output_release_status=BC_STS_SUCCESS;
  Require(d.CloseHardware()==BC_STS_SUCCESS && !io.invalid,
          "mocked hardware lifecycle closes without any real device access");
  f.driver.trace.Emit(crystalhd_vaapi_trace::Event::ContextDestroy,TraceFields(&d,nullptr));
}
int main(int argc,char **argv) {
  if(argc!=3 || std::strcmp(argv[1],"--case") ||
      (std::strcmp(argv[2],"positive") && std::strcmp(argv[2],"retired-backing") &&
       std::strcmp(argv[2],"wrong-parity") && std::strcmp(argv[2],"release-failure"))) {
    std::fprintf(stderr,"usage: %s --case positive|retired-backing|wrong-parity|release-failure\n",argv[0]);return 2;
  }
  Require(fcntl(STDERR_FILENO,F_GETFD)>=0,"caller supplies retained native trace stderr");
  RunDispositionPath(argv[2]);
  std::printf("OWN actual replay-disposition path %s: PASS pid=%ld\n",argv[2],static_cast<long>(getpid()));
  return 0;
}
