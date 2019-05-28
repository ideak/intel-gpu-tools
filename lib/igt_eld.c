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

#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "igt_core.h"
#include "igt_eld.h"

#define ELD_PREFIX "eld#"
#define ELD_DELIM " \t"

/**
 * EDID-Like Data (ELD) is metadata parsed and exposed by ALSA for HDMI and
 * DisplayPort connectors supporting audio. This includes the monitor name and
 * the supported audio parameters (formats, sampling rates, sample sizes and so
 * on).
 */

/** eld_parse_entry: parse an ELD entry
 *
 * Here is an example of an ELD entry:
 *
 *     $ cat /proc/asound/card0/eld#0.2
 *     monitor_present         1
 *     eld_valid               1
 *     monitor_name            U2879G6
 *     connection_type         DisplayPort
 *     eld_version             [0x2] CEA-861D or below
 *     edid_version            [0x3] CEA-861-B, C or D
 *     manufacture_id          0xe305
 *     product_id              0x2879
 *     port_id                 0x800
 *     support_hdcp            0
 *     support_ai              0
 *     audio_sync_delay        0
 *     speakers                [0x1] FL/FR
 *     sad_count               1
 *     sad0_coding_type        [0x1] LPCM
 *     sad0_channels           2
 *     sad0_rates              [0xe0] 32000 44100 48000
 *     sad0_bits               [0xe0000] 16 20 24
 */
static bool eld_parse_entry(const char *path, struct eld_entry *eld)
{
	FILE *f;
	char buf[1024];
	char *key, *value;
	size_t len;
	bool monitor_present;

	memset(eld, 0, sizeof(*eld));

	f = fopen(path, "r");
	if (!f) {
		igt_debug("Failed to open ELD file: %s\n", path);
		return false;
	}

	while ((fgets(buf, sizeof(buf), f)) != NULL) {
		len = strlen(buf);
		if (buf[len - 1] == '\n')
			buf[len - 1] = '\0';

		key = strtok(buf, ELD_DELIM);
		value = strtok(NULL, "");
		/* Skip whitespace at the beginning */
		value += strspn(value, ELD_DELIM);

		if (strcmp(key, "monitor_present") == 0)
			monitor_present = strcmp(value, "1") == 0;
		else if (strcmp(key, "eld_valid") == 0)
			eld->valid = strcmp(value, "1") == 0;
		else if (strcmp(key, "monitor_name") == 0)
			snprintf(eld->monitor_name, sizeof(eld->monitor_name),
				 "%s", value);
	}

	if (ferror(f) != 0) {
		igt_debug("Failed to read ELD file: %d\n", ferror(f));
		return false;
	}

	fclose(f);

	return monitor_present;
}

/** eld_has_igt: check whether ALSA has detected the audio-capable IGT EDID by
 * parsing ELD entries */
bool eld_has_igt(void)
{
	DIR *dir;
	struct dirent *dirent;
	int i;
	char card[64];
	char path[PATH_MAX];
	struct eld_entry eld;

	for (i = 0; i < 8; i++) {
		snprintf(card, sizeof(card), "/proc/asound/card%d", i);
		dir = opendir(card);
		if (!dir)
			continue;

		while ((dirent = readdir(dir))) {
			if (strncmp(dirent->d_name, ELD_PREFIX,
				    strlen(ELD_PREFIX)) != 0)
				continue;

			snprintf(path, sizeof(path), "%s/%s", card,
				 dirent->d_name);
			if (!eld_parse_entry(path, &eld)) {
				continue;
			}

			if (!eld.valid) {
				igt_debug("Skipping invalid ELD: %s\n", path);
				continue;
			}

			if (strcmp(eld.monitor_name, "IGT") == 0) {
				closedir(dir);
				return true;
			}
		}
		closedir(dir);
	}

	return false;
}
