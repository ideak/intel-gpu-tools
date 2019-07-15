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

#ifndef IGT_INFOFRAME_H
#define IGT_INFOFRAME_H

#include "config.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum infoframe_audio_coding_type {
	INFOFRAME_AUDIO_CT_UNSPECIFIED = 0, /* refer to stream header */
	INFOFRAME_AUDIO_CT_PCM = 1, /* IEC 60958 PCM */
	INFOFRAME_AUDIO_CT_AC3 = 2,
	INFOFRAME_AUDIO_CT_MPEG1 = 3,
	INFOFRAME_AUDIO_CT_MP3 = 4,
	INFOFRAME_AUDIO_CT_MPEG2 = 5,
	INFOFRAME_AUDIO_CT_AAC = 6,
	INFOFRAME_AUDIO_CT_DTS = 7,
	INFOFRAME_AUDIO_CT_ATRAC = 8,
	INFOFRAME_AUDIO_CT_ONE_BIT = 9,
	INFOFRAME_AUDIO_CT_DOLBY = 10, /* Dolby Digital + */
	INFOFRAME_AUDIO_CT_DTS_HD = 11,
	INFOFRAME_AUDIO_CT_MAT = 12,
	INFOFRAME_AUDIO_CT_DST = 13,
	INFOFRAME_AUDIO_CT_WMA_PRO = 14,
};

struct infoframe_audio {
	enum infoframe_audio_coding_type coding_type;
	int channel_count; /* -1 if unspecified */
	int sampling_freq; /* in Hz, -1 if unspecified */
	int sample_size; /* in bits, -1 if unspecified */
	/* TODO: speaker allocation */
};

bool infoframe_audio_parse(struct infoframe_audio *infoframe, int version,
			   const uint8_t *buf, size_t buf_size);

#endif
