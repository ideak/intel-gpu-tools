/*
 * Copyright © 2017 Intel Corporation
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
 */

#include "i915/gem.h"
#include "i915/gem_create.h"
#include "igt.h"
#include "igt_dummyload.h"

#define MAX_REG 0x200000
#define NUM_REGS (MAX_REG / sizeof(uint32_t))

#define PAGE_ALIGN(x) ALIGN(x, 4096)

#define DIRTY1 0x1
#define DIRTY2 0x2
#define RESET 0x4

#define ENGINE(x, y) BIT(4*(x) + (y))

enum {
	RCS0 = ENGINE(I915_ENGINE_CLASS_RENDER, 0),
	BCS0 = ENGINE(I915_ENGINE_CLASS_COPY, 0),
	VCS0 = ENGINE(I915_ENGINE_CLASS_VIDEO, 0),
	VCS1 = ENGINE(I915_ENGINE_CLASS_VIDEO, 1),
	VCS2 = ENGINE(I915_ENGINE_CLASS_VIDEO, 2),
	VCS3 = ENGINE(I915_ENGINE_CLASS_VIDEO, 3),
	VECS0 = ENGINE(I915_ENGINE_CLASS_VIDEO_ENHANCE, 0),
};

#define ALL ~0u
#define GEN_RANGE(x, y) ((ALL >> (32 - (y - x + 1))) << x)
#define GEN4 (ALL << 4)
#define GEN5 (ALL << 5)
#define GEN6 (ALL << 6)
#define GEN7 (ALL << 7)
#define GEN8 (ALL << 8)
#define GEN9 (ALL << 9)
#define GEN10 (ALL << 10)
#define GEN11 (ALL << 11)
#define GEN12 (ALL << 12)

#define NOCTX 0

#define LAST_KNOWN_GEN 12

static const struct named_register {
	const char *name;
	unsigned int gen_mask; /* on which gen the register exists */
	unsigned int engine_mask; /* preferred engine / powerwell */
	uint32_t offset; /* address of register, from bottom of mmio bar */
	uint32_t count;
	uint32_t ignore_bits;
	uint32_t write_mask; /* some registers bits do not exist */
	bool masked;
	bool relative;
} nonpriv_registers[] = {
	{ "NOPID", NOCTX, RCS0, 0x2094 },
	{ "MI_PREDICATE_RESULT_2", NOCTX, RCS0, 0x23bc },
	{
		"INSTPM",
		GEN6, RCS0, 0x20c0,
		.ignore_bits = BIT(8) /* ro counter */,
		.write_mask = BIT(8) /* rsvd varies between gen */,
		.masked = true,
	},
	{ "IA_VERTICES_COUNT", GEN4, RCS0, 0x2310, 2 },
	{ "IA_PRIMITIVES_COUNT", GEN4, RCS0, 0x2318, 2 },
	{ "VS_INVOCATION_COUNT", GEN4, RCS0, 0x2320, 2 },
	{ "HS_INVOCATION_COUNT", GEN4, RCS0, 0x2300, 2 },
	{ "DS_INVOCATION_COUNT", GEN4, RCS0, 0x2308, 2 },
	{ "GS_INVOCATION_COUNT", GEN4, RCS0, 0x2328, 2 },
	{ "GS_PRIMITIVES_COUNT", GEN4, RCS0, 0x2330, 2 },
	{ "CL_INVOCATION_COUNT", GEN4, RCS0, 0x2338, 2 },
	{ "CL_PRIMITIVES_COUNT", GEN4, RCS0, 0x2340, 2 },
	{ "PS_INVOCATION_COUNT_0", GEN4, RCS0, 0x22c8, 2, .write_mask = ~0x3 },
	{ "PS_DEPTH_COUNT_0", GEN4, RCS0, 0x22d8, 2 },
	{ "GPUGPU_DISPATCHDIMX", GEN8, RCS0, 0x2500 },
	{ "GPUGPU_DISPATCHDIMY", GEN8, RCS0, 0x2504 },
	{ "GPUGPU_DISPATCHDIMZ", GEN8, RCS0, 0x2508 },
	{ "MI_PREDICATE_SRC0", GEN8, RCS0, 0x2400, 2 },
	{ "MI_PREDICATE_SRC1", GEN8, RCS0, 0x2408, 2 },
	{ "MI_PREDICATE_DATA", GEN8, RCS0, 0x2410, 2 },
	{ "MI_PRED_RESULT", GEN8, RCS0, 0x2418, .write_mask = 0x1 },
	{ "3DPRIM_END_OFFSET", GEN6, RCS0, 0x2420 },
	{ "3DPRIM_START_VERTEX", GEN6, RCS0, 0x2430 },
	{ "3DPRIM_VERTEX_COUNT", GEN6, RCS0, 0x2434 },
	{ "3DPRIM_INSTANCE_COUNT", GEN6, RCS0, 0x2438 },
	{ "3DPRIM_START_INSTANCE", GEN6, RCS0, 0x243c },
	{ "3DPRIM_BASE_VERTEX", GEN6, RCS0, 0x2440 },
	{ "GPGPU_THREADS_DISPATCHED", GEN8, RCS0, 0x2290, 2 },
	{ "PS_INVOCATION_COUNT_1", GEN8, RCS0, 0x22f0, 2, .write_mask = ~0x3 },
	{ "PS_DEPTH_COUNT_1", GEN8, RCS0, 0x22f8, 2 },
	{ "BB_OFFSET", GEN8, RCS0, 0x2158, .ignore_bits = 0x7 },
	{ "MI_PREDICATE_RESULT_1", GEN8, RCS0, 0x241c },
	{ "OA_CTX_CONTROL", GEN8, RCS0, 0x2360 },
	{ "OACTXID", GEN8, RCS0, 0x2364 },
	{ "PS_INVOCATION_COUNT_2", GEN8, RCS0, 0x2448, 2, .write_mask = ~0x3 },
	{ "PS_DEPTH_COUNT_2", GEN8, RCS0, 0x2450, 2 },
	{ "Cache_Mode_0", GEN7, RCS0, 0x7000, .masked = true },
	{ "Cache_Mode_1", GEN7, RCS0, 0x7004, .masked = true },
	{ "GT_MODE", GEN8, RCS0, 0x7008, .masked = true },
	{ "L3_Config", GEN_RANGE(8, 11), RCS0, 0x7034 },
	{ "TD_CTL", GEN_RANGE(8, 11), RCS0, 0xe400, .write_mask = 0xffff },
	{ "TD_CTL2", GEN_RANGE(8, 11), RCS0, 0xe404 },
	{ "SO_NUM_PRIMS_WRITTEN0", GEN6, RCS0, 0x5200, 2 },
	{ "SO_NUM_PRIMS_WRITTEN1", GEN6, RCS0, 0x5208, 2 },
	{ "SO_NUM_PRIMS_WRITTEN2", GEN6, RCS0, 0x5210, 2 },
	{ "SO_NUM_PRIMS_WRITTEN3", GEN6, RCS0, 0x5218, 2 },
	{ "SO_PRIM_STORAGE_NEEDED0", GEN6, RCS0, 0x5240, 2 },
	{ "SO_PRIM_STORAGE_NEEDED1", GEN6, RCS0, 0x5248, 2 },
	{ "SO_PRIM_STORAGE_NEEDED2", GEN6, RCS0, 0x5250, 2 },
	{ "SO_PRIM_STORAGE_NEEDED3", GEN6, RCS0, 0x5258, 2 },
	{ "SO_WRITE_OFFSET0", GEN7, RCS0, 0x5280, .write_mask = ~0x3 },
	{ "SO_WRITE_OFFSET1", GEN7, RCS0, 0x5284, .write_mask = ~0x3 },
	{ "SO_WRITE_OFFSET2", GEN7, RCS0, 0x5288, .write_mask = ~0x3 },
	{ "SO_WRITE_OFFSET3", GEN7, RCS0, 0x528c, .write_mask = ~0x3 },
	{ "OA_CONTROL", NOCTX, RCS0, 0x2b00 },
	{ "PERF_CNT_1", NOCTX, RCS0, 0x91b8, 2 },
	{ "PERF_CNT_2", NOCTX, RCS0, 0x91c0, 2 },

	{ "CTX_PREEMPT", NOCTX /* GEN10 */, RCS0, 0x2248 },
	{ "CS_CHICKEN1", GEN11, RCS0, 0x2580, .masked = true },

	/* Privileged (enabled by w/a + FORCE_TO_NONPRIV) */
	{ "CTX_PREEMPT", NOCTX /* GEN9 */, RCS0, 0x2248 },
	{ "CS_CHICKEN1", GEN_RANGE(9, 10), RCS0, 0x2580, .masked = true },
	{ "COMMON_SLICE_CHICKEN2", GEN_RANGE(9, 9), RCS0, 0x7014, .masked = true },
	{ "HDC_CHICKEN1", GEN_RANGE(9, 10), RCS0, 0x7304, .masked = true },
	{ "SLICE_COMMON_ECO_CHICKEN1", GEN_RANGE(11, 11) /* + glk */, RCS0,  0x731c, .masked = true },
	{ "L3SQREG4", NOCTX /* GEN9:skl,kbl */, RCS0, 0xb118, .write_mask = ~0x1ffff0 },
	{ "HALF_SLICE_CHICKEN7", GEN_RANGE(11, 11), RCS0, 0xe194, .masked = true },
	{ "SAMPLER_MODE", GEN_RANGE(11, 11), RCS0, 0xe18c, .masked = true },

	{ "BCS_SWCTRL", GEN8, BCS0, 0x22200, .write_mask = 0x3, .masked = true },

	{ "MFC_VDBOX1", NOCTX, VCS0, 0x12800, 64 },
	{ "MFC_VDBOX2", NOCTX, VCS1, 0x1c800, 64 },

	{ "xCS_GPR", GEN9, ALL, 0x600, 32, .relative = true },

	{}
}, ignore_registers[] = {
	{ "RCS timestamp", GEN6, ~0u, 0x2358 },
	{ "BCS timestamp", GEN7, ~0u, 0x22358 },

	{ "xCS timestamp", GEN8, ALL, 0x358, .relative = true },

	/* huc read only */
	{ "BSD 0x2000", GEN11, ALL, 0x2000, .relative = true },
	{ "BSD 0x2014", GEN11, ALL, 0x2014, .relative = true },
	{ "BSD 0x23b0", GEN11, ALL, 0x23b0, .relative = true },

	{}
};

static const char *
register_name(uint32_t offset, uint32_t mmio_base, char *buf, size_t len)
{
	for (const struct named_register *r = nonpriv_registers; r->name; r++) {
		unsigned int width = r->count ? 4*r->count : 4;
		uint32_t base;

		base = r->offset;
		if (r->relative)
			base += mmio_base;

		if (offset >= base && offset < base + width) {
			if (r->count <= 1)
				return r->name;

			snprintf(buf, len, "%s[%d]",
				 r->name, (offset - base) / 4);
			return buf;
		}
	}

	return "unknown";
}

static const struct named_register *
lookup_register(uint32_t offset, uint32_t mmio_base)
{
	for (const struct named_register *r = nonpriv_registers; r->name; r++) {
		unsigned int width = r->count ? 4*r->count : 4;
		uint32_t base;

		base = r->offset;
		if (r->relative)
			base += mmio_base;

		if (offset >= base && offset < base + width)
			return r;
	}

	return NULL;
}

static bool ignore_register(uint32_t offset, uint32_t mmio_base)
{
	for (const struct named_register *r = ignore_registers; r->name; r++) {
		unsigned int width = r->count ? 4*r->count : 4;
		uint32_t base;

		base = r->offset;
		if (r->relative)
			base += mmio_base;

		if (offset >= base && offset < base + width)
			return true;
	}

	return false;
}

static void tmpl_regs(int fd,
		      const struct intel_execution_engine2 *e,
		      uint32_t handle,
		      uint32_t value)
{
	const unsigned int gen_bit = 1 << intel_gen(intel_get_drm_devid(fd));
	const unsigned int engine_bit = ENGINE(e->class, e->instance);
	const uint32_t mmio_base = gem_engine_mmio_base(fd, e->name);
	unsigned int regs_size;
	uint32_t *regs;

	regs_size = NUM_REGS * sizeof(uint32_t);
	regs_size = PAGE_ALIGN(regs_size);

	regs = gem_mmap__cpu(fd, handle, 0, regs_size, PROT_WRITE);
	gem_set_domain(fd, handle,
		       I915_GEM_DOMAIN_CPU, I915_GEM_DOMAIN_CPU);

	for (const struct named_register *r = nonpriv_registers; r->name; r++) {
		uint32_t offset;

		if (!(r->engine_mask & engine_bit))
			continue;
		if (!(r->gen_mask & gen_bit))
			continue;
		if (r->relative && !mmio_base)
			continue;

		offset = r->offset;
		if (r->relative)
			offset += mmio_base;

		for (unsigned count = r->count ?: 1; count--; offset += 4) {
			uint32_t x = value;
			if (r->write_mask)
				x &= r->write_mask;
			if (r->masked)
				x &= 0xffff;
			regs[offset/sizeof(*regs)] = x;
		}
	}
	munmap(regs, regs_size);
}

static uint32_t read_regs(int fd,
			  const intel_ctx_t *ctx,
			  const struct intel_execution_engine2 *e,
			  unsigned int flags)
{
	const unsigned int gen = intel_gen(intel_get_drm_devid(fd));
	const unsigned int gen_bit = 1 << gen;
	const unsigned int engine_bit = ENGINE(e->class, e->instance);
	const uint32_t mmio_base = gem_engine_mmio_base(fd, e->name);
	const bool r64b = gen >= 8;
	struct drm_i915_gem_exec_object2 obj[2];
	struct drm_i915_gem_relocation_entry *reloc;
	struct drm_i915_gem_execbuffer2 execbuf;
	unsigned int regs_size, batch_size, n;
	uint32_t *batch, *b;

	reloc = calloc(NUM_REGS, sizeof(*reloc));
	igt_assert(reloc);

	regs_size = NUM_REGS * sizeof(uint32_t);
	regs_size = PAGE_ALIGN(regs_size);

	batch_size = NUM_REGS * 4 * sizeof(uint32_t) + 4;
	batch_size = PAGE_ALIGN(batch_size);

	memset(obj, 0, sizeof(obj));
	obj[0].handle = gem_create(fd, regs_size);
	obj[1].handle = gem_create(fd, batch_size);
	obj[1].relocs_ptr = to_user_pointer(reloc);

	b = batch = gem_mmap__cpu(fd, obj[1].handle, 0, batch_size, PROT_WRITE);
	gem_set_domain(fd, obj[1].handle,
		       I915_GEM_DOMAIN_CPU, I915_GEM_DOMAIN_CPU);

	n = 0;
	for (const struct named_register *r = nonpriv_registers; r->name; r++) {
		uint32_t offset;

		if (!(r->engine_mask & engine_bit))
			continue;
		if (!(r->gen_mask & gen_bit))
			continue;
		if (r->relative && !mmio_base)
			continue;

		offset = r->offset;
		if (r->relative)
			offset += mmio_base;

		for (unsigned count = r->count ?: 1; count--; offset += 4) {
			*b++ = 0x24 << 23 | (1 + r64b); /* SRM */
			*b++ = offset;
			reloc[n].target_handle = obj[0].handle;
			reloc[n].presumed_offset = 0;
			reloc[n].offset = (b - batch) * sizeof(*b);
			reloc[n].delta = offset;
			reloc[n].read_domains = I915_GEM_DOMAIN_RENDER;
			reloc[n].write_domain = I915_GEM_DOMAIN_RENDER;
			*b++ = offset;
			if (r64b)
				*b++ = 0;
			n++;
		}
	}

	obj[1].relocation_count = n;
	*b++ = MI_BATCH_BUFFER_END;
	munmap(batch, batch_size);

	memset(&execbuf, 0, sizeof(execbuf));
	execbuf.buffers_ptr = to_user_pointer(obj);
	execbuf.buffer_count = 2;
	execbuf.flags = e->flags;
	execbuf.rsvd1 = ctx->id;
	gem_execbuf(fd, &execbuf);
	gem_close(fd, obj[1].handle);
	free(reloc);

	return obj[0].handle;
}

static void write_regs(int fd,
		       const intel_ctx_t *ctx,
		       const struct intel_execution_engine2 *e,
		       unsigned int flags,
		       uint32_t value)
{
	const unsigned int gen_bit = 1 << intel_gen(intel_get_drm_devid(fd));
	const unsigned int engine_bit = ENGINE(e->class, e->instance);
	const uint32_t mmio_base = gem_engine_mmio_base(fd, e->name);
	struct drm_i915_gem_exec_object2 obj;
	struct drm_i915_gem_execbuffer2 execbuf;
	unsigned int batch_size;
	uint32_t *batch, *b;

	batch_size = NUM_REGS * 3 * sizeof(uint32_t) + 4;
	batch_size = PAGE_ALIGN(batch_size);

	memset(&obj, 0, sizeof(obj));
	obj.handle = gem_create(fd, batch_size);

	b = batch = gem_mmap__cpu(fd, obj.handle, 0, batch_size, PROT_WRITE);
	gem_set_domain(fd, obj.handle,
		       I915_GEM_DOMAIN_CPU, I915_GEM_DOMAIN_CPU);
	for (const struct named_register *r = nonpriv_registers; r->name; r++) {
		uint32_t offset;

		if (!(r->engine_mask & engine_bit))
			continue;
		if (!(r->gen_mask & gen_bit))
			continue;
		if (r->relative && !mmio_base)
			continue;

		offset = r->offset;
		if (r->relative)
			offset += mmio_base;

		for (unsigned count = r->count ?: 1; count--; offset += 4) {
			uint32_t x = value;
			if (r->write_mask)
				x &= r->write_mask;
			if (r->masked)
				x |= 0xffffu << 16;

			*b++ = 0x22 << 23 | 1; /* LRI */
			*b++ = offset;
			*b++ = x;
		}
	}
	*b++ = MI_BATCH_BUFFER_END;
	munmap(batch, batch_size);

	memset(&execbuf, 0, sizeof(execbuf));
	execbuf.buffers_ptr = to_user_pointer(&obj);
	execbuf.buffer_count = 1;
	execbuf.flags = e->flags;
	execbuf.rsvd1 = ctx->id;
	gem_execbuf(fd, &execbuf);
	gem_close(fd, obj.handle);
}

static void restore_regs(int fd,
			 const intel_ctx_t *ctx,
			 const struct intel_execution_engine2 *e,
			 unsigned int flags,
			 uint32_t regs)
{
	const unsigned int gen = intel_gen(intel_get_drm_devid(fd));
	const unsigned int gen_bit = 1 << gen;
	const unsigned int engine_bit = ENGINE(e->class, e->instance);
	const uint32_t mmio_base = gem_engine_mmio_base(fd, e->name);
	const bool r64b = gen >= 8;
	struct drm_i915_gem_exec_object2 obj[2];
	struct drm_i915_gem_execbuffer2 execbuf;
	struct drm_i915_gem_relocation_entry *reloc;
	unsigned int batch_size, n;
	uint32_t *batch, *b;

	if (gen < 7) /* no LRM */
		return;

	reloc = calloc(NUM_REGS, sizeof(*reloc));
	igt_assert(reloc);

	batch_size = NUM_REGS * 3 * sizeof(uint32_t) + 4;
	batch_size = PAGE_ALIGN(batch_size);

	memset(obj, 0, sizeof(obj));
	obj[0].handle = regs;
	obj[1].handle = gem_create(fd, batch_size);
	obj[1].relocs_ptr = to_user_pointer(reloc);

	b = batch = gem_mmap__cpu(fd, obj[1].handle, 0, batch_size, PROT_WRITE);
	gem_set_domain(fd, obj[1].handle,
		       I915_GEM_DOMAIN_CPU, I915_GEM_DOMAIN_CPU);

	n = 0;
	for (const struct named_register *r = nonpriv_registers; r->name; r++) {
		uint32_t offset;

		if (!(r->engine_mask & engine_bit))
			continue;
		if (!(r->gen_mask & gen_bit))
			continue;
		if (r->relative && !mmio_base)
			continue;

		offset = r->offset;
		if (r->relative)
			offset += mmio_base;

		for (unsigned count = r->count ?: 1; count--; offset += 4) {
			*b++ = 0x29 << 23 | (1 + r64b); /* LRM */
			*b++ = offset;
			reloc[n].target_handle = obj[0].handle;
			reloc[n].presumed_offset = 0;
			reloc[n].offset = (b - batch) * sizeof(*b);
			reloc[n].delta = offset;
			reloc[n].read_domains = I915_GEM_DOMAIN_RENDER;
			reloc[n].write_domain = 0;
			*b++ = offset;
			if (r64b)
				*b++ = 0;
			n++;
		}
	}
	obj[1].relocation_count = n;
	*b++ = MI_BATCH_BUFFER_END;
	munmap(batch, batch_size);

	memset(&execbuf, 0, sizeof(execbuf));
	execbuf.buffers_ptr = to_user_pointer(obj);
	execbuf.buffer_count = 2;
	execbuf.flags = e->flags;
	execbuf.rsvd1 = ctx->id;
	gem_execbuf(fd, &execbuf);
	gem_close(fd, obj[1].handle);
}

__attribute__((unused))
static void dump_regs(int fd,
		      const struct intel_execution_engine2 *e,
		      unsigned int regs)
{
	const unsigned int gen = intel_gen(intel_get_drm_devid(fd));
	const unsigned int gen_bit = 1 << gen;
	const unsigned int engine_bit = ENGINE(e->class, e->instance);
	const uint32_t mmio_base = gem_engine_mmio_base(fd, e->name);
	unsigned int regs_size;
	uint32_t *out;

	regs_size = NUM_REGS * sizeof(uint32_t);
	regs_size = PAGE_ALIGN(regs_size);

	out = gem_mmap__cpu(fd, regs, 0, regs_size, PROT_READ);
	gem_set_domain(fd, regs, I915_GEM_DOMAIN_CPU, 0);

	for (const struct named_register *r = nonpriv_registers; r->name; r++) {
		uint32_t offset;

		if (!(r->engine_mask & engine_bit))
			continue;
		if (!(r->gen_mask & gen_bit))
			continue;
		if (r->relative && !mmio_base)
			continue;

		offset = r->offset;
		if (r->relative)
			offset += mmio_base;

		if (r->count <= 1) {
			igt_debug("0x%04x (%s): 0x%08x\n",
				  offset, r->name, out[offset / 4]);
		} else {
			for (unsigned x = 0; x < r->count; x++)
				igt_debug("0x%04x (%s[%d]): 0x%08x\n",
					  offset + 4 * x, r->name, x,
					  out[offset / 4 + x]);
		}
	}
	munmap(out, regs_size);
}

static void compare_regs(int fd, const struct intel_execution_engine2 *e,
			 uint32_t A, uint32_t B, const char *who)
{
	const uint32_t mmio_base = gem_engine_mmio_base(fd, e->name);
	unsigned int num_errors;
	unsigned int regs_size;
	uint32_t *a, *b;
	char buf[80];

	regs_size = NUM_REGS * sizeof(uint32_t);
	regs_size = PAGE_ALIGN(regs_size);

	a = gem_mmap__cpu(fd, A, 0, regs_size, PROT_READ);
	gem_set_domain(fd, A, I915_GEM_DOMAIN_CPU, 0);

	b = gem_mmap__cpu(fd, B, 0, regs_size, PROT_READ);
	gem_set_domain(fd, B, I915_GEM_DOMAIN_CPU, 0);

	num_errors = 0;
	for (unsigned int n = 0; n < NUM_REGS; n++) {
		const struct named_register *r;
		uint32_t offset = n * sizeof(uint32_t);
		uint32_t mask;

		if (a[n] == b[n])
			continue;

		if (ignore_register(offset, mmio_base))
			continue;

		mask = ~0u;
		r = lookup_register(offset, mmio_base);
		if (r && r->masked)
			mask >>= 16;
		if (r && r->ignore_bits)
			mask &= ~r->ignore_bits;

		if ((a[n] & mask) == (b[n] & mask))
			continue;

		igt_warn("Register 0x%04x (%s): A=%08x B=%08x\n",
			 offset,
			 register_name(offset, mmio_base, buf, sizeof(buf)),
			 a[n] & mask, b[n] & mask);
		num_errors++;
	}
	munmap(b, regs_size);
	munmap(a, regs_size);

	igt_assert_f(num_errors == 0,
		     "%d registers mistached between %s.\n",
		     num_errors, who);
}

static void nonpriv(int fd, const intel_ctx_cfg_t *cfg,
		    const struct intel_execution_engine2 *e,
		    unsigned int flags)
{
	static const uint32_t values[] = {
		0x0,
		0xffffffff,
		0xcccccccc,
		0x33333333,
		0x55555555,
		0xaaaaaaaa,
		0xf0f00f0f,
		0xa0a00303,
		0x0505c0c0,
		0xdeadbeef
	};
	unsigned int num_values = ARRAY_SIZE(values);

	/* Sigh -- hsw: we need cmdparser access to our own registers! */
	igt_skip_on(intel_gen(intel_get_drm_devid(fd)) < 8);

	gem_quiescent_gpu(fd);

	for (int v = 0; v < num_values; v++) {
		igt_spin_t *spin = NULL;
		const intel_ctx_t *ctx;
		uint32_t regs[2], tmpl;

		ctx = intel_ctx_create(fd, cfg);

		tmpl = read_regs(fd, ctx, e, flags);
		regs[0] = read_regs(fd, ctx, e, flags);

		tmpl_regs(fd, e, tmpl, values[v]);

		spin = igt_spin_new(fd, .ctx = ctx, .engine = e->flags);

		igt_debug("%s[%d]: Setting all registers to 0x%08x\n",
			  __func__, v, values[v]);
		write_regs(fd, ctx, e, flags, values[v]);

		if (flags & DIRTY2) {
			const intel_ctx_t *sw = intel_ctx_create(fd, &ctx->cfg);
			igt_spin_t *syncpt, *dirt;

			/* Explicit sync to keep the switch between write/read */
			syncpt = igt_spin_new(fd,
					      .ctx = ctx,
					      .engine = e->flags,
					      .flags = IGT_SPIN_FENCE_OUT);

			dirt = igt_spin_new(fd,
					    .ctx = sw,
					    .engine = e->flags,
					    .fence = syncpt->out_fence,
					    .flags = (IGT_SPIN_FENCE_IN |
						      IGT_SPIN_FENCE_OUT));
			igt_spin_free(fd, syncpt);

			syncpt = igt_spin_new(fd,
					      .ctx = ctx,
					      .engine = e->flags,
					      .fence = dirt->out_fence,
					      .flags = IGT_SPIN_FENCE_IN);
			igt_spin_free(fd, dirt);

			igt_spin_free(fd, syncpt);
			intel_ctx_destroy(fd, sw);
		}

		regs[1] = read_regs(fd, ctx, e, flags);

		/*
		 * Restore the original register values before the HW idles.
		 * Or else it may never restart!
		 */
		restore_regs(fd, ctx, e, flags, regs[0]);

		igt_spin_free(fd, spin);

		compare_regs(fd, e, tmpl, regs[1], "nonpriv read/writes");

		for (int n = 0; n < ARRAY_SIZE(regs); n++)
			gem_close(fd, regs[n]);
		intel_ctx_destroy(fd, ctx);
		gem_close(fd, tmpl);
	}
}

static void isolation(int fd, const intel_ctx_cfg_t *cfg,
		      const struct intel_execution_engine2 *e,
		      unsigned int flags)
{
	static const uint32_t values[] = {
		0x0,
		0xffffffff,
		0xcccccccc,
		0x33333333,
		0x55555555,
		0xaaaaaaaa,
		0xdeadbeef
	};
	unsigned int num_values =
		flags & (DIRTY1 | DIRTY2) ? ARRAY_SIZE(values) : 1;

	gem_quiescent_gpu(fd);

	for (int v = 0; v < num_values; v++) {
		igt_spin_t *spin = NULL;
		const intel_ctx_t *ctx[2];
		uint32_t regs[2], tmp;

		ctx[0] = intel_ctx_create(fd, cfg);
		regs[0] = read_regs(fd, ctx[0], e, flags);

		spin = igt_spin_new(fd, .ctx = ctx[0], .engine = e->flags);

		if (flags & DIRTY1) {
			igt_debug("%s[%d]: Setting all registers of ctx 0 to 0x%08x\n",
				  __func__, v, values[v]);
			write_regs(fd, ctx[0], e, flags, values[v]);
		}

		/*
		 * We create and execute a new context, whilst the HW is
		 * occupied with the previous context (we should switch from
		 * the old to the new proto-context without idling, which could
		 * then load the powercontext). If all goes well, we only see
		 * the default values from this context, but if goes badly we
		 * see the corruption from the previous context instead!
		 */
		ctx[1] = intel_ctx_create(fd, cfg);
		regs[1] = read_regs(fd, ctx[1], e, flags);

		if (flags & DIRTY2) {
			igt_debug("%s[%d]: Setting all registers of ctx 1 to 0x%08x\n",
				  __func__, v, ~values[v]);
			write_regs(fd, ctx[1], e, flags, ~values[v]);
		}

		/*
		 * Restore the original register values before the HW idles.
		 * Or else it may never restart!
		 */
		tmp = read_regs(fd, ctx[0], e, flags);
		restore_regs(fd, ctx[0], e, flags, regs[0]);

		igt_spin_free(fd, spin);

		if (!(flags & DIRTY1))
			compare_regs(fd, e, regs[0], tmp,
				     "two reads of the same ctx");
		compare_regs(fd, e, regs[0], regs[1], "two virgin contexts");

		for (int n = 0; n < ARRAY_SIZE(ctx); n++) {
			gem_close(fd, regs[n]);
			intel_ctx_destroy(fd, ctx[n]);
		}
		gem_close(fd, tmp);
	}
}

#define NOSLEEP (0 << 8)
#define S3_DEVICES (1 << 8)
#define S3 (2 << 8)
#define S4_DEVICES (3 << 8)
#define S4 (4 << 8)
#define SLEEP_MASK (0xf << 8)

static const intel_ctx_t *
create_reset_context(int i915, const intel_ctx_cfg_t *cfg)
{
	const intel_ctx_t *ctx = intel_ctx_create(i915, cfg);
	struct drm_i915_gem_context_param param = {
		.ctx_id = ctx->id,
		.param = I915_CONTEXT_PARAM_BANNABLE,
	};

	gem_context_set_param(i915, &param);
	return ctx;
}

static void inject_reset_context(int fd, const intel_ctx_cfg_t *cfg,
				 const struct intel_execution_engine2 *e)
{
	const intel_ctx_t *ctx = create_reset_context(fd, cfg);
	struct igt_spin_factory opts = {
		.ctx = ctx,
		.engine = e->flags,
		.flags = IGT_SPIN_FAST,
	};
	igt_spin_t *spin;

	/*
	 * Force a context switch before triggering the reset, or else
	 * we risk corrupting the target context and we can't blame the
	 * HW for screwing up if the context was already broken.
	 */

	if (gem_class_can_store_dword(fd, e->class))
		opts.flags |= IGT_SPIN_POLL_RUN;

	spin = __igt_spin_factory(fd, &opts);

	if (igt_spin_has_poll(spin))
		igt_spin_busywait_until_started(spin);
	else
		usleep(1000); /* better than nothing */

	igt_force_gpu_reset(fd);

	igt_spin_free(fd, spin);
	intel_ctx_destroy(fd, ctx);
}

static void preservation(int fd, const intel_ctx_cfg_t *cfg,
			 const struct intel_execution_engine2 *e,
			 unsigned int flags)
{
	static const uint32_t values[] = {
		0x0,
		0xffffffff,
		0xcccccccc,
		0x33333333,
		0x55555555,
		0xaaaaaaaa,
		0xdeadbeef
	};
	const unsigned int num_values = ARRAY_SIZE(values);
	const intel_ctx_t *ctx[num_values + 1];
	uint32_t regs[num_values + 1][2];
	igt_spin_t *spin;

	gem_quiescent_gpu(fd);

	ctx[num_values] = intel_ctx_create(fd, cfg);
	spin = igt_spin_new(fd, .ctx = ctx[num_values], .engine = e->flags);
	regs[num_values][0] = read_regs(fd, ctx[num_values], e, flags);
	for (int v = 0; v < num_values; v++) {
		ctx[v] = intel_ctx_create(fd, cfg);
		write_regs(fd, ctx[v], e, flags, values[v]);

		regs[v][0] = read_regs(fd, ctx[v], e, flags);

	}
	gem_close(fd, read_regs(fd, ctx[num_values], e, flags));
	igt_spin_free(fd, spin);

	if (flags & RESET)
		inject_reset_context(fd, cfg, e);

	switch (flags & SLEEP_MASK) {
	case NOSLEEP:
		break;

	case S3_DEVICES:
		igt_system_suspend_autoresume(SUSPEND_STATE_MEM,
					      SUSPEND_TEST_DEVICES);
		break;

	case S3:
		igt_system_suspend_autoresume(SUSPEND_STATE_MEM,
					      SUSPEND_TEST_NONE);
		break;

	case S4_DEVICES:
		igt_system_suspend_autoresume(SUSPEND_STATE_DISK,
					      SUSPEND_TEST_DEVICES);
		break;

	case S4:
		igt_system_suspend_autoresume(SUSPEND_STATE_DISK,
					      SUSPEND_TEST_NONE);
		break;
	}

	spin = igt_spin_new(fd, .ctx = ctx[num_values], .engine = e->flags);
	for (int v = 0; v < num_values; v++)
		regs[v][1] = read_regs(fd, ctx[v], e, flags);
	regs[num_values][1] = read_regs(fd, ctx[num_values], e, flags);
	igt_spin_free(fd, spin);

	for (int v = 0; v < num_values; v++) {
		char buf[80];

		snprintf(buf, sizeof(buf), "dirty %x context\n", values[v]);
		compare_regs(fd, e, regs[v][0], regs[v][1], buf);

		gem_close(fd, regs[v][0]);
		gem_close(fd, regs[v][1]);
		intel_ctx_destroy(fd, ctx[v]);
	}
	compare_regs(fd, e, regs[num_values][0], regs[num_values][1], "clean");
	intel_ctx_destroy(fd, ctx[num_values]);
}

static unsigned int __has_context_isolation(int fd)
{
	struct drm_i915_getparam gp;
	int value = 0;

	memset(&gp, 0, sizeof(gp));
	gp.param = 50; /* I915_PARAM_HAS_CONTEXT_ISOLATION */
	gp.value = &value;

	igt_ioctl(fd, DRM_IOCTL_I915_GETPARAM, &gp);
	errno = 0;

	return value;
}

#define test_each_engine(e, i915, cfg, mask) \
	for_each_ctx_cfg_engine(i915, cfg, e) \
		for_each_if(mask & (1 << (e)->class)) \
			igt_dynamic_f("%s", (e)->name)

igt_main
{
	unsigned int has_context_isolation = 0;
	const struct intel_execution_engine2 *e;
	intel_ctx_cfg_t cfg;
	int i915 = -1;

	igt_fixture {
		int gen;

		i915 = drm_open_driver(DRIVER_INTEL);
		igt_require_gem(i915);
		igt_require(gem_has_contexts(i915));
		cfg = intel_ctx_cfg_all_physical(i915);

		has_context_isolation = __has_context_isolation(i915);
		igt_require(has_context_isolation);

		gen = intel_gen(intel_get_drm_devid(i915));

		igt_warn_on_f(gen > LAST_KNOWN_GEN,
			      "GEN not recognized! Test needs to be updated to run.\n");
		igt_skip_on(gen > LAST_KNOWN_GEN);
	}

	igt_fixture {
		igt_fork_hang_detector(i915);
	}

	igt_subtest_with_dynamic("nonpriv") {
		test_each_engine(e, i915, &cfg, has_context_isolation)
			nonpriv(i915, &cfg, e, 0);
	}

	igt_subtest_with_dynamic("nonpriv-switch") {
		test_each_engine(e, i915, &cfg, has_context_isolation)
			nonpriv(i915, &cfg, e, DIRTY2);
	}

	igt_subtest_with_dynamic("clean") {
		test_each_engine(e, i915, &cfg, has_context_isolation)
			isolation(i915, &cfg, e, 0);
	}

	igt_subtest_with_dynamic("dirty-create") {
		test_each_engine(e, i915, &cfg, has_context_isolation)
			isolation(i915, &cfg, e, DIRTY1);
	}

	igt_subtest_with_dynamic("dirty-switch") {
		test_each_engine(e, i915, &cfg, has_context_isolation)
			isolation(i915, &cfg, e, DIRTY2);
	}

	igt_subtest_with_dynamic("preservation") {
		test_each_engine(e, i915, &cfg, has_context_isolation)
			preservation(i915, &cfg, e, 0);
	}

	igt_subtest_with_dynamic("preservation-S3") {
		test_each_engine(e, i915, &cfg, has_context_isolation)
			preservation(i915, &cfg, e, S3);
	}

	igt_subtest_with_dynamic("preservation-S4") {
		test_each_engine(e, i915, &cfg, has_context_isolation)
			preservation(i915, &cfg, e, S4);
	}

	igt_fixture {
		igt_stop_hang_detector();
	}

	igt_subtest_with_dynamic("preservation-reset") {
		igt_hang_t hang = igt_allow_hang(i915, 0, 0);

		test_each_engine(e, i915, &cfg, has_context_isolation)
			preservation(i915, &cfg, e, RESET);

		igt_disallow_hang(i915, hang);
	}
}
