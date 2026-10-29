/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
    v4l2 common internal API header

    This header contains internal shared ioctl definitions for use by the
    internal low-level v4l2 drivers.
    Each ioctl begins with VIDIOC_INT_ to clearly mark that it is an internal
    define,

    Copyright (C) 2005  Hans Verkuil <hverkuil@kernel.org>

 */


static inline bool v4l2_is_colorspace_valid(__u32 colorspace)
{
	return colorspace > V4L2_COLORSPACE_DEFAULT &&
	       colorspace < V4L2_COLORSPACE_LAST;
}

static inline bool v4l2_is_xfer_func_valid(__u32 xfer_func)
{
	return xfer_func > V4L2_XFER_FUNC_DEFAULT &&
	       xfer_func < V4L2_XFER_FUNC_LAST;
}

static inline bool v4l2_is_ycbcr_enc_valid(__u8 ycbcr_enc)
{
	return ycbcr_enc > V4L2_YCBCR_ENC_DEFAULT &&
	       ycbcr_enc < V4L2_YCBCR_ENC_LAST;
}

static inline bool v4l2_is_hsv_enc_valid(__u8 hsv_enc)
{
	return hsv_enc == V4L2_HSV_ENC_180 || hsv_enc == V4L2_HSV_ENC_256;
}

static inline bool v4l2_is_quant_valid(__u8 quantization)
{
	return quantization == V4L2_QUANTIZATION_FULL_RANGE ||
	       quantization == V4L2_QUANTIZATION_LIM_RANGE;
}
