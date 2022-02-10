// SPDX-License-Identifier: MIT
/*
* Copyright © 2022 Intel Corporation
*/

#include "igt_core.h"
#include "igt_types.h"

IGT_TEST_DESCRIPTION("Test bad-scoped file descriptor variable");

igt_main
{
	igt_describe("Check if using a scoped variable inside a subtest will abort it");
	igt_subtest("bad-scoped-variable") {
		/*
		 * Not allowed to nest a scoped variable inside a subtest as
		 * we expect to longjmp out of the subtest on failure/skip
		 * and automatic cleanup is not invoked for such jmps.
		 * So, this test is expected to fail with SIGABRT.
		 */
		igt_fd_t(f);
	}
}
