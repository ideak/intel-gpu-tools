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
 */

#ifndef __IGT_DEVICE_SCAN_H__
#define __IGT_DEVICE_SCAN_H__

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>

enum igt_devices_print_type {
	IGT_PRINT_SIMPLE,
	IGT_PRINT_DETAIL,
};

#define INTEGRATED_I915_GPU_PCI_ID "0000:00:02.0"
#define PCI_SLOT_NAME_SIZE 12
struct igt_device_card {
	char subsystem[NAME_MAX];
	char card[NAME_MAX];
	char render[NAME_MAX];
	char pci_slot_name[PCI_SLOT_NAME_SIZE+1];
};

void igt_devices_scan(bool force);

void igt_devices_print(enum igt_devices_print_type printtype);
void igt_devices_print_vendors(void);
void igt_device_print_filter_types(void);

void igt_devices_free(void);

/*
 * Handle device filter collection array.
 * IGT can store/retrieve filters passed by user using '--device' args.
 */

int igt_device_filter_count(void);
int igt_device_filter_add(const char *filter);
void igt_device_filter_free_all(void);
const char *igt_device_filter_get(int num);

/* Use filter to match the device and fill card structure */
bool igt_device_card_match(const char *filter, struct igt_device_card *card);
bool igt_device_card_match_pci(const char *filter,
	struct igt_device_card *card);
bool igt_device_find_first_i915_discrete_card(struct igt_device_card *card);
bool igt_device_find_integrated_card(struct igt_device_card *card);
int igt_open_card(struct igt_device_card *card);
int igt_open_render(struct igt_device_card *card);

#endif /* __IGT_DEVICE_SCAN_H__ */
