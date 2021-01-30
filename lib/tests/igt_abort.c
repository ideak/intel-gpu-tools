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

#include "igt_core.h"
#include "drmtest.h"

#include "igt_tests_common.h"

char test[] = "test";
char *fake_argv[] = { test };
int fake_argc = ARRAY_SIZE(fake_argv);

__noreturn static void fake_simple_test(void)
{
	igt_simple_init(fake_argc, fake_argv);

	igt_abort_on_f(true, "I'm out!\n");

	exit(0); /* unreachable */
}

__noreturn static void fake_fixture_test(void)
{
	igt_subtest_init(fake_argc, fake_argv);

	igt_fixture {
		igt_abort_on_f(true, "I'm out!\n");
	}

	exit(0); /* unreachable */
}

__noreturn static void fake_outside_fixture_test(void)
{
	igt_subtest_init(fake_argc, fake_argv);

	igt_abort_on_f(true, "I'm out!\n");

	exit(0); /* unreachable */
}

__noreturn static void fake_subtest_test(void)
{
	igt_subtest_init(fake_argc, fake_argv);

	igt_subtest("A")
		;

	igt_subtest("B")
		igt_abort_on_f(true, "I'm out!\n");

	igt_subtest("C")
		exit(0); /* unreachable */

	exit(0); /* unreachable */
}

__noreturn static void fake_dynamic_test(void)
{
	igt_subtest_init(fake_argc, fake_argv);

	igt_subtest_with_dynamic("A") {
		igt_dynamic("AA")
			;
		igt_dynamic("AB")
			igt_abort_on_f(true, "I'm out!\n");

		igt_dynamic("AC")
			exit(0); /* unreachable */

	}

	igt_subtest("B")
		exit(0); /* unreachable */

	exit(0); /* unreachable */
}

__noreturn static void fake_outside_dynamic_test(void)
{
	igt_subtest_init(fake_argc, fake_argv);

	igt_subtest_with_dynamic("A") {
		igt_dynamic("AA")
			;

		igt_abort_on_f(true, "I'm out!\n");

		igt_dynamic("AB")
			exit(0); /* unreachable */

		igt_dynamic("AC")
			exit(0); /* unreachable */

	}

	igt_subtest("B")
		exit(0); /* unreachable */

	exit(0); /* unreachable */
}

int main(int argc, char **argv)
{
	int status;
	pid_t pid;

	/* make sure that we log the message and can abort from a simple test*/ {
		static char err[4096];
		int errfd;

		pid = do_fork_bg_with_pipes(fake_simple_test, NULL, &errfd);

		read_whole_pipe(errfd, err, sizeof(err));

		internal_assert(strstr(err, "CRITICAL: Test abort"));
		internal_assert(strstr(err, "I'm out!"));

		internal_assert(safe_wait(pid, &status) != -1);
		internal_assert_wexited(status, IGT_EXIT_ABORT);
	}

	/* make sure that we can abort from a fixture */ {
		pid = do_fork_bg_with_pipes(fake_fixture_test, NULL, NULL);
		internal_assert(safe_wait(pid, &status) != -1);
		internal_assert_wexited(status, IGT_EXIT_ABORT);
	}

	/* make sure that we can abort from outside fixture/subtest */ {
		pid = do_fork_bg_with_pipes(fake_outside_fixture_test, NULL, NULL);
		internal_assert(safe_wait(pid, &status) != -1);
		internal_assert_wexited(status, IGT_EXIT_ABORT);
	}

	/* make sure we abort during B and don't see B's end/C start */ {
		static char out[4096];
		int outfd;

		pid = do_fork_bg_with_pipes(fake_subtest_test, &outfd, NULL);

		read_whole_pipe(outfd, out, sizeof(out));

		internal_assert(safe_wait(pid, &status) != -1);
		internal_assert_wexited(status, IGT_EXIT_ABORT);

		internal_assert(strstr(out, "Starting subtest: A"));
		internal_assert(strstr(out, "Subtest A:"));

		internal_assert(strstr(out, "Starting subtest: B"));
		internal_assert(!strstr(out, "Subtest B:"));

		internal_assert(!strstr(out, "Starting subtest: C"));

		close(outfd);
	}

	/* make sure we abort during AB and don't see AC/B */ {
		static char out[4096];
		int outfd;

		pid = do_fork_bg_with_pipes(fake_dynamic_test, &outfd, NULL);

		read_whole_pipe(outfd, out, sizeof(out));

		internal_assert(safe_wait(pid, &status) != -1);
		internal_assert_wexited(status, IGT_EXIT_ABORT);

		internal_assert(strstr(out, "Starting subtest: A"));
		internal_assert(strstr(out, "Starting dynamic subtest: AA"));
		internal_assert(strstr(out, "Dynamic subtest AA:"));

		internal_assert(strstr(out, "Starting dynamic subtest: AB"));
		internal_assert(!strstr(out, "Dynamic subtest AB:"));

		internal_assert(!strstr(out, "Starting subtest: B"));

		close(outfd);

	}

	/* make sure we abort between AA and AB */ {
		static char out[4096];
		int outfd;

		pid = do_fork_bg_with_pipes(fake_outside_dynamic_test, &outfd, NULL);

		read_whole_pipe(outfd, out, sizeof(out));

		internal_assert(safe_wait(pid, &status) != -1);
		internal_assert_wexited(status, IGT_EXIT_ABORT);

		internal_assert(strstr(out, "Starting subtest: A"));
		internal_assert(strstr(out, "Starting dynamic subtest: AA"));
		internal_assert(strstr(out, "Dynamic subtest AA:"));

		internal_assert(!strstr(out, "Starting dynamic subtest: AB"));
		internal_assert(!strstr(out, "Dynamic subtest AB:"));

		internal_assert(!strstr(out, "Starting subtest: B"));

		close(outfd);

	}

	return 0;
}
