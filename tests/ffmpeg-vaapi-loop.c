/* SPDX-License-Identifier: GPL-2.0-or-later
 * Execute exact LGPL FFmpeg excerpts against an ID/PTS/reference ledger.
 * The shims record requests; they do not decode, render pixels or run FFmpeg.
 */
#include <errno.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_REFERENCES 8
#define AVERROR(e) (-(e))
#define AVERROR_EOF (-541478725)
#define AV_LOG_DEBUG 1
#define AV_LOG_VERBOSE 2
#define AV_LOG_ERROR 3
#define AV_FRAME_FLAG_INTERLACED 1
#define AV_FRAME_FLAG_TOP_FIELD_FIRST 2
#define VA_DEINTERLACING_BOTTOM_FIELD 1
#define VA_DEINTERLACING_BOTTOM_FIELD_FIRST 2
#define VA_STATUS_SUCCESS 0
#define VAProcFilterDeinterlacing 1
#define VAProcFilterParameterBufferType 2
#define HWACCEL_CHANGED 16

enum {
    VAProcDeinterlacingNone, VAProcDeinterlacingBob,
    VAProcDeinterlacingWeave, VAProcDeinterlacingMotionAdaptive,
    VAProcDeinterlacingMotionCompensated, VAProcDeinterlacingCount
};
typedef int VAStatus;
typedef uint32_t VASurfaceID;
typedef struct AVBufferRef { void *data; } AVBufferRef;
typedef struct AVFrame {
    uint8_t *data[4];
    int64_t pts;
    int flags, format, width, height;
    AVBufferRef *hw_frames_ctx;
    unsigned id, kind, released, field_flags;
} AVFrame;
typedef struct AVFilterContext AVFilterContext;
typedef struct AVFilterLink {
    AVFilterContext *src, *dst;
} AVFilterLink;
struct AVFilterContext {
    void *priv;
    AVFilterLink **inputs, **outputs;
};
typedef struct { int type; } VAProcFilterCapDeinterlacing;
typedef struct {
    unsigned num_forward_references, num_backward_references;
} VAProcPipelineCaps;
typedef struct { int type, algorithm, flags; }
    VAProcFilterParameterBufferDeinterlacing;
typedef struct {
    VASurfaceID surface;
    uint32_t *filters;
    unsigned num_filters;
    VASurfaceID *forward_references, *backward_references;
    unsigned num_forward_references, num_backward_references;
} VAProcPipelineParameterBuffer;
typedef struct { void *display; } VAAPIHWContext;
typedef struct {
    VAAPIHWContext *hwctx;
    unsigned va_context, output_width, output_height;
    uint32_t filter_buffers[1];
} VAAPIVPPContext;

#include "deint-context.inc"

enum { INPUT = 1, OUTPUT = 2, CAPACITY = 4096 };
static AVFrame frames[CAPACITY];
static unsigned frame_count, output_count, render_count, uninit_count;
static struct { unsigned id, field; int64_t pts; } outputs[CAPACITY];
static unsigned rendered[CAPACITY];
static VAProcFilterParameterBufferDeinterlacing filter_params;
static int upstream_status = AVERROR_EOF;

static void require(int okay, const char *reason)
{
    if (!okay) {
        fprintf(stderr, "FAIL: %s\n", reason);
        exit(1);
    }
}

static AVFrame *new_frame(unsigned kind, unsigned id)
{
    AVFrame *frame;
    require(frame_count < CAPACITY && id < CAPACITY, "ledger capacity exceeded");
    frame = &frames[frame_count++];
    memset(frame, 0, sizeof(*frame));
    frame->kind = kind;
    frame->id = id;
    return frame;
}

static void av_frame_free(AVFrame **frame)
{
    if (!*frame)
        return;
    require((*frame)->released == 0, "a frame reference was released twice");
    ++(*frame)->released;
    *frame = NULL;
}

static void av_log(void *context, int level, const char *format, ...)
{
    (void)context; (void)level; (void)format;
}
static const char *av_get_pix_fmt_name(int format)
{
    (void)format;
    return "vaapi";
}
static const char *vaErrorStr(int status)
{
    (void)status;
    return "instrumented success";
}
static VAStatus vaQueryVideoProcFilterCaps(void *display, unsigned context,
                                         int type, void *caps, unsigned *count)
{
    VAProcFilterCapDeinterlacing *deint = caps;
    (void)display; (void)context;
    require(type == VAProcFilterDeinterlacing && *count > 0,
            "filter capability request changed");
    deint[0].type = VAProcDeinterlacingBob;
    *count = 1;
    return VA_STATUS_SUCCESS;
}
static VAStatus vaQueryVideoProcPipelineCaps(void *display, unsigned context,
                                           uint32_t *filters, unsigned count,
                                           VAProcPipelineCaps *caps)
{
    (void)display; (void)context;
    require(count == 1 && filters[0] == 9, "pipeline filter binding changed");
    caps->num_forward_references = caps->num_backward_references = 0;
    return VA_STATUS_SUCCESS;
}
static int ff_vaapi_vpp_make_param_buffers(AVFilterContext *avctx, int type,
                                         void *data, unsigned size, unsigned count)
{
    DeintVAAPIContext *ctx = avctx->priv;
    require(type == VAProcFilterParameterBufferType && count == 1 &&
                size == sizeof(filter_params), "filter parameter request changed");
    memcpy(&filter_params, data, size);
    ctx->vpp_ctx.filter_buffers[0] = 9;
    return 0;
}
static void ff_vaapi_vpp_pipeline_uninit(AVFilterContext *context)
{
    (void)context;
    ++uninit_count;
}
static AVFrame *ff_get_video_buffer(AVFilterLink *link, unsigned width, unsigned height)
{
    AVFrame *frame = new_frame(OUTPUT, 0);
    (void)link;
    frame->width = width;
    frame->height = height;
    return frame;
}
static int av_frame_copy_props(AVFrame *output, AVFrame *input)
{
    require(!input->released, "properties read from a released input");
    output->id = input->id;
    output->pts = input->pts;
    output->flags = input->flags;
    return 0;
}
static int ff_vaapi_vpp_init_params(AVFilterContext *context,
                                   VAProcPipelineParameterBuffer *params,
                                   AVFrame *input, AVFrame *output)
{
    (void)context; (void)output;
    require(!input->released, "render requested from a released input");
    memset(params, 0, sizeof(*params));
    params->surface = (VASurfaceID)(uintptr_t)input->data[3];
    return 0;
}
static VAStatus vaMapBuffer(void *display, unsigned buffer, void **address)
{
    (void)display;
    require(buffer == 9, "unexpected filter parameter buffer");
    *address = &filter_params;
    return VA_STATUS_SUCCESS;
}
static VAStatus vaUnmapBuffer(void *display, unsigned buffer)
{
    (void)display;
    require(buffer == 9, "unexpected filter parameter unmap");
    return VA_STATUS_SUCCESS;
}
static int ff_vaapi_vpp_render_picture(AVFilterContext *context,
                                      VAProcPipelineParameterBuffer *params,
                                      AVFrame *output)
{
    (void)context;
    require(params->surface == output->id + 1 && params->num_filters == 1 &&
                !params->num_forward_references && !params->num_backward_references,
            "output metadata and actual requested source/refs disagree");
    output->field_flags = filter_params.flags;
    ++rendered[output->id];
    ++render_count;
    return 0;
}
static int ff_filter_frame(AVFilterLink *link, AVFrame *frame)
{
    (void)link;
    require(output_count < CAPACITY && frame->kind == OUTPUT &&
                !(frame->flags & AV_FRAME_FLAG_INTERLACED),
            "output ownership or progressive flag changed");
    outputs[output_count].id = frame->id;
    outputs[output_count].pts = frame->pts;
    outputs[output_count++].field = frame->field_flags;
    av_frame_free(&frame); /* downstream takes this output reference */
    return 0;
}
static int ff_request_frame(AVFilterLink *link)
{
    (void)link;
    return upstream_status;
}

/* Retain FFmpeg's existing signed/unsigned libva interfaces unchanged. The
 * surrounding scaffold still compiles with all requested warnings as errors. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpointer-sign"
#pragma GCC diagnostic ignored "-Wsign-compare"
#include "deint-mode-name.inc"
#include "deint-uninit.inc"
#include "deint-build.inc"
#include "deint-filter-frame.inc"
#include "deint-request-frame.inc"
#pragma GCC diagnostic pop

typedef struct { AVFilterContext *filter; } InputFilter;
typedef struct { AVFilterContext *filter; } OutputFilter;
typedef struct { AVFilterContext *filter; } AVFilterGraph;
typedef struct {
    int nb_inputs, nb_outputs;
    InputFilter **inputs;
    OutputFilter **outputs;
} FilterGraph;
typedef struct { AVFilterGraph *graph; } FilterGraphThread;
static void avfilter_graph_free(AVFilterGraph **graph)
{
    require(*graph != NULL, "graph freed twice");
    deint_vaapi_pipeline_uninit((*graph)->filter);
    *graph = NULL;
}
#include "filter-cleanup.inc"

typedef struct { AVBufferRef *hw_frames_ctx; } InputFilterPriv;
static int hardware_changed(AVBufferRef *old, AVBufferRef *new)
{
    InputFilterPriv storage = {old}, *ifp = &storage;
    AVFrame next = {0}, *frame = &next;
    int need_reinit = 0;
    frame->hw_frames_ctx = new;
#include "filter-hwcontext-reinit.inc"
    return need_reinit;
}

typedef struct {
    DeintVAAPIContext ctx;
    VAAPIHWContext hw;
    AVFilterContext filter;
    AVFilterLink input, output;
    AVFilterLink *inputs[1], *outputs[1];
    InputFilter fg_input, *fg_inputs[1];
    OutputFilter fg_output, *fg_outputs[1];
    FilterGraph graph;
    AVFilterGraph allocated;
    FilterGraphThread thread;
} Fixture;

static void start(Fixture *f, int field_rate)
{
    memset(f, 0, sizeof(*f));
    f->ctx.mode = VAProcDeinterlacingBob;
    f->ctx.field_rate = field_rate;
    f->ctx.vpp_ctx.hwctx = &f->hw;
    f->ctx.vpp_ctx.output_width = 1280;
    f->ctx.vpp_ctx.output_height = 720;
    f->filter.priv = &f->ctx;
    f->filter.inputs = f->inputs;
    f->filter.outputs = f->outputs;
    f->inputs[0] = &f->input;
    f->outputs[0] = &f->output;
    f->input.dst = &f->filter;
    f->output.src = &f->filter;
    f->fg_input.filter = f->fg_output.filter = &f->filter;
    f->fg_inputs[0] = &f->fg_input;
    f->fg_outputs[0] = &f->fg_output;
    f->graph.nb_inputs = f->graph.nb_outputs = 1;
    f->graph.inputs = f->fg_inputs;
    f->graph.outputs = f->fg_outputs;
    f->allocated.filter = &f->filter;
    f->thread.graph = &f->allocated;
    require(deint_vaapi_build_filter_params(&f->filter) == 0,
            "actual filter parameter setup failed");
    require(f->ctx.queue_depth == field_rate &&
                f->ctx.extra_delay_for_timestamps == (field_rate == 2),
            "zero-reference field timestamp lookahead changed");
}
static void push(Fixture *f, unsigned id, int64_t pts)
{
    AVFrame *input = new_frame(INPUT, id);
    input->data[3] = (uint8_t *)(uintptr_t)(id + 1);
    input->pts = pts;
    input->width = 1280;
    input->height = 720;
    input->flags = AV_FRAME_FLAG_INTERLACED | AV_FRAME_FLAG_TOP_FIELD_FIRST;
    require(deint_vaapi_filter_frame(&f->input, input) == 0,
            "actual input function failed");
}
static void finish_eof(Fixture *f)
{
    unsigned before;
    upstream_status = AVERROR_EOF;
    require(deint_vaapi_request_frame(&f->output) == 0,
            "first field-rate EOF request did not flush");
    before = output_count;
    require(deint_vaapi_request_frame(&f->output) == AVERROR_EOF &&
                output_count == before, "repeated EOF duplicated an output");
}
static void cleanup(Fixture *f)
{
    unsigned before = uninit_count;
    cleanup_filtergraph(&f->graph, &f->thread);
    require(!f->fg_input.filter && !f->fg_output.filter && !f->thread.graph &&
                f->ctx.queue_count == 0 && uninit_count == before + 1,
            "actual graph cleanup did not unlink and uninitialize once");
}
static void all_released(void)
{
    for (unsigned i = 0; i < frame_count; ++i)
        require(frames[i].released == 1, "a queued frame reference was not released");
    require(render_count == output_count, "a rendered field was not delivered");
}
static void reset_ledger(void)
{
    all_released();
    frame_count = output_count = render_count = uninit_count = 0;
    memset(rendered, 0, sizeof(rendered));
}

static void field_sequence(int reconfigure, int eof_at_boundaries)
{
    Fixture f;
    reset_ledger();
    start(&f, 2);
    for (unsigned id = 0; id < 150; ++id) {
        push(&f, id, id);
        if (reconfigure && (id == 49 || id == 99)) {
            if (eof_at_boundaries)
                finish_eof(&f);
            cleanup(&f);
            all_released();
            start(&f, 2);
        }
    }
    finish_eof(&f);
    cleanup(&f);
    all_released();
    require(output_count == (reconfigure && !eof_at_boundaries ? 296U : 300U),
            "EOF versus teardown did not preserve the expected field count");
    unsigned got = 0;
    for (unsigned expected = 0; expected < 300; ++expected) {
        int missing = reconfigure && !eof_at_boundaries &&
            (expected == 98 || expected == 99 || expected == 198 || expected == 199);
        if (missing)
            continue;
        require(outputs[got].id == expected / 2 && outputs[got].pts == expected &&
                    outputs[got].field == (expected % 2 ? VA_DEINTERLACING_BOTTOM_FIELD : 0),
                "output identity, field parity, or normalized PTS changed");
        ++got;
    }
    require(rendered[49] == (reconfigure && !eof_at_boundaries ? 0U : 2U) &&
                rendered[99] == (reconfigure && !eof_at_boundaries ? 0U : 2U) &&
                rendered[149] == 2, "pending last-picture request boundary changed");
}

static void frame_rate_control(void)
{
    Fixture f;
    reset_ledger();
    for (unsigned epoch = 0; epoch < 3; ++epoch) {
        start(&f, 1);
        for (unsigned i = 0; i < 50; ++i)
            push(&f, epoch * 50 + i, epoch * 50 + i);
        cleanup(&f);
        all_released();
    }
    require(output_count == 150, "frame-rate control lost a picture");
    for (unsigned i = 0; i < 150; ++i)
        require(outputs[i].id == i && outputs[i].pts == i && outputs[i].field == 0,
                "frame-rate identity/PTS changed");
}

static void irregular_pts_and_upstream_status(void)
{
    static const int64_t expected[] = {0, 3, 6, 7, 8, 9};
    Fixture f;
    reset_ledger();
    start(&f, 2);
    push(&f, 0, 0);
    push(&f, 1, 3);
    push(&f, 2, 4);
    require(output_count == 4, "lookahead did not retain exactly one picture");
    upstream_status = AVERROR(EAGAIN);
    require(deint_vaapi_request_frame(&f.output) == AVERROR(EAGAIN) &&
                output_count == 4 && !f.ctx.eof,
            "temporary upstream absence was treated as EOF");
    upstream_status = AVERROR(EIO);
    require(deint_vaapi_request_frame(&f.output) == AVERROR(EIO) &&
                output_count == 4 && !f.ctx.eof,
            "upstream error was treated as EOF");
    finish_eof(&f);
    require(output_count == 6, "EOF did not request the retained last picture");
    for (unsigned i = 0; i < 6; ++i)
        require(outputs[i].pts == expected[i] && outputs[i].id == i / 2,
                "independent irregular-PTS oracle disagrees");
    cleanup(&f);
    all_released();
}

int main(void)
{
    int backing_a, backing_b;
    AVBufferRef a = {&backing_a}, alias = {&backing_a}, b = {&backing_b};
    require(!hardware_changed(NULL, NULL) && !hardware_changed(&a, &alias) &&
                hardware_changed(NULL, &a) == HWACCEL_CHANGED &&
                hardware_changed(&a, NULL) == HWACCEL_CHANGED &&
                hardware_changed(&a, &b) == HWACCEL_CHANGED,
            "actual hardware-context identity predicate changed");
    field_sequence(0, 0); /* one graph, final EOF: 300 */
    field_sequence(1, 0); /* two undrained teardowns: 296, not a repaired PASS */
    field_sequence(1, 1); /* counterfactual explicit EOF before every teardown: 300 */
    frame_rate_control();
    irregular_pts_and_upstream_status();
    puts("PASS: exact-source zero-reference Bob EOF/teardown differential, "
         "frame IDs/parities/PTS, reference returns and hardware-context predicate");
    puts("Scope: function/queue scaffold; no full FFmpeg, actual VA, pixels or hardware.");
    return 0;
}
