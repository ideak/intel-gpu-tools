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

IGT_TEST_DESCRIPTION("Test memory limits on mob's.");

static void test_triangle_render(struct vmw_svga_device *device, int32 cid)
{
	uint8 *rendered_tri;
	struct vmw_default_objects objects;

	vmw_create_default_objects(device, cid, &objects,
				   &vmw_default_rect_size);
	rendered_tri = vmw_triangle_draw(device, cid, &objects, true);
	vmw_triangle_assert_values(rendered_tri, objects.color_rt);

	free(rendered_tri);
	vmw_destroy_default_objects(device, &objects);
}

igt_main
{
	struct vmw_svga_device device;
	int32 cid;
	uint64 max_mob_mem;
	uint64 max_mob_size;

	igt_fixture
	{
		vmw_svga_device_init(&device, vmw_svga_device_node_render);
		igt_require(device.drm_fd != -1);

		cid = vmw_ioctl_context_create(device.drm_fd);
		igt_require(cid != SVGA3D_INVALID_ID);

		max_mob_mem = vmw_ioctl_get_param(device.drm_fd,
						  DRM_VMW_PARAM_MAX_MOB_MEMORY);
		max_mob_size = vmw_ioctl_get_param(device.drm_fd,
						   DRM_VMW_PARAM_MAX_MOB_SIZE);
	}

	igt_describe("Test whether max memory allocations cause problems.");
	igt_subtest("max_mob_mem_stress")
	{
		uint32 mob_num;
		struct vmw_mob **mob_objs;
		int i;

		mob_num = max_mob_mem / max_mob_size;
		mob_objs = (struct vmw_mob **)calloc(mob_num,
						     sizeof(struct vmw_mob *));

		/* Enough mobs to reach max_mob_mem */
		for (i = 0; i < mob_num; i++) {
			char *readback;

			mob_objs[i] = vmw_ioctl_mob_create(device.drm_fd,
							   max_mob_size);
			/* Writing mob to ensure it gets created */
			readback =
				vmw_ioctl_mob_map(device.drm_fd, mob_objs[i]);
			memset(readback, 0, mob_objs[i]->size);
			vmw_ioctl_mob_unmap(mob_objs[i]);
		}

		test_triangle_render(&device, cid);

		for (i = 0; i < mob_num; i++)
			vmw_ioctl_mob_close_handle(device.drm_fd, mob_objs[i]);
		free(mob_objs);
	}

	igt_fixture
	{
		vmw_ioctl_context_destroy(device.drm_fd, cid);
		vmw_svga_device_fini(&device);
	}
}
