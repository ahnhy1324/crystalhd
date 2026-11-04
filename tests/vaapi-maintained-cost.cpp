// SPDX-License-Identifier: LGPL-2.1-or-later
// CPU-only OWN collector controls. No DIL/device/client/media is executed.
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
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
  if (forbid_allocations.load()) throw std::bad_alloc();
  if (void *value=std::malloc(size?size:1)) return value;
  throw std::bad_alloc();
}
void *operator new[](std::size_t size) {return ::operator new(size);}
// Keep replacement deallocation behind the same non-inlined boundary used by
// the existing owned pixel harness; GCC must not mistake its std::free for a
// mismatched call inside an inlined standard allocator.
__attribute__((noinline)) void operator delete(void *value) noexcept {std::free(value);}
__attribute__((noinline)) void operator delete[](void *value) noexcept {std::free(value);}
__attribute__((noinline)) void operator delete(void *value,std::size_t) noexcept {std::free(value);}
__attribute__((noinline)) void operator delete[](void *value,std::size_t) noexcept {std::free(value);}
static int clock_mode=0;
static thread_local unsigned wall_reads=0,cpu_reads=0;
static int OwnClock(clockid_t clock,timespec *value) {
  if ((clock_mode==1 && clock==CLOCK_MONOTONIC) ||
      (clock_mode==2 && clock==CLOCK_THREAD_CPUTIME_ID)) {errno=EIO;return -1;}
  if (clock_mode>=3) {
    const unsigned nth=clock==CLOCK_MONOTONIC?wall_reads++:cpu_reads++;
    value->tv_sec=0;
    if (clock_mode==3) value->tv_nsec=clock==CLOCK_MONOTONIC?(nth?500:1000):(nth?200:100);
    else if (clock_mode==4) value->tv_nsec=clock==CLOCK_MONOTONIC?(nth?1100:1000):(nth?500:100);
    else value->tv_nsec=clock==CLOCK_MONOTONIC?1000*(nth+1):100*(nth+1);
    errno=99;return 0;
  }
  return clock_gettime(clock,value);
}
#define CRYSTALHD_MAINTAINED_COST_CAPACITY 64
#define CRYSTALHD_MAINTAINED_COST_CLOCK OwnClock
#include "vaapi-maintained-cost-observer.h"
namespace cost=crystalhd_test_maintained_cost;
static void Require(bool condition,const char *why) {
  if (!condition) {std::fprintf(stderr,"maintained-cost control: %s\n",why);std::exit(1);}
}
static cost::Identity FullIdentity(uint64_t context=11) {
  cost::Identity i;i.driver=7;i.context=context;i.generation=2;i.token=1234;
  i.decode=55;i.ordinal=66;i.surface=77;i.owner=77;i.kind=cost::Kind::Full;return i;
}
static bool EarlyReturn() {
  cost::Scope parent(cost::Stage::ProcessOutput,FullIdentity());
  cost::Scope child(cost::Stage::DilAcquire);
  errno=88;return child.Return(false);
}
static std::string Read(const std::string &path) {
  std::ifstream input(path);Require(bool(input),"exclusive per-PID report exists");
  return std::string(std::istreambuf_iterator<char>(input),std::istreambuf_iterator<char>());
}
static uint64_t Number(const std::string &text,const char *name) {
  const std::string needle=std::string("\"")+name+"\":";
  const auto pos=text.find(needle);Require(pos!=std::string::npos,"expected scalar in report");
  return std::strtoull(text.c_str()+pos+needle.size(),nullptr,10);
}
static void VerifyPublished(const std::string &prefix,pid_t pid,bool complete,unsigned expected) {
  const std::string path=prefix+"-"+std::to_string(pid)+".jsonl";
  const std::string report=Read(path);
  Require(report.find("\"schema\":\"crystalhd-maintained-cost-v1\"")!=std::string::npos,"versioned report");
  Require(report.find("\"fork_policy\":\"PARENT_PID_ONLY_CHILD_FORWARD_ONLY\"")!=std::string::npos,"explicit process scope");
  Require(report.find(complete?"\"complete\":true":"\"complete\":false")!=std::string::npos,"honest completion gate");
  Require(Number(report,"events")==expected,"published fixed ready-event count");
  struct stat info{};Require(stat(path.c_str(),&info)==0 && (info.st_mode&0777)==0600,"private report mode");
}
static void RecordOne() {
  const int before=errno;
  cost::Scope scope(cost::Stage::ReceiveOne,FullIdentity());
  Require(errno==before,"entry preserves errno");
  scope.Result(0);errno=88;
}
static int Child(const char *mode,const char *self) {
  Require(cost::state.enabled,"child uses explicit diagnostic marker");
  const std::string prefix=std::getenv("CRYSTALHD_TEST_MAINTAINED_COST_PREFIX");
  const std::string report_path=prefix+"-"+std::to_string(getpid())+".jsonl";
  if (!std::strcmp(mode,"wall-fail")) clock_mode=1;
  if (!std::strcmp(mode,"cpu-fail")) clock_mode=2;
  if (!std::strcmp(mode,"clock-regress")) clock_mode=3;
  if (!std::strcmp(mode,"cpu-exceeds-wall")) clock_mode=4;
  bool complete=true;unsigned count=1;
  if (!std::strcmp(mode,"nested")) {
    clock_mode=5;
    {cost::Scope parent(cost::Stage::ProcessOutput,FullIdentity());
      {cost::Scope child(cost::Stage::FieldConvert);child.Detail(3);child.Result(1);}
      parent.Result(0);}
    const auto &p=cost::state.events[0];const auto &c=cost::state.events[1];
    Require(c.parent==1 && p.parent==0 && c.tid==p.tid,"actual parent/TID, not proximity");
    Require(p.wall==3000 && p.cpu==300 && c.wall==1000 && c.cpu==100,"ordered paired clocks enclose CPU");
    Require(c.phase==cost::Phase::Inherited && c.identity.token==p.identity.token,"same-thread scalar inheritance");
    count=2;
  } else if (!std::strcmp(mode,"normal-no-allocation")) {
    errno=77;forbid_allocations=true;
    {cost::Scope parent(cost::Stage::ReceiveOne,FullIdentity());
      Require(errno==77,"constructor errno");
      const int value=cost::Call(cost::Stage::DilAcquire,{},[]{errno=88;return 13;});
      Require(value==13 && errno==88,"call once, exact result/errno");
      parent.Result(value);}
    forbid_allocations=false;
    Require(errno==88,"destructor errno");count=2;
  } else if (!std::strcmp(mode,"early-return")) {
    errno=77;Require(!EarlyReturn() && errno==88,"early return ownership/errno");
    Require(cost::state.inflight==0,"early-return closes both scopes");count=2;
  } else if (!std::strcmp(mode,"exception")) {
    try {cost::Scope p(cost::Stage::ProcessOutput);cost::Scope c(cost::Stage::DilAcquire);throw 17;}
    catch (int value) {Require(value==17,"original exception preserved");}
    Require(cost::state.inflight==0,"exception unwinds scopes");count=2;
  } else if (!std::strcmp(mode,"identity-refinement")) {
    cost::Identity partial;partial.driver=7;partial.context=11;partial.generation=2;partial.kind=cost::Kind::Decode;
    {cost::Scope p(cost::Stage::ReceiveOne,partial);
      {cost::Scope c(cost::Stage::DilAcquire);}
      p.Bind(FullIdentity(),cost::Phase::Output);}
    Require(cost::state.events[0].identity.kind==cost::Kind::Full,"parent late output resolution");
    Require(cost::state.events[1].identity.kind==cost::Kind::Decode && cost::state.events[1].identity.token==0,
            "earlier child is not retroactively relabelled full");count=2;
  } else if (!std::strcmp(mode,"explicit-surface")) {
    struct Surface {uint64_t trace_context=11,trace_generation=2,expected_timestamp=1234,
      trace_decode_identity=55,trace_submission_ordinal=66;uint32_t id=99,backing_owner=uint32_t(-1);} surface;
    {cost::Scope p(cost::Stage::ReceiveOne,FullIdentity());
      {cost::Scope c(cost::Stage::CanvasCompose,cost::SurfaceIdentity(&surface));}
      surface.trace_context=0;
      {cost::Scope c(cost::Stage::CpuAccessBegin,cost::SurfaceIdentity(&surface),cost::Write,2);}}
    Require(cost::state.events[1].identity.driver==7 && cost::state.events[1].identity.surface==99 &&
        cost::state.events[1].identity.kind==cost::Kind::Full,"explicit surface inherits driver only");
    Require(cost::state.events[2].identity.context==0 && cost::state.events[2].identity.kind==cost::Kind::Surface,
        "missing surface binding cannot borrow parent's context");count=3;
  } else if (!std::strcmp(mode,"bootstrap-trace")) {
    struct Fields {uint64_t context=0,generation=0,token=0,decode_identity=0,
      submission_ordinal=0,surface=41,owner=41;} fields;
    const auto identity=cost::TraceIdentity(7,fields);
    Require(identity.kind==cost::Kind::Surface && identity.context==0 && identity.token==0 &&
        identity.surface==41 && identity.owner==41,"unbound Sync trace is partial surface, not fabricated none/full");
    {cost::Scope scope(cost::Stage::SyncBody);scope.Bind(identity,cost::Phase::Admission);}
    Require(cost::state.events[0].identity.kind==cost::Kind::Surface,"bootstrap scalar identity retained");
  } else if (!std::strcmp(mode,"finish-before-next-operation")) {
    {cost::Scope p(cost::Stage::CanvasCompose,FullIdentity());
      cost::Scope pixels(cost::Stage::CanvasPixels);pixels.Finish();pixels.Finish();
      {cost::Scope capture(cost::Stage::DiagnosticCapture,{},0,1);}}
    Require(cost::state.events[2].parent==1,"capture is sibling after closed pixel scope");
    Require(cost::state.events[1].wall_end<=cost::state.events[2].wall_begin,"pixel span excludes capture");count=3;
  } else if (!std::strcmp(mode,"threads")) {
    std::vector<std::thread> threads;
    for (unsigned i=0;i<4;++i) threads.emplace_back([i]{cost::Scope p(cost::Stage::ReceiveOne,FullIdentity(20+i));cost::Scope c(cost::Stage::DilAcquire);});
    for (auto &thread:threads) thread.join();
    for (unsigned i=0;i<8;++i) {
      const auto &e=cost::state.events[i];
      if (e.parent) Require(cost::state.events[e.parent-1].tid==e.tid,"thread-local actual parent chain");
      else Require(e.stage==cost::Stage::ReceiveOne,"independent thread roots");
    }
    count=8;
  } else if (!std::strcmp(mode,"overflow")) {
    for (unsigned i=0;i<69;++i) RecordOne();
    Require(cost::state.attempted==69 && cost::state.dropped==5,"bounded overflow is explicit");complete=false;count=64;
  } else if (!std::strcmp(mode,"out-of-order")) {
    {cost::Scope p(cost::Stage::ReceiveOne);cost::Scope c(cost::Stage::DilAcquire);p.Finish();}
    Require(cost::state.lifecycle_failures!=0,"LIFO violation invalidates");complete=false;count=2;
  } else if (!std::strcmp(mode,"wrong-thread")) {
    {cost::Scope scope(cost::Stage::ReceiveOne);std::thread other([&]{scope.Finish();});other.join();}
    Require(cost::state.lifecycle_failures!=0,"different endpoint thread invalidates");complete=false;
  } else if (!std::strcmp(mode,"inflight")) {
    {cost::Scope scope(cost::Stage::ReceiveOne);errno=66;cost::state.Publish();Require(errno==66,"publication errno");}
    VerifyPublished(prefix,getpid(),false,0);return 0;
  } else if (!std::strcmp(mode,"existing")) {
    const int fd=open(report_path.c_str(),O_WRONLY|O_CREAT|O_EXCL,0600);Require(fd>=0,"preexisting report control");
    Require(write(fd,"original\n",9)==9 && close(fd)==0,"original report content");RecordOne();cost::state.Publish();
    Require(Read(report_path)=="original\n","existing report never overwritten");return 0;
  } else if (!std::strcmp(mode,"fork-copy") || !std::strcmp(mode,"fork-exec")) {
    RecordOne();const pid_t child=fork();Require(child>=0,"owned fork");
    if (child==0) {
      RecordOne(); // Pre-exec child forwards its OWN work without inherited recording.
      if (!std::strcmp(mode,"fork-exec")) {execl(self,self,"--child","exec-leaf",static_cast<char *>(nullptr));_exit(2);}
      cost::state.Publish();std::exit(0);
    }
    int status=0;Require(waitpid(child,&status,0)==child && WIFEXITED(status) && WEXITSTATUS(status)==0,"owned fork child exit");
    if (!std::strcmp(mode,"fork-exec")) VerifyPublished(prefix,child,true,1);
    else Require(access((prefix+"-"+std::to_string(child)+".jsonl").c_str(),F_OK)!=0,"no inherited child publication");
    Require(cost::state.attempted==1 && cost::state.fork_seen,"parent-only fork ledger");
  } else {
    RecordOne();
    if (clock_mode==4) {
      const auto &event=cost::state.events[0];
      Require(event.wall==100 && event.cpu==400 && cost::state.lifecycle_failures==0 &&
          cost::state.clock_failures==0,"independent raw clock deltas remain valid without clamping");
    } else if (clock_mode) complete=false;
  }
  Require(access(report_path.c_str(),F_OK)!=0,"no per-call publication");
  errno=55;cost::state.Publish();Require(errno==55,"final publication preserves errno");
  VerifyPublished(prefix,getpid(),complete,count);return 0;
}
int main(int argc,char **argv) {
  if (argc==3 && !std::strcmp(argv[1],"--child")) return Child(argv[2],argv[0]);
  char directory[]="/tmp/crystalhd-cost-cpu-XXXXXX";Require(mkdtemp(directory)!=nullptr,"private control directory");
  const char *modes[]={"normal-no-allocation","nested","early-return","exception","identity-refinement", "explicit-surface",
    "bootstrap-trace","finish-before-next-operation","threads","overflow","out-of-order","wrong-thread","inflight","existing",
    "fork-copy","fork-exec","wall-fail","cpu-fail","clock-regress","cpu-exceeds-wall"};
  for (const char *mode:modes) {
    const std::string prefix=std::string(directory)+"/"+mode;
    const pid_t child=fork();Require(child>=0,"control launcher fork");
    if (child==0) {
      Require(setenv("CRYSTALHD_TEST_MAINTAINED_COST","v1",1)==0 &&
          setenv("CRYSTALHD_TEST_MAINTAINED_COST_PREFIX",prefix.c_str(),1)==0,"control environment");
      execl(argv[0],argv[0],"--child",mode,static_cast<char *>(nullptr));_exit(2);
    }
    int status=0;Require(waitpid(child,&status,0)==child && WIFEXITED(status) && WEXITSTATUS(status)==0,mode);
  }
  std::printf("maintained-cost: %zu OWN CPU controls passed; no hardware/media\n",sizeof(modes)/sizeof(modes[0]));
  return 0;
}
