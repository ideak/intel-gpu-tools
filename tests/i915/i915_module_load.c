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
 */
#include "igt.h"
#include <dirent.h>
#include <sys/utsname.h>
#include <linux/limits.h>
#include <signal.h>
#include <libgen.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <fcntl.h>

#include "i915/gem_create.h"
#include "igt_debugfs.h"
#include "igt_aux.h"
#include "igt_kmod.h"
#include "igt_sysfs.h"
#include "igt_core.h"

static void store_all(int i915)
{
	const unsigned int gen = intel_gen(intel_get_drm_devid(i915));
	uint32_t engines[I915_EXEC_RING_MASK + 1];
	uint32_t batch[16];
	unsigned int sz = ALIGN(sizeof(batch) * ARRAY_SIZE(engines), 4096);
	struct drm_i915_gem_relocation_entry reloc = {
		.offset = sizeof(uint32_t),
		.read_domains = I915_GEM_DOMAIN_RENDER,
		.write_domain = I915_GEM_DOMAIN_RENDER,
	};
	struct drm_i915_gem_exec_object2 obj[2] = {
		{
			.handle = gem_create(i915, sizeof(engines)),
			.flags = EXEC_OBJECT_WRITE,
		},
		{
			.handle = gem_create(i915, sz),
			.relocation_count = 1,
			.relocs_ptr = to_user_pointer(&reloc),
		},
	};
	struct drm_i915_gem_execbuffer2 execbuf = {
		.buffers_ptr = to_user_pointer(obj),
		.buffer_count = 2,
	};
	const struct intel_execution_engine2 *e;
	const intel_ctx_t *ctx;
	int reloc_sz = sizeof(uint32_t);
	unsigned int nengine, value;
	void *cs;
	int i;

	i = 0;
	batch[i] = MI_STORE_DWORD_IMM | (gen < 6 ? 1 << 22 : 0);
	if (gen >= 8) {
		batch[++i] = 0;
		batch[++i] = 0;
		reloc_sz = sizeof(uint64_t);
	} else if (gen >= 4) {
		batch[++i] = 0;
		batch[++i] = 0;
		reloc.offset += sizeof(uint32_t);
	} else {
		batch[i]--;
		batch[++i] = 0;
	}
	batch[value = ++i] = 0xc0ffee;
	batch[++i] = MI_BATCH_BUFFER_END;

	nengine = 0;
	cs = gem_mmap__device_coherent(i915, obj[1].handle, 0, sz, PROT_WRITE);

	ctx = intel_ctx_create_all_physical(i915);
	for_each_ctx_engine(i915, ctx, e) {
		uint64_t addr;

		igt_assert(reloc.presumed_offset != -1);
		addr = reloc.presumed_offset + reloc.delta;

		if (!gem_class_can_store_dword(i915, e->class))
			continue;

		batch[value] = nengine;

		execbuf.flags = e->flags;
		if (gen < 6)
			execbuf.flags |= I915_EXEC_SECURE;
		execbuf.flags |= I915_EXEC_NO_RELOC | I915_EXEC_HANDLE_LUT;
		execbuf.rsvd1 = ctx->id;

		memcpy(cs + execbuf.batch_start_offset, batch, sizeof(batch));
		memcpy(cs + reloc.offset, &addr, reloc_sz);
		gem_execbuf(i915, &execbuf);

		if (++nengine == ARRAY_SIZE(engines))
			break;

		reloc.delta += sizeof(uint32_t);
		reloc.offset += sizeof(batch);
		execbuf.batch_start_offset += sizeof(batch);
	}
	munmap(cs, sz);
	gem_close(i915, obj[1].handle);

	memset(engines, 0xdeadbeef, sizeof(engines));
	gem_read(i915, obj[0].handle, 0, engines, nengine * sizeof(engines[0]));
	gem_close(i915, obj[0].handle);
	intel_ctx_destroy(i915, ctx);

	for (i = 0; i < nengine; i++)
		igt_assert_eq_u32(engines[i], i);
}

static int open_parameters(const char *module_name)
{
	char path[256];

	snprintf(path, sizeof(path), "/sys/module/%s/parameters", module_name);
	return open(path, O_RDONLY);
}

static int
inject_fault(const char *module_name, const char *opt, int fault)
{
	char buf[1024];
	int dir;

	igt_assert(fault > 0);
	snprintf(buf, sizeof(buf), "%s=%d", opt, fault);

	if (igt_kmod_load(module_name, buf)) {
		igt_warn("Failed to load module '%s' with options '%s'\n",
			 module_name, buf);
		return 1;
	}

	dir = open_parameters(module_name);
	igt_sysfs_scanf(dir, opt, "%d", &fault);
	close(dir);

	igt_debug("Loaded '%s %s', result=%d\n", module_name, buf, fault);

	if (strcmp(module_name, "i915")) /* XXX better ideas! */
		igt_kmod_unload(module_name, 0);
	else
		igt_i915_driver_unload();

	return fault;
}

static void gem_sanitycheck(void)
{
	struct drm_i915_gem_caching args = {};
	int i915 = __drm_open_driver(DRIVER_INTEL);
	int err;

	err = 0;
	if (ioctl(i915, DRM_IOCTL_I915_GEM_SET_CACHING, &args))
		err = -errno;
	if (err == -ENOENT)
		store_all(i915);
	errno = 0;

	close(i915);
	igt_assert_eq(err, -ENOENT);
}

static void
hda_dynamic_debug(bool enable)
{
	FILE *fp;
	const char snd_hda_intel_on[] = "module snd_hda_intel +pf";
	const char snd_hda_core_on[] = "module snd_hda_core +pf";

	const char snd_hda_intel_off[] = "module snd_hda_core =_";
	const char snd_hda_core_off[] = "module snd_hda_intel =_";

	fp = fopen("/sys/kernel/debug/dynamic_debug/control", "w");
	if (!fp) {
		igt_debug("hda dynamic debug not available\n");
		return;
	}

	if (enable) {
		fwrite(snd_hda_intel_on, 1, sizeof(snd_hda_intel_on), fp);
		fwrite(snd_hda_core_on, 1, sizeof(snd_hda_core_on), fp);
	} else {
		fwrite(snd_hda_intel_off, 1, sizeof(snd_hda_intel_off), fp);
		fwrite(snd_hda_core_off, 1, sizeof(snd_hda_core_off), fp);
	}

	fclose(fp);
}

igt_main
{
	igt_subtest("reload") {
		int load_error;

		igt_i915_driver_unload();

		hda_dynamic_debug(true);
		load_error = igt_i915_driver_load(NULL);
		hda_dynamic_debug(false);

		igt_assert_eq(load_error, 0);

		gem_sanitycheck();

		/* only default modparams, can leave module loaded */
	}

	igt_subtest("reload-no-display") {
		igt_i915_driver_unload();

		igt_assert_eq(igt_i915_driver_load("disable_display=1"), 0);

		igt_i915_driver_unload();
	}

	igt_subtest("reload-with-fault-injection") {
		const char *param;
		int i = 0;

		igt_i915_driver_unload();

		param = "inject_probe_failure";
		if (!igt_kmod_has_param("i915", param))
			param = "inject_load_failure";
		igt_require(igt_kmod_has_param("i915", param));

		while (inject_fault("i915", param, ++i) == 0)
			;

		/* We expect to hit at least one fault! */
		igt_assert(i > 1);

		/* inject_fault() leaves the module unloaded */
	}

	/* Subtests should unload the module themselves if they use modparams */
}
