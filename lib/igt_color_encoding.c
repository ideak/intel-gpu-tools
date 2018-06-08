/*
 * Copyright © 2018 Intel Corporation
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
 */

#include "igt_color_encoding.h"
#include "igt_matrix.h"
#include "igt_core.h"

struct color_encoding {
	float kr, kb;
};

static const struct color_encoding color_encodings[IGT_NUM_COLOR_ENCODINGS] = {
	[IGT_COLOR_YCBCR_BT601] = { .kr = .299f, .kb = .114f, },
	[IGT_COLOR_YCBCR_BT709] = { .kr = .2126f, .kb = .0722f, },
	[IGT_COLOR_YCBCR_BT2020] = { .kr = .2627f, .kb = .0593f, },
};

static struct igt_mat4 rgb_to_ycbcr_matrix(const struct color_encoding *e)
{
	float kr = e->kr;
	float kb = e->kb;
	float kg = 1.0f - kr - kb;

	struct igt_mat4 ret = {
		.d[m(0, 0)] = kr,
		.d[m(0, 1)] = kg,
		.d[m(0, 2)] = kb,

		.d[m(1, 0)] = -kr / (1.0f - kb),
		.d[m(1, 1)] = -kg / (1.0f - kb),
		.d[m(1, 2)] = 1.0f,

		.d[m(2, 0)] = 1.0f,
		.d[m(2, 1)] = -kg / (1.0f - kr),
		.d[m(2, 2)] = -kb / (1.0f - kr),

		.d[m(3, 3)] = 1.0f,
	};

	return ret;
}

static struct igt_mat4 ycbcr_to_rgb_matrix(const struct color_encoding *e)
{
	float kr = e->kr;
	float kb = e->kb;
	float kg = 1.0f - kr - kb;

	struct igt_mat4 ret = {
		.d[m(0, 0)] = 1.0f,
		.d[m(0, 1)] = 0.0f,
		.d[m(0, 2)] = 1.0 - kr,

		.d[m(1, 0)] = 1.0f,
		.d[m(1, 1)] = -(1.0 - kb) * kb / kg,
		.d[m(1, 2)] = -(1.0 - kr) * kr / kg,

		.d[m(2, 0)] = 1.0f,
		.d[m(2, 1)] = 1.0 - kb,
		.d[m(2, 2)] = 0.0f,

		.d[m(3, 3)] = 1.0f,
	};

	return ret;
}

static struct igt_mat4 ycbcr_input_convert_matrix(enum igt_color_range color_range)
{
	struct igt_mat4 t, s;

	if (color_range == IGT_COLOR_YCBCR_FULL_RANGE) {
		t = igt_matrix_translate(0.0f, -128.0f, -128.0f);
		s = igt_matrix_scale(1.0f, 2.0f, 2.0f);
	} else {
		t = igt_matrix_translate(-16.0f, -128.0f, -128.0f);
		s = igt_matrix_scale(255.0f / (235.0f - 16.0f),
				     255.0f / (240.0f - 128.0f),
				     255.0f / (240.0f - 128.0f));
	}

	return igt_matrix_multiply(&s, &t);
}

static struct igt_mat4 ycbcr_output_convert_matrix(enum igt_color_range color_range)
{
	struct igt_mat4 s, t;

	if (color_range == IGT_COLOR_YCBCR_FULL_RANGE) {
		s = igt_matrix_scale(1.0f, 0.5f, 0.5f);
		t = igt_matrix_translate(0.0f, 128.0f, 128.0f);
	} else {
		s = igt_matrix_scale((235.0f - 16.0f) / 255.0f,
				     (240.0f - 128.0f) / 255.0f,
				     (240.0f - 128.0f) / 255.0f);
		t = igt_matrix_translate(16.0f, 128.0f, 128.0f);
	}

	return igt_matrix_multiply(&t, &s);
}

struct igt_mat4 igt_ycbcr_to_rgb_matrix(enum igt_color_encoding color_encoding,
					enum igt_color_range color_range)
{
	const struct color_encoding *e = &color_encodings[color_encoding];
	struct igt_mat4 r, c;

	r = ycbcr_input_convert_matrix(color_range);
	c = ycbcr_to_rgb_matrix(e);

	return igt_matrix_multiply(&c, &r);
}

struct igt_mat4 igt_rgb_to_ycbcr_matrix(enum igt_color_encoding color_encoding,
					enum igt_color_range color_range)
{
	const struct color_encoding *e = &color_encodings[color_encoding];
	struct igt_mat4 c, r;

	c = rgb_to_ycbcr_matrix(e);
	r = ycbcr_output_convert_matrix(color_range);

	return igt_matrix_multiply(&r, &c);
}

const char *igt_color_encoding_to_str(enum igt_color_encoding encoding)
{
	switch (encoding) {
	case IGT_COLOR_YCBCR_BT601: return "ITU-R BT.601 YCbCr";
	case IGT_COLOR_YCBCR_BT709: return "ITU-R BT.709 YCbCr";
	case IGT_COLOR_YCBCR_BT2020: return "ITU-R BT.2020 YCbCr";
	default: igt_assert(0); return NULL;
	}
}

const char *igt_color_range_to_str(enum igt_color_range range)
{
	switch (range) {
	case IGT_COLOR_YCBCR_LIMITED_RANGE: return "YCbCr limited range";
	case IGT_COLOR_YCBCR_FULL_RANGE: return "YCbCr full range";
	default: igt_assert(0); return NULL;
	}
}
