/*
 * Copyright © 2015 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 *
 */

#include "kms_color_helper.h"

void paint_gradient_rectangles(data_t *data,
			       drmModeModeInfo *mode,
			       color_t *colors,
			       struct igt_fb *fb)
{
	cairo_t *cr = igt_get_cairo_ctx(data->drm_fd, fb);
	int i, l = mode->hdisplay / 3;
	int rows_remaining = mode->hdisplay % 3;

	/* Paint 3 gradient rectangles with red/green/blue between 1.0 and
	 * 0.5. We want to avoid 0 so each max LUTs only affect their own
	 * rectangle.
	 */
	for (i = 0 ; i < 3; i++) {
		igt_paint_color_gradient_range(cr, i * l, 0, l, mode->vdisplay,
					       colors[i].r != 0 ? 0.2 : 0,
					       colors[i].g != 0 ? 0.2 : 0,
					       colors[i].b != 0 ? 0.2 : 0,
					       colors[i].r,
					       colors[i].g,
					       colors[i].b);
	}

	if (rows_remaining > 0)
		igt_paint_color_gradient_range(cr, i * l, 0, rows_remaining,
					       mode->vdisplay,
					       colors[i-1].r != 0 ? 0.2 : 0,
					       colors[i-1].g != 0 ? 0.2 : 0,
					       colors[i-1].b != 0 ? 0.2 : 0,
					       colors[i-1].r,
					       colors[i-1].g,
					       colors[i-1].b);

	igt_put_cairo_ctx(cr);
}

void paint_rectangles(data_t *data,
		      drmModeModeInfo *mode,
		      color_t *colors,
		      struct igt_fb *fb)
{
	cairo_t *cr = igt_get_cairo_ctx(data->drm_fd, fb);
	int i, l = mode->hdisplay / 3;
	int rows_remaining = mode->hdisplay % 3;

	/* Paint 3 solid rectangles. */
	for (i = 0 ; i < 3; i++) {
		igt_paint_color(cr, i * l, 0, l, mode->vdisplay,
				colors[i].r, colors[i].g, colors[i].b);
	}

	if (rows_remaining > 0)
		igt_paint_color(cr, i * l, 0, rows_remaining, mode->vdisplay,
				colors[i-1].r, colors[i-1].g, colors[i-1].b);

	igt_put_cairo_ctx(cr);
}

gamma_lut_t *alloc_lut(int lut_size)
{
	gamma_lut_t *gamma;

	igt_assert_lt(0, lut_size);

	gamma = malloc(sizeof(*gamma) + lut_size * sizeof(gamma->coeffs[0]));
	igt_assert(gamma);
	gamma->size = lut_size;

	return gamma;
}

void free_lut(gamma_lut_t *gamma)
{
	if (!gamma)
		return;

	free(gamma);
}

gamma_lut_t *generate_table(int lut_size, double exp)
{
	gamma_lut_t *gamma = alloc_lut(lut_size);
	int i;

	gamma->coeffs[0] = 0.0;
	for (i = 1; i < lut_size; i++)
		gamma->coeffs[i] = pow(i * 1.0 / (lut_size - 1), exp);

	return gamma;
}

gamma_lut_t *generate_table_max(int lut_size)
{
	gamma_lut_t *gamma = alloc_lut(lut_size);
	int i;

	gamma->coeffs[0] = 0.0;
	for (i = 1; i < lut_size; i++)
		gamma->coeffs[i] = 1.0;

	return gamma;
}

gamma_lut_t *generate_table_zero(int lut_size)
{
	gamma_lut_t *gamma = alloc_lut(lut_size);
	int i;

	for (i = 0; i < lut_size; i++)
		gamma->coeffs[i] = 0.0;

	return gamma;
}

struct drm_color_lut *coeffs_to_lut(data_t *data,
				    const gamma_lut_t *gamma,
				    uint32_t color_depth,
				    int off)
{
	struct drm_color_lut *lut;
	int i, lut_size = gamma->size;
	uint32_t max_value = (1 << 16) - 1;
	uint32_t mask;

	if (is_i915_device(data->drm_fd))
		mask = ((1 << color_depth) - 1) << 8;
	else
		mask = max_value;

	lut = malloc(sizeof(struct drm_color_lut) * lut_size);

	if (IS_CHERRYVIEW(data->devid))
		lut_size -= 1;
	for (i = 0; i < lut_size; i++) {
		uint32_t v = (gamma->coeffs[i] * max_value);

		/*
		 * Hardware might encode colors on a different number of bits
		 * than what is in our framebuffer (10 or 12bits for example).
		 * Mask the lower bits not provided by the framebuffer so we
		 * can do CRC comparisons.
		 */
		v &= mask;

		lut[i].red = v;
		lut[i].green = v;
		lut[i].blue = v;
	}

	if (IS_CHERRYVIEW(data->devid))
		lut[lut_size].red =
			lut[lut_size].green =
			lut[lut_size].blue = lut[lut_size - 1].red;

	return lut;
}

void set_degamma(data_t *data,
		 igt_pipe_t *pipe,
		 const gamma_lut_t *gamma)
{
	size_t size = sizeof(struct drm_color_lut) * gamma->size;
	struct drm_color_lut *lut = coeffs_to_lut(data, gamma,
						  data->color_depth, 0);

	igt_pipe_obj_replace_prop_blob(pipe, IGT_CRTC_DEGAMMA_LUT, lut, size);

	free(lut);
}

void set_gamma(data_t *data,
	       igt_pipe_t *pipe, const gamma_lut_t *gamma)
{
	size_t size = sizeof(struct drm_color_lut) * gamma->size;
	struct drm_color_lut *lut = coeffs_to_lut(data, gamma,
						  data->color_depth, 0);

	igt_pipe_obj_replace_prop_blob(pipe, IGT_CRTC_GAMMA_LUT, lut, size);

	free(lut);
}

void set_ctm(igt_pipe_t *pipe, const double *coefficients)
{
	struct drm_color_ctm ctm;
	int i;

	for (i = 0; i < ARRAY_SIZE(ctm.matrix); i++) {
		if (coefficients[i] < 0) {
			ctm.matrix[i] =
				(int64_t) (-coefficients[i] *
				((int64_t) 1L << 32));
			ctm.matrix[i] |= 1ULL << 63;
		} else
			ctm.matrix[i] =
				(int64_t) (coefficients[i] *
				((int64_t) 1L << 32));
	}

	igt_pipe_obj_replace_prop_blob(pipe, IGT_CRTC_CTM, &ctm, sizeof(ctm));
}

void disable_prop(igt_pipe_t *pipe, enum igt_atomic_crtc_properties prop)
{
	if (igt_pipe_obj_has_prop(pipe, prop))
		igt_pipe_obj_replace_prop_blob(pipe, prop, NULL, 0);
}

drmModePropertyBlobPtr
get_blob(data_t *data, igt_pipe_t *pipe, enum igt_atomic_crtc_properties prop)
{
	uint64_t prop_value;

	prop_value = igt_pipe_obj_get_prop(pipe, prop);

	if (prop_value == 0)
		return NULL;

	return drmModeGetPropertyBlob(data->drm_fd, prop_value);
}

bool crc_equal(igt_crc_t *a, igt_crc_t *b)
{
	return memcmp(a->crc, b->crc, sizeof(a->crc[0]) * a->n_words) == 0;
}

int
pipe_set_property_blob_id(igt_pipe_t *pipe,
			  enum igt_atomic_crtc_properties prop,
			  uint32_t blob_id)
{
	int ret;

	igt_pipe_obj_replace_prop_blob(pipe, prop, NULL, 0);

	igt_pipe_obj_set_prop_value(pipe, prop, blob_id);

	ret = igt_display_try_commit2(pipe->display,
				      pipe->display->is_atomic ?
				      COMMIT_ATOMIC : COMMIT_LEGACY);

	igt_pipe_obj_set_prop_value(pipe, prop, 0);

	return ret;
}

int
pipe_set_property_blob(igt_pipe_t *pipe,
		       enum igt_atomic_crtc_properties prop,
		       void *ptr, size_t length)
{
	igt_pipe_obj_replace_prop_blob(pipe, prop, ptr, length);

	return igt_display_try_commit2(pipe->display,
				       pipe->display->is_atomic ?
				       COMMIT_ATOMIC : COMMIT_LEGACY);
}

void
invalid_gamma_lut_sizes(data_t *data)
{
	igt_display_t *display = &data->display;
	igt_pipe_t *pipe = &display->pipes[0];
	size_t gamma_lut_size = data->gamma_lut_size *
				sizeof(struct drm_color_lut);
	struct drm_color_lut *gamma_lut;

	igt_require(igt_pipe_obj_has_prop(pipe, IGT_CRTC_GAMMA_LUT));

	gamma_lut = malloc(gamma_lut_size * 2);

	igt_display_commit2(display,
			    display->is_atomic ?
			    COMMIT_ATOMIC : COMMIT_LEGACY);

	igt_assert_eq(pipe_set_property_blob(pipe, IGT_CRTC_GAMMA_LUT,
					     gamma_lut, 1), -EINVAL);
	igt_assert_eq(pipe_set_property_blob(pipe, IGT_CRTC_GAMMA_LUT,
					     gamma_lut, gamma_lut_size + 1),
					     -EINVAL);
	igt_assert_eq(pipe_set_property_blob(pipe, IGT_CRTC_GAMMA_LUT,
					     gamma_lut, gamma_lut_size - 1),
					     -EINVAL);
	igt_assert_eq(pipe_set_property_blob(pipe, IGT_CRTC_GAMMA_LUT,
					     gamma_lut, gamma_lut_size +
					     sizeof(struct drm_color_lut)),
					     -EINVAL);
	igt_assert_eq(pipe_set_property_blob_id(pipe, IGT_CRTC_GAMMA_LUT,
		      pipe->crtc_id), -EINVAL);
	igt_assert_eq(pipe_set_property_blob_id(pipe, IGT_CRTC_GAMMA_LUT,
		      4096 * 4096), -EINVAL);

	free(gamma_lut);
}

void
invalid_degamma_lut_sizes(data_t *data)
{
	igt_display_t *display = &data->display;
	igt_pipe_t *pipe = &display->pipes[0];
	size_t degamma_lut_size = data->degamma_lut_size *
				  sizeof(struct drm_color_lut);
	struct drm_color_lut *degamma_lut;

	igt_require(igt_pipe_obj_has_prop(pipe, IGT_CRTC_DEGAMMA_LUT));

	degamma_lut = malloc(degamma_lut_size * 2);

	igt_display_commit2(display, display->is_atomic ?
			    COMMIT_ATOMIC : COMMIT_LEGACY);

	igt_assert_eq(pipe_set_property_blob(pipe, IGT_CRTC_DEGAMMA_LUT,
					     degamma_lut, 1), -EINVAL);
	igt_assert_eq(pipe_set_property_blob(pipe, IGT_CRTC_DEGAMMA_LUT,
					     degamma_lut, degamma_lut_size + 1),
					     -EINVAL);
	igt_assert_eq(pipe_set_property_blob(pipe, IGT_CRTC_DEGAMMA_LUT,
					     degamma_lut, degamma_lut_size - 1),
					     -EINVAL);
	igt_assert_eq(pipe_set_property_blob(pipe, IGT_CRTC_DEGAMMA_LUT,
					     degamma_lut, degamma_lut_size +
					     sizeof(struct drm_color_lut)),
					     -EINVAL);
	igt_assert_eq(pipe_set_property_blob_id(pipe, IGT_CRTC_DEGAMMA_LUT,
						pipe->crtc_id), -EINVAL);
	igt_assert_eq(pipe_set_property_blob_id(pipe, IGT_CRTC_DEGAMMA_LUT,
						4096 * 4096), -EINVAL);

	free(degamma_lut);
}

void invalid_ctm_matrix_sizes(data_t *data)
{
	igt_display_t *display = &data->display;
	igt_pipe_t *pipe = &display->pipes[0];
	void *ptr;

	igt_require(igt_pipe_obj_has_prop(pipe, IGT_CRTC_CTM));

	ptr = malloc(sizeof(struct drm_color_ctm) * 4);

	igt_assert_eq(pipe_set_property_blob(pipe, IGT_CRTC_CTM, ptr, 1),
					     -EINVAL);
	igt_assert_eq(pipe_set_property_blob(pipe, IGT_CRTC_CTM, ptr,
					     sizeof(struct drm_color_ctm) + 1),
					     -EINVAL);
	igt_assert_eq(pipe_set_property_blob(pipe, IGT_CRTC_CTM, ptr,
					     sizeof(struct drm_color_ctm) - 1),
					     -EINVAL);
	igt_assert_eq(pipe_set_property_blob(pipe, IGT_CRTC_CTM, ptr,
					     sizeof(struct drm_color_ctm) * 2),
					     -EINVAL);
	igt_assert_eq(pipe_set_property_blob_id(pipe, IGT_CRTC_CTM,
						pipe->crtc_id), -EINVAL);
	igt_assert_eq(pipe_set_property_blob_id(pipe, IGT_CRTC_CTM,
						4096 * 4096), -EINVAL);

	free(ptr);
}

