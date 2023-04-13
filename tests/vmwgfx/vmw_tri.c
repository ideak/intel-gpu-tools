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

IGT_TEST_DESCRIPTION("Check whether basic 3D pipeline works correctly.");

static void draw_triangle(struct vmw_svga_device *device, int32 cid)
{
	struct vmw_default_objects objects;
	uint8 *rendered_img;
	bool save_status;

	vmw_create_default_objects(device, cid, &objects,
				   &vmw_default_rect_size);
	rendered_img = vmw_triangle_draw(device, cid, &objects, true);

	save_status = vmw_save_data_as_png(objects.color_rt, rendered_img,
					   "vmw_tri.png");
	igt_assert(save_status);

	vmw_triangle_assert_values(rendered_img, objects.color_rt);

	/* Clean up */
	free(rendered_img);
	vmw_destroy_default_objects(device, &objects);
}

static void replace_with_coherent_rt(struct vmw_svga_device *device,
				     int32 context_id,
				     struct vmw_default_objects *objects,
				     const SVGA3dSize *rt_size)
{
	struct vmw_execbuf *cmd_buf;
	SVGA3dRenderTargetViewDesc rtv_desc = { 0 };
	SVGA3dCmdDXDefineRenderTargetView rt_view_define_cmd = { 0 };
	SVGA3dCmdDXDefineDepthStencilView ds_view_define_cmd = { 0 };

	objects->color_rt = vmw_ioctl_create_surface_full(
		device->drm_fd,
		SVGA3D_SURFACE_HINT_TEXTURE | SVGA3D_SURFACE_HINT_RENDERTARGET |
			SVGA3D_SURFACE_BIND_RENDER_TARGET,
		SVGA3D_R8G8B8A8_UNORM, 0, SVGA3D_MS_PATTERN_NONE,
		SVGA3D_MS_QUALITY_NONE, SVGA3D_TEX_FILTER_NONE, 1, 1, *rt_size,
		NULL, drm_vmw_surface_flag_coherent);

	objects->depth_rt = vmw_ioctl_create_surface_full(
		device->drm_fd,
		SVGA3D_SURFACE_HINT_DEPTHSTENCIL |
			SVGA3D_SURFACE_HINT_RENDERTARGET |
			SVGA3D_SURFACE_BIND_DEPTH_STENCIL,
		SVGA3D_R24G8_TYPELESS, 0, SVGA3D_MS_PATTERN_NONE,
		SVGA3D_MS_QUALITY_NONE, SVGA3D_TEX_FILTER_NONE, 1, 1, *rt_size,
		NULL, drm_vmw_surface_flag_coherent);

	cmd_buf = vmw_execbuf_create(device->drm_fd, context_id);

	rtv_desc.tex.arraySize = 1;
	rtv_desc.tex.firstArraySlice = 0;
	rtv_desc.tex.mipSlice = 0;
	vmw_bitvector_find_next_bit(device->rt_view_bv,
				    &rt_view_define_cmd.renderTargetViewId);
	rt_view_define_cmd.sid = objects->color_rt->base.handle;
	rt_view_define_cmd.format = SVGA3D_R8G8B8A8_UNORM;
	rt_view_define_cmd.resourceDimension = SVGA3D_RESOURCE_TEXTURE2D;
	rt_view_define_cmd.desc = rtv_desc;
	vmw_execbuf_append(cmd_buf, SVGA_3D_CMD_DX_DEFINE_RENDERTARGET_VIEW,
			   &rt_view_define_cmd, sizeof(rt_view_define_cmd), NULL,
			   0);
	objects->color_rt_id = rt_view_define_cmd.renderTargetViewId;

	vmw_bitvector_find_next_bit(device->ds_view_bv,
				    &ds_view_define_cmd.depthStencilViewId);
	ds_view_define_cmd.sid = objects->depth_rt->base.handle;
	ds_view_define_cmd.format = SVGA3D_D24_UNORM_S8_UINT;
	ds_view_define_cmd.resourceDimension = SVGA3D_RESOURCE_TEXTURE2D;
	ds_view_define_cmd.mipSlice = 0;
	ds_view_define_cmd.firstArraySlice = 0;
	ds_view_define_cmd.arraySize = 1;
	vmw_execbuf_append(cmd_buf, SVGA_3D_CMD_DX_DEFINE_DEPTHSTENCIL_VIEW,
			   &ds_view_define_cmd, sizeof(ds_view_define_cmd), NULL,
			   0);
	objects->ds_view_id = ds_view_define_cmd.depthStencilViewId;

	vmw_execbuf_submit(cmd_buf, NULL);
	vmw_execbuf_destroy(cmd_buf);
}

static void destroy_rt(struct vmw_svga_device *device, int32 context_id,
		       struct vmw_default_objects *objects)
{
	struct vmw_execbuf *cmd_buf;

	SVGA3dCmdDXDestroyRenderTargetView rt_view_cmd = {
		.renderTargetViewId = objects->color_rt_id
	};

	SVGA3dCmdDXDestroyDepthStencilView ds_view_cmd = {
		.depthStencilViewId = objects->ds_view_id
	};

	cmd_buf = vmw_execbuf_create(device->drm_fd, objects->context_id);

	vmw_execbuf_append(cmd_buf, SVGA_3D_CMD_DX_DESTROY_RENDERTARGET_VIEW,
			   &rt_view_cmd, sizeof(rt_view_cmd), NULL, 0);

	vmw_execbuf_append(cmd_buf, SVGA_3D_CMD_DX_DESTROY_DEPTHSTENCIL_VIEW,
			   &ds_view_cmd, sizeof(ds_view_cmd), NULL, 0);

	vmw_ioctl_surface_unref(device->drm_fd, objects->color_rt);
	vmw_ioctl_surface_unref(device->drm_fd, objects->depth_rt);

	vmw_bitvector_free_bit(device->rt_view_bv, objects->color_rt_id);
	vmw_bitvector_free_bit(device->ds_view_bv, objects->ds_view_id);

	vmw_execbuf_submit(cmd_buf, NULL);
	vmw_execbuf_destroy(cmd_buf);
}

static void draw_triangle_on_coherent_rt(struct vmw_svga_device *device,
					 int32 cid)
{
	struct vmw_default_objects objects;
	uint8 *rendered_img;
	struct vmw_surface *default_color_rt;
	struct vmw_surface *default_depth_rt;
	SVGA3dRenderTargetViewId default_color_rt_id;
	SVGA3dDepthStencilViewId default_ds_view_id;

	vmw_create_default_objects(device, cid, &objects,
				   &vmw_default_rect_size);

	/* Replace default rendertargets with coherent equivalents */
	default_color_rt = objects.color_rt;
	default_depth_rt = objects.depth_rt;
	default_color_rt_id = objects.color_rt_id;
	default_ds_view_id = objects.ds_view_id;
	replace_with_coherent_rt(device, cid, &objects, &vmw_default_rect_size);

	rendered_img = vmw_triangle_draw(device, cid, &objects, false);

	vmw_triangle_assert_values(rendered_img, objects.color_rt);

	/* Clean up */
	free(rendered_img);

	destroy_rt(device, cid, &objects);
	objects.color_rt = default_color_rt;
	objects.depth_rt = default_depth_rt;
	objects.color_rt_id = default_color_rt_id;
	objects.ds_view_id = default_ds_view_id;

	vmw_destroy_default_objects(device, &objects);
}

igt_main
{
	struct vmw_svga_device device;
	int32 cid;

	igt_fixture
	{
		vmw_svga_device_init(&device, vmw_svga_device_node_render);
		igt_require(device.drm_fd != -1);

		cid = vmw_ioctl_context_create(device.drm_fd);
		igt_require(cid != SVGA3D_INVALID_ID);
	}

	igt_describe("Tests rendering of a trivial triangle.");
	igt_subtest("tri")
	{
		draw_triangle(&device, cid);
	}

	/*
	 * Check that vmwgfx correctly handles coherent rendertarget
	 * surfaces when no explicit sync is given from userspace
	 */
	igt_describe("Tests rendering of a triangle with coherency.");
	igt_subtest("tri-no-sync-coherent")
	{
		draw_triangle_on_coherent_rt(&device, cid);
	}

	igt_fixture
	{
		vmw_ioctl_context_destroy(device.drm_fd, cid);
		vmw_svga_device_fini(&device);
	}
}
