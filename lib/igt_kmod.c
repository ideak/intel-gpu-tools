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

#include <ctype.h>
#include <signal.h>
#include <errno.h>
#include <sys/utsname.h>

#include "igt_aux.h"
#include "igt_core.h"
#include "igt_kmod.h"
#include "igt_sysfs.h"
#include "igt_taints.h"

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
	const char **config_paths = NULL;
	char *config_paths_str;
	char *dirname;

	if (ctx)
		goto out;

	dirname = getenv("IGT_KMOD_DIRNAME");
	if (dirname)
		igt_debug("kmod dirname = %s\n", dirname);

	config_paths_str = getenv("IGT_KMOD_CONFIG_PATHS");
	if (config_paths_str)
		igt_debug("kmod config paths = %s\n", config_paths_str);

	if (config_paths_str) {
		unsigned count = !!strlen(config_paths_str);
		unsigned i;
		char* p;

		p = config_paths_str;
		while ((p = strchr(p, ':'))) p++, count++;


		config_paths = malloc(sizeof(*config_paths) * (count + 1));
		igt_assert(config_paths != NULL);

		p = config_paths_str;
		for (i = 0; i < count; ++i) {
			igt_assert(p != NULL);
			config_paths[i] = p;

			if ((p = strchr(p, ':')))
				*p++ = '\0';
		}
		config_paths[i] = NULL;
	}

	ctx = kmod_new(dirname, config_paths);
	igt_assert(ctx != NULL);

	free(config_paths);

	kmod_set_log_fn(ctx, squelch, NULL);
out:
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

		if (!strcmp(kmod_name, mod_name)) {
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

static bool
igt_kmod_is_loading(struct kmod_module *kmod)
{
	return kmod_module_get_initstate(kmod) == KMOD_MODULE_COMING;
}

static int modprobe(struct kmod_module *kmod, const char *options)
{
	unsigned int flags;

	flags = 0;
	if (options) /* force a fresh load to set the new options */
		flags |= KMOD_PROBE_FAIL_ON_LOADED;

	return kmod_module_probe_insert_module(kmod, flags, options,
					       NULL, NULL, NULL);
}

/**
 * igt_kmod_has_param:
 * @mod_name: The name of the module
 * @param: The name of the parameter
 *
 * Returns: true if the module has the parameter, false otherwise.
 */
bool igt_kmod_has_param(const char *module_name, const char *param)
{
	struct kmod_module *kmod;
	struct kmod_list *d, *pre;
	bool result = false;

	if (kmod_module_new_from_name(kmod_ctx(), module_name, &kmod))
		return false;

	pre = NULL;
	if (!kmod_module_get_info(kmod, &pre))
		goto out;

	kmod_list_foreach(d, pre) {
		const char *key, *val;

		key = kmod_module_info_get_key(d);
		if (strcmp(key, "parmtype"))
			continue;

		val = kmod_module_info_get_value(d);
		if (val && strncmp(val, param, strlen(param)) == 0) {
			result = true;
			break;
		}
	}
	kmod_module_info_free_list(pre);

out:
	kmod_module_unref(kmod);
	return result;
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
	return err < 0 ? err : 0;
}

static int igt_kmod_unload_r(struct kmod_module *kmod, unsigned int flags)
{
	struct kmod_list *holders, *pos;
	int err = 0;

	holders = kmod_module_get_holders(kmod);
	kmod_list_foreach(pos, holders) {
		struct kmod_module *it = kmod_module_get_module(pos);
		err = igt_kmod_unload_r(it, flags);
		kmod_module_unref(it);
		if (err < 0)
			break;
	}
	kmod_module_unref_list(holders);
	if (err < 0)
		return err;

	if (igt_kmod_is_loading(kmod)) {
		const char *mod_name = kmod_module_get_name(kmod);
		igt_debug("%s still initializing\n", mod_name);
		err = igt_wait(!igt_kmod_is_loading(kmod), 10000, 100);
		if (err < 0) {
			igt_debug("%s failed to complete init within the timeout\n",
				  mod_name);
			return err;
		}
	}

	return kmod_module_remove_module(kmod, flags);
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

	err = igt_kmod_unload_r(kmod, flags);
	if (err < 0) {
		igt_debug("Could not remove module %s (%s)\n", mod_name,
			  strerror(-err));
	}

out:
	kmod_module_unref(kmod);
	return err < 0 ? err : 0;
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

static void *strdup_realloc(char *origptr, const char *strdata)
{
	size_t nbytes = strlen(strdata) + 1;
	char *newptr = realloc(origptr, nbytes);

	memcpy(newptr, strdata, nbytes);
	return newptr;
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
	int ret;

	if (opts)
		igt_info("Reloading i915 with %s\n\n", opts);

	ret = igt_kmod_load("i915", opts);
	if (ret) {
		igt_warn("Could not load i915\n");
		return ret;
	}

	bind_fbcon(true);
	igt_kmod_load("snd_hda_intel", NULL);

	return 0;
}

static int igt_always_unload_audio_driver(char **who)
{
	int ret;
	const char *sound[] = {
		"snd_hda_intel",
		"snd_hdmi_lpe_audio",
		NULL,
	};

	/*
	 * With old Kernels, the dependencies between audio and DRM drivers
	 * are not shown. So, it may not be mandatory to remove the audio
	 * driver before unload/unbind the DRM one. So, let's print warnings,
	 * but return 0 on errors, as, if the dependency is mandatory, this
	 * will be detected later when trying to unbind/unload the DRM driver.
	 */
	for (const char **m = sound; *m; m++) {
		if (igt_kmod_is_loaded(*m)) {
			if (who)
				*who = strdup_realloc(*who, *m);

			ret = igt_lsof_kill_audio_processes();
			if (ret) {
				igt_warn("Could not stop %d audio process(es)\n", ret);
				igt_kmod_list_loaded();
				igt_lsof("/dev/snd");
				return 0;
			}

			ret = pipewire_pulse_start_reserve();
			if (ret)
				igt_warn("Failed to notify pipewire_pulse\n");
			kick_snd_hda_intel();
			ret = igt_kmod_unload(*m, 0);
			pipewire_pulse_stop_reserve();
			if (ret) {
				igt_warn("Could not unload audio driver %s\n", *m);
				igt_kmod_list_loaded();
				igt_lsof("/dev/snd");
				return 0;
			}
		}
	}
	return 0;
}

struct module_ref {
	char *name;
	unsigned long mem;
	unsigned int ref_count;
	unsigned int num_required;
	unsigned int *required_by;
};

static struct module_ref *read_module_dependencies(unsigned int *num_mod)
{
	FILE *fp = fopen("/proc/modules", "r");
	struct module_ref *mod = NULL;
	size_t line_buf_size = 0;
	char *required_by, *p;
	unsigned n_mod = 0;
	unsigned num_req;
	char *line = NULL;
	int len = 0;
	int i, ret;

	igt_assert(fp);

	while ((len = getline(&line, &line_buf_size, fp)) > 0) {
		mod = realloc(mod, (n_mod + 1) * sizeof(*mod));
		igt_assert(mod);
		mod[n_mod].required_by = NULL;

		p = strtok(line, " ");
		mod[n_mod].name = strdup(p);

		p = strtok(NULL, " ");
		ret = sscanf(p, "%lu", &mod[n_mod].mem);
		igt_assert(ret == 1);

		p = strtok(NULL, " ");
		ret = sscanf(p, "%u", &mod[n_mod].ref_count);
		igt_assert(ret == 1);

		num_req = 0;
		required_by = strtok(NULL, " ");
		if (strcmp(required_by, "-")) {
			p = strtok(required_by, ",");
			while (p) {
				for (i = 0; i < n_mod; i++)
					if (!strcmp(p, mod[i].name))
						break;

				igt_assert(i < n_mod);

				mod[n_mod].required_by = realloc(mod[n_mod].required_by,
								 (num_req + 1) * sizeof(unsigned int));
				mod[n_mod].required_by[num_req] = i;
				num_req++;
				p = strtok(NULL, ",");
			}
		}
		mod[n_mod].num_required = num_req;
		n_mod++;
	}
	free(line);
	fclose(fp);

	*num_mod = n_mod;
	return mod;
}

static void free_module_ref(struct module_ref *mod, unsigned int num_mod)
{
	int i;

	for (i = 0; i < num_mod; i++) {
		free(mod[i].name);
		free(mod[i].required_by);
	}
	free(mod);
}

static int igt_unload_driver(struct module_ref *mod, unsigned int num_mod,
			     unsigned int pos)
{
	int ret, i;

	/* Recursively remove depending modules, if any */
	for (i = 0; i < mod[pos].num_required; i++) {
		ret = igt_unload_driver(mod, num_mod,
					mod[num_mod].required_by[i]);
		if (ret)
			return ret;
	}

	return igt_kmod_unload(mod[pos].name, 0);
}

#define LINUX_VERSION(major, minor, patch)			\
		     ((major) << 16 | (minor) << 8 | (patch))

static int linux_kernel_version(void)
{
	struct utsname utsname;
	int ver[3] = { 0 };
	int i = 0;
	int n = 0;
	char *p;

	if (uname(&utsname))
		return 0;

	p = utsname.release;

	while (*p && i < 3) {
		if (isdigit(*p)) {
			n = n * 10 + (*p - '0');
			p++;
			continue;
		}
		if (n > 255)
			n = 255;
		ver[i] = n;
		i++;
		n = 0;

		if (*p != '.')
			break;
		p++;
	}

	return LINUX_VERSION(ver[0], ver[1], ver[2]);
}

int igt_audio_driver_unload(char **who)
{
	const char *drm_driver = "i915";
	unsigned int num_mod, i, j;
	struct module_ref *mod;
	int pos = -1;
	int ret = 0;

	/*
	 * On older Kernels, there's no way to check if the audio driver
	 * binds into the DRM one. So, always remove audio drivers that
	 * might be binding.
	 */
	if (linux_kernel_version() < LINUX_VERSION(5, 20, 0))
		return igt_always_unload_audio_driver(who);

	/*
	 * Newer Kernels gained support for showing the dependencies between
	 * audio and DRM drivers via /proc/modules and lsmod. Use it to
	 * detect if removing the audio driver is needed, properly unloading
	 * any audio processes that could be using the audio driver and
	 * handling module dependencies. Such solution should be generic
	 * enough to work with newer SOC/SOF drivers if ever needed.
	 */

	mod = read_module_dependencies(&num_mod);

	for (i = 0; i < num_mod; i++)
		if (!strcmp(mod[i].name, drm_driver))
			break;

	if (i == num_mod) {
		ret = 0;
		goto ret;
	}

	/* Recursively remove all drivers that depend on drm_driver */
	for (j = 0; j < mod[i].num_required; j++) {
		pos = mod[i].required_by[j];
		if (who)
			*who = strdup_realloc(*who, mod[pos].name);

		/*
		 * If a sound driver depends on drm_driver, kill audio processes
		 * first, in order to make it possible to unload the driver
		 */
		if (strstr(mod[pos].name, "snd")) {
			if (igt_lsof_kill_audio_processes()) {
				ret = EACCES;
				goto ret;
			}
		}

		ret = pipewire_pulse_start_reserve();
		if (ret)
			igt_warn("Failed to notify pipewire_pulse\n");
		ret = igt_unload_driver(mod, num_mod, pos);
		pipewire_pulse_stop_reserve();
		if (ret)
			break;
	}

ret:
	if (ret) {
		igt_warn("Couldn't unload %s, which is using the %s driver\n",
			 mod[pos].name, drm_driver);
		igt_kmod_list_loaded();
		igt_lsof("/dev/snd");
	}

	free_module_ref(mod, num_mod);

	return ret;
}

int __igt_i915_driver_unload(char **who)
{
	int ret;

	const char *aux[] = {
		/* gen5: ips uses symbol_get() so only a soft module dependency */
		"intel_ips",
		/* mei_gsc uses an i915 aux dev and the other mei mods depend on it */
		"mei_pxp",
		"mei_hdcp",
		"mei_gsc",
		NULL,
	};

	/* unbind vt */
	bind_fbcon(false);

	ret = igt_audio_driver_unload(who);
	if (ret)
		return ret;

	for (const char **m = aux; *m; m++) {
		if (!igt_kmod_is_loaded(*m))
			continue;

		ret = igt_kmod_unload(*m, 0);
		if (ret) {
			if (who)
				*who = strdup_realloc(*who, *m);

			return ret;
		}
	}

	if (igt_kmod_is_loaded("i915")) {
		ret = igt_kmod_unload("i915", 0);
		if (ret) {
			if (who)
				*who = strdup_realloc(*who, "i915");

			return ret;
		}
	}

	return 0;
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
	char *who = NULL;
	int ret;

	ret = __igt_i915_driver_unload(&who);
	if (ret) {
		igt_warn("Could not unload %s\n", who);
		igt_kmod_list_loaded();
		igt_lsof("/dev/dri");
		igt_lsof("/dev/snd");
		free(who);
		return ret;
	}
	free(who);

	if (igt_kmod_is_loaded("intel-gtt"))
		igt_kmod_unload("intel-gtt", 0);

	igt_kmod_unload("drm_kms_helper", 0);
	igt_kmod_unload("drm", 0);

	if (igt_kmod_is_loaded("i915")) {
		igt_warn("i915.ko still loaded!\n");
		return -EBUSY;
	}

	return 0;
}

/**
 * igt_amdgpu_driver_load:
 * @opts: options to pass to amdgpu driver
 *
 * Returns: IGT_EXIT_SUCCESS or IGT_EXIT_FAILURE.
 *
 * Loads the amdgpu driver and its dependencies.
 *
 */
int
igt_amdgpu_driver_load(const char *opts)
{
	if (opts)
		igt_info("Reloading amdgpu with %s\n\n", opts);

	if (igt_kmod_load("amdgpu", opts)) {
		igt_warn("Could not load amdgpu\n");
		return IGT_EXIT_FAILURE;
	}

	bind_fbcon(true);

	return IGT_EXIT_SUCCESS;
}

/**
 * igt_amdgpu_driver_unload:
 *
 * Returns: IGT_EXIT_SUCCESS on success, IGT_EXIT_FAILURE on failure
 * and IGT_EXIT_SKIP if amdgpu could not be unloaded.
 *
 * Unloads the amdgpu driver and its dependencies.
 *
 */
int
igt_amdgpu_driver_unload(void)
{
	bind_fbcon(false);

	if (igt_kmod_is_loaded("amdgpu")) {
		if (igt_kmod_unload("amdgpu", 0)) {
			igt_warn("Could not unload amdgpu\n");
			igt_kmod_list_loaded();
			igt_lsof("/dev/dri");
			return IGT_EXIT_SKIP;
		}
	}

	igt_kmod_unload("drm_kms_helper", 0);
	igt_kmod_unload("drm", 0);

	if (igt_kmod_is_loaded("amdgpu")) {
		igt_warn("amdgpu.ko still loaded!\n");
		return IGT_EXIT_FAILURE;
	}

	return IGT_EXIT_SUCCESS;
}

static void kmsg_dump(int fd)
{
	char record[4096 + 1];

	if (fd == -1) {
		igt_warn("Unable to retrieve kernel log (from /dev/kmsg)\n");
		return;
	}

	record[sizeof(record) - 1] = '\0';

	for (;;) {
		const char *start, *end;
		ssize_t r;

		r = read(fd, record, sizeof(record) - 1);
		if (r < 0) {
			if (errno == EINTR)
				continue;

			if (errno == EPIPE) {
				igt_warn("kmsg truncated: too many messages. You may want to increase log_buf_len in kmcdline\n");
				continue;
			}

			if (errno != EAGAIN)
				igt_warn("kmsg truncated: unknown error (%m)\n");

			break;
		}

		start = strchr(record, ';');
		if (start) {
			start++;
			end = strchrnul(start, '\n');
			igt_warn("%.*s\n", (int)(end - start), start);
		}
	}
}

static void tests_add(struct igt_kselftest_list *tl, struct igt_list_head *list)
{
	struct igt_kselftest_list *pos;

	igt_list_for_each_entry(pos, list, link)
		if (pos->number > tl->number)
			break;

	igt_list_add_tail(&tl->link, &pos->link);
}

void igt_kselftest_get_tests(struct kmod_module *kmod,
			     const char *filter,
			     struct igt_list_head *tests)
{
	const char *param_prefix = "igt__";
	const int prefix_len = strlen(param_prefix);
	struct kmod_list *d, *pre;
	struct igt_kselftest_list *tl;

	pre = NULL;
	if (!kmod_module_get_info(kmod, &pre))
		return;

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

		if (filter && strncmp(tl->name, filter, strlen(filter))) {
			free(tl);
			continue;
		}

		tests_add(tl, tests);
	}
	kmod_module_info_free_list(pre);
}

static int open_parameters(const char *module_name)
{
	char path[256];

	snprintf(path, sizeof(path), "/sys/module/%s/parameters", module_name);
	return open(path, O_RDONLY);
}

int igt_kselftest_init(struct igt_kselftest *tst,
		       const char *module_name)
{
	int err;

	memset(tst, 0, sizeof(*tst));

	tst->module_name = strdup(module_name);
	if (!tst->module_name)
		return 1;

	tst->kmsg = -1;

	err = kmod_module_new_from_name(kmod_ctx(), module_name, &tst->kmod);
	if (err)
		return err;

	return 0;
}

int igt_kselftest_begin(struct igt_kselftest *tst)
{
	int err;

	if (strcmp(tst->module_name, "i915") == 0)
		igt_i915_driver_unload();

	err = kmod_module_remove_module(tst->kmod, KMOD_REMOVE_FORCE);
	igt_require(err == 0 || err == -ENOENT);

	tst->kmsg = open("/dev/kmsg", O_RDONLY | O_NONBLOCK);

	return 0;
}

int igt_kselftest_execute(struct igt_kselftest *tst,
			  struct igt_kselftest_list *tl,
			  const char *options,
			  const char *result)
{
	unsigned long taints;
	char buf[1024];
	int err;

	igt_skip_on(igt_kernel_tainted(&taints));

	lseek(tst->kmsg, 0, SEEK_END);

	snprintf(buf, sizeof(buf), "%s=1 %s", tl->param, options ?: "");

	err = modprobe(tst->kmod, buf);
	if (err == 0 && result) {
		int dir = open_parameters(tst->module_name);
		igt_sysfs_scanf(dir, result, "%d", &err);
		close(dir);
	}
	if (err == -ENOTTY) /* special case */
		err = 0;
	if (err)
		kmsg_dump(tst->kmsg);

	kmod_module_remove_module(tst->kmod, 0);

	errno = 0;
	igt_assert_f(err == 0,
		     "kselftest \"%s %s\" failed: %s [%d]\n",
		     tst->module_name, buf, strerror(-err), -err);

	igt_assert_eq(igt_kernel_tainted(&taints), 0);

	return err;
}

void igt_kselftest_end(struct igt_kselftest *tst)
{
	kmod_module_remove_module(tst->kmod, KMOD_REMOVE_FORCE);
	close(tst->kmsg);
}

void igt_kselftest_fini(struct igt_kselftest *tst)
{
	free(tst->module_name);
	kmod_module_unref(tst->kmod);
}

static const char *unfilter(const char *filter, const char *name)
{
	if (!filter)
		return name;

	name += strlen(filter);
	if (!isalpha(*name))
		name++;

	return name;
}

void igt_kselftests(const char *module_name,
		    const char *options,
		    const char *result,
		    const char *filter)
{
	struct igt_kselftest tst;
	IGT_LIST_HEAD(tests);
	struct igt_kselftest_list *tl, *tn;

	if (igt_kselftest_init(&tst, module_name) != 0)
		return;

	igt_fixture
		igt_require(igt_kselftest_begin(&tst) == 0);

	igt_kselftest_get_tests(tst.kmod, filter, &tests);
	igt_subtest_with_dynamic(filter ?: "all") {
		igt_list_for_each_entry_safe(tl, tn, &tests, link) {
			unsigned long taints;

			igt_dynamic_f("%s", unfilter(filter, tl->name))
				igt_kselftest_execute(&tst, tl, options, result);
			free(tl);

			if (igt_kernel_tainted(&taints)) {
				igt_info("Kernel tainted, not executing more selftests.\n");
				break;
			}
		}
	}

	igt_fixture {
		igt_kselftest_end(&tst);
		igt_require(!igt_list_empty(&tests));
	}

	igt_kselftest_fini(&tst);
}
