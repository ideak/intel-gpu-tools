// SPDX-License-Identifier: GPL-2.0 OR MIT
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

#include "igt_vmwgfx.h"

/**
 * SECTION:igt_vmwgfx
 * @short_description: VMWGFX support library
 * @title: VMWGFX
 * @include: igt.h
 *
 * This library provides various auxiliary helper functions for writing VMWGFX
 * tests.
 */

#define VMW_INTEGRAL_BITSIZE (sizeof(*((struct vmw_bitvector *)0)->bv) * 8)

/*
 * Default Shaders
 */
static const uint32 SVGADXPixelShader[] = {
	0x40, 0xe,	 0x3001062, 0x1010f2, 0x1,	0x3000065, 0x1020f2,
	0x0,  0x5000036, 0x1020f2,  0x0,      0x101e46, 0x1,	   0x100003e,
};
static const uint32 SVGADXVertexShader[] = {
	0x10040,    0x1f,      0x300005f, 0x101072,  0x0,      0x300005f,
	0x1010f2,   0x1,       0x4000067, 0x1020f2,  0x0,      0x1,
	0x3000065,  0x1020f2,  0x1,	  0x5000036, 0x102072, 0x0,
	0x101246,   0x0,       0x5000036, 0x102082,  0x0,      0x4001,
	0x3f800000, 0x5000036, 0x1020f2,  0x1,	     0x101e46, 0x1,
	0x100003e,
};

struct vmw_bitvector vmw_bitvector_alloc(uint32 size)
{
	struct vmw_bitvector bitvector;
	uint32 nwords;
	uint32 *bv;

	nwords = (size - 1) / VMW_INTEGRAL_BITSIZE + 1;
	bv = calloc(nwords, sizeof(uint32));

	bitvector.size = size;
	bitvector.nwords = nwords;
	bitvector.bv = bv;
	return bitvector;
}

void vmw_bitvector_free(struct vmw_bitvector bitvector)
{
	free(bitvector.bv);
}

bool vmw_bitvector_find_next_bit(struct vmw_bitvector bitvector,
				 uint32 *position)
{
	uint32 index = 0;
	uint32 curr_word = 0;
	uint32 bit_index = 0;

	for (curr_word = 0; curr_word < bitvector.nwords; curr_word++) {
		if (bitvector.bv[curr_word] != UINT32_MAX) {
			for (bit_index = 0; index < bitvector.size;
			     index++, bit_index++) {
				uint32 bitmask = 1 << bit_index;

				if ((bitmask & bitvector.bv[curr_word]) == 0) {
					bitvector.bv[curr_word] |= bitmask;
					*position = index;
					return true;
				}
			}
			return false;
		} else {
			index += VMW_INTEGRAL_BITSIZE;
		}
	}
	return false;
}

void vmw_bitvector_free_bit(struct vmw_bitvector bitvector, uint32 position)
{
	uint32 curr_word = position / VMW_INTEGRAL_BITSIZE;
	uint32 bit_index = position % VMW_INTEGRAL_BITSIZE;
	uint32 bitmask = ~(1 << bit_index);

	bitvector.bv[curr_word] &= bitmask;
}

void vmw_svga_device_init(struct vmw_svga_device *device,
			  enum vmw_svga_device_node device_node)
{
	if (device_node == vmw_svga_device_node_master)
		device->drm_fd = drm_open_driver_master(DRIVER_VMWGFX);
	else
		device->drm_fd = drm_open_driver_render(DRIVER_VMWGFX);
	device->element_layout_bv = vmw_bitvector_alloc(50);
	device->blend_state_bv = vmw_bitvector_alloc(50);
	device->depthstencil_state_bv = vmw_bitvector_alloc(20);
	device->rasterizer_state_bv = vmw_bitvector_alloc(50);
	device->rt_view_bv = vmw_bitvector_alloc(500);
	device->ds_view_bv = vmw_bitvector_alloc(10);
	device->shader_bv = vmw_bitvector_alloc(500);
}

void vmw_svga_device_fini(struct vmw_svga_device *device)
{
	vmw_bitvector_free(device->element_layout_bv);
	vmw_bitvector_free(device->blend_state_bv);
	vmw_bitvector_free(device->depthstencil_state_bv);
	vmw_bitvector_free(device->rasterizer_state_bv);
	vmw_bitvector_free(device->rt_view_bv);
	vmw_bitvector_free(device->ds_view_bv);
	vmw_bitvector_free(device->shader_bv);
	close(device->drm_fd);
}

bool vmw_save_data_as_png(struct vmw_surface *surface, void *data,
			  const char *filename)
{
	cairo_surface_t *cairo_surface;
	cairo_status_t ret;
	uint32 width = surface->params.base.base_size.width;
	uint32 height = surface->params.base.base_size.height;
	uint32 pixel_size =
		g_SVGA3dSurfaceDescs[surface->params.base.format].bytesPerBlock;
	uint32 stride;
	cairo_format_t format;

	stride = pixel_size * width;
	/* Can separate this into another function as it grows */
	switch (surface->params.base.format) {
	case SVGA3D_R8G8B8A8_UNORM:
		format = CAIRO_FORMAT_ARGB32;
		break;
	default:
		format = CAIRO_FORMAT_INVALID;
		break;
	}

	cairo_surface = cairo_image_surface_create_for_data(
		(uint8 *)data, format, width, height, stride);
	ret = cairo_surface_write_to_png(cairo_surface, filename);
	cairo_surface_destroy(cairo_surface);
	return (ret == CAIRO_STATUS_SUCCESS);
}

void *vmw_surface_data_pixel(struct vmw_surface *surface, uint8 *img_data,
			     uint32 x, uint32 y)
{
	uint32 width = surface->params.base.base_size.width;
	uint32 pixel_size =
		g_SVGA3dSurfaceDescs[surface->params.base.format].bytesPerBlock;

	return &img_data[y * width * pixel_size + x * pixel_size];
}

uint64 vmw_ioctl_get_param(int fd, uint32 param)
{
	struct drm_vmw_getparam_arg arg = { 0 };
	int ret;

	arg.param = param;

	do {
		ret = drmCommandWriteRead(fd, DRM_VMW_GET_PARAM, &arg,
					  sizeof(arg));
	} while (ret == -ERESTART);
	if (ret)
		fprintf(stderr, "IOCTL failed %d: %s\n", ret, strerror(-ret));
	return arg.value;
}

void vmw_ioctl_get_3d_cap(int fd, uint64 buffer, uint32 max_size)
{
	struct drm_vmw_get_3d_cap_arg arg = { 0 };
	int ret;

	arg.buffer = buffer;
	arg.max_size = max_size;

	do {
		ret = drmCommandWrite(fd, DRM_VMW_GET_3D_CAP, &arg,
				      sizeof(arg));
	} while (ret == -ERESTART);
	if (ret)
		fprintf(stderr, "IOCTL failed %d: %s\n", ret, strerror(-ret));
}

/**
 * vmw_ioctl_fence_finish
 *
 * @fence: the fence report for the fence ioctl
 * @fd: the driver file descriptor
 *
 * fills out the arguments for the fence wait ioctl and then waits until
 * the fence finishes, then checks if the fence has failed or succeeds and
 * returns that value.
 */
int vmw_ioctl_fence_finish(int fd, struct drm_vmw_fence_rep *fence)
{
	struct drm_vmw_fence_wait_arg arg = { 0 };
	int ret;

	arg.handle = fence->handle;
	arg.timeout_us = VMW_FENCE_TIMEOUT_SECONDS * 1000000;
	arg.flags = fence->mask;

	ret = drmCommandWriteRead(fd, DRM_VMW_FENCE_WAIT, &arg, sizeof(arg));

	if (ret != 0)
		fprintf(stderr, "%s Failed\n", __func__);

	return ret;
}

/**
 * vmw_ioctl_command
 *
 * @fence: the fence report for the fence ioctl
 * @fd: the driver file descriptor
 *
 * fills out the arguments for the fence wait ioctl and then waits until
 * the fence finishes, returns 0 if fence has succeeded, 1 otherwise.
 */
int32 vmw_ioctl_command(int drm_fd, int32_t cid, void *commands, uint32_t size,
			struct drm_vmw_fence_rep *fence)
{
	struct drm_vmw_execbuf_arg arg = { 0 };
	int ret;
	const int argsize = sizeof(arg);

	memset(&arg, 0, sizeof(arg));

	arg.fence_rep = (unsigned long)fence;
	arg.commands = (unsigned long)commands;
	arg.command_size = size;
	arg.throttle_us = 0; /* deprecated */
	arg.version = DRM_VMW_EXECBUF_VERSION;
	arg.context_handle = cid;

	do {
		ret = drmCommandWrite(drm_fd, DRM_VMW_EXECBUF, &arg, argsize);
		if (ret == -EBUSY)
			usleep(1000);
	} while (ret == -ERESTART || ret == -EBUSY);
	if (ret) {
		igt_info("%s error %s.\n", __func__, strerror(-ret));
		return 1;
	}
	return 0;
}

/**
 * vmw_ioctl_mob_create
 *
 * @fd: the driver file descriptor
 * @size: the size of the mob
 *
 * Creates a new mob using the fd of the size inputed as
 * an argument, calling the mob create ioctl to form a new
 * mob
 */
struct vmw_mob *vmw_ioctl_mob_create(int fd, uint32_t size)
{
	struct vmw_mob *mob;
	union drm_vmw_alloc_dmabuf_arg arg;
	struct drm_vmw_alloc_dmabuf_req *req = &arg.req;
	struct drm_vmw_dmabuf_rep *rep = &arg.rep;
	int ret;

	mob = calloc(1, sizeof(struct vmw_mob));
	if (!mob)
		goto out_err1;

	memset(&arg, 0, sizeof(arg));
	req->size = size;
	do {
		ret = drmCommandWriteRead(fd, DRM_VMW_ALLOC_DMABUF, &arg,
					  sizeof(arg));
	} while (ret == -ERESTART);

	if (ret) {
		fprintf(stderr, "IOCTL failed %d: %s\n", ret, strerror(-ret));
		goto out_err1;
	}

	mob->data = NULL;
	mob->handle = rep->handle;
	mob->map_handle = rep->map_handle;
	mob->map_count = 0;
	mob->size = size;

	return mob;

out_err1:
	free(mob);
	return NULL;
}

/**
 * vmw_ioctl_mob_close_handle
 *
 * @mob: the mob to be unreferenced
 * @fd: the driver file descriptor
 *
 * Closes the user-space handle of the mob.
 */
void vmw_ioctl_mob_close_handle(int fd, struct vmw_mob *mob)
{
	struct drm_vmw_handle_close_arg arg;

	if (mob->data) {
		munmap(mob->data, mob->size);
		mob->data = NULL;
	}

	memset(&arg, 0, sizeof(arg));
	arg.handle = mob->handle;
	drmCommandWrite(fd, DRM_VMW_HANDLE_CLOSE, &arg, sizeof(arg));

	free(mob);
}

struct vmw_surface vmw_ioctl_surface_ref(int fd, int32 sid, uint32 handle_type)
{
	int ret;
	union drm_vmw_gb_surface_reference_ext_arg arg;
	struct vmw_surface surface;

	arg.req.handle_type = handle_type;
	arg.req.sid = sid;

	ret = drmCommandWriteRead(fd, DRM_VMW_GB_SURFACE_REF_EXT, &arg,
				  sizeof(arg));
	if (ret != 0)
		fprintf(stderr, "%s Failed\n", __func__);

	surface.base = arg.rep.crep;
	surface.params = arg.rep.creq;
	return surface;
}

/**
 * vmw_ioctl_mob_map
 *
 * @mob: the mob to be mapped
 * @fd: the driver file descriptor
 *
 * Maps an existing mob and increments the mob mapping counter
 */
void *vmw_ioctl_mob_map(int fd, struct vmw_mob *mob)
{
	void *map;

	if (mob->data == NULL) {
		map = mmap(NULL, mob->size, PROT_READ | PROT_WRITE, MAP_SHARED,
			   fd, mob->map_handle);
		if (map == MAP_FAILED) {
			fprintf(stderr, "%s: Map failed.\n", __func__);
			return NULL;
		}

		// MADV_HUGEPAGE only exists on Linux
#ifdef MADV_HUGEPAGE
		(void)madvise(map, mob->size, MADV_HUGEPAGE);
#endif
		mob->data = map;
	}

	++mob->map_count;

	return mob->data;
}

/**
 * vmw_ioctl_mob_unmap
 *
 * @mob: the mob to be mapped
 *
 * Unmaps the existing mob and decrements the mob mapping counter
 */
void vmw_ioctl_mob_unmap(struct vmw_mob *mob)
{
	--mob->map_count;
	munmap(mob->data, mob->size);
	mob->data = NULL;
}

/**
 * vmw_ioctl_buffer_create
 *
 * @flags: SVGA3D flags which define what the buffer will be used for
 * @size: the size of the buffer
 * @mob: the mob to be mapped
 * @fd: the driver file descriptor
 *
 * Uses the flags and takes in a mob to create a buffer of a predetermined size.
 * A surface buffer is created by calling the surface create ioctl.
 */
struct vmw_surface *vmw_ioctl_buffer_create(int fd, SVGA3dSurfaceAllFlags flags,
					    uint32_t size, struct vmw_mob *mob)
{
	SVGA3dSize surface_size = { .width = size, .height = 1, .depth = 1 };

	return vmw_create_surface_simple(fd, flags, SVGA3D_BUFFER, surface_size,
					 mob);
}

/**
 * vmw_ioctl_surface_unref
 *
 * @surface: the surface to be ureferenced
 * @fd: the driver file descriptor
 *
 * Unreferences the surface.
 */
void vmw_ioctl_surface_unref(int fd, struct vmw_surface *surface)
{
	struct drm_vmw_surface_arg s_arg;

	memset(&s_arg, 0, sizeof(s_arg));
	s_arg.sid = surface->base.handle;

	(void)drmCommandWrite(fd, DRM_VMW_UNREF_SURFACE, &s_arg, sizeof(s_arg));
	free(surface);
}

struct vmw_surface *vmw_ioctl_create_surface_full(
	int fd, SVGA3dSurfaceAllFlags flags, SVGA3dSurfaceFormat format,
	uint32 multisample_count, SVGA3dMSPattern multisample_pattern,
	SVGA3dMSQualityLevel quality_level, SVGA3dTextureFilter autogen_filter,
	uint32 num_mip_levels, uint32 array_size, SVGA3dSize size,
	struct vmw_mob *mob, enum drm_vmw_surface_flags surface_flags)
{
	struct vmw_surface *surface;
	int32 ret;
	union drm_vmw_gb_surface_create_ext_arg arg = { 0 };

	surface = calloc(1, sizeof(struct vmw_surface));
	if (!surface)
		goto out_err1;

	arg.req.base.base_size.width = size.width;
	arg.req.base.base_size.height = size.height;
	arg.req.base.base_size.depth = size.depth;
	arg.req.base.array_size = array_size;
	arg.req.base.autogen_filter = autogen_filter;
	arg.req.base.drm_surface_flags |= surface_flags;
	if (mob) {
		arg.req.base.buffer_handle = mob->handle;
	} else {
		arg.req.base.buffer_handle = SVGA3D_INVALID_ID;
		arg.req.base.drm_surface_flags |=
			drm_vmw_surface_flag_create_buffer;
	}
	arg.req.base.format = format;
	arg.req.base.mip_levels = num_mip_levels;
	arg.req.base.multisample_count = multisample_count;
	arg.req.base.svga3d_flags = SVGA3D_FLAGS_LOWER_32(flags);
	arg.req.svga3d_flags_upper_32_bits = SVGA3D_FLAGS_UPPER_32(flags);
	arg.req.multisample_pattern = multisample_pattern;
	arg.req.quality_level = quality_level;
	arg.req.version = drm_vmw_gb_surface_v1;

	surface->params = arg.req;

	do {
		ret = drmCommandWriteRead(fd, DRM_VMW_GB_SURFACE_CREATE_EXT,
					  &arg, sizeof(arg));
	} while (ret == -ERESTART);

	if (ret) {
		fprintf(stderr, "IOCTL failed %d: %s\n", ret, strerror(-ret));
		goto out_err1;
	}

	surface->base = arg.rep;
	surface->mob = mob;
	return surface;

out_err1:
	free(surface);
	return NULL;
}

struct vmw_surface *vmw_create_surface_simple(int fd,
					      SVGA3dSurfaceAllFlags flags,
					      SVGA3dSurfaceFormat format,
					      SVGA3dSize size,
					      struct vmw_mob *mob)
{
	/*
	 * TODO:
	 * Should check flag for SVGA3D_SURFACE_MULTISAMPLE and generate
	 * Assuming no multisampling for now.
	 */
	uint32 multisample_count = 0;
	SVGA3dMSPattern multisample_pattern = SVGA3D_MS_PATTERN_NONE;
	SVGA3dMSQualityLevel quality_level = SVGA3D_MS_QUALITY_NONE;
	uint32 array_size;

	array_size = (flags & SVGA3D_SURFACE_CUBEMAP) != 0 ?
			     SVGA3D_MAX_SURFACE_FACES :
			     1;

	return vmw_ioctl_create_surface_full(fd, flags, format,
					     multisample_count,
					     multisample_pattern, quality_level,
					     SVGA3D_TEX_FILTER_NONE, 1,
					     array_size, size, mob, 0);
}

/**
 * vmw_ioctl_syncforcpu
 *
 * @handle: the handle for the sync
 * @dont_block: defines whether or not to block
 * @readonly: defines whether or not it is read only
 * @allow_cs: defines whether or not to allow cs
 * @fd: the driver file descriptor
 *
 * Sets the arguments, including the handle and the flags and
 * then calls an ioctl to sync with the cpu
 */
int vmw_ioctl_syncforcpu(int fd, uint32_t handle, bool dont_block,
			 bool readonly, bool allow_cs)
{
	struct drm_vmw_synccpu_arg arg;
	int ret;

	memset(&arg, 0, sizeof(arg));
	arg.op = drm_vmw_synccpu_grab;
	arg.handle = handle;
	arg.flags = drm_vmw_synccpu_read;
	if (!readonly)
		arg.flags |= drm_vmw_synccpu_write;
	if (dont_block)
		arg.flags |= drm_vmw_synccpu_dontblock;
	if (allow_cs)
		arg.flags |= drm_vmw_synccpu_allow_cs;

	ret = drmCommandWrite(fd, DRM_VMW_SYNCCPU, &arg, sizeof(arg));
	if (ret) {
		fprintf(stderr, "%s failed %d: %s\n", __func__, ret,
			strerror(-ret));
	}
	return ret;
}

/**
 * vmw_ioctl_releasefromcpu
 *
 * @handle: the handle for the sync
 * @readonly: defines whether or not it is read only
 * @allow_cs: defines whether or not to allow cs
 * @fd: the driver file descriptor
 *
 * Sets the arguments, including the handle and the flags and
 * then calls an ioctl to release from cpu
 */
int vmw_ioctl_releasefromcpu(int fd, uint32_t handle, bool readonly,
			     bool allow_cs)
{
	struct drm_vmw_synccpu_arg arg;
	int ret;

	memset(&arg, 0, sizeof(arg));
	arg.op = drm_vmw_synccpu_release;
	arg.handle = handle;
	arg.flags = drm_vmw_synccpu_read;
	if (!readonly)
		arg.flags |= drm_vmw_synccpu_write;
	if (allow_cs)
		arg.flags |= drm_vmw_synccpu_allow_cs;

	ret = drmCommandWrite(fd, DRM_VMW_SYNCCPU, &arg, sizeof(arg));
	if (ret) {
		fprintf(stderr, "%s failed %d: %s\n", __func__, ret,
			strerror(-ret));
	}
	return ret;
}

/**
 * vmw_execbuf_create
 *
 * @drm_fd: the direct rendering manager file descriptor
 * @cid: the context id
 *
 * Creates a new execution buffer for execution commands
 */
struct vmw_execbuf *vmw_execbuf_create(int drm_fd, int32_t cid)
{
	struct vmw_execbuf *command_buffer = malloc(sizeof(struct vmw_execbuf));

	command_buffer->drm_fd = drm_fd;
	command_buffer->cid = cid;
	command_buffer->buffer = malloc(VMW_EXECBUF_BASE_SIZE);
	command_buffer->buffer_size = VMW_EXECBUF_BASE_SIZE;
	command_buffer->offset = 0;

	return command_buffer;
}

/**
 * vmw_execbuf_set_cid
 *
 * @cid: the command buffer id
 *
 * Sets the execution buffers cid
 */
void vmw_execbuf_set_cid(struct vmw_execbuf *execbuf, int32_t cid)
{
	execbuf->cid = cid;
}

/**
 * vmw_execbuf_destroy
 *
 * @execbuf: the execution buffer to be destroyed
 *
 * Destroys the execution buffer
 */
void vmw_execbuf_destroy(struct vmw_execbuf *execbuf)
{
	memset(execbuf->buffer, 0, execbuf->buffer_size);

	free(execbuf->buffer);

	execbuf->drm_fd = 0;
	execbuf->cid = 0;
	execbuf->buffer_size = 0;
	execbuf->offset = 0;

	free(execbuf);
}

/**
 * vmw_execbuf_append
 *
 * @execbuf: the execution buffer
 * @cid: the command buffer id
 * @cmdId: the command ID
 * @cmdData: the command data
 * @cmdSize: the size of the commands
 * @trailerData: the trailer data
 * @trailerSize: the size of the trailer
 * @fd: the driver file descriptor
 *
 * Appends the header, command data, and trailer data.
 * Reallocates the buffer if the command data exceeds the buffer size.
 * Changes the offset based on the data appended.
 */
int vmw_execbuf_append(struct vmw_execbuf *execbuf, uint32_t cmd_id,
		       const void *cmd_data, uint32_t cmd_size,
		       const void *trailer_data, uint32_t trailer_size)
{
	SVGA3dCmdHeader header;
	uint32_t length;
	uint32_t offset;

	header.id = cmd_id;
	header.size = cmd_size + trailer_size;

	length = sizeof(header) + cmd_size + trailer_size;

	if (length > (execbuf->buffer_size - execbuf->offset)) {
		int increase_size =
			length - (execbuf->buffer_size - execbuf->offset);
		execbuf->buffer_size +=
			ALIGN(increase_size, VMW_EXECBUF_BASE_SIZE);
		execbuf->buffer =
			realloc(execbuf->buffer, execbuf->buffer_size);
	}

	offset = execbuf->offset;
	memcpy(execbuf->buffer + offset, &header, sizeof(header));
	offset += sizeof(header);
	memcpy(execbuf->buffer + offset, cmd_data, cmd_size);
	offset += cmd_size;
	if (trailer_size) {
		memcpy(execbuf->buffer + offset, trailer_data, trailer_size);
		offset += trailer_size;
	}
	execbuf->offset = offset;

	return offset;
}

/**
 * vmw_execbuf_submit
 *
 * @execbuf: the execution buffer
 * @fence: the vmw fence response
 *
 * Submits the commands from the buffer and updates the fence response
 */
int32 vmw_execbuf_submit(struct vmw_execbuf *execbuf,
			 struct drm_vmw_fence_rep *fence)
{
	uint32_t size = execbuf->offset;
	int32 ret;

	assert(execbuf->offset > 0);
	assert(execbuf->offset <= execbuf->buffer_size);

	ret = vmw_ioctl_command(execbuf->drm_fd, execbuf->cid, execbuf->buffer,
				size, fence);
	execbuf->offset = 0;
	return ret;
}

int32 vmw_ioctl_context_create(int drm_fd)
{
	int ret;
	union drm_vmw_extended_context_arg arg = { 0 };

	arg.req = drm_vmw_context_dx;

	do {
		ret = drmCommandWriteRead(drm_fd,
					  DRM_VMW_CREATE_EXTENDED_CONTEXT, &arg,
					  sizeof(arg));
	} while (ret == -ERESTART);

	if (ret) {
		fprintf(stderr, "%s failed %d: %s\n", __func__, ret,
			strerror(-ret));
		return SVGA3D_INVALID_ID;
	}
	return arg.rep.cid;
}

void vmw_ioctl_context_destroy(int drm_fd, int32 cid)
{
	struct drm_vmw_context_arg c_arg;

	memset(&c_arg, 0, sizeof(c_arg));
	c_arg.cid = cid;

	(void)drmCommandWrite(drm_fd, DRM_VMW_UNREF_CONTEXT, &c_arg,
			      sizeof(c_arg));
}

struct vmw_shader vmw_shader_define_and_bind(struct vmw_svga_device *device,
					     struct vmw_execbuf *cmd_buf,
					     SVGA3dShaderType shader_type,
					     uint32 size,
					     const void *shader_text)
{
	struct vmw_shader shader;
	struct vmw_mob *shader_mob;
	SVGA3dShaderId shader_id;
	void *data;

	SVGA3dCmdDXDefineShader define_cmd = { 0 };
	SVGA3dCmdDXBindShader bind_cmd = { 0 };

	shader_mob = vmw_ioctl_mob_create(cmd_buf->drm_fd, size);
	data = vmw_ioctl_mob_map(cmd_buf->drm_fd, shader_mob);
	memcpy(data, shader_text, size);
	vmw_ioctl_mob_unmap(shader_mob);

	vmw_bitvector_find_next_bit(device->shader_bv, &shader_id);

	define_cmd.shaderId = shader_id;
	define_cmd.sizeInBytes = size;
	define_cmd.type = shader_type;
	vmw_execbuf_append(cmd_buf, SVGA_3D_CMD_DX_DEFINE_SHADER, &define_cmd,
			   sizeof(define_cmd), NULL, 0);

	bind_cmd.cid = cmd_buf->cid;
	bind_cmd.shid = shader_id;
	bind_cmd.mobid = shader_mob->handle;
	bind_cmd.offsetInBytes = 0;
	vmw_execbuf_append(cmd_buf, SVGA_3D_CMD_DX_BIND_SHADER, &bind_cmd,
			   sizeof(bind_cmd), NULL, 0);

	shader.shid = shader_id;
	shader.context_id = cmd_buf->cid;
	shader.mob = shader_mob;
	return shader;
}

void vmw_shader_destroy(struct vmw_svga_device *device,
			struct vmw_execbuf *cmd_buf, struct vmw_shader shader)
{
	SVGA3dCmdDXDestroyShader destroy_cmd = { .shaderId = shader.shid };

	vmw_execbuf_append(cmd_buf, SVGA_3D_CMD_DX_DESTROY_SHADER, &destroy_cmd,
			   sizeof(destroy_cmd), NULL, 0);
	vmw_ioctl_mob_close_handle(cmd_buf->drm_fd, shader.mob);
	vmw_bitvector_free_bit(device->shader_bv, shader.shid);
}

void vmw_create_default_objects(struct vmw_svga_device *device,
				int32 context_id,
				struct vmw_default_objects *objects,
				const SVGA3dSize *rt_size)
{
	uint32 i = 0;
	struct vmw_execbuf *cmd_buf;
	struct drm_vmw_fence_rep cmd_fence = { 0 };

	SVGA3dInputElementDesc input_elements[] = {
		{ 0, 0, SVGA3D_R32G32B32A32_FLOAT, SVGA3D_INPUT_PER_VERTEX_DATA,
		  0, 0 },
		{ 0, offsetof(struct vmw_vertex, r), SVGA3D_R32G32B32A32_FLOAT,
		  SVGA3D_INPUT_PER_VERTEX_DATA, 0, 1 },
	};
	uint32 input_element_count = ARRAY_SIZE(input_elements);

	SVGA3dCmdDXDefineElementLayout element_layout_cmd = { 0 };
	SVGA3dDXBlendStatePerRT rt_blend_state = { 0 };
	SVGA3dCmdDXDefineBlendState blend_cmd = { 0 };
	SVGA3dCmdDXDefineDepthStencilState depthstencil_cmd = { 0 };
	SVGA3dCmdDXDefineRasterizerState rasterizer_cmd = { 0 };
	SVGA3dRenderTargetViewDesc rtv_desc = { 0 };
	SVGA3dCmdDXDefineRenderTargetView rt_view_cmd = { 0 };
	SVGA3dCmdDXDefineDepthStencilView ds_view_cmd = { 0 };

	objects->context_id = context_id;

	cmd_buf = vmw_execbuf_create(device->drm_fd, context_id);

	vmw_bitvector_find_next_bit(device->element_layout_bv,
				    &element_layout_cmd.elementLayoutId);
	vmw_execbuf_append(
		cmd_buf, SVGA_3D_CMD_DX_DEFINE_ELEMENTLAYOUT,
		&element_layout_cmd, sizeof(element_layout_cmd), &input_elements,
		input_element_count * sizeof(SVGA3dInputElementDesc));
	objects->element_layout_id = element_layout_cmd.elementLayoutId;

	rt_blend_state.renderTargetWriteMask = 0x0F;
	rt_blend_state.blendEnable = false;
	rt_blend_state.srcBlend = SVGA3D_BLENDOP_ONE;
	rt_blend_state.destBlend = SVGA3D_BLENDOP_ZERO;
	rt_blend_state.blendOp = SVGA3D_BLENDEQ_ADD;
	rt_blend_state.srcBlendAlpha = SVGA3D_BLENDOP_ONE;
	rt_blend_state.destBlendAlpha = SVGA3D_BLENDOP_ZERO;
	rt_blend_state.blendOpAlpha = SVGA3D_BLENDEQ_ADD;
	rt_blend_state.logicOpEnable = false;
	rt_blend_state.logicOp = 0;
	vmw_bitvector_find_next_bit(device->blend_state_bv, &blend_cmd.blendId);
	blend_cmd.alphaToCoverageEnable = 0;
	blend_cmd.independentBlendEnable = 1;
	for (i = 0; i < ARRAY_SIZE(blend_cmd.perRT); i++)
		blend_cmd.perRT[i] = rt_blend_state;
	vmw_execbuf_append(cmd_buf, SVGA_3D_CMD_DX_DEFINE_BLEND_STATE,
			   &blend_cmd, sizeof(blend_cmd), NULL, 0);
	objects->blend_id = blend_cmd.blendId;

	vmw_bitvector_find_next_bit(device->depthstencil_state_bv,
				    &depthstencil_cmd.depthStencilId);
	depthstencil_cmd.depthEnable = true;
	depthstencil_cmd.depthWriteMask = SVGA3D_DEPTH_WRITE_MASK_ALL;
	depthstencil_cmd.depthFunc = SVGA3D_CMP_LESSEQUAL;
	depthstencil_cmd.stencilEnable = false;
	depthstencil_cmd.frontEnable = false;
	depthstencil_cmd.backEnable = false;
	depthstencil_cmd.stencilReadMask = 0;
	depthstencil_cmd.stencilWriteMask = 0;
	depthstencil_cmd.frontStencilFailOp = SVGA3D_STENCILOP_KEEP;
	depthstencil_cmd.frontStencilDepthFailOp = SVGA3D_STENCILOP_KEEP;
	depthstencil_cmd.frontStencilPassOp = SVGA3D_STENCILOP_KEEP;
	depthstencil_cmd.frontStencilFunc = SVGA3D_CMP_ALWAYS;
	depthstencil_cmd.backStencilFailOp = SVGA3D_STENCILOP_KEEP;
	depthstencil_cmd.backStencilDepthFailOp = SVGA3D_STENCILOP_KEEP;
	depthstencil_cmd.backStencilPassOp = SVGA3D_STENCILOP_KEEP;
	depthstencil_cmd.backStencilFunc = SVGA3D_CMP_ALWAYS;
	vmw_execbuf_append(cmd_buf, SVGA_3D_CMD_DX_DEFINE_DEPTHSTENCIL_STATE,
			   &depthstencil_cmd, sizeof(depthstencil_cmd), NULL, 0);
	objects->depthstencil_id = depthstencil_cmd.depthStencilId;

	vmw_bitvector_find_next_bit(device->rasterizer_state_bv,
				    &rasterizer_cmd.rasterizerId);
	rasterizer_cmd.fillMode = SVGA3D_FILLMODE_FILL;
	rasterizer_cmd.cullMode = SVGA3D_CULL_NONE;
	rasterizer_cmd.frontCounterClockwise = false;
	rasterizer_cmd.depthBias = 0;
	rasterizer_cmd.depthBiasClamp = 0.0;
	rasterizer_cmd.slopeScaledDepthBias = 0.0;
	rasterizer_cmd.depthClipEnable = true;
	rasterizer_cmd.scissorEnable = false;
	rasterizer_cmd.multisampleEnable = false;
	rasterizer_cmd.antialiasedLineEnable = false;
	rasterizer_cmd.lineWidth = 0.0;
	rasterizer_cmd.lineStippleEnable = 0;
	rasterizer_cmd.lineStippleFactor = 0;
	rasterizer_cmd.lineStipplePattern = 0;
	rasterizer_cmd.provokingVertexLast = 0;
	vmw_execbuf_append(cmd_buf, SVGA_3D_CMD_DX_DEFINE_RASTERIZER_STATE,
			   &rasterizer_cmd, sizeof(rasterizer_cmd), NULL, 0);
	objects->rasterizer_id = rasterizer_cmd.rasterizerId;

	objects->color_rt = vmw_create_surface_simple(
		device->drm_fd,
		SVGA3D_SURFACE_HINT_TEXTURE | SVGA3D_SURFACE_HINT_RENDERTARGET |
			SVGA3D_SURFACE_BIND_RENDER_TARGET,
		SVGA3D_R8G8B8A8_UNORM, *rt_size, NULL);

	objects->depth_rt = vmw_create_surface_simple(
		device->drm_fd,
		SVGA3D_SURFACE_HINT_DEPTHSTENCIL |
			SVGA3D_SURFACE_HINT_RENDERTARGET |
			SVGA3D_SURFACE_BIND_DEPTH_STENCIL,
		SVGA3D_R24G8_TYPELESS, *rt_size, NULL);

	rtv_desc.tex.arraySize = 1;
	rtv_desc.tex.firstArraySlice = 0;
	rtv_desc.tex.mipSlice = 0;
	vmw_bitvector_find_next_bit(device->rt_view_bv,
				    &rt_view_cmd.renderTargetViewId);
	rt_view_cmd.sid = objects->color_rt->base.handle;
	rt_view_cmd.format = SVGA3D_R8G8B8A8_UNORM;
	rt_view_cmd.resourceDimension = SVGA3D_RESOURCE_TEXTURE2D;
	rt_view_cmd.desc = rtv_desc;
	vmw_execbuf_append(cmd_buf, SVGA_3D_CMD_DX_DEFINE_RENDERTARGET_VIEW,
			   &rt_view_cmd, sizeof(rt_view_cmd), NULL, 0);
	objects->color_rt_id = rt_view_cmd.renderTargetViewId;

	vmw_bitvector_find_next_bit(device->ds_view_bv,
				    &ds_view_cmd.depthStencilViewId);
	ds_view_cmd.sid = objects->depth_rt->base.handle;
	ds_view_cmd.format = SVGA3D_D24_UNORM_S8_UINT;
	ds_view_cmd.resourceDimension = SVGA3D_RESOURCE_TEXTURE2D;
	ds_view_cmd.mipSlice = 0;
	ds_view_cmd.firstArraySlice = 0;
	ds_view_cmd.arraySize = 1;
	vmw_execbuf_append(cmd_buf, SVGA_3D_CMD_DX_DEFINE_DEPTHSTENCIL_VIEW,
			   &ds_view_cmd, sizeof(ds_view_cmd), NULL, 0);
	objects->ds_view_id = ds_view_cmd.depthStencilViewId;

	objects->vertex_shader = vmw_shader_define_and_bind(
		device, cmd_buf, SVGA3D_SHADERTYPE_VS,
		sizeof(SVGADXVertexShader), SVGADXVertexShader);

	objects->pixel_shader = vmw_shader_define_and_bind(
		device, cmd_buf, SVGA3D_SHADERTYPE_PS, sizeof(SVGADXPixelShader),
		SVGADXPixelShader);

	vmw_execbuf_submit(cmd_buf, &cmd_fence);
	vmw_ioctl_fence_finish(device->drm_fd, &cmd_fence);
	vmw_execbuf_destroy(cmd_buf);

	objects->rt_size = *rt_size;
}

void vmw_set_default_objects(int drm_fd, struct vmw_default_objects *objects)
{
	struct vmw_execbuf *cmd_buf;
	struct drm_vmw_fence_rep cmd_fence = { 0 };

	SVGA3dCmdDXSetInputLayout element_layout_cmd = {
		.elementLayoutId = objects->element_layout_id
	};

	SVGA3dCmdDXSetBlendState blend_cmd = { .blendId = objects->blend_id,
					       .blendFactor = { 1.0f, 1.0f,
								1.0f, 1.0f },
					       .sampleMask = 0xFFFFFFFF };

	SVGA3dCmdDXSetDepthStencilState depthstencil_cmd = {
		.depthStencilId = objects->depthstencil_id, .stencilRef = 0
	};

	SVGA3dCmdDXSetRasterizerState rasterizer_cmd = {
		.rasterizerId = objects->rasterizer_id
	};

	SVGA3dViewport viewport = { .x = 0.0,
				    .y = 0.0,
				    .width = objects->rt_size.width,
				    .height = objects->rt_size.height,
				    .minDepth = 0.0,
				    .maxDepth = 1.0 };
	SVGA3dCmdDXSetViewports viewports_cmd = { 0 };

	SVGASignedRect scissor_rect = { .left = 0,
					.right = objects->rt_size.width,
					.top = 0,
					.bottom = objects->rt_size.height };
	SVGA3dCmdDXSetScissorRects rects_cmd = { 0 };

	SVGA3dCmdDXSetRenderTargets rt_cmd = { .depthStencilViewId =
						       objects->ds_view_id };

	SVGA3dCmdDXSetShader vs_cmd = { .shaderId = objects->vertex_shader.shid,
					.type = SVGA3D_SHADERTYPE_VS };

	SVGA3dCmdDXSetShader ps_cmd = { .shaderId = objects->pixel_shader.shid,
					.type = SVGA3D_SHADERTYPE_PS };

	cmd_buf = vmw_execbuf_create(drm_fd, objects->context_id);

	vmw_execbuf_append(cmd_buf, SVGA_3D_CMD_DX_SET_INPUT_LAYOUT,
			   &element_layout_cmd, sizeof(element_layout_cmd), NULL,
			   0);

	vmw_execbuf_append(cmd_buf, SVGA_3D_CMD_DX_SET_BLEND_STATE, &blend_cmd,
			   sizeof(blend_cmd), NULL, 0);

	vmw_execbuf_append(cmd_buf, SVGA_3D_CMD_DX_SET_DEPTHSTENCIL_STATE,
			   &depthstencil_cmd, sizeof(depthstencil_cmd), NULL, 0);

	vmw_execbuf_append(cmd_buf, SVGA_3D_CMD_DX_SET_RASTERIZER_STATE,
			   &rasterizer_cmd, sizeof(rasterizer_cmd), NULL, 0);

	vmw_execbuf_append(cmd_buf, SVGA_3D_CMD_DX_SET_VIEWPORTS,
			   &viewports_cmd, sizeof(viewports_cmd), &viewport,
			   sizeof(SVGA3dViewport));

	vmw_execbuf_append(cmd_buf, SVGA_3D_CMD_DX_SET_SCISSORRECTS, &rects_cmd,
			   sizeof(rects_cmd), &scissor_rect,
			   sizeof(SVGASignedRect));

	vmw_execbuf_append(cmd_buf, SVGA_3D_CMD_DX_SET_RENDERTARGETS, &rt_cmd,
			   sizeof(rt_cmd), &objects->color_rt_id,
			   sizeof(SVGA3dRenderTargetViewId));

	vmw_execbuf_append(cmd_buf, SVGA_3D_CMD_DX_SET_SHADER, &vs_cmd,
			   sizeof(vs_cmd), NULL, 0);

	vmw_execbuf_append(cmd_buf, SVGA_3D_CMD_DX_SET_SHADER, &ps_cmd,
			   sizeof(ps_cmd), NULL, 0);

	vmw_execbuf_submit(cmd_buf, &cmd_fence);
	vmw_ioctl_fence_finish(drm_fd, &cmd_fence);
	vmw_execbuf_destroy(cmd_buf);
}

void vmw_destroy_default_objects(struct vmw_svga_device *device,
				 struct vmw_default_objects *objects)
{
	struct vmw_execbuf *cmd_buf;
	struct drm_vmw_fence_rep cmd_fence = { 0 };

	SVGA3dCmdDXDestroyElementLayout element_layout_cmd = {
		.elementLayoutId = objects->element_layout_id
	};

	SVGA3dCmdDXDestroyBlendState blend_cmd = { .blendId =
							   objects->blend_id };

	SVGA3dCmdDXDestroyDepthStencilState depthstencil_cmd = {
		.depthStencilId = objects->depthstencil_id
	};

	SVGA3dCmdDXDestroyRasterizerState rasterizer_cmd = {
		.rasterizerId = objects->rasterizer_id
	};

	SVGA3dCmdDXDestroyRenderTargetView rt_view_cmd = {
		.renderTargetViewId = objects->color_rt_id
	};

	SVGA3dCmdDXDestroyDepthStencilView ds_view_cmd = {
		.depthStencilViewId = objects->ds_view_id
	};

	cmd_buf = vmw_execbuf_create(device->drm_fd, objects->context_id);

	vmw_execbuf_append(cmd_buf, SVGA_3D_CMD_DX_DESTROY_ELEMENTLAYOUT,
			   &element_layout_cmd, sizeof(element_layout_cmd), NULL,
			   0);

	vmw_execbuf_append(cmd_buf, SVGA_3D_CMD_DX_DESTROY_BLEND_STATE,
			   &blend_cmd, sizeof(blend_cmd), NULL, 0);

	vmw_execbuf_append(cmd_buf, SVGA_3D_CMD_DX_DESTROY_DEPTHSTENCIL_STATE,
			   &depthstencil_cmd, sizeof(depthstencil_cmd), NULL, 0);

	vmw_execbuf_append(cmd_buf, SVGA_3D_CMD_DX_DESTROY_RASTERIZER_STATE,
			   &rasterizer_cmd, sizeof(rasterizer_cmd), NULL, 0);

	vmw_execbuf_append(cmd_buf, SVGA_3D_CMD_DX_DESTROY_RENDERTARGET_VIEW,
			   &rt_view_cmd, sizeof(rt_view_cmd), NULL, 0);

	vmw_execbuf_append(cmd_buf, SVGA_3D_CMD_DX_DESTROY_DEPTHSTENCIL_VIEW,
			   &ds_view_cmd, sizeof(ds_view_cmd), NULL, 0);

	vmw_ioctl_surface_unref(device->drm_fd, objects->color_rt);
	vmw_ioctl_surface_unref(device->drm_fd, objects->depth_rt);

	vmw_bitvector_free_bit(device->element_layout_bv,
			       objects->element_layout_id);
	vmw_bitvector_free_bit(device->blend_state_bv, objects->blend_id);
	vmw_bitvector_free_bit(device->depthstencil_state_bv,
			       objects->depthstencil_id);
	vmw_bitvector_free_bit(device->rasterizer_state_bv,
			       objects->rasterizer_id);
	vmw_bitvector_free_bit(device->rt_view_bv, objects->color_rt_id);
	vmw_bitvector_free_bit(device->ds_view_bv, objects->ds_view_id);

	vmw_shader_destroy(device, cmd_buf, objects->vertex_shader);
	vmw_shader_destroy(device, cmd_buf, objects->pixel_shader);

	vmw_execbuf_submit(cmd_buf, &cmd_fence);
	vmw_ioctl_fence_finish(device->drm_fd, &cmd_fence);
	vmw_execbuf_destroy(cmd_buf);
}

void vmw_cmd_set_topology(struct vmw_execbuf *cmd_buf,
			  SVGA3dPrimitiveType topology)
{
	SVGA3dCmdDXSetTopology cmd = { .topology = topology };

	vmw_execbuf_append(cmd_buf, SVGA_3D_CMD_DX_SET_TOPOLOGY, &cmd,
			   sizeof(cmd), NULL, 0);
}

void vmw_cmd_set_vertex_buffers(struct vmw_execbuf *cmd_buf,
				uint32 start_buffer,
				SVGA3dVertexBuffer *buffers, uint32 num_buffers)
{
	SVGA3dCmdDXSetVertexBuffers cmd = { .startBuffer = start_buffer };

	vmw_execbuf_append(cmd_buf, SVGA_3D_CMD_DX_SET_VERTEX_BUFFERS, &cmd,
			   sizeof(cmd), buffers,
			   num_buffers * sizeof(SVGA3dVertexBuffer));
}

void vmw_cmd_update_gb_surface(struct vmw_execbuf *cmd_buf, SVGA3dSurfaceId sid)
{
	SVGA3dCmdUpdateGBSurface cmd = { .sid = sid };

	vmw_execbuf_append(cmd_buf, SVGA_3D_CMD_UPDATE_GB_SURFACE, &cmd,
			   sizeof(cmd), NULL, 0);
}

void vmw_cmd_clear_depthstencil_view(struct vmw_execbuf *cmd_buf, uint16 flags,
				     uint16 stencil,
				     SVGA3dDepthStencilViewId dsvid,
				     float depth)
{
	SVGA3dCmdDXClearDepthStencilView cmd = { .flags = flags,
						 .stencil = stencil,
						 .depthStencilViewId = dsvid,
						 .depth = depth };

	vmw_execbuf_append(cmd_buf, SVGA_3D_CMD_DX_CLEAR_DEPTHSTENCIL_VIEW,
			   &cmd, sizeof(cmd), NULL, 0);
}

void vmw_cmd_clear_rendertarget_view(struct vmw_execbuf *cmd_buf,
				     SVGA3dRenderTargetViewId rtvid,
				     SVGA3dRGBAFloat rgba)
{
	SVGA3dCmdDXClearRenderTargetView cmd = { .renderTargetViewId = rtvid,
						 .rgba = rgba };

	vmw_execbuf_append(cmd_buf, SVGA_3D_CMD_DX_CLEAR_RENDERTARGET_VIEW,
			   &cmd, sizeof(cmd), NULL, 0);
}

void vmw_cmd_draw(struct vmw_execbuf *cmd_buf, uint32 vertex_count,
		  uint32 start_vertex_location)
{
	SVGA3dCmdDXDraw cmd = { .vertexCount = vertex_count,
				.startVertexLocation = start_vertex_location };

	vmw_execbuf_append(cmd_buf, SVGA_3D_CMD_DX_DRAW, &cmd, sizeof(cmd), NULL,
			   0);
}

void vmw_cmd_readback_gb_surface(struct vmw_execbuf *cmd_buf, uint32 sid)
{
	SVGA3dCmdReadbackGBSurface cmd = { .sid = sid };

	vmw_execbuf_append(cmd_buf, SVGA_3D_CMD_READBACK_GB_SURFACE, &cmd,
			   sizeof(cmd), NULL, 0);
}

void *vmw_readback_surface(int drm_fd, struct vmw_surface *surface)
{
	void *values;
	void *readback;
	struct vmw_mob readback_mob = {
		.size = surface->base.buffer_size,
		.handle = surface->base.buffer_handle,
		.map_handle = surface->base.buffer_map_handle
	};

	values = malloc(surface->base.buffer_size);

	readback = vmw_ioctl_mob_map(drm_fd, &readback_mob);
	memcpy(values, readback, readback_mob.size);
	vmw_ioctl_mob_unmap(&readback_mob);

	return values;
}

void vmw_cmd_surface_copy(struct vmw_execbuf *cmd_buf, SVGA3dSurfaceImageId src,
			  SVGA3dSurfaceImageId dest, const SVGA3dCopyBox *boxes,
			  uint32 num_boxes)
{
	SVGA3dCmdSurfaceCopy cmd = { .src = src, .dest = dest };

	vmw_execbuf_append(cmd_buf, SVGA_3D_CMD_SURFACE_COPY, &cmd, sizeof(cmd),
			   boxes, num_boxes * sizeof(SVGA3dCopyBox));
}

uint8 *vmw_triangle_draw(struct vmw_svga_device *device, int32 cid,
			 struct vmw_default_objects *objects, bool do_sync)
{
	struct vmw_execbuf *cmd_buf;
	struct drm_vmw_fence_rep cmd_fence;
	struct vmw_mob *vertex_mob;
	struct vmw_surface *vertex_buffer;
	SVGA3dVertexBuffer vb_binding;
	SVGA3dRGBAFloat clear_color;
	void *vertex_data;
	uint8 *rendered_img;
	struct vmw_vertex vertices[3] = {
		{ 0.0, 0.75, 0.5, 1.0, 0.0, 1.0, 0.0, 1.0 },
		{ 0.75, -0.75, 0.5, 1.0, 1.0, 0.0, 0.0, 1.0 },
		{ -0.75, -0.75, 0.5, 1.0, 0.0, 0.0, 1.0, 1.0 },
	};

	/* Vertex setup */
	vertex_mob = vmw_ioctl_mob_create(device->drm_fd, sizeof(vertices));
	vertex_buffer = vmw_ioctl_buffer_create(
		device->drm_fd,
		SVGA3D_SURFACE_HINT_VERTEXBUFFER |
			SVGA3D_SURFACE_BIND_VERTEX_BUFFER,
		sizeof(vertices), vertex_mob);

	vmw_set_default_objects(device->drm_fd, objects);

	cmd_buf = vmw_execbuf_create(device->drm_fd, cid);

	vmw_cmd_set_topology(cmd_buf, SVGA3D_PRIMITIVE_TRIANGLELIST);

	vb_binding.sid = vertex_buffer->base.handle;
	vb_binding.offset = 0;
	vb_binding.stride = sizeof(vertices[0]);
	vmw_cmd_set_vertex_buffers(cmd_buf, 0, &vb_binding, 1);

	/* Copy data into vertex buffer */
	vertex_data = vmw_ioctl_mob_map(device->drm_fd, vertex_mob);
	memcpy(vertex_data, vertices, sizeof(vertices));
	vmw_ioctl_mob_unmap(vertex_mob);

	vmw_cmd_update_gb_surface(cmd_buf, vertex_buffer->base.handle);

	/* Clear color = 50% gray */
	clear_color.r = 0.5;
	clear_color.g = 0.5;
	clear_color.b = 0.5;
	clear_color.a = 1.0;

	/* Clear */
	vmw_cmd_clear_depthstencil_view(cmd_buf, 0xFFFF, 0, objects->ds_view_id,
					1.0);
	vmw_cmd_clear_rendertarget_view(cmd_buf, objects->color_rt_id,
					clear_color);

	/* Draw */
	vmw_cmd_draw(cmd_buf, 3, 0);
	vmw_cmd_draw(cmd_buf, 3, 0);

	/* Readback */
	vmw_cmd_readback_gb_surface(cmd_buf, objects->color_rt->base.handle);

	/* Submit commands */
	vmw_execbuf_submit(cmd_buf, &cmd_fence);
	if (do_sync)
		vmw_ioctl_fence_finish(device->drm_fd, &cmd_fence);
	vmw_execbuf_destroy(cmd_buf);

	/* Read framebuffer into system mem and save */
	rendered_img = vmw_readback_surface(device->drm_fd, objects->color_rt);

	vmw_ioctl_surface_unref(device->drm_fd, vertex_buffer);
	vmw_ioctl_mob_close_handle(device->drm_fd, vertex_mob);
	return rendered_img;
}

void vmw_triangle_assert_values(uint8 *rendered_img,
				struct vmw_surface *color_rt)
{
	uint8 *out_pixel;
	uint8 *center_pixel;
	uint8 *rv_pixel;
	uint8 *gv_pixel;
	uint8 *bv_pixel;

	/* Assert some pixel values */
	out_pixel = vmw_surface_data_pixel(color_rt, rendered_img, 10, 10);
	igt_assert_eq(out_pixel[0], 127); // r
	igt_assert_eq(out_pixel[1], 127); // g
	igt_assert_eq(out_pixel[2], 127); // b

	center_pixel = vmw_surface_data_pixel(color_rt, rendered_img, 200, 200);
	igt_assert_eq(center_pixel[0], 64); // r
	igt_assert_eq(center_pixel[1], 127); // g
	igt_assert_eq(center_pixel[2], 64); // b

	rv_pixel = vmw_surface_data_pixel(color_rt, rendered_img, 349, 349);
	igt_assert_eq(rv_pixel[0], 254); // r
	igt_assert_eq(rv_pixel[1], 0); // g
	igt_assert_eq(rv_pixel[2], 0); // b

	gv_pixel = vmw_surface_data_pixel(color_rt, rendered_img, 200, 52);
	igt_assert_eq(gv_pixel[0], 1); // r
	igt_assert_eq(gv_pixel[1], 253); // g
	igt_assert_eq(gv_pixel[2], 1); // b

	bv_pixel = vmw_surface_data_pixel(color_rt, rendered_img, 50, 349);
	igt_assert_eq(bv_pixel[0], 0); // r
	igt_assert_eq(bv_pixel[1], 0); // g
	igt_assert_eq(bv_pixel[2], 254); // b
}

SVGA3dDevCapResult vmw_format_get_caps(int drm_fd,
				       SVGA3dDevCapIndex dev_cap_index)
{
	uint64 size;
	uint32 *cap_buffer;
	SVGA3dDevCapResult result = { 0 };

	if (dev_cap_index >= SVGA3D_DEVCAP_MAX)
		return result;

	size = vmw_ioctl_get_param(drm_fd, DRM_VMW_PARAM_3D_CAPS_SIZE);
	cap_buffer = (uint32 *)malloc(size);
	memset(cap_buffer, 0, size);

	vmw_ioctl_get_3d_cap(drm_fd, (uint64) (unsigned long) cap_buffer, size);
	result = (SVGA3dDevCapResult)cap_buffer[dev_cap_index];

	free(cap_buffer);
	return result;
}

bool vmw_is_format_supported(int drm_fd, SVGA3dDevCapIndex dev_cap_index)
{
	SVGA3dDevCapResult result;

	result = vmw_format_get_caps(drm_fd, dev_cap_index);
	return result.u & SVGA3D_FORMAT_POSITIVE;
}
