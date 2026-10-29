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

static int deint_vaapi_request_frame(AVFilterLink *link)
{
    AVFilterContext *avctx = link->src;
    DeintVAAPIContext *ctx   = avctx->priv;
    int ret;

    if (ctx->eof)
        return AVERROR_EOF;

    ret = ff_request_frame(link->src->inputs[0]);
    if (ret == AVERROR_EOF && ctx->extra_delay_for_timestamps) {
        ctx->eof = 1;
        deint_vaapi_filter_frame(link->src->inputs[0], NULL);
    } else if (ret < 0)
        return ret;

    return 0;
}
