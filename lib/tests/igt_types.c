// SPDX-License-Identifier: MIT
/*
* Copyright © 2022 Intel Corporation
*/

#include "igt_core.h"
#include "igt_types.h"

IGT_TEST_DESCRIPTION("Test scoped variable handling");

/* a lookalike of igt_fd_t for testing */
#define scoped_int_t(x__) \
	volatile int x__ cleanup_with(cleanup) = IGT_OUTER_SCOPE_INIT(-1)

static int cleanup_called;

static void cleanup(volatile int *x)
{
	cleanup_called++;
	*x = -1;
}

static void delegate(void)
{
	scoped_int_t(x);

	igt_fixture
		x = 1;

	igt_describe("Pretend to be doing a subtest");
	igt_subtest("empty-subtest")
		x = 2;

	igt_fixture {
		/* Check that we went through both blocks without cleanup */
		igt_assert(!cleanup_called);
		igt_assert(x == 2);
	}
}

static void skip_delegate(void)
{
	scoped_int_t(x);

	igt_fixture
		x = 1;

	igt_describe("Check if skipping a test will not update a scoped variable");

	igt_subtest("skipped-subtest") {
		igt_skip("Early skip for testing\n");
		x = 2; /* not reached due to lonjmp from igt_skip */
	}

	igt_fixture {
		/* Check that we went through both blocks without cleanup */
		igt_assert(!cleanup_called);
		igt_assert(x == 1);
	}
}

igt_main
{
	/* Basic check that scopes will call their destructor */
	cleanup_called = 0;
	igt_fixture {
		scoped_int_t(x);
	}

	igt_describe("Check if cleanup is called after fixture");
	igt_subtest("cleanup-after-fixture")
		igt_assert(cleanup_called);

	/* But not before we go out of scope! */
	cleanup_called = 0;
	igt_subtest_group {
		scoped_int_t(x);

		igt_fixture {
			x = 0xdeadbeef;
		}

		igt_describe("Check if cleanup not called before subtest group");
		igt_subtest("cleanup-not-before-subtest-group") {
			/* Check no scope destructor was called */
			igt_assert(cleanup_called == 0);
			/* Confirm that we did pass through a scoped block */
			igt_assert_eq_u32(x, 0xdeadbeef);
		}
	}
	igt_describe("Check if cleanup is called after subtest group");
	igt_subtest("cleanup-after-subtest-group")
		igt_assert(cleanup_called);

	/* longjmp and __attribute__(cleanup) do not mix well together */
#if 0 /* See bad_subtest_type, this is caught by an internal assertion */
	cleanup_called = 0;
	igt_describe("Check skipping a subtest");
	igt_subtest("skip-subtest") {
		scoped_int_t(x);

		igt_skip("Checking scoped cleanup on skip\n");
	}
	igt_describe("Check cleanup after skipping a subtest");
	igt_subtest("cleanup-after-skip")
		igt_assert_f(!cleanup_called,
				"scoped closure was not compatible with igt_skip\n");
#endif

	/*
	 * However, if we igt_skip inside another block (subtest-group), then we
	 * will get cleanup on the outer scope.
	 */
	cleanup_called = 0;
	igt_subtest_group {
		scoped_int_t(x);

		igt_describe("Check skipping a subtest group");
		igt_subtest("skip-subtest-group")
			igt_skip("Checking scoped cleanup after skip\n");
	}
	igt_describe("Check cleanup after skipping a subtest group");
	igt_subtest("cleanup-after-skip-group")
		igt_assert(cleanup_called);

	/* Check the same holds true for function calls */
	cleanup_called = 0;
	delegate();
	igt_describe("Check cleanup after delegation");
	igt_subtest("cleanup-after-delegation")
		igt_assert(cleanup_called);

	cleanup_called = 0;
	igt_subtest_group
		delegate();
	igt_describe("Check cleanup after group delegation");
	igt_subtest("cleanup-after-group-delegation")
		igt_assert(cleanup_called);

	/* Check what happens with a igt_skip inside a function */
	cleanup_called = 0;
	skip_delegate();
	igt_describe("Check cleanup after skipping delegation");
	igt_subtest("cleanup-after-skipped-delegation")
		igt_assert(cleanup_called);

	cleanup_called = 0;
	igt_subtest_group
		skip_delegate();
	igt_describe("Check cleanup after skipping group delegation");
	igt_subtest("cleanup-after-group-skipped-delegation")
		igt_assert(cleanup_called);
}
