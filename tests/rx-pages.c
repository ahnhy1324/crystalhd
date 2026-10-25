/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Extract the real callbacks. Two mappings of one memfd model the registered
 * user VA and the pinned-page kernel view. Child monitors unmap or replace
 * only their own user VA; the owner's physical backing remains registered.
 * No device, kernel module, DMA or external application is modified. */
#define _GNU_SOURCE
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <unistd.h>
#include "bc_dts_types.h"
struct _BC_DTS_PROC_OUT;
#include "bc_dts_defs.h"
#define PAGE_SHIFT 12
#define PAGE_SIZE (1UL << PAGE_SHIFT)
#define __user
#define container_of(p,t,m) ((t *)((char *)(p)-offsetof(t,m)))
#define offset_in_page(p) ((uintptr_t)(p)&(PAGE_SIZE-1))
#define min_t(t,a,b) ((t)(a)<(t)(b)?(t)(a):(t)(b))
typedef uint64_t u64;
struct page { char *data; };
struct crystalhd_rx_buffer { uint32_t capacity; void *cookie; };
enum { crystalhd_dio_inv, crystalhd_dio_locked, crystalhd_dio_sg_mapped };
struct crystalhd_dio_req {
 int sig, page_cnt; struct page **pages;
 struct { void *xfr_buff; uint32_t xfr_len; bool dir_tx; } uinfo;
 struct crystalhd_rx_buffer rx_buffer;
};
static unsigned mappings, checks, failures;
static inline void memcpy_from_page(char *to, struct page *page,
                                    size_t offset, size_t length)
{ assert(offset<=PAGE_SIZE && length<=PAGE_SIZE-offset); mappings++; memcpy(to,page->data+offset,length); }
static inline void memcpy_to_page(struct page *page, size_t offset,
                                  const char *from, size_t length)
{ assert(offset<=PAGE_SIZE && length<=PAGE_SIZE-offset); mappings++; memcpy(page->data+offset,from,length); }
/* Old callbacks resolve the caller's actual mapping, just as uaccess does.
 * process_vm_* on this process returns EFAULT instead of crashing the test. */
static inline size_t copy_from_user(void *to, const void *from, size_t length)
{
 struct iovec local={to,length}, remote={(void *)from,length};
 ssize_t n=process_vm_readv(getpid(),&local,1,&remote,1,0);
 return n<0?length:length-(size_t)n;
}
static inline size_t copy_to_user(void *to, const void *from, size_t length)
{
 struct iovec local={(void *)from,length}, remote={to,length};
 ssize_t n=process_vm_writev(getpid(),&local,1,&remote,1,0);
 return n<0?length:length-(size_t)n;
}
#include "callbacks.h"
#define CHECK(x) do { checks++; if(!(x)) { failures++; fprintf(stderr,"FAIL line %u: %s\n",__LINE__,#x); } } while(0)
static void fill(char *p, size_t n) { for(size_t i=0;i<n;i++)p[i]=(char)((i*37+11)&255); }
int main(void)
{
 const size_t bytes=2*PAGE_SIZE, prefix=13, capacity=2*PAGE_SIZE-22;
 int fd=memfd_create("crystalhd-rx-pages",MFD_CLOEXEC);assert(fd>=0 && ftruncate(fd,(off_t)bytes)==0);
 char *user=mmap(NULL,bytes,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);
 char *kernel=mmap(NULL,bytes,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);
 assert(user!=MAP_FAILED && kernel!=MAP_FAILED && user!=kernel);
 struct page page[2]={{kernel},{kernel+PAGE_SIZE}};
 struct page *pages[2]={&page[0],&page[1]};
 struct crystalhd_dio_req dio={.sig=crystalhd_dio_sg_mapped,.page_cnt=2,.pages=pages,
  .uinfo={user+prefix,(uint32_t)capacity,false},.rx_buffer={(uint32_t)capacity,NULL}};
 dio.rx_buffer.cookie=&dio;
 char output[2*PAGE_SIZE], input[2*PAGE_SIZE], expected[2*PAGE_SIZE];
 const struct { uint32_t offset; size_t length; } ranges[]={
  {0,0},{0,1},{0,144},{PAGE_SIZE-13-4,16},{PAGE_SIZE-13,512},
  {(uint32_t)capacity-1,1},{(uint32_t)capacity,0},{0,capacity}};
 for(size_t i=0;i<sizeof(ranges)/sizeof(ranges[0]);i++) {
  fill(kernel,bytes);memset(output,0,sizeof(output));
  uint32_t off=ranges[i].offset;size_t len=ranges[i].length;
  CHECK(crystalhd_dio_rx_read(&dio.rx_buffer,off,output,len)==BC_STS_SUCCESS);
  CHECK(!memcmp(output,kernel+prefix+off,len));
  memset(input,0x5a,sizeof(input));memcpy(expected,kernel,bytes);memcpy(expected+prefix+off,input,len);
  CHECK(crystalhd_dio_rx_write(&dio.rx_buffer,off,input,len)==BC_STS_SUCCESS);
  CHECK(!memcmp(kernel,expected,bytes));
 }
 /* Every invalid request must reject before any page is mapped or written. */
 fill(kernel,bytes);memcpy(expected,kernel,bytes);mappings=0;
 CHECK(crystalhd_dio_rx_read(&dio.rx_buffer,(uint32_t)capacity+1,output,0)==BC_STS_INV_ARG);
 CHECK(crystalhd_dio_rx_write(&dio.rx_buffer,(uint32_t)capacity-1,input,2)==BC_STS_INV_ARG);
 CHECK(crystalhd_dio_rx_write(&dio.rx_buffer,0,input,SIZE_MAX)==BC_STS_INV_ARG);
 dio.rx_buffer.cookie=NULL;CHECK(crystalhd_dio_rx_read(&dio.rx_buffer,0,output,1)==BC_STS_INV_ARG);dio.rx_buffer.cookie=&dio;
 dio.uinfo.dir_tx=true;CHECK(crystalhd_dio_rx_write(&dio.rx_buffer,0,input,1)==BC_STS_INV_ARG);dio.uinfo.dir_tx=false;
 dio.sig=crystalhd_dio_inv;CHECK(crystalhd_dio_rx_read(&dio.rx_buffer,0,output,1)==BC_STS_INV_ARG);dio.sig=crystalhd_dio_sg_mapped;
 dio.page_cnt=1;CHECK(crystalhd_dio_rx_write(&dio.rx_buffer,0,input,capacity)==BC_STS_INV_ARG);dio.page_cnt=2;
 pages[1]=NULL;CHECK(crystalhd_dio_rx_write(&dio.rx_buffer,0,input,capacity)==BC_STS_INV_ARG);pages[1]=&page[1];
 dio.uinfo.xfr_len--;CHECK(crystalhd_dio_rx_read(&dio.rx_buffer,0,output,1)==BC_STS_INV_ARG);dio.uinfo.xfr_len++;
 CHECK(!memcmp(kernel,expected,bytes));CHECK(mappings==0);
 /* Real child address spaces: absent VA, then unrelated bytes at the same VA.
  * Neither unmaps nor replaces the parent's registered mapping. */
 for(unsigned replace=0;replace<2;replace++) {
  fill(kernel,bytes);fflush(NULL);pid_t child=fork();assert(child>=0);
  if(child==0) {
   assert(munmap(user,bytes)==0);
   if(replace) { assert(mmap(user,bytes,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED,-1,0)==user);memset(user,0xee,bytes); }
   size_t off=PAGE_SIZE-prefix-4;
   if(crystalhd_dio_rx_read(&dio.rx_buffer,(uint32_t)off,output,16)!=BC_STS_SUCCESS || memcmp(output,kernel+prefix+off,16))_exit(1);
   memset(input,0x7b,16);
   if(crystalhd_dio_rx_write(&dio.rx_buffer,(uint32_t)off,input,16)!=BC_STS_SUCCESS || memcmp(kernel+prefix+off,input,16))_exit(2);
   if(replace)for(size_t i=0;i<bytes;i++)if((unsigned char)user[i]!=0xee)_exit(3);
   _exit(0);
  }
  int status;assert(waitpid(child,&status,0)==child);CHECK(WIFEXITED(status)&&WEXITSTATUS(status)==0);
  memset(input,0x7b,16);CHECK(!memcmp(user+PAGE_SIZE-4,input,16));
 }
 CHECK(munmap(user,bytes)==0);CHECK(munmap(kernel,bytes)==0);CHECK(close(fd)==0);
 printf("RX page callbacks: %u checks, %u failures\n",checks,failures);
 return failures?1:0;
}
