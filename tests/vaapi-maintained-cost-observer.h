// SPDX-License-Identifier: LGPL-2.1-or-later
// OWN diagnostic scalar collector, absent from the ordinary VA backend.
#ifndef CRYSTALHD_TEST_MAINTAINED_COST_OBSERVER_H
#define CRYSTALHD_TEST_MAINTAINED_COST_OBSERVER_H
#include <array>
#include <atomic>
#include <cerrno>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <limits.h>
#include <mutex>
#include <pthread.h>
#include <sys/syscall.h>
#include <time.h>
#include <type_traits>
#include <unistd.h>
#ifndef CRYSTALHD_MAINTAINED_COST_CAPACITY
#define CRYSTALHD_MAINTAINED_COST_CAPACITY 131072
#endif
#if CRYSTALHD_MAINTAINED_COST_CAPACITY < 2 || CRYSTALHD_MAINTAINED_COST_CAPACITY > 131072
#error "Invalid fixed maintained-cost capacity"
#endif
#ifndef CRYSTALHD_MAINTAINED_COST_CLOCK
#define CRYSTALHD_MAINTAINED_COST_CLOCK clock_gettime
#endif
namespace crystalhd_test_maintained_cost {
enum class Stage {
  ReceiveOne, DilAcquire, DilRelease, ProcessOutput, FirstFieldStage,
  SecondFieldProcess, FieldConvert, FieldConvertPixels, CanvasCompose,
  CanvasPixels, CpuAccessBegin, CpuAccessEnd, DecodedCpuBegin, DecodedCpuEnd,
  I915Domain, PublicCopy, PublicCopyPixels, PublicPromote, DirectPublic,
  SyncAdmission, SyncBody, DiagnosticCapture
#if defined(CRYSTALHD_VAAPI_TEST_ENDPICTURE_BODY_COST)
  , EndPictureBody
#endif
};
inline constexpr const char *kStages[] = {
  "receive_one", "dil_acquire", "dil_release", "process_output", "first_field_stage",
  "second_field_process", "field_convert", "field_convert_pixels", "canvas_compose",
  "canvas_pixels", "cpu_access_begin", "cpu_access_end", "decoded_cpu_begin",
  "decoded_cpu_end", "i915_domain", "public_copy", "public_copy_pixels",
  "public_promote", "direct_public", "sync_admission", "sync_body", "diagnostic_capture"
#if defined(CRYSTALHD_VAAPI_TEST_ENDPICTURE_BODY_COST)
  , "end_picture_body"
#endif
};
#if defined(CRYSTALHD_VAAPI_TEST_ENDPICTURE_BODY_COST)
inline constexpr const char *kSchema = "crystalhd-maintained-cost-endpicture-v1";
#else
inline constexpr const char *kSchema = "crystalhd-maintained-cost-v2";
#endif
enum Flag : uint64_t { Compact=1, DecodedPublic=2, DirectPublic=4,
  SelectedCapture=8, I915Wc=16, Read=32, Write=64 };
enum class Kind { None, Decode, Surface, Full };
enum class Phase { Entry, Inherited, Output, Admission };
inline const char *KindName(Kind k) {
  switch (k) {case Kind::None:return "none";case Kind::Decode:return "decode";
    case Kind::Surface:return "surface";case Kind::Full:return "full";}
  return "invalid";
}
inline const char *PhaseName(Phase p) {
  switch (p) {case Phase::Entry:return "entry";case Phase::Inherited:return "inherited";
    case Phase::Output:return "resolved_after_output";case Phase::Admission:return "resolved_after_admission";}
  return "invalid";
}
struct Identity {
  uint64_t driver=0, context=0, generation=0, token=0, decode=0, ordinal=0, surface=0, owner=0;
  Kind kind=Kind::None;
};
inline bool Full(const Identity &i) {
  return i.driver && i.context && i.generation && i.token && i.decode && i.ordinal &&
      i.surface && i.owner && i.surface!=uint32_t(-1) && i.owner!=uint32_t(-1);
}
// The trace adapter also describes valid bootstrap Sync requests that know a
// surface/owner but have no decoder context yet. Reserve None for session-only.
template <typename Fields> inline Identity TraceIdentity(uint64_t driver,const Fields &f) {
  Identity i;i.driver=driver;i.context=f.context;i.generation=f.generation;
  i.token=f.token;i.decode=f.decode_identity;i.ordinal=f.submission_ordinal;
  i.surface=f.surface;i.owner=f.owner;
  if (Full(i)) i.kind=Kind::Full;
  else if (i.driver && i.context && i.generation) i.kind=Kind::Decode;
  else if (i.context || i.generation || i.token || i.decode || i.ordinal || i.surface || i.owner)
    i.kind=Kind::Surface;
  return i;
}
// Explicit surface identity inherits only a driver session, never missing source
// tuple components. Only a default scope may inherit its actual parent tuple.
template <typename Surface> inline Identity SurfaceIdentity(const Surface *s) {
  Identity i; i.kind=Kind::Surface;
  if (s) {
    i.context=s->trace_context;i.generation=s->trace_generation;i.token=s->expected_timestamp;
    i.decode=s->trace_decode_identity;i.ordinal=s->trace_submission_ordinal;
    i.surface=s->id==uint32_t(-1)?0:s->id;
    i.owner=s->backing_owner==uint32_t(-1)?i.surface:s->backing_owner;
  }
  return i;
}
struct Event {
  uint64_t parent=0, wall_begin=0,wall_end=0,cpu_begin=0,cpu_end=0,wall=0,cpu=0;
  Identity identity;
  uint64_t detail=0, flags=0;
  int64_t result=0;
  pid_t tid=0,end_tid=0;
  Stage stage=Stage::ReceiveOne;
  Phase phase=Phase::Entry;
  bool has_result=false;
  std::atomic<bool> ready{false};
};
class State;
inline State *fork_state=nullptr;
class State {
 public:
  std::array<Event,CRYSTALHD_MAINTAINED_COST_CAPACITY> events{};
  std::atomic<unsigned> attempted{0},dropped{0},inflight{0},clock_failures{0},lifecycle_failures{0};
  std::atomic<bool> fork_seen{false};
  std::mutex gate;
  pid_t pid=0;
  bool enabled=false, publishing=false;
  char prefix[PATH_MAX]{};
  State() {
    const int saved=errno;pid=getpid();
    const char *marker=std::getenv("CRYSTALHD_TEST_MAINTAINED_COST");
    const char *path=std::getenv("CRYSTALHD_TEST_MAINTAINED_COST_PREFIX");
    enabled=marker && std::strcmp(marker,"v2")==0 && path && path[0]=='/' && std::strlen(path)<sizeof(prefix);
    if (enabled) {
      std::memcpy(prefix,path,std::strlen(path)+1);fork_state=this;
      if (pthread_atfork(ForkPrepare,nullptr,nullptr)) ++lifecycle_failures;
    }
    errno=saved;
  }
  State(const State&)=delete;State& operator=(const State&)=delete;
  static void ForkPrepare() {
    if (fork_state && fork_state->enabled && fork_state->pid==getpid()) fork_state->fork_seen=true;
  }
  uint64_t Clock(clockid_t clock) noexcept {
    timespec value{};
    if (CRYSTALHD_MAINTAINED_COST_CLOCK(clock,&value)!=0 || value.tv_sec<0 || value.tv_nsec<0 || value.tv_nsec>=1000000000) {
      ++clock_failures;return 0;
    }
    return static_cast<uint64_t>(value.tv_sec)*UINT64_C(1000000000)+static_cast<uint64_t>(value.tv_nsec);
  }
  bool Reserve(unsigned *index) {
    if (!enabled || pid!=getpid()) return false;
    std::lock_guard<std::mutex> hold(gate);
    if (publishing) return false;
    *index=attempted.fetch_add(1,std::memory_order_relaxed);
    if (*index>=events.size()) {++dropped;return false;}
    ++inflight;return true;
  }
  static bool WriteAll(int fd,const char *data,size_t size) {
    while (size) {
      const ssize_t done=write(fd,data,size);
      if (done<0 && errno==EINTR) continue;
      if (done<=0) return false;
      data+=done;size-=static_cast<size_t>(done);
    }
    return true;
  }
  ~State() {Publish();if (fork_state==this) fork_state=nullptr;}
  void Publish() noexcept {
    const int saved=errno;
    if (!enabled || pid!=getpid()) {errno=saved;return;}
    unsigned busy;
    {std::lock_guard<std::mutex> hold(gate);if (publishing) {errno=saved;return;}publishing=true;busy=inflight.load(std::memory_order_acquire);}
    char path[PATH_MAX];const int length=std::snprintf(path,sizeof(path),"%s-%ld.jsonl",prefix,static_cast<long>(pid));
    if (length<=0 || static_cast<size_t>(length)>=sizeof(path)) {errno=saved;return;}
    const int fd=open(path,O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW,0600);
    if (fd<0) {errno=saved;return;}
    bool ok=dprintf(fd,"{\"type\":\"header\",\"schema\":\"%s\",\"pid\":%ld,\"fork_policy\":\"PARENT_PID_ONLY_CHILD_FORWARD_ONLY\",\"scope\":\"OWN_MAINTAINED_BACKEND_ONLY\",\"capacity\":%zu,\"clock_names\":{\"wall\":\"CLOCK_MONOTONIC\",\"cpu\":\"CLOCK_THREAD_CPUTIME_ID\"},\"stages\":[",kSchema,static_cast<long>(pid),events.size())>=0;
    for (unsigned i=0;ok && i<sizeof(kStages)/sizeof(kStages[0]);++i)
      ok=dprintf(fd,"%s\"%s\"",i?",":"",kStages[i])>=0;
    if (ok) ok=dprintf(fd,"],\"flags\":{\"compact\":1,\"decoded_public\":2,\"direct_public\":4,\"selected_capture\":8,\"i915_wc\":16,\"read\":32,\"write\":64}}\n")>=0;
    const unsigned total=attempted.load(std::memory_order_acquire);
    const unsigned count=total<events.size()?total:static_cast<unsigned>(events.size());
    unsigned written=0,unpublished=0;
    for (unsigned index=0;index<count;++index) {
      const auto &e=events[index];
      if (!e.ready.load(std::memory_order_acquire)) {++unpublished;continue;}
      if (!ok) continue;
      const Identity &i=e.identity;
      ok=dprintf(fd,"{\"type\":\"event\",\"seq\":%u,\"id\":%u,\"parent_id\":%" PRIu64 ",\"pid\":%ld,\"tid\":%ld,\"end_tid\":%ld,\"stage\":\"%s\",\"wall_begin_ns\":%" PRIu64 ",\"wall_end_ns\":%" PRIu64 ",\"cpu_begin_ns\":%" PRIu64 ",\"cpu_end_ns\":%" PRIu64 ",\"wall_ns\":%" PRIu64 ",\"cpu_ns\":%" PRIu64 ",\"driver_instance\":%" PRIu64 ",\"context\":%" PRIu64 ",\"generation\":%" PRIu64 ",\"token\":%" PRIu64 ",\"decode_identity\":%" PRIu64 ",\"submission_ordinal\":%" PRIu64 ",\"surface\":%" PRIu64 ",\"owner\":%" PRIu64 ",\"binding_kind\":\"%s\",\"binding_phase\":\"%s\",\"detail\":%" PRIu64 ",\"flags\":%" PRIu64 ",\"result\":",index,index+1,e.parent,static_cast<long>(pid),static_cast<long>(e.tid),static_cast<long>(e.end_tid),kStages[static_cast<unsigned>(e.stage)],e.wall_begin,e.wall_end,e.cpu_begin,e.cpu_end,e.wall,e.cpu,i.driver,i.context,i.generation,i.token,i.decode,i.ordinal,i.surface,i.owner,KindName(i.kind),PhaseName(e.phase),e.detail,e.flags)>=0;
      if (ok) ok=e.has_result?dprintf(fd,"%" PRId64 "}\n",e.result)>=0:dprintf(fd,"null}\n")>=0;
      if (ok) ++written;
    }
    const unsigned lost=dropped.load(),bad_clock=clock_failures.load(),bad_life=lifecycle_failures.load();
    const bool complete=ok && !lost && !bad_clock && !bad_life && !busy && !unpublished;
    if (ok) (void)dprintf(fd,"{\"type\":\"footer\",\"events\":%u,\"attempted_events\":%u,\"dropped\":%u,\"inflight\":%u,\"unpublished\":%u,\"clock_failures\":%u,\"lifecycle_failures\":%u,\"fork_seen\":%s,\"complete\":%s}\n",written,total,lost,busy,unpublished,bad_clock,bad_life,fork_seen.load()?"true":"false",complete?"true":"false");
    close(fd);errno=saved;
  }
};
inline State state;
class Scope;
inline thread_local Scope *active_scope=nullptr;
class Scope {
  State *state_=nullptr;
  Event *event_=nullptr;
  Scope *previous_=nullptr;
  unsigned index_=0;
  bool finished_=false;
 public:
  explicit Scope(Stage stage,Identity identity={},uint64_t flags=0,uint64_t detail=0) noexcept {
    const int saved=errno;
    if (state.Reserve(&index_)) {
      state_=&state;event_=&state.events[index_];previous_=active_scope;
      event_->stage=stage;event_->tid=static_cast<pid_t>(syscall(SYS_gettid));
      if (previous_ && previous_->event_ && !previous_->finished_) {
        event_->parent=previous_->index_+1;
        if (identity.kind==Kind::None && identity.driver==0) {identity=previous_->event_->identity;event_->phase=Phase::Inherited;}
        else if (!identity.driver) identity.driver=previous_->event_->identity.driver;
      } else if (previous_) ++state_->lifecycle_failures;
      if (identity.kind==Kind::Surface && Full(identity)) identity.kind=Kind::Full;
      event_->identity=identity;event_->flags=flags;event_->detail=detail;
      active_scope=this;
      event_->wall_begin=state_->Clock(CLOCK_MONOTONIC);
      event_->cpu_begin=state_->Clock(CLOCK_THREAD_CPUTIME_ID);
    }
    errno=saved;
  }
  Scope(const Scope&)=delete;Scope& operator=(const Scope&)=delete;
  ~Scope() {Finish();}
  void Bind(Identity identity,Phase phase) noexcept {
    if (!event_ || finished_) return;
    if (!identity.driver) identity.driver=event_->identity.driver;
    if (identity.kind==Kind::Surface && Full(identity)) identity.kind=Kind::Full;
    event_->identity=identity;event_->phase=phase;
  }
  void Flags(uint64_t value) noexcept {if (event_ && !finished_) event_->flags=value;}
  void Detail(uint64_t value) noexcept {if (event_ && !finished_) event_->detail=value;}
  void Result(int64_t value) noexcept {if (event_ && !finished_) {event_->result=value;event_->has_result=true;}}
  template <typename T> T Return(T value) noexcept {Result(static_cast<int64_t>(value));return value;}
  void Finish() noexcept {
    const int saved=errno;
    if (!event_ || finished_ || !state_ || state_->pid!=getpid()) {errno=saved;return;}
    event_->cpu_end=state_->Clock(CLOCK_THREAD_CPUTIME_ID);
    event_->wall_end=state_->Clock(CLOCK_MONOTONIC);
    event_->end_tid=static_cast<pid_t>(syscall(SYS_gettid));
    if (event_->cpu_end<event_->cpu_begin || event_->wall_end<event_->wall_begin ||
        event_->tid!=event_->end_tid || active_scope!=this) ++state_->lifecycle_failures;
    else {
      event_->wall=event_->wall_end-event_->wall_begin;event_->cpu=event_->cpu_end-event_->cpu_begin;
      // Independent wall/thread-CPU accounting can disagree on a short span.
      // Preserve both raw deltas; derived off-CPU may be unavailable. This is
      // not a parent/thread/lifetime failure and must not be clamped.
    }
    if (active_scope==this) active_scope=previous_;
    finished_=true;event_->ready.store(true,std::memory_order_release);
    state_->inflight.fetch_sub(1,std::memory_order_release);errno=saved;
  }
};
template <typename Function>
inline auto Call(Stage stage,Identity identity,Function &&function,uint64_t flags=0,uint64_t detail=0) {
  Scope scope(stage,identity,flags,detail);
  if constexpr (std::is_void_v<decltype(function())>) {function();return;}
  else return scope.Return(function());
}
} // namespace crystalhd_test_maintained_cost
#endif
