/* SPDX-License-Identifier: MIT */
/*
 * A helper library for parsing and making use of real EDID data from monitors
 * and make them compatible with IGT and Chamelium.
 *
 * Copyright 2022 Google LLC.
 *
 * Authors: Mark Yacoub <markyacoub@chromium.org>
 */

#ifndef TESTS_CHAMELIUM_MONITOR_EDIDS_MONITOR_EDIDS_HELPER_H_
#define TESTS_CHAMELIUM_MONITOR_EDIDS_MONITOR_EDIDS_HELPER_H_

#include <stdint.h>

#include "igt_chamelium.h"

/* Max Length can be increased as needed, when new EDIDs are added. */
#define EDID_NAME_MAX_LEN 28
#define EDID_HEX_STR_MAX_LEN 512

typedef struct monitor_edid {
	char name[EDID_NAME_MAX_LEN + 1];
	char edid[EDID_HEX_STR_MAX_LEN + 1];
} monitor_edid;

const char *monitor_edid_get_name(const monitor_edid *edid);
struct chamelium_edid *
get_chameleon_edid_from_monitor_edid(struct chamelium *chamelium,
				     const monitor_edid *edid);
void free_chamelium_edid_from_monitor_edid(struct chamelium_edid *edid);

#endif /* TESTS_CHAMELIUM_MONITOR_EDIDS_MONITOR_EDIDS_HELPER_H_ */