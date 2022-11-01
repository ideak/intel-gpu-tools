// SPDX-License-Identifier: GPL-2.0
/*
 * A helper library for parsing and making use of real EDID data from monitors
 * and make them compatible with IGT and Chamelium.
 *
 * Copyright 2022 Google LLC.
 *
 * Authors: Mark Yacoub <markyacoub@chromium.org>
 */

#include "monitor_edids_helper.h"

#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include "igt_core.h"

static uint8_t convert_hex_char_to_byte(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;

	assert(0);
	return 0;
}

static uint8_t *get_edid_bytes_from_hex_str(const char *edid_str)
{
	int i;

	int edid_size = strlen(edid_str) / 2; /* each asci is a nibble. */
	uint8_t *edid = (uint8_t *)malloc(edid_size);

	for (i = 0; i < edid_size; i++) {
		edid[i] = convert_hex_char_to_byte(edid_str[i * 2]) << 4 |
			  convert_hex_char_to_byte(edid_str[i * 2 + 1]);
	}

	return edid;
}

const char *monitor_edid_get_name(const monitor_edid *edid)
{
	return edid->name;
}

struct chamelium_edid *
get_chameleon_edid_from_monitor_edid(struct chamelium *chamelium,
				     const monitor_edid *edid)
{
	int i;
	struct chamelium_edid *chamelium_edid;

	uint8_t *base_edid = get_edid_bytes_from_hex_str(edid->edid);
	assert(base_edid);

	/*Print the full formatted EDID on debug. */
	for (i = 0; i < strlen(edid->edid) / 2; i++) {
		igt_debug("%02x ", base_edid[i]);
		if (i % 16 == 15)
			igt_debug("\n");
	}

	chamelium_edid = malloc(sizeof(struct chamelium_edid));
	assert(chamelium_edid);

	chamelium_edid->base = (struct edid *)base_edid;
	chamelium_edid->chamelium = chamelium;
	for (i = 0; i < CHAMELIUM_MAX_PORTS; i++) {
		chamelium_edid->raw[i] = NULL;
		chamelium_edid->ids[i] = 0;
	}

	return chamelium_edid;
}

void free_chamelium_edid_from_monitor_edid(struct chamelium_edid *edid)
{
	int i;

	free(edid->base);
	for (i = 0; i < CHAMELIUM_MAX_PORTS; i++)
		free(edid->raw[i]);

	free(edid);
	edid = NULL;
}
