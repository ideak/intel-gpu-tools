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

#ifndef IGT_KMS_COLOR_HELPER_H
#define IGT_KMS_COLOR_HELPER_H

/*
 * This header is for code that is shared between kms_color.c and
 * kms_color_chamelium.c. Reusability elsewhere can be questionable.
 */

#include <math.h>
#include <unistd.h>

#include "drm.h"
#include "drmtest.h"
#include "igt.h"


/* Internal */
typedef struct {
	double r, g, b;
} color_t;

typedef struct {
	int drm_fd;
	uint32_t devid;
	igt_display_t display;
	igt_pipe_crc_t *pipe_crc;

	uint32_t color_depth;
	uint64_t degamma_lut_size;
	uint64_t gamma_lut_size;
	#ifdef HAVE_CHAMELIUM
	struct chamelium *chamelium;
	struct chamelium_port **ports;
	int port_count;
	#endif
} data_t;

typedef struct {
	int size;
	double coeffs[];
} gamma_lut_t;

void paint_gradient_rectangles(data_t *data,
			       drmModeModeInfo *mode,
			       color_t *colors,
			       struct igt_fb *fb);
void paint_rectangles(data_t *data,
		      drmModeModeInfo *mode,
		      color_t *colors,
		      struct igt_fb *fb);
gamma_lut_t *alloc_lut(int lut_size);
void free_lut(gamma_lut_t *gamma);
gamma_lut_t *generate_table(int lut_size, double exp);
gamma_lut_t *generate_table_max(int lut_size);
gamma_lut_t *generate_table_zero(int lut_size);
struct drm_color_lut *coeffs_to_lut(data_t *data,
				    const gamma_lut_t *gamma,
				    uint32_t color_depth,
				    int off);
void set_degamma(data_t *data,
		 igt_pipe_t *pipe,
		 const gamma_lut_t *gamma);
void set_gamma(data_t *data,
	       igt_pipe_t *pipe,
	       const gamma_lut_t *gamma);
void set_ctm(igt_pipe_t *pipe, const double *coefficients);
void disable_prop(igt_pipe_t *pipe, enum igt_atomic_crtc_properties prop);

#define disable_degamma(pipe) disable_prop(pipe, IGT_CRTC_DEGAMMA_LUT)
#define disable_gamma(pipe) disable_prop(pipe, IGT_CRTC_GAMMA_LUT)
#define disable_ctm(pipe) disable_prop(pipe, IGT_CRTC_CTM)

drmModePropertyBlobPtr get_blob(data_t *data, igt_pipe_t *pipe,
				enum igt_atomic_crtc_properties prop);
bool crc_equal(igt_crc_t *a, igt_crc_t *b);
int pipe_set_property_blob_id(igt_pipe_t *pipe,
			      enum igt_atomic_crtc_properties prop,
			      uint32_t blob_id);
int pipe_set_property_blob(igt_pipe_t *pipe,
			   enum igt_atomic_crtc_properties prop,
			   void *ptr, size_t length);
void invalid_gamma_lut_sizes(data_t *data);
void invalid_degamma_lut_sizes(data_t *data);
void invalid_ctm_matrix_sizes(data_t *data);
#endif

