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

#include "config.h"
#include "igt.h"

#define PLAYBACK_CHANNELS	2
#define PLAYBACK_FRAMES		1024

#define CAPTURE_SAMPLE_RATE	48000
#define CAPTURE_CHANNELS	2
#define CAPTURE_DEVICE_NAME	"default"
#define CAPTURE_FRAMES		2048

#define RUN_TIMEOUT		2000

struct test_data {
	struct alsa *alsa;
	struct audio_signal *signal;

	int streak;
};

static int sampling_rates[] = {
	32000,
	44100,
	48000,
	88200,
	96000,
	176400,
	192000,
};

static int sampling_rates_count = sizeof(sampling_rates) / sizeof(int);

static int test_frequencies[] = {
	300,
	600,
	1200,
	80000,
	10000,
};

static int test_frequencies_count = sizeof(test_frequencies) / sizeof(int);

static int output_callback(void *data, short *buffer, int frames)
{
	struct test_data *test_data = (struct test_data *) data;

	audio_signal_fill(test_data->signal, buffer, frames);

	return 0;
}

static int input_callback(void *data, short *buffer, int frames)
{
	struct test_data *test_data = (struct test_data *) data;
	bool detect;

	detect = audio_signal_detect(test_data->signal, CAPTURE_CHANNELS,
				     CAPTURE_SAMPLE_RATE, buffer, frames);
	if (detect)
		test_data->streak++;
	else
		test_data->streak = 0;

	/* A streak of 3 gives confidence that the signal is good. */
	if (test_data->streak == 3)
		return 1;

	return 0;
}

static void test_integrity(const char *device_name)
{
	struct test_data data;
	int sampling_rate;
	bool run = false;
	bool test;
	int i, j;
	int ret;

	data.alsa = alsa_init();
	igt_assert(data.alsa);

	ret = alsa_open_input(data.alsa, CAPTURE_DEVICE_NAME);
	igt_assert(ret >= 0);

	alsa_configure_input(data.alsa, CAPTURE_CHANNELS,
			     CAPTURE_SAMPLE_RATE);

	alsa_register_input_callback(data.alsa, input_callback, &data,
				     CAPTURE_FRAMES);

	for (i = 0; i < sampling_rates_count; i++) {
		ret = alsa_open_output(data.alsa, device_name);
		igt_assert(ret >= 0);

		sampling_rate = sampling_rates[i];

		test = alsa_test_output_configuration(data.alsa,
						      PLAYBACK_CHANNELS,
						      sampling_rate);
		if (!test) {
			alsa_close_output(data.alsa);
			continue;
		}

		igt_debug("Testing with sampling rate %d\n", sampling_rate);

		alsa_configure_output(data.alsa, PLAYBACK_CHANNELS,
				       sampling_rate);

		data.signal = audio_signal_init(PLAYBACK_CHANNELS,
						sampling_rate);
		igt_assert(data.signal);

		for (j = 0; j < test_frequencies_count; j++)
			audio_signal_add_frequency(data.signal,
						   test_frequencies[j]);

		audio_signal_synthesize(data.signal);

		alsa_register_output_callback(data.alsa, output_callback,
					      &data, PLAYBACK_FRAMES);

		data.streak = 0;

		ret = alsa_run(data.alsa, RUN_TIMEOUT);
		igt_assert(ret > 0);

		audio_signal_clean(data.signal);
		free(data.signal);

		alsa_close_output(data.alsa);

		run = true;
	}

	/* Make sure we tested at least one frequency */
	igt_assert(run);

	alsa_close_input(data.alsa);
	free(data.alsa);
}

igt_main
{
	igt_subtest("hdmi-integrity")
		test_integrity("HDMI");
}
