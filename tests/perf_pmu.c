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

#include "igt.h"
#include "igt_core.h"
#include "igt_perf.h"
#include "igt_sysfs.h"
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
	int fd;

	fd = open_pmu(__I915_PMU_ENGINE(e->class, e->instance, sample));

	close(fd);
}

static uint64_t pmu_read_single(int fd)
{
	uint64_t data[2];

	igt_assert_eq(read(fd, data, sizeof(data)), sizeof(data));

	return data[0];
}

static void pmu_read_multi(int fd, unsigned int num, uint64_t *val)
{
	uint64_t buf[2 + num];
	unsigned int i;

	igt_assert_eq(read(fd, buf, sizeof(buf)), sizeof(buf));

	for (i = 0; i < num; i++)
		val[i] = buf[2 + i];
}

#define assert_within_epsilon(x, ref, tolerance) \
	igt_assert_f((double)(x) <= (1.0 + (tolerance)) * (double)(ref) && \
		     (double)(x) >= (1.0 - (tolerance)) * (double)(ref), \
		     "'%s' != '%s' (%f not within %f%% tolerance of %f)\n",\
		     #x, #ref, (double)(x), (tolerance) * 100.0, (double)ref)

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

static void
single(int gem_fd, const struct intel_execution_engine2 *e, bool busy)
{
	double ref = busy ? batch_duration_ns : 0.0f;
	igt_spin_t *spin;
	uint64_t val;
	int fd;

	fd = open_pmu(I915_PMU_ENGINE_BUSY(e->class, e->instance));

	if (busy) {
		spin = igt_spin_batch_new(gem_fd, 0, e2ring(gem_fd, e), 0);
		igt_spin_batch_set_timeout(spin, batch_duration_ns);
	} else {
		usleep(batch_duration_ns / 1000);
	}

	if (busy)
		gem_sync(gem_fd, spin->handle);

	val = pmu_read_single(fd);

	if (busy)
		igt_spin_batch_free(gem_fd, spin);
	close(fd);

	assert_within_epsilon(val, ref, tolerance);
}

static void log_busy(int fd, unsigned int num_engines, uint64_t *val)
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
	       const unsigned int num_engines)
{
	const struct intel_execution_engine2 *e_;
	uint64_t val[num_engines];
	int fd[num_engines];
	igt_spin_t *spin;
	unsigned int busy_idx, i;

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
	igt_spin_batch_set_timeout(spin, batch_duration_ns);

	gem_sync(gem_fd, spin->handle);

	pmu_read_multi(fd[0], num_engines, val);
	log_busy(fd[0], num_engines, val);

	igt_spin_batch_free(gem_fd, spin);
	close(fd[0]);

	assert_within_epsilon(val[busy_idx], batch_duration_ns, tolerance);
	for (i = 0; i < num_engines; i++) {
		if (i == busy_idx)
			continue;
		assert_within_epsilon(val[i], 0.0f, tolerance);
	}

}

static void
most_busy_check_all(int gem_fd, const struct intel_execution_engine2 *e,
		    const unsigned int num_engines)
{
	const struct intel_execution_engine2 *e_;
	uint64_t val[num_engines];
	int fd[num_engines];
	igt_spin_t *spin[num_engines];
	unsigned int idle_idx, i;

	gem_require_engine(gem_fd, e->class, e->instance);

	i = 0;
	fd[0] = -1;
	for_each_engine_class_instance(fd, e_) {
		if (!gem_has_engine(gem_fd, e_->class, e_->instance))
			continue;

		fd[i] = open_group(I915_PMU_ENGINE_BUSY(e_->class,
							e_->instance),
				   fd[0]);

		if (e == e_) {
			idle_idx = i;
		} else {
			spin[i] = igt_spin_batch_new(gem_fd, 0,
						     e2ring(gem_fd, e_), 0);
			igt_spin_batch_set_timeout(spin[i], batch_duration_ns);
		}

		i++;
	}

	for (i = 0; i < num_engines; i++) {
		if (i != idle_idx)
			gem_sync(gem_fd, spin[i]->handle);
	}

	pmu_read_multi(fd[0], num_engines, val);
	log_busy(fd[0], num_engines, val);

	for (i = 0; i < num_engines; i++) {
		if (i != idle_idx)
			igt_spin_batch_free(gem_fd, spin[i]);
	}
	close(fd[0]);

	for (i = 0; i < num_engines; i++) {
		if (i == idle_idx)
			assert_within_epsilon(val[i], 0.0f, tolerance);
		else
			assert_within_epsilon(val[i], batch_duration_ns,
					      tolerance);
	}
}

static void
all_busy_check_all(int gem_fd, const unsigned int num_engines)
{
	const struct intel_execution_engine2 *e;
	uint64_t val[num_engines];
	int fd[num_engines];
	igt_spin_t *spin[num_engines];
	unsigned int i;

	i = 0;
	fd[0] = -1;
	for_each_engine_class_instance(fd, e) {
		if (!gem_has_engine(gem_fd, e->class, e->instance))
			continue;

		fd[i] = open_group(I915_PMU_ENGINE_BUSY(e->class, e->instance),
				   fd[0]);

		spin[i] = igt_spin_batch_new(gem_fd, 0, e2ring(gem_fd, e), 0);
		igt_spin_batch_set_timeout(spin[i], batch_duration_ns);

		i++;
	}

	for (i = 0; i < num_engines; i++)
		gem_sync(gem_fd, spin[i]->handle);

	pmu_read_multi(fd[0], num_engines, val);
	log_busy(fd[0], num_engines, val);

	for (i = 0; i < num_engines; i++)
		igt_spin_batch_free(gem_fd, spin[i]);
	close(fd[0]);

	for (i = 0; i < num_engines; i++)
		assert_within_epsilon(val[i], batch_duration_ns, tolerance);
}

static void
no_sema(int gem_fd, const struct intel_execution_engine2 *e, bool busy)
{
	igt_spin_t *spin;
	uint64_t val[2];
	int fd;

	fd = open_group(I915_PMU_ENGINE_SEMA(e->class, e->instance), -1);
	open_group(I915_PMU_ENGINE_WAIT(e->class, e->instance), fd);

	if (busy) {
		spin = igt_spin_batch_new(gem_fd, 0, e2ring(gem_fd, e), 0);
		igt_spin_batch_set_timeout(spin, batch_duration_ns);
	} else {
		usleep(batch_duration_ns / 1000);
	}

	if (busy)
		gem_sync(gem_fd, spin->handle);

	pmu_read_multi(fd, 2, val);

	if (busy)
		igt_spin_batch_free(gem_fd, spin);
	close(fd);

	assert_within_epsilon(val[0], 0.0f, tolerance);
	assert_within_epsilon(val[1], 0.0f, tolerance);
}

#define MI_INSTR(opcode, flags) (((opcode) << 23) | (flags))
#define MI_SEMAPHORE_WAIT	MI_INSTR(0x1c, 2) /* GEN8+ */
#define   MI_SEMAPHORE_POLL		(1<<15)
#define   MI_SEMAPHORE_SAD_GTE_SDD	(1<<12)

static void
sema_wait(int gem_fd, const struct intel_execution_engine2 *e)
{
	struct drm_i915_gem_relocation_entry reloc[2] = {};
	struct drm_i915_gem_exec_object2 obj[2] = {};
	struct drm_i915_gem_execbuffer2 eb = {};
	uint32_t bb_handle, obj_handle;
	unsigned long slept;
	uint32_t *obj_ptr;
	uint32_t batch[16];
	uint64_t val[2];
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

	gem_execbuf(gem_fd, &eb);
	do { /* wait for the batch to start executing */
		usleep(5e3);
	} while (!obj_ptr[1]);
	usleep(5e3); /* wait for the register sampling */

	val[0] = pmu_read_single(fd);
	slept = measured_usleep(batch_duration_ns / 1000);
	val[1] = pmu_read_single(fd);
	igt_debug("slept %.3fms, sampled %.3fms\n",
		  slept*1e-6, (val[1] - val[0])*1e-6);

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
	unsigned int slept;
	igt_spin_t *spin;
	uint64_t val[2];
	int fd[2];

	fd[0] = open_pmu(config);

	/*
	 * Second PMU client which is initialized after the first one,
	 * and exists before it, should not affect accounting as reported
	 * in the first client.
	 */
	fd[1] = open_pmu(config);

	spin = igt_spin_batch_new(gem_fd, 0, e2ring(gem_fd, e), 0);
	igt_spin_batch_set_timeout(spin, 2 * batch_duration_ns);

	slept = measured_usleep(batch_duration_ns / 1000);
	val[1] = pmu_read_single(fd[1]);
	close(fd[1]);

	gem_sync(gem_fd, spin->handle);

	val[0] = pmu_read_single(fd[0]);

	igt_spin_batch_free(gem_fd, spin);
	close(fd[0]);

	assert_within_epsilon(val[0], 2 * batch_duration_ns, tolerance);
	assert_within_epsilon(val[1], slept, tolerance);
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
	struct timespec start = { };
	igt_spin_t *spin;
	uint64_t val, ref;
	int fd;

	igt_require(cpu0_hotplug_support());

	fd = perf_i915_open(I915_PMU_ENGINE_BUSY(I915_ENGINE_CLASS_RENDER, 0));
	igt_assert(fd >= 0);

	spin = igt_spin_batch_new(gem_fd, 0, I915_EXEC_RENDER, 0);

	igt_nsec_elapsed(&start);

	/*
	 * Toggle online status of all the CPUs in a child process and ensure
	 * this has not affected busyness stats in the parent.
	 */
	igt_fork(child, 1) {
		int cpu = 0;

		for (;;) {
			char name[128];
			int cpufd;

			sprintf(name, "/sys/devices/system/cpu/cpu%d/online",
				cpu);
			cpufd = open(name, O_WRONLY);
			if (cpufd == -1) {
				igt_assert(cpu > 0);
				break;
			}
			igt_assert_eq(write(cpufd, "0", 2), 2);

			usleep(1e6);

			igt_assert_eq(write(cpufd, "1", 2), 2);

			close(cpufd);
			cpu++;
		}
	}

	igt_waitchildren();

	igt_spin_batch_end(spin);
	gem_sync(gem_fd, spin->handle);

	ref = igt_nsec_elapsed(&start);
	val = pmu_read_single(fd);

	igt_spin_batch_free(gem_fd, spin);
	close(fd);

	assert_within_epsilon(val, ref, tolerance);
}

static unsigned long calibrate_nop(int fd, const uint64_t calibration_us)
{
	const uint64_t cal_min_us = calibration_us * 3;
	const unsigned int tolerance_pct = 10;
	const uint32_t bbe = MI_BATCH_BUFFER_END;
	const unsigned int loops = 17;
	struct drm_i915_gem_exec_object2 obj = {};
	struct drm_i915_gem_execbuffer2 eb = {
		.buffer_count = 1, .buffers_ptr = to_user_pointer(&obj),
	};
	struct timespec t_begin = { };
	uint64_t size, last_size, ns;

	igt_nsec_elapsed(&t_begin);

	size = 256 * 1024;
	do {
		struct timespec t_start = { };

		obj.handle = gem_create(fd, size);
		gem_write(fd, obj.handle, size - sizeof(bbe), &bbe,
			  sizeof(bbe));
		gem_execbuf(fd, &eb);
		gem_sync(fd, obj.handle);

		igt_nsec_elapsed(&t_start);

		for (int loop = 0; loop < loops; loop++)
			gem_execbuf(fd, &eb);
		gem_sync(fd, obj.handle);

		ns = igt_nsec_elapsed(&t_start);

		gem_close(fd, obj.handle);

		last_size = size;
		size = calibration_us * 1000 * size * loops / ns;
		size = ALIGN(size, sizeof(uint32_t));
	} while (igt_nsec_elapsed(&t_begin) / 1000 < cal_min_us ||
		 abs(size - last_size) > (size * tolerance_pct / 100));

	return size;
}

static void
test_interrupts(int gem_fd)
{
	const uint32_t bbe = MI_BATCH_BUFFER_END;
	const unsigned int test_duration_ms = 1000;
	struct drm_i915_gem_exec_object2 obj = { };
	struct drm_i915_gem_execbuffer2 eb = {
		.buffers_ptr = to_user_pointer(&obj),
		.buffer_count = 1,
		.flags = I915_EXEC_FENCE_OUT,
	};
	unsigned long sz;
	igt_spin_t *spin;
	const int target = 30;
	struct pollfd pfd;
	uint64_t idle, busy;
	int fd;

	sz = calibrate_nop(gem_fd, test_duration_ms * 1000 / target);
	gem_quiescent_gpu(gem_fd);

	fd = open_pmu(I915_PMU_INTERRUPTS);
	spin = igt_spin_batch_new(gem_fd, 0, 0, 0);

	obj.handle = gem_create(gem_fd, sz);
	gem_write(gem_fd, obj.handle, sz - sizeof(bbe), &bbe, sizeof(bbe));

	pfd.events = POLLIN;
	pfd.fd = -1;
	for (int i = 0; i < target; i++) {
		int new;

		/* Merge all the fences together so we can wait on them all */
		gem_execbuf_wr(gem_fd, &eb);
		new = eb.rsvd2 >> 32;
		if (pfd.fd == -1) {
			pfd.fd = new;
		} else {
			int old = pfd.fd;
			pfd.fd = sync_fence_merge(old, new);
			close(old);
			close(new);
		}
	}

	/* Wait for idle state. */
	idle = pmu_read_single(fd);
	do {
		busy = idle;
		usleep(1e3);
		idle = pmu_read_single(fd);
	} while (idle != busy);

	/* Install the fences and enable signaling */
	igt_assert_eq(poll(&pfd, 1, 10), 0);

	/* Unplug the calibrated queue and wait for all the fences */
	igt_spin_batch_free(gem_fd, spin);
	igt_assert_eq(poll(&pfd, 1, 2 * test_duration_ms), 1);
	close(pfd.fd);

	/* Check at least as many interrupts has been generated. */
	busy = pmu_read_single(fd) - idle;
	close(fd);

	igt_assert_lte(target, busy);
}

static void
test_frequency(int gem_fd)
{
	const uint64_t duration_ns = 2e9;
	uint32_t min_freq, max_freq, boost_freq;
	uint64_t min[2], max[2], start[2];
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

	pmu_read_multi(fd, 2, start);

	spin = igt_spin_batch_new(gem_fd, 0, I915_EXEC_RENDER, 0);
	igt_spin_batch_set_timeout(spin, duration_ns);
	gem_sync(gem_fd, spin->handle);

	pmu_read_multi(fd, 2, min);
	min[0] -= start[0];
	min[1] -= start[1];

	igt_spin_batch_free(gem_fd, spin);

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

	pmu_read_multi(fd, 2, start);

	spin = igt_spin_batch_new(gem_fd, 0, I915_EXEC_RENDER, 0);
	igt_spin_batch_set_timeout(spin, duration_ns);
	gem_sync(gem_fd, spin->handle);

	pmu_read_multi(fd, 2, max);
	max[0] -= start[0];
	max[1] -= start[1];

	igt_spin_batch_free(gem_fd, spin);

	/*
	 * Restore min/max.
	 */
	igt_sysfs_set_u32(sysfs, "gt_min_freq_mhz", min_freq);
	if (igt_sysfs_get_u32(sysfs, "gt_min_freq_mhz") != min_freq)
		igt_warn("Unable to restore min frequency to saved value [%u MHz], now %u MHz\n",
			 min_freq, igt_sysfs_get_u32(sysfs, "gt_min_freq_mhz"));
	close(fd);

	igt_assert(min[0] < max[0]);
	igt_assert(min[1] < max[1]);
}

static bool wait_for_rc6(int fd)
{
	struct timespec tv = {};
	uint64_t start, now;

	start = pmu_read_single(fd);
	do {
		usleep(50);
		now = pmu_read_single(fd);
	} while (start == now && !igt_seconds_elapsed(&tv));

	return start != now;
}

static void
test_rc6(int gem_fd)
{
	int64_t duration_ns = 2e9;
	uint64_t idle, busy, prev;
	unsigned int slept;
	int fd, fw;

	fd = open_pmu(I915_PMU_RC6_RESIDENCY);

	gem_quiescent_gpu(gem_fd);
	igt_require(wait_for_rc6(fd));

	/* Go idle and check full RC6. */
	prev = pmu_read_single(fd);
	slept = measured_usleep(duration_ns / 1000);
	idle = pmu_read_single(fd);

	assert_within_epsilon(idle - prev, slept, tolerance);

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
		/**
		 * Test that a single engine metric can be initialized.
		 */
		igt_subtest_f("init-busy-%s", e->name)
			init(fd, e, I915_SAMPLE_BUSY);

		igt_subtest_f("init-wait-%s", e->name)
			init(fd, e, I915_SAMPLE_WAIT);

		igt_subtest_f("init-sema-%s", e->name)
			init(fd, e, I915_SAMPLE_SEMA);

		/**
		 * Test that engines show no load when idle.
		 */
		igt_subtest_f("idle-%s", e->name)
			single(fd, e, false);

		/**
		 * Test that a single engine reports load correctly.
		 */
		igt_subtest_f("busy-%s", e->name)
			single(fd, e, true);

		/**
		 * Test that when one engine is loaded other report no load.
		 */
		igt_subtest_f("busy-check-all-%s", e->name)
			busy_check_all(fd, e, num_engines);

		/**
		 * Test that when all except one engine are loaded all loads
		 * are correctly reported.
		 */
		igt_subtest_f("most-busy-check-all-%s", e->name)
			most_busy_check_all(fd, e, num_engines);

		/**
		 * Test that semphore counters report no activity on idle
		 * or busy engines.
		 */
		igt_subtest_f("idle-no-semaphores-%s", e->name)
			no_sema(fd, e, false);

		igt_subtest_f("busy-no-semaphores-%s", e->name)
			no_sema(fd, e, true);

		/**
		 * Test that semaphore waits are correctly reported.
		 */
		igt_subtest_f("semaphore-wait-%s", e->name)
			sema_wait(fd, e);

		/**
		 * Test that event waits are correctly reported.
		 */
		if (e->class == I915_ENGINE_CLASS_RENDER)
			igt_subtest_f("event-wait-%s", e->name)
				event_wait(fd, e);

		/**
		 * Check that two perf clients do not influence each others
		 * observations.
		 */
		igt_subtest_f("multi-client-%s", e->name)
			multi_client(fd, e);
	}

	/**
	 * Test that when all engines are loaded all loads are
	 * correctly reported.
	 */
	igt_subtest("all-busy-check-all")
		all_busy_check_all(fd, num_engines);

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

	/**
	 * Test RC6 residency reporting.
	 */
	igt_subtest("rc6")
		test_rc6(fd);

	/**
	 * Check render nodes are counted.
	 */
	igt_subtest_group {
		int render_fd;

		igt_fixture {
			render_fd = drm_open_driver_render(DRIVER_INTEL);
			igt_require_gem(render_fd);

			gem_quiescent_gpu(fd);
		}

		for_each_engine_class_instance(fd, e) {
			igt_subtest_f("render-node-busy-%s", e->name)
				single(fd, e, true);
		}

		igt_fixture {
			close(render_fd);
		}
	}
}
