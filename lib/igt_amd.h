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

#ifndef IGT_AMD_H
#define IGT_AMD_H

#include <stdint.h>
#include "igt_fb.h"

uint32_t igt_amd_create_bo(int fd, uint64_t size);
void *igt_amd_mmap_bo(int fd, uint32_t handle, uint64_t size, int prot);
unsigned int igt_amd_compute_offset(unsigned int* swizzle_pattern,
				       unsigned int x, unsigned int y);
unsigned int igt_amd_fb_get_blk_size_table_idx(unsigned int bpp);
void igt_amd_fb_calculate_tile_dimension(unsigned int bpp,
				       unsigned int *width, unsigned int *height);
uint32_t igt_amd_fb_tiled_offset(unsigned int bpp, unsigned int x_input,
				       unsigned int y_input, unsigned int width_input);
void igt_amd_fb_to_tiled(struct igt_fb *dst, void *dst_buf, struct igt_fb *src,
				       void *src_buf, unsigned int plane);
void igt_amd_fb_convert_plane_to_tiled(struct igt_fb *dst, void *dst_buf,
				       struct igt_fb *src, void *src_buf);
bool igt_amd_is_tiled(uint64_t modifier);
#endif /* IGT_AMD_H */
