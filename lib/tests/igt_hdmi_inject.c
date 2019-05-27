/*
 * Copyright © 2017 Intel Corporation
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

#include "igt.h"

static const unsigned char edid_header[] = {
	0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00
};

/**
 * Sanity check the header of the base EDID block.
 *
 * Return: 8 if the header is perfect, down to 0 if it's totally wrong.
 */
static int edid_header_is_valid(const unsigned char *raw_edid)
{
	int i, score = 0;

	for (i = 0; i < sizeof(edid_header); i++)
		if (raw_edid[i] == edid_header[i])
			score++;

	return score;
}

/**
 * Sanity check the checksum of the EDID block.
 *
 * Return: 0 if the block is perfect.
 * See byte 127 of spec
 * https://en.wikipedia.org/wiki/Extended_Display_Identification_Data#EDID_1.3_data_format
 */
static int edid_block_checksum(const unsigned char *raw_edid)
{
	int i;
	unsigned char csum = 0;
	for (i = 0; i < EDID_LENGTH; i++) {
		csum += raw_edid[i];
	}

	return csum;
}

typedef void (*hdmi_inject_func)(const unsigned char *edid, size_t length,
				 unsigned char *new_edid_ptr[], size_t *new_length);

igt_simple_main
{
	const struct {
		const char *desc;
		hdmi_inject_func inject;
	} funcs[] = {
		{ "3D", kmstest_edid_add_3d },
		{ "4k", kmstest_edid_add_4k },
		{ NULL, NULL },
	}, *f;

	for (f = funcs; f->inject; f++) {
		unsigned char *edid;
		size_t length;

		f->inject(igt_kms_get_base_edid(), EDID_LENGTH, &edid,
			  &length);

		igt_assert_f(edid_header_is_valid(edid) == 8,
			     "invalid header on HDMI %s", f->desc);
		/* check base edid block */
		igt_assert_f(edid_block_checksum(edid) == 0,
			     "checksum failed on HDMI %s", f->desc);
		/* check extension block */
		igt_assert_f(edid_block_checksum(edid + EDID_LENGTH) == 0,
			     "CEA block checksum failed on HDMI %s", f->desc);
	}
}
