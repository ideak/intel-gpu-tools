/*
 * Copyright © 2006 Intel Corporation
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
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 * Authors:
 *    Eric Anholt <eric@anholt.net>
 *
 */

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "igt_aux.h"
#include "intel_io.h"
#include "intel_chipset.h"
#include "drmtest.h"

/* kernel types for intel_vbt_defs.h */
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
#define __packed __attribute__ ((packed))

#define _INTEL_BIOS_PRIVATE
#include "intel_vbt_defs.h"

/* no bother to include "edid.h" */
#define _H_ACTIVE(x) (x[2] + ((x[4] & 0xF0) << 4))
#define _H_SYNC_OFF(x) (x[8] + ((x[11] & 0xC0) << 2))
#define _H_SYNC_WIDTH(x) (x[9] + ((x[11] & 0x30) << 4))
#define _H_BLANK(x) (x[3] + ((x[4] & 0x0F) << 8))
#define _V_ACTIVE(x) (x[5] + ((x[7] & 0xF0) << 4))
#define _V_SYNC_OFF(x) ((x[10] >> 4) + ((x[11] & 0x0C) << 2))
#define _V_SYNC_WIDTH(x) ((x[10] & 0x0F) + ((x[11] & 0x03) << 4))
#define _V_BLANK(x) (x[6] + ((x[7] & 0x0F) << 8))
#define _PIXEL_CLOCK(x) (x[0] + (x[1] << 8)) * 10000

#define YESNO(val) ((val) ? "yes" : "no")

/* This is not for mapping to memory layout. */
struct bdb_block {
	uint8_t id;
	uint32_t size;
	uint8_t data[];
};

struct context {
	const struct vbt_header *vbt;
	const struct bdb_header *bdb;
	int size;

	uint32_t devid;
	int panel_type;
	bool dump_all_panel_types;
	bool hexdump;
};

/* Get BDB block size given a pointer to Block ID. */
static uint32_t _get_blocksize(const uint8_t *block_base)
{
	/* The MIPI Sequence Block v3+ has a separate size field. */
	if (*block_base == BDB_MIPI_SEQUENCE && *(block_base + 3) >= 3)
		return *((const uint32_t *)(block_base + 4));
	else
		return *((const uint16_t *)(block_base + 1));
}

/* Get BDB block size give a pointer to data after Block ID and Block Size. */
static u32 get_blocksize(const void *block_data)
{
	return _get_blocksize(block_data - 3);
}

static const void *find_raw_section(const struct context *context, int section_id)
{
	const struct bdb_header *bdb = context->bdb;
	int length = context->size;
	const uint8_t *base = (const uint8_t *)bdb;
	int index = 0;
	uint32_t total, current_size;
	unsigned char current_id;

	/* skip to first section */
	index += bdb->header_size;
	total = bdb->bdb_size;
	if (total > length)
		total = length;

	/* walk the sections looking for section_id */
	while (index + 3 < total) {
		current_id = *(base + index);
		current_size = _get_blocksize(base + index);
		index += 3;

		if (index + current_size > total)
			return NULL;

		if (current_id == section_id)
			return base + index;

		index += current_size;
	}

	return NULL;
}

/*
 * Offset from the start of BDB to the start of the
 * block data (just past the block header).
 */
static u32 raw_block_offset(const struct context *context, enum bdb_block_id section_id)
{
	const void *block;

	block = find_raw_section(context, section_id);
	if (!block)
		return 0;

	return block - (const void *)context->bdb;
}

static const void *block_data(const struct bdb_block *block)
{
	return block->data + 3;
}

static struct bdb_block *find_section(const struct context *context, int section_id);

static size_t lfp_data_min_size(const struct context *context)
{
	const struct bdb_lvds_lfp_data_ptrs *ptrs;
	struct bdb_block *ptrs_block;
	size_t size;

	ptrs_block = find_section(context, BDB_LVDS_LFP_DATA_PTRS);
	if (!ptrs_block)
		return 0;

	ptrs = block_data(ptrs_block);

	size = sizeof(struct bdb_lvds_lfp_data);
	if (ptrs->panel_name.table_size)
		size = max(size, ptrs->panel_name.offset +
			   sizeof(struct bdb_lvds_lfp_data_tail));

	free(ptrs_block);

	return size;
}

static int make_lvds_data_ptr(struct lvds_lfp_data_ptr_table *table,
			      int table_size, int total_size)
{
	if (total_size < table_size)
		return total_size;

	table->table_size = table_size;
	table->offset = total_size - table_size;

	return total_size - table_size;
}

static void next_lvds_data_ptr(struct lvds_lfp_data_ptr_table *next,
			       const struct lvds_lfp_data_ptr_table *prev,
			       int size)
{
	next->table_size = prev->table_size;
	next->offset = prev->offset + size;
}

static void *generate_lvds_data_ptrs(const struct context *context)
{
	int size, table_size, block_size, offset, fp_timing_size;
	const void *block;
	struct bdb_lvds_lfp_data_ptrs *ptrs;
	void *ptrs_block;

	/*
	 * The hardcoded fp_timing_size is only valid for
	 * modernish VBTs. All older VBTs definitely should
	 * include block 41 and thus we don't need to
	 * generate one.
	 */
	if (context->bdb->version < 155)
		return NULL;

	fp_timing_size = 38;

	block = find_raw_section(context, BDB_LVDS_LFP_DATA);
	if (!block)
		return NULL;

	block_size = get_blocksize(block);

	size = block_size;

	size = fp_timing_size + sizeof(struct lvds_dvo_timing) +
		sizeof(struct lvds_pnp_id);
	if (size * 16 > block_size)
		return NULL;

	ptrs_block = calloc(1, sizeof(*ptrs) + 3);
	if (!ptrs_block)
		return NULL;

	*(uint8_t *)(ptrs_block + 0) = BDB_LVDS_LFP_DATA_PTRS;
	*(uint16_t *)(ptrs_block + 1) = sizeof(*ptrs);
	ptrs = ptrs_block + 3;

	table_size = sizeof(struct lvds_pnp_id);
	size = make_lvds_data_ptr(&ptrs->ptr[0].panel_pnp_id, table_size, size);

	table_size = sizeof(struct lvds_dvo_timing);
	size = make_lvds_data_ptr(&ptrs->ptr[0].dvo_timing, table_size, size);

	table_size = fp_timing_size;
	size = make_lvds_data_ptr(&ptrs->ptr[0].fp_timing, table_size, size);

	if (ptrs->ptr[0].fp_timing.table_size)
		ptrs->lvds_entries++;
	if (ptrs->ptr[0].dvo_timing.table_size)
		ptrs->lvds_entries++;
	if (ptrs->ptr[0].panel_pnp_id.table_size)
		ptrs->lvds_entries++;

	if (size != 0 || ptrs->lvds_entries != 3)
		return NULL;

	size = fp_timing_size + sizeof(struct lvds_dvo_timing) +
		sizeof(struct lvds_pnp_id);
	for (int i = 1; i < 16; i++) {
		next_lvds_data_ptr(&ptrs->ptr[i].fp_timing, &ptrs->ptr[i-1].fp_timing, size);
		next_lvds_data_ptr(&ptrs->ptr[i].dvo_timing, &ptrs->ptr[i-1].dvo_timing, size);
		next_lvds_data_ptr(&ptrs->ptr[i].panel_pnp_id, &ptrs->ptr[i-1].panel_pnp_id, size);
	}

	table_size = sizeof(struct lvds_lfp_panel_name);

	if (16 * (size + table_size) <= block_size) {
		ptrs->panel_name.table_size = table_size;
		ptrs->panel_name.offset = size * 16;
	}

	offset = block - (const void *)context->bdb;
	for (int i = 0; i < 16; i++) {
		ptrs->ptr[i].fp_timing.offset += offset;
		ptrs->ptr[i].dvo_timing.offset += offset;
		ptrs->ptr[i].panel_pnp_id.offset += offset;
	}

	if (ptrs->panel_name.offset)
		ptrs->panel_name.offset += offset;

	return ptrs_block;
}

static size_t block_min_size(const struct context *context, int section_id)
{
	switch (section_id) {
	case BDB_GENERAL_FEATURES:
		return sizeof(struct bdb_general_features);
	case BDB_GENERAL_DEFINITIONS:
		return sizeof(struct bdb_general_definitions);
	case BDB_PSR:
		return sizeof(struct bdb_psr);
	case BDB_CHILD_DEVICE_TABLE:
		return sizeof(struct bdb_legacy_child_devices);
	case BDB_DRIVER_FEATURES:
		return sizeof(struct bdb_driver_features);
	case BDB_SDVO_LVDS_OPTIONS:
		return sizeof(struct bdb_sdvo_lvds_options);
	case BDB_SDVO_PANEL_DTDS:
		/* FIXME? */
		return 0;
	case BDB_EDP:
		return sizeof(struct bdb_edp);
	case BDB_LVDS_OPTIONS:
		return sizeof(struct bdb_lvds_options);
	case BDB_LVDS_LFP_DATA_PTRS:
		return sizeof(struct bdb_lvds_lfp_data_ptrs);
	case BDB_LVDS_LFP_DATA:
		return lfp_data_min_size(context);
	case BDB_LVDS_BACKLIGHT:
		return sizeof(struct bdb_lfp_backlight_data);
	case BDB_LFP_POWER:
		return sizeof(struct bdb_lfp_power);
	case BDB_MIPI_CONFIG:
		return sizeof(struct bdb_mipi_config);
	case BDB_MIPI_SEQUENCE:
		return sizeof(struct bdb_mipi_sequence);
	case BDB_COMPRESSION_PARAMETERS:
		return sizeof(struct bdb_compression_parameters);
	case BDB_GENERIC_DTD:
		/* FIXME check spec */
		return sizeof(struct bdb_generic_dtd);
	default:
		return 0;
	}
}

static bool validate_lfp_data_ptrs(const struct context *context,
				   const struct bdb_lvds_lfp_data_ptrs *ptrs)
{
	int fp_timing_size, dvo_timing_size, panel_pnp_id_size, panel_name_size;
	int data_block_size, lfp_data_size;
	const void *block;
	int i;

	block = find_raw_section(context, BDB_LVDS_LFP_DATA);
	if (!block)
		return false;

	data_block_size = get_blocksize(block);
	if (data_block_size == 0)
		return false;

	/* always 3 indicating the presence of fp_timing+dvo_timing+panel_pnp_id */
	if (ptrs->lvds_entries != 3)
		return false;

	fp_timing_size = ptrs->ptr[0].fp_timing.table_size;
	dvo_timing_size = ptrs->ptr[0].dvo_timing.table_size;
	panel_pnp_id_size = ptrs->ptr[0].panel_pnp_id.table_size;
	panel_name_size = ptrs->panel_name.table_size;

	/* fp_timing has variable size */
	if (fp_timing_size < 32 ||
	    dvo_timing_size != sizeof(struct lvds_dvo_timing) ||
	    panel_pnp_id_size != sizeof(struct lvds_pnp_id))
		return false;

	/* panel_name is not present in old VBTs */
	if (panel_name_size != 0 &&
	    panel_name_size != sizeof(struct lvds_lfp_panel_name))
		return false;

	lfp_data_size = ptrs->ptr[1].fp_timing.offset - ptrs->ptr[0].fp_timing.offset;
	if (16 * lfp_data_size > data_block_size)
		return false;

	/* make sure the table entries have uniform size */
	for (i = 1; i < 16; i++) {
		if (ptrs->ptr[i].fp_timing.table_size != fp_timing_size ||
		    ptrs->ptr[i].dvo_timing.table_size != dvo_timing_size ||
		    ptrs->ptr[i].panel_pnp_id.table_size != panel_pnp_id_size)
			return false;

		if (ptrs->ptr[i].fp_timing.offset - ptrs->ptr[i-1].fp_timing.offset != lfp_data_size ||
		    ptrs->ptr[i].dvo_timing.offset - ptrs->ptr[i-1].dvo_timing.offset != lfp_data_size ||
		    ptrs->ptr[i].panel_pnp_id.offset - ptrs->ptr[i-1].panel_pnp_id.offset != lfp_data_size)
			return false;
	}

	/*
	 * Except for vlv/chv machines all real VBTs seem to have 6
	 * unaccounted bytes in the fp_timing table. And it doesn't
	 * appear to be a really intentional hole as the fp_timing
	 * 0xffff terminator is always within those 6 missing bytes.
	 */
	if (fp_timing_size + 6 + dvo_timing_size + panel_pnp_id_size == lfp_data_size)
		fp_timing_size += 6;

	if (fp_timing_size + dvo_timing_size + panel_pnp_id_size != lfp_data_size)
		return false;

	if (ptrs->ptr[0].fp_timing.offset + fp_timing_size != ptrs->ptr[0].dvo_timing.offset ||
	    ptrs->ptr[0].dvo_timing.offset + dvo_timing_size != ptrs->ptr[0].panel_pnp_id.offset ||
	    ptrs->ptr[0].panel_pnp_id.offset + panel_pnp_id_size != lfp_data_size)
		return false;

	/* make sure the tables fit inside the data block */
	for (i = 0; i < 16; i++) {
		if (ptrs->ptr[i].fp_timing.offset + fp_timing_size > data_block_size ||
		    ptrs->ptr[i].dvo_timing.offset + dvo_timing_size > data_block_size ||
		    ptrs->ptr[i].panel_pnp_id.offset + panel_pnp_id_size > data_block_size)
			return false;
	}

	if (ptrs->panel_name.offset + 16 * panel_name_size > data_block_size)
		return false;

	/* make sure fp_timing terminators are present at expected locations */
	for (i = 0; i < 16; i++) {
		const u16 *t = block + ptrs->ptr[i].fp_timing.offset + fp_timing_size - 2;

		if (*t != 0xffff)
			return false;
	}

	return true;
}

/* make the data table offsets relative to the data block */
static bool fixup_lfp_data_ptrs(const struct context *context,
				void *ptrs_block)
{
	struct bdb_lvds_lfp_data_ptrs *ptrs = ptrs_block;
	u32 offset;
	int i;

	offset = raw_block_offset(context, BDB_LVDS_LFP_DATA);

	for (i = 0; i < 16; i++) {
		if (ptrs->ptr[i].fp_timing.offset < offset ||
		    ptrs->ptr[i].dvo_timing.offset < offset ||
		    ptrs->ptr[i].panel_pnp_id.offset < offset)
			return false;

		ptrs->ptr[i].fp_timing.offset -= offset;
		ptrs->ptr[i].dvo_timing.offset -= offset;
		ptrs->ptr[i].panel_pnp_id.offset -= offset;
	}

	if (ptrs->panel_name.table_size) {
		if (ptrs->panel_name.offset < offset)
			return false;

		ptrs->panel_name.offset -= offset;
	}

	return validate_lfp_data_ptrs(context, ptrs);
}

static struct bdb_block *find_section(const struct context *context, int section_id)
{
	size_t min_size = block_min_size(context, section_id);
	struct bdb_block *block;
	void *temp_block = NULL;
	const void *data;
	size_t size;

	data = find_raw_section(context, section_id);
	if (!data && section_id == BDB_LVDS_LFP_DATA_PTRS) {
		fprintf(stderr, "Generating LVDS data table pointers\n");
		temp_block = generate_lvds_data_ptrs(context);
		if (temp_block)
			data = temp_block + 3;
	}
	if (!data)
		return NULL;

	size = get_blocksize(data);

	/*
	 * Version number and new block size are considered
	 * part of the header for MIPI sequenece block v3+.
	 */
	if (section_id == BDB_MIPI_SEQUENCE && *(const u8*)data >= 3)
		size += 5;

	/* expect to have the full definition for each block with modern VBTs */
	if (min_size && size > min_size &&
	    section_id != BDB_CHILD_DEVICE_TABLE &&
	    section_id != BDB_SDVO_LVDS_OPTIONS &&
	    section_id != BDB_GENERAL_DEFINITIONS &&
	    context->bdb->version >= 155)
		fprintf(stderr, "Block %d min size %zu less than block size %zu\n",
			section_id, min_size, size);

	block = calloc(1, sizeof(*block) + 3 + max(size, min_size));
	if (!block) {
		free(temp_block);
		return NULL;
	}

	block->id = section_id;
	block->size = size;
	memcpy(block->data, data - 3, 3 + size);

	free(temp_block);

	if (section_id == BDB_LVDS_LFP_DATA_PTRS &&
	    !fixup_lfp_data_ptrs(context, 3 + block->data)) {
		fprintf(stderr, "VBT has malformed LFP data table pointers\n");
		free(block);
		return NULL;
	}

	return block;
}

static unsigned int panel_bits(unsigned int value, int panel_type, int num_bits)
{
	return (value >> (panel_type * num_bits)) & (BIT(num_bits) - 1);
}

static bool panel_bool(unsigned int value, int panel_type)
{
	return panel_bits(value, panel_type, 1);
}

static int decode_ssc_freq(struct context *context, bool alternate)
{
	switch (intel_gen(context->devid)) {
	case 2:
		return alternate ? 66 : 48;
	case 3:
	case 4:
		return alternate ? 100 : 96;
	default:
		return alternate ? 100 : 120;
	}
}

static const char * const panel_fitting[] = {
	[0] = "disabled",
	[1] = "text only",
	[2] = "graphics only",
	[3] = "text & graphics",
};

static void dump_general_features(struct context *context,
				  const struct bdb_block *block)
{
	const struct bdb_general_features *features = block_data(block);

	printf("\tPanel fitting: %s (0x%x)\n",
	       panel_fitting[features->panel_fitting], features->panel_fitting);
	printf("\tFlexaim: %s\n", YESNO(features->flexaim));
	printf("\tMessage: %s\n", YESNO(features->msg_enable));
	printf("\tClear screen: %d\n", features->clear_screen);
	printf("\tDVO color flip required: %s\n", YESNO(features->color_flip));

	printf("\tExternal VBT: %s\n", YESNO(features->download_ext_vbt));
	printf("\tLVDS SSC Enable: %s\n", YESNO(features->enable_ssc));
	printf("\tLVDS SSC frequency: %d MHz (0x%x)\n",
	       decode_ssc_freq(context, features->ssc_freq),
	       features->ssc_freq);
	printf("\tLFP on override: %s\n",
	       YESNO(features->enable_lfp_on_override));
	printf("\tDisable SSC on clone: %s\n",
	       YESNO(features->disable_ssc_ddt));
	printf("\tUnderscan support for VGA timings: %s\n",
	       YESNO(features->underscan_vga_timings));
	if (context->bdb->version >= 183)
		printf("\tDynamic CD clock: %s\n", YESNO(features->display_clock_mode));
	printf("\tHotplug support in VBIOS: %s\n",
	       YESNO(features->vbios_hotplug_support));

	printf("\tDisable smooth vision: %s\n",
	       YESNO(features->disable_smooth_vision));
	printf("\tSingle DVI for CRT/DVI: %s\n", YESNO(features->single_dvi));
	if (context->bdb->version >= 181)
		printf("\tEnable 180 degree rotation: %s\n", YESNO(features->rotate_180));
	printf("\tInverted FDI Rx polarity: %s\n", YESNO(features->fdi_rx_polarity_inverted));
	if (context->bdb->version >= 160) {
		printf("\tExtended VBIOS mode: %s\n", YESNO(features->vbios_extended_mode));
		printf("\tCopy iLFP DTD to SDVO LVDS DTD: %s\n", YESNO(features->copy_ilfp_dtd_to_sdvo_lvds_dtd));
		printf("\tBest fit panel timing algorithm: %s\n", YESNO(features->panel_best_fit_timing));
		printf("\tIgnore strap state: %s\n", YESNO(features->ignore_strap_state));
	}

	printf("\tLegacy monitor detect: %s\n",
	       YESNO(features->legacy_monitor_detect));

	printf("\tIntegrated CRT: %s\n", YESNO(features->int_crt_support));
	printf("\tIntegrated TV: %s\n", YESNO(features->int_tv_support));
	printf("\tIntegrated EFP: %s\n", YESNO(features->int_efp_support));
	printf("\tDP SSC enable: %s\n", YESNO(features->dp_ssc_enable));
	printf("\tDP SSC frequency: %d MHz (0x%x)\n",
	       decode_ssc_freq(context, features->dp_ssc_freq),
	       features->dp_ssc_freq);
	printf("\tDP SSC dongle supported: %s\n", YESNO(features->dp_ssc_dongle_supported));
}

static void dump_backlight_info(struct context *context,
				const struct bdb_block *block)
{
	const struct bdb_lfp_backlight_data *backlight = block_data(block);
	const struct lfp_backlight_data_entry *blc;
	const struct lfp_backlight_control_method *control;
	int i;

	if (sizeof(*blc) != backlight->entry_size) {
		printf("\tBacklight struct sizes don't match (expected %zu, got %u), skipping\n",
		     sizeof(*blc), backlight->entry_size);
		return;
	}

	for (i = 0; i < ARRAY_SIZE(backlight->data); i++) {
		if (i != context->panel_type && !context->dump_all_panel_types)
			continue;

		printf("\tPanel %d%s\n", i,
		       context->panel_type == i ? " *" : "");

		blc = &backlight->data[i];

		printf("\t\tInverter type: %u\n", blc->type);
		printf("\t\tActive low: %u\n", blc->active_low_pwm);
		printf("\t\tPWM freq: %u\n", blc->pwm_freq_hz);
		printf("\t\tMinimum brightness: %u\n", blc->min_brightness);

		printf("\t\tLevel: %u\n", backlight->level[i]);

		control = &backlight->backlight_control[i];

		printf("\t\tControl type: %u\n", control->type);
		printf("\t\tController: %u\n", control->controller);
	}
}

static const struct {
	unsigned short type;
	const char *name;
} child_device_types[] = {
	{ DEVICE_TYPE_NONE, "none" },
	{ DEVICE_TYPE_CRT, "CRT" },
	{ DEVICE_TYPE_TV, "TV" },
	{ DEVICE_TYPE_EFP, "EFP" },
	{ DEVICE_TYPE_LFP, "LFP" },
	{ DEVICE_TYPE_CRT_DPMS, "CRT" },
	{ DEVICE_TYPE_CRT_DPMS_HOTPLUG, "CRT" },
	{ DEVICE_TYPE_TV_COMPOSITE, "TV composite" },
	{ DEVICE_TYPE_TV_MACROVISION, "TV" },
	{ DEVICE_TYPE_TV_RF_COMPOSITE, "TV" },
	{ DEVICE_TYPE_TV_SVIDEO_COMPOSITE, "TV S-Video" },
	{ DEVICE_TYPE_TV_SCART, "TV SCART" },
	{ DEVICE_TYPE_TV_CODEC_HOTPLUG_PWR, "TV" },
	{ DEVICE_TYPE_EFP_HOTPLUG_PWR, "EFP" },
	{ DEVICE_TYPE_EFP_DVI_HOTPLUG_PWR, "DVI" },
	{ DEVICE_TYPE_EFP_DVI_I, "DVI-I" },
	{ DEVICE_TYPE_EFP_DVI_D_DUAL, "DL-DVI-D" },
	{ DEVICE_TYPE_EFP_DVI_D_HDCP, "DVI-D" },
	{ DEVICE_TYPE_OPENLDI_HOTPLUG_PWR, "OpenLDI" },
	{ DEVICE_TYPE_OPENLDI_DUALPIX, "OpenLDI" },
	{ DEVICE_TYPE_LFP_PANELLINK, "PanelLink" },
	{ DEVICE_TYPE_LFP_CMOS_PWR, "CMOS LFP" },
	{ DEVICE_TYPE_LFP_LVDS_PWR, "LVDS" },
	{ DEVICE_TYPE_LFP_LVDS_DUAL, "LVDS" },
	{ DEVICE_TYPE_LFP_LVDS_DUAL_HDCP, "LVDS" },
	{ DEVICE_TYPE_INT_LFP, "LFP" },
	{ DEVICE_TYPE_INT_TV, "TV" },
	{ DEVICE_TYPE_DP, "DisplayPort" },
	{ DEVICE_TYPE_DP_DUAL_MODE, "DisplayPort/HDMI/DVI" },
	{ DEVICE_TYPE_DP_DVI, "DisplayPort/DVI" },
	{ DEVICE_TYPE_HDMI, "HDMI/DVI" },
	{ DEVICE_TYPE_DVI, "DVI" },
	{ DEVICE_TYPE_eDP, "eDP" },
	{ DEVICE_TYPE_MIPI, "MIPI" },
};
static const int num_child_device_types =
	sizeof(child_device_types) / sizeof(child_device_types[0]);

static const char *child_device_type(unsigned short type)
{
	int i;

	for (i = 0; i < num_child_device_types; i++)
		if (child_device_types[i].type == type)
			return child_device_types[i].name;

	return "unknown";
}

static const struct {
	unsigned short mask;
	const char *name;
} child_device_type_bits[] = {
	{ DEVICE_TYPE_CLASS_EXTENSION, "Class extension" },
	{ DEVICE_TYPE_POWER_MANAGEMENT, "Power management" },
	{ DEVICE_TYPE_HOTPLUG_SIGNALING, "Hotplug signaling" },
	{ DEVICE_TYPE_INTERNAL_CONNECTOR, "Internal connector" },
	{ DEVICE_TYPE_NOT_HDMI_OUTPUT, "HDMI output" }, /* decoded as inverse */
	{ DEVICE_TYPE_MIPI_OUTPUT, "MIPI output" },
	{ DEVICE_TYPE_COMPOSITE_OUTPUT, "Composite output" },
	{ DEVICE_TYPE_DUAL_CHANNEL, "Dual channel" },
	{ 1 << 7, "Content protection" },
	{ DEVICE_TYPE_HIGH_SPEED_LINK, "High speed link" },
	{ DEVICE_TYPE_LVDS_SIGNALING, "LVDS signaling" },
	{ DEVICE_TYPE_TMDS_DVI_SIGNALING, "TMDS/DVI signaling" },
	{ DEVICE_TYPE_VIDEO_SIGNALING, "Video signaling" },
	{ DEVICE_TYPE_DISPLAYPORT_OUTPUT, "DisplayPort output" },
	{ DEVICE_TYPE_DIGITAL_OUTPUT, "Digital output" },
	{ DEVICE_TYPE_ANALOG_OUTPUT, "Analog output" },
};

static void dump_child_device_type_bits(uint16_t type)
{
	int i;

	type ^= DEVICE_TYPE_NOT_HDMI_OUTPUT;

	for (i = 0; i < ARRAY_SIZE(child_device_type_bits); i++) {
		if (child_device_type_bits[i].mask & type)
			printf("\t\t\t%s\n", child_device_type_bits[i].name);
	}
}

static const struct {
	unsigned char handle;
	const char *name;
} child_device_handles[] = {
	{ DEVICE_HANDLE_CRT, "CRT" },
	{ DEVICE_HANDLE_EFP1, "EFP 1 (HDMI/DVI/DP)" },
	{ DEVICE_HANDLE_EFP2, "EFP 2 (HDMI/DVI/DP)" },
	{ DEVICE_HANDLE_EFP3, "EFP 3 (HDMI/DVI/DP)" },
	{ DEVICE_HANDLE_EFP4, "EFP 4 (HDMI/DVI/DP)" },
	{ DEVICE_HANDLE_LFP1, "LFP 1 (eDP)" },
	{ DEVICE_HANDLE_LFP2, "LFP 2 (eDP)" },
};
static const int num_child_device_handles =
	sizeof(child_device_handles) / sizeof(child_device_handles[0]);

static const char *child_device_handle(unsigned char handle)
{
	int i;

	for (i = 0; i < num_child_device_handles; i++)
		if (child_device_handles[i].handle == handle)
			return child_device_handles[i].name;

	return "unknown";
}

static const char *dvo_port_names[] = {
	[DVO_PORT_HDMIA] = "HDMI-A",
	[DVO_PORT_HDMIB] = "HDMI-B",
	[DVO_PORT_HDMIC] = "HDMI-C",
	[DVO_PORT_HDMID] = "HDMI-D",
	[DVO_PORT_HDMIE] = "HDMI-E",
	[DVO_PORT_HDMIF] = "HDMI-F",
	[DVO_PORT_HDMIG] = "HDMI-G",
	[DVO_PORT_HDMIH] = "HDMI-H",
	[DVO_PORT_HDMII] = "HDMI-I",
	[DVO_PORT_LVDS] = "LVDS",
	[DVO_PORT_TV] = "TV",
	[DVO_PORT_CRT] = "CRT",
	[DVO_PORT_DPB] = "DP-B",
	[DVO_PORT_DPC] = "DP-C",
	[DVO_PORT_DPD] = "DP-D",
	[DVO_PORT_DPA] = "DP-A",
	[DVO_PORT_DPE] = "DP-E",
	[DVO_PORT_DPF] = "DP-F",
	[DVO_PORT_DPG] = "DP-G",
	[DVO_PORT_DPH] = "DP-H",
	[DVO_PORT_DPI] = "DP-I",
	[DVO_PORT_MIPIA] = "MIPI-A",
	[DVO_PORT_MIPIB] = "MIPI-B",
	[DVO_PORT_MIPIC] = "MIPI-C",
	[DVO_PORT_MIPID] = "MIPI-D",
};

static const char *dvo_port(uint8_t type)
{
	if (type < ARRAY_SIZE(dvo_port_names) && dvo_port_names[type])
		return dvo_port_names[type];
	else
		return "unknown";
}

static const char *aux_ch_names[] = {
	[0] = "none",
	[DP_AUX_A >> 4] = "AUX-A",
	[DP_AUX_B >> 4] = "AUX-B",
	[DP_AUX_C >> 4] = "AUX-C",
	[DP_AUX_D >> 4] = "AUX-D",
	[DP_AUX_E >> 4] = "AUX-E",
	[DP_AUX_F >> 4] = "AUX-F",
	[DP_AUX_G >> 4] = "AUX-G",
	[DP_AUX_H >> 4] = "AUX-H",
	[DP_AUX_I >> 4] = "AUX-I",
};

static const char *aux_ch(uint8_t aux_ch)
{
	aux_ch >>= 4;

	if (aux_ch < ARRAY_SIZE(aux_ch_names) && aux_ch_names[aux_ch])
		return aux_ch_names[aux_ch];
	else
		return "unknown";
}

static const char *mipi_bridge_type(uint8_t type)
{
	switch (type) {
	case 1:
		return "ASUS";
	case 2:
		return "Toshiba";
	case 3:
		return "Renesas";
	default:
		return "unknown";
	}
}

static void dump_hmdi_max_data_rate(uint8_t hdmi_max_data_rate)
{
	static const uint16_t max_data_rate[] = {
		[HDMI_MAX_DATA_RATE_PLATFORM] = 0,
		[HDMI_MAX_DATA_RATE_297] = 297,
		[HDMI_MAX_DATA_RATE_165] = 165,
		[HDMI_MAX_DATA_RATE_594] = 594,
		[HDMI_MAX_DATA_RATE_340] = 340,
		[HDMI_MAX_DATA_RATE_300] = 300,
	};

	if (hdmi_max_data_rate >= ARRAY_SIZE(max_data_rate))
		printf("\t\tHDMI max data rate: <unknown> (0x%02x)\n",
		       hdmi_max_data_rate);
	else if (hdmi_max_data_rate == HDMI_MAX_DATA_RATE_PLATFORM)
		printf("\t\tHDMI max data rate: <platform max> (0x%02x)\n",
		       hdmi_max_data_rate);
	else
		printf("\t\tHDMI max data rate: %d MHz (0x%02x)\n",
		       max_data_rate[hdmi_max_data_rate],
		       hdmi_max_data_rate);
}

static int parse_dp_max_link_rate_216(uint8_t dp_max_link_rate)
{
	static const uint16_t max_link_rate[] = {
		[BDB_216_VBT_DP_MAX_LINK_RATE_HBR3] = 810,
		[BDB_216_VBT_DP_MAX_LINK_RATE_HBR2] = 540,
		[BDB_216_VBT_DP_MAX_LINK_RATE_HBR] = 270,
		[BDB_216_VBT_DP_MAX_LINK_RATE_LBR] = 162,
	};

	return max_link_rate[dp_max_link_rate & 0x3];
}

static int parse_dp_max_link_rate_230(uint8_t dp_max_link_rate)
{
	static const uint16_t max_link_rate[] = {
		[BDB_230_VBT_DP_MAX_LINK_RATE_DEF] = 0,
		[BDB_230_VBT_DP_MAX_LINK_RATE_LBR] = 162,
		[BDB_230_VBT_DP_MAX_LINK_RATE_HBR] = 270,
		[BDB_230_VBT_DP_MAX_LINK_RATE_HBR2] = 540,
		[BDB_230_VBT_DP_MAX_LINK_RATE_HBR3] = 810,
		[BDB_230_VBT_DP_MAX_LINK_RATE_UHBR10] = 1000,
		[BDB_230_VBT_DP_MAX_LINK_RATE_UHBR13P5] = 1350,
		[BDB_230_VBT_DP_MAX_LINK_RATE_UHBR20] = 2000,
	};

	return max_link_rate[dp_max_link_rate];
}

static void dump_dp_max_link_rate(uint16_t version, uint8_t dp_max_link_rate)
{
	int link_rate;

	if (version >= 230)
		link_rate = parse_dp_max_link_rate_230(dp_max_link_rate);
	else
		link_rate = parse_dp_max_link_rate_216(dp_max_link_rate);

	if (link_rate == 0)
		printf("\t\tDP max link rate: <platform max> (0x%02x)\n",
		       dp_max_link_rate);
	else
		printf("\t\tDP max link rate: %g Gbps (0x%02x)\n",
		       link_rate / 100.0f, dp_max_link_rate);
}

static const char *dp_vswing(u8 vswing)
{
	switch (vswing) {
	case 0: return "0.4V";
	case 1: return "0.6V";
	case 2: return "0.8V";
	case 3: return "1.2V";
	default: return "<unknown>";
	}
}

static const char *dp_preemph(u8 preemph)
{
	switch (preemph) {
	case 0: return "0dB";
	case 1: return "3.5dB";
	case 2: return "6dB";
	case 3: return "9.5dB";
	default: return "<unknown>";
	}
}

static const char *hdmi_frl_rate(u8 frl_rate)
{
	switch (frl_rate) {
	case 0: return "FRL not supported";
	case 1: return "3 GT/s";
	case 2: return "6 GT/s";
	case 3: return "8 GT/s";
	case 4: return "10 GT/s";
	case 5: return "12 GT/s";
	default: return "<unknown>";
	}
}

static const char *i2c_speed(u8 i2c_speed)
{
	switch (i2c_speed) {
	case 0: return "100 kHz";
	case 1: return "50 kHz";
	case 2: return "400 kHz";
	case 3: return "1 MHz";
	default: return "<unknown>";
	}
}

static void dump_child_device(struct context *context,
			      const struct child_device_config *child)
{
	if (!child->device_type)
		return;

	printf("\tChild device info:\n");
	printf("\t\tDevice handle: 0x%04x (%s)\n", child->handle,
	       child_device_handle(child->handle));
	printf("\t\tDevice type: 0x%04x (%s)\n", child->device_type,
	       child_device_type(child->device_type));
	dump_child_device_type_bits(child->device_type);

	if (context->bdb->version < 152) {
		printf("\t\tSignature: %.*s\n", (int)sizeof(child->device_id), child->device_id);
	} else {
		printf("\t\tI2C speed: %s (0x%02x)\n",
		       i2c_speed(child->i2c_speed), child->i2c_speed);

		if (context->bdb->version >= 158) {
			printf("\t\tDP onboard redriver:\n");
			printf("\t\t\tpresent: %s\n",
			       YESNO((child->dp_onboard_redriver_present)));
			printf("\t\t\tvswing: %s (0x%x)\n",
			       dp_vswing(child->dp_onboard_redriver_vswing),
			       child->dp_onboard_redriver_vswing);
			printf("\t\t\tpre-emphasis: %s (0x%x)\n",
			       dp_preemph(child->dp_onboard_redriver_preemph),
			       child->dp_onboard_redriver_preemph);

			printf("\t\tDP ondock redriver:\n");
			printf("\t\t\tpresent: %s\n",
			       YESNO((child->dp_ondock_redriver_present)));
			printf("\t\t\tvswing: %s (0x%x)\n",
			       dp_vswing(child->dp_ondock_redriver_vswing),
			       child->dp_ondock_redriver_vswing);
			printf("\t\t\tpre-emphasis: %s (0x%x)\n",
			       dp_preemph(child->dp_ondock_redriver_preemph),
			       child->dp_ondock_redriver_preemph);
		}

		if (context->bdb->version >= 204)
			dump_hmdi_max_data_rate(child->hdmi_max_data_rate);
		if (context->bdb->version >= 169)
			printf("\t\tHDMI level shifter value: 0x%02x\n", child->hdmi_level_shifter_value);

		if (context->bdb->version >= 161)
			printf("\t\tOffset to DTD buffer for edidless CHILD: 0x%02x\n", child->dtd_buf_ptr);

		if (context->bdb->version >= 251)
			printf("\t\tDisable compression for external DP/HDMI: %s\n",
			       YESNO(child->disable_compression_for_ext_disp));
		if (context->bdb->version >= 235)
			printf("\t\tLTTPR Mode: %stransparent\n",
			       child->lttpr_non_transparent ? "non-" : "");
		if (context->bdb->version >= 202)
			printf("\t\tDual pipe ganged eDP: %s\n", YESNO(child->ganged_edp));
		if (context->bdb->version >= 198) {
			printf("\t\tCompression method CPS: %s\n", YESNO(child->compression_method_cps));
			printf("\t\tCompression enable: %s\n", YESNO(child->compression_enable));
		}
		if (context->bdb->version >= 161)
			printf("\t\tEdidless EFP: %s\n", YESNO(child->edidless_efp));

		if (context->bdb->version >= 198)
			printf("\t\tCompression structure index: %d\n", child->compression_structure_index);

		if (context->bdb->version >= 237) {
			printf("\t\tHDMI Max FRL rate valid: %s\n",
			       YESNO(child->hdmi_max_frl_rate_valid));
			printf("\t\tHDMI Max FRL rate: %s (0x%x)\n",
			       hdmi_frl_rate(child->hdmi_max_frl_rate),
			       child->hdmi_max_frl_rate);
		}
	}

	printf("\t\tAIM offset: %d\n", child->addin_offset);
	printf("\t\tDVO Port: %s (0x%02x)\n",
	       dvo_port(child->dvo_port), child->dvo_port);

	printf("\t\tAIM I2C pin: 0x%02x\n", child->i2c_pin);
	printf("\t\tAIM Slave address: 0x%02x\n", child->slave_addr);
	printf("\t\tDDC pin: 0x%02x\n", child->ddc_pin);
	printf("\t\tEDID buffer ptr: 0x%02x\n", child->edid_ptr);
	printf("\t\tDVO config: 0x%02x\n", child->dvo_cfg);

	if (context->bdb->version < 155) {
		printf("\t\tDVO2 Port: 0x%02x (%s)\n", child->dvo2_port, dvo_port(child->dvo2_port));
		printf("\t\tI2C2 pin: 0x%02x\n", child->i2c2_pin);
		printf("\t\tSlave2 address: 0x%02x\n", child->slave2_addr);
		printf("\t\tDDC2 pin: 0x%02x\n", child->ddc2_pin);
	} else {
		if (context->bdb->version >= 244)
			printf("\t\teDP/DP max lane count: X%d\n", child->dp_max_lane_count + 1);
		if (context->bdb->version >= 218)
			printf("\t\tUse VBT vswing/premph table: %s\n", YESNO(child->use_vbt_vswing));
		if (context->bdb->version >= 196) {
			printf("\t\tHPD sense invert: %s\n", YESNO(child->hpd_invert));
			printf("\t\tIboost enable: %s\n", YESNO(child->iboost));
		}
		if (context->bdb->version >= 192)
			printf("\t\tOnboard LSPCON: %s\n", YESNO(child->lspcon));
		if (context->bdb->version >= 184)
			printf("\t\tLane reversal: %s\n", YESNO(child->lane_reversal));
		if (context->bdb->version >= 158)
			printf("\t\tEFP routed through dock: %s\n", YESNO(child->efp_routed));

		if (context->bdb->version >= 158) {
			printf("\t\tTMDS compatible? %s\n", YESNO(child->tmds_support));
			printf("\t\tDP compatible? %s\n", YESNO(child->dp_support));
			printf("\t\tHDMI compatible? %s\n", YESNO(child->hdmi_support));
		}

		printf("\t\tAux channel: %s (0x%02x)\n",
		       aux_ch(child->aux_channel), child->aux_channel);

		printf("\t\tDongle detect: 0x%02x\n", child->dongle_detect);
	}

	printf("\t\tIntegrated encoder instead of SDVO: %s\n", YESNO(child->integrated_encoder));
	printf("\t\tHotplug connect status: 0x%02x\n", child->hpd_status);
	printf("\t\tSDVO stall signal available: %s\n", YESNO(child->sdvo_stall));
	printf("\t\tPipe capabilities: 0x%02x\n", child->pipe_cap);

	printf("\t\tDVO wiring: 0x%02x\n", child->dvo_wiring);

	if (context->bdb->version < 171) {
		printf("\t\tDVO2 wiring: 0x%02x\n", child->dvo2_wiring);
	} else {
		printf("\t\tMIPI bridge type: %02x (%s)\n", child->mipi_bridge_type,
		       mipi_bridge_type(child->mipi_bridge_type));
	}

	printf("\t\tDevice class extension: 0x%02x\n", child->extended_type);

	printf("\t\tDVO function: 0x%02x\n", child->dvo_function);

	if (context->bdb->version >= 209) {
		printf("\t\tDP port trace length: 0x%x\n", child->dp_port_trace_length);
		printf("\t\tThunderbolt port: %s\n", YESNO(child->tbt));
	}
	if (context->bdb->version >= 195)
		printf("\t\tDP USB type C support: %s\n", YESNO(child->dp_usb_type_c));

	if (context->bdb->version >= 195) {
		printf("\t\t2X DP GPIO index: 0x%02x\n", child->dp_gpio_index);
		printf("\t\t2X DP GPIO pin number: 0x%02x\n", child->dp_gpio_pin_num);
	}

	if (context->bdb->version >= 196) {
		printf("\t\tIBoost level for DP/eDP: 0x%02x\n", child->dp_iboost_level);
		printf("\t\tIBoost level for HDMI: 0x%02x\n", child->hdmi_iboost_level);
	}

	if (context->bdb->version >= 216)
		dump_dp_max_link_rate(context->bdb->version,
				      child->dp_max_link_rate);
}

static void dump_child_devices(struct context *context, const uint8_t *devices,
			       uint8_t child_dev_num, uint8_t child_dev_size)
{
	struct child_device_config *child;
	int i;

	/*
	 * Use a temp buffer so dump_child_device() doesn't have to worry about
	 * accessing the struct beyond child_dev_size. The tail, if any, remains
	 * initialized to zero.
	 */
	child = calloc(1, sizeof(*child));
	igt_assert(child);

	for (i = 0; i < child_dev_num; i++) {
		memcpy(child, devices + i * child_dev_size,
		       min_t(child_dev_size, sizeof(*child), child_dev_size));

		dump_child_device(context, child);
	}

	free(child);
}

static void dump_general_definitions(struct context *context,
				     const struct bdb_block *block)
{
	const struct bdb_general_definitions *defs = block_data(block);
	int child_dev_num;

	child_dev_num = (block->size - sizeof(*defs)) / defs->child_dev_size;

	printf("\tCRT DDC GMBUS addr: 0x%02x\n", defs->crt_ddc_gmbus_pin);
	printf("\tUse DPMS on AIM devices: %s\n", YESNO(defs->dpms_aim));
	printf("\tSkip CRT detect at boot: %s\n",
	       YESNO(defs->skip_boot_crt_detect));
	printf("\tUse Non ACPI DPMS CRT power states: %s\n",
	       YESNO(defs->dpms_non_acpi));
	printf("\tBoot display type: 0x%02x%02x\n", defs->boot_display[1],
	       defs->boot_display[0]);
	printf("\tChild device size: %d\n", defs->child_dev_size);
	printf("\tChild device count: %d\n", child_dev_num);

	dump_child_devices(context, defs->devices,
			   child_dev_num, defs->child_dev_size);
}

static void dump_legacy_child_devices(struct context *context,
				      const struct bdb_block *block)
{
	const struct bdb_legacy_child_devices *defs = block_data(block);
	int child_dev_num;

	child_dev_num = (block->size - sizeof(*defs)) / defs->child_dev_size;

	printf("\tChild device size: %d\n", defs->child_dev_size);
	printf("\tChild device count: %d\n", child_dev_num);

	dump_child_devices(context, defs->devices,
			   child_dev_num, defs->child_dev_size);
}

static const char * const channel_type[] = {
	[0] = "automatic",
	[1] = "single",
	[2] = "dual",
	[3] = "reserved",
};

static const char * const dps_type[] = {
	[0] = "static DRRS",
	[1] = "D2PO",
	[2] = "seamless DRRS",
	[3] = "reserved",
};

static const char * const blt_type[] = {
	[0] = "default",
	[1] = "CCFL",
	[2] = "LED",
	[3] = "reserved",
};

static const char * const pos_type[] = {
	[0] = "inside shell",
	[1] = "outside shell",
	[2] = "reserved",
	[3] = "reserved",
};

static void dump_lvds_options(struct context *context,
			      const struct bdb_block *block)
{
	const struct bdb_lvds_options *options = block_data(block);

	printf("\tPanel type: %d\n", options->panel_type);
	if (context->bdb->version >= 212)
		printf("\tPanel type 2: %d\n", options->panel_type2);
	printf("\tLVDS EDID available: %s\n", YESNO(options->lvds_edid));
	printf("\tPixel dither: %s\n", YESNO(options->pixel_dither));
	printf("\tPFIT auto ratio: %s\n", YESNO(options->pfit_ratio_auto));
	printf("\tPFIT enhanced graphics mode: %s\n",
	       YESNO(options->pfit_gfx_mode_enhanced));
	printf("\tPFIT enhanced text mode: %s\n",
	       YESNO(options->pfit_text_mode_enhanced));
	printf("\tPFIT mode: %d\n", options->pfit_mode);

	if (block->size < 14)
		return;

	for (int i = 0; i < 16; i++) {
		unsigned int val;

		if (i != context->panel_type && !context->dump_all_panel_types)
			continue;

		printf("\tPanel %d%s\n", i, context->panel_type == i ? " *" : "");

		val = panel_bits(options->lvds_panel_channel_bits, i, 2);
		printf("\t\tChannel type: %s (0x%x)\n",
		       channel_type[val], val);

		printf("\t\tSSC: %s\n",
		       YESNO(panel_bool(options->ssc_bits, i)));

		val = panel_bool(options->ssc_freq, i);
		printf("\t\tSSC frequency: %d MHz (0x%x)\n",
		       decode_ssc_freq(context, val), val);

		printf("\t\tDisable SSC in dual display twin: %s\n",
		       YESNO(panel_bool(options->ssc_ddt, i)));

		if (block->size < 16)
			continue;

		val = panel_bool(options->panel_color_depth, i);
		printf("\t\tPanel color depth: %d (0x%x)\n",
		       val ? 24 : 18, val);

		if (block->size < 24)
			continue;

		val = panel_bits(options->dps_panel_type_bits, i, 2);
		printf("\t\tDPS type: %s (0x%x)\n",
		       dps_type[val], val);

		val = panel_bits(options->blt_control_type_bits, i, 2);
		printf("\t\tBacklight type: %s (0x%x)\n",
		       blt_type[val], val);

		if (context->bdb->version < 200)
			continue;

		printf("\t\tLCDVCC on during S0 state: %s\n",
		       YESNO(panel_bool(options->lcdvcc_s0_enable, i)));

		if (context->bdb->version < 228)
			continue;

		val = panel_bits((options->rotation), i, 2);
		printf("\t\tPanel rotation: %d degrees (0x%x)\n",
		       val * 90, val);

		if (context->bdb->version < 240)
			continue;

		val = panel_bits((options->position), i, 2);
		printf("\t\tPanel position: %s (0x%x)\n",
		       pos_type[val], val);
	}
}

static void dump_lvds_ptr_data(struct context *context,
			       const struct bdb_block *block)
{
	const struct bdb_lvds_lfp_data_ptrs *ptrs = block_data(block);

	printf("\tNumber of entries: %d\n", ptrs->lvds_entries);

	for (int i = 0; i < 16; i++) {
		if (i != context->panel_type && !context->dump_all_panel_types)
			continue;

		printf("\tPanel %d%s\n", i, context->panel_type == i ? " *" : "");

		if (ptrs->lvds_entries >= 1) {
			printf("\t\tFP timing offset: %d\n",
			       ptrs->ptr[i].fp_timing.offset);
			printf("\t\tFP timing table size: %d\n",
			       ptrs->ptr[i].fp_timing.table_size);
		}
		if (ptrs->lvds_entries >= 2) {
			printf("\t\tDVO timing offset: %d\n",
			       ptrs->ptr[i].dvo_timing.offset);
			printf("\t\tDVO timing table size: %d\n",
			       ptrs->ptr[i].dvo_timing.table_size);
		}
		if (ptrs->lvds_entries >= 3) {
			printf("\t\tPanel PnP ID offset: %d\n",
			       ptrs->ptr[i].panel_pnp_id.offset);
			printf("\t\tPanel PnP ID table size: %d\n",
			       ptrs->ptr[i].panel_pnp_id.table_size);
		}
	}

	if (ptrs->panel_name.table_size) {
		printf("\tPanel name offset: %d\n",
		       ptrs->panel_name.offset);
		printf("\tPanel name table size: %d\n",
		       ptrs->panel_name.table_size);
	}
}

static char *decode_pnp_id(u16 mfg_name, char str[4])
{
	mfg_name = ntohs(mfg_name);

	str[0] = '@' + ((mfg_name >> 10) & 0x1f);
	str[1] = '@' + ((mfg_name >> 5) & 0x1f);
	str[2] = '@' + ((mfg_name >> 0) & 0x1f);
	str[3] = '\0';

	return str;
}

static void dump_lvds_data(struct context *context,
			   const struct bdb_block *block)
{
	struct bdb_block *ptrs_block;
	const struct bdb_lvds_lfp_data_ptrs *ptrs;
	int i;
	int hdisplay, hsyncstart, hsyncend, htotal;
	int vdisplay, vsyncstart, vsyncend, vtotal;
	float clock;

	ptrs_block = find_section(context, BDB_LVDS_LFP_DATA_PTRS);
	if (!ptrs_block)
		return;

	ptrs = block_data(ptrs_block);

	for (i = 0; i < 16; i++) {
		const struct lvds_fp_timing *fp_timing =
			block_data(block) + ptrs->ptr[i].fp_timing.offset;
		const uint8_t *timing_data =
			block_data(block) + ptrs->ptr[i].dvo_timing.offset;
		const struct lvds_pnp_id *pnp_id =
			block_data(block) + ptrs->ptr[i].panel_pnp_id.offset;
		const struct bdb_lvds_lfp_data_tail *tail =
			block_data(block) + ptrs->panel_name.offset;
		char mfg[4];

		if (i != context->panel_type && !context->dump_all_panel_types)
			continue;

		hdisplay = _H_ACTIVE(timing_data);
		hsyncstart = hdisplay + _H_SYNC_OFF(timing_data);
		hsyncend = hsyncstart + _H_SYNC_WIDTH(timing_data);
		htotal = hdisplay + _H_BLANK(timing_data);

		vdisplay = _V_ACTIVE(timing_data);
		vsyncstart = vdisplay + _V_SYNC_OFF(timing_data);
		vsyncend = vsyncstart + _V_SYNC_WIDTH(timing_data);
		vtotal = vdisplay + _V_BLANK(timing_data);
		clock = _PIXEL_CLOCK(timing_data) / 1000;

		printf("\tPanel %d%s\n", i, context->panel_type == i ? " *" : "");
		printf("\t\t%dx%d clock %d\n",
		       fp_timing->x_res, fp_timing->y_res,
		       _PIXEL_CLOCK(timing_data));
		printf("\t\tinfo:\n");
		printf("\t\t  LVDS: 0x%08lx\n",
		       (unsigned long)fp_timing->lvds_reg_val);
		printf("\t\t  PP_ON_DELAYS: 0x%08lx\n",
		       (unsigned long)fp_timing->pp_on_reg_val);
		printf("\t\t  PP_OFF_DELAYS: 0x%08lx\n",
		       (unsigned long)fp_timing->pp_off_reg_val);
		printf("\t\t  PP_DIVISOR: 0x%08lx\n",
		       (unsigned long)fp_timing->pp_cycle_reg_val);
		printf("\t\t  PFIT: 0x%08lx\n",
		       (unsigned long)fp_timing->pfit_reg_val);
		printf("\t\ttimings: %d %d %d %d %d %d %d %d %.2f (%s)\n",
		       hdisplay, hsyncstart, hsyncend, htotal,
		       vdisplay, vsyncstart, vsyncend, vtotal, clock,
		       (hsyncend > htotal || vsyncend > vtotal) ?
		       "BAD!" : "good");

		printf("\t\tPnP ID:\n");
		printf("\t\t  Mfg name: %s (0x%x)\n",
		       decode_pnp_id(pnp_id->mfg_name, mfg), pnp_id->mfg_name);
		printf("\t\t  Product code: %u\n", pnp_id->product_code);
		printf("\t\t  Serial: %u\n", pnp_id->serial);
		printf("\t\t  Mfg week: %d\n", pnp_id->mfg_week);
		printf("\t\t  Mfg year: %d\n", 1990 + pnp_id->mfg_year);

		if (!ptrs->panel_name.table_size)
			continue;

		printf("\t\tPanel name: %.*s\n",
		       (int)sizeof(tail->panel_name[0].name), tail->panel_name[i].name);

		if (context->bdb->version < 187)
			continue;

		printf("\t\tScaling enable: %s\n",
		       YESNO(panel_bool(tail->scaling_enable, i)));

		if (context->bdb->version < 188)
			continue;

		printf("\t\tSeamless DRRS min refresh rate: %d\n",
		       tail->seamless_drrs_min_refresh_rate[i]);

		if (context->bdb->version < 208)
			continue;

		printf("\t\tPixel overlap count: %d\n",
		       tail->pixel_overlap_count[i]);

		if (context->bdb->version < 227)
			continue;

		printf("\t\tBlack border:\n");
		printf("\t\t  Top: %d\n", tail->black_border[i].top);
		printf("\t\t  Bottom: %d\n", tail->black_border[i].top);
		printf("\t\t  Left: %d\n", tail->black_border[i].left);
		printf("\t\t  Right: %d\n", tail->black_border[i].right);

		if (context->bdb->version < 231)
			continue;

		printf("\t\tDual LFP port sync enable: %s\n",
		       YESNO(panel_bool(tail->dual_lfp_port_sync_enable, i)));

		if (context->bdb->version < 245)
			continue;

		printf("\t\tGPU dithering for banding artifacts: %s\n",
		       YESNO(panel_bool(tail->gpu_dithering_for_banding_artifacts, i)));
	}

	free(ptrs_block);
}

static const char * const lvds_config[] = {
	[BDB_DRIVER_NO_LVDS] = "No LVDS",
	[BDB_DRIVER_INT_LVDS] = "Integrated LVDS",
	[BDB_DRIVER_SDVO_LVDS] = "SDVO LVDS",
	[BDB_DRIVER_EDP] = "Embedded DisplayPort",
};

static void dump_driver_feature(struct context *context,
				const struct bdb_block *block)
{
	const struct bdb_driver_features *feature = block_data(block);

	printf("\tUse 00000110h ID for Primary LFP: %s\n",
	       YESNO(feature->primary_lfp_id));
	printf("\tEnable Sprite in Clone Mode: %s\n",
	       YESNO(feature->sprite_in_clone));
	printf("\tDriver INT 15h hook: %s\n",
	       YESNO(feature->int15h_hook));
	printf("\tDual View Zoom: %s\n",
	       YESNO(feature->dual_view_zoom));
	printf("\tHot Plug DVO: %s\n",
	       YESNO(feature->hotplug_dvo));
	printf("\tAllow display switching when in Full Screen DOS: %s\n",
	       YESNO(feature->allow_display_switch_dos));
	printf("\tAllow display switching when DVD active: %s\n",
	       YESNO(feature->allow_display_switch_dvd));
	printf("\tBoot Device Algorithm: %s\n",
	       feature->boot_dev_algorithm ? "driver default" : "os default");

	printf("\tBoot Mode X: %u\n", feature->boot_mode_x);
	printf("\tBoot Mode Y: %u\n", feature->boot_mode_y);
	printf("\tBoot Mode Bpp: %u\n", feature->boot_mode_bpp);
	printf("\tBoot Mode Refresh: %u\n", feature->boot_mode_refresh);

	printf("\tEnable LFP as primary: %s\n",
	       YESNO(feature->enable_lfp_primary));
	printf("\tSelective Mode Pruning: %s\n",
	       YESNO(feature->selective_mode_pruning));
	printf("\tDual-Frequency Graphics Technology: %s\n",
	       YESNO(feature->dual_frequency));
	printf("\tDefault Render Clock Frequency: %s\n",
	       feature->render_clock_freq ? "low" : "high");
	printf("\tNT 4.0 Dual Display Clone Support: %s\n",
	       YESNO(feature->nt_clone_support));
	printf("\tDefault Power Scheme user interface: %s\n",
	       feature->power_scheme_ui ? "3rd party" : "CUI");
	printf("\tSprite Display Assignment when Overlay is Active in Clone Mode: %s\n",
	       feature->sprite_display_assign ? "primary" : "secondary");
	printf("\tDisplay Maintain Aspect Scaling via CUI: %s\n",
	       YESNO(feature->cui_aspect_scaling));
	printf("\tPreserve Aspect Ratio: %s\n",
	       YESNO(feature->preserve_aspect_ratio));
	printf("\tEnable SDVO device power down: %s\n",
	       YESNO(feature->sdvo_device_power_down));
	printf("\tCRT hotplug: %s\n", YESNO(feature->crt_hotplug));

	printf("\tLVDS config: %s (0x%x)\n",
	       lvds_config[feature->lvds_config], feature->lvds_config);
	printf("\tTV hotplug: %s\n",
	       YESNO(feature->tv_hotplug));

	printf("\tDisplay subsystem enable: %s\n",
	       YESNO(feature->display_subsystem_enable));
	printf("\tEmbedded platform: %s\n",
	       YESNO(feature->embedded_platform));
	printf("\tDefine Display statically: %s\n",
	       YESNO(feature->static_display));

	printf("\tLegacy CRT max X: %d\n", feature->legacy_crt_max_x);
	printf("\tLegacy CRT max Y: %d\n", feature->legacy_crt_max_y);
	printf("\tLegacy CRT max refresh: %d\n",
	       feature->legacy_crt_max_refresh);

	printf("\tInternal source termination for HDMI: %s\n",
	       YESNO(feature->hdmi_termination));
	printf("\tCEA 861-D HDMI support: %s\n",
	       YESNO(feature->cea861d_hdmi_support));
	printf("\tSelf refresh enable: %s\n",
	       YESNO(feature->self_refresh_enable));

	printf("\tCustom VBT number: 0x%x\n", feature->custom_vbt_version);

	printf("\tPC Features field validity: %s\n",
	       YESNO(feature->pc_feature_valid));
	printf("\tDynamic Media Refresh Rate Switching (DMRRS): %s\n",
	       YESNO(feature->dmrrs_enabled));
	printf("\tIntermediate Pixel Storage (IPS): %s\n",
	       YESNO(feature->ips_enabled));
	printf("\tPanel Self Refresh (PSR): %s\n",
	       YESNO(feature->psr_enabled));
	printf("\tTurbo Boost Technology: %s\n",
	       YESNO(feature->tbt_enabled));
	printf("\tGraphics Power Management (GPMT): %s\n",
	       YESNO(feature->gpmt_enabled));
	printf("\tGraphics Render Standby (RS): %s\n",
	       YESNO(feature->grs_enabled));
	printf("\tDynamic Refresh Rate Switching (DRRS): %s\n",
	       YESNO(feature->drrs_enabled));
	printf("\tAutomatic Display Brightness (ADB): %s\n",
	       YESNO(feature->adb_enabled));
	printf("\tDxgkDDI Backlight Control (DxgkDdiBLC): %s\n",
	       YESNO(feature->bltclt_enabled));
	printf("\tDisplay Power Saving Technology (DPST): %s\n",
	       YESNO(feature->dpst_enabled));
	printf("\tSmart 2D Display Technology (S2DDT): %s\n",
	       YESNO(feature->s2ddt_enabled));
	printf("\tRapid Memory Power Management (RMPM): %s\n",
	       YESNO(feature->rmpm_enabled));
}

static void dump_edp(struct context *context,
		     const struct bdb_block *block)
{
	const struct bdb_edp *edp = block_data(block);
	int bpp, msa;
	int i;

	for (i = 0; i < 16; i++) {
		if (i != context->panel_type && !context->dump_all_panel_types)
			continue;

		printf("\tPanel %d%s\n", i, context->panel_type == i ? " *" : "");

		printf("\t\tPower Sequence: T3 %d T7 %d T9 %d T10 %d T12 %d\n",
		       edp->power_seqs[i].t3,
		       edp->power_seqs[i].t7,
		       edp->power_seqs[i].t9,
		       edp->power_seqs[i].t10,
		       edp->power_seqs[i].t12);

		bpp = panel_bits(edp->color_depth, i, 2);

		printf("\t\tPanel color depth: ");
		switch (bpp) {
		case EDP_18BPP:
			printf("18 bpp\n");
			break;
		case EDP_24BPP:
			printf("24 bpp\n");
			break;
		case EDP_30BPP:
			printf("30 bpp\n");
			break;
		default:
			printf("(unknown value %d)\n", bpp);
			break;
		}

		msa = panel_bits(edp->sdrrs_msa_timing_delay, i, 2);
		printf("\t\teDP sDRRS MSA Delay: Lane %d\n", msa + 1);

		printf("\t\tFast link params:\n");
		printf("\t\t\trate: ");
		switch (edp->fast_link_params[i].rate) {
		case EDP_RATE_1_62:
			printf("1.62Gbps\n");
			break;
		case EDP_RATE_2_7:
			printf("2.7Gbpc\n");
			break;
		case EDP_RATE_5_4:
			printf("5.4Gbps\n");
			break;
		default:
			printf("(unknonn value %d)\n",
			       edp->fast_link_params[i].rate);
			break;
		}
		printf("\t\t\tlanes: X%d",
		       edp->fast_link_params[i].lanes + 1);
		printf("\t\t\tpre-emphasis: %s (0x%x)\n",
		       dp_preemph(edp->fast_link_params[i].preemphasis),
		       edp->fast_link_params[i].preemphasis);
		printf("\t\t\tvswing: %s (0x%x)\n",
		       dp_vswing(edp->fast_link_params[i].vswing),
		       edp->fast_link_params[i].vswing);

		if (context->bdb->version >= 162)
			printf("\t\tStereo 3D feature: %s\n",
			       YESNO(panel_bool(edp->edp_s3d_feature, i)));

		if (context->bdb->version >= 165)
			printf("\t\tT3 optimization: %s\n",
			       YESNO(panel_bool(edp->edp_t3_optimization, i)));

		if (context->bdb->version >= 173) {
			int val = (edp->edp_vswing_preemph >> (i * 4)) & 0xf;

			printf("\t\tVswing/preemphasis table selection: ");
			switch (val) {
			case 0:
				printf("Low power (200 mV)\n");
				break;
			case 1:
				printf("Default (400 mV)\n");
				break;
			default:
				printf("(unknown value %d)\n", val);
				break;
			}
		}

		if (context->bdb->version >= 182)
			printf("\t\tFast link training: %s\n",
			       YESNO(panel_bool(edp->fast_link_training, i)));

		if (context->bdb->version >= 185)
			printf("\t\tDPCD 600h write required: %s\n",
			       YESNO(panel_bool(edp->dpcd_600h_write_required, i)));

		if (context->bdb->version >= 186)
			printf("\t\tPWM delays:\n"
			       "\t\t\tPWM on to backlight enable: %d\n"
			       "\t\t\tBacklight disable to PWM off: %d\n",
			       edp->pwm_delays[i].pwm_on_to_backlight_enable,
			       edp->pwm_delays[i].backlight_disable_to_pwm_off);

		if (context->bdb->version >= 199) {
			printf("\t\tFull link params provided: %s\n",
			       YESNO(panel_bool(edp->full_link_params_provided, i)));

			printf("\t\tFull link params:\n");
			printf("\t\t\tpre-emphasis: %s (0x%x)\n",
			       dp_preemph(edp->full_link_params[i].preemphasis),
			       edp->full_link_params[i].preemphasis);
			printf("\t\t\tvswing: %s (0x%x)\n",
			       dp_vswing(edp->full_link_params[i].vswing),
			       edp->full_link_params[i].vswing);
		}

		if (context->bdb->version >= 224) {
			u16 rate = edp->edp_fast_link_training_rate[i];

			printf("\t\teDP fast link training data rate: %g Gbps (0x%02x)\n",
			       rate / 5000.0f, rate);
		}

		if (context->bdb->version >= 244) {
			u16 rate = edp->edp_max_port_link_rate[i];

			printf("\t\teDP max port link rate: %g Gbps (0x%02x)\n",
			       rate / 5000.0f, rate);
		}
	}
}

static void dump_psr(struct context *context,
		     const struct bdb_block *block)
{
	const struct bdb_psr *psr_block = block_data(block);
	int i;
	uint32_t psr2_tp_time;

	/* The same block ID was used for something else before? */
	if (context->bdb->version < 165)
		return;

	psr2_tp_time = psr_block->psr2_tp2_tp3_wakeup_time;
	for (i = 0; i < 16; i++) {
		const struct psr_table *psr = &psr_block->psr_table[i];

		if (i != context->panel_type && !context->dump_all_panel_types)
			continue;

		printf("\tPanel %d%s\n", i, context->panel_type == i ? " *" : "");

		printf("\t\tFull link: %s\n", YESNO(psr->full_link));
		printf("\t\tRequire AUX to wakeup: %s\n", YESNO(psr->require_aux_to_wakeup));

		switch (psr->lines_to_wait) {
		case 0:
		case 1:
			printf("\t\tLines to wait before link standby: %d\n",
			       psr->lines_to_wait);
			break;
		case 2:
		case 3:
			printf("\t\tLines to wait before link standby: %d\n",
			       1 << psr->lines_to_wait);
			break;
		default:
			printf("\t\tLines to wait before link standby: (unknown) (0x%x)\n",
			       psr->lines_to_wait);
			break;
		}

		printf("\t\tIdle frames to for PSR enable: %d\n",
		       psr->idle_frames);

		printf("\t\tTP1 wakeup time: %d usec (0x%x)\n",
		       psr->tp1_wakeup_time * 100,
		       psr->tp1_wakeup_time);

		printf("\t\tTP2/TP3 wakeup time: %d usec (0x%x)\n",
		       psr->tp2_tp3_wakeup_time * 100,
		       psr->tp2_tp3_wakeup_time);

		if (context->bdb->version >= 226) {
			int index;
			static const uint16_t psr2_tp_times[] = {500, 100, 2500, 5};

			index = panel_bits(psr2_tp_time, i, 2);

			printf("\t\tPSR2 TP2/TP3 wakeup time: %d usec (0x%x)\n",
			       psr2_tp_times[index], index);
		}
	}
}

static void dump_lfp_power(struct context *context,
			   const struct bdb_block *block)
{
	const struct bdb_lfp_power *lfp_block = block_data(block);
	int i;

	printf("\tALS enable: %s\n",
	       YESNO(lfp_block->features.als_enable));
	printf("\tDisplay LACE support: %s\n",
	       YESNO(lfp_block->features.lace_support));
	printf("\tDefault Display LACE enabled status: %s\n",
	       YESNO(lfp_block->features.lace_enabled_status));
	printf("\tPower conservation preference level: %d\n",
	       lfp_block->features.power_conservation_pref);

	for (i = 0; i < 5; i++) {
		printf("\tALS backlight adjust: %d\n",
		       lfp_block->als[i].backlight_adjust);
		printf("\tALS Lux: %d\n",
		       lfp_block->als[i].lux);
	}

	printf("\tDisplay LACE aggressiveness profile: %d\n",
	       lfp_block->lace_aggressiveness_profile);

	if (context->bdb->version < 228)
		return;

	for (i = 0; i < 16; i++) {
		if (i != context->panel_type && !context->dump_all_panel_types)
			continue;

		printf("\tPanel %d%s\n", i, context->panel_type == i ? " *" : "");

		printf("\t\tDisplay Power Saving Technology (DPST): %s\n",
		       YESNO(panel_bool(lfp_block->dpst, i)));
		printf("\t\tPanel Self Refresh (PSR): %s\n",
		       YESNO(panel_bool(lfp_block->psr, i)));
		printf("\t\tDynamic Refresh Rate Switching (DRRS): %s\n",
		       YESNO(panel_bool(lfp_block->drrs, i)));
		printf("\t\tDisplay LACE support: %s\n",
		       YESNO(panel_bool(lfp_block->lace_support, i)));
		printf("\t\tAssertive Display Technology (ADT): %s\n",
		       YESNO(panel_bool(lfp_block->adt, i)));
		printf("\t\tDynamic Media Refresh Rate Switching (DMRRS): %s\n",
		       YESNO(panel_bool(lfp_block->dmrrs, i)));
		printf("\t\tAutomatic Display Brightness (ADB): %s\n",
		       YESNO(panel_bool(lfp_block->adb, i)));
		printf("\t\tDefault Display LACE enabled: %s\n",
		       YESNO(panel_bool(lfp_block->lace_enabled_status, i)));
		printf("\t\tLACE Aggressiveness: %d\n",
		       lfp_block->aggressiveness[i].lace_aggressiveness);
		printf("\t\tDPST Aggressiveness: %d\n",
		       lfp_block->aggressiveness[i].dpst_aggressiveness);

		if (context->bdb->version < 232)
			continue;

		printf("\t\tEDP 4k/2k HOBL feature: %s\n",
		       YESNO(panel_bool(lfp_block->hobl, i)));

		if (context->bdb->version < 233)
			continue;

		printf("\t\tVariable Refresh Rate (VRR): %s\n",
		       YESNO(panel_bool(lfp_block->vrr_feature_enabled, i)));

		if (context->bdb->version < 247)
			continue;

		printf("\t\tELP: %s\n",
		       YESNO(panel_bool(lfp_block->elp, i)));
		printf("\t\tOPST: %s\n",
		       YESNO(panel_bool(lfp_block->opst, i)));
		printf("\t\tELP Aggressiveness: %d\n",
		       lfp_block->aggressiveness2[i].elp_aggressiveness);
		printf("\t\tOPST Aggrgessiveness: %d\n",
		       lfp_block->aggressiveness2[i].opst_aggressiveness);
	}
}

static void
print_detail_timing_data(const struct lvds_dvo_timing *dvo_timing)
{
	int display, sync_start, sync_end, total;

	display = (dvo_timing->hactive_hi << 8) | dvo_timing->hactive_lo;
	sync_start = display +
		((dvo_timing->hsync_off_hi << 8) | dvo_timing->hsync_off_lo);
	sync_end = sync_start + ((dvo_timing->hsync_pulse_width_hi << 8) |
				 dvo_timing->hsync_pulse_width_lo);
	total = display +
		((dvo_timing->hblank_hi << 8) | dvo_timing->hblank_lo);
	printf("\thdisplay: %d\n", display);
	printf("\thsync [%d, %d] %s\n", sync_start, sync_end,
	       dvo_timing->hsync_positive ? "+sync" : "-sync");
	printf("\thtotal: %d\n", total);

	display = (dvo_timing->vactive_hi << 8) | dvo_timing->vactive_lo;
	sync_start = display + ((dvo_timing->vsync_off_hi << 8) |
				dvo_timing->vsync_off_lo);
	sync_end = sync_start + ((dvo_timing->vsync_pulse_width_hi << 8) |
				 dvo_timing->vsync_pulse_width_lo);
	total = display +
		((dvo_timing->vblank_hi << 8) | dvo_timing->vblank_lo);
	printf("\tvdisplay: %d\n", display);
	printf("\tvsync [%d, %d] %s\n", sync_start, sync_end,
	       dvo_timing->vsync_positive ? "+sync" : "-sync");
	printf("\tvtotal: %d\n", total);

	printf("\tclock: %d\n", dvo_timing->clock * 10);
}

static void dump_sdvo_panel_dtds(struct context *context,
				 const struct bdb_block *block)
{
	const struct lvds_dvo_timing *dvo_timing = block_data(block);
	int n, count;

	count = block->size / sizeof(struct lvds_dvo_timing);
	for (n = 0; n < count; n++) {
		printf("%d:\n", n);
		print_detail_timing_data(dvo_timing++);
	}
}

static void dump_sdvo_lvds_options(struct context *context,
				   const struct bdb_block *block)
{
	const struct bdb_sdvo_lvds_options *options = block_data(block);

	printf("\tbacklight: %d\n", options->panel_backlight);
	printf("\th40 type: %d\n", options->h40_set_panel_type);
	printf("\ttype: %d\n", options->panel_type);
	printf("\tssc_clk_freq: %d\n", options->ssc_clk_freq);
	printf("\tals_low_trip: %d\n", options->als_low_trip);
	printf("\tals_high_trip: %d\n", options->als_high_trip);
	/*
	u8 sclalarcoeff_tab_row_num;
	u8 sclalarcoeff_tab_row_size;
	u8 coefficient[8];
	*/
	printf("\tmisc[0]: %x\n", options->panel_misc_bits_1);
	printf("\tmisc[1]: %x\n", options->panel_misc_bits_2);
	printf("\tmisc[2]: %x\n", options->panel_misc_bits_3);
	printf("\tmisc[3]: %x\n", options->panel_misc_bits_4);
}

static void dump_mipi_config(struct context *context,
			     const struct bdb_block *block)
{
	const struct bdb_mipi_config *start = block_data(block);

	for (int i = 0; i < ARRAY_SIZE(start->config); i++) {
		const struct mipi_config *config =
			&start->config[context->panel_type];
		const struct mipi_pps_data *pps =
			&start->pps[context->panel_type];
		const struct edp_pwm_delays *pwm_delays =
			&start->pwm_delays[context->panel_type];

		if (i != context->panel_type && !context->dump_all_panel_types)
			continue;

		printf("\tPanel %d%s\n", i,
		       context->panel_type == i ? " *" : "");

		printf("\t\tGeneral Param\n");
		printf("\t\t\t BTA disable: %s\n", config->bta ? "Disabled" : "Enabled");
		printf("\t\t\t Panel Rotation: %d degrees\n", config->rotation * 90);

		printf("\t\t\t Video Mode Color Format: ");
		if (config->videomode_color_format == 0)
			printf("Not supported\n");
		else if (config->videomode_color_format == 1)
			printf("RGB565\n");
		else if (config->videomode_color_format == 2)
			printf("RGB666\n");
		else if (config->videomode_color_format == 3)
			printf("RGB666 Loosely Packed\n");
		else if (config->videomode_color_format == 4)
			printf("RGB888\n");
		printf("\t\t\t PPS GPIO Pins: %s \n",
		       config->pwm_blc ? "Using SOC" : "Using PMIC");
		printf("\t\t\t CABC Support: %s\n",
		       config->cabc ? "supported" : "not supported");
		printf("\t\t\t Mode: %s\n",
		       config->cmd_mode ? "COMMAND" : "VIDEO");
		printf("\t\t\t Video transfer mode: %s (0x%x)\n",
		       config->vtm == 1 ? "non-burst with sync pulse" :
		       config->vtm == 2 ? "non-burst with sync events" :
		       config->vtm == 3 ? "burst" : "<unknown>",
		       config->vtm);
		printf("\t\t\t Dithering: %s\n",
		       config->dithering ? "done in Display Controller" : "done in Panel Controller");

		printf("\t\tPort Desc\n");
		printf("\t\t\t Pixel overlap: %d\n", config->pixel_overlap);
		printf("\t\t\t Lane Count: %d\n", config->lane_cnt + 1);
		printf("\t\t\t Dual Link Support: ");
		if (config->dual_link == 0)
			printf("not supported\n");
		else if (config->dual_link == 1)
			printf("Front Back mode\n");
		else
			printf("Pixel Alternative Mode\n");

		printf("\t\tDphy Flags\n");
		printf("\t\t\t Clock Stop: %s\n",
		       config->clk_stop ? "ENABLED" : "DISABLED");
		printf("\t\t\t EOT disabled: %s\n\n",
		       config->eot_disabled ? "EOT not to be sent" : "EOT to be sent");

		printf("\t\tHSTxTimeOut: 0x%x\n", config->hs_tx_timeout);
		printf("\t\tLPRXTimeOut: 0x%x\n", config->lp_rx_timeout);
		printf("\t\tTurnAroundTimeOut: 0x%x\n", config->turn_around_timeout);
		printf("\t\tDeviceResetTimer: 0x%x\n", config->device_reset_timer);
		printf("\t\tMasterinitTimer: 0x%x\n", config->master_init_timer);
		printf("\t\tDBIBandwidthTimer: 0x%x\n", config->dbi_bw_timer);
		printf("\t\tLpByteClkValue: 0x%x\n\n", config->lp_byte_clk_val);

		printf("\t\tDphy Params\n");
		printf("\t\t\tExit to zero Count: 0x%x\n", config->exit_zero_cnt);
		printf("\t\t\tTrail Count: 0x%X\n", config->trail_cnt);
		printf("\t\t\tClk zero count: 0x%x\n", config->clk_zero_cnt);
		printf("\t\t\tPrepare count:0x%x\n\n", config->prepare_cnt);

		printf("\t\tClockLaneSwitchingCount: 0x%x\n", config->clk_lane_switch_cnt);
		printf("\t\tHighToLowSwitchingCount: 0x%x\n\n", config->hl_switch_cnt);

		printf("\t\tTimings based on Dphy spec\n");
		printf("\t\t\tTClkMiss: 0x%x\n", config->tclk_miss);
		printf("\t\t\tTClkPost: 0x%x\n", config->tclk_post);
		printf("\t\t\tTClkPre: 0x%x\n", config->tclk_pre);
		printf("\t\t\tTClkPrepare: 0x%x\n", config->tclk_prepare);
		printf("\t\t\tTClkSettle: 0x%x\n", config->tclk_settle);
		printf("\t\t\tTClkTermEnable: 0x%x\n\n", config->tclk_term_enable);

		printf("\t\tTClkTrail: 0x%x\n", config->tclk_trail);
		printf("\t\tTClkPrepareTClkZero: 0x%x\n", config->tclk_prepare_clkzero);
		printf("\t\tTHSExit: 0x%x\n", config->ths_exit);
		printf("\t\tTHsPrepare: 0x%x\n", config->ths_prepare);
		printf("\t\tTHsPrepareTHsZero: 0x%x\n", config->ths_prepare_hszero);
		printf("\t\tTHSSettle: 0x%x\n", config->ths_settle);
		printf("\t\tTHSSkip: 0x%x\n", config->ths_skip);
		printf("\t\tTHsTrail: 0x%x\n", config->ths_trail);
		printf("\t\tTInit: 0x%x\n", config->tinit);
		printf("\t\tTLPX: 0x%x\n", config->tlpx);

		printf("\t\tMIPI PPS\n");
		printf("\t\t\tPanel power ON delay: %d\n", pps->panel_on_delay);
		printf("\t\t\tPanel power on to Backlight enable delay: %d\n", pps->bl_enable_delay);
		printf("\t\t\tBacklight disable to Panel power OFF delay: %d\n", pps->bl_disable_delay);
		printf("\t\t\tPanel power OFF delay: %d\n", pps->panel_off_delay);
		printf("\t\t\tPanel power cycle delay: %d\n", pps->panel_power_cycle_delay);

		if (context->bdb->version >= 186)
			printf("\t\tMIPI PWM delays:\n"
			       "\t\t\tPWM on to backlight enable: %d\n"
			       "\t\t\tBacklight disable to PWM off: %d\n",
			       pwm_delays->pwm_on_to_backlight_enable,
			       pwm_delays->backlight_disable_to_pwm_off);

		if (context->bdb->version >= 190)
			printf("\t\tMIPI PMIC I2C Bus Number: %d\n",
			       start->pmic_i2c_bus_number[i]);
	}
}

static const uint8_t *mipi_dump_send_packet(const uint8_t *data, uint8_t seq_version)
{
	uint8_t flags, type;
	uint16_t len, i;

	flags = *data++;
	type = *data++;
	len = *((const uint16_t *) data);
	data += 2;

	printf("\t\t\tSend DCS: Port %s, VC %d, %s, Type %02x, Length %u, Data",
	       (flags >> 3) & 1 ? "C" : "A",
	       (flags >> 1) & 3,
	       flags & 1 ? "HS" : "LP",
	       type,
	       len);
	for (i = 0; i < len; i++)
		printf(" %02x", *data++);
	printf("\n");

	return data;
}

static const uint8_t *mipi_dump_delay(const uint8_t *data, uint8_t seq_version)
{
	printf("\t\t\tDelay: %u us\n", *((const uint32_t *)data));

	return data + 4;
}

static const uint8_t *mipi_dump_gpio(const uint8_t *data, uint8_t seq_version)
{
	uint8_t index, number, flags;

	if (seq_version >= 3) {
		index = *data++;
		number = *data++;
		flags = *data++;

		if (seq_version >= 4)
			printf("\t\t\tGPIO index %u, number %u, native %d, set %d (0x%02x)\n",
			       index, number, !(flags & 2), flags & 1, flags);
		else
			printf("\t\t\tGPIO index %u, number %u, set %d (0x%02x)\n",
			       index, number, flags & 1, flags);
	} else {
		index = *data++;
		flags = *data++;

		printf("\t\t\tGPIO index %u, source %d, set %d (0x%02x)\n",
		       index, (flags >> 1) & 3, flags & 1, flags);
	}

	return data;
}

static const uint8_t *mipi_dump_i2c(const uint8_t *data, uint8_t seq_version)
{
	uint8_t flags, index, bus, offset, len, i;
	uint16_t address;

	flags = *data++;
	index = *data++;
	bus = *data++;
	address = *((const uint16_t *) data);
	data += 2;
	offset = *data++;
	len = *data++;

	printf("\t\t\tSend I2C: Flags %02x, Index %02x, Bus %02x, Address %04x, Offset %02x, Length %u, Data",
	       flags, index, bus, address, offset, len);
	for (i = 0; i < len; i++)
		printf(" %02x", *data++);
	printf("\n");

	return data;
}

typedef const uint8_t * (*fn_mipi_elem_dump)(const uint8_t *data, uint8_t seq_version);

static const fn_mipi_elem_dump dump_elem[] = {
	[MIPI_SEQ_ELEM_SEND_PKT] = mipi_dump_send_packet,
	[MIPI_SEQ_ELEM_DELAY] = mipi_dump_delay,
	[MIPI_SEQ_ELEM_GPIO] = mipi_dump_gpio,
	[MIPI_SEQ_ELEM_I2C] = mipi_dump_i2c,
};

static const char * const seq_name[] = {
	[MIPI_SEQ_ASSERT_RESET] = "MIPI_SEQ_ASSERT_RESET",
	[MIPI_SEQ_INIT_OTP] = "MIPI_SEQ_INIT_OTP",
	[MIPI_SEQ_DISPLAY_ON] = "MIPI_SEQ_DISPLAY_ON",
	[MIPI_SEQ_DISPLAY_OFF]  = "MIPI_SEQ_DISPLAY_OFF",
	[MIPI_SEQ_DEASSERT_RESET] = "MIPI_SEQ_DEASSERT_RESET",
	[MIPI_SEQ_BACKLIGHT_ON] = "MIPI_SEQ_BACKLIGHT_ON",
	[MIPI_SEQ_BACKLIGHT_OFF] = "MIPI_SEQ_BACKLIGHT_OFF",
	[MIPI_SEQ_TEAR_ON] = "MIPI_SEQ_TEAR_ON",
	[MIPI_SEQ_TEAR_OFF] = "MIPI_SEQ_TEAR_OFF",
	[MIPI_SEQ_POWER_ON] = "MIPI_SEQ_POWER_ON",
	[MIPI_SEQ_POWER_OFF] = "MIPI_SEQ_POWER_OFF",
};

static const char *sequence_name(enum mipi_seq seq_id)
{
	if (seq_id < ARRAY_SIZE(seq_name) && seq_name[seq_id])
		return seq_name[seq_id];
	else
		return "(unknown)";
}

static const uint8_t *dump_sequence(const uint8_t *data, uint8_t seq_version)
{
	fn_mipi_elem_dump mipi_elem_dump;

	printf("\t\tSequence %u - %s\n", *data, sequence_name(*data));

	/* Skip Sequence Byte. */
	data++;

	/* Skip Size of Sequence. */
	if (seq_version >= 3)
		data += 4;

	while (1) {
		uint8_t operation_byte = *data++;
		uint8_t operation_size = 0;

		if (operation_byte == MIPI_SEQ_ELEM_END)
			break;

		if (operation_byte < ARRAY_SIZE(dump_elem))
			mipi_elem_dump = dump_elem[operation_byte];
		else
			mipi_elem_dump = NULL;

		/* Size of Operation. */
		if (seq_version >= 3)
			operation_size = *data++;

		if (mipi_elem_dump) {
			const uint8_t *next = data + operation_size;

			data = mipi_elem_dump(data, seq_version);

			if (operation_size && next != data)
				printf("Error: Inconsistent operation size: %d\n",
					operation_size);
		} else if (operation_size) {
			/* We have size, skip. */
			data += operation_size;
		} else {
			/* No size, can't skip without parsing. */
			printf("Error: Unsupported MIPI element %u\n",
			       operation_byte);
			return NULL;
		}
	}

	return data;
}

/* Find the sequence block and size for the given panel. */
static const uint8_t *
find_panel_sequence_block(const struct bdb_mipi_sequence *sequence,
			  uint16_t panel_id, uint32_t total, uint32_t *seq_size)
{
	const uint8_t *data = &sequence->data[0];
	uint8_t current_id;
	uint32_t current_size;
	int header_size = sequence->version >= 3 ? 5 : 3;
	int index = 0;
	int i;

	/* skip new block size */
	if (sequence->version >= 3)
		data += 4;

	for (i = 0; i < MAX_MIPI_CONFIGURATIONS && index < total; i++) {
		if (index + header_size > total) {
			fprintf(stderr, "Invalid sequence block (header)\n");
			return NULL;
		}

		current_id = *(data + index);
		if (sequence->version >= 3)
			current_size = *((const uint32_t *)(data + index + 1));
		else
			current_size = *((const uint16_t *)(data + index + 1));

		index += header_size;

		if (index + current_size > total) {
			fprintf(stderr, "Invalid sequence block\n");
			return NULL;
		}

		if (current_id == panel_id) {
			*seq_size = current_size;
			return data + index;
		}

		index += current_size;
	}

	fprintf(stderr, "Sequence block detected but no valid configuration\n");

	return NULL;
}

static int goto_next_sequence(const uint8_t *data, int index, int total)
{
	uint16_t len;

	/* Skip Sequence Byte. */
	for (index = index + 1; index < total; index += len) {
		uint8_t operation_byte = *(data + index);
		index++;

		switch (operation_byte) {
		case MIPI_SEQ_ELEM_END:
			return index;
		case MIPI_SEQ_ELEM_SEND_PKT:
			if (index + 4 > total)
				return 0;

			len = *((const uint16_t *)(data + index + 2)) + 4;
			break;
		case MIPI_SEQ_ELEM_DELAY:
			len = 4;
			break;
		case MIPI_SEQ_ELEM_GPIO:
			len = 2;
			break;
		case MIPI_SEQ_ELEM_I2C:
			if (index + 7 > total)
				return 0;
			len = *(data + index + 6) + 7;
			break;
		default:
			fprintf(stderr, "Unknown operation byte\n");
			return 0;
		}
	}

	return 0;
}

static int goto_next_sequence_v3(const uint8_t *data, int index, int total)
{
	int seq_end;
	uint16_t len;
	uint32_t size_of_sequence;

	/*
	 * Could skip sequence based on Size of Sequence alone, but also do some
	 * checking on the structure.
	 */
	if (total < 5) {
		fprintf(stderr, "Too small sequence size\n");
		return 0;
	}

	/* Skip Sequence Byte. */
	index++;

	/*
	 * Size of Sequence. Excludes the Sequence Byte and the size itself,
	 * includes MIPI_SEQ_ELEM_END byte, excludes the final MIPI_SEQ_END
	 * byte.
	 */
	size_of_sequence = *((const uint32_t *)(data + index));
	index += 4;

	seq_end = index + size_of_sequence;
	if (seq_end > total) {
		fprintf(stderr, "Invalid sequence size\n");
		return 0;
	}

	for (; index < total; index += len) {
		uint8_t operation_byte = *(data + index);
		index++;

		if (operation_byte == MIPI_SEQ_ELEM_END) {
			if (index != seq_end) {
				fprintf(stderr, "Invalid element structure\n");
				return 0;
			}
			return index;
		}

		len = *(data + index);
		index++;

		/*
		 * FIXME: Would be nice to check elements like for v1/v2 in
		 * goto_next_sequence() above.
		 */
		switch (operation_byte) {
		case MIPI_SEQ_ELEM_SEND_PKT:
		case MIPI_SEQ_ELEM_DELAY:
		case MIPI_SEQ_ELEM_GPIO:
		case MIPI_SEQ_ELEM_I2C:
		case MIPI_SEQ_ELEM_SPI:
		case MIPI_SEQ_ELEM_PMIC:
			break;
		default:
			fprintf(stderr, "Unknown operation byte %u\n",
				operation_byte);
			break;
		}
	}

	return 0;
}

static void dump_mipi_sequence(struct context *context,
			       const struct bdb_block *block)
{
	const struct bdb_mipi_sequence *sequence = block_data(block);

	/* Check if we have sequence block as well */
	if (!sequence) {
		printf("No MIPI Sequence found\n");
		return;
	}

	printf("\tSequence block version v%u\n", sequence->version);

	/* Fail gracefully for forward incompatible sequence block. */
	if (sequence->version >= 4) {
		fprintf(stderr, "Unable to parse MIPI Sequence Block v%u\n",
			sequence->version);
		return;
	}

	for (int i = 0; i < MAX_MIPI_CONFIGURATIONS; i++) {
		const uint8_t *sequence_ptrs[MIPI_SEQ_MAX] = {};
		const uint8_t *data;
		uint32_t seq_size;
		int index = 0;

		if (i != context->panel_type && !context->dump_all_panel_types)
			continue;

		data = find_panel_sequence_block(sequence, i,
						 block->size, &seq_size);
		if (!data)
			return;

		printf("\tPanel %d%s\n", i,
		       context->panel_type == i ? " *" : "");

		/* Parse the sequences. Corresponds to VBT parsing in the kernel. */
		for (;;) {
			uint8_t seq_id = *(data + index);
			if (seq_id == MIPI_SEQ_END)
				break;

			if (seq_id >= MIPI_SEQ_MAX) {
				fprintf(stderr, "Unknown sequence %u\n", seq_id);
				return;
			}

			sequence_ptrs[seq_id] = data + index;

			if (sequence->version >= 3)
				index = goto_next_sequence_v3(data, index, seq_size);
			else
				index = goto_next_sequence(data, index, seq_size);
			if (!index) {
				fprintf(stderr, "Invalid sequence %u\n", seq_id);
				return;
			}
		}

		/* Dump the sequences. Corresponds to sequence execution in kernel. */
		for (int j = 0; j < ARRAY_SIZE(sequence_ptrs); j++)
			if (sequence_ptrs[j])
				dump_sequence(sequence_ptrs[j], sequence->version);
	}
}

#define KB(x) ((x) * 1024)

static int dsc_buffer_block_size(u8 buffer_block_size)
{
	switch (buffer_block_size) {
	case VBT_RC_BUFFER_BLOCK_SIZE_1KB:
		return KB(1);
		break;
	case VBT_RC_BUFFER_BLOCK_SIZE_4KB:
		return KB(4);
		break;
	case VBT_RC_BUFFER_BLOCK_SIZE_16KB:
		return KB(16);
		break;
	case VBT_RC_BUFFER_BLOCK_SIZE_64KB:
		return KB(64);
		break;
	default:
		return 0;
	}
}

static int actual_buffer_size(u8 buffer_block_size, u8 rc_buffer_size)
{
	return dsc_buffer_block_size(buffer_block_size) * (rc_buffer_size + 1);
}

static const char *dsc_max_bpp(u8 value)
{
	switch (value) {
	case 0:
		return "6";
	case 1:
		return "8";
	case 2:
		return "10";
	case 3:
		return "12";
	default:
		return "<unknown>";
	}
}

static void dump_compression_parameters(struct context *context,
					const struct bdb_block *block)
{
	const struct bdb_compression_parameters *dsc = block_data(block);
	const struct dsc_compression_parameters_entry *data;
	int i;

	for (i = 0; i < ARRAY_SIZE(dsc->data); i++) {
		/* FIXME: need to handle sizeof(*data) != dsc->entry_size */
		data = &dsc->data[i];

		if (i != context->panel_type && !context->dump_all_panel_types)
			continue;

		printf("\tDSC block %d%s\n", i,
		       i == context->panel_type ? " *" : "");
		printf("\t\tDSC version: %u.%u\n", data->version_major,
		       data->version_minor);
		printf("\t\tActual buffer size: %d\n",
		       actual_buffer_size(data->rc_buffer_block_size,
					  data->rc_buffer_size));
		printf("\t\t\tRC buffer block size: %d (%u)\n",
		       dsc_buffer_block_size(data->rc_buffer_block_size),
		       data->rc_buffer_block_size);
		printf("\t\t\tRC buffer size: %u\n", data->rc_buffer_size);
		printf("\t\tSlices per line: 0x%02x\n", data->slices_per_line);
		printf("\t\tLine buffer depth: %u bits (%u)\n",
		       data->line_buffer_depth + 8, data->line_buffer_depth);
		printf("\t\tBlock prediction enable: %u\n",
		       data->block_prediction_enable);
		printf("\t\tMax bpp: %s bpp (%u)\n", dsc_max_bpp(data->max_bpp),
		       data->max_bpp);
		printf("\t\tSupport 8 bpc: %u\n", data->support_8bpc);
		printf("\t\tSupport 10 bpc: %u\n", data->support_10bpc);
		printf("\t\tSupport 12 bpc: %u\n", data->support_12bpc);
		printf("\t\tSlice height: %u\n", data->slice_height);
	}
}

/* get panel type from lvds options block, or -1 if block not found */
static int get_panel_type(struct context *context)
{
	struct bdb_block *block;
	const struct bdb_lvds_options *options;
	int panel_type;

	block = find_section(context, BDB_LVDS_OPTIONS);
	if (!block)
		return -1;

	options = block_data(block);
	panel_type = options->panel_type;

	free(block);

	return panel_type;
}

static int
get_device_id(unsigned char *bios, int size)
{
    int device;
    int offset = (bios[0x19] << 8) + bios[0x18];

    if (offset + 7 >= size)
	return -1;

    if (bios[offset] != 'P' ||
	bios[offset+1] != 'C' ||
	bios[offset+2] != 'I' ||
	bios[offset+3] != 'R')
	return -1;

    device = (bios[offset+7] << 8) + bios[offset+6];

    return device;
}

struct dumper {
	uint8_t id;
	const char *name;
	void (*dump)(struct context *context,
		     const struct bdb_block *block);
};

struct dumper dumpers[] = {
	{
		.id = BDB_GENERAL_FEATURES,
		.name = "General features block",
		.dump = dump_general_features,
	},
	{
		.id = BDB_GENERAL_DEFINITIONS,
		.name = "General definitions block",
		.dump = dump_general_definitions,
	},
	{
		.id = BDB_CHILD_DEVICE_TABLE,
		.name = "Legacy child devices block",
		.dump = dump_legacy_child_devices,
	},
	{
		.id = BDB_LVDS_OPTIONS,
		.name = "LVDS options block",
		.dump = dump_lvds_options,
	},
	{
		.id = BDB_LVDS_LFP_DATA_PTRS,
		.name = "LVDS timing pointer data",
		.dump = dump_lvds_ptr_data,
	},
	{
		.id = BDB_LVDS_LFP_DATA,
		.name = "LVDS panel data block",
		.dump = dump_lvds_data,
	},
	{
		.id = BDB_LVDS_BACKLIGHT,
		.name = "Backlight info block",
		.dump = dump_backlight_info,
	},
	{
		.id = BDB_LFP_POWER,
		.name = "LFP power conservation features block",
		.dump = dump_lfp_power,
	},
	{
		.id = BDB_SDVO_LVDS_OPTIONS,
		.name = "SDVO LVDS options block",
		.dump = dump_sdvo_lvds_options,
	},
	{
		.id = BDB_SDVO_PANEL_DTDS,
		.name = "SDVO panel dtds",
		.dump = dump_sdvo_panel_dtds,
	},
	{
		.id = BDB_DRIVER_FEATURES,
		.name = "Driver feature data block",
		.dump = dump_driver_feature,
	},
	{
		.id = BDB_EDP,
		.name = "eDP block",
		.dump = dump_edp,
	},
	{
		.id = BDB_PSR,
		.name = "PSR block",
		.dump = dump_psr,
	},
	{
		.id = BDB_MIPI_CONFIG,
		.name = "MIPI configuration block",
		.dump = dump_mipi_config,
	},
	{
		.id = BDB_MIPI_SEQUENCE,
		.name = "MIPI sequence block",
		.dump = dump_mipi_sequence,
	},
	{
		.id = BDB_COMPRESSION_PARAMETERS,
		.name = "Compression parameters block",
		.dump = dump_compression_parameters,
	},
};

static void hex_dump(const void *data, uint32_t size)
{
	int i;
	const uint8_t *p = data;

	for (i = 0; i < size; i++) {
		if (i % 16 == 0)
			printf("\t%04x: ", i);
		printf("%02x", p[i]);
		if (i % 16 == 15) {
			if (i + 1 < size)
				printf("\n");
		} else if (i % 8 == 7) {
			printf("  ");
		} else {
			printf(" ");
		}
	}
	printf("\n\n");
}

static void hex_dump_block(const struct bdb_block *block)
{
	hex_dump(block->data, 3 + block->size);
}

static bool dump_section(struct context *context, int section_id)
{
	struct dumper *dumper = NULL;
	struct bdb_block *block;
	int i;

	block = find_section(context, section_id);
	if (!block)
		return false;

	for (i = 0; i < ARRAY_SIZE(dumpers); i++) {
		if (block->id == dumpers[i].id) {
			dumper = &dumpers[i];
			break;
		}
	}

	if (dumper && dumper->name)
		printf("BDB block %d (%d bytes) - %s:\n", block->id, block->size, dumper->name);
	else
		printf("BDB block %d (%d bytes) - Unknown, no decoding available:\n",
		       block->id, block->size);

	if (context->hexdump)
		hex_dump_block(block);
	if (dumper && dumper->dump)
		dumper->dump(context, block);
	printf("\n");

	free(block);

	return true;
}

/* print a description of the VBT of the form <bdb-version>-<vbt-signature> */
static void print_description(struct context *context)
{
	const struct vbt_header *vbt = context->vbt;
	const struct bdb_header *bdb = context->bdb;
	char *desc = strndup((char *)vbt->signature, sizeof(vbt->signature));
	char *p;

	for (p = desc + strlen(desc) - 1; p >= desc && isspace(*p); p--)
		*p = '\0';

	for (p = desc; *p; p++) {
		if (!isalnum(*p))
			*p = '-';
		else
			*p = tolower(*p);
	}

	p = desc;
	if (strncmp(p, "-vbt-", 5) == 0)
		p += 5;

	printf("%d-%s\n", bdb->version, p);

	free (desc);
}

static void dump_headers(struct context *context)
{
	const struct vbt_header *vbt = context->vbt;
	const struct bdb_header *bdb = context->bdb;
	int i, j = 0;

	printf("VBT header:\n");
	if (context->hexdump)
		hex_dump(vbt, vbt->header_size);

	printf("\tVBT signature:\t\t\"%.*s\"\n",
	       (int)sizeof(vbt->signature), vbt->signature);
	printf("\tVBT version:\t\t0x%04x (%d.%d)\n", vbt->version,
	       vbt->version / 100, vbt->version % 100);
	printf("\tVBT header size:\t0x%04x (%u)\n",
	       vbt->header_size, vbt->header_size);
	printf("\tVBT size:\t\t0x%04x (%u)\n", vbt->vbt_size, vbt->vbt_size);
	printf("\tVBT checksum:\t\t0x%02x\n", vbt->vbt_checksum);
	printf("\tBDB offset:\t\t0x%08x (%u)\n", vbt->bdb_offset, vbt->bdb_offset);

	printf("\n");

	printf("BDB header:\n");
	if (context->hexdump)
		hex_dump(bdb, bdb->header_size);

	printf("\tBDB signature:\t\t\"%.*s\"\n",
	       (int)sizeof(bdb->signature), bdb->signature);
	printf("\tBDB version:\t\t%d\n", bdb->version);
	printf("\tBDB header size:\t0x%04x (%u)\n",
	       bdb->header_size, bdb->header_size);
	printf("\tBDB size:\t\t0x%04x (%u)\n", bdb->bdb_size, bdb->bdb_size);
	printf("\n");

	printf("BDB blocks present:");
	for (i = 0; i < 256; i++) {
		struct bdb_block *block;

		block = find_section(context, i);
		if (!block)
			continue;

		if (j++ % 16)
			printf(" %3d", i);
		else
			printf("\n\t%3d", i);

		free(block);
	}
	printf("\n\n");
}

enum opt {
	OPT_UNKNOWN = '?',
	OPT_END = -1,
	OPT_FILE,
	OPT_DEVID,
	OPT_PANEL_TYPE,
	OPT_ALL_PANELS,
	OPT_HEXDUMP,
	OPT_BLOCK,
	OPT_USAGE,
	OPT_HEADER,
	OPT_DESCRIBE,
};

static void usage(const char *toolname)
{
	fprintf(stderr, "usage: %s", toolname);
	fprintf(stderr, " --file=<rom_file>"
			" [--devid=<device_id>]"
			" [--panel-type=<panel_type>]"
			" [--all-panels]"
			" [--hexdump]"
			" [--block=<block_no>]"
			" [--header]"
			" [--describe]"
			" [--help]\n");
}

int main(int argc, char **argv)
{
	uint8_t *VBIOS;
	int index;
	enum opt opt;
	int fd;
	struct vbt_header *vbt = NULL;
	int vbt_off, bdb_off, i;
	const char *filename = NULL;
	const char *toolname = argv[0];
	struct stat finfo;
	int size;
	struct context context = {
		.panel_type = -1,
	};
	char *endp;
	int block_number = -1;
	bool header_only = false, describe = false;

	static struct option options[] = {
		{ "file",	required_argument,	NULL,	OPT_FILE },
		{ "devid",	required_argument,	NULL,	OPT_DEVID },
		{ "panel-type",	required_argument,	NULL,	OPT_PANEL_TYPE },
		{ "all-panels",	no_argument,		NULL,	OPT_ALL_PANELS },
		{ "hexdump",	no_argument,		NULL,	OPT_HEXDUMP },
		{ "block",	required_argument,	NULL,	OPT_BLOCK },
		{ "header",	no_argument,		NULL,	OPT_HEADER },
		{ "describe",	no_argument,		NULL,	OPT_DESCRIBE },
		{ "help",	no_argument,		NULL,	OPT_USAGE },
		{ 0 }
	};

	for (opt = 0; opt != OPT_END; ) {
		opt = getopt_long(argc, argv, "", options, &index);

		switch (opt) {
		case OPT_FILE:
			filename = optarg;
			break;
		case OPT_DEVID:
			context.devid = strtoul(optarg, &endp, 16);
			if (!context.devid || *endp) {
				fprintf(stderr, "invalid devid '%s'\n", optarg);
				return EXIT_FAILURE;
			}
			break;
		case OPT_PANEL_TYPE:
			context.panel_type = strtoul(optarg, &endp, 0);
			if (*endp || context.panel_type > 15) {
				fprintf(stderr, "invalid panel type '%s'\n",
					optarg);
				return EXIT_FAILURE;
			}
			break;
		case OPT_ALL_PANELS:
			context.dump_all_panel_types = true;
			break;
		case OPT_HEXDUMP:
			context.hexdump = true;
			break;
		case OPT_BLOCK:
			block_number = strtoul(optarg, &endp, 0);
			if (*endp) {
				fprintf(stderr, "invalid block number '%s'\n",
					optarg);
				return EXIT_FAILURE;
			}
			break;
		case OPT_HEADER:
			header_only = true;
			break;
		case OPT_DESCRIBE:
			describe = true;
			break;
		case OPT_END:
			break;
		case OPT_USAGE: /* fall-through */
		case OPT_UNKNOWN:
			usage(toolname);
			return EXIT_FAILURE;
		}
	}

	argc -= optind;
	argv += optind;

	if (!filename) {
		if (argc == 1) {
			/* for backwards compatibility */
			filename = argv[0];
		} else {
			usage(toolname);
			return EXIT_FAILURE;
		}
	}

	fd = open(filename, O_RDONLY);
	if (fd == -1) {
		fprintf(stderr, "Couldn't open \"%s\": %s\n",
			filename, strerror(errno));
		return EXIT_FAILURE;
	}

	if (stat(filename, &finfo)) {
		fprintf(stderr, "Failed to stat \"%s\": %s\n",
			filename, strerror(errno));
		return EXIT_FAILURE;
	}
	size = finfo.st_size;

	if (size == 0) {
		int len = 0, ret;
		size = 8192;
		VBIOS = malloc (size);
		while ((ret = read(fd, VBIOS + len, size - len))) {
			if (ret < 0) {
				fprintf(stderr, "Failed to read \"%s\": %s\n",
					filename, strerror(errno));
				return EXIT_FAILURE;
			}

			len += ret;
			if (len == size) {
				size *= 2;
				VBIOS = realloc(VBIOS, size);
			}
		}
	} else {
		VBIOS = mmap(NULL, size, PROT_READ, MAP_SHARED, fd, 0);
		if (VBIOS == MAP_FAILED) {
			fprintf(stderr, "Failed to map \"%s\": %s\n",
				filename, strerror(errno));
			return EXIT_FAILURE;
		}
	}

	/* Scour memory looking for the VBT signature */
	for (i = 0; i + 4 < size; i++) {
		if (!memcmp(VBIOS + i, "$VBT", 4)) {
			vbt_off = i;
			vbt = (struct vbt_header *)(VBIOS + i);
			break;
		}
	}

	if (!vbt) {
		fprintf(stderr, "VBT signature missing\n");
		return EXIT_FAILURE;
	}

	bdb_off = vbt_off + vbt->bdb_offset;
	if (bdb_off >= size - sizeof(struct bdb_header)) {
		fprintf(stderr, "Invalid VBT found, BDB points beyond end of data block\n");
		return EXIT_FAILURE;
	}

	context.vbt = vbt;
	context.bdb = (const struct bdb_header *)(VBIOS + bdb_off);
	context.size = size;

	if (!context.devid) {
		const char *devid_string = getenv("DEVICE");
		if (devid_string)
			context.devid = strtoul(devid_string, NULL, 16);
	}
	if (!context.devid)
		context.devid = get_device_id(VBIOS, size);
	if (!context.devid)
		fprintf(stderr, "Warning: could not find PCI device ID!\n");

	if (context.panel_type == -1)
		context.panel_type = get_panel_type(&context);
	if (context.panel_type == -1) {
		fprintf(stderr, "Warning: panel type not set, using 0\n");
		context.panel_type = 0;
	}

	if (describe) {
		print_description(&context);
	} else if (header_only) {
		dump_headers(&context);
	} else if (block_number != -1) {
		/* dump specific section only */
		if (!dump_section(&context, block_number)) {
			fprintf(stderr, "Block %d not found\n", block_number);
			return EXIT_FAILURE;
		}
	} else {
		dump_headers(&context);

		/* dump all sections  */
		for (i = 0; i < 256; i++)
			dump_section(&context, i);
	}

	return 0;
}
