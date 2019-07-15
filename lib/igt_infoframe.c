/*
 * Copyright © 2019 Intel Corporation
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
 * Authors: Simon Ser <simon.ser@intel.com>
 */

#include "config.h"

#include <string.h>

#include "igt_infoframe.h"

/**
 * SECTION:igt_infoframe
 * @short_description: InfoFrame parsing library
 * @title: InfoFrame
 * @include: igt_infoframe.h
 *
 * This library provides helpers to parse InfoFrames as defined in CEA-861-D
 * section 6.
 */

static const int sampling_freqs[] = {
	-1, /* refer to stream header */
	33000,
	44100,
	48000,
	88200,
	96000,
	176400,
	192000,
};

static const size_t sampling_freqs_len = sizeof(sampling_freqs) / sizeof(sampling_freqs[0]);

static const int sample_sizes[] = {
	-1, /* refer to stream header */
	16,
	20,
	24,
};

static const size_t sample_sizes_len = sizeof(sample_sizes) / sizeof(sample_sizes[0]);

bool infoframe_audio_parse(struct infoframe_audio *infoframe, int version,
			   const uint8_t *buf, size_t buf_size)
{
	int channel_count;
	size_t sampling_freq_idx, sample_size_idx;

	memset(infoframe, 0, sizeof(*infoframe));

	if (version != 1 || buf_size < 5)
		return false;

	infoframe->coding_type = buf[0] >> 4;

	channel_count = buf[0] & 0x7;
	if (channel_count == 0)
		infoframe->channel_count = -1;
	else
		infoframe->channel_count = channel_count + 1;

	sampling_freq_idx = (buf[1] >> 2) & 0x7;
	if (sampling_freq_idx >= sampling_freqs_len)
		return false;
	infoframe->sampling_freq = sampling_freqs[sampling_freq_idx];

	sample_size_idx = buf[1] & 0x3;
	if (sample_size_idx >= sample_sizes_len)
		return false;
	infoframe->sample_size = sample_sizes[sample_size_idx];

	return true;
}
