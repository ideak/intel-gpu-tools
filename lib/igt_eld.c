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

#include "igt_eld.h"

/**
 * EDID-Like Data (ELD) is metadata parsed and exposed by ALSA for HDMI and
 * DisplayPort connectors supporting audio. This includes the monitor name and
 * the supported audio parameters (formats, sampling rates, sample sizes and so
 * on).
 */

/** eld_entry_is_igt: checks whether an ELD entry is mapped to the IGT EDID */
static bool eld_entry_is_igt(const char *path)
{
	FILE *in;
	char buf[1024];
	uint8_t eld_valid = 0;
	uint8_t mon_valid = 0;

	in = fopen(path, "r");
	if (!in)
		return false;

	memset(buf, 0, 1024);

	while ((fgets(buf, 1024, in)) != NULL) {
		char *line = buf;

		if (!strncasecmp(line, "eld_valid", 9) &&
				strstr(line, "1")) {
			eld_valid++;
		}

		if (!strncasecmp(line, "monitor_name", 12) &&
				strstr(line, "IGT")) {
			mon_valid++;
		}
	}

	fclose(in);
	if (mon_valid && eld_valid)
		return true;

	return false;
}

/** eld_has_igt: check whether ALSA has detected the audio-capable IGT EDID by
 * parsing ELD entries */
bool eld_has_igt(void)
{
	DIR *dir;
	struct dirent *snd_hda;
	int i;

	for (i = 0; i < 8; i++) {
		char cards[128];

		snprintf(cards, sizeof(cards), "/proc/asound/card%d", i);
		dir = opendir(cards);
		if (!dir)
			continue;

		while ((snd_hda = readdir(dir))) {
			char fpath[PATH_MAX];

			if (*snd_hda->d_name == '.' ||
			    strstr(snd_hda->d_name, "eld") == 0)
				continue;

			snprintf(fpath, sizeof(fpath), "%s/%s", cards,
				 snd_hda->d_name);
			if (eld_entry_is_igt(fpath)) {
				closedir(dir);
				return true;
			}
		}
		closedir(dir);
	}

	return false;
}
