// SPDX-License-Identifier: GPL-2.0-or-later

static void chd_copy_colors(struct v4l2_pix_format *dst,
			    const struct v4l2_pix_format *src)
{
	dst->colorspace = src->colorspace;
	dst->xfer_func = src->xfer_func;
	dst->ycbcr_enc = src->ycbcr_enc;
	dst->quantization = src->quantization;
}

static bool chd_default_colorspace(u32 colorspace)
{
	return !v4l2_is_colorspace_valid(colorspace) || colorspace == V4L2_COLORSPACE_BT878;
}

static void chd_normalize_colors(struct v4l2_pix_format *p)
{
	if (chd_default_colorspace(p->colorspace))
		p->colorspace = p->width <= 720 && p->height <= 576 ?
			V4L2_COLORSPACE_SMPTE170M : V4L2_COLORSPACE_REC709;
	if (p->priv != V4L2_PIX_FMT_PRIV_MAGIC) {
		p->xfer_func = V4L2_XFER_FUNC_DEFAULT;
		p->ycbcr_enc = V4L2_YCBCR_ENC_DEFAULT;
		p->quantization = V4L2_QUANTIZATION_DEFAULT;
	}
	if (!v4l2_is_xfer_func_valid(p->xfer_func))
		p->xfer_func = V4L2_XFER_FUNC_DEFAULT;
	if (p->ycbcr_enc > U8_MAX || !v4l2_is_ycbcr_enc_valid(p->ycbcr_enc))
		p->ycbcr_enc = V4L2_YCBCR_ENC_DEFAULT;
	if (p->quantization > U8_MAX || !v4l2_is_quant_valid(p->quantization))
		p->quantization = V4L2_QUANTIZATION_DEFAULT;
}

static void chd_source_colors(struct crystalhd_v4l2_file *f)
{
	/* PIB colour_primaries alone does not specify matrix/transfer/range.
	 * Keep explicit client colorimetry; only the unspecified SD/HD default
	 * follows newly discovered dimensions. No color conversion is advertised.
	 */
	if (f->color_default)
		f->output.colorspace = f->output.width <= 720 && f->output.height <= 576 ?
			V4L2_COLORSPACE_SMPTE170M : V4L2_COLORSPACE_REC709;
	chd_copy_colors(&f->capture, &f->output);
}
