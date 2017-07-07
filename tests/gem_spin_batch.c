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

#include "igt.h"

#define MAX_ERROR 5 /* % */

#define assert_within_epsilon(x, ref, tolerance) \
	igt_assert_f(100 * x <= (100 + tolerance) * ref && \
		     100 * x >= (100 - tolerance) * ref, \
		     "'%s' != '%s' (%lld not within %d%% tolerance of %lld)\n",\
		     #x, #ref, (long long)x, tolerance, (long long)ref)

static void spin(int fd, unsigned int engine, unsigned int timeout_sec)
{
	const uint64_t timeout_100ms = 100000000LL;
	unsigned long loops = 0;
	igt_spin_t *spin;
	struct timespec tv = { };
	struct timespec itv = { };
	uint64_t elapsed;

	spin = igt_spin_batch_new(fd, engine, 0);
	while ((elapsed = igt_nsec_elapsed(&tv)) >> 30 < timeout_sec) {
		igt_spin_t *next = __igt_spin_batch_new(fd, engine, 0);

		igt_spin_batch_set_timeout(spin,
					   timeout_100ms - igt_nsec_elapsed(&itv));
		gem_sync(fd, spin->handle);
		igt_debug("loop %d: interval=%fms (target 100ms), elapsed %fms\n",
			  loops,
			  igt_nsec_elapsed(&itv) * 1e-6,
			  igt_nsec_elapsed(&tv) * 1e-6);
		memset(&itv, 0, sizeof(itv));

		igt_spin_batch_free(fd, spin);
		spin = next;
		loops++;
	}
	igt_spin_batch_free(fd, spin);

	igt_info("Completed %ld loops in %lld ns, target %ld\n",
		 loops, (long long)elapsed, (long)(elapsed / timeout_100ms));

	assert_within_epsilon(timeout_100ms * loops, elapsed, MAX_ERROR);
	igt_assert_eq(intel_detect_and_clear_missed_interrupts(fd), 0);
}

static void spin_exit_handler(int sig)
{
	igt_terminate_spin_batches();
}

static void spin_on_all_engines(int fd, unsigned int timeout_sec)
{
	unsigned engine;

	for_each_engine(fd, engine) {
		if (engine == 0)
			continue;

		igt_fork(child, 1) {
			igt_install_exit_handler(spin_exit_handler);
			spin(fd, engine, timeout_sec);
		}
	}

	igt_waitchildren();
}

igt_main
{
	const struct intel_execution_engine *e;
	int fd = -1;

	igt_skip_on_simulation();

	igt_fixture {
		fd = drm_open_driver(DRIVER_INTEL);
		igt_require_gem(fd);
		igt_fork_hang_detector(fd);
		intel_detect_and_clear_missed_interrupts(fd);
	}

	for (e = intel_execution_engines; e->name; e++) {
		if (e->exec_id == 0)
			continue;

		igt_subtest_f("basic-%s", e->name)
			spin(fd, e->exec_id, 3);
	}

	igt_subtest("spin-each")
		spin_on_all_engines(fd, 3);

	igt_fixture {
		igt_stop_hang_detector();
		close(fd);
	}
}
