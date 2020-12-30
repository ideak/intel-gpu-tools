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
#include "igt_debugfs.h"
#include "igt_aux.h"
#include "igt_kmod.h"
#include "igt_sysfs.h"
#include "igt_core.h"

#include <dirent.h>
#include <sys/utsname.h>
#include <linux/limits.h>
#include <signal.h>
#include <libgen.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <fcntl.h>

static void store_all(int fd)
{
	const unsigned int gen = intel_gen(intel_get_drm_devid(fd));
	unsigned int permuted[I915_EXEC_RING_MASK + 1];
	unsigned int engines[I915_EXEC_RING_MASK + 1];
	struct drm_i915_gem_exec_object2 obj[2];
	struct drm_i915_gem_relocation_entry reloc[2 * ARRAY_SIZE(engines)];
	struct drm_i915_gem_execbuffer2 execbuf;
	const struct intel_execution_engine2 *e;
	uint32_t batch[16];
	uint64_t offset;
	unsigned nengine;
	int value;
	int i, j;

	memset(&execbuf, 0, sizeof(execbuf));
	execbuf.buffers_ptr = (uintptr_t)obj;
	execbuf.buffer_count = 2;

	memset(reloc, 0, sizeof(reloc));
	memset(obj, 0, sizeof(obj));
	obj[0].handle = gem_create(fd, 4096);
	obj[1].handle = gem_create(fd, 4096);
	obj[1].relocation_count = 1;

	offset = sizeof(uint32_t);
	i = 0;
	batch[i] = MI_STORE_DWORD_IMM | (gen < 6 ? 1 << 22 : 0);
	if (gen >= 8) {
		batch[++i] = 0;
		batch[++i] = 0;
	} else if (gen >= 4) {
		batch[++i] = 0;
		batch[++i] = 0;
		offset += sizeof(uint32_t);
	} else {
		batch[i]--;
		batch[++i] = 0;
	}
	batch[value = ++i] = 0xc0ffee;
	batch[++i] = MI_BATCH_BUFFER_END;

	nengine = 0;
	intel_detect_and_clear_missed_interrupts(fd);
	__for_each_physical_engine(fd, e) {
		if (!gem_class_can_store_dword(fd, e->class))
			continue;

		igt_assert(2 * (nengine + 1) * sizeof(batch) <= 4096);

		engines[nengine] = e->flags;
		if (gen < 6)
			engines[nengine] |= I915_EXEC_SECURE;
		execbuf.flags = engines[nengine];

		j = 2*nengine;
		reloc[j].target_handle = obj[0].handle;
		reloc[j].presumed_offset = ~0;
		reloc[j].offset = j*sizeof(batch) + offset;
		reloc[j].delta = nengine*sizeof(uint32_t);
		reloc[j].read_domains = I915_GEM_DOMAIN_INSTRUCTION;
		reloc[j].write_domain = I915_GEM_DOMAIN_INSTRUCTION;
		obj[1].relocs_ptr = (uintptr_t)&reloc[j];

		batch[value] = 0xdeadbeef;
		gem_write(fd, obj[1].handle, j*sizeof(batch),
			  batch, sizeof(batch));
		execbuf.batch_start_offset = j*sizeof(batch);
		gem_execbuf(fd, &execbuf);

		j = 2*nengine + 1;
		reloc[j].target_handle = obj[0].handle;
		reloc[j].presumed_offset = ~0;
		reloc[j].offset = j*sizeof(batch) + offset;
		reloc[j].delta = nengine*sizeof(uint32_t);
		reloc[j].read_domains = I915_GEM_DOMAIN_INSTRUCTION;
		reloc[j].write_domain = I915_GEM_DOMAIN_INSTRUCTION;
		obj[1].relocs_ptr = (uintptr_t)&reloc[j];

		batch[value] = nengine;
		gem_write(fd, obj[1].handle, j*sizeof(batch),
			  batch, sizeof(batch));
		execbuf.batch_start_offset = j*sizeof(batch);
		gem_execbuf(fd, &execbuf);

		nengine++;
	}
	gem_sync(fd, obj[1].handle);

	for (i = 0; i < nengine; i++) {
		obj[1].relocs_ptr = (uintptr_t)&reloc[2*i];
		execbuf.batch_start_offset = 2*i*sizeof(batch);
		memcpy(permuted, engines, nengine*sizeof(engines[0]));
		igt_permute_array(permuted, nengine, igt_exchange_int);
		for (j = 0; j < nengine; j++) {
			execbuf.flags = permuted[j];
			gem_execbuf(fd, &execbuf);
		}
		obj[1].relocs_ptr = (uintptr_t)&reloc[2*i+1];
		execbuf.batch_start_offset = (2*i+1)*sizeof(batch);
		execbuf.flags = engines[i];
		gem_execbuf(fd, &execbuf);
	}
	gem_close(fd, obj[1].handle);

	gem_read(fd, obj[0].handle, 0, engines, sizeof(engines));
	gem_close(fd, obj[0].handle);

	for (i = 0; i < nengine; i++)
		igt_assert_eq_u32(engines[i], i);
	igt_assert_eq(intel_detect_and_clear_missed_interrupts(fd), 0);
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
	if (err == -ENOENT) {
		igt_fork_hang_detector(i915);
		store_all(i915);
		igt_stop_hang_detector();
	}
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
