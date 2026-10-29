// SPDX-License-Identifier: GPL-2.0-or-later

static int chd_try_fmt_locked(struct file *file, void *fh, struct v4l2_format *fmt)
{
	struct crystalhd_v4l2_file *f = chd_file(file->private_data);
	struct v4l2_pix_format *p = &fmt->fmt.pix;
	bool output = V4L2_TYPE_IS_OUTPUT(fmt->type);

	(void)fh;
	p->pixelformat = output ? V4L2_PIX_FMT_H264 : V4L2_PIX_FMT_YUYV;
	p->field = V4L2_FIELD_NONE;
	if (output) {
		p->width = clamp_t(u32, p->width, 16, 2048) & ~1U;
		p->height = clamp_t(u32, p->height, 16, 1091);
		p->bytesperline = 0;
		p->sizeimage = clamp_t(u32, p->sizeimage, CHD_CODED_SIZE, 8U * 1024U * 1024U);
		chd_normalize_colors(p);
	} else {
		if (f->format_known) {
			p->width = f->capture.width;
			p->height = f->capture.height;
		} else {
			p->width = clamp_t(u32, p->width, 16, 2048) & ~1U;
			p->height = clamp_t(u32, p->height, 16, 1091);
		}
		p->bytesperline = p->width * 2;
		crystalhd_v4l2_capture_size(p->width, p->height, &p->sizeimage);
		chd_copy_colors(p, &f->capture);
	}
	p->priv = V4L2_PIX_FMT_PRIV_MAGIC;
	p->flags = 0;
	return 0;
}

static int chd_try_fmt(struct file *file, void *fh, struct v4l2_format *fmt)
{
	struct crystalhd_v4l2_file *f = chd_file(file->private_data);
	int rc;

	mutex_lock(&f->run_lock);
	rc = chd_try_fmt_locked(file, fh, fmt);
	mutex_unlock(&f->run_lock);
	return rc;
}

static int chd_get_fmt(struct file *file, void *fh, struct v4l2_format *fmt)
{
	struct crystalhd_v4l2_file *f = chd_file(file->private_data);

	(void)fh;
	mutex_lock(&f->run_lock);
	fmt->fmt.pix = V4L2_TYPE_IS_OUTPUT(fmt->type) ? f->output : f->capture;
	mutex_unlock(&f->run_lock);
	return 0;
}

static int chd_set_fmt(struct file *file, void *fh, struct v4l2_format *fmt)
{
	struct crystalhd_v4l2_file *f = chd_file(file->private_data);
	struct vb2_queue *q = v4l2_m2m_get_vq(f->fh.m2m_ctx, fmt->type);
	bool color_default = chd_default_colorspace(fmt->fmt.pix.colorspace);
	int rc;

	if (!q)
		return -EINVAL;
	if (vb2_is_busy(q) ||
	    (V4L2_TYPE_IS_OUTPUT(fmt->type) &&
	     vb2_is_busy(v4l2_m2m_get_dst_vq(f->fh.m2m_ctx))))
		return -EBUSY;
	mutex_lock(&f->run_lock);
	rc = chd_try_fmt_locked(file, fh, fmt);
	if (!rc) {
		if (V4L2_TYPE_IS_OUTPUT(fmt->type)) {
			f->output = fmt->fmt.pix;
			f->color_default = color_default;
			f->format_known = false;
			f->capture.width = clamp_t(u32, f->output.width, 16, 2048) & ~1U;
			f->capture.height = clamp_t(u32, f->output.height, 16, 1091);
			f->capture.bytesperline = f->capture.width * 2;
			crystalhd_v4l2_capture_size(f->capture.width, f->capture.height,
						  &f->capture.sizeimage);
			chd_source_colors(f);
		} else
			f->capture = fmt->fmt.pix;
	}
	mutex_unlock(&f->run_lock);
	return rc;
}
