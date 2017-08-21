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
 * Authors:
 *  Paul Kocialkowski <paul.kocialkowski@linux.intel.com>
 */

#ifndef IGT_AUDIO_H
#define IGT_AUDIO_H

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "igt.h"
#include <stdbool.h>

struct audio_signal;

struct audio_signal *audio_signal_init(int channels, int sampling_rate);
int audio_signal_add_frequency(struct audio_signal *signal, int frequency);
void audio_signal_synthesize(struct audio_signal *signal);
void audio_signal_clean(struct audio_signal *signal);
void audio_signal_fill(struct audio_signal *signal, short *buffer, int frames);
bool audio_signal_detect(struct audio_signal *signal, int channels,
			 int sampling_rate, short *buffer, int frames);

#endif
