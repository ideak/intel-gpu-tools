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
#include "igt_tests_common.h"
#include "drmtest.h"

char test[] = "test";
char *fake_argv[] = { test };
int fake_argc = ARRAY_SIZE(fake_argv);

static void all_valid_simple_test(void)
{
	igt_simple_init(fake_argc, fake_argv);

	igt_skip("o:");
	igt_assert(false);

	igt_exit();
}

static void all_valid(void)
{
	igt_subtest_init(fake_argc, fake_argv);

	igt_fixture {
	}

	igt_subtest_group {
		igt_subtest_group {
		}

		igt_fixture {
		}

		igt_subtest("a")
		{
			igt_skip("o:\n");
		}

		igt_subtest("b")
		{
			igt_assert(false);
		}

		igt_subtest_with_dynamic("c") {
			igt_dynamic("d")
			{
				igt_skip("o:\n");
			}

			igt_dynamic("e")
			{
				igt_assert(false);
			}
		}

		igt_subtest_with_dynamic("f") {
			igt_skip("o:\n");
		}

		igt_subtest_with_dynamic("g") {
		}
	}

	igt_exit();
}

static void invalid_subtest_in_simple_test(void)
{
	igt_simple_init(fake_argc, fake_argv);

	igt_subtest("a") {
	}

	igt_exit();
}

static void invalid_subtest_group_in_simple_test(void)
{
	igt_simple_init(fake_argc, fake_argv);

	igt_subtest_group {
	}

	igt_exit();
}

static void invalid_subtest_with_dynamic_in_simple_test(void)
{
	igt_simple_init(fake_argc, fake_argv);

	igt_subtest_with_dynamic("a") {
	}

	igt_exit();
}

static void invalid_dynamic_in_simple_test(void)
{
	igt_simple_init(fake_argc, fake_argv);

	igt_dynamic("a") {
	}

	igt_exit();
}

static void invalid_fixture_in_fixture(void)
{
	igt_subtest_init(fake_argc, fake_argv);

	igt_fixture {
		igt_fixture {
		}
	}

	igt_exit();
}

static void invalid_subtest_in_subtest(void)
{
	igt_subtest_init(fake_argc, fake_argv);

	igt_subtest("a") {
		igt_subtest("b") {
		}
	}

	igt_exit();
}

static void invalid_top_level_dynamic(void)
{
	igt_subtest_init(fake_argc, fake_argv);

	igt_dynamic("a") {
	}

	igt_exit();
}

static void invalid_dynamic_in_regular_subtest(void)
{
	igt_subtest_init(fake_argc, fake_argv);

	igt_subtest("a") {
		igt_dynamic("b") {
		}
	}

	igt_exit();
}

static void invalid_fixture_in_subtest(void)
{
	igt_subtest_init(fake_argc, fake_argv);

	igt_subtest("a") {
		igt_fixture {
		}
	}

	igt_exit();
}

static void invalid_top_level_skip(void)
{
	igt_subtest_init(fake_argc, fake_argv);

	igt_skip("o:\n");

	igt_exit();
}

static void invalid_top_level_assert(void)
{
	igt_subtest_init(fake_argc, fake_argv);

	igt_assert(false);

	igt_exit();
}

static void invalid_dynamic_in_dynamic(void)
{
	igt_subtest_init(fake_argc, fake_argv);

	igt_subtest_with_dynamic("a") {
		igt_dynamic("b") {
			igt_dynamic("c") {
			}
		}
	}

	igt_exit();
}

typedef void (*fork_fun)(void);

int main(int argc, char **argv)
{
	int status;

	/* test the invalid nesting scenarios */ {
		fork_fun should_sigabort[] = {
			invalid_subtest_in_simple_test,
			invalid_subtest_group_in_simple_test,
			invalid_subtest_with_dynamic_in_simple_test,
			invalid_dynamic_in_simple_test,
			invalid_fixture_in_fixture,
			invalid_subtest_in_subtest,
			invalid_top_level_dynamic,
			invalid_dynamic_in_regular_subtest,
			invalid_fixture_in_subtest,
			invalid_top_level_skip,
			invalid_top_level_assert,
			invalid_dynamic_in_dynamic,
		};

		for (int i = 0; i < ARRAY_SIZE(should_sigabort); ++i) {
			status = do_fork(should_sigabort[i]);
			internal_assert_wsignaled(status, SIGABRT);
		}
	}

	/* test the valid nesting scenarios */ {
		fork_fun should_not_signal[] = {
			all_valid_simple_test,
			all_valid,
		};

		for (int i = 0; i < ARRAY_SIZE(should_not_signal); ++i) {
			status = do_fork(should_not_signal[i]);
			internal_assert_not_wsignaled(status);
		}
	}

	return 0;
}
