// SPDX-License-Identifier: LGPL-2.1-or-later
// OWN bounded scalar diagnostic only; never included by the ordinary backend.
#ifndef CRYSTALHD_TEST_REPLAY_DISPOSITION_OBSERVER_H
#define CRYSTALHD_TEST_REPLAY_DISPOSITION_OBSERVER_H
#include <array>
#include <atomic>
#include <cerrno>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <limits.h>
#include <mutex>
#include <pthread.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>
#ifndef CRYSTALHD_REPLAY_DISPOSITION_CAPACITY
#define CRYSTALHD_REPLAY_DISPOSITION_CAPACITY 32768
#endif
#if CRYSTALHD_REPLAY_DISPOSITION_CAPACITY < 2 || CRYSTALHD_REPLAY_DISPOSITION_CAPACITY > 32768
#error "Invalid replay disposition fixed capacity"
#endif
#ifndef CRYSTALHD_REPLAY_DISPOSITION_CLOCK
#define CRYSTALHD_REPLAY_DISPOSITION_CLOCK clock_gettime
#endif
namespace crystalhd_test_replay_disposition {
enum class Type { Restart, InputAttempt, InputCommit, Output, Disposition, Release };
inline const char *TypeName(Type t) {
  switch(t) {case Type::Restart:return "restart";case Type::InputAttempt:return "input_attempt";
    case Type::InputCommit:return "input_commit";case Type::Output:return "output";
    case Type::Disposition:return "disposition";case Type::Release:return "release";}
  return "invalid";
}
// Optional booleans use -1=null,0=false,1=true. Optional strings are static
// literals owned by this implementation, never pointers into application data.
struct Event {
  Type type=Type::Restart;
  uint64_t driver_instance=0,context=0,generation=0,token=0,restart_ordinal=0;
  uint64_t receive_id=0,native_seq=0,live_decode_identity=0,live_submission_ordinal=0;
  uint64_t live_surface=0,live_owner=0,input_offset=0,input_bytes=0,unit_bytes=0,cached_units=0;
  uint64_t pair_first_receive_id=0,picture_number=0,sess_num=0,picture_meta_payload=0;
  uint64_t width=0,height=0,field_flags=0,monotonic_ns=0;
  pid_t tid=0;
  int64_t result=0;
  int completed_before=-1,field_valid=-1,pair_valid=-1,replay_duplicate=-1;
  bool has_result=false,fragmented=false,consume_prefeed=false;
  const char *restart_phase=nullptr,*inspect=nullptr,*observe=nullptr,*field_role="none";
};
class State;
inline State *fork_state=nullptr;
class State {
 public:
  std::array<Event,CRYSTALHD_REPLAY_DISPOSITION_CAPACITY> events{};
  std::mutex gate;
  uint64_t attempted=0,dropped=0,clock_failures=0,lifecycle_failures=0,receive_next=0;
  std::atomic<unsigned> inflight{0};
  std::atomic<bool> fork_seen{false};
  pid_t pid=0;bool enabled=false,publishing=false;
  char prefix[PATH_MAX]{};
  State() noexcept {
    const int saved=errno;pid=getpid();
    const char *marker=std::getenv("CRYSTALHD_TEST_REPLAY_DISPOSITION");
    const char *path=std::getenv("CRYSTALHD_TEST_REPLAY_DISPOSITION_PREFIX");
    enabled=marker && std::strcmp(marker,"v1")==0 && path && path[0]=='/' && std::strlen(path)<sizeof(prefix);
    if(enabled) {std::memcpy(prefix,path,std::strlen(path)+1);fork_state=this;
      if(pthread_atfork(ForkPrepare,nullptr,nullptr)) ++lifecycle_failures;}
    errno=saved;
  }
  State(const State&)=delete;State& operator=(const State&)=delete;
  bool Active() const noexcept {return enabled && pid==getpid();}
  static void ForkPrepare() noexcept {
    if(fork_state && fork_state->Active()) fork_state->fork_seen=true;
  }
  void Failure() noexcept {
    const int saved=errno;if(Active()) {std::lock_guard<std::mutex> lock(gate);++lifecycle_failures;}errno=saved;
  }
  uint64_t Begin() noexcept {
    const int saved=errno;uint64_t id=0;
    if(Active()) {std::lock_guard<std::mutex> lock(gate);
      if(!publishing) {id=++receive_next;if(!id) ++lifecycle_failures;else ++inflight;}}
    errno=saved;return id;
  }
  void End() noexcept {
    const int saved=errno;if(Active()) {std::lock_guard<std::mutex> lock(gate);
      if(!inflight) ++lifecycle_failures;else --inflight;}errno=saved;
  }
  void Record(Event e) noexcept {
    const int saved=errno;
    if(Active()) {std::lock_guard<std::mutex> lock(gate);
      if(publishing) {++lifecycle_failures;errno=saved;return;}
      timespec now{};
      if(CRYSTALHD_REPLAY_DISPOSITION_CLOCK(CLOCK_MONOTONIC,&now) || now.tv_sec<0 || now.tv_nsec<0 || now.tv_nsec>=1000000000) ++clock_failures;
      else e.monotonic_ns=static_cast<uint64_t>(now.tv_sec)*UINT64_C(1000000000)+static_cast<uint64_t>(now.tv_nsec);
      e.tid=static_cast<pid_t>(syscall(SYS_gettid));
      const uint64_t index=attempted++;
      if(index>=events.size()) ++dropped;else events[index]=e;
    }errno=saved;
  }
  static bool WriteAll(int fd,const char *data,size_t size) noexcept {
    while(size) {const ssize_t n=write(fd,data,size);if(n<0 && errno==EINTR) continue;
      if(n<=0) return false;
      data+=n;size-=static_cast<size_t>(n);}return true;
  }
  static const char *Bool(int n) noexcept {return n<0?"null":n?"true":"false";}
  static const char *Quoted(const char *s,char (&out)[32]) noexcept {
    if(!s) return "null";
    const int n=std::snprintf(out,sizeof(out),"\"%s\"",s);
    return n>0 && static_cast<size_t>(n)<sizeof(out)?out:"null";
  }
  void Publish() noexcept {
    const int saved=errno;
    if(!Active()) {errno=saved;return;}
    std::lock_guard<std::mutex> lock(gate);
    if(publishing) {errno=saved;return;}publishing=true;
    char path[PATH_MAX];int n=std::snprintf(path,sizeof(path),"%s-%ld.jsonl",prefix,static_cast<long>(pid));
    if(n<=0 || static_cast<size_t>(n)>=sizeof(path)) {errno=saved;return;}
    const int fd=open(path,O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW,0600);
    if(fd<0) {errno=saved;return;}
    char line[3072];
    n=std::snprintf(line,sizeof(line),"{\"type\":\"header\",\"schema\":\"crystalhd-replay-disposition-v1\",\"pid\":%ld,\"fork_policy\":\"PARENT_PID_ONLY_CHILD_FORWARD_ONLY\",\"scope\":\"OWN_REPLAY_DISPOSITION_ONLY\",\"capacity\":%zu,\"bc_success\":0,\"va_success\":0,\"field_constants\":{\"topfield\":16,\"bottomfield\":24,\"interlaced_src\":32,\"bottom_first\":128}}\n",static_cast<long>(pid),events.size());
    bool ok=n>0 && static_cast<size_t>(n)<sizeof(line) && WriteAll(fd,line,static_cast<size_t>(n));
    const uint64_t count=attempted<events.size()?attempted:events.size();
    for(uint64_t i=0;i<count && ok;++i) {
      const Event &e=events[i];char result[32],phase[32],inspect[32],observe[32];
      if(e.has_result) std::snprintf(result,sizeof(result),"%" PRId64,e.result);else std::strcpy(result,"null");
      n=std::snprintf(line,sizeof(line),
        "{\"type\":\"event\",\"seq\":%" PRIu64 ",\"id\":%" PRIu64 ",\"pid\":%ld,\"tid\":%ld,\"monotonic_ns\":%" PRIu64 ",\"event\":\"%s\","
        "\"driver_instance\":%" PRIu64 ",\"context\":%" PRIu64 ",\"generation\":%" PRIu64 ",\"token\":%" PRIu64 ",\"restart_ordinal\":%" PRIu64 ",\"receive_id\":%" PRIu64 ",\"native_seq\":%" PRIu64 ","
        "\"live_decode_identity\":%" PRIu64 ",\"live_submission_ordinal\":%" PRIu64 ",\"live_surface\":%" PRIu64 ",\"live_owner\":%" PRIu64 ",\"result\":%s,\"completed_before\":%s,\"fragmented\":%s,\"consume_prefeed\":%s,"
        "\"input_offset\":%" PRIu64 ",\"input_bytes\":%" PRIu64 ",\"unit_bytes\":%" PRIu64 ",\"cached_units\":%" PRIu64 ",\"restart_phase\":%s,\"inspect\":%s,\"observe\":%s,\"field_role\":\"%s\","
        "\"field_valid\":%s,\"pair_valid\":%s,\"replay_duplicate\":%s,\"pair_first_receive_id\":%" PRIu64 ",\"picture_number\":%" PRIu64 ",\"sess_num\":%" PRIu64 ",\"picture_meta_payload\":%" PRIu64 ",\"width\":%" PRIu64 ",\"height\":%" PRIu64 ",\"field_flags\":%" PRIu64 "}\n",
        i,i+1,static_cast<long>(pid),static_cast<long>(e.tid),e.monotonic_ns,TypeName(e.type),
        e.driver_instance,e.context,e.generation,e.token,e.restart_ordinal,e.receive_id,e.native_seq,
        e.live_decode_identity,e.live_submission_ordinal,e.live_surface,e.live_owner,result,Bool(e.completed_before),e.fragmented?"true":"false",e.consume_prefeed?"true":"false",
        e.input_offset,e.input_bytes,e.unit_bytes,e.cached_units,Quoted(e.restart_phase,phase),Quoted(e.inspect,inspect),Quoted(e.observe,observe),e.field_role,
        Bool(e.field_valid),Bool(e.pair_valid),Bool(e.replay_duplicate),e.pair_first_receive_id,e.picture_number,e.sess_num,e.picture_meta_payload,e.width,e.height,e.field_flags);
      ok=n>0 && static_cast<size_t>(n)<sizeof(line) && WriteAll(fd,line,static_cast<size_t>(n));
    }
    n=std::snprintf(line,sizeof(line),"{\"type\":\"footer\",\"events\":%" PRIu64 ",\"attempted_events\":%" PRIu64 ",\"dropped\":%" PRIu64 ",\"inflight\":%u,\"unpublished\":0,\"clock_failures\":%" PRIu64 ",\"lifecycle_failures\":%" PRIu64 ",\"fork_seen\":%s,\"complete\":%s}\n",count,attempted,dropped,inflight.load(),clock_failures,lifecycle_failures,fork_seen?"true":"false",ok&&!dropped&&!inflight&&!clock_failures&&!lifecycle_failures?"true":"false");
    if(ok && n>0 && static_cast<size_t>(n)<sizeof(line)) WriteAll(fd,line,static_cast<size_t>(n));
    close(fd);errno=saved;
  }
  ~State() {Publish();if(fork_state==this) fork_state=nullptr;}
};
// Bind ownership when this private DSO/TU loads, before any inherited fork can
// execute its first hook. Only exec may establish a fresh process owner.
inline State state;
inline State &Global() {return state;}
inline Event Base(uint64_t driver,uint64_t context,uint64_t generation,uint64_t ordinal) {
  Event e;e.driver_instance=driver;e.context=context;e.generation=generation;e.restart_ordinal=ordinal;return e;
}
inline void Record(Event e) noexcept {Global().Record(e);}
class Receive;
inline thread_local Receive *current_receive=nullptr;
class Receive {
 public:
  Event event;Receive *parent=nullptr;pid_t tid=0;
  bool active=false,output=false,disposed=false,released=false;
  Receive(Event base,bool eligible):event(base) {
    const int saved=errno;
    if(eligible && Global().Active()) {event.receive_id=Global().Begin();active=event.receive_id!=0;
      if(active) {parent=current_receive;current_receive=this;tid=static_cast<pid_t>(syscall(SYS_gettid));if(parent) Global().Failure();}}
    errno=saved;
  }
  Receive(const Receive&)=delete;Receive& operator=(const Receive&)=delete;
  template<typename Fields,typename Pic> void Output(uint64_t seq,const Fields &f,const Pic &p) noexcept {
    if(!active) return;
    if(output) {Global().Failure();return;}
    event.token=f.token;event.native_seq=seq;event.live_decode_identity=f.decode_identity;
    event.live_submission_ordinal=f.submission_ordinal;event.live_surface=f.surface;event.live_owner=f.owner;
    event.picture_number=p.picture_number;event.sess_num=p.sess_num;event.picture_meta_payload=p.picture_meta_payload;
    event.width=p.width;event.height=p.height;event.field_flags=p.flags;event.field_role="whole";
    event.type=Type::Output;Record(event);output=true;
  }
  void Disposition(int64_t result) noexcept {
    if(!active) return;
    if(!output || disposed) {Global().Failure();return;}
    event.type=Type::Disposition;event.result=result;event.has_result=true;Record(event);disposed=true;
  }
  void Release(int64_t result) noexcept {
    if(!active) return;
    if(!disposed || released) {Global().Failure();return;}
    event.type=Type::Release;event.result=result;event.has_result=true;Record(event);released=true;
  }
  ~Receive() {
    const int saved=errno;
    if(active && Global().Active()) {
      if(current_receive!=this || tid!=static_cast<pid_t>(syscall(SYS_gettid)) || !output || !disposed || !released) Global().Failure();
      current_receive=parent;Global().End();
    }errno=saved;
  }
};
inline uint64_t ReceiveId() noexcept {return current_receive && Global().Active()?current_receive->event.receive_id:0;}
inline Event *Current() noexcept {return current_receive && Global().Active()?&current_receive->event:nullptr;}
template<typename Fields,typename Pic> inline void Output(uint64_t seq,const Fields &f,const Pic &p) noexcept {
  if(current_receive && Global().Active()) current_receive->Output(seq,f,p);
}
inline void Inspect(const char *value,bool duplicate,bool first,uint64_t first_id) noexcept {
  if(Event *e=Current()) {e->inspect=value;e->replay_duplicate=duplicate;e->field_role=first?"first":"complement";
    e->pair_first_receive_id=first_id;e->pair_valid=first?-1:0;}
}
inline void Field(bool valid) noexcept {if(Event *e=Current()) e->field_valid=valid;}
inline void Pair() noexcept {if(Event *e=Current()) e->pair_valid=1;}
inline void Observe(const char *value) noexcept {if(Event *e=Current()) e->observe=value;}
}  // namespace crystalhd_test_replay_disposition
#endif
