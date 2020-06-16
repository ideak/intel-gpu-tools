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
 */

#include <pthread.h>
#include <stdlib.h>
#include <stdatomic.h>

#include "igt_core.h"
#include "igt_thread.h"

static pthread_key_t __igt_is_main_thread;

static _Atomic(bool) __thread_failed = false;

void igt_thread_clear_fail_state(void)
{
	assert(igt_thread_is_main());

	__thread_failed = false;
}

void igt_thread_fail(void)
{
	assert(!igt_thread_is_main());

	__thread_failed = true;
}

void igt_thread_assert_no_failures(void)
{
	assert(igt_thread_is_main());

	if (__thread_failed) {
		/* so we won't get stuck in a loop */
		igt_thread_clear_fail_state();
		igt_critical("Failure in a thread!\n");
		igt_fail(IGT_EXIT_FAILURE);
	}
}

bool igt_thread_is_main(void)
{
	return pthread_getspecific(__igt_is_main_thread) != NULL;
}

igt_constructor {
	pthread_key_create(&__igt_is_main_thread, NULL);
	pthread_setspecific(__igt_is_main_thread, (void*) 0x1);
}
