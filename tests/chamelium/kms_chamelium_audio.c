/*
 * Copyright © 2016 Red Hat Inc.
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
 *    Lyude Paul <lyude@redhat.com>
 */

#include "igt_eld.h"
#include "igt_infoframe.h"
#include "kms_chamelium_helper.h"

/* Playback parameters control the audio signal we synthesize and send */
#define PLAYBACK_CHANNELS 2
#define PLAYBACK_SAMPLES 1024

/* Capture paremeters control the audio signal we receive */
#define CAPTURE_SAMPLES 2048

#define AUDIO_TIMEOUT 2000 /* ms */
/* A streak of 3 gives confidence that the signal is good. */
#define MIN_STREAK 3

#define FLATLINE_AMPLITUDE 0.1 /* normalized, ie. in [0, 1] */
#define FLATLINE_AMPLITUDE_ACCURACY 0.001 /* ± 0.1 % of the full amplitude */
#define FLATLINE_ALIGN_ACCURACY 0 /* number of samples */

struct audio_state {
	struct alsa *alsa;
	struct chamelium *chamelium;
	struct chamelium_port *port;
	struct chamelium_stream *stream;

	/* The capture format is only available after capture has started. */
	struct {
		snd_pcm_format_t format;
		int channels;
		int rate;
	} playback, capture;

	char *name;
	struct audio_signal *signal; /* for frequencies test only */
	int channel_mapping[CHAMELIUM_MAX_AUDIO_CHANNELS];

	size_t recv_pages;
	int msec;

	int dump_fd;
	char *dump_path;

	pthread_t thread;
	atomic_bool run;
	atomic_bool positive; /* for pulse test only */
};

/* TODO: enable >48KHz rates, these are not reliable */
static int test_sampling_rates[] = {
	32000, 44100, 48000,
	/* 88200, */
	/* 96000, */
	/* 176400, */
	/* 192000, */
};

static int test_sampling_rates_count =
	sizeof(test_sampling_rates) / sizeof(int);

/* Test frequencies (Hz): a sine signal will be generated for each.
 *
 * Depending on the sampling rate chosen, it might not be possible to properly
 * detect the generated sine (see Nyquist–Shannon sampling theorem).
 * Frequencies that can't be reliably detected will be automatically pruned in
 * #audio_signal_add_frequency. For instance, the 80KHz frequency can only be
 * tested with a 192KHz sampling rate.
 */
static int test_frequencies[] = {
	300, 600, 1200, 10000, 80000,
};

static int test_frequencies_count = sizeof(test_frequencies) / sizeof(int);

static const snd_pcm_format_t test_formats[] = {
	SND_PCM_FORMAT_S16_LE,
	SND_PCM_FORMAT_S24_LE,
	SND_PCM_FORMAT_S32_LE,
};

static const size_t test_formats_count =
	sizeof(test_formats) / sizeof(test_formats[0]);

static void audio_state_init(struct audio_state *state, chamelium_data_t *data,
			     struct alsa *alsa, struct chamelium_port *port,
			     snd_pcm_format_t format, int channels, int rate)
{
	memset(state, 0, sizeof(*state));
	state->dump_fd = -1;

	state->alsa = alsa;
	state->chamelium = data->chamelium;
	state->port = port;

	state->playback.format = format;
	state->playback.channels = channels;
	state->playback.rate = rate;

	alsa_configure_output(alsa, format, channels, rate);

	state->stream = chamelium_stream_init();
	igt_assert_f(state->stream,
		     "Failed to initialize Chamelium stream client\n");
}

static void audio_state_fini(struct audio_state *state)
{
	chamelium_stream_deinit(state->stream);
	free(state->name);
}

static void *run_audio_thread(void *data)
{
	struct alsa *alsa = data;

	alsa_run(alsa, -1);
	return NULL;
}

static void audio_state_start(struct audio_state *state, const char *name)
{
	int ret;
	bool ok;
	size_t i, j;
	enum chamelium_stream_realtime_mode stream_mode;
	char dump_suffix[64];

	free(state->name);
	state->name = strdup(name);
	state->recv_pages = 0;
	state->msec = 0;

	igt_debug("Starting %s test with playback format %s, "
		  "sampling rate %d Hz and %d channels\n",
		  name, snd_pcm_format_name(state->playback.format),
		  state->playback.rate, state->playback.channels);

	chamelium_start_capturing_audio(state->chamelium, state->port, false);

	stream_mode = CHAMELIUM_STREAM_REALTIME_STOP_WHEN_OVERFLOW;
	ok = chamelium_stream_dump_realtime_audio(state->stream, stream_mode);
	igt_assert_f(ok, "Failed to start streaming audio capture\n");

	/* Start playing audio */
	state->run = true;
	ret = pthread_create(&state->thread, NULL, run_audio_thread,
			     state->alsa);
	igt_assert_f(ret == 0, "Failed to start audio playback thread\n");

	/* The Chamelium device only supports this PCM format. */
	state->capture.format = SND_PCM_FORMAT_S32_LE;

	/* Only after we've started playing audio, we can retrieve the capture
	 * format used by the Chamelium device. */
	chamelium_get_audio_format(state->chamelium, state->port,
				   &state->capture.rate,
				   &state->capture.channels);
	if (state->capture.rate == 0) {
		igt_debug("Audio receiver doesn't indicate the capture "
			  "sampling rate, assuming it's %d Hz\n",
			  state->playback.rate);
		state->capture.rate = state->playback.rate;
	}

	chamelium_get_audio_channel_mapping(state->chamelium, state->port,
					    state->channel_mapping);
	/* Make sure we can capture all channels we send. */
	for (i = 0; i < state->playback.channels; i++) {
		ok = false;
		for (j = 0; j < state->capture.channels; j++) {
			if (state->channel_mapping[j] == i) {
				ok = true;
				break;
			}
		}
		igt_assert_f(ok, "Cannot capture all channels\n");
	}

	if (igt_frame_dump_is_enabled()) {
		snprintf(dump_suffix, sizeof(dump_suffix),
			 "capture-%s-%s-%dch-%dHz", name,
			 snd_pcm_format_name(state->playback.format),
			 state->playback.channels, state->playback.rate);

		state->dump_fd = audio_create_wav_file_s32_le(
			dump_suffix, state->capture.rate,
			state->capture.channels, &state->dump_path);
		igt_assert_f(state->dump_fd >= 0,
			     "Failed to create audio dump file\n");
	}
}

static void audio_state_receive(struct audio_state *state, int32_t **recv,
				size_t *recv_len)
{
	bool ok;
	size_t page_count;
	size_t recv_size;

	ok = chamelium_stream_receive_realtime_audio(state->stream, &page_count,
						     recv, recv_len);
	igt_assert_f(ok, "Failed to receive audio from stream server\n");

	state->msec = state->recv_pages * *recv_len /
		      (double)state->capture.channels /
		      (double)state->capture.rate * 1000;
	state->recv_pages++;

	if (state->dump_fd >= 0) {
		recv_size = *recv_len * sizeof(int32_t);
		igt_assert_f(write(state->dump_fd, *recv, recv_size) ==
				     recv_size,
			     "Failed to write to audio dump file\n");
	}
}

static void audio_state_stop(struct audio_state *state, bool success)
{
	bool ok;
	int ret;
	struct chamelium_audio_file *audio_file;
	enum igt_log_level log_level;

	igt_debug("Stopping audio playback\n");
	state->run = false;
	ret = pthread_join(state->thread, NULL);
	igt_assert_f(ret == 0, "Failed to join audio playback thread\n");

	ok = chamelium_stream_stop_realtime_audio(state->stream);
	igt_assert_f(ok, "Failed to stop streaming audio capture\n");

	audio_file =
		chamelium_stop_capturing_audio(state->chamelium, state->port);
	if (audio_file) {
		igt_debug("Audio file saved on the Chamelium in %s\n",
			  audio_file->path);
		chamelium_destroy_audio_file(audio_file);
	}

	if (state->dump_fd >= 0) {
		close(state->dump_fd);
		state->dump_fd = -1;

		if (success) {
			/* Test succeeded, no need to keep the captured data */
			unlink(state->dump_path);
		} else
			igt_debug("Saved captured audio data to %s\n",
				  state->dump_path);
		free(state->dump_path);
		state->dump_path = NULL;
	}

	if (success)
		log_level = IGT_LOG_DEBUG;
	else
		log_level = IGT_LOG_CRITICAL;

	igt_log(IGT_LOG_DOMAIN, log_level,
		"Audio %s test result for format %s, "
		"sampling rate %d Hz and %d channels: %s\n",
		state->name, snd_pcm_format_name(state->playback.format),
		state->playback.rate, state->playback.channels,
		success ? "ALL GREEN" : "FAILED");
}

static void check_audio_infoframe(struct audio_state *state)
{
	struct chamelium_infoframe *infoframe;
	struct infoframe_audio infoframe_audio;
	struct infoframe_audio expected = { 0 };
	bool ok;

	if (!chamelium_supports_get_last_infoframe(state->chamelium)) {
		igt_debug("Skipping audio InfoFrame check: "
			  "Chamelium board doesn't support GetLastInfoFrame\n");
		return;
	}

	expected.coding_type = INFOFRAME_AUDIO_CT_PCM;
	expected.channel_count = state->playback.channels;
	expected.sampling_freq = state->playback.rate;
	expected.sample_size = snd_pcm_format_width(state->playback.format);

	infoframe = chamelium_get_last_infoframe(state->chamelium, state->port,
						 CHAMELIUM_INFOFRAME_AUDIO);
	if (infoframe == NULL && state->playback.channels <= 2) {
		/* Audio InfoFrames are optional for mono and stereo audio */
		igt_debug("Skipping audio InfoFrame check: "
			  "no InfoFrame received\n");
		return;
	}
	igt_assert_f(infoframe != NULL, "no audio InfoFrame received\n");

	ok = infoframe_audio_parse(&infoframe_audio, infoframe->version,
				   infoframe->payload, infoframe->payload_size);
	chamelium_infoframe_destroy(infoframe);
	igt_assert_f(ok, "failed to parse audio InfoFrame\n");

	igt_debug("Checking audio InfoFrame:\n");
	igt_debug("coding_type: got %d, expected %d\n",
		  infoframe_audio.coding_type, expected.coding_type);
	igt_debug("channel_count: got %d, expected %d\n",
		  infoframe_audio.channel_count, expected.channel_count);
	igt_debug("sampling_freq: got %d, expected %d\n",
		  infoframe_audio.sampling_freq, expected.sampling_freq);
	igt_debug("sample_size: got %d, expected %d\n",
		  infoframe_audio.sample_size, expected.sample_size);

	if (infoframe_audio.coding_type != INFOFRAME_AUDIO_CT_UNSPECIFIED)
		igt_assert(infoframe_audio.coding_type == expected.coding_type);
	if (infoframe_audio.channel_count >= 0)
		igt_assert(infoframe_audio.channel_count ==
			   expected.channel_count);
	if (infoframe_audio.sampling_freq >= 0)
		igt_assert(infoframe_audio.sampling_freq ==
			   expected.sampling_freq);
	if (infoframe_audio.sample_size >= 0)
		igt_assert(infoframe_audio.sample_size == expected.sample_size);
}

static int audio_output_frequencies_callback(void *data, void *buffer,
					     int samples)
{
	struct audio_state *state = data;
	double *tmp;
	size_t len;

	len = samples * state->playback.channels;
	tmp = malloc(len * sizeof(double));
	audio_signal_fill(state->signal, tmp, samples);
	audio_convert_to(buffer, tmp, len, state->playback.format);
	free(tmp);

	return state->run ? 0 : -1;
}

static bool test_audio_frequencies(struct audio_state *state)
{
	int freq, step;
	int32_t *recv, *buf;
	double *channel;
	size_t i, j, streak;
	size_t recv_len, buf_len, buf_cap, channel_len;
	bool success;
	int capture_chan;

	state->signal = audio_signal_init(state->playback.channels,
					  state->playback.rate);
	igt_assert_f(state->signal, "Failed to initialize audio signal\n");

	/* We'll choose different frequencies per channel to make sure they are
	 * independent from each other. To do so, we'll add a different offset
	 * to the base frequencies for each channel. We need to choose a big
	 * enough offset so that we're sure to detect mixed up channels. We
	 * choose an offset of two 2 bins in the final FFT to enforce a clear
	 * difference.
	 *
	 * Note that we assume capture_rate == playback_rate. We'll assert this
	 * later on. We cannot retrieve the capture rate before starting
	 * playing audio, so we don't really have the choice.
	 */
	step = 2 * state->playback.rate / CAPTURE_SAMPLES;
	for (i = 0; i < test_frequencies_count; i++) {
		for (j = 0; j < state->playback.channels; j++) {
			freq = test_frequencies[i] + j * step;
			audio_signal_add_frequency(state->signal, freq, j);
		}
	}
	audio_signal_synthesize(state->signal);

	alsa_register_output_callback(state->alsa,
				      audio_output_frequencies_callback, state,
				      PLAYBACK_SAMPLES);

	audio_state_start(state, "frequencies");

	igt_assert_f(state->capture.rate == state->playback.rate,
		     "Capture rate (%dHz) doesn't match playback rate (%dHz)\n",
		     state->capture.rate, state->playback.rate);

	/* Needs to be a multiple of 128, because that's the number of samples
	 * we get per channel each time we receive an audio page from the
	 * Chamelium device.
	 *
	 * Additionally, this value needs to be high enough to guarantee we
	 * capture a full period of each sine we generate. If we capture 2048
	 * samples at a 192KHz sampling rate, we get a full period for a >94Hz
	 * sines. For lower sampling rates, the capture duration will be
	 * longer.
	 */
	channel_len = CAPTURE_SAMPLES;
	channel = malloc(sizeof(double) * channel_len);

	buf_cap = state->capture.channels * channel_len;
	buf = malloc(sizeof(int32_t) * buf_cap);
	buf_len = 0;

	recv = NULL;
	recv_len = 0;

	success = false;
	streak = 0;
	while (!success && state->msec < AUDIO_TIMEOUT) {
		audio_state_receive(state, &recv, &recv_len);

		memcpy(&buf[buf_len], recv, recv_len * sizeof(int32_t));
		buf_len += recv_len;

		if (buf_len < buf_cap)
			continue;
		igt_assert(buf_len == buf_cap);

		igt_debug("Detecting audio signal, t=%d msec\n", state->msec);

		for (j = 0; j < state->playback.channels; j++) {
			capture_chan = state->channel_mapping[j];
			igt_assert(capture_chan >= 0);
			igt_debug("Processing channel %zu (captured as "
				  "channel %d)\n",
				  j, capture_chan);

			audio_extract_channel_s32_le(channel, channel_len, buf,
						     buf_len,
						     state->capture.channels,
						     capture_chan);

			if (audio_signal_detect(state->signal,
						state->capture.rate, j, channel,
						channel_len))
				streak++;
			else
				streak = 0;
		}

		buf_len = 0;

		success = streak == MIN_STREAK * state->playback.channels;
	}

	audio_state_stop(state, success);

	free(recv);
	free(buf);
	free(channel);
	audio_signal_fini(state->signal);

	check_audio_infoframe(state);

	return success;
}

static int audio_output_flatline_callback(void *data, void *buffer, int samples)
{
	struct audio_state *state = data;
	double *tmp;
	size_t len, i;

	len = samples * state->playback.channels;
	tmp = malloc(len * sizeof(double));
	for (i = 0; i < len; i++)
		tmp[i] = (state->positive ? 1 : -1) * FLATLINE_AMPLITUDE;
	audio_convert_to(buffer, tmp, len, state->playback.format);
	free(tmp);

	return state->run ? 0 : -1;
}

static bool detect_flatline_amplitude(double *buf, size_t buf_len, bool pos)
{
	double expected, min, max;
	size_t i;
	bool ok;

	min = max = NAN;
	for (i = 0; i < buf_len; i++) {
		if (isnan(min) || buf[i] < min)
			min = buf[i];
		if (isnan(max) || buf[i] > max)
			max = buf[i];
	}

	expected = (pos ? 1 : -1) * FLATLINE_AMPLITUDE;
	ok = (min >= expected - FLATLINE_AMPLITUDE_ACCURACY &&
	      max <= expected + FLATLINE_AMPLITUDE_ACCURACY);
	if (ok)
		igt_debug("Flatline wave amplitude detected\n");
	else
		igt_debug("Flatline amplitude not detected (min=%f, max=%f)\n",
			  min, max);
	return ok;
}

static ssize_t detect_falling_edge(double *buf, size_t buf_len)
{
	size_t i;

	for (i = 0; i < buf_len; i++) {
		if (buf[i] < 0)
			return i;
	}

	return -1;
}

/** test_audio_flatline:
 *
 * Send a constant value (one positive, then a negative one) and check that:
 *
 * - The amplitude of the flatline is correct
 * - All channels switch from a positive signal to a negative one at the same
 *   time (ie. all channels are aligned)
 */
static bool test_audio_flatline(struct audio_state *state)
{
	bool success, amp_success, align_success;
	int32_t *recv;
	size_t recv_len, i, channel_len;
	ssize_t j;
	int streak, capture_chan;
	double *channel;
	int falling_edges[CHAMELIUM_MAX_AUDIO_CHANNELS];

	alsa_register_output_callback(state->alsa,
				      audio_output_flatline_callback, state,
				      PLAYBACK_SAMPLES);

	/* Start by sending a positive signal */
	state->positive = true;

	audio_state_start(state, "flatline");

	for (i = 0; i < state->playback.channels; i++)
		falling_edges[i] = -1;

	recv = NULL;
	recv_len = 0;
	amp_success = false;
	streak = 0;
	while (!amp_success && state->msec < AUDIO_TIMEOUT) {
		audio_state_receive(state, &recv, &recv_len);

		igt_debug("Detecting audio signal, t=%d msec\n", state->msec);

		for (i = 0; i < state->playback.channels; i++) {
			capture_chan = state->channel_mapping[i];
			igt_assert(capture_chan >= 0);
			igt_debug("Processing channel %zu (captured as "
				  "channel %d)\n",
				  i, capture_chan);

			channel_len = audio_extract_channel_s32_le(
				NULL, 0, recv, recv_len,
				state->capture.channels, capture_chan);
			channel = malloc(channel_len * sizeof(double));
			audio_extract_channel_s32_le(channel, channel_len, recv,
						     recv_len,
						     state->capture.channels,
						     capture_chan);

			/* Check whether the amplitude is fine */
			if (detect_flatline_amplitude(channel, channel_len,
						      state->positive))
				streak++;
			else
				streak = 0;

			/* If we're now sending a negative signal, detect the
			 * falling edge */
			j = detect_falling_edge(channel, channel_len);
			if (!state->positive && j >= 0) {
				falling_edges[i] =
					recv_len * state->recv_pages + j;
			}

			free(channel);
		}

		amp_success = streak == MIN_STREAK * state->playback.channels;

		if (amp_success && state->positive) {
			/* Switch to a negative signal after we've detected the
			 * positive one. */
			state->positive = false;
			amp_success = false;
			streak = 0;
			igt_debug("Switching to negative square wave\n");
		}
	}

	/* Check alignment between all channels by comparing the index of the
	 * falling edge. */
	align_success = true;
	for (i = 0; i < state->playback.channels; i++) {
		if (falling_edges[i] < 0) {
			igt_critical(
				"Falling edge not detected for channel %zu\n",
				i);
			align_success = false;
			continue;
		}

		if (abs(falling_edges[0] - falling_edges[i]) >
		    FLATLINE_ALIGN_ACCURACY) {
			igt_critical("Channel alignment mismatch: "
				     "channel 0 has a falling edge at index %d "
				     "while channel %zu has index %d\n",
				     falling_edges[0], i, falling_edges[i]);
			align_success = false;
		}
	}

	success = amp_success && align_success;
	audio_state_stop(state, success);

	free(recv);

	return success;
}

static bool check_audio_configuration(struct alsa *alsa,
				      snd_pcm_format_t format, int channels,
				      int sampling_rate)
{
	if (!alsa_test_output_configuration(alsa, format, channels,
					    sampling_rate)) {
		igt_debug("Skipping test with format %s, sampling rate %d Hz "
			  "and %d channels because at least one of the "
			  "selected output devices doesn't support this "
			  "configuration\n",
			  snd_pcm_format_name(format), sampling_rate, channels);
		return false;
	}
	/* TODO: the Chamelium device sends a malformed signal for some audio
	 * configurations. See crbug.com/950917 */
	if ((format != SND_PCM_FORMAT_S16_LE && sampling_rate >= 44100) ||
	    channels > 2) {
		igt_debug("Skipping test with format %s, sampling rate %d Hz "
			  "and %d channels because the Chamelium device "
			  "doesn't support this configuration\n",
			  snd_pcm_format_name(format), sampling_rate, channels);
		return false;
	}
	return true;
}

static const char test_display_audio_desc[] =
	"Playback various audio signals with various audio formats/rates, "
	"capture them and check they are correct";
static void test_display_audio(chamelium_data_t *data,
			       struct chamelium_port *port,
			       const char *audio_device,
			       enum igt_custom_edid_type edid)
{
	bool run, success;
	struct alsa *alsa;
	int ret;
	igt_output_t *output;
	igt_plane_t *primary;
	struct igt_fb fb;
	drmModeModeInfo *mode;
	drmModeConnector *connector;
	int fb_id, i, j;
	int channels, sampling_rate;
	snd_pcm_format_t format;
	struct audio_state state;

	igt_require(alsa_has_exclusive_access());

	/* Old Chamelium devices need an update for DisplayPort audio and
	 * chamelium_get_audio_format support. */
	igt_require(chamelium_has_audio_support(data->chamelium, port));

	alsa = alsa_init();
	igt_assert(alsa);

	igt_modeset_disable_all_outputs(&data->display);
	chamelium_reset_state(&data->display, data->chamelium, port,
			      data->ports, data->port_count);

	output = chamelium_prepare_output(data, port, edid);
	connector = chamelium_port_get_connector(data->chamelium, port, false);
	primary = igt_output_get_plane_type(output, DRM_PLANE_TYPE_PRIMARY);
	igt_assert(primary);

	/* Enable the output because the receiver won't try to receive audio if
	 * it doesn't receive video. */
	igt_assert(connector->count_modes > 0);
	mode = &connector->modes[0];

	fb_id = igt_create_color_pattern_fb(data->drm_fd, mode->hdisplay,
					    mode->vdisplay, DRM_FORMAT_XRGB8888,
					    DRM_FORMAT_MOD_LINEAR, 0, 0, 0,
					    &fb);
	igt_assert(fb_id > 0);

	chamelium_enable_output(data, port, output, mode, &fb);

	run = false;
	success = true;
	for (i = 0; i < test_sampling_rates_count; i++) {
		for (j = 0; j < test_formats_count; j++) {
			ret = alsa_open_output(alsa, audio_device);
			igt_assert_f(ret >= 0, "Failed to open ALSA output\n");

			/* TODO: playback on all 8 available channels (this
			 * isn't supported by Chamelium devices yet, see
			 * https://crbug.com/950917) */
			format = test_formats[j];
			channels = PLAYBACK_CHANNELS;
			sampling_rate = test_sampling_rates[i];

			if (!check_audio_configuration(alsa, format, channels,
						       sampling_rate))
				continue;

			run = true;

			audio_state_init(&state, data, alsa, port, format,
					 channels, sampling_rate);
			success &= test_audio_frequencies(&state);
			success &= test_audio_flatline(&state);
			audio_state_fini(&state);

			alsa_close_output(alsa);
		}
	}

	/* Make sure we tested at least one frequency and format. */
	igt_assert(run);
	/* Make sure all runs were successful. */
	igt_assert(success);

	igt_remove_fb(data->drm_fd, &fb);

	drmModeFreeConnector(connector);

	free(alsa);
}

static const char test_display_audio_edid_desc[] =
	"Plug a connector with an EDID suitable for audio, check ALSA's "
	"EDID-Like Data reports the correct audio parameters";
static void test_display_audio_edid(chamelium_data_t *data,
				    struct chamelium_port *port,
				    enum igt_custom_edid_type edid)
{
	igt_output_t *output;
	igt_plane_t *primary;
	struct igt_fb fb;
	drmModeModeInfo *mode;
	drmModeConnector *connector;
	int fb_id;
	struct eld_entry eld;
	struct eld_sad *sad;

	igt_require(eld_is_supported());

	igt_modeset_disable_all_outputs(&data->display);
	chamelium_reset_state(&data->display, data->chamelium, port,
			      data->ports, data->port_count);

	output = chamelium_prepare_output(data, port, edid);
	connector = chamelium_port_get_connector(data->chamelium, port, false);
	primary = igt_output_get_plane_type(output, DRM_PLANE_TYPE_PRIMARY);
	igt_assert(primary);

	/* Enable the output because audio cannot be played on inactive
	 * connectors. */
	igt_assert(connector->count_modes > 0);
	mode = &connector->modes[0];

	fb_id = igt_create_color_pattern_fb(data->drm_fd, mode->hdisplay,
					    mode->vdisplay, DRM_FORMAT_XRGB8888,
					    DRM_FORMAT_MOD_LINEAR, 0, 0, 0,
					    &fb);
	igt_assert(fb_id > 0);

	chamelium_enable_output(data, port, output, mode, &fb);

	igt_assert(eld_get_igt(&eld));
	igt_assert(eld.sads_len == 1);

	sad = &eld.sads[0];
	igt_assert(sad->coding_type == CEA_SAD_FORMAT_PCM);
	igt_assert(sad->channels == 2);
	igt_assert(sad->rates ==
		   (CEA_SAD_SAMPLING_RATE_32KHZ | CEA_SAD_SAMPLING_RATE_44KHZ |
		    CEA_SAD_SAMPLING_RATE_48KHZ));
	igt_assert(sad->bits ==
		   (CEA_SAD_SAMPLE_SIZE_16 | CEA_SAD_SAMPLE_SIZE_20 |
		    CEA_SAD_SAMPLE_SIZE_24));

	igt_remove_fb(data->drm_fd, &fb);

	drmModeFreeConnector(connector);
}

IGT_TEST_DESCRIPTION("Testing Audio with a Chamelium board");
igt_main
{
	chamelium_data_t data;
	struct chamelium_port *port;
	int p;

	igt_fixture {
		chamelium_init_test(&data);
	}

	igt_describe("DisplayPort tests");
	igt_subtest_group {
		igt_fixture {
			chamelium_require_connector_present(
				data.ports, DRM_MODE_CONNECTOR_DisplayPort,
				data.port_count, 1);
		}

		igt_describe(test_display_audio_desc);
		connector_subtest("dp-audio", DisplayPort) test_display_audio(
			&data, port, "HDMI", IGT_CUSTOM_EDID_DP_AUDIO);

		igt_describe(test_display_audio_edid_desc);
		connector_subtest("dp-audio-edid", DisplayPort)
			test_display_audio_edid(&data, port,
						IGT_CUSTOM_EDID_DP_AUDIO);
	}

	igt_describe("HDMI tests");
	igt_subtest_group {
		igt_fixture {
			chamelium_require_connector_present(
				data.ports, DRM_MODE_CONNECTOR_HDMIA,
				data.port_count, 1);
		}

		igt_describe(test_display_audio_desc);
		connector_subtest("hdmi-audio", HDMIA) test_display_audio(
			&data, port, "HDMI", IGT_CUSTOM_EDID_HDMI_AUDIO);

		igt_describe(test_display_audio_edid_desc);
		connector_subtest("hdmi-audio-edid", HDMIA)
			test_display_audio_edid(&data, port,
						IGT_CUSTOM_EDID_HDMI_AUDIO);
	}

	igt_fixture {
		igt_display_fini(&data.display);
		close(data.drm_fd);
	}
}
