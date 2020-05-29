/*
 * Copyright © 2018 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 *
 */

#include "gpu_cmds.h"

uint32_t
gen7_fill_curbe_buffer_data(struct intel_bb *ibb, uint8_t color)
{
	uint32_t *curbe_buffer;
	uint32_t offset;

	intel_bb_ptr_align(ibb, 64);
	curbe_buffer = intel_bb_ptr(ibb);
	offset = intel_bb_offset(ibb);

	*curbe_buffer = color;
	intel_bb_ptr_add(ibb, 32);

	return offset;
}

uint32_t
gen11_fill_curbe_buffer_data(struct intel_bb *ibb)
{
	uint32_t *curbe_buffer;
	uint32_t offset;

	intel_bb_ptr_align(ibb, 64);
	curbe_buffer = intel_bb_ptr(ibb);
	offset = intel_bb_offset(ibb);

	*curbe_buffer++ = 0;
	*curbe_buffer = 1;
	intel_bb_ptr_add(ibb, 64);

	return offset;
}

static uint32_t
gen7_fill_kernel(struct intel_bb *ibb,
		const uint32_t kernel[][4],
		size_t size)
{
	uint32_t *kernel_dst;
	uint32_t offset;

	intel_bb_ptr_align(ibb, 64);
	kernel_dst = intel_bb_ptr(ibb);
	offset = intel_bb_offset(ibb);

	memcpy(kernel_dst, kernel, size);

	intel_bb_ptr_add(ibb, size);

	return offset;
}

static uint32_t
gen7_fill_surface_state(struct intel_bb *ibb,
			struct intel_buf *buf,
			uint32_t format,
			int is_dst)
{
	struct gen7_surface_state *ss;
	uint32_t write_domain, read_domain, offset;
	uint64_t address;

	if (is_dst) {
		write_domain = read_domain = I915_GEM_DOMAIN_RENDER;
	} else {
		write_domain = 0;
		read_domain = I915_GEM_DOMAIN_SAMPLER;
	}

	intel_bb_ptr_align(ibb, 64);
	offset = intel_bb_offset(ibb);
	ss = intel_bb_ptr(ibb);
	intel_bb_ptr_add(ibb, 64);

	ss->ss0.surface_type = SURFACE_2D;
	ss->ss0.surface_format = format;
	ss->ss0.render_cache_read_write = 1;

	if (buf->tiling == I915_TILING_X)
		ss->ss0.tiled_mode = 2;
	else if (buf->tiling == I915_TILING_Y)
		ss->ss0.tiled_mode = 3;

	address = intel_bb_offset_reloc(ibb, buf->handle,
					read_domain, write_domain,
					offset + 4, 0x0);
	igt_assert(address >> 32 == 0);

	ss->ss1.base_addr = address;

	ss->ss2.height = intel_buf_height(buf) - 1;
	ss->ss2.width  = intel_buf_width(buf) - 1;
	ss->ss3.pitch  = buf->stride - 1;

	ss->ss7.shader_chanel_select_r = 4;
	ss->ss7.shader_chanel_select_g = 5;
	ss->ss7.shader_chanel_select_b = 6;
	ss->ss7.shader_chanel_select_a = 7;

	return offset;
}

static uint32_t
gen8_fill_surface_state(struct intel_bb *ibb,
			struct intel_buf *buf,
			uint32_t format,
			int is_dst)
{
	struct gen8_surface_state *ss;
	uint32_t write_domain, read_domain, offset;
	uint64_t address;

	if (is_dst) {
		write_domain = read_domain = I915_GEM_DOMAIN_RENDER;
	} else {
		write_domain = 0;
		read_domain = I915_GEM_DOMAIN_SAMPLER;
	}

	intel_bb_ptr_align(ibb, 64);
	offset = intel_bb_offset(ibb);
	ss = intel_bb_ptr(ibb);
	intel_bb_ptr_add(ibb, 64);

	ss->ss0.surface_type = SURFACE_2D;
	ss->ss0.surface_format = format;
	ss->ss0.render_cache_read_write = 1;
	ss->ss0.vertical_alignment = 1; /* align 4 */
	ss->ss0.horizontal_alignment = 1; /* align 4 */

	if (buf->tiling == I915_TILING_X)
		ss->ss0.tiled_mode = 2;
	else if (buf->tiling == I915_TILING_Y)
		ss->ss0.tiled_mode = 3;

	address = intel_bb_offset_reloc(ibb, buf->handle,
					read_domain, write_domain,
					offset + 4 * 8, 0x0);

	ss->ss8.base_addr = (uint32_t) address;
	ss->ss9.base_addr_hi = address >> 32;

	ss->ss2.height = intel_buf_height(buf) - 1;
	ss->ss2.width  = intel_buf_width(buf) - 1;
	ss->ss3.pitch  = buf->stride - 1;

	ss->ss7.shader_chanel_select_r = 4;
	ss->ss7.shader_chanel_select_g = 5;
	ss->ss7.shader_chanel_select_b = 6;
	ss->ss7.shader_chanel_select_a = 7;

	return offset;
}

static uint32_t
gen11_fill_surface_state(struct intel_bb *ibb,
			 const struct intel_buf *buf,
			 uint32_t surface_type,
			 uint32_t format,
			 uint32_t vertical_alignment,
			 uint32_t horizontal_alignment,
			 int is_dst)
{
	struct gen8_surface_state *ss;
	uint32_t write_domain, read_domain, offset;
	uint64_t address;

	if (is_dst) {
		write_domain = read_domain = I915_GEM_DOMAIN_RENDER;
	} else {
		write_domain = 0;
		read_domain = I915_GEM_DOMAIN_SAMPLER;
	}

	intel_bb_ptr_align(ibb, 64);
	offset = intel_bb_offset(ibb);
	ss = intel_bb_ptr(ibb);
	intel_bb_ptr_add(ibb, 64);

	ss->ss0.surface_type = surface_type;
	ss->ss0.surface_format = format;
	ss->ss0.render_cache_read_write = 1;
	ss->ss0.vertical_alignment = vertical_alignment; /* align 4 */
	ss->ss0.horizontal_alignment = horizontal_alignment; /* align 4 */

	if (buf->tiling == I915_TILING_X)
		ss->ss0.tiled_mode = 2;
	else if (buf->tiling == I915_TILING_Y)
		ss->ss0.tiled_mode = 3;
	else
		ss->ss0.tiled_mode = 0;

	address = intel_bb_offset_reloc(ibb, buf->handle,
					read_domain, write_domain,
					offset + 4 * 8, 0x0);

	ss->ss8.base_addr = (uint32_t) address;
	ss->ss9.base_addr_hi = address >> 32;

	if (is_dst) {
		ss->ss1.memory_object_control = 2;
		ss->ss2.height = 1;
		ss->ss2.width  = 95;
		ss->ss3.pitch  = 0;
		ss->ss7.shader_chanel_select_r = 4;
		ss->ss7.shader_chanel_select_g = 5;
		ss->ss7.shader_chanel_select_b = 6;
		ss->ss7.shader_chanel_select_a = 7;
	}
	else {
		ss->ss1.qpitch = 4040;
		ss->ss1.base_mip_level = 31;
		ss->ss2.height = 9216;
		ss->ss2.width  = 1019;
		ss->ss3.pitch  = 64;
		ss->ss5.mip_count = 2;
	}

	return offset;
}

static uint32_t
gen7_fill_binding_table(struct intel_bb *ibb,
			struct intel_buf *buf)
{
	uint32_t binding_table_offset;
	uint32_t *binding_table;
	uint32_t devid = intel_get_drm_devid(ibb->i915);

	intel_bb_ptr_align(ibb, 64);
	binding_table_offset = intel_bb_offset(ibb);
	binding_table = intel_bb_ptr(ibb);
	intel_bb_ptr_add(ibb, 64);

	if (IS_GEN7(devid))
		binding_table[0] = gen7_fill_surface_state(ibb, buf,
							   SURFACEFORMAT_R8_UNORM, 1);

	else
		binding_table[0] = gen8_fill_surface_state(ibb, buf,
							   SURFACEFORMAT_R8_UNORM, 1);

	return binding_table_offset;
}

static uint32_t
gen11_fill_binding_table(struct intel_bb *ibb,
			 const struct intel_buf *src,
			 const struct intel_buf *dst)
{
	uint32_t binding_table_offset;
	uint32_t *binding_table;

	intel_bb_ptr_align(ibb, 64);
	binding_table_offset = intel_bb_offset(ibb);
	binding_table = intel_bb_ptr(ibb);
	intel_bb_ptr_add(ibb, 64);

	binding_table[0] = gen11_fill_surface_state(ibb, src,
						    SURFACE_1D,
						    SURFACEFORMAT_R32G32B32A32_FLOAT,
						    0, 0, 0);
	binding_table[1] = gen11_fill_surface_state(ibb, dst,
						    SURFACE_BUFFER,
						    SURFACEFORMAT_RAW,
						    1, 1, 1);

	return binding_table_offset;
}

uint32_t
gen7_fill_interface_descriptor(struct intel_bb *ibb,
			       struct intel_buf *buf,
			       const uint32_t kernel[][4],
			       size_t size)
{
	struct gen7_interface_descriptor_data *idd;
	uint32_t offset;
	uint32_t binding_table_offset, kernel_offset;

	binding_table_offset = gen7_fill_binding_table(ibb, buf);
	kernel_offset = gen7_fill_kernel(ibb, kernel, size);

	intel_bb_ptr_align(ibb, 64);
	idd = intel_bb_ptr(ibb);
	offset = intel_bb_offset(ibb);

	idd->desc0.kernel_start_pointer = (kernel_offset >> 6);

	idd->desc1.single_program_flow = 1;
	idd->desc1.floating_point_mode = GEN7_FLOATING_POINT_IEEE_754;

	idd->desc2.sampler_count = 0;      /* 0 samplers used */
	idd->desc2.sampler_state_pointer = 0;

	idd->desc3.binding_table_entry_count = 0;
	idd->desc3.binding_table_pointer = (binding_table_offset >> 5);

	idd->desc4.constant_urb_entry_read_offset = 0;
	idd->desc4.constant_urb_entry_read_length = 1; /* grf 1 */

	intel_bb_ptr_add(ibb, sizeof(*idd));

	return offset;
}

uint32_t
gen8_fill_interface_descriptor(struct intel_bb *ibb,
			       struct intel_buf *buf,
			       const uint32_t kernel[][4],
			       size_t size)
{
	struct gen8_interface_descriptor_data *idd;
	uint32_t offset;
	uint32_t binding_table_offset, kernel_offset;

	binding_table_offset = gen7_fill_binding_table(ibb, buf);
	kernel_offset = gen7_fill_kernel(ibb, kernel, size);

	intel_bb_ptr_align(ibb, 64);
	idd = intel_bb_ptr(ibb);
	offset = intel_bb_offset(ibb);

	idd->desc0.kernel_start_pointer = (kernel_offset >> 6);

	idd->desc2.single_program_flow = 1;
	idd->desc2.floating_point_mode = GEN8_FLOATING_POINT_IEEE_754;

	idd->desc3.sampler_count = 0;      /* 0 samplers used */
	idd->desc3.sampler_state_pointer = 0;

	idd->desc4.binding_table_entry_count = 0;
	idd->desc4.binding_table_pointer = (binding_table_offset >> 5);

	idd->desc5.constant_urb_entry_read_offset = 0;
	idd->desc5.constant_urb_entry_read_length = 1; /* grf 1 */

	idd->desc6.num_threads_in_tg = 1;

	return offset;
}

uint32_t
gen11_fill_interface_descriptor(struct intel_bb *ibb,
				struct intel_buf *src, struct intel_buf *dst,
				const uint32_t kernel[][4],
				size_t size)
{
	struct gen8_interface_descriptor_data *idd;
	uint32_t offset;
	uint32_t binding_table_offset, kernel_offset;

	binding_table_offset = gen11_fill_binding_table(ibb, src, dst);
	kernel_offset = gen7_fill_kernel(ibb, kernel, size);

	intel_bb_ptr_align(ibb, 64);
	idd = intel_bb_ptr(ibb);
	offset = intel_bb_offset(ibb);

	idd->desc0.kernel_start_pointer = (kernel_offset >> 6);

	idd->desc2.single_program_flow = 1;
	idd->desc2.floating_point_mode = GEN8_FLOATING_POINT_IEEE_754;

	idd->desc3.sampler_count = 0;      /* 0 samplers used */
	idd->desc3.sampler_state_pointer = 0;

	idd->desc4.binding_table_entry_count = 0;
	idd->desc4.binding_table_pointer = (binding_table_offset >> 5);

	idd->desc5.constant_urb_entry_read_offset = 0;
	idd->desc5.constant_urb_entry_read_length = 1; /* grf 1 */

	idd->desc6.num_threads_in_tg = 1;

	return offset;
}

void
gen7_emit_state_base_address(struct intel_bb *ibb)
{
	intel_bb_out(ibb, GEN7_STATE_BASE_ADDRESS | (10 - 2));

	/* general */
	intel_bb_out(ibb, 0);

	/* surface */
	intel_bb_emit_reloc(ibb, ibb->handle,
			    I915_GEM_DOMAIN_INSTRUCTION, 0,
			    BASE_ADDRESS_MODIFY, 0x0);

	/* dynamic */
	intel_bb_emit_reloc(ibb, ibb->handle,
			    I915_GEM_DOMAIN_INSTRUCTION, 0,
			    BASE_ADDRESS_MODIFY, 0x0);

	/* indirect */
	intel_bb_out(ibb, 0);

	/* instruction */
	intel_bb_emit_reloc(ibb, ibb->handle,
			    I915_GEM_DOMAIN_INSTRUCTION, 0,
			    BASE_ADDRESS_MODIFY, 0x0);

	/* general/dynamic/indirect/instruction access Bound */
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0 | BASE_ADDRESS_MODIFY);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0 | BASE_ADDRESS_MODIFY);
}

void
gen8_emit_state_base_address(struct intel_bb *ibb)
{
	intel_bb_out(ibb, GEN8_STATE_BASE_ADDRESS | (16 - 2));

	/* general */
	intel_bb_out(ibb, 0 | BASE_ADDRESS_MODIFY);
	intel_bb_out(ibb, 0);

	/* stateless data port */
	intel_bb_out(ibb, 0 | BASE_ADDRESS_MODIFY);

	/* surface */
	intel_bb_emit_reloc(ibb, ibb->handle,
			    I915_GEM_DOMAIN_SAMPLER, 0,
			    BASE_ADDRESS_MODIFY, 0x0);

	/* dynamic */
	intel_bb_emit_reloc(ibb, ibb->handle,
			    I915_GEM_DOMAIN_RENDER | I915_GEM_DOMAIN_INSTRUCTION,
			    0,
			    BASE_ADDRESS_MODIFY, 0x0);

	/* indirect */
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);

	/* instruction */
	intel_bb_emit_reloc(ibb, ibb->handle,
			    I915_GEM_DOMAIN_INSTRUCTION,
			    0,
			    BASE_ADDRESS_MODIFY, 0x0);


	/* general state buffer size */
	intel_bb_out(ibb, 0xfffff000 | 1);
	/* dynamic state buffer size */
	intel_bb_out(ibb, 1 << 12 | 1);
	/* indirect object buffer size */
	intel_bb_out(ibb, 0xfffff000 | 1);
	/* instruction buffer size, must set modify enable bit, otherwise it may
	 * result in GPU hang
	 */
	intel_bb_out(ibb, 1 << 12 | 1);
}

void
gen9_emit_state_base_address(struct intel_bb *ibb)
{
	intel_bb_out(ibb, GEN8_STATE_BASE_ADDRESS | (19 - 2));

	/* general */
	intel_bb_out(ibb, 0 | BASE_ADDRESS_MODIFY);
	intel_bb_out(ibb, 0);

	/* stateless data port */
	intel_bb_out(ibb, 0 | BASE_ADDRESS_MODIFY);

	/* surface */
	intel_bb_emit_reloc(ibb, ibb->handle,
			    I915_GEM_DOMAIN_SAMPLER, 0,
			    BASE_ADDRESS_MODIFY, 0x0);

	/* dynamic */
	intel_bb_emit_reloc(ibb, ibb->handle,
			    I915_GEM_DOMAIN_RENDER | I915_GEM_DOMAIN_INSTRUCTION,
			    0,
			    BASE_ADDRESS_MODIFY, 0x0);

	/* indirect */
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);

	/* instruction */
	intel_bb_emit_reloc(ibb, ibb->handle,
			    I915_GEM_DOMAIN_INSTRUCTION, 0,
			    BASE_ADDRESS_MODIFY, 0x0);

	/* general state buffer size */
	intel_bb_out(ibb, 0xfffff000 | 1);
	/* dynamic state buffer size */
	intel_bb_out(ibb, 1 << 12 | 1);
	/* indirect object buffer size */
	intel_bb_out(ibb, 0xfffff000 | 1);
	/* intruction buffer size, must set modify enable bit, otherwise it may
	 * result in GPU hang
	 */
	intel_bb_out(ibb, 1 << 12 | 1);

	/* Bindless surface state base address */
	intel_bb_out(ibb, 0 | BASE_ADDRESS_MODIFY);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0xfffff000);
}

void
gen7_emit_vfe_state(struct intel_bb *ibb, uint32_t threads,
		    uint32_t urb_entries, uint32_t urb_size,
		    uint32_t curbe_size, uint32_t mode)
{
	intel_bb_out(ibb, GEN7_MEDIA_VFE_STATE | (8 - 2));

	/* scratch buffer */
	intel_bb_out(ibb, 0);

	/* number of threads & urb entries */
	intel_bb_out(ibb, threads << 16 |
		     urb_entries << 8 |
		     mode << 2); /* GPGPU vs media mode */

	intel_bb_out(ibb, 0);

	/* urb entry size & curbe size */
	intel_bb_out(ibb, urb_size << 16 |	/* in 256 bits unit */
		     curbe_size);		/* in 256 bits unit */

	/* scoreboard */
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
}

void
gen8_emit_vfe_state(struct intel_bb *ibb, uint32_t threads,
		    uint32_t urb_entries, uint32_t urb_size,
		    uint32_t curbe_size)
{
	intel_bb_out(ibb, GEN7_MEDIA_VFE_STATE | (9 - 2));

	/* scratch buffer */
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);

	/* number of threads & urb entries */
	intel_bb_out(ibb, threads << 16 | urb_entries << 8);

	intel_bb_out(ibb, 0);

	/* urb entry size & curbe size */
	intel_bb_out(ibb, urb_size << 16 | curbe_size);

	/* scoreboard */
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
}

void
gen7_emit_curbe_load(struct intel_bb *ibb, uint32_t curbe_buffer)
{
	intel_bb_out(ibb, GEN7_MEDIA_CURBE_LOAD | (4 - 2));
	intel_bb_out(ibb, 0);
	/* curbe total data length */
	intel_bb_out(ibb, 64);
	/* curbe data start address, is relative to the dynamics base address */
	intel_bb_out(ibb, curbe_buffer);
}

void
gen7_emit_interface_descriptor_load(struct intel_bb *ibb,
				    uint32_t interface_descriptor)
{
	intel_bb_out(ibb, GEN7_MEDIA_INTERFACE_DESCRIPTOR_LOAD | (4 - 2));
	intel_bb_out(ibb, 0);
	/* interface descriptor data length */
	if (ibb->gen == 7)
		intel_bb_out(ibb, sizeof(struct gen7_interface_descriptor_data));
	else
		intel_bb_out(ibb, sizeof(struct gen8_interface_descriptor_data));
	/* interface descriptor address, is relative to the dynamics base
	 * address
	 */
	intel_bb_out(ibb, interface_descriptor);
}

void
gen7_emit_gpgpu_walk(struct intel_bb *ibb,
		     unsigned int x, unsigned int y,
		     unsigned int width, unsigned int height)
{
	uint32_t x_dim, y_dim, tmp, right_mask;

	/*
	 * Simply do SIMD16 based dispatch, so every thread uses
	 * SIMD16 channels.
	 *
	 * Define our own thread group size, e.g 16x1 for every group, then
	 * will have 1 thread each group in SIMD16 dispatch. So thread
	 * width/height/depth are all 1.
	 *
	 * Then thread group X = width / 16 (aligned to 16)
	 * thread group Y = height;
	 */
	x_dim = (width + 15) / 16;
	y_dim = height;

	tmp = width & 15;
	if (tmp == 0)
		right_mask = (1 << 16) - 1;
	else
		right_mask = (1 << tmp) - 1;

	intel_bb_out(ibb, GEN7_GPGPU_WALKER | 9);

	/* interface descriptor offset */
	intel_bb_out(ibb, 0);

	/* SIMD size, thread w/h/d */
	intel_bb_out(ibb, 1 << 30 | /* SIMD16 */
		  0 << 16 | /* depth:1 */
		  0 << 8 | /* height:1 */
		  0); /* width:1 */

	/* thread group X */
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, x_dim);

	/* thread group Y */
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, y_dim);

	/* thread group Z */
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 1);

	/* right mask */
	intel_bb_out(ibb, right_mask);

	/* bottom mask, height 1, always 0xffffffff */
	intel_bb_out(ibb, 0xffffffff);
}

void
gen8_emit_gpgpu_walk(struct intel_bb *ibb,
		     unsigned int x, unsigned int y,
		     unsigned int width, unsigned int height)
{
	uint32_t x_dim, y_dim, tmp, right_mask;

	/*
	 * Simply do SIMD16 based dispatch, so every thread uses
	 * SIMD16 channels.
	 *
	 * Define our own thread group size, e.g 16x1 for every group, then
	 * will have 1 thread each group in SIMD16 dispatch. So thread
	 * width/height/depth are all 1.
	 *
	 * Then thread group X = width / 16 (aligned to 16)
	 * thread group Y = height;
	 */
	x_dim = (width + 15) / 16;
	y_dim = height;

	tmp = width & 15;
	if (tmp == 0)
		right_mask = (1 << 16) - 1;
	else
		right_mask = (1 << tmp) - 1;

	intel_bb_out(ibb, GEN7_GPGPU_WALKER | 13);

	intel_bb_out(ibb, 0); /* kernel offset */
	intel_bb_out(ibb, 0); /* indirect data length */
	intel_bb_out(ibb, 0); /* indirect data offset */

	/* SIMD size, thread w/h/d */
	intel_bb_out(ibb, 1 << 30 | /* SIMD16 */
		     0 << 16 | /* depth:1 */
		     0 << 8 | /* height:1 */
		     0); /* width:1 */

	/* thread group X */
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, x_dim);

	/* thread group Y */
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, y_dim);

	/* thread group Z */
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 1);

	/* right mask */
	intel_bb_out(ibb, right_mask);

	/* bottom mask, height 1, always 0xffffffff */
	intel_bb_out(ibb, 0xffffffff);
}

void
gen8_emit_media_state_flush(struct intel_bb *ibb)
{
	intel_bb_out(ibb, GEN8_MEDIA_STATE_FLUSH | (2 - 2));
	intel_bb_out(ibb, 0);
}

void
gen_emit_media_object(struct intel_bb *ibb,
		      unsigned int xoffset, unsigned int yoffset)
{
	intel_bb_out(ibb, GEN7_MEDIA_OBJECT | (8 - 2));

	/* interface descriptor offset */
	intel_bb_out(ibb, 0);

	/* without indirect data */
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);

	/* scoreboard */
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);

	/* inline data (xoffset, yoffset) */
	intel_bb_out(ibb, xoffset);
	intel_bb_out(ibb, yoffset);
	if (AT_LEAST_GEN(ibb->devid, 8) && !IS_CHERRYVIEW(ibb->devid))
		gen8_emit_media_state_flush(ibb);
}

void
gen7_emit_media_objects(struct intel_bb *ibb,
			unsigned int x, unsigned int y,
			unsigned int width, unsigned int height)
{
	int i, j;

	for (i = 0; i < width / 16; i++)
		for (j = 0; j < height / 16; j++)
			gen_emit_media_object(ibb, x + i * 16, y + j * 16);
}
