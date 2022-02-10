/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2022 Intel Corporation
 */

#ifndef IGT_TYPES_H
#define IGT_TYPES_H

/*
 * GCC can automatically cleanup variables that go out of scope, but only
 * through normal means. Breaking out of scope using longjmp (i.e. igt_skip)
 * is not handled automatically by GCC. Such scoped variables must be tracked
 * in an outer scope to the skipping subtest.
 *
 * BAD:
 * 	igt_subtest("bad") {
 * 		igt_fd_t(fd);
 *
 * 		fd = drm_open_driver();
 * 	}
 *
 * GOOD:
 * 	igt_subtest_group() {
 * 		igt_fd_t(fd);
 *
 * 		igt_fixture {
 * 			fd = drm_open_driver();
 * 		}
 *
 * 		igt_subtest("good")
 * 			;
 * 	}
 *
 * A rule of thumb is that anything that is initialised through a fixture can
 * be combined with automatic cleanup.
 */

#define cleanup_with(fn) __attribute__((__cleanup__(fn)))

/* Prevent use within the inner scope subtests, it will be broken by igt_skip */
#define IGT_OUTER_SCOPE_INIT(x) ({ __igt_assert_in_outer_scope(); x; })

void igt_cleanup_fd(volatile int *fd);
#define igt_fd_t(x__) \
	volatile int x__ cleanup_with(igt_cleanup_fd) = IGT_OUTER_SCOPE_INIT(-1)

#endif /* IGT_TYPES_H */
