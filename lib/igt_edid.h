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

#include <xf86drmMode.h>

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

#define EDID_PT_INTERLACED (1 << 7)
#define EDID_PT_STEREO (1 << 5)

/* Sync type */
#define EDID_PT_SYNC_ANALOG (0b00 << 3)
#define EDID_PT_SYNC_DIGITAL_COMPOSITE (0b10 << 3)
#define EDID_PT_SYNC_DIGITAL_SEPARATE (0b11 << 3)

/* Applies to EDID_PT_SYNC_DIGITAL_SEPARATE only */
#define EDID_PT_VSYNC_POSITIVE (1 << 2)
#define EDID_PT_HSYNC_POSITIVE (1 << 1)

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
	uint8_t misc; /* EDID_PT_* */
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

enum cea_sad_format {
	CEA_SAD_FORMAT_PCM = 1,
	CEA_SAD_FORMAT_AC3 = 2,
	CEA_SAD_FORMAT_MPEG1 = 3, /* Layers 1 & 2 */
	CEA_SAD_FORMAT_MP3 = 4,
	CEA_SAD_FORMAT_MPEG2 = 5,
	CEA_SAD_FORMAT_AAC = 6,
	CEA_SAD_FORMAT_DTS = 7,
	CEA_SAD_FORMAT_ATRAC = 8,
	CEA_SAD_FORMAT_SACD = 9, /* One-bit audio */
	CEA_SAD_FORMAT_DD_PLUS = 10,
	CEA_SAD_FORMAT_DTS_HD = 11,
	CEA_SAD_FORMAT_DOLBY = 12, /* MLP/Dolby TrueHD */
	CEA_SAD_FORMAT_DST = 13,
	CEA_SAD_FORMAT_WMA = 14, /* Microsoft WMA Pro */
};

enum cea_sad_sampling_rate {
	CEA_SAD_SAMPLING_RATE_32KHZ = 1 << 0,
	CEA_SAD_SAMPLING_RATE_44KHZ = 1 << 1,
	CEA_SAD_SAMPLING_RATE_48KHZ = 1 << 2,
	CEA_SAD_SAMPLING_RATE_88KHZ = 1 << 3,
	CEA_SAD_SAMPLING_RATE_96KHZ = 1 << 4,
	CEA_SAD_SAMPLING_RATE_176KHZ = 1 << 5,
	CEA_SAD_SAMPLING_RATE_192KHZ = 1 << 6,
};

/* for PCM only */
enum cea_sad_pcm_sample_size {
	CEA_SAD_SAMPLE_SIZE_16 = 1 << 0,
	CEA_SAD_SAMPLE_SIZE_20 = 1 << 1,
	CEA_SAD_SAMPLE_SIZE_24 = 1 << 2,
};

/* Short Audio Descriptor */
struct cea_sad {
	uint8_t format_channels;
	uint8_t sampling_rates;
	uint8_t bitrate;
} __attribute__((packed));

/* Vendor Specific Data */
struct cea_vsd {
	uint8_t ieee_oui[3];
	char data[];
};

enum cea_speaker_alloc_item {
	CEA_SPEAKER_FRONT_LEFT_RIGHT = 1 << 0,
	CEA_SPEAKER_LFE = 1 << 1,
	CEA_SPEAKER_FRONT_CENTER = 1 << 2,
	CEA_SPEAKER_REAR_LEFT_RIGHT = 1 << 3,
	CEA_SPEAKER_REAR_CENTER = 1 << 4,
	CEA_SPEAKER_FRONT_LEFT_RIGHT_CENTER = 1 << 5,
	CEA_SPEAKER_REAR_LEFT_RIGHT_CENTER = 1 << 6,
};

struct cea_speaker_alloc {
	uint8_t speakers; /* enum cea_speaker_alloc_item */
	uint8_t reserved[2];
} __attribute__((packed));

enum edid_cea_data_type {
	EDID_CEA_DATA_AUDIO = 1,
	EDID_CEA_DATA_VIDEO = 2,
	EDID_CEA_DATA_VENDOR_SPECIFIC = 3,
	EDID_CEA_DATA_SPEAKER_ALLOC = 4,
};

struct edid_cea_data_block {
	uint8_t type_len; /* type is from enum edid_cea_data_type */
	union {
		struct cea_sad sads[0];
		struct cea_vsd vsds[0];
		struct cea_speaker_alloc speakers[0];
	} data;
} __attribute__((packed));

enum edid_cea_flag {
	EDID_CEA_YCBCR422 = 1 << 4,
	EDID_CEA_YCBCR444 = 1 << 5,
	EDID_CEA_BASIC_AUDIO = 1 << 6,
	EDID_CEA_UNDERSCAN = 1 << 7,
};

struct edid_cea {
	uint8_t revision;
	uint8_t dtd_start;
	uint8_t misc;
	char data[123]; /* DBC & DTD collection, padded with zeros */
	uint8_t checksum;
} __attribute__((packed));

enum edid_ext_tag {
	EDID_EXT_CEA = 0x02,
};

struct edid_ext {
	uint8_t tag; /* enum edid_ext_tag */
	union {
		struct edid_cea cea;
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
	uint8_t extensions_len;
	uint8_t checksum;
	struct edid_ext extensions[];
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

void cea_sad_init_pcm(struct cea_sad *sad, int channels,
		      uint8_t sampling_rates, uint8_t sample_sizes);
void edid_ext_update_cea_checksum(struct edid_ext *ext);
const struct cea_vsd *cea_vsd_get_hdmi_default(size_t *size);
size_t edid_cea_data_block_set_sad(struct edid_cea_data_block *block,
				   const struct cea_sad *sads, size_t sads_len);
size_t edid_cea_data_block_set_vsd(struct edid_cea_data_block *block,
				   const struct cea_vsd *vsd, size_t vsd_size);
size_t edid_cea_data_block_set_speaker_alloc(struct edid_cea_data_block *block,
					     const struct cea_speaker_alloc *speakers);
void edid_ext_set_cea(struct edid_ext *ext, size_t data_blocks_size,
		      uint8_t flags);

#endif
