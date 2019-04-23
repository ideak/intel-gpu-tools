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

#include <errno.h>
#include <fcntl.h>
#include <gsl/gsl_fft_real.h>
#include <math.h>
#include <unistd.h>

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
	int16_t *period;
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
			value = sin(value) * INT16_MAX / signal->freqs_count;

			period[j] = (int16_t) value;
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
void audio_signal_fill(struct audio_signal *signal, int16_t *buffer, int frames)
{
	int16_t *destination, *source;
	int total;
	int freq_frames;
	int freq_offset;
	int count;
	int i, j, k;

	memset(buffer, 0, sizeof(int16_t) * signal->channels * frames);

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
 * Checks that frequencies specified in signal, and only those, are included
 * in the input data.
 *
 * sampling_rate is given in Hz. data_len is the number of elements in data.
 */
bool audio_signal_detect(struct audio_signal *signal, int sampling_rate,
			 double *data, size_t data_len)
{
	size_t bin_power_len = data_len / 2 + 1;
	double bin_power[bin_power_len];
	bool detected[signal->freqs_count];
	int ret, freq_accuracy, freq, local_max_freq;
	double max, local_max, threshold;
	size_t i, j;
	bool above, success;

	/* Allowed error in Hz due to FFT step */
	freq_accuracy = sampling_rate / data_len;
	igt_debug("Allowed freq. error: %d Hz\n", freq_accuracy);

	ret = gsl_fft_real_radix2_transform(data, 1, data_len);
	igt_assert(ret == 0);

	/* Compute the power received by every bin of the FFT, and record the
	 * maximum power received as a way to normalize all the others.
	 *
	 * For i < data_len / 2, the real part of the i-th term is stored at
	 * data[i] and its imaginary part is stored at data[data_len - i].
	 * i = 0 and i = data_len / 2 are special cases, they are purely real
	 * so their imaginary part isn't stored.
	 *
	 * The power is encoded as the magnitude of the complex number and the
	 * phase is encoded as its angle.
	 */
	max = 0;
	bin_power[0] = data[0];
	for (i = 1; i < bin_power_len - 1; i++) {
		bin_power[i] = hypot(data[i], data[data_len - i]);
		if (bin_power[i] > max)
			max = bin_power[i];
	}
	bin_power[bin_power_len - 1] = data[data_len / 2];

	for (i = 0; i < signal->freqs_count; i++)
		detected[i] = false;

	/* Do a linear search through the FFT bins' power to find the the local
	 * maximums that exceed half of the absolute maximum that we previously
	 * calculated.
	 *
	 * Since the frequencies might not be perfectly aligned with the bins of
	 * the FFT, we need to find the local maximum across some consecutive
	 * bins. Once the power returns under the power threshold, we compare
	 * the frequency of the bin that received the maximum power to the
	 * expected frequencies. If found, we mark this frequency as such,
	 * otherwise we warn that an unexpected frequency was found.
	 */
	threshold = max / 2;
	success = true;
	above = false;
	local_max = 0;
	local_max_freq = -1;
	for (i = 0; i < bin_power_len; i++) {
		freq = sampling_rate * i / data_len;

		if (bin_power[i] > threshold)
			above = true;

		if (!above) {
			continue;
		}

		/* If we were above the threshold and we're not anymore, it's
		 * time to decide whether the peak frequency is correct or
		 * invalid. */
		if (bin_power[i] < threshold) {
			for (j = 0; j < signal->freqs_count; j++) {
				if (signal->freqs[j].freq >
				    local_max_freq - freq_accuracy &&
				    signal->freqs[j].freq <
				    local_max_freq + freq_accuracy) {
					detected[j] = true;
					igt_debug("Frequency %d detected\n",
						  local_max_freq);
					break;
				}
			}

			/* We haven't generated this frequency, but we detected
			 * it. */
			if (j == signal->freqs_count) {
				igt_debug("Detected additional frequency: %d\n",
					  local_max_freq);
				success = false;
			}

			above = false;
			local_max = 0;
			local_max_freq = -1;
		}

		if (bin_power[i] > local_max) {
			local_max = bin_power[i];
			local_max_freq = freq;
		}
	}

	/* Check that all frequencies we generated have been detected. */
	for (i = 0; i < signal->freqs_count; i++) {
		if (!detected[i]) {
			igt_debug("Missing frequency: %d\n",
				  signal->freqs[i].freq);
			success = false;
		}
	}

	return success;
}

/**
 * Extracts a single channel from a multi-channel S32_LE input buffer.
 */
size_t audio_extract_channel_s32_le(double *dst, size_t dst_cap,
				    int32_t *src, size_t src_len,
				    int n_channels, int channel)
{
	size_t dst_len, i;

	igt_assert(channel < n_channels);
	igt_assert(src_len % n_channels == 0);
	dst_len = src_len / n_channels;
	igt_assert(dst_len <= dst_cap);
	for (i = 0; i < dst_len; i++)
		dst[i] = (double) src[i * n_channels + channel];

	return dst_len;
}

#define RIFF_TAG "RIFF"
#define WAVE_TAG "WAVE"
#define FMT_TAG "fmt "
#define DATA_TAG "data"

static void
append_to_buffer(char *dst, size_t *i, const void *src, size_t src_size)
{
	memcpy(&dst[*i], src, src_size);
	*i += src_size;
}

/**
 * audio_create_wav_file_s32_le:
 * @qualifier: the basename of the file (the test name will be prepended, and
 * the file extension will be appended)
 * @sample_rate: the sample rate in Hz
 * @channels: the number of channels
 * @path: if non-NULL, will be set to a pointer to the new file path (the
 * caller is responsible for free-ing it)
 *
 * Creates a new WAV file.
 *
 * After calling this function, the caller is expected to write S32_LE PCM data
 * to the returned file descriptor.
 *
 * See http://www-mmsp.ece.mcgill.ca/Documents/AudioFormats/WAVE/WAVE.html for
 * a WAV file format specification.
 *
 * Returns: a file descriptor to the newly created file, or -1 on error.
 */
int audio_create_wav_file_s32_le(const char *qualifier, uint32_t sample_rate,
				 uint16_t channels, char **path)
{
	char _path[PATH_MAX];
	const char *test_name, *subtest_name;
	int fd;
	char header[44];
	size_t i = 0;
	uint32_t file_size, chunk_size, byte_rate;
	uint16_t format, block_align, bits_per_sample;

	test_name = igt_test_name();
	subtest_name = igt_subtest_name();

	igt_assert(igt_frame_dump_path);
	snprintf(_path, sizeof(_path), "%s/audio-%s-%s-%s.wav",
		 igt_frame_dump_path, test_name, subtest_name, qualifier);

	if (path)
		*path = strdup(_path);

	igt_debug("Dumping %s audio to %s\n", qualifier, _path);
	fd = open(_path, O_WRONLY | O_CREAT | O_TRUNC);
	if (fd < 0) {
		igt_warn("open failed: %s\n", strerror(errno));
		return -1;
	}

	/* File header */
	file_size = UINT32_MAX; /* unknown file size */
	append_to_buffer(header, &i, RIFF_TAG, strlen(RIFF_TAG));
	append_to_buffer(header, &i, &file_size, sizeof(file_size));
	append_to_buffer(header, &i, WAVE_TAG, strlen(WAVE_TAG));

	/* Format chunk */
	chunk_size = 16;
	format = 1; /* PCM */
	bits_per_sample = 32; /* S32_LE */
	byte_rate = sample_rate * channels * bits_per_sample / 8;
	block_align = channels * bits_per_sample / 8;
	append_to_buffer(header, &i, FMT_TAG, strlen(FMT_TAG));
	append_to_buffer(header, &i, &chunk_size, sizeof(chunk_size));
	append_to_buffer(header, &i, &format, sizeof(format));
	append_to_buffer(header, &i, &channels, sizeof(channels));
	append_to_buffer(header, &i, &sample_rate, sizeof(sample_rate));
	append_to_buffer(header, &i, &byte_rate, sizeof(byte_rate));
	append_to_buffer(header, &i, &block_align, sizeof(block_align));
	append_to_buffer(header, &i, &bits_per_sample, sizeof(bits_per_sample));

	/* Data chunk */
	chunk_size = UINT32_MAX; /* unknown chunk size */
	append_to_buffer(header, &i, DATA_TAG, strlen(DATA_TAG));
	append_to_buffer(header, &i, &chunk_size, sizeof(chunk_size));

	igt_assert(i == sizeof(header));

	if (write(fd, header, sizeof(header)) != sizeof(header)) {
		igt_warn("write failed: %s'n", strerror(errno));
		close(fd);
		return -1;
	}

	return fd;
}
