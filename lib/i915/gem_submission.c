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

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include <sys/ioctl.h>
#include <sys/time.h>

#include <i915_drm.h>

#include "i915/gem.h"
#include "i915/gem_create.h"
#include "i915/gem_engine_topology.h"
#include "i915/gem_submission.h"

#include "igt_core.h"
#include "igt_gt.h"
#include "igt_params.h"
#include "igt_sysfs.h"
#include "intel_chipset.h"
#include "intel_reg.h"
#include "ioctl_wrappers.h"

/**
 * SECTION:gem_submission
 * @short_description: Helpers for determining submission method
 * @title: GEM Submission
 *
 * This helper library contains functions used for getting information on
 * currently used hardware submission method. Different generations of hardware
 * support different submission backends, currently we're distinguishing 3
 * different methods: legacy ringbuffer submission, execlists, GuC submission.
 * For legacy ringbuffer submission, there's also a variation where we're using
 * semaphores for synchronization between engines.
 */

static bool has_semaphores(int fd, int dir)
{
	int val = 0;
	struct drm_i915_getparam gp = {
		gp.param = I915_PARAM_HAS_SEMAPHORES,
		gp.value = &val,
	};
	if (ioctl(fd, DRM_IOCTL_I915_GETPARAM, &gp) < 0)
		val = igt_sysfs_get_boolean(dir, "semaphores");
	return val;
}

/**
 * gem_submission_method:
 * @fd: open i915 drm file descriptor
 *
 * Returns: Submission method bitmap.
 */
unsigned gem_submission_method(int fd)
{
	const int gen = intel_gen(intel_get_drm_devid(fd));
	unsigned flags = 0;

	int dir;

	dir = igt_params_open(fd);
	if (dir < 0)
		return 0;

	if (igt_sysfs_get_u32(dir, "enable_guc") & 1) {
		flags |= GEM_SUBMISSION_GUC | GEM_SUBMISSION_EXECLISTS;
		goto out;
	}

	if (gen >= 8) {
		flags |= GEM_SUBMISSION_EXECLISTS;
		goto out;
	}

	if (has_semaphores(fd, dir))
		flags |= GEM_SUBMISSION_SEMAPHORES;

out:
	close(dir);
	return flags;
}

/**
 * gem_submission_print_method:
 * @fd: open i915 drm file descriptor
 *
 * Helper for pretty-printing currently used submission method
 */
void gem_submission_print_method(int fd)
{
	const unsigned flags = gem_submission_method(fd);
	const struct intel_device_info *info;

	info = intel_get_device_info(intel_get_drm_devid(fd));
	if (info)
		igt_info("Running on %s\n", info->codename);

	if (flags & GEM_SUBMISSION_GUC) {
		igt_info("Using GuC submission\n");
		return;
	}

	if (flags & GEM_SUBMISSION_EXECLISTS) {
		igt_info("Using Execlists submission\n");
		return;
	}

	igt_info("Using Legacy submission%s\n",
		 flags & GEM_SUBMISSION_SEMAPHORES ? ", with semaphores" : "");
}

/**
 * gem_has_semaphores:
 * @fd: open i915 drm file descriptor
 *
 * Feature test macro to query whether the driver is using semaphores for
 * synchronization between engines.
 */
bool gem_has_semaphores(int fd)
{
	return gem_submission_method(fd) & GEM_SUBMISSION_SEMAPHORES;
}

/**
 * gem_has_execlists:
 * @fd: open i915 drm file descriptor
 *
 * Feature test macro to query whether the driver is using execlists as a
 * hardware submission method.
 */
bool gem_has_execlists(int fd)
{
	return gem_submission_method(fd) & GEM_SUBMISSION_EXECLISTS;
}

/**
 * gem_has_guc_submission:
 * @fd: open i915 drm file descriptor
 *
 * Feature test macro to query whether the driver is using the GuC as a
 * hardware submission method.
 */
bool gem_has_guc_submission(int fd)
{
	return gem_submission_method(fd) & GEM_SUBMISSION_GUC;
}

static bool is_wedged(int i915)
{
	int err = 0;
	if (ioctl(i915, DRM_IOCTL_I915_GEM_THROTTLE))
		err = -errno;
	return err == -EIO;
}

/**
 * gem_test_engine:
 * @i915: open i915 drm file descriptor
 * @engine: the engine (I915_EXEC_RING id) to exercise
 *
 * Execute a nop batch on the engine specified, or ALL_ENGINES for all,
 * and check it executes.
 */
void gem_test_engine(int i915, unsigned int engine)
{
	const uint32_t bbe = MI_BATCH_BUFFER_END;
	struct drm_i915_gem_exec_object2 obj = { };
	struct drm_i915_gem_execbuffer2 execbuf = {
		.buffers_ptr = to_user_pointer(&obj),
		.buffer_count = 1,
	};

	i915 = gem_reopen_driver(i915);
	igt_assert(!is_wedged(i915));

	obj.handle = gem_create(i915, 4096);
	gem_write(i915, obj.handle, 0, &bbe, sizeof(bbe));

	if (engine == ALL_ENGINES) {
		const struct intel_execution_engine2 *e2;

		__for_each_physical_engine(i915, e2) {
			execbuf.flags = e2->flags;
			gem_execbuf(i915, &execbuf);
		}
	} else {
		execbuf.flags = engine;
		gem_execbuf(i915, &execbuf);
	}
	gem_sync(i915, obj.handle);
	gem_close(i915, obj.handle);

	igt_assert(!is_wedged(i915));
	close(i915);
}

int gem_cmdparser_version(int i915, uint32_t engine)
{
	int version = 0;
	drm_i915_getparam_t gp = {
		.param = I915_PARAM_CMD_PARSER_VERSION,
		.value = &version,
	};

	ioctl(i915, DRM_IOCTL_I915_GETPARAM, &gp);
	return version;
}

bool gem_has_blitter(int i915)
{
	unsigned int blt;

	blt = 0;
	if (intel_gen(intel_get_drm_devid(i915)) >= 6)
		blt = I915_EXEC_BLT;

	return gem_has_ring(i915, blt);
}

void gem_require_blitter(int i915)
{
	igt_require(gem_has_blitter(i915));
}

static bool gem_engine_has_immutable_submission(int i915, int class)
{
	const int gen = intel_gen(intel_get_drm_devid(i915));
        int parser_version;

	parser_version = gem_cmdparser_version(i915, 0);
	if (parser_version < 0)
		return false;

	if (gen == 9 && class == I915_ENGINE_CLASS_COPY && parser_version > 9)
		return true;

	return false;
}

/**
 * gem_class_has_mutable_submission:
 * @i915: open i915 drm file descriptor
 * @class: engine class
 *
 * Returns boolean value if the engine class allows batch modifications
 * post execbuf.
 */
bool gem_class_has_mutable_submission(int i915, int class)
{
	return !gem_engine_has_immutable_submission(i915, class);
}

/**
 * gem_engine_has_mutable_submission:
 * @i915: open i915 drm file descriptor
 * @engine: the engine (I915_EXEC_RING id) of target
 *
 * Returns boolean value if the engine allows batch modifications
 * post execbuf.
 */
bool gem_engine_has_mutable_submission(int i915, unsigned int engine)
{
	return gem_class_has_mutable_submission(i915,
						gem_execbuf_flags_to_engine_class(engine));
}

static int __execbuf(int i915, struct drm_i915_gem_execbuffer2 *execbuf)
{
	int err;

	err = 0;
	if (ioctl(i915, DRM_IOCTL_I915_GEM_EXECBUFFER2, execbuf)) {
		err = -errno;
		igt_assume(err);
	}

	errno = 0;
	return err;
}

static void alarm_handler(int sig)
{
}

static unsigned int
__measure_ringsize(int i915, uint32_t ctx_id, unsigned int engine)
{
	struct sigaction old_sa, sa = { .sa_handler = alarm_handler };
	struct drm_i915_gem_exec_object2 obj[2];
	struct drm_i915_gem_execbuffer2 execbuf;
	const uint32_t bbe = MI_BATCH_BUFFER_END;
	unsigned int last[2]= { -1, -1 }, count;
	struct itimerval itv;
	IGT_CORK_HANDLE(cork);

	memset(obj, 0, sizeof(obj));
	obj[1].handle = gem_create(i915, 4096);
	gem_write(i915, obj[1].handle, 0, &bbe, sizeof(bbe));

	memset(&execbuf, 0, sizeof(execbuf));
	execbuf.buffers_ptr = to_user_pointer(&obj[1]);
	execbuf.buffer_count = 1;
	execbuf.rsvd1 = ctx_id;
	execbuf.flags = engine;
	gem_execbuf(i915, &execbuf);

	obj[0].handle = igt_cork_plug(&cork, i915);

	execbuf.buffers_ptr = to_user_pointer(obj);
	execbuf.buffer_count = 2;

	sigaction(SIGALRM, &sa, &old_sa);
	itv.it_interval.tv_sec = 0;
	itv.it_interval.tv_usec = 1000;
	itv.it_value.tv_sec = 0;
	itv.it_value.tv_usec = 10000;
	setitimer(ITIMER_REAL, &itv, NULL);

	count = 0;
	do {
		int err = __execbuf(i915, &execbuf);

		if (err == 0) {
			count++;
			continue;
		}

		if (err == -EWOULDBLOCK)
			break;

		if (last[1] == count)
			break;

		/* sleep until the next timer interrupt (woken on signal) */
		pause();
		last[1] = last[0];
		last[0] = count;
	} while (1);
	igt_assert(count > 2);

	memset(&itv, 0, sizeof(itv));
	setitimer(ITIMER_REAL, &itv, NULL);
	sigaction(SIGALRM, &old_sa, NULL);

	igt_cork_unplug(&cork);
	gem_close(i915, obj[0].handle);
	gem_close(i915, obj[1].handle);

	/* Be conservative, expect relocations, in case we must wrap later */
	return count / 2 - 2;
}

unsigned int gem_submission_measure(int i915, const intel_ctx_cfg_t *cfg,
				    unsigned int engine)
{
	const intel_ctx_t *ctx = NULL;
	unsigned int size;
	bool nonblock;

	nonblock = fcntl(i915, F_GETFL) & O_NONBLOCK;
	if (!nonblock)
		fcntl(i915, F_SETFL, fcntl(i915, F_GETFL) | O_NONBLOCK);

	if (cfg) {
		if (gem_has_contexts(i915))
			ctx = intel_ctx_create(i915, cfg);
		else
			ctx = intel_ctx_0(i915);
	}

	if (engine == ALL_ENGINES) {
		struct intel_execution_engine2 *e;

		size = -1;
		if (ctx) {
			for_each_ctx_engine(i915, ctx, e) {
				unsigned int this =  __measure_ringsize(i915, ctx->id, e->flags);
				if (this < size)
					size = this;
			}
		} else {
			__for_each_physical_engine(i915, e) {
				unsigned int this =  __measure_ringsize(i915, 0, e->flags);
				if (this < size)
					size = this;
			}
		}
	} else {
		if (ctx)
			size =  __measure_ringsize(i915, ctx->id, engine);
		else
			size =  __measure_ringsize(i915, 0, engine);
	}

	if (ctx)
		intel_ctx_destroy(i915, ctx);


	if (!nonblock)
		fcntl(i915, F_SETFL, fcntl(i915, F_GETFL) & ~O_NONBLOCK);

	return size;
}

/**
 * gem_has_relocations:
 * @fd: opened i915 drm file descriptor
 *
 * Feature test macro to query whether kernel allows for generation to
 * use relocations.
 *
 * Returns: true if we can use relocations, otherwise false
 */

bool gem_has_relocations(int i915)
{
	struct drm_i915_gem_relocation_entry reloc = {};
	struct drm_i915_gem_exec_object2 obj = {
		.handle = gem_create(i915, 4096),
		.relocs_ptr = to_user_pointer(&reloc),
		.relocation_count = 1,
	};
	struct drm_i915_gem_execbuffer2 execbuf = {
		.buffers_ptr = to_user_pointer(&obj),
		.buffer_count = 1,
	};
	bool has_relocs;

	has_relocs = __gem_execbuf(i915, &execbuf) == -ENOENT;
	gem_close(i915, obj.handle);

	return has_relocs;
}
