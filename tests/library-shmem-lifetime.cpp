// SPDX-License-Identifier: LGPL-2.1-or-later
// Process-local SysV mapping lifetime; no real segment or device is opened.
#include "7411d.h"
#include "libcrystalhd_priv.h"
#include <atomic>
#include <cerrno>
#include <cstdarg>
#include <climits>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <pthread.h>
#include <sched.h>
#include <sys/resource.h>
#include <sys/shm.h>
#include <sys/wait.h>
#include <unistd.h>

extern bc_dil_glob_s *bc_dil_glob_ptr;

namespace {
enum Scenario {
 Basic, CreateReuse, Mismatch, DetachFailure, PeerAttached, StatGone, StatFailure,
 RemoveGone, RemoveFailure, Concurrent, ForkLocked, ForkBeforeRegistration,
 ForkAfterRegistration, AtForkFailure
};

Scenario scenario;
bc_dil_glob_s shared_state;
bc_dil_glob_s second_mapping;
std::atomic<unsigned> shmget_calls(0), shmat_calls(0), shmdt_calls(0);
std::atomic<unsigned> stat_calls(0), remove_calls(0), atfork_calls(0);
std::atomic<bool> shmat_entered(false), release_shmat(false);
std::atomic<bool> prepare_entered(false), worker_done(false);
std::atomic<bool> atfork_call_entered(false), atfork_registered(false);
std::atomic<bool> release_atfork_call(false);
void (*registered_prepare)(void);
void (*registered_parent)(void);
void (*registered_child)(void);
pthread_mutex_t block_lock=PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t block_cond=PTHREAD_COND_INITIALIZER;
unsigned checks, failures;

const char *ScenarioName(Scenario value)
{
 static const char *const names[]={
  "basic", "create-reuse", "mismatch", "detach-failure", "peer-attached",
  "stat-gone", "stat-failure", "remove-gone", "remove-failure", "concurrent",
  "fork-locked", "fork-before-registration", "fork-after-registration",
  "atfork-failure"
 };
 return names[value];
}

void Check(bool ok, const char *message)
{
 ++checks;
 if (!ok) { ++failures; fprintf(stderr,"FAIL: %s\n",message); }
}

void ChildCheck(bool ok)
{
 if (!ok) _exit(80);
}

void ResetChildState()
{
 memset(&shared_state,0,sizeof(shared_state));
 memset(&second_mapping,0,sizeof(second_mapping));
 shmget_calls=shmat_calls=shmdt_calls=stat_calls=remove_calls=atfork_calls=0;
 shmat_entered=release_shmat=prepare_entered=worker_done=false;
 atfork_call_entered=atfork_registered=release_atfork_call=false;
 registered_prepare=registered_parent=registered_child=NULL;
}

void FinishLastReference(BC_STATUS expected=BC_STS_SUCCESS)
{
 ChildCheck(DtsDelDilShMem()==expected);
 ChildCheck(bc_dil_glob_ptr==NULL);
}

void BasicCase()
{
 ChildCheck(DtsGetDilShMem(17)==BC_STS_SUCCESS);
 bc_dil_glob_ptr->DevID=0x1612;
 ChildCheck(DtsGetDilShMem(17)==BC_STS_SUCCESS);
 ChildCheck(shmat_calls==1 && bc_dil_glob_ptr==&shared_state);
 ChildCheck(DtsDelDilShMem()==BC_STS_SUCCESS);
 ChildCheck(shmdt_calls==0 && bc_dil_glob_ptr->DevID==0x1612);
 FinishLastReference();
 ChildCheck(shmdt_calls==1 && stat_calls==1 && remove_calls==1);
}

void CreateReuseCase()
{
 ChildCheck(DtsGetDilShMem(17)==BC_STS_SUCCESS);
 bc_dil_glob_ptr->gDilOpMode=4;
 int id=-2;
 ChildCheck(DtsCreateShMem(&id)==BC_STS_SUCCESS && id==17);
 ChildCheck(shmget_calls==0 && shmat_calls==1 && bc_dil_glob_ptr->gDilOpMode==4);
 ChildCheck(DtsDelDilShMem()==BC_STS_SUCCESS && shmdt_calls==0);
 ChildCheck(bc_dil_glob_ptr==&shared_state && bc_dil_glob_ptr->gDilOpMode==4);
 FinishLastReference();
}

void MismatchCase()
{
 ChildCheck(DtsGetDilShMem(17)==BC_STS_SUCCESS);
 bc_dil_glob_ptr->DevID=0x1615;
 errno=0;
 ChildCheck(DtsGetDilShMem(18)==BC_STS_ERROR && errno==EBUSY);
 errno=0;
 ChildCheck(DtsGetDilShMem(UINT_MAX)==BC_STS_ERROR && errno==EINVAL);
 ChildCheck(shmat_calls==1 && bc_dil_glob_ptr==&shared_state &&
            bc_dil_glob_ptr->DevID==0x1615);
 FinishLastReference();
}

void DetachFailureCase()
{
 ChildCheck(DtsGetDilShMem(17)==BC_STS_SUCCESS);
 errno=0;
 ChildCheck(DtsDelDilShMem()==BC_STS_ERROR && errno==EIO);
 ChildCheck(shmdt_calls==1 && bc_dil_glob_ptr==&shared_state);
 ChildCheck(DtsGetDilShMem(17)==BC_STS_SUCCESS && shmat_calls==1);
 ChildCheck(DtsDelDilShMem()==BC_STS_SUCCESS && shmdt_calls==1);
 ChildCheck(DtsDelDilShMem()==BC_STS_SUCCESS);
 ChildCheck(shmdt_calls==2 && bc_dil_glob_ptr==NULL);
}

void CleanupCase()
{
 ChildCheck(DtsGetDilShMem(17)==BC_STS_SUCCESS);
 const bool error=scenario==StatFailure || scenario==RemoveFailure;
 errno=0;
 FinishLastReference(error ? BC_STS_ERROR : BC_STS_SUCCESS);
 if(error) ChildCheck(errno==EIO);
 ChildCheck(shmdt_calls==1 && stat_calls==1);
 ChildCheck(remove_calls==((scenario==PeerAttached || scenario==StatGone ||
                            scenario==StatFailure) ? 0U : 1U));
 ChildCheck(shmget_calls==0);
 errno=0;
 ChildCheck(DtsDelDilShMem()==BC_STS_ERROR && errno==EINVAL);
 if(error) {
  ChildCheck(DtsGetDilShMem(17)==BC_STS_SUCCESS && shmat_calls==2 &&
             bc_dil_glob_ptr==&second_mapping);
  FinishLastReference(BC_STS_ERROR);
 }
}

void *AcquireThread(void *)
{
 return reinterpret_cast<void *>(static_cast<intptr_t>(DtsGetDilShMem(17)));
}

void *ReleaseThread(void *)
{
 return reinterpret_cast<void *>(static_cast<intptr_t>(DtsDelDilShMem()));
}

void ConcurrentCase()
{
 const unsigned count=12;
 pthread_t threads[count];
 for(unsigned i=0;i<count;i++)
  ChildCheck(pthread_create(&threads[i],NULL,AcquireThread,NULL)==0);
 for(unsigned i=0;i<count;i++) {
  void *result=NULL;
  ChildCheck(pthread_join(threads[i],&result)==0 &&
             reinterpret_cast<intptr_t>(result)==BC_STS_SUCCESS);
 }
 ChildCheck(shmat_calls==1 && bc_dil_glob_ptr==&shared_state);
 bc_dil_glob_ptr->DevID=0x1615;
 for(unsigned i=0;i<count;i++)
  ChildCheck(pthread_create(&threads[i],NULL,ReleaseThread,NULL)==0);
 for(unsigned i=0;i<count;i++) {
  void *result=NULL;
  ChildCheck(pthread_join(threads[i],&result)==0 &&
             reinterpret_cast<intptr_t>(result)==BC_STS_SUCCESS);
 }
 ChildCheck(shmdt_calls==1 && stat_calls==1 && remove_calls==1 &&
            bc_dil_glob_ptr==NULL);
}

void *BlockingAcquire(void *)
{
 const BC_STATUS status=DtsGetDilShMem(17);
 worker_done=true;
 return reinterpret_cast<void *>(static_cast<intptr_t>(status));
}

void *ReleaseBlockedShmat(void *)
{
 while(!prepare_entered.load()) sched_yield();
 ChildCheck(pthread_mutex_lock(&block_lock)==0);
 release_shmat=true;
 ChildCheck(pthread_cond_broadcast(&block_cond)==0);
 ChildCheck(pthread_mutex_unlock(&block_lock)==0);
 return NULL;
}

void ForkLockedCase()
{
 pthread_t worker, releaser;
 ChildCheck(pthread_create(&worker,NULL,BlockingAcquire,NULL)==0);
 ChildCheck(pthread_mutex_lock(&block_lock)==0);
 while(!shmat_entered.load())
  ChildCheck(pthread_cond_wait(&block_cond,&block_lock)==0);
 ChildCheck(pthread_mutex_unlock(&block_lock)==0);
 ChildCheck(pthread_create(&releaser,NULL,ReleaseBlockedShmat,NULL)==0);
 const pid_t child=fork();
 ChildCheck(child>=0);
 if(child==0) {
  alarm(3);
  ChildCheck(bc_dil_glob_ptr==&shared_state && shmat_calls==1 && shmdt_calls==0);
  ChildCheck(DtsGetDilShMem(17)==BC_STS_SUCCESS);
  ChildCheck(DtsDelDilShMem()==BC_STS_SUCCESS);
  ChildCheck(bc_dil_glob_ptr==&shared_state && shmat_calls==1 && shmdt_calls==0);
  _exit(0);
 }
 void *result=NULL;
 ChildCheck(pthread_join(worker,&result)==0 &&
            reinterpret_cast<intptr_t>(result)==BC_STS_SUCCESS);
 ChildCheck(worker_done.load());
 ChildCheck(pthread_join(releaser,NULL)==0);
 int status=0;
 ChildCheck(waitpid(child,&status,0)==child && WIFEXITED(status) && WEXITSTATUS(status)==0);
 ChildCheck(DtsDelDilShMem()==BC_STS_SUCCESS && bc_dil_glob_ptr==NULL);
}

void ForkBeforeRegistrationCase()
{
 pthread_t worker;
 ChildCheck(pthread_create(&worker,NULL,BlockingAcquire,NULL)==0);
 while(!atfork_call_entered.load()) sched_yield();
 ChildCheck(atfork_calls==1 && shmat_calls==0 && bc_dil_glob_ptr==NULL);
 const pid_t child=fork();
 ChildCheck(child>=0);
 if(child==0) {
  alarm(3);
  ChildCheck(atfork_calls==1 && bc_dil_glob_ptr==NULL && shmat_calls==0);
  ChildCheck(DtsGetDilShMem(17)==BC_STS_SUCCESS && atfork_calls==2 &&
             bc_dil_glob_ptr==&shared_state && shmat_calls==1);
  FinishLastReference();
  _exit(0);
 }
 release_atfork_call=true;
 void *result=NULL;
 ChildCheck(pthread_join(worker,&result)==0 &&
            reinterpret_cast<intptr_t>(result)==BC_STS_SUCCESS);
 ChildCheck(worker_done.load());
 int status=0;
 ChildCheck(waitpid(child,&status,0)==child && WIFEXITED(status) &&
            WEXITSTATUS(status)==0);
 ChildCheck(DtsDelDilShMem()==BC_STS_SUCCESS && bc_dil_glob_ptr==NULL &&
            shmdt_calls==1);
}

void ForkAfterRegistrationCase()
{
 pthread_t worker;
 ChildCheck(pthread_create(&worker,NULL,BlockingAcquire,NULL)==0);
 while(!atfork_registered.load()) sched_yield();
 ChildCheck(atfork_calls==1 && shmat_calls==0 && bc_dil_glob_ptr==NULL);
 const pid_t child=fork();
 ChildCheck(child>=0);
 if(child==0) {
  alarm(3);
  ChildCheck(atfork_calls==1 && bc_dil_glob_ptr==NULL && shmat_calls==0);
  ChildCheck(DtsGetDilShMem(17)==BC_STS_SUCCESS && atfork_calls==1 &&
             bc_dil_glob_ptr==&shared_state && shmat_calls==1);
  FinishLastReference();
  _exit(0);
 }
 void *result=NULL;
 ChildCheck(pthread_join(worker,&result)==0 &&
            reinterpret_cast<intptr_t>(result)==BC_STS_SUCCESS);
 ChildCheck(worker_done.load());
 int status=0;
 ChildCheck(waitpid(child,&status,0)==child && WIFEXITED(status) &&
            WEXITSTATUS(status)==0);
 ChildCheck(DtsDelDilShMem()==BC_STS_SUCCESS && bc_dil_glob_ptr==NULL &&
            shmdt_calls==1);
}

void AtForkFailureCase()
{
 errno=0;
 ChildCheck(DtsGetDilShMem(17)==BC_STS_ERROR && errno==EAGAIN);
 ChildCheck(atfork_calls==1 && shmat_calls==0 && bc_dil_glob_ptr==NULL);
 int id=-2;
 ChildCheck(DtsCreateShMem(&id)==BC_STS_ERROR && id==-1);
 ChildCheck(atfork_calls==1 && shmget_calls==0 && shmat_calls==0 &&
            bc_dil_glob_ptr==NULL);
}

void RunScenario()
{
 ResetChildState();
 alarm(8);
 switch(scenario) {
 case Basic: BasicCase(); break;
 case CreateReuse: CreateReuseCase(); break;
 case Mismatch: MismatchCase(); break;
 case DetachFailure: DetachFailureCase(); break;
 case PeerAttached: case StatGone: case StatFailure: case RemoveGone: case RemoveFailure:
  CleanupCase(); break;
 case Concurrent: ConcurrentCase(); break;
 case ForkLocked: ForkLockedCase(); break;
 case ForkBeforeRegistration: ForkBeforeRegistrationCase(); break;
 case ForkAfterRegistration: ForkAfterRegistrationCase(); break;
 case AtForkFailure: AtForkFailureCase(); break;
 }
}

void WrappedAtForkPrepare()
{
 if(scenario==ForkLocked)
  ChildCheck(shmat_entered.load() && !release_shmat.load());
 else {
  ChildCheck(scenario==ForkAfterRegistration && atfork_registered.load() &&
             !release_atfork_call.load() && shmat_calls==0);
 }
 prepare_entered=true;
 registered_prepare();
}

void WrappedAtForkParent()
{
 registered_parent();
 if(scenario==ForkAfterRegistration)
  release_atfork_call=true;
}

void WrappedAtForkChild()
{
 registered_child();
}
}

extern "C" int __wrap_shmget(key_t, size_t, int)
{
 ++shmget_calls;
 _exit(81);
}

extern "C" void *__wrap_shmat(int id, const void *address, int flags)
{
 const unsigned call=++shmat_calls;
 ChildCheck(id==17 && address==NULL && flags==0);
 if(scenario==ForkLocked) {
  ChildCheck(pthread_mutex_lock(&block_lock)==0);
  shmat_entered=true;
  ChildCheck(pthread_cond_broadcast(&block_cond)==0);
  while(!release_shmat.load())
   ChildCheck(pthread_cond_wait(&block_cond,&block_lock)==0);
  ChildCheck(pthread_mutex_unlock(&block_lock)==0);
 }
 return call==1 ? static_cast<void *>(&shared_state) : static_cast<void *>(&second_mapping);
}

extern "C" int __wrap_shmdt(const void *address)
{
 const unsigned call=++shmdt_calls;
 const void *expected=(scenario==DetachFailure || call==1) ?
                      static_cast<const void *>(&shared_state) :
                      static_cast<const void *>(&second_mapping);
 ChildCheck(address==expected);
 if(scenario==DetachFailure && call==1) { errno=EIO; return -1; }
 return 0;
}

extern "C" int __wrap_shmctl(int id, int command, struct shmid_ds *buffer)
{
 ChildCheck(id==17);
 if(command==IPC_STAT) {
  ++stat_calls;
  if(scenario==StatGone) { errno=EINVAL; return -1; }
  if(scenario==StatFailure) { errno=EIO; return -1; }
  ChildCheck(buffer!=NULL);
  memset(buffer,0,sizeof(*buffer));
  buffer->shm_nattch=scenario==PeerAttached ? 1 : 0;
  return 0;
 }
 ChildCheck(command==IPC_RMID && buffer==NULL);
 ++remove_calls;
 if(scenario==RemoveGone) { errno=EIDRM; return -1; }
 if(scenario==RemoveFailure) { errno=EIO; return -1; }
 return 0;
}

extern "C" int __real_pthread_atfork(void (*)(void), void (*)(void), void (*)(void));
extern "C" int __wrap_pthread_atfork(void (*prepare)(void), void (*parent)(void),
                                      void (*child)(void))
{
 const unsigned call=++atfork_calls;
 if(scenario==AtForkFailure) return EAGAIN;
 if(scenario==ForkBeforeRegistration) {
  if(call==1) {
   atfork_call_entered=true;
   while(!release_atfork_call.load()) sched_yield();
  }
  return __real_pthread_atfork(prepare,parent,child);
 }
 if(scenario==ForkLocked || scenario==ForkAfterRegistration) {
  ChildCheck(call==1);
  registered_prepare=prepare;
  registered_parent=parent;
  registered_child=child;
  const int result=__real_pthread_atfork(WrappedAtForkPrepare,
                                         WrappedAtForkParent,
                                         WrappedAtForkChild);
  if(scenario==ForkAfterRegistration && result==0) {
   atfork_registered=true;
   while(!release_atfork_call.load()) sched_yield();
  }
  return result;
 }
 return __real_pthread_atfork(prepare,parent,child);
}

extern "C" int __wrap_fprintf(FILE *stream, const char *format, ...)
{
 va_list args;
 va_start(args,format);
 const int result=vfprintf(stream,format,args);
 va_end(args);
 if(strstr(format,"detach") || strstr(format,"shmctl") || strstr(format,"delete"))
  errno=ENOSPC;
 return result;
}

int main()
{
 const rlimit limit={0,0};
 if(setrlimit(RLIMIT_CORE,&limit)!=0) return 2;
 for(Scenario item : {Basic,CreateReuse,Mismatch,DetachFailure,PeerAttached,StatGone,
                      StatFailure,RemoveGone,RemoveFailure,Concurrent,
                      ForkLocked,ForkBeforeRegistration,ForkAfterRegistration,
                      AtForkFailure}) {
  scenario=item;
  const pid_t child=fork();
  if(child<0) return 2;
  if(child==0) { RunScenario(); _exit(0); }
  int status=0;
  if(waitpid(child,&status,0)!=child) return 2;
  Check(WIFEXITED(status) && WEXITSTATUS(status)==0,ScenarioName(item));
 }
 printf("library shmem lifetime: %u checks, %u failures\n",checks,failures);
 return failures ? 1 : 0;
}
