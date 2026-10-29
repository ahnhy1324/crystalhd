/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Pinned metadata bodies; no decoder, registered node, kernel or ioctl runs. */
#include <errno.h>
#include <limits.h>
#include <time.h>
#include <linux/videodev2.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef uint32_t u32;
/* Kernel-only enum sentinels from the pinned 7.0 UAPI header. */
#define V4L2_COLORSPACE_LAST (V4L2_COLORSPACE_DCI_P3 + 1)
#define V4L2_XFER_FUNC_LAST (V4L2_XFER_FUNC_SMPTE2084 + 1)
#define V4L2_YCBCR_ENC_LAST (V4L2_YCBCR_ENC_SMPTE240M + 1)
_Static_assert(V4L2_COLORSPACE_LAST == 13 && V4L2_XFER_FUNC_LAST == 8 &&
	       V4L2_YCBCR_ENC_LAST == 9, "pinned kernel validation limits changed");
#define U8_MAX UINT8_MAX
#define CHD_CODED_SIZE (2U * 1024U * 1024U)
#define clamp_t(t, v, lo, hi) ((t)(v) < (t)(lo) ? (t)(lo) : ((t)(v) > (t)(hi) ? (t)(hi) : (t)(v)))

static void require(bool condition, const char *message)
{
	if (!condition) { fprintf(stderr, "FAIL: %s\n", message); exit(1); }
}

struct mutex { unsigned held; };
static void mutex_lock(struct mutex *lock) { require(!lock->held, "nested model lock"); ++lock->held; }
static void mutex_unlock(struct mutex *lock) { require(lock->held == 1, "unbalanced model lock"); --lock->held; }
struct vb2_queue { bool busy; };
struct crystalhd_v4l2_file {
	struct v4l2_pix_format output, capture;
	bool color_default, format_known;
	struct mutex run_lock;
	struct { void *m2m_ctx; } fh;
	struct vb2_queue output_queue, capture_queue;
};
struct file { void *private_data; };
static struct crystalhd_v4l2_file *chd_file(void *private_data) { return private_data; }
static struct vb2_queue *v4l2_m2m_get_vq(void *context, unsigned type)
{
	struct crystalhd_v4l2_file *f = context;
	return type == V4L2_BUF_TYPE_VIDEO_OUTPUT ? &f->output_queue :
		type == V4L2_BUF_TYPE_VIDEO_CAPTURE ? &f->capture_queue : NULL;
}
static struct vb2_queue *v4l2_m2m_get_dst_vq(void *context)
{
	return &((struct crystalhd_v4l2_file *)context)->capture_queue;
}
static bool vb2_is_busy(struct vb2_queue *queue) { return queue->busy; }
static int crystalhd_v4l2_capture_size(unsigned width, unsigned height, unsigned *size)
{
	/* Buffer sizing is outside this metadata model, not a DMA-capacity claim. */
	(void)width; (void)height; *size = CHD_CODED_SIZE; return 0;
}

#include "kernel-validation.inc"
#include "node-colors.inc"
#include "node-formats.inc"

static void initial_format(struct crystalhd_v4l2_file *f)
{
	memset(f, 0, sizeof(*f));
	f->fh.m2m_ctx = f;
#include "node-initial-format.inc"
}

enum { AV_CODEC_ID_RAWVIDEO, AV_CODEC_ID_H264 };
typedef struct V4L2Context {
	struct v4l2_format format;
	enum v4l2_buf_type type;
	unsigned width, height, min_buf_size;
	int av_codec_id, av_pix_fmt;
} V4L2Context;
struct v4l2_format_update { unsigned v4l2_fmt; int av_fmt; bool update_avfmt, update_v4l2; };
typedef struct V4L2m2mContext { int fd; } V4L2m2mContext;
static V4L2m2mContext context = {17};
static struct file *active_file;
static V4L2m2mContext *ctx_to_m2mctx(V4L2Context *ctx) { (void)ctx; return &context; }
static unsigned v4l2_get_framesize_compressed(V4L2Context *ctx, unsigned width, unsigned height)
{
	(void)ctx; (void)width; (void)height; return CHD_CODED_SIZE;
}
static int v4l2_get_coded_format(V4L2Context *ctx, unsigned *fourcc)
{
	(void)ctx; *fourcc = V4L2_PIX_FMT_H264; return 0;
}
static int v4l2_get_raw_format(V4L2Context *ctx, int *format)
{
	(void)ctx; (void)format; require(false, "raw-format negotiation is outside this model"); return -EINVAL;
}
static int format_ioctl(int fd, unsigned long request, struct v4l2_format *format)
{
	require(fd == context.fd && active_file, "unexpected model ioctl target");
	if (request == VIDIOC_TRY_FMT) return chd_try_fmt(active_file, NULL, format);
	if (request == VIDIOC_S_FMT) return chd_set_fmt(active_file, NULL, format);
	require(false, "unexpected model ioctl"); return -EINVAL;
}
#define ioctl format_ioctl
#include "ffmpeg-save-format.inc"
#include "ffmpeg-get-format.inc"
#include "ffmpeg-set-format.inc"
#undef ioctl

static void zero_client_dimensions(V4L2Context *output, V4L2Context *capture)
{
#include "ffmpeg-zero-dimensions.inc"
}
static void discover(struct crystalhd_v4l2_file *f, unsigned width, unsigned height)
{
	/* Source discovery's dimensions are a boundary; execute actual colour update. */
	f->output.width = f->capture.width = width;
	f->output.height = f->capture.height = height;
	f->format_known = true;
	chd_source_colors(f);
}
static void query_is_pure(struct file *file, struct v4l2_format *format, bool try_format)
{
	struct crystalhd_v4l2_file before;
	memcpy(&before, file->private_data, sizeof(before));
	require((try_format ? chd_try_fmt(file, NULL, format) : chd_get_fmt(file, NULL, format)) == 0,
		"format query failed");
	require(!memcmp(&before, file->private_data, sizeof(before)), "format query changed driver state");
}

static void direct_and_normalized_default(void)
{
	struct crystalhd_v4l2_file direct, queried;
	struct file a = {&direct}, b = {&queried};
	struct v4l2_format raw = {.type = V4L2_BUF_TYPE_VIDEO_OUTPUT}, tried = raw, committed = raw;
	initial_format(&direct); initial_format(&queried);
	query_is_pure(&b, &tried, true);
	require(chd_set_fmt(&a, NULL, &committed) == 0 && !memcmp(&tried, &committed, sizeof(tried)),
		"TRY_FMT and S_FMT differ for the same input");
	require(direct.color_default && tried.fmt.pix.colorspace == V4L2_COLORSPACE_SMPTE170M,
		"initial unspecified SD normalization changed");
	struct v4l2_format explicit_request = tried;
	require(chd_set_fmt(&b, NULL, &tried) == 0 && !queried.color_default,
		"normalized default was not committed as an explicit value");
	discover(&direct, 1920, 1080); discover(&queried, 1920, 1080);
	struct crystalhd_v4l2_file deliberate;
	struct file c = {&deliberate}; initial_format(&deliberate);
	require(!memcmp(&explicit_request, &tried, sizeof(tried)), "explicit and returned requests differ");
	require(chd_set_fmt(&c, NULL, &explicit_request) == 0, "explicit SD request failed");
	discover(&deliberate, 1920, 1080);
	require(deliberate.capture.colorspace == V4L2_COLORSPACE_SMPTE170M && !deliberate.color_default,
		"explicit caller colour was overridden");
	require(direct.capture.colorspace == V4L2_COLORSPACE_REC709 &&
		queried.capture.colorspace == V4L2_COLORSPACE_SMPTE170M,
		"source-qualified default/normalized roundtrip mechanism changed");
	struct v4l2_format get = {.type = V4L2_BUF_TYPE_VIDEO_CAPTURE};
	query_is_pure(&b, &get, false);
	require(get.fmt.pix.colorspace == V4L2_COLORSPACE_SMPTE170M && get.fmt.pix.width == 1920,
		"G_FMT did not return retained source dimensions/colour");
}

static void explicit_tuple_and_busy(void)
{
	struct crystalhd_v4l2_file state; initial_format(&state);
	struct file file = {&state};
	struct v4l2_format format = {.type = V4L2_BUF_TYPE_VIDEO_OUTPUT};
	format.fmt.pix = (struct v4l2_pix_format){.width=1920, .height=1080,
		.colorspace=V4L2_COLORSPACE_REC709, .xfer_func=V4L2_XFER_FUNC_709,
		.ycbcr_enc=V4L2_YCBCR_ENC_709, .quantization=V4L2_QUANTIZATION_FULL_RANGE,
		.priv=V4L2_PIX_FMT_PRIV_MAGIC};
	query_is_pure(&file, &format, true);
	require(chd_set_fmt(&file, NULL, &format) == 0, "extended explicit format failed");
	discover(&state, 640, 480);
	require(!state.color_default && state.capture.colorspace == V4L2_COLORSPACE_REC709 &&
		state.capture.xfer_func == V4L2_XFER_FUNC_709 && state.capture.ycbcr_enc == V4L2_YCBCR_ENC_709 &&
		state.capture.quantization == V4L2_QUANTIZATION_FULL_RANGE,
		"explicit extended colour tuple was overridden");
	state.capture_queue.busy = true;
	struct crystalhd_v4l2_file before; memcpy(&before, &state, sizeof(before));
	require(chd_set_fmt(&file, NULL, &format) == -EBUSY && !memcmp(&before, &state, sizeof(before)),
		"busy format commit changed driver state");
}

static void exact_ffmpeg_roundtrip(void)
{
	struct crystalhd_v4l2_file state; initial_format(&state);
	struct file file = {&state}; active_file = &file;
	V4L2Context output = {.type=V4L2_BUF_TYPE_VIDEO_OUTPUT, .av_codec_id=AV_CODEC_ID_H264};
	V4L2Context capture = {0}; zero_client_dimensions(&output, &capture);
	require(!output.width && !output.height, "pinned FFmpeg zero-dimension initialization changed");
	require(ff_v4l2_context_get_format(&output, 0) == 0, "pinned coded-format TRY failed");
	require(ff_v4l2_context_set_format(&output) == 0 && !state.color_default,
		"pinned TRY/commit did not retain normalized explicit colour");
	discover(&state, 1280, 720);
	require(state.capture.colorspace == V4L2_COLORSPACE_SMPTE170M,
		"pinned client/node metadata mechanism changed");
}

int main(void)
{
	direct_and_normalized_default();
	explicit_tuple_and_busy();
	exact_ffmpeg_roundtrip();
	puts("PASS: exact metadata bodies; pure queries, TRY/S normalization equality, explicit tuple, "
	     "busy isolation and normalized-default sensitivity reproduced");
	puts("Scope: source mechanism only; driver intent inference and historical native attribution NOT_PROVEN");
	return 0;
}
