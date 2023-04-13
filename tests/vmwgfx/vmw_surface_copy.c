// SPDX-License-Identifier: GPL-2.0 OR MIT
/**********************************************************
 * Copyright 2021-2022 VMware, Inc.
 *
 * Permission is hereby granted, free of charge, to any person
 * obtaining a copy of this software and associated documentation
 * files (the "Software"), to deal in the Software without
 * restriction, including without limitation the rights to use, copy,
 * modify, merge, publish, distribute, sublicense, and/or sell copies
 * of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 **********************************************************/

#include "igt_vmwgfx.h"

IGT_TEST_DESCRIPTION("Test surface copies.");

/**
 * Compares two 2d surfaces to see if their size and contents are equal.
 */
static bool are_surfaces_identical(int fd, struct vmw_surface *s1,
				   struct vmw_surface *s2)
{
	struct vmw_mob mob1 = { .size = s1->base.buffer_size,
				.handle = s1->base.buffer_handle,
				.map_handle = s1->base.buffer_map_handle };
	struct vmw_mob mob2 = { .size = s2->base.buffer_size,
				.handle = s2->base.buffer_handle,
				.map_handle = s2->base.buffer_map_handle };
	char *readback1;
	char *readback2;
	bool is_equal = true;
	uint32 i, j;
	uint32 stride1, stride2, height, width;

	if ((s1->params.base.base_size.width !=
	     s2->params.base.base_size.width) ||
	    (s1->params.base.base_size.height !=
	     s2->params.base.base_size.height)) {
		return false;
	}
	width = s1->params.base.base_size.width;
	height = s1->params.base.base_size.height;
	stride1 = s1->params.buffer_byte_stride == 0 ?
			  width :
			  s1->params.buffer_byte_stride;
	stride2 = s2->params.buffer_byte_stride == 0 ?
			  width :
			  s2->params.buffer_byte_stride;

	readback1 = vmw_ioctl_mob_map(fd, &mob1);
	readback2 = vmw_ioctl_mob_map(fd, &mob2);
	for (i = 0; i < width; i++) {
		for (j = 0; j < height; j++) {
			if (readback1[j * stride1 + i] !=
			    readback2[j * stride2 + i]) {
				is_equal = false;
				goto are_surfaces_identical_end;
			}
		}
	}
are_surfaces_identical_end:
	vmw_ioctl_mob_unmap(&mob1);
	vmw_ioctl_mob_unmap(&mob2);
	return is_equal;
}

static void set_surface_value(int fd, struct vmw_surface *surface, char value)
{
	struct vmw_mob mob = { .size = surface->base.buffer_size,
			       .handle = surface->base.buffer_handle,
			       .map_handle = surface->base.buffer_map_handle };
	char *readback;

	readback = vmw_ioctl_mob_map(fd, &mob);
	memset(readback, value, mob.size);
	vmw_ioctl_mob_unmap(&mob);
}

static void exec_surface_copy(struct vmw_execbuf *cmd_buf,
			      struct drm_vmw_fence_rep *cmd_fence,
			      SVGA3dSurfaceImageId src,
			      SVGA3dSurfaceImageId dest, SVGA3dCopyBox *box)
{
	vmw_cmd_surface_copy(cmd_buf, src, dest, box, 1);
	vmw_execbuf_submit(cmd_buf, cmd_fence);
	vmw_ioctl_fence_finish(cmd_buf->drm_fd, cmd_fence);
}

static void test_invalid_copies(int fd, int32 cid)
{
	struct vmw_surface *s1;
	struct vmw_surface *s2;
	struct vmw_execbuf *cmd_buf;
	struct drm_vmw_fence_rep cmd_fence;
	SVGA3dSize surface_size;
	SVGA3dCopyBox box;
	SVGA3dSurfaceImageId src;
	SVGA3dSurfaceImageId dest;
	SVGA3dSurfaceImageId bad_surface;

	surface_size.width = 128;
	surface_size.height = 128;
	surface_size.depth = 1;

	igt_require(
		vmw_is_format_supported(fd, SVGA3D_DEVCAP_SURFACEFMT_A8R8G8B8));

	s1 = vmw_create_surface_simple(fd, 0, SVGA3D_A8R8G8B8, surface_size,
				       NULL);
	s2 = vmw_create_surface_simple(fd, 0, SVGA3D_A8R8G8B8, surface_size,
				       NULL);
	cmd_buf = vmw_execbuf_create(fd, cid);

	box.x = 0;
	box.y = 0;
	box.z = 0;
	box.w = 1;
	box.h = 1;
	box.d = 1;
	box.srcx = 0;
	box.srcy = 0;
	box.srcz = 0;

	src.sid = s1->base.handle;
	src.face = 0;
	src.mipmap = 0;
	dest.sid = s2->base.handle;
	dest.face = 0;
	dest.mipmap = 0;

	/* Testing a valid copy first */
	exec_surface_copy(cmd_buf, &cmd_fence, src, dest, &box);
	igt_assert(are_surfaces_identical(fd, s1, s2));

	/* Setting surfaces to different values */
	set_surface_value(fd, s1, 0);
	set_surface_value(fd, s2, 16);

	/* Testing invalid copies */

	/* x */
	box.x = 129;
	box.w = 1;
	exec_surface_copy(cmd_buf, &cmd_fence, src, dest, &box);
	igt_assert(!are_surfaces_identical(fd, s1, s2));

	box.x = 0;
	box.w = 129;
	exec_surface_copy(cmd_buf, &cmd_fence, src, dest, &box);
	igt_assert(!are_surfaces_identical(fd, s1, s2));

	box.srcx = 129;
	box.w = 1;
	exec_surface_copy(cmd_buf, &cmd_fence, src, dest, &box);
	igt_assert(!are_surfaces_identical(fd, s1, s2));

	box.srcx = 0;
	box.w = 129;
	exec_surface_copy(cmd_buf, &cmd_fence, src, dest, &box);
	igt_assert(!are_surfaces_identical(fd, s1, s2));

	/* y */
	box.y = 129;
	box.h = 1;
	exec_surface_copy(cmd_buf, &cmd_fence, src, dest, &box);
	igt_assert(!are_surfaces_identical(fd, s1, s2));

	box.y = 0;
	box.h = 129;
	exec_surface_copy(cmd_buf, &cmd_fence, src, dest, &box);
	igt_assert(!are_surfaces_identical(fd, s1, s2));

	box.srcy = 129;
	box.h = 1;
	exec_surface_copy(cmd_buf, &cmd_fence, src, dest, &box);
	igt_assert(!are_surfaces_identical(fd, s1, s2));

	box.srcy = 0;
	box.h = 129;
	exec_surface_copy(cmd_buf, &cmd_fence, src, dest, &box);
	igt_assert(!are_surfaces_identical(fd, s1, s2));

	/* z */
	box.z = 2;
	box.d = 1;
	exec_surface_copy(cmd_buf, &cmd_fence, src, dest, &box);
	igt_assert(!are_surfaces_identical(fd, s1, s2));

	box.z = 0;
	box.d = 2;
	exec_surface_copy(cmd_buf, &cmd_fence, src, dest, &box);
	igt_assert(!are_surfaces_identical(fd, s1, s2));

	box.srcz = 2;
	box.d = 1;
	exec_surface_copy(cmd_buf, &cmd_fence, src, dest, &box);
	igt_assert(!are_surfaces_identical(fd, s1, s2));

	box.srcz = 0;
	box.d = 2;
	exec_surface_copy(cmd_buf, &cmd_fence, src, dest, &box);
	igt_assert(!are_surfaces_identical(fd, s1, s2));

	/* Invalid surface id */
	bad_surface.sid = src.sid + dest.sid + 1;
	bad_surface.face = 0;
	bad_surface.mipmap = 0;

	box.x = 0;
	box.y = 0;
	box.z = 0;
	box.w = 1;
	box.h = 1;
	box.d = 1;
	box.srcx = 0;
	box.srcy = 0;
	box.srcz = 0;

	vmw_cmd_surface_copy(cmd_buf, bad_surface, dest, &box, 1);
	igt_assert(vmw_execbuf_submit(cmd_buf, &cmd_fence) != 0);

	vmw_cmd_surface_copy(cmd_buf, src, bad_surface, &box, 1);
	igt_assert(vmw_execbuf_submit(cmd_buf, &cmd_fence) != 0);
	vmw_ioctl_fence_finish(fd, &cmd_fence);

	bad_surface.sid = src.sid;
	bad_surface.face = 2;

	exec_surface_copy(cmd_buf, &cmd_fence, bad_surface, dest, &box);
	igt_assert(!are_surfaces_identical(fd, s1, s2));

	exec_surface_copy(cmd_buf, &cmd_fence, src, bad_surface, &box);
	igt_assert(!are_surfaces_identical(fd, s1, s2));

	vmw_execbuf_destroy(cmd_buf);
	vmw_ioctl_surface_unref(fd, s1);
	vmw_ioctl_surface_unref(fd, s2);
}

static void test_invalid_copies_3d(int fd, int32 cid)
{
	struct vmw_surface *s1;
	struct vmw_surface *s2;
	struct vmw_execbuf *cmd_buf;
	struct drm_vmw_fence_rep cmd_fence;
	SVGA3dSize surface_size;
	SVGA3dCopyBox box;
	SVGA3dSurfaceImageId src;
	SVGA3dSurfaceImageId dest;

	surface_size.width = 128;
	surface_size.height = 128;
	surface_size.depth = 1;

	igt_require(
		vmw_is_format_supported(fd, SVGA3D_DEVCAP_SURFACEFMT_A8R8G8B8));
	igt_require(vmw_is_format_supported(fd, SVGA3D_DEVCAP_DXFMT_Z_D32));

	s1 = vmw_create_surface_simple(fd, 0, SVGA3D_A8R8G8B8, surface_size,
				       NULL);
	s2 = vmw_create_surface_simple(fd, 0, SVGA3D_Z_D32, surface_size, NULL);
	cmd_buf = vmw_execbuf_create(fd, cid);

	box.x = 0;
	box.y = 0;
	box.z = 0;
	box.w = 10;
	box.h = 10;
	box.d = 10;
	box.srcx = 0;
	box.srcy = 0;
	box.srcz = 0;

	src.sid = s1->base.handle;
	src.face = 0;
	src.mipmap = 0;
	dest.sid = s2->base.handle;
	dest.face = 0;
	dest.mipmap = 0;

	set_surface_value(fd, s1, 0);
	set_surface_value(fd, s2, 16);

	exec_surface_copy(cmd_buf, &cmd_fence, src, dest, &box);
	igt_assert(!are_surfaces_identical(fd, s1, s2));

	exec_surface_copy(cmd_buf, &cmd_fence, dest, src, &box);
	igt_assert(!are_surfaces_identical(fd, s1, s2));

	vmw_execbuf_destroy(cmd_buf);
	vmw_ioctl_surface_unref(fd, s1);
	vmw_ioctl_surface_unref(fd, s2);
}

igt_main
{
	int fd;
	int32 cid;

	igt_fixture
	{
		fd = drm_open_driver_render(DRIVER_VMWGFX);
		igt_require(fd != -1);

		cid = vmw_ioctl_context_create(fd);
		igt_require(cid != SVGA3D_INVALID_ID);
	}

	igt_describe("Test surface copies (valid and invalid ones).");
	igt_subtest("test_invalid_copies")
	{
		test_invalid_copies(fd, cid);
	}

	igt_describe("Test surface copies on 3D enabled contexts.");
	igt_subtest("test_invalid_copies_3d")
	{
		igt_require(vmw_ioctl_get_param(fd, DRM_VMW_PARAM_3D));
		test_invalid_copies_3d(fd, cid);
	}

	igt_fixture
	{
		vmw_ioctl_context_destroy(fd, cid);
		close(fd);
	}
}
