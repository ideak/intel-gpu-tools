/*
 * Copyright © 2019 Intel Corporation
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
#include <sys/wait.h>

#include "igt_core.h"
#include "drmtest.h"

#include "igt_tests_common.h"

static int do_fork(void (*test_to_run)(void))
{
	int pid, status;

	switch (pid = fork()) {
	case -1:
		internal_assert(0);
	case 0:
		test_to_run();
	default:
		while (waitpid(pid, &status, 0) == -1 &&
		       errno == EINTR)
			;

		return status;
	}
}

static void dynamic_subtest_in_normal_subtest(void)
{
	char prog[] = "igt_no_exit";
	char *fake_argv[] = {prog};
	int fake_argc = ARRAY_SIZE(fake_argv);

	igt_subtest_init(fake_argc, fake_argv);

	igt_subtest("normal-subtest") {
		igt_dynamic_subsubtest("dynamic") {
			igt_info("Dynamic subtest in normal subtest\n");
		}
	}

	igt_exit();
}

static void invalid_dynamic_subtest_name(void)
{
	char prog[] = "igt_no_exit";
	char *fake_argv[] = {prog};
	int fake_argc = ARRAY_SIZE(fake_argv);

	igt_subtest_init(fake_argc, fake_argv);

	igt_subtest_with_dynamic_subsubtests("subtest") {
		igt_dynamic_subsubtest("# invalid name !") {
			igt_info("Invalid dynamic subtest name test\n");
		}
	}

	igt_exit();
}

static void dynamic_subtest_in_toplevel(void)
{
	char prog[] = "igt_no_exit";
	char *fake_argv[] = {prog};
	int fake_argc = ARRAY_SIZE(fake_argv);

	igt_subtest_init(fake_argc, fake_argv);

	igt_dynamic_subsubtest("dynamic-subtest-in-toplevel") {
		igt_info("Dynamic subtests need to be in a subtest\n");
	}

	igt_exit();
}

static void subtest_itself_failing(void)
{
	char prog[] = "igt_no_exit";
	char *fake_argv[] = {prog};
	int fake_argc = ARRAY_SIZE(fake_argv);

	igt_subtest_init(fake_argc, fake_argv);

	igt_subtest_with_dynamic_subsubtests("subtest") {
		igt_assert(false);
	}

	igt_exit();
}

static void subtest_itself_skipping(void)
{
	char prog[] = "igt_no_exit";
	char *fake_argv[] = {prog};
	int fake_argc = ARRAY_SIZE(fake_argv);

	igt_subtest_init(fake_argc, fake_argv);

	igt_subtest_with_dynamic_subsubtests("subtest") {
		igt_skip("Skipping\n");
	}

	igt_exit();
}

static void dynamic_subtest_failure_leads_to_fail(void)
{
	char prog[] = "igt_no_exit";
	char *fake_argv[] = {prog};
	int fake_argc = ARRAY_SIZE(fake_argv);

	igt_subtest_init(fake_argc, fake_argv);

	igt_subtest_with_dynamic_subsubtests("subtest") {
		igt_dynamic_subsubtest("dynamic") {
			igt_assert(false);
		}
	}

	igt_exit();
}

static void no_dynamic_subtests_entered_leads_to_skip(void)
{
	char prog[] = "igt_no_exit";
	char *fake_argv[] = {prog};
	int fake_argc = ARRAY_SIZE(fake_argv);

	igt_subtest_init(fake_argc, fake_argv);

	igt_subtest_with_dynamic_subsubtests("subtest") {
	}

	igt_exit();
}

int main(int argc, char **argv)
{
	int ret;

	ret = do_fork(dynamic_subtest_in_normal_subtest);
	internal_assert_wsignaled(ret, SIGABRT);

	ret = do_fork(invalid_dynamic_subtest_name);
	internal_assert_wsignaled(ret, SIGABRT);

	ret = do_fork(dynamic_subtest_in_toplevel);
	internal_assert_wsignaled(ret, SIGABRT);

	ret = do_fork(subtest_itself_failing);
	internal_assert_wsignaled(ret, SIGABRT);

	ret = do_fork(subtest_itself_skipping);
	internal_assert_wexited(ret, IGT_EXIT_SKIP);

	ret = do_fork(dynamic_subtest_failure_leads_to_fail);
	internal_assert_wexited(ret, IGT_EXIT_FAILURE);

	ret = do_fork(no_dynamic_subtests_entered_leads_to_skip);
	internal_assert_wexited(ret, IGT_EXIT_SKIP);

	return 0;
}
