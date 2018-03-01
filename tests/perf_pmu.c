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
 *
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <inttypes.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/times.h>
#include <sys/types.h>
#include <dirent.h>
#include <time.h>
#include <poll.h>
#include <sched.h>

#include "igt.h"
#include "igt_core.h"
#include "igt_perf.h"
#include "igt_sysfs.h"
#include "igt_pm.h"
#include "sw_sync.h"

IGT_TEST_DESCRIPTION("Test the i915 pmu perf interface");

const double tolerance = 0.05f;
const unsigned long batch_duration_ns = 500e6;

static int open_pmu(uint64_t config)
{
	int fd;

	fd = perf_i915_open(config);
	igt_skip_on(fd < 0 && errno == ENODEV);
	igt_assert(fd >= 0);

	return fd;
}

static int open_group(uint64_t config, int group)
{
	int fd;

	fd = perf_i915_open_group(config, group);
	igt_skip_on(fd < 0 && errno == ENODEV);
	igt_assert(fd >= 0);

	return fd;
}

static void
init(int gem_fd, const struct intel_execution_engine2 *e, uint8_t sample)
{
	int fd, err = 0;
	bool exists;

	errno = 0;
	fd = perf_i915_open(__I915_PMU_ENGINE(e->class, e->instance, sample));
	if (fd < 0)
		err = errno;

	exists = gem_has_engine(gem_fd, e->class, e->instance);
	if (intel_gen(intel_get_drm_devid(gem_fd)) < 6 &&
	    sample == I915_SAMPLE_SEMA)
		exists = false;

	if (exists) {
		igt_assert_eq(err, 0);
		igt_assert_fd(fd);
		close(fd);
	} else {
		igt_assert_lt(fd, 0);
		igt_assert_eq(err, ENODEV);
	}
}

static uint64_t __pmu_read_single(int fd, uint64_t *ts)
{
	uint64_t data[2];

	igt_assert_eq(read(fd, data, sizeof(data)), sizeof(data));

	if (ts)
		*ts = data[1];

	return data[0];
}

static uint64_t pmu_read_single(int fd)
{
	return __pmu_read_single(fd, NULL);
}

static uint64_t pmu_read_multi(int fd, unsigned int num, uint64_t *val)
{
	uint64_t buf[2 + num];
	unsigned int i;

	igt_assert_eq(read(fd, buf, sizeof(buf)), sizeof(buf));

	for (i = 0; i < num; i++)
		val[i] = buf[2 + i];

	return buf[1];
}

#define __assert_within_epsilon(x, ref, tol_up, tol_down) \
	igt_assert_f((double)(x) <= (1.0 + (tol_up)) * (double)(ref) && \
		     (double)(x) >= (1.0 - (tol_down)) * (double)(ref), \
		     "'%s' != '%s' (%f not within +%f%%/-%f%% tolerance of %f)\n",\
		     #x, #ref, (double)(x), \
		     (tol_up) * 100.0, (tol_down) * 100.0, \
		     (double)(ref))

#define assert_within_epsilon(x, ref, tolerance) \
	__assert_within_epsilon(x, ref, tolerance, tolerance)

/*
 * Helper for cases where we assert on time spent sleeping (directly or
 * indirectly), so make it more robust by ensuring the system sleep time
 * is within test tolerance to start with.
 */
static unsigned int measured_usleep(unsigned int usec)
{
	struct timespec ts = { };
	unsigned int slept;

	slept = igt_nsec_elapsed(&ts);
	igt_assert(slept == 0);
	do {
		usleep(usec - slept);
		slept = igt_nsec_elapsed(&ts) / 1000;
	} while (slept < usec);

	return igt_nsec_elapsed(&ts);
}

static unsigned int e2ring(int gem_fd, const struct intel_execution_engine2 *e)
{
	return gem_class_instance_to_eb_flags(gem_fd, e->class, e->instance);
}

#define TEST_BUSY (1)
#define FLAG_SYNC (2)
#define TEST_TRAILING_IDLE (4)
#define TEST_RUNTIME_PM (8)
#define FLAG_LONG (16)
#define FLAG_HANG (32)

static void end_spin(int fd, igt_spin_t *spin, unsigned int flags)
{
	if (!spin)
		return;

	igt_spin_batch_end(spin);

	if (flags & FLAG_SYNC)
		gem_sync(fd, spin->handle);

	if (flags & TEST_TRAILING_IDLE)
		usleep(batch_duration_ns / 5000);
}

static void
single(int gem_fd, const struct intel_execution_engine2 *e, unsigned int flags)
{
	unsigned long slept;
	igt_spin_t *spin;
	uint64_t val;
	int fd;

	fd = open_pmu(I915_PMU_ENGINE_BUSY(e->class, e->instance));

	if (flags & TEST_BUSY)
		spin = igt_spin_batch_new(gem_fd, 0, e2ring(gem_fd, e), 0);
	else
		spin = NULL;

	val = pmu_read_single(fd);
	slept = measured_usleep(batch_duration_ns / 1000);
	if (flags & TEST_TRAILING_IDLE)
		end_spin(gem_fd, spin, flags);
	val = pmu_read_single(fd) - val;

	if (flags & FLAG_HANG)
		igt_force_gpu_reset(gem_fd);
	else
		end_spin(gem_fd, spin, FLAG_SYNC);

	assert_within_epsilon(val, flags & TEST_BUSY ? slept : 0.f, tolerance);

	/* Check for idle after hang. */
	if (flags & FLAG_HANG) {
		/* Sleep for a bit for reset unwind to settle. */
		usleep(500e3);
		/*
		 * Ensure batch was executing before reset, meaning it must be
		 * idle by now. Unless it did not even manage to start before we
		 * triggered the reset, in which case the idleness check below
		 * might fail. The latter is very unlikely since there are two
		 * sleeps during which it had an opportunity to start.
		 */
		igt_assert(!gem_bo_busy(gem_fd, spin->handle));
		val = pmu_read_single(fd);
		slept = measured_usleep(batch_duration_ns / 1000);
		val = pmu_read_single(fd) - val;

		assert_within_epsilon(val, 0, tolerance);
	}

	igt_spin_batch_free(gem_fd, spin);
	close(fd);

	gem_quiescent_gpu(gem_fd);
}

static void
busy_start(int gem_fd, const struct intel_execution_engine2 *e)
{
	unsigned long slept;
	uint64_t val, ts[2];
	igt_spin_t *spin;
	int fd;

	/*
	 * Defeat the busy stats delayed disable, we need to guarantee we are
	 * the first user.
	 */
	sleep(2);

	spin = __igt_spin_batch_new(gem_fd, 0, e2ring(gem_fd, e), 0);

	/*
	 * Sleep for a bit after making the engine busy to make sure the PMU
	 * gets enabled when the batch is already running.
	 */
	usleep(500e3);

	fd = open_pmu(I915_PMU_ENGINE_BUSY(e->class, e->instance));

	val = __pmu_read_single(fd, &ts[0]);
	slept = measured_usleep(batch_duration_ns / 1000);
	val = __pmu_read_single(fd, &ts[1]) - val;
	igt_debug("slept=%lu perf=%"PRIu64"\n", slept, ts[1] - ts[0]);

	igt_spin_batch_free(gem_fd, spin);
	close(fd);

	assert_within_epsilon(val, ts[1] - ts[0], tolerance);
	gem_quiescent_gpu(gem_fd);
}

/*
 * This test has a potentially low rate of catching the issue it is trying to
 * catch. Or in other words, quite high rate of false negative successes. We
 * will depend on the CI systems running it a lot to detect issues.
 */
static void
busy_double_start(int gem_fd, const struct intel_execution_engine2 *e)
{
	unsigned long slept;
	uint64_t val, val2, ts[2];
	igt_spin_t *spin[2];
	uint32_t ctx;
	int fd;

	ctx = gem_context_create(gem_fd);

	/*
	 * Defeat the busy stats delayed disable, we need to guarantee we are
	 * the first user.
	 */
	sleep(2);

	/*
	 * Submit two contexts, with a pause in between targeting the ELSP
	 * re-submission in execlists mode. Make sure busyness is correctly
	 * reported with the engine busy, and after the engine went idle.
	 */
	spin[0] = __igt_spin_batch_new(gem_fd, 0, e2ring(gem_fd, e), 0);
	usleep(500e3);
	spin[1] = __igt_spin_batch_new(gem_fd, ctx, e2ring(gem_fd, e), 0);

	/*
	 * Open PMU as fast as possible after the second spin batch in attempt
	 * to be faster than the driver handling lite-restore.
	 */
	fd = open_pmu(I915_PMU_ENGINE_BUSY(e->class, e->instance));

	val = __pmu_read_single(fd, &ts[0]);
	slept = measured_usleep(batch_duration_ns / 1000);
	val = __pmu_read_single(fd, &ts[1]) - val;
	igt_debug("slept=%lu perf=%"PRIu64"\n", slept, ts[1] - ts[0]);

	igt_spin_batch_end(spin[0]);
	igt_spin_batch_end(spin[1]);

	/* Wait for GPU idle to verify PMU reports idle. */
	gem_quiescent_gpu(gem_fd);

	val2 = pmu_read_single(fd);
	usleep(batch_duration_ns / 1000);
	val2 = pmu_read_single(fd) - val2;

	igt_info("busy=%"PRIu64" idle=%"PRIu64"\n", val, val2);

	igt_spin_batch_free(gem_fd, spin[0]);
	igt_spin_batch_free(gem_fd, spin[1]);

	close(fd);

	gem_context_destroy(gem_fd, ctx);

	assert_within_epsilon(val, ts[1] - ts[0], tolerance);
	igt_assert_eq(val2, 0);

	gem_quiescent_gpu(gem_fd);
}

static void log_busy(unsigned int num_engines, uint64_t *val)
{
	char buf[1024];
	int rem = sizeof(buf);
	unsigned int i;
	char *p = buf;

	for (i = 0; i < num_engines; i++) {
		int len;

		len = snprintf(p, rem, "%u=%" PRIu64 "\n",  i, val[i]);
		igt_assert(len > 0);
		rem -= len;
		p += len;
	}

	igt_info("%s", buf);
}

static void
busy_check_all(int gem_fd, const struct intel_execution_engine2 *e,
	       const unsigned int num_engines, unsigned int flags)
{
	const struct intel_execution_engine2 *e_;
	uint64_t tval[2][num_engines];
	unsigned int busy_idx = 0, i;
	uint64_t val[num_engines];
	int fd[num_engines];
	unsigned long slept;
	igt_spin_t *spin;

	i = 0;
	fd[0] = -1;
	for_each_engine_class_instance(fd, e_) {
		if (!gem_has_engine(gem_fd, e_->class, e_->instance))
			continue;
		else if (e == e_)
			busy_idx = i;

		fd[i++] = open_group(I915_PMU_ENGINE_BUSY(e_->class,
							  e_->instance),
				     fd[0]);
	}

	igt_assert_eq(i, num_engines);

	spin = igt_spin_batch_new(gem_fd, 0, e2ring(gem_fd, e), 0);
	pmu_read_multi(fd[0], num_engines, tval[0]);
	slept = measured_usleep(batch_duration_ns / 1000);
	if (flags & TEST_TRAILING_IDLE)
		end_spin(gem_fd, spin, flags);
	pmu_read_multi(fd[0], num_engines, tval[1]);

	end_spin(gem_fd, spin, FLAG_SYNC);
	igt_spin_batch_free(gem_fd, spin);
	close(fd[0]);

	for (i = 0; i < num_engines; i++)
		val[i] = tval[1][i] - tval[0][i];

	log_busy(num_engines, val);

	assert_within_epsilon(val[busy_idx], slept, tolerance);
	for (i = 0; i < num_engines; i++) {
		if (i == busy_idx)
			continue;
		assert_within_epsilon(val[i], 0.0f, tolerance);
	}
	gem_quiescent_gpu(gem_fd);
}

static void
__submit_spin_batch(int gem_fd,
		    struct drm_i915_gem_exec_object2 *obj,
		    const struct intel_execution_engine2 *e)
{
	struct drm_i915_gem_execbuffer2 eb = {
		.buffer_count = 1,
		.buffers_ptr = to_user_pointer(obj),
		.flags = e2ring(gem_fd, e),
	};

	gem_execbuf(gem_fd, &eb);
}

static void
most_busy_check_all(int gem_fd, const struct intel_execution_engine2 *e,
		    const unsigned int num_engines, unsigned int flags)
{
	struct drm_i915_gem_exec_object2 obj = {};
	const struct intel_execution_engine2 *e_;
	uint64_t tval[2][num_engines];
	uint64_t val[num_engines];
	int fd[num_engines];
	unsigned long slept;
	igt_spin_t *spin = NULL;
	unsigned int idle_idx, i;

	i = 0;
	for_each_engine_class_instance(fd, e_) {
		if (!gem_has_engine(gem_fd, e_->class, e_->instance))
			continue;

		if (e == e_) {
			idle_idx = i;
		} else if (spin) {
			__submit_spin_batch(gem_fd, &obj, e_);
		} else {
			spin = igt_spin_batch_new(gem_fd, 0,
						  e2ring(gem_fd, e_), 0);
			obj.handle = spin->handle;
		}

		val[i++] = I915_PMU_ENGINE_BUSY(e_->class, e_->instance);
	}
	igt_assert(i == num_engines);

	fd[0] = -1;
	for (i = 0; i < num_engines; i++)
		fd[i] = open_group(val[i], fd[0]);

	pmu_read_multi(fd[0], num_engines, tval[0]);
	slept = measured_usleep(batch_duration_ns / 1000);
	if (flags & TEST_TRAILING_IDLE)
		end_spin(gem_fd, spin, flags);
	pmu_read_multi(fd[0], num_engines, tval[1]);

	end_spin(gem_fd, spin, FLAG_SYNC);
	igt_spin_batch_free(gem_fd, spin);
	close(fd[0]);

	for (i = 0; i < num_engines; i++)
		val[i] = tval[1][i] - tval[0][i];

	log_busy(num_engines, val);

	for (i = 0; i < num_engines; i++) {
		if (i == idle_idx)
			assert_within_epsilon(val[i], 0.0f, tolerance);
		else
			assert_within_epsilon(val[i], slept, tolerance);
	}
	gem_quiescent_gpu(gem_fd);
}

static void
all_busy_check_all(int gem_fd, const unsigned int num_engines,
		   unsigned int flags)
{
	struct drm_i915_gem_exec_object2 obj = {};
	const struct intel_execution_engine2 *e;
	uint64_t tval[2][num_engines];
	uint64_t val[num_engines];
	int fd[num_engines];
	unsigned long slept;
	igt_spin_t *spin = NULL;
	unsigned int i;

	i = 0;
	for_each_engine_class_instance(fd, e) {
		if (!gem_has_engine(gem_fd, e->class, e->instance))
			continue;

		if (spin) {
			__submit_spin_batch(gem_fd, &obj, e);
		} else {
			spin = igt_spin_batch_new(gem_fd, 0,
						  e2ring(gem_fd, e), 0);
			obj.handle = spin->handle;
		}

		val[i++] = I915_PMU_ENGINE_BUSY(e->class, e->instance);
	}
	igt_assert(i == num_engines);

	fd[0] = -1;
	for (i = 0; i < num_engines; i++)
		fd[i] = open_group(val[i], fd[0]);

	pmu_read_multi(fd[0], num_engines, tval[0]);
	slept = measured_usleep(batch_duration_ns / 1000);
	if (flags & TEST_TRAILING_IDLE)
		end_spin(gem_fd, spin, flags);
	pmu_read_multi(fd[0], num_engines, tval[1]);

	end_spin(gem_fd, spin, FLAG_SYNC);
	igt_spin_batch_free(gem_fd, spin);
	close(fd[0]);

	for (i = 0; i < num_engines; i++)
		val[i] = tval[1][i] - tval[0][i];

	log_busy(num_engines, val);

	for (i = 0; i < num_engines; i++)
		assert_within_epsilon(val[i], slept, tolerance);
	gem_quiescent_gpu(gem_fd);
}

static void
no_sema(int gem_fd, const struct intel_execution_engine2 *e, unsigned int flags)
{
	igt_spin_t *spin;
	uint64_t val[2][2];
	int fd;

	fd = open_group(I915_PMU_ENGINE_SEMA(e->class, e->instance), -1);
	open_group(I915_PMU_ENGINE_WAIT(e->class, e->instance), fd);

	if (flags & TEST_BUSY)
		spin = igt_spin_batch_new(gem_fd, 0, e2ring(gem_fd, e), 0);
	else
		spin = NULL;

	pmu_read_multi(fd, 2, val[0]);
	measured_usleep(batch_duration_ns / 1000);
	if (flags & TEST_TRAILING_IDLE)
		end_spin(gem_fd, spin, flags);
	pmu_read_multi(fd, 2, val[1]);

	val[0][0] = val[1][0] - val[0][0];
	val[0][1] = val[1][1] - val[0][1];

	if (spin) {
		end_spin(gem_fd, spin, FLAG_SYNC);
		igt_spin_batch_free(gem_fd, spin);
	}
	close(fd);

	assert_within_epsilon(val[0][0], 0.0f, tolerance);
	assert_within_epsilon(val[0][1], 0.0f, tolerance);
}

#define MI_INSTR(opcode, flags) (((opcode) << 23) | (flags))
#define MI_SEMAPHORE_WAIT	MI_INSTR(0x1c, 2) /* GEN8+ */
#define   MI_SEMAPHORE_POLL		(1<<15)
#define   MI_SEMAPHORE_SAD_GTE_SDD	(1<<12)

static void
sema_wait(int gem_fd, const struct intel_execution_engine2 *e,
	  unsigned int flags)
{
	struct drm_i915_gem_relocation_entry reloc[2] = {};
	struct drm_i915_gem_exec_object2 obj[2] = {};
	struct drm_i915_gem_execbuffer2 eb = {};
	uint32_t bb_handle, obj_handle;
	unsigned long slept;
	uint32_t *obj_ptr;
	uint32_t batch[16];
	uint64_t val[2], ts[2];
	int fd;

	igt_require(intel_gen(intel_get_drm_devid(gem_fd)) >= 8);

	/**
	 * Setup up a batchbuffer with a polling semaphore wait command which
	 * will wait on an value in a shared bo to change. This way we are able
	 * to control how much time we will spend in this bb.
	 */

	bb_handle = gem_create(gem_fd, 4096);
	obj_handle = gem_create(gem_fd, 4096);

	obj_ptr = gem_mmap__wc(gem_fd, obj_handle, 0, 4096, PROT_WRITE);

	batch[0] = MI_STORE_DWORD_IMM;
	batch[1] = sizeof(*obj_ptr);
	batch[2] = 0;
	batch[3] = 1;
	batch[4] = MI_SEMAPHORE_WAIT |
		   MI_SEMAPHORE_POLL |
		   MI_SEMAPHORE_SAD_GTE_SDD;
	batch[5] = 1;
	batch[6] = 0x0;
	batch[7] = 0x0;
	batch[8] = MI_BATCH_BUFFER_END;

	gem_write(gem_fd, bb_handle, 0, batch, sizeof(batch));

	reloc[0].target_handle = obj_handle;
	reloc[0].offset = 1 * sizeof(uint32_t);
	reloc[0].read_domains = I915_GEM_DOMAIN_RENDER;
	reloc[0].write_domain = I915_GEM_DOMAIN_RENDER;
	reloc[0].delta = sizeof(*obj_ptr);

	reloc[1].target_handle = obj_handle;
	reloc[1].offset = 6 * sizeof(uint32_t);
	reloc[1].read_domains = I915_GEM_DOMAIN_RENDER;

	obj[0].handle = obj_handle;

	obj[1].handle = bb_handle;
	obj[1].relocation_count = 2;
	obj[1].relocs_ptr = to_user_pointer(reloc);

	eb.buffer_count = 2;
	eb.buffers_ptr = to_user_pointer(obj);
	eb.flags = e2ring(gem_fd, e);

	/**
	 * Start the semaphore wait PMU and after some known time let the above
	 * semaphore wait command finish. Then check that the PMU is reporting
	 * to expected time spent in semaphore wait state.
	 */

	fd = open_pmu(I915_PMU_ENGINE_SEMA(e->class, e->instance));

	val[0] = pmu_read_single(fd);

	gem_execbuf(gem_fd, &eb);
	do { /* wait for the batch to start executing */
		usleep(5e3);
	} while (!obj_ptr[1]);

	igt_assert_f(igt_wait(pmu_read_single(fd) != val[0], 10, 1),
		     "sampling failed to start withing 10ms");

	val[0] = __pmu_read_single(fd, &ts[0]);
	slept = measured_usleep(batch_duration_ns / 1000);
	if (flags & TEST_TRAILING_IDLE)
		obj_ptr[0] = 1;
	val[1] = __pmu_read_single(fd, &ts[1]);
	igt_debug("slept %.3fms (perf %.3fms), sampled %.3fms\n",
		  slept * 1e-6,
		  (ts[1] - ts[0]) * 1e-6,
		  (val[1] - val[0]) * 1e-6);

	obj_ptr[0] = 1;
	gem_sync(gem_fd, bb_handle);

	munmap(obj_ptr, 4096);
	gem_close(gem_fd, obj_handle);
	gem_close(gem_fd, bb_handle);
	close(fd);

	assert_within_epsilon(val[1] - val[0], slept, tolerance);
}

#define   MI_WAIT_FOR_PIPE_C_VBLANK (1<<21)
#define   MI_WAIT_FOR_PIPE_B_VBLANK (1<<11)
#define   MI_WAIT_FOR_PIPE_A_VBLANK (1<<3)

typedef struct {
	igt_display_t display;
	struct igt_fb primary_fb;
	igt_output_t *output;
	enum pipe pipe;
} data_t;

static void prepare_crtc(data_t *data, int fd, igt_output_t *output)
{
	drmModeModeInfo *mode;
	igt_display_t *display = &data->display;
	igt_plane_t *primary;

	/* select the pipe we want to use */
	igt_output_set_pipe(output, data->pipe);

	/* create and set the primary plane fb */
	mode = igt_output_get_mode(output);
	igt_create_color_fb(fd, mode->hdisplay, mode->vdisplay,
			    DRM_FORMAT_XRGB8888,
			    LOCAL_DRM_FORMAT_MOD_NONE,
			    0.0, 0.0, 0.0,
			    &data->primary_fb);

	primary = igt_output_get_plane_type(output, DRM_PLANE_TYPE_PRIMARY);
	igt_plane_set_fb(primary, &data->primary_fb);

	igt_display_commit(display);

	igt_wait_for_vblank(fd, data->pipe);
}

static void cleanup_crtc(data_t *data, int fd, igt_output_t *output)
{
	igt_display_t *display = &data->display;
	igt_plane_t *primary;

	igt_remove_fb(fd, &data->primary_fb);

	primary = igt_output_get_plane_type(output, DRM_PLANE_TYPE_PRIMARY);
	igt_plane_set_fb(primary, NULL);

	igt_output_set_pipe(output, PIPE_ANY);
	igt_display_commit(display);
}

static int wait_vblank(int fd, union drm_wait_vblank *vbl)
{
	int err;

	err = 0;
	if (igt_ioctl(fd, DRM_IOCTL_WAIT_VBLANK, vbl))
		err = -errno;

	return err;
}

static void
event_wait(int gem_fd, const struct intel_execution_engine2 *e)
{
	struct drm_i915_gem_exec_object2 obj = { };
	struct drm_i915_gem_execbuffer2 eb = { };
	const uint32_t DERRMR = 0x44050;
	const uint32_t FORCEWAKE_MT = 0xa188;
	unsigned int valid_tests = 0;
	uint32_t batch[16], *b;
	uint16_t devid;
	igt_output_t *output;
	data_t data;
	enum pipe p;
	int fd;

	devid = intel_get_drm_devid(gem_fd);
	igt_require(intel_gen(devid) >= 7);
	igt_skip_on(IS_VALLEYVIEW(devid) || IS_CHERRYVIEW(devid));

	kmstest_set_vt_graphics_mode();
	igt_display_init(&data.display, gem_fd);

	/**
	 * We will use the display to render event forwarind so need to
	 * program the DERRMR register and restore it at exit.
	 * Note we assume that the default/desired value for DERRMR will always
	 * be ~0u (all routing disable). To be fancy, we could do a SRM of the
	 * reg beforehand and then LRM at the end.
	 *
	 * We will emit a MI_WAIT_FOR_EVENT listening for vblank events,
	 * have a background helper to indirectly enable vblank irqs, and
	 * listen to the recorded time spent in engine wait state as reported
	 * by the PMU.
	 */
	obj.handle = gem_create(gem_fd, 4096);

	b = batch;
	*b++ = MI_LOAD_REGISTER_IMM;
	*b++ = FORCEWAKE_MT;
	*b++ = 2 << 16 | 2;
	*b++ = MI_LOAD_REGISTER_IMM;
	*b++ = DERRMR;
	*b++ = ~0u;
	*b++ = MI_WAIT_FOR_EVENT;
	*b++ = MI_LOAD_REGISTER_IMM;
	*b++ = DERRMR;
	*b++ = ~0u;
	*b++ = MI_LOAD_REGISTER_IMM;
	*b++ = FORCEWAKE_MT;
	*b++ = 2 << 16;
	*b++ = MI_BATCH_BUFFER_END;

	eb.buffer_count = 1;
	eb.buffers_ptr = to_user_pointer(&obj);
	eb.flags = e2ring(gem_fd, e) | I915_EXEC_SECURE;

	for_each_pipe_with_valid_output(&data.display, p, output) {
		struct igt_helper_process waiter = { };
		const unsigned int frames = 3;
		uint64_t val[2];

		batch[6] = MI_WAIT_FOR_EVENT;
		switch (p) {
		case PIPE_A:
			batch[6] |= MI_WAIT_FOR_PIPE_A_VBLANK;
			batch[5] = ~(1 << 3);
			break;
		case PIPE_B:
			batch[6] |= MI_WAIT_FOR_PIPE_B_VBLANK;
			batch[5] = ~(1 << 11);
			break;
		case PIPE_C:
			batch[6] |= MI_WAIT_FOR_PIPE_C_VBLANK;
			batch[5] = ~(1 << 21);
			break;
		default:
			continue;
		}

		gem_write(gem_fd, obj.handle, 0, batch, sizeof(batch));

		data.pipe = p;
		prepare_crtc(&data, gem_fd, output);

		fd = open_pmu(I915_PMU_ENGINE_WAIT(e->class, e->instance));

		val[0] = pmu_read_single(fd);

		igt_fork_helper(&waiter) {
			const uint32_t pipe_id_flag =
					kmstest_get_vbl_flag(data.pipe);

			for (;;) {
				union drm_wait_vblank vbl = { };

				vbl.request.type = DRM_VBLANK_RELATIVE;
				vbl.request.type |= pipe_id_flag;
				vbl.request.sequence = 1;
				igt_assert_eq(wait_vblank(gem_fd, &vbl), 0);
			}
		}

		for (unsigned int frame = 0; frame < frames; frame++) {
			gem_execbuf(gem_fd, &eb);
			gem_sync(gem_fd, obj.handle);
		}

		igt_stop_helper(&waiter);

		val[1] = pmu_read_single(fd);

		close(fd);

		cleanup_crtc(&data, gem_fd, output);
		valid_tests++;

		igt_assert(val[1] - val[0] > 0);
	}

	gem_close(gem_fd, obj.handle);

	igt_require_f(valid_tests,
		      "no valid crtc/connector combinations found\n");
}

static void
multi_client(int gem_fd, const struct intel_execution_engine2 *e)
{
	uint64_t config = I915_PMU_ENGINE_BUSY(e->class, e->instance);
	unsigned long slept[2];
	uint64_t val[2], ts[2], perf_slept[2];
	igt_spin_t *spin;
	int fd[2];

	gem_quiescent_gpu(gem_fd);

	fd[0] = open_pmu(config);

	/*
	 * Second PMU client which is initialized after the first one,
	 * and exists before it, should not affect accounting as reported
	 * in the first client.
	 */
	fd[1] = open_pmu(config);

	spin = igt_spin_batch_new(gem_fd, 0, e2ring(gem_fd, e), 0);

	val[0] = val[1] = __pmu_read_single(fd[0], &ts[0]);
	slept[1] = measured_usleep(batch_duration_ns / 1000);
	val[1] = __pmu_read_single(fd[1], &ts[1]) - val[1];
	perf_slept[1] = ts[1] - ts[0];
	igt_debug("slept=%lu perf=%"PRIu64"\n", slept[1], perf_slept[1]);
	close(fd[1]);

	slept[0] = measured_usleep(batch_duration_ns / 1000) + slept[1];
	val[0] = __pmu_read_single(fd[0], &ts[1]) - val[0];
	perf_slept[0] = ts[1] - ts[0];
	igt_debug("slept=%lu perf=%"PRIu64"\n", slept[0], perf_slept[0]);

	igt_spin_batch_end(spin);
	gem_sync(gem_fd, spin->handle);
	igt_spin_batch_free(gem_fd, spin);
	close(fd[0]);

	assert_within_epsilon(val[0], perf_slept[0], tolerance);
	assert_within_epsilon(val[1], perf_slept[1], tolerance);
}

/**
 * Tests that i915 PMU corectly errors out in invalid initialization.
 * i915 PMU is uncore PMU, thus:
 *  - sampling period is not supported
 *  - pid > 0 is not supported since we can't count per-process (we count
 *    per whole system)
 *  - cpu != 0 is not supported since i915 PMU only allows running on one cpu
 *    and that is normally CPU0.
 */
static void invalid_init(void)
{
	struct perf_event_attr attr;

#define ATTR_INIT() \
do { \
	memset(&attr, 0, sizeof (attr)); \
	attr.config = I915_PMU_ENGINE_BUSY(I915_ENGINE_CLASS_RENDER, 0); \
	attr.type = i915_type_id(); \
	igt_assert(attr.type != 0); \
	errno = 0; \
} while(0)

	ATTR_INIT();
	attr.sample_period = 100;
	igt_assert_eq(perf_event_open(&attr, -1, 0, -1, 0), -1);
	igt_assert_eq(errno, EINVAL);

	ATTR_INIT();
	igt_assert_eq(perf_event_open(&attr, 0, 0, -1, 0), -1);
	igt_assert_eq(errno, EINVAL);

	ATTR_INIT();
	igt_assert_eq(perf_event_open(&attr, -1, 1, -1, 0), -1);
	igt_assert_eq(errno, EINVAL);
}

static void init_other(unsigned int i, bool valid)
{
	int fd;

	fd = perf_i915_open(__I915_PMU_OTHER(i));
	igt_require(!(fd < 0 && errno == ENODEV));
	if (valid) {
		igt_assert(fd >= 0);
	} else {
		igt_assert(fd < 0);
		return;
	}

	close(fd);
}

static void read_other(unsigned int i, bool valid)
{
	int fd;

	fd = perf_i915_open(__I915_PMU_OTHER(i));
	igt_require(!(fd < 0 && errno == ENODEV));
	if (valid) {
		igt_assert(fd >= 0);
	} else {
		igt_assert(fd < 0);
		return;
	}

	(void)pmu_read_single(fd);

	close(fd);
}

static bool cpu0_hotplug_support(void)
{
	return access("/sys/devices/system/cpu/cpu0/online", W_OK) == 0;
}

static void cpu_hotplug(int gem_fd)
{
	igt_spin_t *spin[2];
	uint64_t ts[2];
	uint64_t val;
	int link[2];
	int fd, ret;
	int cur = 0;

	igt_skip_on(IS_BROXTON(intel_get_drm_devid(gem_fd)));
	igt_require(cpu0_hotplug_support());

	fd = open_pmu(I915_PMU_ENGINE_BUSY(I915_ENGINE_CLASS_RENDER, 0));

	/*
	 * Create two spinners so test can ensure shorter gaps in engine
	 * busyness as it is terminating one and re-starting the other.
	 */
	spin[0] = igt_spin_batch_new(gem_fd, 0, I915_EXEC_RENDER, 0);
	spin[1] = __igt_spin_batch_new(gem_fd, 0, I915_EXEC_RENDER, 0);

	val = __pmu_read_single(fd, &ts[0]);

	ret = pipe2(link, O_NONBLOCK);
	igt_assert_eq(ret, 0);

	/*
	 * Toggle online status of all the CPUs in a child process and ensure
	 * this has not affected busyness stats in the parent.
	 */
	igt_fork(child, 1) {
		int cpu = 0;

		close(link[0]);

		for (;;) {
			char name[128];
			int cpufd;

			igt_assert_lt(snprintf(name, sizeof(name),
					       "/sys/devices/system/cpu/cpu%d/online",
					       cpu), sizeof(name));
			cpufd = open(name, O_WRONLY);
			if (cpufd == -1) {
				igt_assert(cpu > 0);
				/*
				 * Signal parent that we cycled through all
				 * CPUs and we are done.
				 */
				igt_assert_eq(write(link[1], "*", 1), 1);
				break;
			}

			/* Offline followed by online a CPU. */
			igt_assert_eq(write(cpufd, "0", 2), 2);
			usleep(1e6);
			igt_assert_eq(write(cpufd, "1", 2), 2);

			close(cpufd);
			cpu++;
		}
	}

	close(link[1]);

	/*
	 * Very long batches can be declared as GPU hangs so emit shorter ones
	 * until the CPU core shuffler finishes one loop.
	 */
	for (;;) {
		char buf;
		int ret2;

		usleep(500e3);
		end_spin(gem_fd, spin[cur], 0);

		/* Check if the child is signaling completion. */
		ret2 = read(link[0], &buf, 1);
		if ( ret2 == 1 || (ret2 < 0 && errno != EAGAIN))
			break;

		igt_spin_batch_free(gem_fd, spin[cur]);
		spin[cur] = __igt_spin_batch_new(gem_fd, 0, I915_EXEC_RENDER,
						 0);
		cur ^= 1;
	}

	val = __pmu_read_single(fd, &ts[1]) - val;

	end_spin(gem_fd, spin[0], FLAG_SYNC);
	end_spin(gem_fd, spin[1], FLAG_SYNC);
	igt_spin_batch_free(gem_fd, spin[0]);
	igt_spin_batch_free(gem_fd, spin[1]);
	igt_waitchildren();
	close(fd);
	close(link[0]);

	assert_within_epsilon(val, ts[1] - ts[0], tolerance);
}

static void
test_interrupts(int gem_fd)
{
	const unsigned int test_duration_ms = 1000;
	const int target = 30;
	igt_spin_t *spin[target];
	struct pollfd pfd;
	uint64_t idle, busy;
	int fence_fd;
	int fd;

	gem_quiescent_gpu(gem_fd);

	fd = open_pmu(I915_PMU_INTERRUPTS);

	/* Queue spinning batches. */
	for (int i = 0; i < target; i++) {
		spin[i] = __igt_spin_batch_new_fence(gem_fd,
						     0, I915_EXEC_RENDER);
		if (i == 0) {
			fence_fd = spin[i]->out_fence;
		} else {
			int old_fd = fence_fd;

			fence_fd = sync_fence_merge(old_fd,
						    spin[i]->out_fence);
			close(old_fd);
		}

		igt_assert(fence_fd >= 0);
	}

	/* Wait for idle state. */
	idle = pmu_read_single(fd);
	do {
		busy = idle;
		usleep(1e3);
		idle = pmu_read_single(fd);
	} while (idle != busy);

	/* Arm batch expiration. */
	for (int i = 0; i < target; i++)
		igt_spin_batch_set_timeout(spin[i],
					   (i + 1) * test_duration_ms * 1e6
					   / target);

	/* Wait for last batch to finish. */
	pfd.events = POLLIN;
	pfd.fd = fence_fd;
	igt_assert_eq(poll(&pfd, 1, 2 * test_duration_ms), 1);
	close(fence_fd);

	/* Free batches. */
	for (int i = 0; i < target; i++)
		igt_spin_batch_free(gem_fd, spin[i]);

	/* Check at least as many interrupts has been generated. */
	busy = pmu_read_single(fd) - idle;
	close(fd);

	igt_assert_lte(target, busy);
}

static void
test_interrupts_sync(int gem_fd)
{
	const unsigned int test_duration_ms = 1000;
	const int target = 30;
	igt_spin_t *spin[target];
	struct pollfd pfd;
	uint64_t idle, busy;
	int fd;

	gem_quiescent_gpu(gem_fd);

	fd = open_pmu(I915_PMU_INTERRUPTS);

	/* Queue spinning batches. */
	for (int i = 0; i < target; i++)
		spin[i] = __igt_spin_batch_new_fence(gem_fd, 0, 0);

	/* Wait for idle state. */
	idle = pmu_read_single(fd);
	do {
		busy = idle;
		usleep(1e3);
		idle = pmu_read_single(fd);
	} while (idle != busy);

	/* Process the batch queue. */
	pfd.events = POLLIN;
	for (int i = 0; i < target; i++) {
		const unsigned int timeout_ms = test_duration_ms / target;

		pfd.fd = spin[i]->out_fence;
		igt_spin_batch_set_timeout(spin[i], timeout_ms * 1e6);
		igt_assert_eq(poll(&pfd, 1, 2 * timeout_ms), 1);
		igt_spin_batch_free(gem_fd, spin[i]);
	}

	/* Check at least as many interrupts has been generated. */
	busy = pmu_read_single(fd) - idle;
	close(fd);

	igt_assert_lte(target, busy);
}

static void
test_frequency(int gem_fd)
{
	uint32_t min_freq, max_freq, boost_freq;
	uint64_t val[2], start[2], slept;
	double min[2], max[2];
	igt_spin_t *spin;
	int fd, sysfs;

	sysfs = igt_sysfs_open(gem_fd, NULL);
	igt_require(sysfs >= 0);

	min_freq = igt_sysfs_get_u32(sysfs, "gt_RPn_freq_mhz");
	max_freq = igt_sysfs_get_u32(sysfs, "gt_RP0_freq_mhz");
	boost_freq = igt_sysfs_get_u32(sysfs, "gt_boost_freq_mhz");
	igt_info("Frequency: min=%u, max=%u, boost=%u MHz\n",
		 min_freq, max_freq, boost_freq);
	igt_require(min_freq > 0 && max_freq > 0 && boost_freq > 0);
	igt_require(max_freq > min_freq);
	igt_require(boost_freq > min_freq);

	fd = open_group(I915_PMU_REQUESTED_FREQUENCY, -1);
	open_group(I915_PMU_ACTUAL_FREQUENCY, fd);

	/*
	 * Set GPU to min frequency and read PMU counters.
	 */
	igt_require(igt_sysfs_set_u32(sysfs, "gt_min_freq_mhz", min_freq));
	igt_require(igt_sysfs_get_u32(sysfs, "gt_min_freq_mhz") == min_freq);
	igt_require(igt_sysfs_set_u32(sysfs, "gt_max_freq_mhz", min_freq));
	igt_require(igt_sysfs_get_u32(sysfs, "gt_max_freq_mhz") == min_freq);
	igt_require(igt_sysfs_set_u32(sysfs, "gt_boost_freq_mhz", min_freq));
	igt_require(igt_sysfs_get_u32(sysfs, "gt_boost_freq_mhz") == min_freq);

	gem_quiescent_gpu(gem_fd); /* Idle to be sure the change takes effect */
	spin = igt_spin_batch_new(gem_fd, 0, I915_EXEC_RENDER, 0);

	slept = pmu_read_multi(fd, 2, start);
	measured_usleep(batch_duration_ns / 1000);
	slept = pmu_read_multi(fd, 2, val) - slept;

	min[0] = 1e9*(val[0] - start[0]) / slept;
	min[1] = 1e9*(val[1] - start[1]) / slept;

	igt_spin_batch_free(gem_fd, spin);
	gem_quiescent_gpu(gem_fd); /* Don't leak busy bo into the next phase */

	usleep(1e6);

	/*
	 * Set GPU to max frequency and read PMU counters.
	 */
	igt_require(igt_sysfs_set_u32(sysfs, "gt_max_freq_mhz", max_freq));
	igt_require(igt_sysfs_get_u32(sysfs, "gt_max_freq_mhz") == max_freq);
	igt_require(igt_sysfs_set_u32(sysfs, "gt_boost_freq_mhz", boost_freq));
	igt_require(igt_sysfs_get_u32(sysfs, "gt_boost_freq_mhz") == boost_freq);

	igt_require(igt_sysfs_set_u32(sysfs, "gt_min_freq_mhz", max_freq));
	igt_require(igt_sysfs_get_u32(sysfs, "gt_min_freq_mhz") == max_freq);

	gem_quiescent_gpu(gem_fd);
	spin = igt_spin_batch_new(gem_fd, 0, I915_EXEC_RENDER, 0);

	slept = pmu_read_multi(fd, 2, start);
	measured_usleep(batch_duration_ns / 1000);
	slept = pmu_read_multi(fd, 2, val) - slept;

	max[0] = 1e9*(val[0] - start[0]) / slept;
	max[1] = 1e9*(val[1] - start[1]) / slept;

	igt_spin_batch_free(gem_fd, spin);
	gem_quiescent_gpu(gem_fd);

	/*
	 * Restore min/max.
	 */
	igt_sysfs_set_u32(sysfs, "gt_min_freq_mhz", min_freq);
	if (igt_sysfs_get_u32(sysfs, "gt_min_freq_mhz") != min_freq)
		igt_warn("Unable to restore min frequency to saved value [%u MHz], now %u MHz\n",
			 min_freq, igt_sysfs_get_u32(sysfs, "gt_min_freq_mhz"));
	close(fd);

	igt_info("Min frequency: requested %.1f, actual %.1f\n",
		 min[0], min[1]);
	igt_info("Max frequency: requested %.1f, actual %.1f\n",
		 max[0], max[1]);

	assert_within_epsilon(min[0], min_freq, tolerance);
	/*
	 * On thermally throttled devices we cannot be sure maximum frequency
	 * can be reached so use larger tolerance downards.
	 */
	__assert_within_epsilon(max[0], max_freq, tolerance, 0.15f);
}

static bool wait_for_rc6(int fd)
{
	struct timespec tv = {};
	uint64_t start, now;

	/* First wait for roughly an RC6 Evaluation Interval */
	usleep(160 * 1000);

	/* Then poll for RC6 to start ticking */
	now = pmu_read_single(fd);
	do {
		start = now;
		usleep(5000);
		now = pmu_read_single(fd);
		if (now - start > 1e6)
			return true;
	} while (!igt_seconds_elapsed(&tv));

	return false;
}

static void
test_rc6(int gem_fd, unsigned int flags)
{
	int64_t duration_ns = 2e9;
	uint64_t idle, busy, prev, ts[2];
	unsigned long slept;
	int fd, fw;

	gem_quiescent_gpu(gem_fd);

	fd = open_pmu(I915_PMU_RC6_RESIDENCY);

	if (flags & TEST_RUNTIME_PM) {
		drmModeRes *res;

		res = drmModeGetResources(gem_fd);
		igt_assert(res);

		/* force all connectors off */
		kmstest_set_vt_graphics_mode();
		kmstest_unset_all_crtcs(gem_fd, res);
		drmModeFreeResources(res);

		igt_require(igt_setup_runtime_pm());
		igt_require(igt_wait_for_pm_status(IGT_RUNTIME_PM_STATUS_SUSPENDED));

		/*
		 * Sleep for a bit to see if once woken up estimated RC6 hasn't
		 * drifted to far in advance of real RC6.
		 */
		if (flags & FLAG_LONG) {
			pmu_read_single(fd);
			sleep(5);
			pmu_read_single(fd);
		}
	}

	igt_require(wait_for_rc6(fd));

	/* While idle check full RC6. */
	prev = __pmu_read_single(fd, &ts[0]);
	slept = measured_usleep(duration_ns / 1000);
	idle = __pmu_read_single(fd, &ts[1]);
	igt_debug("slept=%lu perf=%"PRIu64"\n", slept, ts[1] - ts[0]);

	assert_within_epsilon(idle - prev, ts[1] - ts[0], tolerance);

	/* Wake up device and check no RC6. */
	fw = igt_open_forcewake_handle(gem_fd);
	igt_assert(fw >= 0);
	usleep(1e3); /* wait for the rc6 cycle counter to stop ticking */

	prev = pmu_read_single(fd);
	usleep(duration_ns / 1000);
	busy = pmu_read_single(fd);

	close(fw);
	close(fd);

	assert_within_epsilon(busy - prev, 0.0, tolerance);
}

static void
test_enable_race(int gem_fd, const struct intel_execution_engine2 *e)
{
	uint64_t config = I915_PMU_ENGINE_BUSY(e->class, e->instance);
	struct igt_helper_process engine_load = { };
	const uint32_t bbend = MI_BATCH_BUFFER_END;
	struct drm_i915_gem_exec_object2 obj = { };
	struct drm_i915_gem_execbuffer2 eb = { };
	int fd;

	igt_require(gem_has_execlists(gem_fd));
	igt_require(gem_has_engine(gem_fd, e->class, e->instance));

	obj.handle = gem_create(gem_fd, 4096);
	gem_write(gem_fd, obj.handle, 0, &bbend, sizeof(bbend));

	eb.buffer_count = 1;
	eb.buffers_ptr = to_user_pointer(&obj);
	eb.flags = e2ring(gem_fd, e);

	/*
	 * This test is probabilistic so run in a few times to increase the
	 * chance of hitting the race.
	 */
	igt_until_timeout(10) {
		/*
		 * Defeat the busy stats delayed disable, we need to guarantee
		 * we are the first PMU user.
		 */
		gem_quiescent_gpu(gem_fd);
		sleep(2);

		/* Apply interrupt-heavy load on the engine. */
		igt_fork_helper(&engine_load) {
			for (;;)
				gem_execbuf(gem_fd, &eb);
		}

		/* Wait a bit to allow engine load to start. */
		usleep(500e3);

		/* Enable the PMU. */
		fd = open_pmu(config);

		/* Stop load and close the PMU. */
		igt_stop_helper(&engine_load);
		close(fd);
	}

	/* Cleanup. */
	gem_close(gem_fd, obj.handle);
	gem_quiescent_gpu(gem_fd);
}

static double __error(double val, double ref)
{
	igt_assert(ref > 1e-5 /* smallval */);
	return (100.0 * val / ref) - 100.0;
}

static void __rearm_spin_batch(igt_spin_t *spin)
{
	const uint32_t mi_arb_chk = 0x5 << 23;

       *spin->batch = mi_arb_chk;
       __sync_synchronize();
}

#define div_round_up(a, b) (((a) + (b) - 1) / (b))

static void
accuracy(int gem_fd, const struct intel_execution_engine2 *e,
	 unsigned long target_busy_pct)
{
	const unsigned int min_test_loops = 7;
	const unsigned long min_test_us = 1e6;
	unsigned long busy_us = 2500;
	unsigned long idle_us = 100 * (busy_us - target_busy_pct *
				busy_us / 100) / target_busy_pct;
	unsigned long pwm_calibration_us;
	unsigned long test_us;
	double busy_r, expected;
	uint64_t val[2];
	uint64_t ts[2];
	int link[2];
	int fd;

	/* Sampling platforms cannot reach the high accuracy criteria. */
	igt_require(gem_has_execlists(gem_fd));

	while (idle_us < 2500) {
		busy_us *= 2;
		idle_us *= 2;
	}

	pwm_calibration_us = min_test_loops * (busy_us + idle_us);
	while (pwm_calibration_us < min_test_us)
		pwm_calibration_us += busy_us + idle_us;
	test_us = min_test_loops * (idle_us + busy_us);
	while (test_us < min_test_us)
		test_us += busy_us + idle_us;

	igt_info("calibration=%luus, test=%luus; ratio=%.2f%% (%luus/%luus)\n",
		 pwm_calibration_us, test_us,
		 (double)busy_us / (busy_us + idle_us) * 100.0,
		 busy_us, idle_us);

	assert_within_epsilon((double)busy_us / (busy_us + idle_us),
				(double)target_busy_pct / 100.0, tolerance);

	igt_assert(pipe(link) == 0);

	/* Emit PWM pattern on the engine from a child. */
	igt_fork(child, 1) {
		struct sched_param rt = { .sched_priority = 99 };
		const unsigned long timeout[] = {
			pwm_calibration_us * 1000, test_us * 2 * 1000
		};
		struct drm_i915_gem_exec_object2 obj = {};
		uint64_t total_busy_ns = 0, total_idle_ns = 0;
		igt_spin_t *spin;
		int ret;

		/* We need the best sleep accuracy we can get. */
		ret = sched_setscheduler(0,
					 SCHED_FIFO | SCHED_RESET_ON_FORK,
					 &rt);
		if (ret)
			igt_warn("Failed to set scheduling policy!\n");

		/* Allocate our spin batch and idle it. */
		spin = igt_spin_batch_new(gem_fd, 0, e2ring(gem_fd, e), 0);
		obj.handle = spin->handle;
		__submit_spin_batch(gem_fd, &obj, e); /* record its location */
		igt_spin_batch_end(spin);
		gem_sync(gem_fd, obj.handle);
		obj.flags |= EXEC_OBJECT_PINNED;

		/* 1st pass is calibration, second pass is the test. */
		for (int pass = 0; pass < ARRAY_SIZE(timeout); pass++) {
			uint64_t busy_ns = -total_busy_ns;
			uint64_t idle_ns = -total_idle_ns;
			struct timespec test_start = { };

			igt_nsec_elapsed(&test_start);
			do {
				struct timespec t_busy = { };
				unsigned int target_idle_us;

				igt_nsec_elapsed(&t_busy);

				/* Restart the spinbatch. */
				__rearm_spin_batch(spin);
				__submit_spin_batch(gem_fd, &obj, e);
				measured_usleep(busy_us);
				igt_spin_batch_end(spin);
				gem_sync(gem_fd, obj.handle);

				total_busy_ns += igt_nsec_elapsed(&t_busy);

				target_idle_us =
					(100 * total_busy_ns / target_busy_pct - (total_busy_ns + total_idle_ns)) / 1000;
				total_idle_ns += measured_usleep(target_idle_us);
			} while (igt_nsec_elapsed(&test_start) < timeout[pass]);

			busy_ns += total_busy_ns;
			idle_ns += total_idle_ns;

			expected = (double)busy_ns / (busy_ns + idle_ns);
			igt_info("%u: busy %"PRIu64"us, idle %"PRIu64"us: %.2f%% (target: %lu%%)\n",
				 pass, busy_ns / 1000, idle_ns / 1000,
				 100 * expected, target_busy_pct);
			write(link[1], &expected, sizeof(expected));
		}

		igt_spin_batch_free(gem_fd, spin);
	}

	/* Let the child run. */
	read(link[0], &expected, sizeof(expected));
	assert_within_epsilon(expected, target_busy_pct/100., 0.05);

	/* Collect engine busyness for an interesting part of child runtime. */
	fd = open_pmu(I915_PMU_ENGINE_BUSY(e->class, e->instance));
	val[0] = __pmu_read_single(fd, &ts[0]);
	read(link[0], &expected, sizeof(expected));
	val[1] = __pmu_read_single(fd, &ts[1]);
	close(fd);

	close(link[1]);
	close(link[0]);

	igt_waitchildren();

	busy_r = (double)(val[1] - val[0]) / (ts[1] - ts[0]);

	igt_info("error=%.2f%% (%.2f%% vs %.2f%%)\n",
		 __error(busy_r, expected), 100 * busy_r, 100 * expected);

	assert_within_epsilon(busy_r, expected, 0.15);
	assert_within_epsilon(1 - busy_r, 1 - expected, 0.15);
}

igt_main
{
	const unsigned int num_other_metrics =
				I915_PMU_LAST - __I915_PMU_OTHER(0) + 1;
	unsigned int num_engines = 0;
	int fd = -1;
	const struct intel_execution_engine2 *e;
	unsigned int i;

	igt_fixture {
		fd = drm_open_driver_master(DRIVER_INTEL);

		igt_require_gem(fd);
		igt_require(i915_type_id() > 0);

		for_each_engine_class_instance(fd, e) {
			if (gem_has_engine(fd, e->class, e->instance))
				num_engines++;
		}
	}

	/**
	 * Test invalid access via perf API is rejected.
	 */
	igt_subtest("invalid-init")
		invalid_init();

	for_each_engine_class_instance(fd, e) {
		const unsigned int pct[] = { 2, 50, 98 };

		/**
		 * Test that a single engine metric can be initialized or it
		 * is correctly rejected.
		 */
		igt_subtest_f("init-busy-%s", e->name)
			init(fd, e, I915_SAMPLE_BUSY);

		igt_subtest_f("init-wait-%s", e->name)
			init(fd, e, I915_SAMPLE_WAIT);

		igt_subtest_f("init-sema-%s", e->name)
			init(fd, e, I915_SAMPLE_SEMA);

		igt_subtest_group {
			igt_fixture {
				gem_require_engine(fd, e->class, e->instance);
			}

			/**
			 * Test that engines show no load when idle.
			 */
			igt_subtest_f("idle-%s", e->name)
				single(fd, e, 0);

			/**
			 * Test that a single engine reports load correctly.
			 */
			igt_subtest_f("busy-%s", e->name)
				single(fd, e, TEST_BUSY);
			igt_subtest_f("busy-idle-%s", e->name)
				single(fd, e, TEST_BUSY | TEST_TRAILING_IDLE);

			/**
			 * Test that when one engine is loaded other report no
			 * load.
			 */
			igt_subtest_f("busy-check-all-%s", e->name)
				busy_check_all(fd, e, num_engines, TEST_BUSY);
			igt_subtest_f("busy-idle-check-all-%s", e->name)
				busy_check_all(fd, e, num_engines,
					       TEST_BUSY | TEST_TRAILING_IDLE);

			/**
			 * Test that when all except one engine are loaded all
			 * loads are correctly reported.
			 */
			igt_subtest_f("most-busy-check-all-%s", e->name)
				most_busy_check_all(fd, e, num_engines,
						    TEST_BUSY);
			igt_subtest_f("most-busy-idle-check-all-%s", e->name)
				most_busy_check_all(fd, e, num_engines,
						    TEST_BUSY |
						    TEST_TRAILING_IDLE);

			/**
			 * Test that semphore counters report no activity on
			 * idle or busy engines.
			 */
			igt_subtest_f("idle-no-semaphores-%s", e->name)
				no_sema(fd, e, 0);

			igt_subtest_f("busy-no-semaphores-%s", e->name)
				no_sema(fd, e, TEST_BUSY);

			igt_subtest_f("busy-idle-no-semaphores-%s", e->name)
				no_sema(fd, e, TEST_BUSY | TEST_TRAILING_IDLE);

			/**
			 * Test that semaphore waits are correctly reported.
			 */
			igt_subtest_f("semaphore-wait-%s", e->name)
				sema_wait(fd, e, TEST_BUSY);

			igt_subtest_f("semaphore-wait-idle-%s", e->name)
				sema_wait(fd, e,
					  TEST_BUSY | TEST_TRAILING_IDLE);

			/**
			 * Check that two perf clients do not influence each
			 * others observations.
			 */
			igt_subtest_f("multi-client-%s", e->name)
				multi_client(fd, e);

			/**
			* Check that reported usage is correct when PMU is
			* enabled after the batch is running.
			*/
			igt_subtest_f("busy-start-%s", e->name)
				busy_start(fd, e);

			/**
			 * Check that reported usage is correct when PMU is
			 * enabled after two batches are running.
			 */
			igt_subtest_f("busy-double-start-%s", e->name) {
				gem_require_contexts(fd);
				busy_double_start(fd, e);
			}

			/**
			 * Check that the PMU can be safely enabled in face of
			 * interrupt-heavy engine load.
			 */
			igt_subtest_f("enable-race-%s", e->name)
				test_enable_race(fd, e);

			/**
			 * Check engine busyness accuracy is as expected.
			 */
			for (i = 0; i < ARRAY_SIZE(pct); i++) {
				igt_subtest_f("busy-accuracy-%u-%s",
					      pct[i], e->name)
					accuracy(fd, e, pct[i]);
			}

			igt_subtest_f("busy-hang-%s", e->name)
				single(fd, e, TEST_BUSY | FLAG_HANG);
		}

		/**
		 * Test that event waits are correctly reported.
		 */
		if (e->class == I915_ENGINE_CLASS_RENDER)
			igt_subtest_f("event-wait-%s", e->name)
				event_wait(fd, e);
	}

	/**
	 * Test that when all engines are loaded all loads are
	 * correctly reported.
	 */
	igt_subtest("all-busy-check-all")
		all_busy_check_all(fd, num_engines, TEST_BUSY);
	igt_subtest("all-busy-idle-check-all")
		all_busy_check_all(fd, num_engines,
				   TEST_BUSY | TEST_TRAILING_IDLE);

	/**
	 * Test that non-engine counters can be initialized and read. Apart
	 * from the invalid metric which should fail.
	 */
	for (i = 0; i < num_other_metrics + 1; i++) {
		igt_subtest_f("other-init-%u", i)
			init_other(i, i < num_other_metrics);

		igt_subtest_f("other-read-%u", i)
			read_other(i, i < num_other_metrics);
	}

	/**
	 * Test counters are not affected by CPU offline/online events.
	 */
	igt_subtest("cpu-hotplug")
		cpu_hotplug(fd);

	/**
	 * Test GPU frequency.
	 */
	igt_subtest("frequency")
		test_frequency(fd);

	/**
	 * Test interrupt count reporting.
	 */
	igt_subtest("interrupts")
		test_interrupts(fd);

	igt_subtest("interrupts-sync")
		test_interrupts_sync(fd);

	/**
	 * Test RC6 residency reporting.
	 */
	igt_subtest("rc6")
		test_rc6(fd, 0);

	igt_subtest("rc6-runtime-pm")
		test_rc6(fd, TEST_RUNTIME_PM);

	igt_subtest("rc6-runtime-pm-long")
		test_rc6(fd, TEST_RUNTIME_PM | FLAG_LONG);

	/**
	 * Check render nodes are counted.
	 */
	igt_subtest_group {
		int render_fd = -1;

		igt_fixture {
			render_fd = drm_open_driver_render(DRIVER_INTEL);
			igt_require_gem(render_fd);

			gem_quiescent_gpu(fd);
		}

		for_each_engine_class_instance(fd, e) {
			igt_subtest_group {
				igt_fixture {
					gem_require_engine(render_fd,
							   e->class,
							   e->instance);
				}

				igt_subtest_f("render-node-busy-%s", e->name)
					single(fd, e, TEST_BUSY);
				igt_subtest_f("render-node-busy-idle-%s",
					      e->name)
					single(fd, e,
					       TEST_BUSY | TEST_TRAILING_IDLE);
			}
		}

		igt_fixture {
			close(render_fd);
		}
	}
}
