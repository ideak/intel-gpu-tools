/*
 * Copyright 2019 Advanced Micro Devices, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE COPYRIGHT HOLDER(S) OR AUTHOR(S) BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

#include <fcntl.h>
#include <sys/stat.h>

#include "igt_amd.h"
#include "igt.h"
#include <amdgpu_drm.h>

#define X0 1
#define X1 2
#define X2 4
#define X3 8
#define X4 16
#define X5 32
#define X6 64
#define X7 128
#define Y0 1
#define Y1 2
#define Y2 4
#define Y3 8
#define Y4 16
#define Y5 32
#define Y6 64
#define Y7 128

struct dim2d
{
    int w;
    int h;
};

uint32_t igt_amd_create_bo(int fd, uint64_t size)
{
	union drm_amdgpu_gem_create create;

	memset(&create, 0, sizeof(create));
	create.in.bo_size = size;
	create.in.alignment = 256;
	create.in.domains = AMDGPU_GEM_DOMAIN_VRAM;
	create.in.domain_flags = AMDGPU_GEM_CREATE_CPU_ACCESS_REQUIRED
				 | AMDGPU_GEM_CREATE_VRAM_CLEARED;

	do_ioctl(fd, DRM_IOCTL_AMDGPU_GEM_CREATE, &create);
	igt_assert(create.out.handle);

	return create.out.handle;
}

void *igt_amd_mmap_bo(int fd, uint32_t handle, uint64_t size, int prot)
{
	union drm_amdgpu_gem_mmap map;
	void *ptr;

	memset(&map, 0, sizeof(map));
	map.in.handle = handle;

	do_ioctl(fd, DRM_IOCTL_AMDGPU_GEM_MMAP, &map);

	ptr = mmap(0, size, prot, MAP_SHARED, fd, map.out.addr_ptr);
	return ptr == MAP_FAILED ? NULL : ptr;
}

unsigned int igt_amd_compute_offset(unsigned int* swizzle_pattern,
				       unsigned int x, unsigned int y)
{
    unsigned int offset = 0, index = 0;
    unsigned int blk_size_table_index = 0, interleave = 0;
    unsigned int channel[16] =
				{0, 0, 1, 1, 2, 2, 2, 1, 2, 1, 2, 1, 2, 1, 2, 1};
    unsigned int i, v;

    for (i = 0; i < 16; i++)
    {
        v = 0;
        if (channel[i] == 1)
        {
            blk_size_table_index = 0;
            interleave = swizzle_pattern[i];

            while (interleave > 1) {
				blk_size_table_index++;
				interleave = (interleave + 1) >> 1;
			}

            index = blk_size_table_index + 2;
            v ^= (x >> index) & 1;
        }
        else if (channel[i] == 2)
        {
            blk_size_table_index = 0;
            interleave = swizzle_pattern[i];

            while (interleave > 1) {
				blk_size_table_index++;
				interleave = (interleave + 1) >> 1;
			}

            index = blk_size_table_index;
            v ^= (y >> index) & 1;
        }

        offset |= (v << i);
    }

	return offset;
}

unsigned int igt_amd_fb_get_blk_size_table_idx(unsigned int bpp)
{
	unsigned int element_bytes;
	unsigned int blk_size_table_index = 0;

	element_bytes = bpp >> 3;

	while (element_bytes > 1) {
		blk_size_table_index++;
		element_bytes = (element_bytes + 1) >> 1;
	}

	return blk_size_table_index;
}

void igt_amd_fb_calculate_tile_dimension(unsigned int bpp,
				       unsigned int *width, unsigned int *height)
{
	unsigned int blk_size_table_index;
	unsigned int blk_size_log2, blk_size_log2_256B;
	unsigned int width_amp, height_amp;

	// swizzle 64kb tile block
	unsigned int block256_2d[][2] = {{16, 16}, {16, 8}, {8, 8}, {8, 4}, {4, 4}};
	blk_size_log2 = 16;

	blk_size_table_index = igt_amd_fb_get_blk_size_table_idx(bpp);

	blk_size_log2_256B = blk_size_log2 - 8;

	width_amp = blk_size_log2_256B / 2;
	height_amp = blk_size_log2_256B - width_amp;

	*width  = (block256_2d[blk_size_table_index][0] << width_amp);
	*height = (block256_2d[blk_size_table_index][1] << height_amp);
}

uint32_t igt_amd_fb_tiled_offset(unsigned int bpp, unsigned int x_input,
				       unsigned int y_input, unsigned int width_input)
{
	unsigned int width, height, pitch;
	unsigned int pb, yb, xb, blk_idx, blk_offset, addr;
	unsigned int blk_size_table_index, blk_size_log2;
	unsigned int* swizzle_pattern;

	// swizzle 64kb tile block
	unsigned int sw_64k_s[][16]=
	{
	    {X0, X1, X2, X3, Y0, Y1, Y2, Y3, Y4, X4, Y5, X5, Y6, X6, Y7, X7},
	    {0,  X0, X1, X2, Y0, Y1, Y2, X3, Y3, X4, Y4, X5, Y5, X6, Y6, X7},
	    {0,  0,  X0, X1, Y0, Y1, Y2, X2, Y3, X3, Y4, X4, Y5, X5, Y6, X6},
	    {0,  0,  0,  X0, Y0, Y1, X1, X2, Y2, X3, Y3, X4, Y4, X5, Y5, X6},
	    {0,  0,  0,  0,  Y0, Y1, X0, X1, Y2, X2, Y3, X3, Y4, X4, Y5, X5},
	};
	igt_amd_fb_calculate_tile_dimension(bpp, &width, &height);
	blk_size_table_index = igt_amd_fb_get_blk_size_table_idx(bpp);
	blk_size_log2 = 16;

	pitch = (width_input + (width - 1)) & (~(width - 1));

	swizzle_pattern = sw_64k_s[blk_size_table_index];

	pb = pitch / width;
	yb = y_input / height;
	xb = x_input / width;
	blk_idx = yb * pb + xb;
	blk_offset = igt_amd_compute_offset(swizzle_pattern,
					x_input << blk_size_table_index, y_input);
	addr = (blk_idx << blk_size_log2) + blk_offset;

    return (uint32_t)addr;
}

void igt_amd_fb_to_tiled(struct igt_fb *dst, void *dst_buf, struct igt_fb *src,
				       void *src_buf, unsigned int plane)
{
	uint32_t src_offset, dst_offset;
	unsigned int bpp = src->plane_bpp[plane];
	unsigned int width = dst->plane_width[plane];
	unsigned int height = dst->plane_height[plane];
	unsigned int x, y;

	for (y = 0; y < height; y++) {
		for (x = 0; x < width; x++) {
			src_offset = src->offsets[plane];
			dst_offset = dst->offsets[plane];

			src_offset += src->strides[plane] * y + x * bpp / 8;
			dst_offset += igt_amd_fb_tiled_offset(bpp, x, y, width);

			switch (bpp) {
			case 16:
				*(uint16_t *)(dst_buf + dst_offset) =
					*(uint16_t *)(src_buf + src_offset);
				break;
			case 32:
				*(uint32_t *)(dst_buf + dst_offset) =
					*(uint32_t *)(src_buf + src_offset);
				break;
			}
		}
	}
}

void igt_amd_fb_convert_plane_to_tiled(struct igt_fb *dst, void *dst_buf,
				       struct igt_fb *src, void *src_buf)
{
	unsigned int plane;

	for (plane = 0; plane < src->num_planes; plane++) {
		igt_require(AMD_FMT_MOD_GET(TILE, dst->modifier) ==
					AMD_FMT_MOD_TILE_GFX9_64K_S);
		igt_amd_fb_to_tiled(dst, dst_buf, src, src_buf, plane);
	}
}

bool igt_amd_is_tiled(uint64_t modifier)
{
	if (IS_AMD_FMT_MOD(modifier) && AMD_FMT_MOD_GET(TILE, modifier))
		return true;
	else
		return false;
}

/**
 * igt_amd_output_has_hpd: check if connector has HPD debugfs entry
 * @drm_fd: DRM file descriptor
 * @connector_name: The connector's name, on which we're reading the status
 */
static bool igt_amd_output_has_hpd(int drm_fd, char *connector_name)
{
	int fd;
	int res;
	struct stat stat;

	fd = igt_debugfs_connector_dir(drm_fd, connector_name, O_RDONLY);
	if (fd < 0) {
		igt_info("output %s: debugfs not found\n", connector_name);
		return false;
	}

	res = fstatat(fd, DEBUGFS_HPD_TRIGGER, &stat, 0);
	if (res != 0) {
		igt_info("%s debugfs not supported\n", DEBUGFS_HPD_TRIGGER);
		close(fd);
		return false;
	}

	close(fd);
	return true;
}

/**
 * igt_amd_require_hpd: Checks if connectors have HPD debugfs
 * @display: A pointer to an #igt_display_t structure
 * @drm_fd: DRM file descriptor
 *
 * Checks if the AMDGPU driver has support the 'trigger_hotplug'
 * entry for HPD. Skip test if HPD is not supported.
 */
void igt_amd_require_hpd(igt_display_t *display, int drm_fd)
{
	igt_output_t *output;

	for_each_connected_output(display, output) {
		if (igt_amd_output_has_hpd(drm_fd, output->name))
			return;
	}

	igt_skip("No HPD debugfs support.\n");
}

/**
 * igt_amd_trigger_hotplut: Triggers a debugfs HPD
 * @drm_fd: DRM file descriptor
 * @connector_name: The connector's name, which we trigger the hotplug on
 *
 * igt_amd_require_hpd should be called before calling this.
 */
int igt_amd_trigger_hotplug(int drm_fd, char *connector_name)
{
	int fd, hpd_fd;
	int wr_len;
	const char *enable_hpd = "1";

	fd = igt_debugfs_connector_dir(drm_fd, connector_name, O_RDONLY);
	igt_assert(fd >= 0);
	hpd_fd = openat(fd, DEBUGFS_HPD_TRIGGER, O_WRONLY);
	close(fd);
	igt_assert(hpd_fd >= 0);

	wr_len = write(hpd_fd, enable_hpd, strlen(enable_hpd));
	close(hpd_fd);
	igt_assert_eq(wr_len, strlen(enable_hpd));

	return 0;
}

/*
 * igt_amd_read_link_settings:
 * @drm_fd: DRM file descriptor
 * @connector_name: The name of the connector to read the link_settings
 * @lane_count: Lane count
 * @link_rate: Link rate
 * @link_spread: Spread spectrum
 *
 * The indices of @lane_count, @link_rate, and @link_spread correspond to the
 * values of "Current", "Verified", "Reported", and "Preferred", respectively.
 */
void igt_amd_read_link_settings(
	int drm_fd, char *connector_name, int *lane_count, int *link_rate, int *link_spread)
{
	int fd, ret;
	char buf[101];
	int i = 0;
	char *token_end, *val_token;

	fd = igt_debugfs_connector_dir(drm_fd, connector_name, O_RDONLY);
	if (fd < 0) {
		igt_info("Could not open connector %s debugfs directory\n",
			 connector_name);
		return;
	}
	ret = igt_debugfs_simple_read(fd, DEBUGFS_DP_LINK_SETTINGS, buf, sizeof(buf));
	igt_assert_f(ret >= 0, "Reading %s for connector %s failed.\n",
		     DEBUGFS_DP_LINK_SETTINGS, connector_name);

	close(fd);

	/* Between current, verified, reported, and preferred are null terminators,
	 * replace them with ';' to use as the delimiter for strtok. */
	while (strlen(buf) < sizeof(buf) - 1 && buf[strlen(buf)] == '\0')
		buf[strlen(buf)] = ';';

	/* Parse values read from file. */
	for (char *token = strtok_r(buf, ";", &token_end);
	     token != NULL;
	     token = strtok_r(NULL, ";", &token_end))
	{
		strtok_r(token, ": ", &val_token);
		lane_count[i] = strtol(val_token, &val_token, 10);
		link_rate[i] = strtol(val_token, &val_token, 10);
		link_spread[i] = strtol(val_token, &val_token, 10);
		i++;

		if (i > 3) return;
	}
}

/*
 * igt_amd_write_link_settings:
 * @drm_fd: DRM file descriptor
 * @connector_name: The name of the connector to write the link_settings
 * @lane_count: Lane count
 * @link_rate: Link rate
 * @training_type: Link training type
 */
void igt_amd_write_link_settings(
	int drm_fd, char *connector_name, enum dc_lane_count lane_count,
	enum dc_link_rate link_rate, enum dc_link_training_type training_type)
{
	int ls_fd, fd;
	const int buf_len = 40;
	char buf[buf_len];
	int wr_len = 0;

	memset(buf, '\0', sizeof(char) * buf_len);

	fd = igt_debugfs_connector_dir(drm_fd, connector_name, O_RDONLY);
	igt_assert(fd >= 0);
	ls_fd = openat(fd, DEBUGFS_DP_LINK_SETTINGS, O_WRONLY);
	close(fd);
	igt_assert(ls_fd >= 0);

	/* dp_link_settings_write expects a \n at the end or else it will
	 * dereference a null pointer.
	 */
	if (training_type == LINK_TRAINING_DEFAULT)
		snprintf(buf, sizeof(buf), "%02x %02x \n", lane_count, link_rate);
	else
		snprintf(buf, sizeof(buf), "%02x %02x %02x \n", lane_count,
			 link_rate, training_type);

	wr_len = write(ls_fd, buf, strlen(buf));
	igt_assert_eq(wr_len, strlen(buf));

	close(ls_fd);
}

/**
 * igt_amd_output_has_link_settings: check if connector has link_settings debugfs entry
 * @drm_fd: DRM file descriptor
 * @connector_name: The connector's name, on which we're reading the status
 */
bool igt_amd_output_has_link_settings(int drm_fd, char *connector_name)
{
	int fd;
	int res;
	struct stat stat;

	fd = igt_debugfs_connector_dir(drm_fd, connector_name, O_RDONLY);
	if (fd < 0) {
		igt_info("output %s: debugfs not found\n", connector_name);
		return false;
	}

	res = fstatat(fd, DEBUGFS_DP_LINK_SETTINGS, &stat, 0);
	if (res != 0) {
		igt_info("output %s: %s debugfs not supported\n", connector_name, DEBUGFS_DP_LINK_SETTINGS);
		close(fd);
		return false;
	}

	close(fd);
	return true;
}
