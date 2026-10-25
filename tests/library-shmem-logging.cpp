// SPDX-License-Identifier: LGPL-2.1-or-later
// Actual DIL attachment/error paths; no real SysV segment or device is opened.
#include "7411d.h"
#include "libcrystalhd_priv.h"
#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <csignal>
#include <string>
#include <initializer_list>
#include <sys/resource.h>
#include <sys/shm.h>
#include <sys/wait.h>
#include <unistd.h>

extern bc_dil_glob_s *bc_dil_glob_ptr;
namespace {
enum Path { Direct, New, Existing, Stale };
Path selected;
bool fail_attach;
unsigned get_calls, stats, removes, attaches;
bc_dil_glob_s shared;
unsigned checks, failures;
void Check(bool ok, const char *message) {
 ++checks;
 if (!ok) { ++failures; fprintf(stderr,"FAIL: %s\n",message); }
}
void ChildCheck(bool ok) { if (!ok) _exit(80); }
void ExpectedFault(int signal) { _exit(signal==SIGSEGV ? 87 : 88); }
}
extern "C" int __wrap_shmget(key_t key, size_t size, int flags) {
 ChildCheck(key==static_cast<key_t>(BC_DIL_SHMEM_KEY) && size==1024);
 ++get_calls;
 if (selected!=New && get_calls==1) { errno=EEXIST; return -1; }
 ChildCheck(flags==(0644|((selected==New || get_calls==3) ? IPC_CREAT|IPC_EXCL : 0)));
 return 17;
}
extern "C" int __wrap_shmctl(int id, int command, struct shmid_ds *buffer) {
 ChildCheck(id==17);
 if (command==IPC_STAT) { ++stats; memset(buffer,0,sizeof(*buffer)); buffer->shm_nattch=selected==Stale ? 0 : 2; return 0; }
 ChildCheck(command==IPC_RMID && selected==Stale); ++removes; return 0;
}
extern "C" void *__wrap_shmat(int id, const void *address, int flags) {
 ChildCheck(id==17 && !address && !flags && ++attaches==1);
 ChildCheck(get_calls==(selected==Direct ? 0U : selected==New ? 1U : selected==Stale ? 3U : 2U));
 ChildCheck(stats==(selected==Existing || selected==Stale ? 1U : 0U));
 ChildCheck(removes==(selected==Stale ? 1U : 0U));
 if (fail_attach) { errno=ENOMEM; return reinterpret_cast<void *>(-1); }
 shared.gDilOpMode=1; return &shared;
}
extern "C" int __wrap_fprintf(FILE *stream, const char *format, ...) {
 va_list args; va_start(args,format); const int result=vfprintf(stream,format,args); va_end(args);
 // A diagnostic sink is permitted to clobber errno even when writing succeeds.
 if (strstr(format,"shared memory") || strstr(format,"Shared memory")) errno=EIO;
 return result;
}
int main() {
 for (Path path : {Direct,New,Existing,Stale}) for (bool failure : {false,true}) {
  selected=path; fail_attach=failure;
  int descriptors[2]; if (pipe(descriptors)!=0) return 2;
  const pid_t child=fork(); if (child<0) return 2;
  if (child==0) {
   close(descriptors[0]); if (dup2(descriptors[1],STDERR_FILENO)<0) _exit(81); close(descriptors[1]);
   const rlimit limit={0,0}; if (setrlimit(RLIMIT_CORE,&limit)!=0) _exit(81);
   alarm(3);
   if (path==Existing && failure) {
    struct sigaction action={}; action.sa_handler=ExpectedFault; sigemptyset(&action.sa_mask);
    if (sigaction(SIGSEGV,&action,nullptr)!=0) _exit(81);
   }
   errno=EALREADY; int id=-2;
   const BC_STATUS status=path==Direct ? DtsGetDilShMem(17) : DtsCreateShMem(&id);
   // The existing-segment caller still dereferences the failure sentinel.
   ChildCheck(!(path==Existing && failure));
   ChildCheck(status==(path==Direct && failure ? BC_STS_ERROR : BC_STS_SUCCESS));
   ChildCheck(path==Direct || id==17);
   ChildCheck(bc_dil_glob_ptr==(failure ? reinterpret_cast<bc_dil_glob_s *>(-1) : &shared));
   ChildCheck(errno==(failure ? ENOMEM : path==Direct || path==New ? EALREADY : EEXIST));
   _exit(0);
  }
  close(descriptors[1]); std::string log; char bytes[512]; ssize_t size;
  while ((size=read(descriptors[0],bytes,sizeof(bytes)))>0) log.append(bytes,size);
  Check(size==0,"diagnostic pipe reaches EOF"); close(descriptors[0]);
  int status; if (waitpid(child,&status,0)!=child) return 2;
  Check(WIFEXITED(status) && WEXITSTATUS(status)==(path==Existing && failure ? 87 : 0),
        "legacy status/sentinel/errno/syscall count and existing-segment fault are preserved");
#ifdef CRYSTALHD_TEST_DIL_LOG_DISABLED
  Check(log.find("shared memory")==std::string::npos && log.find("Shared memory")==std::string::npos,
        "compiled-out diagnostic sink emits no attachment message");
#else
  const std::string attach="Unable to open shared memory: stage=shmat errno="+std::to_string(ENOMEM)+"\n";
  const char *paths[]={"","new","existing","stale-recreated"};
  const std::string context="Shared memory attach failed: path="+std::string(paths[path])+" errno="+std::to_string(ENOMEM)+"\n";
  Check((log.find(attach)!=std::string::npos)==failure,"attachment stage and saved errno are retained");
  Check((log.find(context)!=std::string::npos)==(failure && path!=Direct),
        "new/existing/recreated context is retained, including before the child fault");
  Check(log.find("0x")==std::string::npos && log.find("shmid")==std::string::npos,
        "diagnostics omit addresses, segment identifiers and content");
#endif
 }
 printf("library shmem diagnostics: %u checks, %u failures\n",checks,failures);
 return failures ? 1 : 0;
}
