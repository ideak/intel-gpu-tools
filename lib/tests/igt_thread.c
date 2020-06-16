/*
 * Copyright © 2020 Intel Corporation
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

#include <pthread.h>

#include "drmtest.h"
#include "igt_core.h"
#include "igt_tests_common.h"

char prog[] = "igt_thread";
char *fake_argv[] = { prog };
int fake_argc = ARRAY_SIZE(fake_argv);

static void *success_thread(void *data)
{
	return NULL;
}

static void *failure_thread(void *data)
{
	igt_assert(false);
	return NULL;
}

static void one_subtest_fail(void) {
	igt_subtest_init(fake_argc, fake_argv);

	igt_subtest("subtest-a") {
		pthread_t thread;
		pthread_create(&thread, 0, failure_thread, NULL);
		pthread_join(thread, NULL);
	}

	igt_subtest("subtest-b") {
		pthread_t thread;
		pthread_create(&thread, 0, success_thread, NULL);
		pthread_join(thread, NULL);
	}

	igt_exit();
}

static void one_dynamic_fail(void) {
	igt_subtest_init(fake_argc, fake_argv);

	igt_subtest_with_dynamic("dynamic-container") {
		igt_dynamic("dynamic-a") {
			pthread_t thread;
			pthread_create(&thread, 0, failure_thread, NULL);
			pthread_join(thread, NULL);
		}

		igt_dynamic("dynamic-b") {
			pthread_t thread;
			pthread_create(&thread, 0, success_thread, NULL);
			pthread_join(thread, NULL);
		}
	}

	igt_exit();
}

static void simple_success(void) {
	pthread_t thread;

	igt_simple_init(fake_argc, fake_argv);

	pthread_create(&thread, 0, success_thread, NULL);
	pthread_join(thread, NULL);

	igt_exit();
}

static void simple_failure(void) {
	pthread_t thread;

	igt_simple_init(fake_argc, fake_argv);

	pthread_create(&thread, 0, failure_thread, NULL);
	pthread_join(thread, NULL);

	igt_exit();
}

int main(int argc, char **argv)
{
	int status;
	int outfd;
	pid_t pid;

	/* failing should be limited just to a single subtest */ {
		static char out[4096];

		pid = do_fork_bg_with_pipes(one_subtest_fail, &outfd, NULL);

		read_whole_pipe(outfd, out, sizeof(out));

		internal_assert(safe_wait(pid, &status) != -1);
		internal_assert_wexited(status, IGT_EXIT_FAILURE);

		internal_assert(matches(out, "\\[thread:.*\\] Stack trace"));
		internal_assert(strstr(out, "Subtest subtest-a: FAIL"));
		internal_assert(strstr(out, "Subtest subtest-b: SUCCESS"));

		close(outfd);
	}

	/* failing should be limited just to a dynamic subsubtest */ {
		static char out[4096];

		pid = do_fork_bg_with_pipes(one_dynamic_fail, &outfd, NULL);

		read_whole_pipe(outfd, out, sizeof(out));

		internal_assert(safe_wait(pid, &status) != -1);
		internal_assert_wexited(status, IGT_EXIT_FAILURE);

		internal_assert(matches(out, "\\[thread:.*\\] Stack trace"));
		internal_assert(strstr(out, "Dynamic subtest dynamic-a: FAIL"));
		internal_assert(strstr(out, "Dynamic subtest dynamic-b: SUCCESS"));

		close(outfd);
	}

	/* success in a simple test */ {
		static char out[4096];

		pid = do_fork_bg_with_pipes(simple_success, &outfd, NULL);

		read_whole_pipe(outfd, out, sizeof(out));

		internal_assert(safe_wait(pid, &status) != -1);
		internal_assert_wexited(status, IGT_EXIT_SUCCESS);


		internal_assert(matches(out, "^SUCCESS"));

		close(outfd);
	}

	/* success in a simple test */ {
		static char out[4096];

		pid = do_fork_bg_with_pipes(simple_success, &outfd, NULL);

		read_whole_pipe(outfd, out, sizeof(out));

		internal_assert(safe_wait(pid, &status) != -1);
		internal_assert_wexited(status, IGT_EXIT_SUCCESS);

		internal_assert(matches(out, "^SUCCESS"));

		close(outfd);
	}

	/* failure in a simple test */ {
		static char out[4096];

		pid = do_fork_bg_with_pipes(simple_failure, &outfd, NULL);

		read_whole_pipe(outfd, out, sizeof(out));

		internal_assert(safe_wait(pid, &status) != -1);
		internal_assert_wexited(status, IGT_EXIT_FAILURE);

		internal_assert(matches(out, "\\[thread:.*\\] Stack trace"));
		internal_assert(matches(out, "^FAIL"));

		close(outfd);
	}

	return 0;
}
