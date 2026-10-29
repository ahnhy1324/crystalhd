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

static int deint_vaapi_build_filter_params(AVFilterContext *avctx)
{
    VAAPIVPPContext *vpp_ctx = avctx->priv;
    DeintVAAPIContext *ctx   = avctx->priv;
    VAStatus vas;
    VAProcFilterParameterBufferDeinterlacing params;
    int i;

    ctx->nb_deint_caps = VAProcDeinterlacingCount;
    vas = vaQueryVideoProcFilterCaps(vpp_ctx->hwctx->display,
                                     vpp_ctx->va_context,
                                     VAProcFilterDeinterlacing,
                                     &ctx->deint_caps,
                                     &ctx->nb_deint_caps);
    if (vas != VA_STATUS_SUCCESS) {
        av_log(avctx, AV_LOG_ERROR, "Failed to query deinterlacing "
               "caps: %d (%s).\n", vas, vaErrorStr(vas));
        return AVERROR(EIO);
    }

    if (ctx->mode == VAProcDeinterlacingNone) {
        for (i = 0; i < ctx->nb_deint_caps; i++) {
            if (ctx->deint_caps[i].type > ctx->mode)
                ctx->mode = ctx->deint_caps[i].type;
        }
        av_log(avctx, AV_LOG_VERBOSE, "Picking %d (%s) as default "
               "deinterlacing mode.\n", ctx->mode,
               deint_vaapi_mode_name(ctx->mode));
    } else {
        for (i = 0; i < ctx->nb_deint_caps; i++) {
            if (ctx->deint_caps[i].type == ctx->mode)
                break;
        }
        if (i >= ctx->nb_deint_caps) {
            av_log(avctx, AV_LOG_ERROR, "Deinterlacing mode %d (%s) is "
                   "not supported.\n", ctx->mode,
                   deint_vaapi_mode_name(ctx->mode));
            return AVERROR(EINVAL);
        }
    }

    params.type      = VAProcFilterDeinterlacing;
    params.algorithm = ctx->mode;
    params.flags     = 0;

    vas = ff_vaapi_vpp_make_param_buffers(avctx,
                                          VAProcFilterParameterBufferType,
                                          &params,
                                          sizeof(params),
                                          1);
    if (vas)
        return vas;

    vas = vaQueryVideoProcPipelineCaps(vpp_ctx->hwctx->display,
                                       vpp_ctx->va_context,
                                       &vpp_ctx->filter_buffers[0], 1,
                                       &ctx->pipeline_caps);
    if (vas != VA_STATUS_SUCCESS) {
        av_log(avctx, AV_LOG_ERROR, "Failed to query pipeline "
               "caps: %d (%s).\n", vas, vaErrorStr(vas));
        return AVERROR(EIO);
    }

    ctx->extra_delay_for_timestamps = ctx->field_rate == 2 &&
        ctx->pipeline_caps.num_backward_references == 0;

    ctx->queue_depth = ctx->pipeline_caps.num_backward_references +
                       ctx->pipeline_caps.num_forward_references +
                       ctx->extra_delay_for_timestamps + 1;
    if (ctx->queue_depth > MAX_REFERENCES) {
        av_log(avctx, AV_LOG_ERROR, "Pipeline requires too many "
               "references (%u forward, %u back).\n",
               ctx->pipeline_caps.num_forward_references,
               ctx->pipeline_caps.num_backward_references);
        return AVERROR(ENOSYS);
    }

    return 0;
}
