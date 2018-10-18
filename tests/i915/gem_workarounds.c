/*
 * Copyright © 2014 Intel Corporation
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
 * Authors:
 *  Arun Siluvery <arun.siluvery@linux.intel.com>
 *
 */

#include "igt.h"

#include <fcntl.h>

#define PAGE_SIZE 4096
#define PAGE_ALIGN(x) ALIGN(x, PAGE_SIZE)

static int gen;

enum operation {
	GPU_RESET,
	SUSPEND_RESUME,
	HIBERNATE_RESUME,
	SIMPLE_READ,
};

struct intel_wa_reg {
	uint32_t addr;
	uint32_t value;
	uint32_t mask;
};

static struct write_only_list {
	unsigned int gen;
	uint32_t addr;
} wo_list[] = {
	{ 10, 0xE5F0 } /* WaForceContextSaveRestoreNonCoherent:cnl */

	/*
	 * FIXME: If you are contemplating adding stuff here
	 * consider this as a temporary solution. You need to
	 * manually check from context image that your workaround
	 * is having an effect. Consider creating a context image
	 * validator to act as a superior solution.
	 */
};

static struct intel_wa_reg *wa_regs;
static int num_wa_regs;

static bool write_only(const uint32_t addr)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(wo_list); i++) {
		if (gen == wo_list[i].gen &&
		    addr == wo_list[i].addr) {
			igt_info("Skipping check for 0x%x due to write only\n", addr);
			return true;
		}
	}

	return false;
}

#define MI_STORE_REGISTER_MEM (0x24 << 23)

static int workaround_fail_count(int fd, uint32_t ctx)
{
	struct drm_i915_gem_exec_object2 obj[2];
	struct drm_i915_gem_relocation_entry *reloc;
	struct drm_i915_gem_execbuffer2 execbuf;
	uint32_t result_sz, batch_sz;
	uint32_t *base, *out;
	int fail_count = 0;

	reloc = calloc(num_wa_regs, sizeof(*reloc));
	igt_assert(reloc);

	result_sz = 4 * num_wa_regs;
	result_sz = PAGE_ALIGN(result_sz);

	batch_sz = 16 * num_wa_regs + 4;
	batch_sz = PAGE_ALIGN(batch_sz);

	memset(obj, 0, sizeof(obj));
	obj[0].handle = gem_create(fd, result_sz);
	gem_set_caching(fd, obj[0].handle, I915_CACHING_CACHED);
	obj[1].handle = gem_create(fd, batch_sz);
	obj[1].relocs_ptr = to_user_pointer(reloc);
	obj[1].relocation_count = num_wa_regs;

	out = base = gem_mmap__cpu(fd, obj[1].handle, 0, batch_sz, PROT_WRITE);
	for (int i = 0; i < num_wa_regs; i++) {
		*out++ = MI_STORE_REGISTER_MEM | ((gen >= 8 ? 4 : 2) - 2);
		*out++ = wa_regs[i].addr;
		reloc[i].target_handle = obj[0].handle;
		reloc[i].offset = (out - base) * sizeof(*out);
		reloc[i].delta = i * sizeof(uint32_t);
		reloc[i].read_domains = I915_GEM_DOMAIN_INSTRUCTION;
		reloc[i].write_domain = I915_GEM_DOMAIN_INSTRUCTION;
		*out++ = reloc[i].delta;
		if (gen >= 8)
			*out++ = 0;
	}
	*out++ = MI_BATCH_BUFFER_END;
	munmap(base, batch_sz);

	memset(&execbuf, 0, sizeof(execbuf));
	execbuf.buffers_ptr = to_user_pointer(obj);
	execbuf.buffer_count = 2;
	execbuf.rsvd1 = ctx;
	gem_execbuf(fd, &execbuf);

	gem_set_domain(fd, obj[0].handle, I915_GEM_DOMAIN_CPU, 0);

	igt_debug("Address\tval\t\tmask\t\tread\t\tresult\n");

	out = gem_mmap__cpu(fd, obj[0].handle, 0, result_sz, PROT_READ);
	for (int i = 0; i < num_wa_regs; i++) {
		const bool ok =
			(wa_regs[i].value & wa_regs[i].mask) ==
			(out[i] & wa_regs[i].mask);
		char buf[80];

		snprintf(buf, sizeof(buf),
			 "0x%05X\t0x%08X\t0x%08X\t0x%08X",
			 wa_regs[i].addr, wa_regs[i].value, wa_regs[i].mask,
			 out[i]);

		if (ok) {
			igt_debug("%s\tOK\n", buf);
		} else if (write_only(wa_regs[i].addr)) {
			igt_debug("%s\tIGNORED (w/o)\n", buf);
		} else {
			igt_warn("%s\tFAIL\n", buf);
			fail_count++;
		}
	}
	munmap(out, result_sz);

	gem_close(fd, obj[1].handle);
	gem_close(fd, obj[0].handle);
	free(reloc);

	return fail_count;
}

static int reopen(int fd)
{
	char path[256];

	snprintf(path, sizeof(path), "/proc/self/fd/%d", fd);
	fd = open(path, O_RDWR);
	igt_assert_lte(0, fd);

	return fd;
}

#define CONTEXT 0x1
#define FD 0x2
static void check_workarounds(int fd, enum operation op, unsigned int flags)
{
	uint32_t ctx = 0;

	if (flags & FD)
		fd = reopen(fd);

	if (flags & CONTEXT) {
		gem_require_contexts(fd);
		ctx = gem_context_create(fd);
	}

	igt_assert_eq(workaround_fail_count(fd, ctx), 0);

	switch (op) {
	case GPU_RESET:
		igt_force_gpu_reset(fd);
		break;

	case SUSPEND_RESUME:
		igt_system_suspend_autoresume(SUSPEND_STATE_MEM,
					      SUSPEND_TEST_NONE);
		break;

	case HIBERNATE_RESUME:
		igt_system_suspend_autoresume(SUSPEND_STATE_DISK,
					      SUSPEND_TEST_NONE);
		break;

	case SIMPLE_READ:
		break;

	default:
		igt_assert(0);
	}

	igt_assert_eq(workaround_fail_count(fd, ctx), 0);

	if (flags & CONTEXT)
		gem_context_destroy(fd, ctx);
	if (flags & FD)
		close(fd);
}

igt_main
{
	int device = -1;
	const struct {
		const char *name;
		enum operation op;
	} ops[] =   {
		{ "basic-read", SIMPLE_READ },
		{ "reset", GPU_RESET },
		{ "suspend-resume", SUSPEND_RESUME },
		{ "hibernate-resume", HIBERNATE_RESUME },
		{ }
	}, *op;
	const struct {
		const char *name;
		unsigned int flags;
	} modes[] =   {
		{ "", 0 },
		{ "-context", CONTEXT },
		{ "-fd", FD },
		{ }
	}, *m;

	igt_fixture {
		FILE *file;
		char *line = NULL;
		size_t line_size;
		int i, fd;

		device = drm_open_driver(DRIVER_INTEL);
		igt_require_gem(device);

		gen = intel_gen(intel_get_drm_devid(device));

		fd = igt_debugfs_open(device, "i915_wa_registers", O_RDONLY);
		file = fdopen(fd, "r");
		igt_assert(getline(&line, &line_size, file) > 0);
		igt_debug("i915_wa_registers: %s", line);
		sscanf(line, "Workarounds applied: %d", &num_wa_regs);
		igt_require(num_wa_regs > 0);

		wa_regs = malloc(num_wa_regs * sizeof(*wa_regs));
		igt_assert(wa_regs);

		i = 0;
		while (getline(&line, &line_size, file) > 0) {
			igt_debug("%s", line);
			if (sscanf(line, "0x%X: 0x%08X, mask: 0x%08X",
				   &wa_regs[i].addr,
				   &wa_regs[i].value,
				   &wa_regs[i].mask) == 3)
				i++;
		}

		igt_assert_lte(i, num_wa_regs);

		free(line);
		fclose(file);
		close(fd);
	}

	for (op = ops; op->name; op++) {
		for (m = modes; m->name; m++) {
			igt_subtest_f("%s%s", op->name, m->name)
				check_workarounds(device, op->op, m->flags);
		}
	}
}
