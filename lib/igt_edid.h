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

#ifndef IGT_EDID_H
#define IGT_EDID_H

#include "config.h"

#include <stdint.h>

struct est_timings {
	uint8_t t1;
	uint8_t t2;
	uint8_t mfg_rsvd;
} __attribute__((packed));

#define STD_TIMINGS_LEN 8

enum std_timing_aspect {
	STD_TIMING_16_10 = 0b00,
	STD_TIMING_4_3 = 0b01,
	STD_TIMING_5_4 = 0b10,
	STD_TIMING_16_9 = 0b11,
};

struct std_timing {
	uint8_t hsize;
	uint8_t vfreq_aspect;
} __attribute__((packed));

#define DETAILED_TIMINGS_LEN 4

#define EDID_PT_HSYNC_POSITIVE (1 << 1)
#define EDID_PT_VSYNC_POSITIVE (1 << 2)
#define EDID_PT_SEPARATE_SYNC  (3 << 3)
#define EDID_PT_STEREO         (1 << 5)
#define EDID_PT_INTERLACED     (1 << 7)

struct detailed_pixel_timing {
	uint8_t hactive_lo;
	uint8_t hblank_lo;
	uint8_t hactive_hblank_hi;
	uint8_t vactive_lo;
	uint8_t vblank_lo;
	uint8_t vactive_vblank_hi;
	uint8_t hsync_offset_lo;
	uint8_t hsync_pulse_width_lo;
	uint8_t vsync_offset_pulse_width_lo;
	uint8_t hsync_vsync_offset_pulse_width_hi;
	uint8_t width_mm_lo;
	uint8_t height_mm_lo;
	uint8_t width_height_mm_hi;
	uint8_t hborder;
	uint8_t vborder;
	uint8_t misc;
} __attribute__((packed));

struct detailed_data_string {
	char str[13];
} __attribute__((packed));

struct detailed_data_monitor_range {
	uint8_t min_vfreq;
	uint8_t max_vfreq;
	uint8_t min_hfreq_khz;
	uint8_t max_hfreq_khz;
	uint8_t pixel_clock_mhz; /* need to multiply by 10 */
	uint8_t flags;
	union {
		char pad[7];
		struct {
			uint8_t reserved;
			uint8_t hfreq_start_khz; /* need to multiply by 2 */
			uint8_t c; /* need to divide by 2 */
			uint8_t m[2];
			uint8_t k;
			uint8_t j; /* need to divide by 2 */
		} __attribute__((packed)) gtf2;
		struct {
			uint8_t version;
			uint8_t data1; /* high 6 bits: extra clock resolution */
			uint8_t data2; /* plus low 2 of above: max hactive */
			uint8_t supported_aspects;
			uint8_t flags; /* preferred aspect and blanking support */
			uint8_t supported_scalings;
			uint8_t preferred_refresh;
		} __attribute__((packed)) cvt;
	} formula;
} __attribute__((packed));

enum detailed_non_pixel_type {
	EDID_DETAIL_EST_TIMINGS = 0xf7,
	EDID_DETAIL_CVT_3BYTE = 0xf8,
	EDID_DETAIL_COLOR_MGMT_DATA = 0xf9,
	EDID_DETAIL_STD_MODES = 0xfa,
	EDID_DETAIL_MONITOR_CPDATA = 0xfb,
	EDID_DETAIL_MONITOR_NAME = 0xfc,
	EDID_DETAIL_MONITOR_RANGE = 0xfd,
	EDID_DETAIL_MONITOR_STRING = 0xfe,
	EDID_DETAIL_MONITOR_SERIAL = 0xff,
};

struct detailed_non_pixel {
	uint8_t pad1;
	uint8_t type; /* enum detailed_non_pixel_type */
	uint8_t pad2;
	union {
		struct detailed_data_string str;
		struct detailed_data_monitor_range range;
		struct detailed_data_string string;
		/* TODO: other types */
	} data;
} __attribute__((packed));

struct detailed_timing {
	uint8_t pixel_clock[2]; /* need to multiply by 10 KHz, zero if not a pixel timing */
	union {
		struct detailed_pixel_timing pixel_data;
		struct detailed_non_pixel other_data;
	} data;
} __attribute__((packed));

struct edid {
	char header[8];
	/* Vendor & product info */
	uint8_t mfg_id[2];
	uint8_t prod_code[2];
	uint8_t serial[4];
	uint8_t mfg_week;
	uint8_t mfg_year;
	/* EDID version */
	uint8_t version;
	uint8_t revision;
	/* Display info: */
	uint8_t input;
	uint8_t width_cm;
	uint8_t height_cm;
	uint8_t gamma;
	uint8_t features;
	/* Color characteristics */
	uint8_t red_green_lo;
	uint8_t black_white_lo;
	uint8_t red_x;
	uint8_t red_y;
	uint8_t green_x;
	uint8_t green_y;
	uint8_t blue_x;
	uint8_t blue_y;
	uint8_t white_x;
	uint8_t white_y;
	/* Est. timings and mfg rsvd timings*/
	struct est_timings established_timings;
	/* Standard timings 1-8*/
	struct std_timing standard_timings[STD_TIMINGS_LEN];
	/* Detailing timings 1-4 */
	struct detailed_timing detailed_timings[DETAILED_TIMINGS_LEN];
	/* Number of 128 byte ext. blocks */
	uint8_t extensions;
	/* Checksum */
	uint8_t checksum;
} __attribute__((packed));

void edid_init(struct edid *edid);
void edid_init_with_mode(struct edid *edid, drmModeModeInfo *mode);
void edid_update_checksum(struct edid *edid);
void detailed_timing_set_mode(struct detailed_timing *dt, drmModeModeInfo *mode,
			      int width_mm, int height_mm);
void detailed_timing_set_monitor_range_mode(struct detailed_timing *dt,
					    drmModeModeInfo *mode);
void detailed_timing_set_string(struct detailed_timing *dt,
				enum detailed_non_pixel_type type,
				const char *str);

#endif
