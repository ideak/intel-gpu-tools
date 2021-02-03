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
