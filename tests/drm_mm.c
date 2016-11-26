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
#include <libkmod.h>

IGT_TEST_DESCRIPTION("Basic sanity check of DRM's range manager (struct drm_mm)");

static void squelch(void *data, int priority,
		    const char *file, int line, const char *fn,
		    const char *format, va_list args)
{
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

static void igt_kselftests(const char *module_name)
{
	struct kmod_ctx *ctx;
	struct kmod_module *kmod;
	struct kmod_list *d, *pre;
	int err, kmsg = -1;

	ctx = kmod_new(NULL, NULL);
	igt_assert(ctx != NULL);

	kmod_set_log_fn(ctx, squelch, NULL);

	igt_require(kmod_module_new_from_name(ctx, module_name, &kmod) == 0);
	igt_fixture {
		err = kmod_module_remove_module(kmod, KMOD_REMOVE_FORCE);
		igt_require(err == 0 || err == -ENOENT);

		kmsg = open("/dev/kmsg", O_RDONLY | O_NONBLOCK);
	}

	pre = NULL;
	if (kmod_module_get_info(kmod, &pre)) {
		kmod_list_foreach(d, pre) {
			const char *key, *val;
			char *option, *colon;

			key = kmod_module_info_get_key(d);
			if (strcmp(key, "parmtype"))
				continue;

			val = kmod_module_info_get_value(d);
			if (!val || strncmp(val, "subtest__", 9))
				continue;

			option = strdup(val);
			colon = strchr(option, ':');
			*colon = '\0';

			igt_subtest_f("%s", option + 9) {
				lseek(kmsg, 0, SEEK_END);
				strcpy(colon, "=1");

				err = 0;
				if (kmod_module_insert_module(kmod, 0, option))
					err = -errno;
				kmod_module_remove_module(kmod, 0);
				if (err)
					kmsg_dump(kmsg);

				errno = 0;
				igt_assert_f(err == 0,
					     "kselftest \"%s %s\" failed: %s [%d]\n",
					     module_name, option,
					     strerror(-err), -err);
			}
		}
		kmod_module_info_free_list(pre);
	}

	igt_fixture {
		close(kmsg);
		kmod_module_remove_module(kmod, KMOD_REMOVE_FORCE);
	}
}

igt_main
{
	igt_kselftests("test-drm_mm");
}
