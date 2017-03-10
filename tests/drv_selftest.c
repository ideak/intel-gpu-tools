/*
 * Copyright © 2016 Intel Corporation
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

#include "igt.h"
#include "igt_kmod.h"

IGT_TEST_DESCRIPTION("Basic unit tests for i915.ko");

igt_main
{
	/*
	 * Set of subtest names that are always exposed by IGT,
	 * regardless of the running kernel's capabilities. Selftests
	 * that the kernel has but are not on these lists are also
	 * exposed. This is a known intentional violation of the
	 * general rule that subtest enumeration must not change
	 * depending on the runtime environment.
	 */
	struct igt_kselftest_mockentry i915_mock_testlist[] = {
#define selftest(n, x) { .name = "mock_" #n, .do_mock = true },
#include "i915_mock_selftests.h"
#undef selftest
		{ NULL, false }
	};
	struct igt_kselftest_mockentry i915_live_testlist[] = {
#define selftest(n, x) { .name = "live_" #n, .do_mock = true },
#include "i915_live_selftests.h"
#undef selftest
		{ NULL, false }
	};

	igt_kselftests("i915", "mock_selftests=-1", NULL, "mock", i915_mock_testlist);
	igt_kselftests("i915", "live_selftests=-1", "live_selftests", "live", i915_live_testlist);
}
