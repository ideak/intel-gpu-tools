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
#ifdef __linux__
#include <linux/limits.h>
#endif
#include <signal.h>
#include <libgen.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <fcntl.h>

#include "i915/gem.h"
#include "i915/gem_create.h"
#include "igt_debugfs.h"
#include "igt_aux.h"
#include "igt_kmod.h"
#include "igt_sysfs.h"
#include "igt_core.h"

#define BAR_SIZE_SHIFT 20
#define MIN_BAR_SIZE 256

IGT_TEST_DESCRIPTION("Tests the i915 module loading.");

static void store_all(int i915)
{
	const unsigned int gen = intel_gen(intel_get_drm_devid(i915));
	uint32_t engines[I915_EXEC_RING_MASK + 1];
	uint32_t batch[16];
	uint64_t ahnd, offset, bb_offset;
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
	ahnd = get_reloc_ahnd(i915, ctx->id);
	if (ahnd)
		obj[1].relocation_count = 0;
	bb_offset = get_offset(ahnd, obj[1].handle, sz, 4096);
	offset = get_offset(ahnd, obj[0].handle, sizeof(engines), 0);

	for_each_ctx_engine(i915, ctx, e) {
		uint64_t addr;

		igt_assert(reloc.presumed_offset != -1);
		addr = reloc.presumed_offset + reloc.delta;

		if (!gem_class_can_store_dword(i915, e->class))
			continue;

		if (ahnd) {
			i = 1;
			batch[i++] = offset + reloc.delta;
			batch[i++] = offset >> 32;
			obj[0].offset = offset;
			obj[0].flags |= EXEC_OBJECT_PINNED;
			obj[1].offset = bb_offset;
			obj[1].flags |= EXEC_OBJECT_PINNED;
		}

		batch[value] = nengine;

		execbuf.flags = e->flags;
		if (gen < 6)
			execbuf.flags |= I915_EXEC_SECURE;
		execbuf.flags |= I915_EXEC_NO_RELOC | I915_EXEC_HANDLE_LUT;
		execbuf.rsvd1 = ctx->id;

		memcpy(cs + execbuf.batch_start_offset, batch, sizeof(batch));
		if (!ahnd)
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
	put_offset(ahnd, obj[0].handle);
	put_offset(ahnd, obj[1].handle);
	put_ahnd(ahnd);

	for (i = 0; i < nengine; i++)
		igt_assert_eq_u32(engines[i], i);
}

static int open_parameters(const char *module_name)
{
	char path[256];

	snprintf(path, sizeof(path), "/sys/module/%s/parameters", module_name);
	return open(path, O_RDONLY);
}

static void unload_or_die(const char *module_name)
{
	int err, loop;

	/* should be unloaded, so expect a no-op */
	for (loop = 0;; loop++) {
		err = igt_kmod_unload(module_name, 0);
		if (err == -ENOENT) /* -ENOENT == unloaded already */
			err = 0;
		if (!err || loop >= 10)
			break;

		sleep(1); /* wait for external clients to drop */
		if (!strcmp(module_name, "i915"))
			igt_i915_driver_unload();
	}

	igt_abort_on_f(err,
		       "Failed to unload '%s' err:%d after %ds, leaving dangerous modparams intact!\n",
		       module_name, err, loop);
}

static void must_unload(int sig)
{
	unload_or_die("i915");
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
	struct drm_i915_gem_busy args = {};
	int i915 = __drm_open_driver(DRIVER_INTEL);
	int expected = -ENOENT;
	int err;

	err = 0;
	if (ioctl(i915,DRM_IOCTL_I915_GEM_BUSY, &args))
		err = -errno;
	if (err == expected)
		store_all(i915);
	errno = 0;

	close(i915);
	igt_assert_eq(err, expected);
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

static void load_and_check_i915(void)
{
	int error;
	int drm_fd;

	hda_dynamic_debug(true);
	error = igt_i915_driver_load(NULL);
	hda_dynamic_debug(false);

	igt_assert_eq(error, 0);

	/* driver is ready, check if it's bound */
	drm_fd = __drm_open_driver(DRIVER_INTEL);
	igt_fail_on_f(drm_fd < 0, "Cannot open the i915 DRM driver after modprobing i915.\n");

	/* make sure the GPU is idle */
	gem_quiescent_gpu(drm_fd);
	close(drm_fd);

	/* make sure we can do basic memory ops */
	gem_sanitycheck();
}

static uint32_t  driver_load_with_lmem_bar_size(uint32_t lmem_bar_size, bool check_support)
{
	int i915 = -1;
	char lmem_bar[64];

	igt_i915_driver_unload();
	if (lmem_bar_size == 0)
		igt_assert_eq(igt_i915_driver_load(NULL), 0);
	else {
		sprintf(lmem_bar, "lmem_bar_size=%u", lmem_bar_size);
		igt_assert_eq(igt_i915_driver_load(lmem_bar), 0);
	}

	i915 = __drm_open_driver(DRIVER_INTEL);
	igt_require_fd(i915);
	igt_require_gem(i915);
	igt_require(gem_has_lmem(i915));

	if (check_support) {
		char *tmp;

		tmp = __igt_params_get(i915, "lmem_bar_size");
		if (!tmp)
			igt_skip("lmem_bar_size modparam not supported on this kernel. Skipping the test.\n");
		free(tmp);
	}

	for_each_memory_region(r, i915) {
		if (r->ci.memory_class == I915_MEMORY_CLASS_DEVICE) {
			lmem_bar_size = (r->cpu_size >> BAR_SIZE_SHIFT);

			igt_skip_on_f(lmem_bar_size == 0, "CPU visible size should be greater than zero. Skipping for older kernel.\n");
		}
	}

	close(i915);

	return lmem_bar_size;
}

igt_main
{
	igt_describe("Check if i915 and friends are not yet loaded, then load them.");
	igt_subtest("load") {
		const char * unwanted_drivers[] = {
			"i915",
			"intel-gtt",
			"snd_hda_intel",
			"snd_hdmi_lpe_audio",
			NULL
		};

		for (int i = 0; unwanted_drivers[i] != NULL; i++) {
			igt_skip_on_f(igt_kmod_is_loaded(unwanted_drivers[i]),
			              "%s is already loaded\n", unwanted_drivers[i]);
		}

		load_and_check_i915();
	}

	igt_describe("Verify the basic functionality of i915 driver after it's reloaded.");
	igt_subtest("reload") {
		igt_i915_driver_unload();

		load_and_check_i915();

		/* only default modparams, can leave module loaded */
	}

	igt_describe("Verify that i915 driver can be successfully loaded with disabled display.");
	igt_subtest("reload-no-display") {
		igt_i915_driver_unload();

		igt_assert_eq(igt_i915_driver_load("disable_display=1"), 0);

		igt_i915_driver_unload();
	}

	igt_describe("Verify that i915 driver can be successfully reloaded at least once"
		     " with fault injection.");
	igt_subtest("reload-with-fault-injection") {
		const char *param;
		int i;

		igt_i915_driver_unload();

		/*
		 * inject_fault() leaves the module unloaded, but if that fails
		 * we must abort the run. Otherwise, we leave a dangerous
		 * modparam affecting all subsequent tests causing bizarre
		 * failures.
		 */
		igt_install_exit_handler(must_unload);

		i = 0;
		param = getenv("IGT_SRANDOM");
		if (param)
			i = atoi(param);
		if (!i)
			i = time(NULL);
		igt_info("Using IGT_SRANDOM=%d for randomised faults\n", i);
		srandom(i);

		param = "inject_probe_failure";
		if (!igt_kmod_has_param("i915", param))
			param = "inject_load_failure";
		igt_require(igt_kmod_has_param("i915", param));

		i = 1;
		while (inject_fault("i915", param, i) == 0)
			i += 1 + random() % 17;

		unload_or_die("i915");
	}

	igt_describe("Check whether lmem bar size can be resized to only supported sizes.");
	igt_subtest("resize-bar") {
		uint32_t result_bar_size;
		uint32_t lmem_bar_size;
		int i915 = -1;

		if (igt_kmod_is_loaded("i915")) {
			i915 = __drm_open_driver(DRIVER_INTEL);
			igt_require_fd(i915);
			igt_require_gem(i915);
			igt_require(gem_has_lmem(i915));
			igt_skip_on_f(igt_sysfs_get_num_gt(i915) > 1, "Skips for more than one lmem instance.\n");
			close(i915);
		}

		/* Test for lmem_bar_size modparam support */
		lmem_bar_size = driver_load_with_lmem_bar_size(MIN_BAR_SIZE, true);
		igt_skip_on_f(lmem_bar_size != MIN_BAR_SIZE, "Device lacks PCI resizeable BAR support.\n");

		lmem_bar_size = driver_load_with_lmem_bar_size(0, false);

		lmem_bar_size = roundup_power_of_two(lmem_bar_size);

		igt_skip_on_f(lmem_bar_size == MIN_BAR_SIZE, "Bar is already set to minimum size.\n");

		while (lmem_bar_size > MIN_BAR_SIZE) {
			lmem_bar_size = lmem_bar_size >> 1;

			result_bar_size = driver_load_with_lmem_bar_size(lmem_bar_size, false);

			igt_assert_f(lmem_bar_size == result_bar_size, "Bar couldn't be resized.\n");
		}

		/* Test with unsupported sizes */
		lmem_bar_size = 80;
		result_bar_size = driver_load_with_lmem_bar_size(lmem_bar_size, false);
		igt_assert_f(lmem_bar_size != result_bar_size, "Bar resized to unsupported size.\n");

		lmem_bar_size = 16400;
		result_bar_size = driver_load_with_lmem_bar_size(lmem_bar_size, false);
		igt_assert_f(lmem_bar_size != result_bar_size, "Bar resized to unsupported size.\n");

		igt_i915_driver_unload();
	}

	/* Subtests should unload the module themselves if they use modparams */
}
