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
#include "igt_sysfs.h"
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
 * @brief generic helper to check if the amdgpu dm debugfs entry defined
 *
 * @param drm_fd DRM file descriptor
 * @param interface_name The debugfs interface entry name with prefix "amdgpu_"
 * @return true if <debugfs_root>/interface_name exists and defined
 * @return false otherwise
 */
static bool amd_has_debugfs(int drm_fd, const char *interface_name)
{
	int fd;
	int res;
	struct stat stat;

	fd = igt_debugfs_dir(drm_fd);
	if (fd < 0) {
		igt_info("Couldn't open debugfs dir!\n");
		return false;
	}

	res = fstatat(fd, interface_name, &stat, 0);
	if (res != 0) {
		igt_info("debugfs %s not supported\n", interface_name);
		close(fd);
		return false;
	}

	close(fd);
	return true;
}


/**
 * @brief generic helper to check if the debugfs entry of given connector has the
 *        debugfs interface defined.
 * @param drm_fd: DRM file descriptor
 * @param connector_name: The connector's name, on which we're reading the status
 * @param interface_name: The debugfs interface name to check
 * @return true if <debugfs_root>/connector_name/interface_name exists and defined
 * @return false otherwise
 */
static bool igt_amd_output_has_debugfs(int drm_fd, char *connector_name, const char *interface_name)
{
	int fd;
	int res;
	struct stat stat;

	fd = igt_debugfs_connector_dir(drm_fd, connector_name, O_RDONLY);
	if (fd < 0) {
		igt_info("output %s: debugfs not found\n", connector_name);
		return false;
	}

	res = fstatat(fd, interface_name, &stat, 0);
	if (res != 0) {
		igt_info("output %s: %s debugfs not supported\n", connector_name, interface_name);
		close(fd);
		return false;
	}

	close(fd);
	return true;
}

/**
 * igt_amd_output_has_dsc: check if connector has dsc debugfs entry
 * @drm_fd: DRM file descriptor
 * @connector_name: The connector's name, on which we're reading the status
 */
static bool igt_amd_output_has_dsc(int drm_fd, char *connector_name)
{
	return igt_amd_output_has_debugfs(drm_fd, connector_name, DEBUGFS_DSC_CLOCK_EN);
}

/**
 * is_dp_dsc_supported: Checks if connector is DSC capable
 * @display: A pointer to an #igt_display_t structure
 * @drm_fd: DRM file descriptor
 */
bool is_dp_dsc_supported(int drm_fd, char *connector_name)
{
	char buf[512];
	int fd, ret;

	fd = igt_debugfs_connector_dir(drm_fd, connector_name, O_RDONLY);

	if (fd < 0) {
		igt_info("Couldn't open connector %s debugfs directory\n",
			 connector_name);
		return false;
	}

	ret = igt_debugfs_simple_read(fd, DEBUGFS_DSC_FEC_SUPPORT, buf, sizeof(buf));
	close(fd);

	igt_assert_f(ret >= 0, "Reading %s for connector %s failed.\n",
		     DEBUGFS_DSC_FEC_SUPPORT, connector_name);

	return strstr(buf, "DSC_Sink_Support: yes");
}

/**
 * is_dp_fec_supported: Checks if connector is FEC capable
 * @display: A pointer to an #igt_display_t structure
 * @drm_fd: DRM file descriptor
 */
bool is_dp_fec_supported(int drm_fd, char *connector_name)
{
	char buf[512];
	int fd, ret;

	fd = igt_debugfs_connector_dir(drm_fd, connector_name, O_RDONLY);

	if (fd < 0) {
		igt_info("Couldn't open connector %s debugfs directory\n",
			 connector_name);
		return false;
	}

	ret = igt_debugfs_simple_read(fd, DEBUGFS_DSC_FEC_SUPPORT, buf, sizeof(buf));
	close(fd);

	igt_assert_f(ret >= 0, "Reading %s for connector %s failed.\n",
		     DEBUGFS_DSC_FEC_SUPPORT, connector_name);

	return strstr(buf, "FEC_Sink_Support: yes");
}

/**
 * igt_amd_require_dsc: Checks if connectors have DSC debugfs
 * @display: A pointer to an #igt_display_t structure
 * @drm_fd: DRM file descriptor
 *
 * Checks if the AMDGPU driver has support of debugfs entries for
 * DSC. Skip test if DSC is not supported.
 */
void igt_amd_require_dsc(igt_display_t *display, int drm_fd)
{
	igt_output_t *output;

	for_each_connected_output(display, output) {
		if (igt_amd_output_has_dsc(drm_fd, output->name))
			return;
	}

	igt_skip("No DSC debugfs support.\n");
}

/**
 * igt_amd_read_dsc_clock_status: Read the DSC Clock Enable debugfs
 * @drm_fd: DRM file descriptor
 * @connector_name: The connector's name, which we use to read status on
 *
 */
int igt_amd_read_dsc_clock_status(int drm_fd, char *connector_name)
{
	char buf[4];
	int fd, ret;

	fd = igt_debugfs_connector_dir(drm_fd, connector_name, O_RDONLY);
	if (fd < 0) {
		igt_info("Couldn't open connector %s debugfs directory\n",
			 connector_name);
		return false;
	}
	ret = igt_debugfs_simple_read(fd, DEBUGFS_DSC_CLOCK_EN, buf, sizeof(buf));
	close(fd);

	igt_assert_f(ret >= 0, "Reading %s for connector %s failed.\n",
		     DEBUGFS_DSC_CLOCK_EN, connector_name);

	return strtol(buf, NULL, 0);
}


/**
 * igt_amd_write_dsc_clock_en: Write the DSC Clock Enable debugfs
 * @drm_fd: DRM file descriptor
 * @connector_name: The connector's name, which we use to read status on
 * @dsc_force: DSC force parameter, 0 - DSC automatic, 1 - DSC force on,
 * 2 - DSC force off
 *
 */
void igt_amd_write_dsc_clock_en(int drm_fd, char *connector_name, int dsc_force)
{
	int fd, dsc_fd;
	char src[4];
	int wr_len;

	fd = igt_debugfs_connector_dir(drm_fd, connector_name, O_RDONLY);
	igt_assert(fd >= 0);
	dsc_fd = openat(fd, DEBUGFS_DSC_CLOCK_EN, O_WRONLY);
	close(fd);
	igt_assert(dsc_fd >= 0);

	if (dsc_force == DSC_FORCE_ON)
		snprintf(src, sizeof(src), "%d", 1);
	else if (dsc_force == DSC_FORCE_OFF)
		snprintf(src, sizeof(src), "%d", 2);
	else
		snprintf(src, sizeof(src), "%d", 0);

	igt_info("DSC Clock force, write %s > dsc_clock_en\n", src);

	wr_len = write(dsc_fd, src, strlen(src));
	close(dsc_fd);
	igt_assert_eq(wr_len, strlen(src));
}

/**
 * igt_amd_write_dsc_param_slice_height: Write the DSC Slice Height debugfs
 * @drm_fd: DRM file descriptor
 * @connector_name: The connector's name, which we use to read status on
 * @slice_height: DSC slice height parameter, accepts any positive integer,
 * 		  if parameter is negative - it will not write to debugfs.
 *
 */
void igt_amd_write_dsc_param_slice_height(int drm_fd, char *connector_name, int slice_height)
{
	int fd, dsc_fd;
	char src[32];
	int wr_len;

	fd = igt_debugfs_connector_dir(drm_fd, connector_name, O_RDONLY);
	igt_assert(fd >= 0);
	dsc_fd = openat(fd, DEBUGFS_DSC_SLICE_HEIGHT, O_WRONLY);
	close(fd);
	igt_assert(dsc_fd >= 0);

	if (slice_height >= 0) {
		snprintf(src, sizeof(src), "%#x", slice_height);
	} else {
		igt_warn("DSC SLICE HEIGHT, slice height parameter is invalid (%d)\n", slice_height);
		goto exit;
	}

	igt_info("DSC SLICE HEIGHT, write %s > dsc_slice_height\n", src);

	wr_len = write(dsc_fd, src, strlen(src));
	igt_assert_eq(wr_len, strlen(src));
exit:
	close(dsc_fd);
}

/**
 * igt_amd_read_dsc_param_slice_height: Read the DSC Slice Height debugfs
 * @drm_fd: DRM file descriptor
 * @connector_name: The connector's name, which we use to read status on
 *
 */
int igt_amd_read_dsc_param_slice_height(int drm_fd, char *connector_name)
{
	char buf[32];
	int fd, ret;

	fd = igt_debugfs_connector_dir(drm_fd, connector_name, O_RDONLY);
	if (fd < 0) {
		igt_info("Couldn't open connector %s debugfs directory\n",
			 connector_name);
		return false;
	}
	ret = igt_debugfs_simple_read(fd, DEBUGFS_DSC_SLICE_HEIGHT, buf, sizeof(buf));
	close(fd);

	igt_assert_f(ret >= 0, "Reading %s for connector %s failed.\n",
		     DEBUGFS_DSC_SLICE_HEIGHT, connector_name);

	return strtol(buf, NULL, 0);
}

/**
 * igt_amd_write_dsc_param_slice_width: Write the DSC Slice Width debugfs
 * @drm_fd: DRM file descriptor
 * @connector_name: The connector's name, which we use to read status on
 * @slice_width: DSC slice width parameter, accepts any positive integer,
 * 		 if parameter is negative - it will not write to debugfs.
 *
 */
void igt_amd_write_dsc_param_slice_width(int drm_fd, char *connector_name, int slice_width)
{
	int fd, dsc_fd;
	char src[32];
	int wr_len;

	fd = igt_debugfs_connector_dir(drm_fd, connector_name, O_RDONLY);
	igt_assert(fd >= 0);
	dsc_fd = openat(fd, DEBUGFS_DSC_SLICE_WIDTH, O_WRONLY);
	close(fd);
	igt_assert(dsc_fd >= 0);

	if (slice_width >= 0) {
		snprintf(src, sizeof(src), "%#x", slice_width);
	} else {
		igt_warn("DSC SLICE WIDTH, slice width parameter is invalid (%d)\n", slice_width);
		goto exit;
	}

	igt_info("DSC SLICE WIDTH, write %s > dsc_slice_width\n", src);

	wr_len = write(dsc_fd, src, strlen(src));
	igt_assert_eq(wr_len, strlen(src));
exit:
	close(dsc_fd);
}

/**
 * igt_amd_read_dsc_param_slice_width: Read the DSC Slice Width debugfs
 * @drm_fd: DRM file descriptor
 * @connector_name: The connector's name, which we use to read status on
 *
 */
int igt_amd_read_dsc_param_slice_width(int drm_fd, char *connector_name)
{
	char buf[32];
	int fd, ret;

	fd = igt_debugfs_connector_dir(drm_fd, connector_name, O_RDONLY);
	if (fd < 0) {
		igt_info("Couldn't open connector %s debugfs directory\n",
			 connector_name);
		return false;
	}
	ret = igt_debugfs_simple_read(fd, DEBUGFS_DSC_SLICE_WIDTH, buf, sizeof(buf));
	close(fd);

	igt_assert_f(ret >= 0, "Reading %s for connector %s failed.\n",
		     DEBUGFS_DSC_SLICE_WIDTH, connector_name);

	return strtol(buf, NULL, 0);
}

/**
 * igt_amd_write_dsc_param_bpp: Write the DSC Bits Per Pixel debugfs
 * @drm_fd: DRM file descriptor
 * @connector_name: The connector's name, which we use to read status on
 * @bpp: DSC bits per pixel parameter, accepts any positive integer,
 * 	 if parameter is negative - it will not write to debugfs.
 *
 */
void igt_amd_write_dsc_param_bpp(int drm_fd, char *connector_name, int bpp)
{
	int fd, dsc_fd;
	char src[32];
	int wr_len;

	fd = igt_debugfs_connector_dir(drm_fd, connector_name, O_RDONLY);
	igt_assert(fd >= 0);
	dsc_fd = openat(fd, DEBUGFS_DSC_BITS_PER_PIXEL, O_WRONLY);
	close(fd);
	igt_assert(dsc_fd >= 0);

	if (bpp >= 0) {
		snprintf(src, sizeof(src), "%#x", bpp);
	} else {
		igt_warn("DSC BITS PER PIXEL, bits per pixel parameter is invalid (%d)\n", bpp);
		goto exit;
	}

	igt_info("DSC BITS PER PIXEL, write %s > dsc_bits_per_pixel\n", src);

	wr_len = write(dsc_fd, src, strlen(src));
	igt_assert_eq(wr_len, strlen(src));
exit:
	close(dsc_fd);
}

/**
 * igt_amd_read_dsc_param_bpp: Read the DSC Bits Per Pixel debugfs
 * @drm_fd: DRM file descriptor
 * @connector_name: The connector's name, which we use to read status on
 *
 */
int igt_amd_read_dsc_param_bpp(int drm_fd, char *connector_name)
{
	char buf[32];
	int fd, ret;

	fd = igt_debugfs_connector_dir(drm_fd, connector_name, O_RDONLY);
	if (fd < 0) {
		igt_info("Couldn't open connector %s debugfs directory\n",
			 connector_name);
		return false;
	}
	ret = igt_debugfs_simple_read(fd, DEBUGFS_DSC_BITS_PER_PIXEL, buf, sizeof(buf));
	close(fd);

	igt_assert_f(ret >= 0, "Reading %s for connector %s failed.\n",
		     DEBUGFS_DSC_BITS_PER_PIXEL, connector_name);

	return strtol(buf, NULL, 0);
}

/**
 * igt_amd_read_dsc_param_pic_width: Read the DSC Picture Width debugfs
 * @drm_fd: DRM file descriptor
 * @connector_name: The connector's name, which we use to read status on
 *
 */
int igt_amd_read_dsc_param_pic_width(int drm_fd, char *connector_name)
{
	char buf[4];
	int fd, ret;

	fd = igt_debugfs_connector_dir(drm_fd, connector_name, O_RDONLY);
	if (fd < 0) {
		igt_info("Couldn't open connector %s debugfs directory\n",
			 connector_name);
		return false;
	}
	ret = igt_debugfs_simple_read(fd, DEBUGFS_DSC_PIC_WIDTH, buf, sizeof(buf));
	close(fd);

	igt_assert_f(ret >= 0, "Reading %s for connector %s failed.\n",
		     DEBUGFS_DSC_PIC_WIDTH, connector_name);

	return strtol(buf, NULL, 0);
}

/**
 * igt_amd_read_dsc_param_pic_height: Read the DSC Picture Height debugfs
 * @drm_fd: DRM file descriptor
 * @connector_name: The connector's name, which we use to read status on
 *
 */
int igt_amd_read_dsc_param_pic_height(int drm_fd, char *connector_name)
{
	char buf[4];
	int fd, ret;

	fd = igt_debugfs_connector_dir(drm_fd, connector_name, O_RDONLY);
	if (fd < 0) {
		igt_info("Couldn't open connector %s debugfs directory\n",
			 connector_name);
		return false;
	}
	ret = igt_debugfs_simple_read(fd, DEBUGFS_DSC_PIC_HEIGHT, buf, sizeof(buf));
	close(fd);

	igt_assert_f(ret >= 0, "Reading %s for connector %s failed.\n",
		     DEBUGFS_DSC_PIC_HEIGHT, connector_name);

	return strtol(buf, NULL, 0);
}

/**
 * igt_amd_read_dsc_param_chunk_size: Read the DSC Chunk Size debugfs
 * @drm_fd: DRM file descriptor
 * @connector_name: The connector's name, which we use to read status on
 *
 */
int igt_amd_read_dsc_param_chunk_size(int drm_fd, char *connector_name)
{
	char buf[4];
	int fd, ret;

	fd = igt_debugfs_connector_dir(drm_fd, connector_name, O_RDONLY);
	if (fd < 0) {
		igt_info("Couldn't open connector %s debugfs directory\n",
			 connector_name);
		return false;
	}
	ret = igt_debugfs_simple_read(fd, DEBUGFS_DSC_CHUNK_SIZE, buf, sizeof(buf));
	close(fd);

	igt_assert_f(ret >= 0, "Reading %s for connector %s failed.\n",
		     DEBUGFS_DSC_CHUNK_SIZE, connector_name);

	return strtol(buf, NULL, 0);
}

/**
 * igt_amd_read_dsc_param_slice_bpg: Read the DSC Slice BPG Offset debugfs
 * @drm_fd: DRM file descriptor
 * @connector_name: The connector's name, which we use to read status on
 *
 */
int igt_amd_read_dsc_param_slice_bpg(int drm_fd, char *connector_name)
{
	char buf[4];
	int fd, ret;

	fd = igt_debugfs_connector_dir(drm_fd, connector_name, O_RDONLY);
	if (fd < 0) {
		igt_info("Couldn't open connector %s debugfs directory\n",
			 connector_name);
		return false;
	}
	ret = igt_debugfs_simple_read(fd, DEBUGFS_DSC_SLICE_BPG, buf, sizeof(buf));
	close(fd);

	igt_assert_f(ret >= 0, "Reading %s for connector %s failed.\n",
		     DEBUGFS_DSC_SLICE_BPG, connector_name);

	return strtol(buf, NULL, 0);
}

/**
 * igt_amd_output_has_hpd: check if connector has HPD debugfs entry
 * @drm_fd: DRM file descriptor
 * @connector_name: The connector's name, on which we're reading the status
 */
static bool igt_amd_output_has_hpd(int drm_fd, char *connector_name)
{
        return igt_amd_output_has_debugfs(drm_fd, connector_name, DEBUGFS_HPD_TRIGGER);
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
		link_rate[i] = strtol(val_token, &val_token, 16);
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
	return igt_amd_output_has_debugfs(drm_fd, connector_name, DEBUGFS_DP_LINK_SETTINGS);
}

/*
 * igt_amd_read_ilr_setting:
 * @drm_fd: DRM file descriptor
 * @connector_name: The name of the connector to read the link_settings
 * @supported_ilr: supported link rates
 *
 * The indices of @supported_ilr correspond to the supported customized
 * link rates reported from DPCD 00010h ~ 0001Fh
 */
void igt_amd_read_ilr_setting(
	int drm_fd, char *connector_name, int *supported_ilr)
{
	int fd, ret;
	char buf[256] = {'\0'};
	int i = 0;
	char *token_end, *val_token, *tmp;

	fd = igt_debugfs_connector_dir(drm_fd, connector_name, O_RDONLY);
	if (fd < 0) {
		igt_info("Could not open connector %s debugfs directory\n",
			 connector_name);
		return;
	}
	ret = igt_debugfs_simple_read(fd, DEBUGFS_EDP_ILR_SETTING, buf, sizeof(buf));
	igt_assert_f(ret >= 0, "Reading %s for connector %s failed.\n",
		     DEBUGFS_EDP_ILR_SETTING, connector_name);

	close(fd);

	tmp = strstr(buf, "not supported");
	if (tmp) {
		igt_info("Connector %s: eDP panel doesn't support ILR\n%s",
			 connector_name, buf);
		return;
	}

	/* Parse values read from file. */
	for (char *token = strtok_r(buf, "\n", &token_end);
	     token != NULL;
	     token = strtok_r(NULL, "\n", &token_end))
	{
		strtok_r(token, "] ", &val_token);
		supported_ilr[i] = strtol(val_token, &val_token, 10);
		i++;

		if (i >= MAX_SUPPORTED_ILR) return;
	}
}

/*
 * igt_amd_write_link_settings:
 * @drm_fd: DRM file descriptor
 * @connector_name: The name of the connector to write the link_settings
 * @lane_count: Lane count
 * @link_rate_set: Intermediate link rate
 */
void igt_amd_write_ilr_setting(
	int drm_fd, char *connector_name, enum dc_lane_count lane_count,
	uint8_t link_rate_set)
{
	int ls_fd, fd;
	const int buf_len = 40;
	char buf[buf_len];
	int wr_len = 0;

	memset(buf, '\0', sizeof(char) * buf_len);

	fd = igt_debugfs_connector_dir(drm_fd, connector_name, O_RDONLY);
	igt_assert(fd >= 0);
	ls_fd = openat(fd, DEBUGFS_EDP_ILR_SETTING, O_WRONLY);
	close(fd);
	igt_assert(ls_fd >= 0);

	/* edp_ilr_write expects a \n at the end or else it will
	 * dereference a null pointer.
	 */
	snprintf(buf, sizeof(buf), "%02x %02x \n", lane_count, link_rate_set);

	wr_len = write(ls_fd, buf, strlen(buf));
	igt_assert_eq(wr_len, strlen(buf));

	close(ls_fd);
}

/**
 * igt_amd_output_has_ilr_setting: check if connector has ilr_setting debugfs entry
 * @drm_fd: DRM file descriptor
 * @connector_name: The connector's name, on which we're reading the status
 */
bool igt_amd_output_has_ilr_setting(int drm_fd, char *connector_name)
{
	return igt_amd_output_has_debugfs(drm_fd, connector_name, DEBUGFS_EDP_ILR_SETTING);
}

/**
 * igt_amd_output_has_psr_cap: check if eDP connector has psr_capability debugfs entry
 * @drm_fd: DRM file descriptor
 * @connector_name: The connector's name, on which we're reading the status
 */
bool igt_amd_output_has_psr_cap(int drm_fd, char *connector_name)
{
	return igt_amd_output_has_debugfs(drm_fd, connector_name, DEBUGFS_EDP_PSR_CAP);
}

/**
 * igt_amd_psr_support_sink: check if sink device support PSR
 * @drm_fd: DRM file descriptor
 * @connector_name: The connector's name, on which we're reading the status
 * @mode: expected PSR mode, either PSR1 or PSR2
 */
bool igt_amd_psr_support_sink(int drm_fd, char *connector_name, enum psr_mode mode)
{
	char buf[PSR_STATUS_MAX_LEN];
	int ret;
	int fd;

	fd = igt_debugfs_connector_dir(drm_fd, connector_name, O_RDONLY);
	if (fd < 0) {
		igt_info("output %s: debugfs not found\n", connector_name);
		return false;
	}

	ret = igt_debugfs_simple_read(fd, DEBUGFS_EDP_PSR_CAP, buf, sizeof(buf));
	igt_assert_f(ret >= 0, "Reading %s for connector %s failed.\n",
		     DEBUGFS_EDP_PSR_CAP, connector_name);
	close(fd);

	if (ret < 1)
		return false;

	if (mode == PSR_MODE_1)
		return strstr(buf, "Sink support: yes [0x01]");
	else
		return strstr(buf, "Sink support: yes [0x03]") ||
		       strstr(buf, "Sink support: yes [0x04]");
}

/**
 * igt_amd_psr_support_drv: check if AMDGPU kernel driver support PSR
 * @drm_fd: DRM file descriptor
 * @connector_name: The connector's name, on which we're reading the status
 * @mode: expected PSR mode, either PSR1 or PSR2
 */
bool igt_amd_psr_support_drv(int drm_fd, char *connector_name, enum psr_mode mode)
{
	char buf[PSR_STATUS_MAX_LEN];
	int ret;
	int fd;

	fd = igt_debugfs_connector_dir(drm_fd, connector_name, O_RDONLY);
	if (fd < 0) {
		igt_info("output %s: debugfs not found\n", connector_name);
		return false;
	}

	ret = igt_debugfs_simple_read(fd, DEBUGFS_EDP_PSR_CAP, buf, sizeof(buf));
	igt_assert_f(ret >= 0, "Reading %s for connector %s failed.\n",
		     DEBUGFS_EDP_PSR_CAP, connector_name);
	close(fd);

	if (ret < 1)
		return false;

	if (mode == PSR_MODE_1)
		return strstr(buf, "Driver support: yes");
	else
		return strstr(buf, "Driver support: yes [0x01]");
}

/**
 * igt_amd_output_has_psr_state: check if eDP connector has psr_state debugfs entry
 * @drm_fd: DRM file descriptor
 * @connector_name: The connector's name, on which we're reading the status
 */
bool igt_amd_output_has_psr_state(int drm_fd, char *connector_name)
{
	return igt_amd_output_has_debugfs(drm_fd, connector_name, DEBUGFS_EDP_PSR_STATE);
}

/**
 * @brief Read PSR State from debugfs interface
 * @param drm_fd DRM file descriptor
 * @param connector_name The connector's name, on which we're reading the status
 * @return PSR state as integer
 */
int igt_amd_read_psr_state(int drm_fd, char *connector_name)
{
	char buf[4];
	int fd, ret;

	fd = igt_debugfs_connector_dir(drm_fd, connector_name, O_RDONLY);
	if (fd < 0) {
		igt_info("Couldn't open connector %s debugfs directory\n", connector_name);
		return -1;
	}

	ret = igt_debugfs_simple_read(fd, DEBUGFS_EDP_PSR_STATE, buf, sizeof(buf));
	close(fd);

	igt_assert_f(ret >= 0, "Reading %s for connector %s failed.\n",
		     DEBUGFS_EDP_PSR_STATE, connector_name);

	return strtol(buf, NULL, 10);
}

/**
 * @brief check if AMDGPU DM visual confirm debugfs interface entry exist and defined
 *
 * @param drm_fd DRM file descriptor
 * @return true if visual confirm debugfs interface exists and defined
 * @return false otherwise
 */
bool igt_amd_has_visual_confirm(int drm_fd)
{
	return amd_has_debugfs(drm_fd, DEBUGFS_DM_VISUAL_CONFIRM);
}

/**
 * @brief Read amdgpu DM visual confirm debugfs interface
 *
 * @param drm_fd DRM file descriptor
 * @return int visual confirm debug option as integer
 */
int  igt_amd_get_visual_confirm(int drm_fd)
{
	char buf[4];	/* current 4 bytes are enough */
	int fd, ret;

	fd = igt_debugfs_dir(drm_fd);
	if (fd < 0) {
		igt_info("Couldn't open debugfs dir!\n");
		return -1;
	}

	ret = igt_debugfs_simple_read(fd, DEBUGFS_DM_VISUAL_CONFIRM, buf, sizeof(buf));
	close(fd);

	igt_assert_f(ret >= 0, "Reading %s failed.\n",
		     DEBUGFS_DM_VISUAL_CONFIRM);

	return strtol(buf, NULL, 10);
}

/**
 * @brief Write amdgpu DM visual confirm debug option to debugfs interface
 *
 * @param drm_fd DRM file descriptor
 * @param option amdgpu DC visual confirm debug option
 * @return true if set visual confirm option success
 * @return false otherwise
 */
bool igt_amd_set_visual_confirm(int drm_fd, enum amdgpu_debug_visual_confirm option)
{
	char buf[4];
	int fd;
	bool res;

	fd = igt_debugfs_dir(drm_fd);
	if (fd < 0) {
		igt_info("Couldn't open debugfs dir!\n");
		return false;
	}

	memset(buf, '\0', sizeof(buf));
	snprintf(buf, sizeof(buf), "%d\n", option);

	res = igt_sysfs_set(fd, DEBUGFS_DM_VISUAL_CONFIRM, buf);
	close(fd);

	return res;
}
