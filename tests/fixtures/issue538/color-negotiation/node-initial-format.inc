// SPDX-License-Identifier: GPL-2.0-or-later

	f->output.pixelformat = V4L2_PIX_FMT_H264;
	f->output.width = 640;
	f->output.height = 480;
	f->output.colorspace = V4L2_COLORSPACE_SMPTE170M;
	f->output.priv = V4L2_PIX_FMT_PRIV_MAGIC;
	f->color_default = true;
	f->output.sizeimage = CHD_CODED_SIZE;
	f->output.field = V4L2_FIELD_NONE;
	f->capture.pixelformat = V4L2_PIX_FMT_YUYV;
	f->capture.width = 640;
	f->capture.height = 480;
	f->capture.bytesperline = 1280;
	f->capture.field = V4L2_FIELD_NONE;
	f->capture.colorspace = V4L2_COLORSPACE_SMPTE170M;
	f->capture.priv = V4L2_PIX_FMT_PRIV_MAGIC;
	crystalhd_v4l2_capture_size(640, 480, &f->capture.sizeimage);
