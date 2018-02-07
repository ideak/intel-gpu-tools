/*
 * Copyright © 2016 Intel Corporation
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

/** @file gem_mocs_settings.c
 *
 * Check that the MOCs cache settings are valid.
 */

#include "igt.h"
#include "igt_gt.h"
#include "igt_sysfs.h"

#define MAX_NUMBER_MOCS_REGISTERS	(64)
enum {
	NONE,
	RESET,
	RC6,
	SUSPEND,
	HIBERNATE,
	MAX_MOCS_TEST_MODES
};

static const char * const test_modes[] = {
	[NONE] = "settings",
	[RESET] = "reset",
	[RC6] = "rc6",
	[SUSPEND] = "suspend",
	[HIBERNATE] = "hibernate"
};

#define MOCS_NON_DEFAULT_CTX	(1<<0)
#define MOCS_DIRTY_VALUES	(1<<1)
#define ALL_MOCS_FLAGS		(MOCS_NON_DEFAULT_CTX | \
				 MOCS_DIRTY_VALUES)

#define GEN9_LNCFCMOCS0		(0xB020)	/* L3 Cache Control base */
#define GEN9_GFX_MOCS_0		(0xc800)	/* Graphics MOCS base register*/
#define GEN9_MFX0_MOCS_0	(0xc900)	/* Media 0 MOCS base register*/
#define GEN9_MFX1_MOCS_0	(0xcA00)	/* Media 1 MOCS base register*/
#define GEN9_VEBOX_MOCS_0	(0xcB00)	/* Video MOCS base register*/
#define GEN9_BLT_MOCS_0		(0xcc00)	/* Blitter MOCS base register*/

struct mocs_entry {
	uint32_t	control_value;
	uint16_t	l3cc_value;
};

struct mocs_table {
	uint32_t		size;
	const struct mocs_entry	*table;
};

/* The first entries in the MOCS tables are defined by uABI */
static const struct mocs_entry skylake_mocs_table[] = {
	{ 0x00000009, 0x0010 },
	{ 0x00000038, 0x0030 },
	{ 0x0000003b, 0x0030 },
};

static const struct mocs_entry dirty_skylake_mocs_table[] = {
	{ 0x00003FFF, 0x003F }, /* no snoop bit */
	{ 0x00003FFF, 0x003F },
	{ 0x00003FFF, 0x003F },
};

static const struct mocs_entry broxton_mocs_table[] = {
	{ 0x00000009, 0x0010 },
	{ 0x00000038, 0x0030 },
	{ 0x00000039, 0x0030 },
};

static const struct mocs_entry dirty_broxton_mocs_table[] = {
	{ 0x00007FFF, 0x003F },
	{ 0x00007FFF, 0x003F },
	{ 0x00007FFF, 0x003F },
};

static const uint32_t write_values[] = {
	0xFFFFFFFF,
	0xFFFFFFFF,
	0xFFFFFFFF,
	0xFFFFFFFF
};

static bool get_mocs_settings(int fd, struct mocs_table *table, bool dirty)
{
	uint32_t devid = intel_get_drm_devid(fd);
	bool result = false;

	if (IS_SKYLAKE(devid) || IS_KABYLAKE(devid)) {
		if (dirty) {
			table->size  = ARRAY_SIZE(dirty_skylake_mocs_table);
			table->table = dirty_skylake_mocs_table;
		} else {
			table->size  = ARRAY_SIZE(skylake_mocs_table);
			table->table = skylake_mocs_table;
		}
		result = true;
	} else if (IS_BROXTON(devid)) {
		if (dirty) {
			table->size  = ARRAY_SIZE(dirty_broxton_mocs_table);
			table->table = dirty_broxton_mocs_table;
		} else {
			table->size  = ARRAY_SIZE(broxton_mocs_table);
			table->table = broxton_mocs_table;
		}
		result = true;
	}

	return result;
}

#define LOCAL_I915_EXEC_BSD1 (I915_EXEC_BSD | (1<<13))
#define LOCAL_I915_EXEC_BSD2 (I915_EXEC_BSD | (2<<13))

static uint32_t get_engine_base(uint32_t engine)
{
	switch (engine) {
	case LOCAL_I915_EXEC_BSD1:	return GEN9_MFX0_MOCS_0;
	case LOCAL_I915_EXEC_BSD2:	return GEN9_MFX1_MOCS_0;
	case I915_EXEC_RENDER:		return GEN9_GFX_MOCS_0;
	case I915_EXEC_BLT:		return GEN9_BLT_MOCS_0;
	case I915_EXEC_VEBOX:		return GEN9_VEBOX_MOCS_0;
	default:			return 0;
	}
}

#define MI_STORE_REGISTER_MEM_64_BIT_ADDR	((0x24 << 23) | 2)

static int create_read_batch(struct drm_i915_gem_relocation_entry *reloc,
			     uint32_t *batch,
			     uint32_t dst_handle,
			     uint32_t size,
			     uint32_t reg_base)
{
	unsigned int offset = 0;

	for (uint32_t index = 0; index < size; index++, offset += 4) {
		batch[offset]   = MI_STORE_REGISTER_MEM_64_BIT_ADDR;
		batch[offset+1] = reg_base + (index * sizeof(uint32_t));
		batch[offset+2] = index * sizeof(uint32_t);	/* reloc */
		batch[offset+3] = 0;

		reloc[index].offset = (offset + 2) * sizeof(uint32_t);
		reloc[index].delta = index * sizeof(uint32_t);
		reloc[index].target_handle = dst_handle;
		reloc[index].write_domain = I915_GEM_DOMAIN_RENDER;
		reloc[index].read_domains = I915_GEM_DOMAIN_RENDER;
	}

	batch[offset++] = MI_BATCH_BUFFER_END;
	batch[offset++] = 0;

	return offset * sizeof(uint32_t);
}

static void do_read_registers(int fd,
			      uint32_t ctx_id,
			      uint32_t dst_handle,
			      uint32_t reg_base,
			      uint32_t size,
			      uint32_t engine_id)
{
	struct drm_i915_gem_execbuffer2 execbuf;
	struct drm_i915_gem_exec_object2 obj[2];
	struct drm_i915_gem_relocation_entry reloc[size];
	uint32_t batch[size * 4 + 4];
	uint32_t handle = gem_create(fd, 4096);

	memset(reloc, 0, sizeof(reloc));
	memset(obj, 0, sizeof(obj));
	memset(&execbuf, 0, sizeof(execbuf));

	obj[0].handle = dst_handle;

	obj[1].handle = handle;
	obj[1].relocation_count = size;
	obj[1].relocs_ptr = to_user_pointer(reloc);

	execbuf.buffers_ptr = to_user_pointer(obj);
	execbuf.buffer_count = 2;
	execbuf.batch_len =
		create_read_batch(reloc, batch, dst_handle, size, reg_base);
	i915_execbuffer2_set_context_id(execbuf, ctx_id);
	execbuf.flags = I915_EXEC_SECURE | engine_id;

	gem_write(fd, handle, 0, batch, execbuf.batch_len);
	gem_execbuf(fd, &execbuf);
	gem_close(fd, handle);
}

#define LOCAL_MI_LOAD_REGISTER_IMM	(0x22 << 23)

static int create_write_batch(uint32_t *batch,
			      const uint32_t *values,
			      uint32_t size,
			      uint32_t reg_base)
{
	unsigned int i;
	unsigned int offset = 0;

	batch[offset++] = LOCAL_MI_LOAD_REGISTER_IMM | (size * 2 - 1);

	for (i = 0; i < size; i++) {
		batch[offset++] = reg_base + (i * 4);
		batch[offset++] = values[i];
	}

	batch[offset++] = MI_BATCH_BUFFER_END;

	return offset * sizeof(uint32_t);
}

static void write_registers(int fd,
			    uint32_t ctx_id,
			    uint32_t reg_base,
			    const uint32_t *values,
			    uint32_t size,
			    uint32_t engine_id)
{
	struct drm_i915_gem_exec_object2 obj;
	struct drm_i915_gem_execbuffer2 execbuf;
	uint32_t batch[size * 4 + 2];
	uint32_t handle = gem_create(fd, 4096);

	memset(&obj, 0, sizeof(obj));
	memset(&execbuf, 0, sizeof(execbuf));

	obj.handle = handle;

	execbuf.buffers_ptr = to_user_pointer(&obj);
	execbuf.buffer_count = 1;
	execbuf.batch_len = create_write_batch(batch, values, size, reg_base);
	i915_execbuffer2_set_context_id(execbuf, ctx_id);
	execbuf.flags = I915_EXEC_SECURE | engine_id;

	gem_write(fd, handle, 0, batch, execbuf.batch_len);
	gem_execbuf(fd, &execbuf);
	gem_close(fd, handle);
}

static void check_control_registers(int fd,
				    unsigned engine,
				    uint32_t ctx_id,
				    bool dirty)
{
	const uint32_t reg_base = get_engine_base(engine);
	uint32_t dst_handle = gem_create(fd, 4096);
	uint32_t *read_regs;
	struct mocs_table table;

	igt_assert(get_mocs_settings(fd, &table, dirty));

	do_read_registers(fd,
			  ctx_id,
			  dst_handle,
			  reg_base,
			  table.size,
			  engine);

	read_regs = gem_mmap__cpu(fd, dst_handle, 0, 4096, PROT_READ);

	gem_set_domain(fd, dst_handle, I915_GEM_DOMAIN_CPU, 0);
	for (int index = 0; index < table.size; index++)
		igt_assert_eq_u32(read_regs[index],
				  table.table[index].control_value);

	munmap(read_regs, 4096);
	gem_close(fd, dst_handle);
}

static void check_l3cc_registers(int fd,
				 unsigned engine,
				 uint32_t ctx_id,
				 bool dirty)
{
	struct mocs_table table;
	uint32_t dst_handle = gem_create(fd, 4096);
	uint32_t *read_regs;
	int index;

	igt_assert(get_mocs_settings(fd, &table, dirty));

	do_read_registers(fd,
			  ctx_id,
			  dst_handle,
			  GEN9_LNCFCMOCS0,
			  (table.size + 1) / 2,
			  engine);

	read_regs = gem_mmap__cpu(fd, dst_handle, 0, 4096, PROT_READ);

	gem_set_domain(fd, dst_handle, I915_GEM_DOMAIN_CPU, 0);

	for (index = 0; index < table.size / 2; index++) {
		igt_assert_eq_u32(read_regs[index] & 0xffff,
				  table.table[index * 2].l3cc_value);
		igt_assert_eq_u32(read_regs[index] >> 16,
				  table.table[index * 2 + 1].l3cc_value);
	}

	if (table.size & 1)
		igt_assert_eq_u32(read_regs[index] & 0xffff,
				  table.table[index * 2].l3cc_value);

	munmap(read_regs, 4096);
	gem_close(fd, dst_handle);
}


static uint32_t rc6_residency(int dir)
{
	return igt_sysfs_get_u32(dir, "power/rc6_residency_ms");
}

static void rc6_wait(int fd)
{
	int sysfs;
	uint32_t residency;

	sysfs = igt_sysfs_open(fd, NULL);
	igt_assert_lte(0, sysfs);

	residency = rc6_residency(sysfs);
	igt_require(igt_wait(rc6_residency(sysfs) != residency, 10000, 2));

	close(sysfs);
}

static void check_mocs_values(int fd, unsigned engine, uint32_t ctx_id, bool dirty)
{
	check_control_registers(fd, engine, ctx_id, dirty);

	if (engine == I915_EXEC_RENDER)
		check_l3cc_registers(fd, engine, ctx_id, dirty);
}

static void write_dirty_mocs(int fd, unsigned engine, uint32_t ctx_id)
{
	write_registers(fd, ctx_id, get_engine_base(engine),
			write_values, ARRAY_SIZE(write_values),
			engine);

	if (engine == I915_EXEC_RENDER)
		write_registers(fd, ctx_id, GEN9_LNCFCMOCS0,
				write_values, ARRAY_SIZE(write_values),
				engine);
}

static void run_test(int fd, unsigned engine, unsigned flags, unsigned mode)
{
	uint32_t ctx_id = 0;
	uint32_t ctx_clean_id;
	uint32_t ctx_dirty_id;

	gem_require_ring(fd, engine);

	/* Skip if we don't know where the registers are for this engine */
	igt_require(get_engine_base(engine));

	if (flags & MOCS_NON_DEFAULT_CTX)
		ctx_id = gem_context_create(fd);

	if (flags & MOCS_DIRTY_VALUES) {
		ctx_dirty_id = gem_context_create(fd);
		write_dirty_mocs(fd, engine, ctx_dirty_id);
		check_mocs_values(fd, engine, ctx_dirty_id, true);
	}

	check_mocs_values(fd, engine, ctx_id, false);

	switch (mode) {
	case NONE:	break;
	case RESET:	igt_force_gpu_reset(fd);	break;
	case SUSPEND:	igt_system_suspend_autoresume(SUSPEND_STATE_MEM,
						      SUSPEND_TEST_NONE); break;
	case HIBERNATE:	igt_system_suspend_autoresume(SUSPEND_STATE_DISK,
						      SUSPEND_TEST_NONE); break;
	case RC6:	rc6_wait(fd);			break;
	}

	check_mocs_values(fd, engine, ctx_id, false);

	if (flags & MOCS_DIRTY_VALUES) {
		ctx_clean_id = gem_context_create(fd);
		check_mocs_values(fd, engine, ctx_dirty_id, true);
		check_mocs_values(fd, engine, ctx_clean_id, false);
		gem_context_destroy(fd, ctx_dirty_id);
		gem_context_destroy(fd, ctx_clean_id);
	}

	if (ctx_id)
		gem_context_destroy(fd, ctx_id);
}

igt_main
{
	const struct intel_execution_engine *e;
	struct mocs_table table;
	int fd = -1;

	igt_fixture {
		fd = drm_open_driver_master(DRIVER_INTEL); /* for SECURE */
		igt_require_gem(fd);
		gem_require_mocs_registers(fd);
		igt_require(get_mocs_settings(fd, &table, false));
	}

	for (e = intel_execution_engines; e->name; e++) {
		/* We don't know which engine will be assigned to us if we're
		 * using plain I915_EXEC_BSD, I915_EXEC_DEFAULT is just
		 * duplicating render
		 */
		if ((e->exec_id == I915_EXEC_BSD && !e->flags) ||
		    e->exec_id == I915_EXEC_DEFAULT)
			continue;

		for (unsigned mode = NONE; mode < MAX_MOCS_TEST_MODES; mode++) {
			for (unsigned flags = 0; flags < ALL_MOCS_FLAGS + 1; flags++) {
				/* Trying to test non-render engines for dirtying MOCS
				 * values from one context having effect on different
				 * context is bound to fail - only render engine is
				 * doing context save/restore of MOCS registers.
				 * Let's also limit testing values on non-default
				 * contexts to render-only.
				 */
				if (flags && e->exec_id != I915_EXEC_RENDER)
					continue;

				igt_subtest_f("mocs-%s%s%s-%s",
					      test_modes[mode],
					      flags & MOCS_NON_DEFAULT_CTX ? "-ctx": "",
					      flags & MOCS_DIRTY_VALUES ? "-dirty" : "",
					      e->name) {
					if (flags & (MOCS_NON_DEFAULT_CTX | MOCS_DIRTY_VALUES))
						gem_require_contexts(fd);

					run_test(fd, e->exec_id | e->flags, flags, mode);
				}
			}
		}
	}

	igt_fixture
		close(fd);
}
