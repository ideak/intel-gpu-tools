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

IGT_TEST_DESCRIPTION("Perform tests related to vmwgfx's ref_count codepaths.");

static uint32 data[10] = { 9, 8, 7, 6, 5, 4, 3, 2, 1, 0 };

static void write_to_mob(int fd, struct vmw_mob *mob)
{
	void *write_data;

	write_data = vmw_ioctl_mob_map(fd, mob);
	memcpy(write_data, data, sizeof(data));
	vmw_ioctl_mob_unmap(mob);
}

static bool verify_mob_data(int fd, struct vmw_mob *mob)
{
	uint32 *read_data;
	void *readback;
	uint32 i;
	bool data_is_equal = true;

	read_data = malloc(mob->size);

	readback = vmw_ioctl_mob_map(fd, mob);
	memcpy(read_data, readback, sizeof(data));
	vmw_ioctl_mob_unmap(mob);

	for (i = 0; i < ARRAY_SIZE(data); i++) {
		if (read_data[i] != data[i]) {
			data_is_equal = false;
			break;
		}
	}

	free(read_data);
	return data_is_equal;
}

static struct vmw_surface *
create_and_write_shareable_surface(int32 fd, SVGA3dSize surface_size)
{
	struct vmw_mob mob = { 0 };
	struct vmw_surface *surface;

	surface = vmw_ioctl_create_surface_full(
		fd, SVGA3D_SURFACE_HINT_RENDERTARGET, SVGA3D_BUFFER, 0,
		SVGA3D_MS_PATTERN_NONE, SVGA3D_MS_QUALITY_NONE,
		SVGA3D_TEX_FILTER_NONE, 1, 1, surface_size, NULL,
		drm_vmw_surface_flag_shareable);

	mob.handle = surface->base.buffer_handle;
	mob.map_handle = surface->base.buffer_map_handle;
	mob.size = surface->base.buffer_size;

	write_to_mob(fd, &mob);

	return surface;
}

static bool ref_surface_and_check_contents(int32 fd, uint32 surface_handle)
{
	struct vmw_surface surface;
	struct vmw_mob mob = { 0 };

	surface = vmw_ioctl_surface_ref(fd, surface_handle,
					DRM_VMW_HANDLE_LEGACY);

	mob.handle = surface.base.handle;
	mob.size = surface.base.buffer_size;
	mob.map_handle = surface.base.buffer_map_handle;

	return verify_mob_data(fd, &mob);
}

igt_main
{
	int32 fd1, fd2;
	const uint32 size = sizeof(data);
	SVGA3dSize surface_size = { .width = size, .height = 1, .depth = 1 };

	igt_fixture
	{
		fd1 = drm_open_driver_render(DRIVER_VMWGFX);
		fd2 = drm_open_driver_render(DRIVER_VMWGFX);
		igt_require(fd1 != -1);
		igt_require(fd2 != -1);
	}

	igt_describe("Test prime transfers with explicit mobs.");
	igt_subtest("surface_prime_transfer_explicit_mob")
	{
		struct vmw_mob *mob;
		struct vmw_surface *surface;
		int32 surface_fd;
		uint32 surface_handle;

		mob = vmw_ioctl_mob_create(fd1, size);
		surface = vmw_ioctl_create_surface_full(
			fd1, SVGA3D_SURFACE_HINT_RENDERTARGET, SVGA3D_BUFFER, 0,
			SVGA3D_MS_PATTERN_NONE, SVGA3D_MS_QUALITY_NONE,
			SVGA3D_TEX_FILTER_NONE, 1, 1, surface_size, mob,
			drm_vmw_surface_flag_shareable);

		write_to_mob(fd1, mob);

		surface_fd =
			prime_handle_to_fd_for_mmap(fd1, surface->base.handle);

		vmw_ioctl_mob_close_handle(fd1, mob);
		vmw_ioctl_surface_unref(fd1, surface);

		surface_handle = prime_fd_to_handle(fd2, surface_fd);
		close(surface_fd);

		igt_assert(ref_surface_and_check_contents(fd2, surface_handle));
	}

	igt_describe("Test prime transfers with implicit mobs.");
	igt_subtest("surface_prime_transfer_implicit_mob")
	{
		struct vmw_surface *surface;
		int32 surface_fd;
		uint32 surface_handle;

		surface = create_and_write_shareable_surface(fd1, surface_size);

		surface_fd =
			prime_handle_to_fd_for_mmap(fd1, surface->base.handle);

		vmw_ioctl_surface_unref(fd1, surface);

		surface_handle = prime_fd_to_handle(fd2, surface_fd);
		close(surface_fd);

		igt_assert(ref_surface_and_check_contents(fd2, surface_handle));
	}

	igt_describe("Test prime transfers with a fd dup.");
	igt_subtest("surface_prime_transfer_fd_dup")
	{
		int32 surface_fd1, surface_fd2;
		uint32 surface_handle;
		struct vmw_surface *surface;

		surface = create_and_write_shareable_surface(fd1, surface_size);

		surface_fd1 =
			prime_handle_to_fd_for_mmap(fd1, surface->base.handle);
		vmw_ioctl_surface_unref(fd1, surface);

		surface_fd2 = dup(surface_fd1);
		close(surface_fd1);

		surface_handle = prime_fd_to_handle(fd2, surface_fd2);
		close(surface_fd2);

		igt_assert(ref_surface_and_check_contents(fd2, surface_handle));
	}

	igt_describe("Test prime lifetime with 2 surfaces.");
	igt_subtest("surface_prime_transfer_two_surfaces")
	{
		int32 surface_fd;
		uint32 surface_handle1, surface_handle2;
		struct vmw_surface *surface1, *surface2;

		surface1 =
			create_and_write_shareable_surface(fd1, surface_size);
		surface2 =
			create_and_write_shareable_surface(fd1, surface_size);

		surface_fd =
			prime_handle_to_fd_for_mmap(fd1, surface1->base.handle);
		vmw_ioctl_surface_unref(fd1, surface1);

		surface_handle1 = prime_fd_to_handle(fd2, surface_fd);
		close(surface_fd);

		surface_fd =
			prime_handle_to_fd_for_mmap(fd1, surface2->base.handle);
		vmw_ioctl_surface_unref(fd1, surface2);

		surface_handle2 = prime_fd_to_handle(fd2, surface_fd);
		close(surface_fd);

		igt_assert(
			ref_surface_and_check_contents(fd2, surface_handle1));
		igt_assert(
			ref_surface_and_check_contents(fd2, surface_handle2));
	}

	igt_describe("Test prime transfers with multiple handles.");
	igt_subtest("surface_prime_transfer_single_surface_multiple_handle")
	{
		int32 surface_fd;
		uint32 surface_handle_old;
		uint32 surface_handle1, surface_handle2, surface_handle3;
		struct vmw_surface *surface;

		surface = create_and_write_shareable_surface(fd1, surface_size);
		surface_handle_old = surface->base.handle;

		surface_fd =
			prime_handle_to_fd_for_mmap(fd1, surface->base.handle);
		vmw_ioctl_surface_unref(fd1, surface);

		surface_handle1 = prime_fd_to_handle(fd1, surface_fd);
		surface_handle2 = prime_fd_to_handle(fd2, surface_fd);
		surface_handle3 = prime_fd_to_handle(fd2, surface_fd);
		close(surface_fd);

		igt_assert_eq_u32(surface_handle_old, surface_handle1);
		igt_assert_eq_u32(surface_handle2, surface_handle3);

		igt_assert(
			ref_surface_and_check_contents(fd1, surface_handle1));
		igt_assert(
			ref_surface_and_check_contents(fd2, surface_handle2));
	}

	igt_describe("Test repeated unrefs on a mob.");
	igt_subtest("mob_repeated_unref")
	{
		struct vmw_mob *mob;
		int i = 0;

		mob = vmw_ioctl_mob_create(fd1, size);
		write_to_mob(fd1, mob);

		/* Shouldn't crash on multiple invocations */
		for (i = 0; i < 3; i++) {
			int ret;
			struct drm_vmw_handle_close_arg arg = {
				.handle = mob->handle
			};
			ret = drmCommandWrite(fd1, DRM_VMW_HANDLE_CLOSE, &arg,
					      sizeof(arg));
			igt_assert_eq(ret, 0);
		}
		free(mob);
	}

	igt_describe("Test repeated unrefs on a surface.");
	igt_subtest("surface_repeated_unref")
	{
		struct vmw_surface *surface;
		int i = 0;

		surface = vmw_ioctl_create_surface_full(
			fd1, SVGA3D_SURFACE_HINT_RENDERTARGET, SVGA3D_BUFFER, 0,
			SVGA3D_MS_PATTERN_NONE, SVGA3D_MS_QUALITY_NONE,
			SVGA3D_TEX_FILTER_NONE, 1, 1, surface_size, NULL,
			drm_vmw_surface_flag_shareable);

		/* Shouldn't crash on multiple invocations */
		for (i = 0; i < 3; i++) {
			struct drm_vmw_surface_arg s_arg = {
				.sid = surface->base.handle,
				.handle_type = DRM_VMW_HANDLE_LEGACY
			};
			drmCommandWrite(fd1, DRM_VMW_UNREF_SURFACE, &s_arg,
					sizeof(s_arg));
		}
		free(surface);
	}

	igt_describe("Test unref on a refed surface.");
	igt_subtest("surface_alloc_ref_unref")
	{
		struct vmw_surface *surface;
		struct vmw_surface ref_surface;
		struct vmw_mob readback_mob = { 0 };

		surface = create_and_write_shareable_surface(fd1, surface_size);

		ref_surface = vmw_ioctl_surface_ref(fd1, surface->base.handle,
						    DRM_VMW_HANDLE_LEGACY);

		vmw_ioctl_surface_unref(fd1, surface);

		readback_mob.handle = ref_surface.base.handle;
		readback_mob.size = ref_surface.base.buffer_size;
		readback_mob.map_handle = ref_surface.base.buffer_map_handle;

		igt_assert(verify_mob_data(fd1, &readback_mob));
	}

	igt_fixture
	{
		close(fd1);
		close(fd2);
	}
}
