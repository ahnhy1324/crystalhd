/*
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

static int deint_vaapi_filter_frame(AVFilterLink *inlink, AVFrame *input_frame)
{
    AVFilterContext   *avctx = inlink->dst;
    AVFilterLink    *outlink = avctx->outputs[0];
    VAAPIVPPContext *vpp_ctx = avctx->priv;
    DeintVAAPIContext *ctx   = avctx->priv;
    AVFrame *output_frame    = NULL;
    VASurfaceID input_surface;
    VASurfaceID backward_references[MAX_REFERENCES];
    VASurfaceID forward_references[MAX_REFERENCES];
    VAProcPipelineParameterBuffer params;
    VAProcFilterParameterBufferDeinterlacing *filter_params;
    VAStatus vas;
    void *filter_params_addr = NULL;
    int err, i, field, current_frame_index;

    // NULL frame is used to flush the queue in field mode
    if (input_frame)
        av_log(avctx, AV_LOG_DEBUG, "Filter input: %s, %ux%u (%"PRId64").\n",
            av_get_pix_fmt_name(input_frame->format),
            input_frame->width, input_frame->height, input_frame->pts);

    if (ctx->queue_count < ctx->queue_depth) {
        ctx->frame_queue[ctx->queue_count++] = input_frame;
        if (ctx->queue_count < ctx->queue_depth) {
            // Need more reference surfaces before we can continue.
            return 0;
        }
    } else {
        av_frame_free(&ctx->frame_queue[0]);
        for (i = 0; i + 1 < ctx->queue_count; i++)
            ctx->frame_queue[i] = ctx->frame_queue[i + 1];
        ctx->frame_queue[i] = input_frame;
    }

    current_frame_index = ctx->pipeline_caps.num_forward_references;

    input_frame = ctx->frame_queue[current_frame_index];
    if (!input_frame)
        return 0;

    input_surface = (VASurfaceID)(uintptr_t)input_frame->data[3];
    for (i = 0; i < ctx->pipeline_caps.num_forward_references; i++)
        forward_references[i] = (VASurfaceID)(uintptr_t)
            ctx->frame_queue[current_frame_index - i - 1]->data[3];
    for (i = 0; i < ctx->pipeline_caps.num_backward_references; i++)
        backward_references[i] = (VASurfaceID)(uintptr_t)
            ctx->frame_queue[current_frame_index + i + 1]->data[3];

    av_log(avctx, AV_LOG_DEBUG, "Using surface %#x for "
           "deinterlace input.\n", input_surface);
    av_log(avctx, AV_LOG_DEBUG, "Backward references:");
    for (i = 0; i < ctx->pipeline_caps.num_backward_references; i++)
        av_log(avctx, AV_LOG_DEBUG, " %#x", backward_references[i]);
    av_log(avctx, AV_LOG_DEBUG, "\n");
    av_log(avctx, AV_LOG_DEBUG, "Forward  references:");
    for (i = 0; i < ctx->pipeline_caps.num_forward_references; i++)
        av_log(avctx, AV_LOG_DEBUG, " %#x", forward_references[i]);
    av_log(avctx, AV_LOG_DEBUG, "\n");

    for (field = 0; field < ctx->field_rate; field++) {
        output_frame = ff_get_video_buffer(outlink, vpp_ctx->output_width,
                                           vpp_ctx->output_height);
        if (!output_frame) {
            err = AVERROR(ENOMEM);
            goto fail;
        }

        err = av_frame_copy_props(output_frame, input_frame);
        if (err < 0)
            goto fail;

        err = ff_vaapi_vpp_init_params(avctx, &params,
                                       input_frame, output_frame);
        if (err < 0)
            goto fail;

        if (!ctx->auto_enable || (input_frame->flags & AV_FRAME_FLAG_INTERLACED)) {
            vas = vaMapBuffer(vpp_ctx->hwctx->display, vpp_ctx->filter_buffers[0],
                              &filter_params_addr);
            if (vas != VA_STATUS_SUCCESS) {
                av_log(avctx, AV_LOG_ERROR, "Failed to map filter parameter "
                       "buffer: %d (%s).\n", vas, vaErrorStr(vas));
                err = AVERROR(EIO);
                goto fail;
            }
            filter_params = filter_params_addr;
            filter_params->flags = 0;
            if (input_frame->flags & AV_FRAME_FLAG_TOP_FIELD_FIRST) {
                filter_params->flags |= field ? VA_DEINTERLACING_BOTTOM_FIELD : 0;
            } else {
                filter_params->flags |= VA_DEINTERLACING_BOTTOM_FIELD_FIRST;
                filter_params->flags |= field ? 0 : VA_DEINTERLACING_BOTTOM_FIELD;
            }
            filter_params_addr = NULL;
            vas = vaUnmapBuffer(vpp_ctx->hwctx->display, vpp_ctx->filter_buffers[0]);
            if (vas != VA_STATUS_SUCCESS)
                av_log(avctx, AV_LOG_ERROR, "Failed to unmap filter parameter "
                       "buffer: %d (%s).\n", vas, vaErrorStr(vas));

            params.filters     = &vpp_ctx->filter_buffers[0];
            params.num_filters = 1;

            params.forward_references = forward_references;
            params.num_forward_references =
                ctx->pipeline_caps.num_forward_references;
            params.backward_references = backward_references;
            params.num_backward_references =
                ctx->pipeline_caps.num_backward_references;

        } else {
            params.filters     = NULL;
            params.num_filters = 0;
        }

        err = ff_vaapi_vpp_render_picture(avctx, &params, output_frame);
        if (err < 0)
            goto fail;

        if (ctx->field_rate == 2) {
            if (field == 0)
                output_frame->pts = 2 * input_frame->pts;
            else if (ctx->eof)
                output_frame->pts = 3 * input_frame->pts - ctx->prev_pts;
            else
                output_frame->pts = input_frame->pts +
                    ctx->frame_queue[current_frame_index + 1]->pts;
        }
        output_frame->flags &= ~AV_FRAME_FLAG_INTERLACED;

        av_log(avctx, AV_LOG_DEBUG, "Filter output: %s, %ux%u (%"PRId64").\n",
               av_get_pix_fmt_name(output_frame->format),
               output_frame->width, output_frame->height, output_frame->pts);

        err = ff_filter_frame(outlink, output_frame);
        if (err < 0)
            break;
    }

    ctx->prev_pts = input_frame->pts;

    return err;

fail:
    if (filter_params_addr)
        vaUnmapBuffer(vpp_ctx->hwctx->display, vpp_ctx->filter_buffers[0]);
    av_frame_free(&output_frame);
    return err;
}
