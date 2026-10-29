/*
 * V4L2 context helper functions.
 *
 * Copyright (C) 2017 Alexis Ballier <aballier@gentoo.org>
 * Copyright (C) 2017 Jorge Ramirez <jorge.ramirez-ortiz@linaro.org>
 *
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

int ff_v4l2_context_set_format(V4L2Context* ctx)
{
    int ret;

    ret = ioctl(ctx_to_m2mctx(ctx)->fd, VIDIOC_S_FMT, &ctx->format);
    if (ret != 0)
        return ret;

    // Check returned size against min size and if smaller have another go
    // Only worry about plane[0] as this is meant to enforce limits for
    // encoded streams where we might know a bit more about the shape
    // than the driver
    if (V4L2_TYPE_IS_MULTIPLANAR(ctx->format.type)) {
        if (ctx->min_buf_size <= ctx->format.fmt.pix_mp.plane_fmt[0].sizeimage)
            return 0;
        ctx->format.fmt.pix_mp.plane_fmt[0].sizeimage = ctx->min_buf_size;
    }
    else {
        if (ctx->min_buf_size <= ctx->format.fmt.pix.sizeimage)
            return 0;
        ctx->format.fmt.pix.sizeimage = ctx->min_buf_size;
    }

    ret = ioctl(ctx_to_m2mctx(ctx)->fd, VIDIOC_S_FMT, &ctx->format);
    return ret;
}
