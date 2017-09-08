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

#include <math.h>
#include <gsl/gsl_fft_real.h>

#include "igt_audio.h"
#include "igt_core.h"

#define FREQS_MAX	8

/**
 * SECTION:igt_audio
 * @short_description: Library for audio-related tests
 * @title: Audio
 * @include: igt_audio.h
 *
 * This library contains helpers for audio-related tests. More specifically,
 * it allows generating additions of sine signals as well as detecting them.
 */

struct audio_signal_freq {
	int freq;

	short *period;
	int frames;
	int offset;
};

struct audio_signal {
	int channels;
	int sampling_rate;

	struct audio_signal_freq freqs[FREQS_MAX];
	int freqs_count;
};

/**
 * audio_signal_init:
 * @channels: The number of channels to use for the signal
 * @sampling_rate: The sampling rate to use for the signal
 *
 * Allocate and initialize an audio signal structure with the given parameters.
 *
 * Returns: A newly-allocated audio signal structure
 */
struct audio_signal *audio_signal_init(int channels, int sampling_rate)
{
	struct audio_signal *signal;

	signal = malloc(sizeof(struct audio_signal));
	memset(signal, 0, sizeof(struct audio_signal));

	signal->sampling_rate = sampling_rate;
	signal->channels = channels;

	return signal;
}

/**
 * audio_signal_add_frequency:
 * @signal: The target signal structure
 * @frequency: The frequency to add to the signal
 *
 * Add a frequency to the signal.
 *
 * Returns: An integer equal to zero for success and negative for failure
 */
int audio_signal_add_frequency(struct audio_signal *signal, int frequency)
{
	int index = signal->freqs_count;

	if (index == FREQS_MAX)
		return -1;

	/* Stay within the Nyquist–Shannon sampling theorem. */
	if (frequency > signal->sampling_rate / 2)
		return -1;

	/* Clip the frequency to an integer multiple of the sampling rate.
	 * This to be able to store a full period of it and use that for
	 * signal generation, instead of recurrent calls to sin().
	 */
	frequency = signal->sampling_rate / (signal->sampling_rate / frequency);

	igt_debug("Adding test frequency %d\n", frequency);

	signal->freqs[index].freq = frequency;
	signal->freqs[index].frames = 0;
	signal->freqs[index].offset = 0;
	signal->freqs_count++;

	return 0;
}

/**
 * audio_signal_synthesize:
 * @signal: The target signal structure
 *
 * Synthesize the data tables for the audio signal, that can later be used
 * to fill audio buffers. The resources allocated by this function must be
 * freed with a call to audio_signal_clean when the signal is no longer used.
 */
void audio_signal_synthesize(struct audio_signal *signal)
{
	short *period;
	double value;
	int frames;
	int freq;
	int i, j;

	if (signal->freqs_count == 0)
		return;

	for (i = 0; i < signal->freqs_count; i++) {
		freq = signal->freqs[i].freq;
		frames = signal->sampling_rate / freq;

		period = calloc(1, frames * sizeof(short));

		for (j = 0; j < frames; j++) {
			value = 2.0 * M_PI * freq / signal->sampling_rate * j;
			value = sin(value) * SHRT_MAX / signal->freqs_count;

			period[j] = (short) value;
		}

		signal->freqs[i].period = period;
		signal->freqs[i].frames = frames;
	}
}

/**
 * audio_signal_synthesize:
 * @signal: The target signal structure
 *
 * Free the resources allocated by audio_signal_synthesize and remove
 * the previously-added frequencies.
 */
void audio_signal_clean(struct audio_signal *signal)
{
	int i;

	for (i = 0; i < signal->freqs_count; i++) {
		if (signal->freqs[i].period)
			free(signal->freqs[i].period);

		memset(&signal->freqs[i], 0, sizeof(struct audio_signal_freq));
	}

	signal->freqs_count = 0;
}

/**
 * audio_signal_fill:
 * @signal: The target signal structure
 * @buffer: The target buffer to fill
 * @frames: The number of frames to fill
 *
 * Fill the requested number of frames to the target buffer with the audio
 * signal data (in interleaved S16_LE format), at the requested sampling rate
 * and number of channels.
 */
void audio_signal_fill(struct audio_signal *signal, short *buffer, int frames)
{
	short *destination;
	short *source;
	int total;
	int freq_frames;
	int freq_offset;
	int count;
	int i, j, k;

	memset(buffer, 0, sizeof(short) * signal->channels * frames);

	for (i = 0; i < signal->freqs_count; i++) {
		total = 0;

		while (total < frames) {
			freq_frames = signal->freqs[i].frames;
			freq_offset = signal->freqs[i].offset;

			source = signal->freqs[i].period + freq_offset;
			destination = buffer + total * signal->channels;

			count = freq_frames - freq_offset;
			if (count > (frames - total))
				count = frames - total;

			freq_offset += count;
			freq_offset %= freq_frames;

			signal->freqs[i].offset = freq_offset;

			for (j = 0; j < count; j++) {
				for (k = 0; k < signal->channels; k++) {
					destination[j * signal->channels + k] += source[j];
				}
			}

			total += count;
		}
	}
}

/**
 * audio_signal_detect:
 * @signal: The target signal structure
 * @channels: The input data's number of channels
 * @sampling_rate: The input data's sampling rate
 * @buffer: The input data's buffer
 * @frames: The input data's number of frames
 *
 * Detect that the frequencies specified in @signal, and only those, are
 * present in the input data. The input data's format is required to be S16_LE.
 *
 * Returns: A boolean indicating whether the detection was successful
 */
bool audio_signal_detect(struct audio_signal *signal, int channels,
			 int sampling_rate, short *buffer, int frames)
{
	double data[frames];
	int amplitude[frames / 2];
	bool detected[signal->freqs_count];
	int threshold;
	bool above;
	int error;
	int freq;
	int max;
	int c, i, j;

	/* Allowed error in Hz due to FFT step. */
	error = sampling_rate / frames;

	for (c = 0; c < channels; c++) {
		for (i = 0; i < frames; i++)
			data[i] = (double) buffer[i * channels + c];

		gsl_fft_real_radix2_transform(data, 1, frames);

		max = 0;

		for (i = 0; i < frames / 2; i++) {
			amplitude[i] = sqrt(data[i] * data[i] +
					    data[frames - i] *
					    data[frames - i]);
			if (amplitude[i] > max)
				max = amplitude[i];
		}

		for (i = 0; i < signal->freqs_count; i++)
			detected[i] = false;

		threshold = max / 2;
		above = false;
		max = 0;

		for (i = 0; i < frames / 2; i++) {
			if (amplitude[i] > threshold)
				above = true;

			if (above) {
				if (amplitude[i] < threshold) {
					above = false;
					max = 0;

					for (j = 0; j < signal->freqs_count; j++) {
						if (signal->freqs[j].freq >
						    freq - error &&
						    signal->freqs[j].freq <
						    freq + error) {
							detected[j] = true;
							break;
						}
					}

					/* Detected frequency was not generated. */
					if (j == signal->freqs_count) {
						igt_debug("Detected additional frequency: %d\n",
							  freq);
						return false;
					}
				}

				if (amplitude[i] > max) {
					max = amplitude[i];
					freq = sampling_rate * i / frames;
				}
			}
		}

		for (i = 0; i < signal->freqs_count; i++) {
			if (!detected[i]) {
				igt_debug("Missing frequency: %d\n",
					  signal->freqs[i].freq);
				return false;
			}
		}
	}

	return true;
}
