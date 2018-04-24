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

#ifndef GPU_FILL_H
#define GPU_FILL_H

#include <intel_bufmgr.h>
#include <i915_drm.h>

#include "media_fill.h"
#include "gen7_media.h"
#include "gen8_media.h"
#include "intel_reg.h"
#include "drmtest.h"
#include "intel_batchbuffer.h"
#include "intel_chipset.h"
#include <assert.h>

void
gen7_render_flush(struct intel_batchbuffer *batch, uint32_t batch_end);

uint32_t
gen7_fill_curbe_buffer_data(struct intel_batchbuffer *batch,
			uint8_t color);

uint32_t
gen7_fill_surface_state(struct intel_batchbuffer *batch,
			struct igt_buf *buf,
			uint32_t format,
			int is_dst);

uint32_t
gen7_fill_binding_table(struct intel_batchbuffer *batch,
			struct igt_buf *dst);

uint32_t
gen7_fill_kernel(struct intel_batchbuffer *batch,
		const uint32_t kernel[][4],
		size_t size);

uint32_t
gen7_fill_interface_descriptor(struct intel_batchbuffer *batch, struct igt_buf *dst,
			       const uint32_t kernel[][4], size_t size);

void
gen7_emit_state_base_address(struct intel_batchbuffer *batch);

void
gen7_emit_vfe_state(struct intel_batchbuffer *batch);

void
gen7_emit_vfe_state_gpgpu(struct intel_batchbuffer *batch);

void
gen7_emit_curbe_load(struct intel_batchbuffer *batch, uint32_t curbe_buffer);

void
gen7_emit_interface_descriptor_load(struct intel_batchbuffer *batch, uint32_t interface_descriptor);

void
gen7_emit_media_objects(struct intel_batchbuffer *batch,
			unsigned x, unsigned y,
			unsigned width, unsigned height);

void
gen7_emit_gpgpu_walk(struct intel_batchbuffer *batch,
		     unsigned x, unsigned y,
		     unsigned width, unsigned height);

uint32_t
gen8_fill_surface_state(struct intel_batchbuffer *batch,
			struct igt_buf *buf,
			uint32_t format,
			int is_dst);

uint32_t
gen8_fill_interface_descriptor(struct intel_batchbuffer *batch, struct igt_buf *dst,  const uint32_t kernel[][4], size_t size);

void
gen8_emit_state_base_address(struct intel_batchbuffer *batch);

void
gen8_emit_media_state_flush(struct intel_batchbuffer *batch);

void
gen8_emit_vfe_state(struct intel_batchbuffer *batch);

void
gen8_emit_vfe_state_gpgpu(struct intel_batchbuffer *batch);

void
gen8_emit_gpgpu_walk(struct intel_batchbuffer *batch,
		     unsigned x, unsigned y,
		     unsigned width, unsigned height);

void
gen9_emit_state_base_address(struct intel_batchbuffer *batch);

#endif /* GPU_FILL_H */
