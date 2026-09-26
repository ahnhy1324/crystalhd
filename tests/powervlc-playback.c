/* SPDX-License-Identifier: LGPL-2.1-or-later */
#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#include <vlc/vlc.h>
#include <dlfcn.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>

/* Opt-in 640x360/30fps generated barcode probe; callbacks are NOT a window.
 * Run under an external timeout: libvlc stop/close can block in a driver.
 * Tokens and planes belong to individual lock calls, never a shared plane.
 * Discarded/unshown tokens are reclaimed only after player teardown. */
enum { W=640, H=360, PITCH=W*4, BYTES=PITCH*384, FRAMES=360 };
typedef struct Picture { struct Picture *next; uint8_t *pixels; unsigned id;
  uint64_t hash; int ready; } Picture;
typedef struct {
  pthread_mutex_t mutex; Picture *pictures, *last; unsigned allocated;
  unsigned seen[FRAMES], callbacks, unique, duplicates, regressions, first, id;
  unsigned phase_first, phase_last, advances, prefix; int target, armed, paused;
  int failed, controls; uint64_t paused_hash; unsigned paused_id;
  double last_move, phase_start, phase_end, deadline;
} Audit;
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t);
  return (double)t.tv_sec + (double)t.tv_nsec/1e9; }
static void tick(void) { const struct timespec t={0,20000000}; nanosleep(&t,NULL); }
static void fail(Audit *a, const char *why) {
  if (!a->failed) fprintf(stderr,"FAIL: %s\n",why);
  a->failed=1;
}
static uint64_t fingerprint(const uint8_t *p) {
  uint64_t h=1469598103934665603ULL;
  for (size_t i=0;i<(size_t)PITCH*H;++i) h=(h^p[i])*1099511628211ULL;
  return h;
}
static unsigned light(const uint8_t *p, unsigned x) {
  p += 16*PITCH+x*4; return ((unsigned)p[0]+p[1]+p[2])/3;
}
static int barcode(Picture *p) {
  p->id=0;
  for (unsigned bit=0;bit<9;++bit) if(light(p->pixels,16+24*bit)>128) p->id|=1U<<bit;
  return p->id<FRAMES && light(p->pixels,248)>=200 && light(p->pixels,272)<=50;
}
static void release_picture(Audit *a, Picture *p) {
  Picture **link=&a->pictures; while(*link && *link!=p) link=&(*link)->next;
  if (*link) { *link=p->next; free(p->pixels); free(p); --a->allocated; }
}
static void *lock_picture(void *opaque, void **planes) {
  Audit *a=opaque; Picture *p=calloc(1,sizeof(*p));
  if (!p || posix_memalign((void **)&p->pixels,32,BYTES)!=0) {
    fputs("FAIL: picture allocation\n",stderr); _Exit(2);
  }
  memset(p->pixels,0,BYTES); pthread_mutex_lock(&a->mutex);
  if (++a->allocated>64) { fputs("FAIL: too many unretired pictures\n",stderr); _Exit(2); }
  p->next=a->pictures; a->pictures=p; pthread_mutex_unlock(&a->mutex);
  planes[0]=p->pixels; return p;
}
static void unlock_picture(void *opaque, void *picture, void *const *planes) {
  Audit *a=opaque; Picture *p=picture; (void)planes;
  const int valid=barcode(p); p->hash=fingerprint(p->pixels);
  pthread_mutex_lock(&a->mutex); p->ready=1;
  if(!valid) fail(a,"invalid RV32 barcode/reference pixels");
  pthread_mutex_unlock(&a->mutex);
}
static void display_picture(void *opaque, void *picture) {
  Audit *a=opaque; Picture *p=picture; const double t=now();
  pthread_mutex_lock(&a->mutex);
  if (!p->ready || p->id>=FRAMES || fingerprint(p->pixels)!=p->hash)
    fail(a,"displayed picture changed after unlock");
  if (!a->failed) {
    if (!a->callbacks) a->first=p->id;
    else if (p->id==a->id) ++a->duplicates;
    else { a->last_move=t; if (p->id<a->id) ++a->regressions; }
    if (!a->seen[p->id]++) ++a->unique;
    if (!a->callbacks || p->id!=a->id) {
      if (a->callbacks<8 || p->id%30==0 || p->id>=350)
        printf("picture id=%u callback=%u\n",p->id,a->callbacks+1);
    }
    a->id=p->id; ++a->callbacks;
    if (a->paused && (p->id!=a->paused_id || p->hash!=a->paused_hash))
      fail(a,"pixels moved while pause stability was being measured");
    if (!a->armed && (a->target<0 || (p->id>=(unsigned)a->target &&
         p->id<=(unsigned)a->target+30))) {
      a->armed=1; a->phase_first=a->phase_last=p->id; a->phase_start=a->phase_end=t;
    } else if (!a->armed) ++a->prefix;
    else if (p->id!=a->phase_last) {
      if (p->id<a->phase_last && a->controls) fail(a,"picture regressed after seek/rate phase settled");
      a->phase_last=p->id; a->phase_end=t; ++a->advances;
    }
  }
  if (a->last && a->last!=p) release_picture(a,a->last);
  a->last=p; pthread_mutex_unlock(&a->mutex);
}
static int healthy(Audit *a, libvlc_media_player_t *player) {
  const libvlc_state_t state=libvlc_media_player_get_state(player);
  pthread_mutex_lock(&a->mutex);
  if (state==libvlc_Error) fail(a,"libvlc_Error");
  if (now()>a->deadline) fail(a,"monotonic overall deadline");
  if (!a->paused && now()-a->last_move>8) fail(a,"no actual picture movement for 8 seconds");
  const int ok=!a->failed; pthread_mutex_unlock(&a->mutex); return ok;
}
static int progress_window(Audit *a, libvlc_media_player_t *player, const char *name,
                           int target, float rate, int select_rate) {
  pthread_mutex_lock(&a->mutex); a->target=target; a->armed=0; a->advances=0;
  const unsigned previous=a->id;
  a->prefix=0; a->last_move=now(); pthread_mutex_unlock(&a->mutex);
  if (select_rate && libvlc_media_player_set_rate(player,rate)!=0) return 0;
  double arrival=now()+1.5;
  if(target>(int)previous) arrival=fmin(arrival,now()+((double)target-previous)/30/rate/2);
  if (target>=0) libvlc_media_player_set_time(player,(libvlc_time_t)target*1000/30);
  const double deadline=now()+8;
  while(now()<deadline && healthy(a,player)) {
    if(libvlc_media_player_get_state(player)==libvlc_Ended) break;
    pthread_mutex_lock(&a->mutex);
    if(target>=0 && (a->armed?a->phase_start>arrival:now()>arrival)) {
      fail(a,"seek target arrived too late to distinguish it from ordinary playback");
      pthread_mutex_unlock(&a->mutex); return 0;
    }
    const double wall=a->phase_end-a->phase_start;
    const double media=((double)a->phase_last-a->phase_first)/30;
    const int done=a->armed && a->advances>=8 && wall>=1.2;
    if(done) {
      const int match=media>0 && fabs(media-rate*wall)<=fmax(2.0/30,rate*wall*0.35);
      printf("Control %s: pictures=%u..%u advances=%u prefix=%u media=%.3f wall=%.3f requested=%.2f result=%s\n",
        name,a->phase_first,a->phase_last,a->advances,a->prefix,media,wall,rate,match?"PASS":"FAIL");
      if (!match) fail(a,"observed picture progression did not match selected rate");
      pthread_mutex_unlock(&a->mutex); return match;
    }
    pthread_mutex_unlock(&a->mutex); tick();
  }
  pthread_mutex_lock(&a->mutex); fail(a,"control phase ended/stalled before stable picture progress");
  pthread_mutex_unlock(&a->mutex); return 0;
}
static int progress(Audit *a, libvlc_media_player_t *player, const char *name, int target, float rate) {
  return progress_window(a,player,name,target,rate,1);
}
static int rate_check(Audit *a, libvlc_media_player_t *player, float rate) {
  if (!progress(a,player,"initial",-1,1) || libvlc_media_player_set_rate(player,rate)!=0) return 0;
  const double start=now();
  while(now()-start<2 && healthy(a,player)) {
    if(libvlc_media_player_get_state(player)==libvlc_Ended) return 0;
    tick();
  }
  if (!healthy(a,player)) return 0;
  printf("Rate settling: selected=%.2f checked=%.3fs; starting fresh pixel window\n",rate,now()-start);
  return progress_window(a,player,rate<1?"settled-half":"settled-double",-1,rate,0);
}
static int pause_check(Audit *a, libvlc_media_player_t *player) {
  libvlc_media_player_set_pause(player,1); const double deadline=now()+2;
  while(now()<deadline && libvlc_media_player_get_state(player)!=libvlc_Paused) tick();
  if(libvlc_media_player_get_state(player)!=libvlc_Paused) return 0;
  const double settle=now()+0.15; while(now()<settle) tick();
  pthread_mutex_lock(&a->mutex);
  if(!a->last) { pthread_mutex_unlock(&a->mutex); return 0; }
  a->paused=1; a->paused_id=a->id; a->paused_hash=fingerprint(a->last->pixels);
  pthread_mutex_unlock(&a->mutex); const double start=now();
  while(now()-start<1 && healthy(a,player)) tick();
  const int is_paused=libvlc_media_player_get_state(player)==libvlc_Paused;
  pthread_mutex_lock(&a->mutex);
  const int ok=!a->failed && is_paused && a->last && a->id==a->paused_id &&
    fingerprint(a->last->pixels)==a->paused_hash && now()-start>=1;
  printf("Control pause: actual frame=%u stable=%.3fs result=%s\n",a->paused_id,now()-start,ok?"PASS":"FAIL");
  a->paused=0; a->last_move=now(); pthread_mutex_unlock(&a->mutex);
  libvlc_media_player_set_pause(player,0); return ok;
}
int main(int argc, char **argv) {
  int software=0, controls=0, rate_case=0;
  if(argc<2) goto usage;
  for(int i=2;i<argc;++i) {
    if(!strcmp(argv[i],"--software") && !software) software=1;
    else if(!strcmp(argv[i],"--controls") && !controls && !rate_case) controls=1;
    else if(!strcmp(argv[i],"--half") && !controls && !rate_case) rate_case=1;
    else if(!strcmp(argv[i],"--double") && !controls && !rate_case) rate_case=2;
    else goto usage;
  }
  setvbuf(stdout,NULL,_IOLBF,0);
  if (!getenv("VLC_PLUGIN_PATH") || !*getenv("VLC_PLUGIN_PATH")) {
    fputs("FAIL: use powervlc-playback.sh to select the isolated plugin tree\n",stderr); return 2;
  }
  const char *expected_library=getenv("CRYSTALHD_PROBE_LIBRARY");
  void *device_open=dlsym(RTLD_DEFAULT,"DtsDeviceOpen");
  Dl_info selected={0};
  char *expected=expected_library?realpath(expected_library,NULL):NULL;
  char *actual=device_open && dladdr(device_open,&selected) && selected.dli_fname?
    realpath(selected.dli_fname,NULL):NULL;
  const int correct_library=expected && actual && !strcmp(expected,actual);
  if (!correct_library) {
    fprintf(stderr,"FAIL: requested CrystalHD library was not selected (actual=%s)\n",
      actual?actual:"no DtsDeviceOpen symbol");
    free(expected); free(actual); return 2;
  }
  printf("Verified CrystalHD library: %s\n",actual);
  free(expected); free(actual);
  const char *args[]={"--ignore-config","--no-audio","--no-osd","--no-video-title-show",
    "--no-sub-autodetect-file","--no-spu","--text-renderer=tdummy","--stats","--verbose=2",
    "--video-cache-mb=0","--no-metadata-network-access","--no-disable-screensaver",
    "--no-drop-late-frames","--no-skip-frames","--avcodec-hw=none",
    software?"--no-crystalhd":"--crystalhd",software?"--codec=avcodec,none":"--codec=crystalhd,none"};
  libvlc_instance_t *instance=libvlc_new((int)(sizeof(args)/sizeof(args[0])),args);
  if(!instance) { fprintf(stderr,"FAIL: libvlc_new: %s\n",libvlc_errmsg()); return 2; }
  libvlc_media_t *media=libvlc_media_new_path(instance,argv[1]);
  libvlc_media_player_t *player=media?libvlc_media_player_new_from_media(media):NULL;
  if(!player) { if(media)libvlc_media_release(media); libvlc_release(instance); return 2; }
  const int smoke=controls || rate_case;
  Audit a={0}; pthread_mutex_init(&a.mutex,NULL); a.controls=smoke;
  a.target=-1; a.last_move=now(); a.deadline=now()+120;
  libvlc_video_set_callbacks(player,lock_picture,unlock_picture,display_picture,&a);
  libvlc_video_set_format(player,"RV32",W,H,PITCH);
  printf("libvlc=%s mode=%s scenario=%s; OFFSCREEN/noaudio, external timeout required\n",
    libvlc_get_version(),software?"software":"native-crystalhd-only",
    controls?"controls":rate_case==1?"settled-half":rate_case==2?"settled-double":"complete-file");
  int ok=libvlc_media_player_play(player)==0;
  if(ok && rate_case) ok=rate_check(&a,player,rate_case==1?0.5f:2.0f);
  else if(ok && controls) {
    ok=progress(&a,player,"initial",-1,1) && pause_check(&a,player) &&
      progress(&a,player,"resume",-1,1) && progress(&a,player,"seek6",180,1) &&
      progress(&a,player,"seek2",60,1) && progress(&a,player,"half",-1,0.5f) &&
      progress(&a,player,"double",-1,2) && progress(&a,player,"restore",-1,1);
  } else while(ok && healthy(&a,player) && libvlc_media_player_get_state(player)!=libvlc_Ended) tick();
  const int ended=libvlc_media_player_get_state(player)==libvlc_Ended;
  libvlc_media_stats_t stats={0}; const int have_stats=libvlc_media_get_stats(media,&stats);
  libvlc_media_player_stop(player); libvlc_media_player_release(player);
  pthread_mutex_lock(&a.mutex);
  const int complete=ended && !a.failed && a.unique==FRAMES && a.first==0 && a.id==359 && !a.regressions;
  ok=ok && !a.failed && (smoke || complete);
  printf("Native stats available=%d decoded=%d displayed=%d lost=%d; callback pictures=%u unique=%u first=%u last=%u repeats=%u regressions=%u ended=%d complete-file=%s\n",
    have_stats,stats.i_decoded_video,stats.i_displayed_pictures,stats.i_lost_pictures,
    a.callbacks,a.unique,a.first,a.id,a.duplicates,a.regressions,ended,complete?"YES":"NO");
  if(!smoke && !complete) { printf("Missing barcode IDs:");
    for(unsigned i=0;i<FRAMES;++i) if(!a.seen[i])printf(" %u",i);
    putchar('\n'); }
  printf("RESULT=%s scope=%s\n",ok?"PASS":"FAIL",smoke?"control-progress-only; NOT complete-file":"complete-file pixel coverage");
  while(a.pictures)release_picture(&a,a.pictures);
  pthread_mutex_unlock(&a.mutex); pthread_mutex_destroy(&a.mutex);
  libvlc_media_release(media); libvlc_release(instance); return ok?0:1;
usage:
  fprintf(stderr,"usage: %s AV360.mp4 [--software] [--controls | --half | --double]\n",argv[0]); return 2;
}
