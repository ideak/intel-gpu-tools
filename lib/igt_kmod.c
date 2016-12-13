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
#include "igt_core.h"
#include "igt_sysfs.h"
#include "igt_kmod.h"

#include <signal.h>

/**
 * SECTION:igt_kmod
 * @short_description: Wrappers around libkmod for module loading/unloading
 * @title: kmod
 * @include: igt.h
 *
 * This library provides helpers to load/unload module driver.
 *
 * Note on loading/reloading:
 *
 * Loading/unload/reloading the driver requires that resources to /dev/dri to
 * be released (closed). A potential mistake would be to submit commands to the
 * GPU by having a fd returned by @drm_open_driver, which is closed by atexit
 * signal handler so reloading/unloading the driver will fail if performed
 * afterwards. One possible solution to this issue is to use
 * @__drm_open_driver() or use @igt_set_module_param() to set module parameters
 * dynamically.
 */

static void squelch(void *data, int priority,
		    const char *file, int line, const char *fn,
		    const char *format, va_list args)
{
}

static struct kmod_ctx *kmod_ctx(void)
{
	static struct kmod_ctx *ctx;

	if (!ctx) {
		ctx = kmod_new(NULL, NULL);
		igt_assert(ctx != NULL);

		kmod_set_log_fn(ctx, squelch, NULL);
	}

	return ctx;
}

/**
 * igt_kmod_is_loaded:
 * @mod_name: The name of the module.
 *
 * Returns: True in case the module has been found or false otherwise.
 *
 * Function to check the existance of module @mod_name in list of loaded kernel
 * modules.
 *
 */
bool
igt_kmod_is_loaded(const char *mod_name)
{
	struct kmod_ctx *ctx = kmod_ctx();
	struct kmod_list *mod, *list;
	bool ret = false;

	if (kmod_module_new_from_loaded(ctx, &list) < 0)
		goto out;

	kmod_list_foreach(mod, list) {
		struct kmod_module *kmod = kmod_module_get_module(mod);
		const char *kmod_name = kmod_module_get_name(kmod);

		if (!strncmp(kmod_name, mod_name, strlen(kmod_name))) {
			kmod_module_unref(kmod);
			ret = true;
			break;
		}
		kmod_module_unref(kmod);
	}
	kmod_module_unref_list(list);
out:
	return ret;
}

static int modprobe(struct kmod_module *kmod, const char *options)
{
	return kmod_module_probe_insert_module(kmod, 0, options,
					       NULL, NULL, NULL);
}

/**
 * igt_kmod_load:
 * @mod_name: The name of the module
 * @opts: Parameters for the module. NULL in case no parameters
 * are to be passed, or a '\0' terminated string otherwise.
 *
 * Returns: 0 in case of success or -errno in case the module could not
 * be loaded.
 *
 * This function loads a kernel module using the name specified in @mod_name.
 *
 * @Note: This functions doesn't automatically resolve other module
 * dependencies so make make sure you load the dependencies module(s) before
 * this one.
 */
int
igt_kmod_load(const char *mod_name, const char *opts)
{
	struct kmod_ctx *ctx = kmod_ctx();
	struct kmod_module *kmod;
	int err = 0;

	err = kmod_module_new_from_name(ctx, mod_name, &kmod);
	if (err < 0)
		goto out;

	err = modprobe(kmod, opts);
	if (err < 0) {
		switch (err) {
		case -EEXIST:
			igt_debug("Module %s already inserted\n",
				  kmod_module_get_name(kmod));
			break;
		case -ENOENT:
			igt_debug("Unknown symbol in module %s or "
				  "unknown parameter\n",
				  kmod_module_get_name(kmod));
			break;
		default:
			igt_debug("Could not insert %s (%s)\n",
				  kmod_module_get_name(kmod), strerror(-err));
			break;
		}
	}
out:
	kmod_module_unref(kmod);
	return -err ? err < 0 : err;
}


/**
 * igt_kmod_unload:
 * @mod_name: Module name.
 * @flags: flags are passed directly to libkmod and can be:
 * KMOD_REMOVE_FORCE or KMOD_REMOVE_NOWAIT.
 *
 * Returns: 0 in case of success or -errno otherwise.
 *
 * Removes the module @mod_name.
 *
 */
int
igt_kmod_unload(const char *mod_name, unsigned int flags)
{
	struct kmod_ctx *ctx = kmod_ctx();
	struct kmod_module *kmod;
	int err;

	err = kmod_module_new_from_name(ctx, mod_name, &kmod);
	if (err < 0) {
		igt_debug("Could not use module %s (%s)\n", mod_name,
			  strerror(-err));
		goto out;
	}

	err = kmod_module_remove_module(kmod, flags);
	if (err < 0) {
		igt_debug("Could not remove module %s (%s)\n", mod_name,
			  strerror(-err));
	}

out:
	kmod_module_unref(kmod);
	return -err ? err < 0 : err;
}

/**
 *
 * igt_kmod_list_loaded: List all modules currently loaded.
 *
 */
void
igt_kmod_list_loaded(void)
{
	struct kmod_ctx *ctx = kmod_ctx();
	struct kmod_list *module, *list;

	if (kmod_module_new_from_loaded(ctx, &list) < 0)
		return;

	igt_info("Module\t\t      Used by\n");

	kmod_list_foreach(module, list) {
		struct kmod_module *kmod = kmod_module_get_module(module);
		struct kmod_list *module_deps, *module_deps_list;

		igt_info("%-24s", kmod_module_get_name(kmod));
		module_deps_list = kmod_module_get_holders(kmod);
		if (module_deps_list) {

			kmod_list_foreach(module_deps, module_deps_list) {
				struct kmod_module *kmod_dep;

				kmod_dep = kmod_module_get_module(module_deps);
				igt_info("%s", kmod_module_get_name(kmod_dep));

				if (kmod_list_next(module_deps_list, module_deps))
					igt_info(",");

				kmod_module_unref(kmod_dep);
			}
		}
		kmod_module_unref_list(module_deps_list);

		igt_info("\n");
		kmod_module_unref(kmod);
	}

	kmod_module_unref_list(list);
}

/**
 * igt_i915_driver_load:
 * @opts: options to pass to i915 driver
 *
 * Loads the i915 driver and its dependencies.
 *
 */
int
igt_i915_driver_load(const char *opts)
{
	if (opts)
		igt_info("Reloading i915 with %s\n\n", opts);

	if (igt_kmod_load("i915", opts)) {
		igt_warn("Could not load i915\n");
		return IGT_EXIT_FAILURE;
	}

	kick_fbcon(true);
	igt_kmod_load("snd_hda_intel", NULL);

	return IGT_EXIT_SUCCESS;
}

/**
 * igt_i915_driver_unload:
 *
 * Unloads the i915 driver and its dependencies.
 *
 */
int
igt_i915_driver_unload(void)
{
	/* unbind vt */
	kick_fbcon(false);

	if (igt_kmod_is_loaded("snd_hda_intel")) {
		igt_terminate_process(SIGTERM, "alsactl");

		/* unbind snd_hda_intel */
		kick_snd_hda_intel();

		if (igt_kmod_unload("snd_hda_intel", 0)) {
			igt_warn("Could not unload snd_hda_intel\n");
			igt_kmod_list_loaded();
			igt_lsof("/dev/snd");
			return IGT_EXIT_FAILURE;
		}
	}

	/* gen5 */
	if (igt_kmod_is_loaded("intel_ips"))
		igt_kmod_unload("intel_ips", 0);

	if (igt_kmod_is_loaded("i915")) {
		if (igt_kmod_unload("i915", 0)) {
			igt_warn("Could not unload i915\n");
			igt_kmod_list_loaded();
			igt_lsof("/dev/dri");
			return IGT_EXIT_SKIP;
		}
	}

	if (igt_kmod_is_loaded("intel-gtt"))
		igt_kmod_unload("intel-gtt", 0);

	igt_kmod_unload("drm_kms_helper", 0);
	igt_kmod_unload("drm", 0);

	if (igt_kmod_is_loaded("i915")) {
		igt_warn("i915.ko still loaded!\n");
		return IGT_EXIT_FAILURE;
	}

	return IGT_EXIT_SUCCESS;
}

static void kmsg_dump(int fd)
{
	FILE *file;

	file = NULL;
	if (fd != -1)
		file = fdopen(fd, "r");
	if (file) {
		size_t len = 0;
		char *line = NULL;

		while (getline(&line, &len, file) != -1) {
			char *start = strchr(line, ':');
			if (start)
				igt_warn("%s", start + 2);
		}

		free(line);
		fclose(file);
	} else {
		igt_warn("Unable to retrieve kernel log (from /dev/kmsg)\n");
	}
}

struct test_list {
	struct igt_list link;
	unsigned int number;
	char *name;
	char param[];
};

static void tests_add(struct test_list *tl, struct igt_list *list)
{
	struct test_list *pos;

	igt_list_for_each(pos, list, link)
		if (pos->number > tl->number)
			break;

	igt_list_add_tail(&tl->link, &pos->link);
}

void igt_kselftests(const char *module_name,
		    const char *module_options,
		    const char *filter)
{
	const char *param_prefix = "igt__";
	const int prefix_len = strlen(param_prefix);
	IGT_LIST(tests);
	char options[1024];
	struct kmod_ctx *ctx = kmod_ctx();
	struct kmod_module *kmod;
	struct kmod_list *d, *pre;
	struct test_list *tl, *tn;
	int err, kmsg = -1;

	igt_require(kmod_module_new_from_name(ctx, module_name, &kmod) == 0);
	igt_fixture {
		if (strcmp(module_name, "i915") == 0)
			igt_i915_driver_unload();

		err = kmod_module_remove_module(kmod, KMOD_REMOVE_FORCE);
		igt_require(err == 0 || err == -ENOENT);

		kmsg = open("/dev/kmsg", O_RDONLY | O_NONBLOCK);
	}

	pre = NULL;
	if (kmod_module_get_info(kmod, &pre)) {
		kmod_list_foreach(d, pre) {
			const char *key, *val;
			char *colon;
			int offset;

			key = kmod_module_info_get_key(d);
			if (strcmp(key, "parmtype"))
				continue;

			val = kmod_module_info_get_value(d);
			if (!val || strncmp(val, param_prefix, prefix_len))
				continue;

			if (filter &&
			    strncmp(val + prefix_len, filter, strlen(filter)))
				continue;

			offset = strlen(val) + 1;
			tl = malloc(sizeof(*tl) + offset);
			if (!tl)
				continue;

			memcpy(tl->param, val, offset);
			colon = strchr(tl->param, ':');
			*colon = '\0';

			tl->number = 0;
			tl->name = tl->param + prefix_len;
			if (sscanf(tl->name, "%u__%n",
				   &tl->number, &offset) == 1)
				tl->name += offset;

			tests_add(tl, &tests);
		}
		kmod_module_info_free_list(pre);

		igt_list_for_each_safe(tl, tn, &tests, link) {
			igt_subtest_f("%s", tl->name) {
				lseek(kmsg, 0, SEEK_END);

				snprintf(options, sizeof(options), "%s=1 %s",
					 tl->param, module_options ?: "");

				err = 0;
				if (modprobe(kmod, options))
					err = -errno;
				kmod_module_remove_module(kmod, 0);

				if (err == -ENOTTY) /* special case */
					err = 0;
				if (err)
					kmsg_dump(kmsg);

				errno = 0;
				igt_assert_f(err == 0,
					     "kselftest \"%s %s\" failed: %s [%d]\n",
					     module_name, options,
					     strerror(-err), -err);
			}
			free(tl);
		}
	}

	igt_fixture {
		close(kmsg);
		kmod_module_remove_module(kmod, KMOD_REMOVE_FORCE);

		if (strcmp(module_name, "i915") == 0)
			igt_i915_driver_load(NULL);

		igt_require(!igt_list_empty(&tests));
	}
}
