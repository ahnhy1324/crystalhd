/* Original private engine extraction; author did not build/run it.
 * Qualified a49b SIMD header unchanged, with runtime guard/scalar fallback.
 * No opaque-field access or external implementation changes. */
#include "packed-scale-engine.h"
#include "horizontal-simd.h"
#include <gst/video/video-scaler.h>
#include <math.h>
#include <stdint.h>
enum { IW=1280, IH=720, OW=1920, OH=1080, IB=2560, OB=3840, TAPS=2 };
typedef struct { GstVideoScaler *vertical; guint8 *cache;
  guint row_ids[TAPS]; gpointer lines[TAPS]; } Stripe;
struct _CrystalHdPackedScaleEngine {
  GstVideoInfo input_info,output_info;
  const ChHorizontalTap2 *table; ChHorizontalPlan horizontal;
  Stripe main_stripe,worker_stripe;
  GMutex run_lock,job_lock; GCond changed; GThread *worker;
  gboolean stopping,ready,worker_ok;
  guint64 submitted,completed;
  const guint8 *input; guint8 *output;
  gsize input_stride,output_stride;
};
static gboolean fail(GError **error,gint code,const gchar *message)
{
  g_set_error_literal(error,g_quark_from_static_string("crystalhd-packed-scale-engine"),code,message);
  return FALSE;
}
static gboolean mono_info(const GstVideoInfo *i)
{
  GstVideoMultiviewMode mode=GST_VIDEO_INFO_MULTIVIEW_MODE(i);
  return GST_VIDEO_INFO_VIEWS(i)==1 &&
    (mode==GST_VIDEO_MULTIVIEW_MODE_NONE || mode==GST_VIDEO_MULTIVIEW_MODE_MONO) &&
    GST_VIDEO_INFO_MULTIVIEW_FLAGS(i)==GST_VIDEO_MULTIVIEW_FLAGS_NONE;
}
static gboolean known_color(const GstVideoInfo *i)
{
  return i->colorimetry.range!=GST_VIDEO_COLOR_RANGE_UNKNOWN &&
    i->colorimetry.matrix!=GST_VIDEO_COLOR_MATRIX_UNKNOWN &&
    i->colorimetry.transfer!=GST_VIDEO_TRANSFER_UNKNOWN &&
    i->colorimetry.primaries!=GST_VIDEO_COLOR_PRIMARIES_UNKNOWN &&
    i->chroma_site!=GST_VIDEO_CHROMA_SITE_UNKNOWN;
}
static gboolean admitted(const GstVideoInfo *in,const GstVideoInfo *out)
{
  if(!in || !out || !in->finfo || !out->finfo ||
      GST_VIDEO_INFO_FORMAT(in)!=GST_VIDEO_FORMAT_YUY2 ||
      GST_VIDEO_INFO_FORMAT(out)!=GST_VIDEO_FORMAT_UYVY ||
      GST_VIDEO_INFO_WIDTH(in)!=IW || GST_VIDEO_INFO_HEIGHT(in)!=IH ||
      GST_VIDEO_INFO_WIDTH(out)!=OW || GST_VIDEO_INFO_HEIGHT(out)!=OH) return FALSE;
  const GstVideoInfo *infos[]={in,out};
  for(guint n=0;n<G_N_ELEMENTS(infos);n++) {
    const GstVideoInfo *i=infos[n];
    if(GST_VIDEO_INFO_INTERLACE_MODE(i)!=GST_VIDEO_INTERLACE_MODE_PROGRESSIVE ||
        GST_VIDEO_INFO_FPS_N(i)!=50 || GST_VIDEO_INFO_FPS_D(i)!=1 ||
        GST_VIDEO_INFO_PAR_N(i)!=1 || GST_VIDEO_INFO_PAR_D(i)!=1 ||
        GST_VIDEO_INFO_N_PLANES(i)!=1 || !mono_info(i) || !known_color(i)) return FALSE;
  }
  return gst_video_colorimetry_is_equal(&in->colorimetry,&out->colorimetry) && in->chroma_site==out->chroma_site;
}
static gboolean table_build(CrystalHdPackedScaleEngine *e,GstStructure *options,GError **error)
{
  GstVideoScaler *y=NULL,*uv=NULL,*merged=NULL; ChHorizontalTap2 *table=NULL;
  gboolean ok=FALSE;
  y=gst_video_scaler_new(GST_VIDEO_RESAMPLER_METHOD_LINEAR,GST_VIDEO_SCALER_FLAG_NONE,0,IW,OW,options);
  if(!y || gst_video_scaler_get_max_taps(y)!=TAPS) goto done;
  uv=gst_video_scaler_new(GST_VIDEO_RESAMPLER_METHOD_LINEAR,GST_VIDEO_SCALER_FLAG_NONE,TAPS,IW/2,OW/2,options);
  if(!uv || gst_video_scaler_get_max_taps(uv)!=TAPS) goto done;
  merged=gst_video_scaler_combine_packed_YUV(y,uv,GST_VIDEO_FORMAT_UYVY,GST_VIDEO_FORMAT_UYVY);
  if(!merged || gst_video_scaler_get_max_taps(merged)!=TAPS) goto done;
  table=g_try_new0(ChHorizontalTap2,OB); if(!table) goto done;
  for(guint i=0;i<OB;i++) {
    guint offset=0,taps=0;
    const gdouble *c=gst_video_scaler_get_coeff(merged,i,&offset,&taps);
    if(!c || taps!=TAPS || !isfinite(c[0]) || !isfinite(c[1]) ||
        c[0]<0 || c[0]>1 || c[1]<0 || c[1]>1) goto done;
    /* Active1.28.2 packed-u8 is Q6. Positive truncation is stock floor(.5+c*64).
     * Exactsum64 proves stock succeeds at its FIRST bias; otherwise reject. */
    guint a=(guint)(0.5+c[0]*64.0),b=(guint)(0.5+c[1]*64.0),step=(i&1)?2:4;
    if(a>64 || b>64 || a+b!=64 || offset>=IB || step>=IB-offset) goto done;
    guint s0=offset^1U,s1=(offset+step)^1U;
    if(s0>=IB || s1>=IB) goto done;
    table[i]=(ChHorizontalTap2){(guint16)s0,(guint16)s1,(guint8)a,(guint8)b};
  }
  if(!ch_horizontal_plan_init(&e->horizontal,table,IB,OB,1)) goto done;
  e->table=table; table=NULL; ok=TRUE;
done:
  g_free(table);
  if(merged) gst_video_scaler_free(merged);
  if(uv) gst_video_scaler_free(uv);
  if(y) gst_video_scaler_free(y);
  return ok || fail(error,2,"unsupported two-tap coefficients or allocation failure");
}
static gboolean stripe_init(Stripe *s,GstStructure *options)
{
  s->vertical=gst_video_scaler_new(GST_VIDEO_RESAMPLER_METHOD_LINEAR,GST_VIDEO_SCALER_FLAG_NONE,0,IH,OH,options);
  if(!s->vertical || gst_video_scaler_get_max_taps(s->vertical)!=TAPS) return FALSE;
  s->cache=g_try_malloc0_n(TAPS,OB); /* Exactly active bytes, NO PAD. */
  return s->cache!=NULL;
}
static void stripe_clear(Stripe *s)
{ if(s->vertical) gst_video_scaler_free(s->vertical); g_free(s->cache); }
static gboolean stripe_run(const CrystalHdPackedScaleEngine *e,Stripe *s,
    const guint8 *input,gsize is,guint8 *output,gsize os,guint first,guint end)
{
  for(guint k=0;k<TAPS;k++) s->row_ids[k]=G_MAXUINT;
  for(guint y=first;y<end;y++) {
    guint in=0,taps=0;
    const gdouble *c=gst_video_scaler_get_coeff(s->vertical,y,&in,&taps);
    if(!c || taps!=TAPS || in>=IH || taps>IH-in) return FALSE;
    for(guint j=0;j<TAPS;j++) {
      guint row=in+j,slot=row%TAPS;
      guint8 *cache=s->cache+(gsize)slot*OB;
      if(s->row_ids[slot]!=row) {
        ch_horizontal_run(&e->horizontal,input+(gsize)row*is,cache);
        s->row_ids[slot]=row;
      }
      s->lines[j]=cache;
    }
    gst_video_scaler_vertical(s->vertical,GST_VIDEO_FORMAT_UYVY,s->lines,output+(gsize)y*os,y,OW);
  }
  return TRUE;
}

static gpointer worker_main(gpointer data)
{
  CrystalHdPackedScaleEngine *e=data;
  g_mutex_lock(&e->job_lock); e->ready=TRUE; g_cond_broadcast(&e->changed);
  for(;;) {
    while(!e->stopping && e->submitted==e->completed) g_cond_wait(&e->changed,&e->job_lock);
    if(e->stopping) break;
    guint64 generation=e->submitted;
    const guint8 *input=e->input; guint8 *output=e->output;
    gsize is=e->input_stride,os=e->output_stride;
    g_mutex_unlock(&e->job_lock);
    gboolean ok=stripe_run(e,&e->worker_stripe,input,is,output,os,OH/2,OH);
    g_mutex_lock(&e->job_lock); e->worker_ok=ok; e->completed=generation;
    g_cond_broadcast(&e->changed);
  }
  g_mutex_unlock(&e->job_lock); return NULL;
}
void crystalhd_packed_scale_engine_free(CrystalHdPackedScaleEngine *e)
{
  if(!e) return;
  /* Caller excludes new entries/waiters; drain any already-active run. */
  g_mutex_lock(&e->run_lock);
  g_mutex_lock(&e->job_lock); e->stopping=TRUE; g_cond_broadcast(&e->changed);
  g_mutex_unlock(&e->job_lock);
  if(e->worker) g_thread_join(e->worker);
  stripe_clear(&e->main_stripe); stripe_clear(&e->worker_stripe);
  g_free((gpointer)e->table);
  g_mutex_unlock(&e->run_lock);
  g_cond_clear(&e->changed); g_mutex_clear(&e->job_lock); g_mutex_clear(&e->run_lock);
  g_free(e);
}
CrystalHdPackedScaleEngine *crystalhd_packed_scale_engine_new(
    const GstVideoInfo *input,const GstVideoInfo *output,GError **error)
{
  guint major,minor,micro,nano; gst_version(&major,&minor,&micro,&nano);
  if(major!=1 || minor!=28 || micro!=2 || nano!=0) {
    fail(error,1,"unqualified GStreamer version; exact1.28.2 required"); return NULL;
  }
  if(!admitted(input,output)) {
    fail(error,1,"unsupported format/rate/geometry/color/chroma/multiview; no fallback"); return NULL;
  }
  CrystalHdPackedScaleEngine *e=g_try_new0(CrystalHdPackedScaleEngine,1);
  if(!e) { fail(error,2,"engine allocation failed"); return NULL; }
  g_mutex_init(&e->run_lock); g_mutex_init(&e->job_lock); g_cond_init(&e->changed);
  e->input_info=*input; e->output_info=*output;
  GstStructure *options=gst_structure_new("crystalhd-packed-resampler",
      GST_VIDEO_RESAMPLER_OPT_ENVELOPE,G_TYPE_DOUBLE,2.0,
      GST_VIDEO_RESAMPLER_OPT_SHARPNESS,G_TYPE_DOUBLE,1.0,
      GST_VIDEO_RESAMPLER_OPT_SHARPEN,G_TYPE_DOUBLE,0.0,NULL);
  gboolean ok=table_build(e,options,error);
  if(ok && (!stripe_init(&e->main_stripe,options) || !stripe_init(&e->worker_stripe,options)))
    ok=fail(error,2,"vertical scaler/cache allocation failed");
  gst_structure_free(options);
  if(!ok) { crystalhd_packed_scale_engine_free(e); return NULL; }
  e->worker=g_thread_try_new("crystalhd-pack",worker_main,e,error);
  if(!e->worker) { crystalhd_packed_scale_engine_free(e); return NULL; }
  g_mutex_lock(&e->job_lock);
  while(!e->ready) g_cond_wait(&e->changed,&e->job_lock);
  g_mutex_unlock(&e->job_lock);
  return e;
}
/* Validate actual mapped plane, positive padded stride and VideoMeta offset.
 * No tight-row/whole-buffer/PAD assumption. */
static gboolean mapped_span(const GstVideoFrame *f,guint active,guint height,
    GstMapFlags access,guintptr *first,guintptr *end,gsize *stride)
{
  if(!f || !f->buffer || !f->map[0].data || (f->map[0].flags&access)!=access ||
      !GST_VIDEO_FRAME_PLANE_DATA(f,0)) return FALSE;
  gint actual=GST_VIDEO_FRAME_PLANE_STRIDE(f,0);
  if(actual<=0 || (guint)actual<active || !height) return FALSE;
  gsize s=(gsize)actual;
  if((gsize)(height-1)>(G_MAXSIZE-active)/s) return FALSE;
  gsize needed=(gsize)(height-1)*s+active;
  guintptr base=(guintptr)f->map[0].data,p=(guintptr)GST_VIDEO_FRAME_PLANE_DATA(f,0);
  if(p<base || p-base>f->map[0].size) return FALSE;
  gsize offset=(gsize)(p-base);
  if(needed>f->map[0].size-offset || needed>UINTPTR_MAX-p) return FALSE;
  *first=p; *end=p+needed; *stride=s; return TRUE;
}
gboolean crystalhd_packed_scale_engine_run(CrystalHdPackedScaleEngine *e,
    const GstVideoFrame *input,GstVideoFrame *output,GError **error)
{
  if(!e || !input || !output || !admitted(&input->info,&output->info) ||
      !gst_video_colorimetry_is_equal(&input->info.colorimetry,&e->input_info.colorimetry) ||
      !gst_video_colorimetry_is_equal(&output->info.colorimetry,&e->output_info.colorimetry) ||
      input->info.chroma_site!=e->input_info.chroma_site || output->info.chroma_site!=e->output_info.chroma_site)
    return fail(error,3,"mapped frame identity differs from engine negotiation");
  guintptr a,b,c,d; gsize is,os;
  if(!mapped_span(input,IB,IH,GST_MAP_READ,&a,&b,&is) ||
      !mapped_span(output,OB,OH,GST_MAP_WRITE,&c,&d,&os) || (a<d && c<b) ||
      GST_BUFFER_FLAG_IS_SET(input->buffer,GST_VIDEO_BUFFER_FLAG_INTERLACED) ||
      GST_BUFFER_FLAG_IS_SET(input->buffer,GST_VIDEO_BUFFER_FLAG_ONEFIELD))
    return fail(error,3,"unsupported mapped bounds/stride/access/alias/interlace flags");
  g_mutex_lock(&e->run_lock); g_mutex_lock(&e->job_lock);
  if(e->stopping || e->submitted!=e->completed || e->submitted==G_MAXUINT64) {
    g_mutex_unlock(&e->job_lock); g_mutex_unlock(&e->run_lock);
    return fail(error,4,"engine stopped or invalid job generation");
  }
  e->input=(const guint8 *)a; e->output=(guint8 *)c;
  e->input_stride=is; e->output_stride=os;
  guint64 generation=++e->submitted;
  g_cond_broadcast(&e->changed); g_mutex_unlock(&e->job_lock);
  gboolean main_ok=stripe_run(e,&e->main_stripe,(const guint8 *)a,is,(guint8 *)c,os,0,OH/2);
  g_mutex_lock(&e->job_lock);
  while(e->completed!=generation) g_cond_wait(&e->changed,&e->job_lock);
  gboolean worker_ok=e->worker_ok;
  e->input=NULL; e->output=NULL; e->input_stride=e->output_stride=0;
  g_mutex_unlock(&e->job_lock); g_mutex_unlock(&e->run_lock);
  return (main_ok && worker_ok) || fail(error,4,"vertical row coefficient bounds failed");
}
gboolean crystalhd_packed_scale_engine_simd_active(const CrystalHdPackedScaleEngine *e)
{ return e && e->horizontal.use_ssse3!=0; }
