// SPDX-License-Identifier: LGPL-2.1-or-later
// Standalone OWN fixed scalar collector controls; no DIL/DRM/device/media.
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <new>
#include <string>
#include <sys/stat.h>
#include <sys/wait.h>
#include <thread>
#include <time.h>
#include <unistd.h>
#include <vector>
static std::atomic<bool> forbid_allocations{false};
void *operator new(std::size_t size) {
  if(forbid_allocations.load()) throw std::bad_alloc();
  if(void *p=std::malloc(size?size:1)) return p;
  throw std::bad_alloc();
}
void *operator new[](std::size_t size) {return ::operator new(size);}
__attribute__((noinline)) void operator delete(void *p) noexcept {std::free(p);}
__attribute__((noinline)) void operator delete[](void *p) noexcept {std::free(p);}
__attribute__((noinline)) void operator delete(void *p,std::size_t) noexcept {std::free(p);}
__attribute__((noinline)) void operator delete[](void *p,std::size_t) noexcept {std::free(p);}
static bool fail_clock=false;
static int OwnClock(clockid_t id,timespec *out) {
  if(fail_clock) {errno=EIO;return -1;}
  return clock_gettime(id,out);
}
#define CRYSTALHD_REPLAY_DISPOSITION_CAPACITY 16
#define CRYSTALHD_REPLAY_DISPOSITION_CLOCK OwnClock
#include "vaapi-replay-disposition-observer.h"
namespace replay=crystalhd_test_replay_disposition;
static void Check(bool ok,const char *why) {
  if(!ok) {std::fprintf(stderr,"replay disposition collector: %s\n",why);std::exit(1);}
}
static std::string Read(const std::string &path) {
  std::ifstream in(path);Check(bool(in),"exclusive per-PID ledger exists");
  return std::string(std::istreambuf_iterator<char>(in),std::istreambuf_iterator<char>());
}
static uint64_t Number(const std::string &s,const char *key) {
  std::string k=std::string("\"")+key+"\":";auto p=s.find(k);
  Check(p!=std::string::npos,"required raw scalar exists");
  return std::strtoull(s.c_str()+p+k.size(),nullptr,10);
}
static replay::Event Sample() {
  auto e=replay::Base(7,11,3,2);e.token=1234;e.type=replay::Type::InputAttempt;
  e.completed_before=1;e.has_result=true;e.result=13;e.input_bytes=e.unit_bytes=19;
  return e;
}
struct Fields {uint64_t token=1234,decode_identity=0,submission_ordinal=0,surface=0,owner=0;};
struct Picture {uint32_t picture_number=71,sess_num=9,picture_meta_payload=0x1234,width=16,height=32,flags=48;};
static void GoodReceive() {
  replay::Receive r(replay::Base(7,11,3,2),true);
  r.Output(99,Fields{},Picture{});replay::Inspect("duplicate",true,true,0);
  replay::Field(true);r.Disposition(0);r.Release(0);
}
static int Child(const char *mode,const char *self) {
  if(!std::strcmp(mode,"fork-before-hook")) {
    // No Global()/Record()/Receive invocation has occurred in this exec. The
    // load-time State must already bind the parent, including atfork policy.
    const std::string prefix=std::getenv("CRYSTALHD_TEST_REPLAY_DISPOSITION_PREFIX");
    const pid_t child=fork();Check(child>=0,"fork before first observer hook");
    if(!child) {
      replay::Record(Sample());replay::Global().Publish();
      const std::string path=prefix+"-"+std::to_string(getpid())+".jsonl";
      _exit(access(path.c_str(),F_OK)==-1?0:113);
    }
    int status=0;Check(waitpid(child,&status,0)==child && WIFEXITED(status) && WEXITSTATUS(status)==0,
                      "pre-hook inherited child cannot establish new ownership/publication");
    auto &owner=replay::Global();Check(owner.pid==getpid() && owner.fork_seen && owner.attempted==0,
                                      "load-time parent owner observes fork without child ledger writes");
    owner.Record(Sample());owner.Publish();
    const auto raw=Read(prefix+"-"+std::to_string(getpid())+".jsonl");
    Check(raw.find("\"complete\":true")!=std::string::npos && Number(raw,"events")==1,
          "parent first hook after fork remains a valid parent-only ledger");return 0;
  }
  auto &s=replay::Global();
  const std::string prefix=std::getenv("CRYSTALHD_TEST_REPLAY_DISPOSITION_PREFIX");
  const std::string path=prefix+"-"+std::to_string(getpid())+".jsonl";
  if(!std::strcmp(mode,"disabled")) {
    Check(!s.enabled,"absent exact marker disables collector");s.Record(Sample());s.Publish();
    Check(access(path.c_str(),F_OK)!=0,"disabled collector cannot publish");return 0;
  }
  Check(s.enabled,"exact diagnostic marker enabled");
  bool complete=true;unsigned count=1;
  if(!std::strcmp(mode,"normal")) {
    s.Record(Sample());
    Check(s.events[0].token==1234 && s.events[0].result==13 && s.events[0].completed_before==1,
          "actual scalar inputs/results copied without pointers");
  } else if(!std::strcmp(mode,"errno-no-allocation")) {
    errno=77;forbid_allocations=true;s.Record(Sample());
    Check(errno==77,"record preserves errno without allocation");
    GoodReceive();forbid_allocations=false;
    Check(errno==77,"all receive decisions preserve errno");count=4;
  } else if(!std::strcmp(mode,"capacity")) {
    for(unsigned i=0;i<20;++i) s.Record(Sample());
    Check(s.attempted==20 && s.dropped==4,"fixed capacity retains attempted/drop counts");count=16;complete=false;
  } else if(!std::strcmp(mode,"clock-failure")) {
    fail_clock=true;errno=91;s.Record(Sample());Check(errno==91 && s.clock_failures==1,"clock failure explicit and errno preserved");complete=false;
  } else if(!std::strcmp(mode,"zero-live-owner")) {
    GoodReceive();count=3;
    Check(s.events[0].live_decode_identity==0 && s.events[0].live_submission_ordinal==0 &&
              s.events[0].live_surface==0 && s.events[0].live_owner==0 && s.events[0].native_seq==99,
          "retired live zeros remain zeros while exact native receipt survives");
  } else if(!std::strcmp(mode,"decision-copy")) {
    GoodReceive();count=3;
    Check(s.events[0].inspect==nullptr && std::strcmp(s.events[1].inspect,"duplicate")==0 &&
              s.events[1].field_valid==1 && s.events[1].pair_valid==-1 &&
              s.events[2].receive_id==s.events[0].receive_id && s.events[2].native_seq==99,
          "output is immutable; disposition adds actual decisions for exact lease");
  } else if(!std::strcmp(mode,"missing-release")) {
    {replay::Receive r(replay::Base(7,11,3,2),true);r.Output(99,Fields{},Picture{});r.Disposition(0);}
    Check(s.inflight==0 && s.lifecycle_failures==1,"missing release invalidates ledger after scope retires");count=2;complete=false;
  } else if(!std::strcmp(mode,"duplicate-output")) {
    {replay::Receive r(replay::Base(7,11,3,2),true);r.Output(99,Fields{},Picture{});r.Output(100,Fields{},Picture{});r.Disposition(0);r.Release(0);}
    Check(s.lifecycle_failures==1,"duplicate output cannot silently replace exact receipt");count=3;complete=false;
  } else if(!std::strcmp(mode,"nested-receive")) {
    {replay::Receive r(replay::Base(7,11,3,2),true);r.Output(99,Fields{},Picture{});
      GoodReceive();r.Disposition(0);r.Release(0);}
    Check(s.lifecycle_failures==1,"same-TID nested lease explicitly invalid");count=6;complete=false;
  } else if(!std::strcmp(mode,"inflight")) {
    auto *r=new replay::Receive(replay::Base(7,11,3,2),true);
    r->Output(99,Fields{},Picture{});s.Publish();
    const auto raw=Read(path);Check(raw.find("\"complete\":false")!=std::string::npos && Number(raw,"inflight")==1,"live scope blocks publication qualification");
    r->Disposition(0);r->Release(0);delete r;return 0;
  } else if(!std::strcmp(mode,"threads")) {
    std::thread a([]{for(unsigned i=0;i<3;++i) GoodReceive();});
    std::thread b([]{GoodReceive();});a.join();b.join();count=12;
    Check(s.inflight==0 && s.lifecycle_failures==0,"thread-local independent receive stacks");
  } else if(!std::strcmp(mode,"fork-copy") || !std::strcmp(mode,"fork-exec")) {
    s.Record(Sample());const pid_t child=fork();Check(child>=0,"fork control creates child");
    if(!child) {
      if(!std::strcmp(mode,"fork-exec")) {execl(self,self,"--child","normal",nullptr);_exit(111);}
      s.Record(Sample());s.Publish();
      const std::string childpath=prefix+"-"+std::to_string(getpid())+".jsonl";
      _exit(access(childpath.c_str(),F_OK)==-1?0:112);
    }
    int status=0;Check(waitpid(child,&status,0)==child && WIFEXITED(status) && WEXITSTATUS(status)==0,"child forwards/exits or execs cleanly");
    Check(s.attempted==1 && s.fork_seen,"parent ledger unaffected by inherited child");
    const std::string childpath=prefix+"-"+std::to_string(child)+".jsonl";
    if(!std::strcmp(mode,"fork-exec")) Check(Read(childpath).find("\"complete\":true")!=std::string::npos,"exec owns a fresh complete per-PID ledger");
  } else if(!std::strcmp(mode,"collision")) {
    const int fd=open(path.c_str(),O_WRONLY|O_CREAT|O_EXCL,0600);Check(fd>=0,"prepare existing report");
    Check(write(fd,"ORIGINAL",8)==8,"prepare immutable existing bytes");close(fd);
    s.Record(Sample());s.Publish();Check(Read(path)=="ORIGINAL","exclusive publication preserves existing evidence");return 0;
  } else if(!std::strcmp(mode,"inactive-receive")) {
    {replay::Receive r(replay::Base(7,11,3,2),false);r.Disposition(0);r.Release(0);}
    Check(s.attempted==0 && s.inflight==0,"auxiliary excluded scope does not fabricate output");count=0;
  } else {Check(false,"known control mode");}
  errno=93;s.Publish();Check(errno==93,"publication preserves errno");
  const auto raw=Read(path);
  Check(raw.find(complete?"\"complete\":true":"\"complete\":false")!=std::string::npos,"honest bounded completion result");
  Check(Number(raw,"events")==count,"all original scalar reservations retained");
  struct stat info{};Check(stat(path.c_str(),&info)==0 && (info.st_mode&0777)==0600,"ledger is private mode0600");
  return 0;
}
int main(int argc,char **argv) {
  if(argc==3 && !std::strcmp(argv[1],"--child")) return Child(argv[2],argv[0]);
  char directory[]="/tmp/crystalhd-replay-disposition-XXXXXX";Check(mkdtemp(directory)!=nullptr,"private controls directory");
  const char *modes[]={"normal","errno-no-allocation","capacity","clock-failure","zero-live-owner","decision-copy","missing-release","duplicate-output","nested-receive","inflight","threads","fork-copy","fork-exec","fork-before-hook","collision","inactive-receive","disabled"};
  for(const char *mode:modes) {
    const std::string prefix=std::string(directory)+"/"+mode;
    const pid_t child=fork();Check(child>=0,"separate ledger per control process");
    if(!child) {setenv("CRYSTALHD_TEST_REPLAY_DISPOSITION_PREFIX",prefix.c_str(),1);
      setenv("CRYSTALHD_TEST_REPLAY_DISPOSITION",!std::strcmp(mode,"disabled")?"0":"v1",1);
      execl(argv[0],argv[0],"--child",mode,nullptr);_exit(111);}
    int status=0;Check(waitpid(child,&status,0)==child && WIFEXITED(status) && WEXITSTATUS(status)==0,mode);
    std::printf("replay disposition collector %s: PASS\n",mode);
  }
  std::printf("OWN replay disposition collector: 17 controls PASS; retained %s\n",directory);return 0;
}
