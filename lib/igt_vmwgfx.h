/* SPDX-License-Identifier: GPL-2.0 OR MIT */
/**********************************************************
 * Copyright 2021-2023 VMware, Inc.
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

#ifndef IGT_VMWGFX_H
#define IGT_VMWGFX_H

#include "igt.h"
#include "vmwgfx_drm.h"
#include "lib/svga/svga3d_cmd.h"
#include "lib/svga/svga3d_dx.h"
#include "lib/svga/svga3d_types.h"
#include "lib/svga/vm_basic_types.h"
#include "lib/svga/svga3d_surfacedefs.h"
#include "lib/svga/svga3d_devcaps.h"

#define VMW_EXECBUF_BASE_SIZE 4096
#define VMW_FENCE_TIMEOUT_SECONDS 3600UL
#define SVGA3D_FLAGS_UPPER_32(svga3d_flags) (svga3d_flags >> 32)
#define SVGA3D_FLAGS_LOWER_32(svga3d_flags) \
	(svga3d_flags & ((uint64_t)UINT32_MAX))

struct vmw_bitvector {
	/* Total number of bits */
	uint32 size;
	/* Number of 32-bit elements in array */
	uint32 nwords;
	uint32 *bv;
};

struct vmw_svga_device {
	int32 drm_fd;
	struct vmw_bitvector element_layout_bv;
	struct vmw_bitvector blend_state_bv;
	struct vmw_bitvector depthstencil_state_bv;
	struct vmw_bitvector rasterizer_state_bv;
	struct vmw_bitvector rt_view_bv;
	struct vmw_bitvector ds_view_bv;
	struct vmw_bitvector shader_bv;
};

enum vmw_svga_device_node {
	vmw_svga_device_node_master,
	vmw_svga_device_node_render,
};

/**
 * struct vmw_execbuf
 *
 * @drm_fd: the direct rendering manager file descriptor.
 * @cid: the command id
 * @buffer: the buffer which contains the commands
 * @offset: the offset for the current command
 *
 * A command buffer which contains a series of commands appended
 * one after the other to be submitted.
 */
struct vmw_execbuf {
	int drm_fd;
	int cid;
	char *buffer;
	uint32_t buffer_size;
	uint32_t offset;
};

/**
 * struct vmw_mob
 *
 * @handle: the handle for the mob
 * @map_handle: the handle for mapping
 * @data: the data inside the mob
 * @map_count: how many mappings it has
 * @size: the size of the mob
 *
 * A mob object for holding data
 */
struct vmw_mob {
	uint32_t handle;
	uint64_t map_handle;
	void *data;
	uint32_t map_count;
	uint32_t size;
};

/**
 * struct vmw_surface
 *
 * @base: the surface rep for the buffer ioctl
 * @mob: the mob which hold the data for the buffer
 *
 * A buffer object which takes the buffer and purposes it for a surface
 */
struct vmw_surface {
	struct drm_vmw_gb_surface_create_rep base;
	struct drm_vmw_gb_surface_create_ext_req params;
	struct vmw_mob *mob;
};

struct vmw_vertex {
	float x, y, z, w;
	float r, g, b, a;
};

struct vmw_shader {
	SVGA3dShaderId shid;
	int32 context_id;
	struct vmw_mob *mob;
};

struct vmw_default_objects {
	uint32 context_id;
	SVGA3dElementLayoutId element_layout_id;
	SVGA3dBlendStateId blend_id;
	SVGA3dDepthStencilStateId depthstencil_id;
	SVGA3dRasterizerStateId rasterizer_id;
	SVGA3dRenderTargetViewId color_rt_id;
	struct vmw_surface *color_rt;
	SVGA3dDepthStencilViewId ds_view_id;
	struct vmw_surface *depth_rt;
	struct vmw_shader vertex_shader;
	struct vmw_shader pixel_shader;
	SVGA3dSize rt_size;
};

const SVGA3dSize vmw_default_rect_size = { 400, 400, 1 };

struct vmw_bitvector vmw_bitvector_alloc(uint32 size);

void vmw_bitvector_free(struct vmw_bitvector bitvector);

bool vmw_bitvector_find_next_bit(struct vmw_bitvector bitvector,
				 uint32 *position);

void vmw_bitvector_free_bit(struct vmw_bitvector bitvector, uint32 position);

void vmw_svga_device_init(struct vmw_svga_device *device,
			  enum vmw_svga_device_node device_node);

void vmw_svga_device_fini(struct vmw_svga_device *device);

bool vmw_save_data_as_png(struct vmw_surface *surface, void *data,
			  const char *filename);

void *vmw_surface_data_pixel(struct vmw_surface *surface, uint8 *img_data,
			     uint32 x, uint32 y);

/* IOCTL wrappers */
uint64 vmw_ioctl_get_param(int fd, uint32 param);
void vmw_ioctl_get_3d_cap(int fd, uint64 buffer, uint32 max_size);
struct vmw_mob *vmw_ioctl_mob_create(int fd, uint32_t size);
void vmw_ioctl_mob_close_handle(int fd, struct vmw_mob *mob);
void *vmw_ioctl_mob_map(int fd, struct vmw_mob *mob);
void vmw_ioctl_mob_unmap(struct vmw_mob *mob);

int32 vmw_ioctl_command(int32_t drm_fd, int32_t cid, void *commands,
			uint32_t size, struct drm_vmw_fence_rep *fence);
int vmw_ioctl_fence_finish(int fd, struct drm_vmw_fence_rep *fence);

int vmw_ioctl_syncforcpu(int fd, uint32_t handle, bool dont_block,
			 bool readonly, bool allow_cs);
int vmw_ioctl_releasefromcpu(int fd, uint32_t handle, bool readonly,
			     bool allow_cs);

struct vmw_surface *vmw_ioctl_buffer_create(int fd, SVGA3dSurfaceAllFlags flags,
					    uint32_t size, struct vmw_mob *mob);
void vmw_ioctl_surface_unref(int fd, struct vmw_surface *buffer);

struct vmw_surface vmw_ioctl_surface_ref(int fd, int32 sid, uint32 handle_type);

struct vmw_surface *vmw_ioctl_create_surface_full(
	int fd, SVGA3dSurfaceAllFlags flags, SVGA3dSurfaceFormat format,
	uint32 multisample_count, SVGA3dMSPattern multisample_pattern,
	SVGA3dMSQualityLevel quality_level, SVGA3dTextureFilter autogen_filter,
	uint32 num_mip_levels, uint32 array_size, SVGA3dSize size,
	struct vmw_mob *mob, enum drm_vmw_surface_flags surface_flags);

struct vmw_surface *vmw_create_surface_simple(int fd,
					      SVGA3dSurfaceAllFlags flags,
					      SVGA3dSurfaceFormat format,
					      SVGA3dSize size,
					      struct vmw_mob *mob);

struct vmw_execbuf *vmw_execbuf_create(int drm_fd, int32_t cid);
void vmw_execbuf_set_cid(struct vmw_execbuf *execbuf, int32_t cid);
void vmw_execbuf_destroy(struct vmw_execbuf *execbuf);
int vmw_execbuf_append(struct vmw_execbuf *execbuf, uint32_t cmd_id,
		       const void *cmd_data, uint32_t cmd_size,
		       const void *trailer_data, uint32_t trailer_size);
int32 vmw_execbuf_submit(struct vmw_execbuf *execbuf,
			 struct drm_vmw_fence_rep *fence);

int32 vmw_ioctl_context_create(int drm_fd);
void vmw_ioctl_context_destroy(int drm_fd, int32 cid);

struct vmw_shader vmw_shader_define_and_bind(struct vmw_svga_device *device,
					     struct vmw_execbuf *cmd_buf,
					     SVGA3dShaderType shader_type,
					     uint32 size,
					     const void *shader_text);

void vmw_shader_destroy(struct vmw_svga_device *device,
			struct vmw_execbuf *cmd_buf, struct vmw_shader shader);
void vmw_create_default_objects(struct vmw_svga_device *device,
				int32 context_id,
				struct vmw_default_objects *objects,
				const SVGA3dSize *rt_size);
void vmw_set_default_objects(int drm_fd, struct vmw_default_objects *objects);
void vmw_destroy_default_objects(struct vmw_svga_device *device,
				 struct vmw_default_objects *objects);

void vmw_cmd_set_topology(struct vmw_execbuf *cmd_buf,
			  SVGA3dPrimitiveType topology);

void vmw_cmd_set_vertex_buffers(struct vmw_execbuf *cmd_buf,
				uint32 start_buffer,
				SVGA3dVertexBuffer *buffers,
				uint32 num_buffers);

void vmw_cmd_update_gb_surface(struct vmw_execbuf *cmd_buf,
			       SVGA3dSurfaceId sid);

void vmw_cmd_clear_depthstencil_view(struct vmw_execbuf *cmd_buf, uint16 flags,
				     uint16 stencil,
				     SVGA3dDepthStencilViewId dsvid,
				     float depth);

void vmw_cmd_clear_rendertarget_view(struct vmw_execbuf *cmd_buf,
				     SVGA3dRenderTargetViewId rtvid,
				     SVGA3dRGBAFloat rgba);

void vmw_cmd_draw(struct vmw_execbuf *cmd_buf, uint32 vertex_count,
		  uint32 start_vertex_location);

void vmw_cmd_readback_gb_surface(struct vmw_execbuf *cmd_buf, uint32 sid);

void *vmw_readback_surface(int drm_fd, struct vmw_surface *surface);

void vmw_cmd_surface_copy(struct vmw_execbuf *cmd_buf, SVGA3dSurfaceImageId src,
			  SVGA3dSurfaceImageId dest, const SVGA3dCopyBox *boxes,
			  uint32 num_boxes);

uint8 *vmw_triangle_draw(struct vmw_svga_device *device, int32 cid,
			 struct vmw_default_objects *objects, bool do_sync);

void vmw_triangle_assert_values(uint8 *rendered_img,
				struct vmw_surface *color_rt);

SVGA3dDevCapResult vmw_format_get_caps(int drm_fd,
				       SVGA3dDevCapIndex dev_cap_index);

bool vmw_is_format_supported(int drm_fd, SVGA3dDevCapIndex dev_cap_index);

#endif /* IGT_VMWGFX_H */
