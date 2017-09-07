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
 */
#include "config.h"
#include "igt.h"
#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

/**
 * Parse the r-value of a [cmd] string.
 */
static bool check_cmd_return_value(const char *s, void *data)
{
	int *val = data;
	char *cmd, *found;
	const char *delim = "[cmd]";
	const int delim_len = strlen(delim);

	if (!(cmd = strstr(s, delim)))
		return false;

	found = cmd + delim_len + 1;
	igt_assert(delim_len + strlen(found) < strlen(cmd));

	*val = atoi(found);
	return true;
}

igt_main
{
	igt_skip_on_simulation();

	igt_subtest("sysfs_l3_parity") {
		int exec_return;

		igt_system_cmd(exec_return,
			       "../tools/intel_l3_parity -r 0 -b 0 "
			       "-s 0 -e");
		igt_assert(exec_return == IGT_EXIT_SUCCESS);

		igt_system_cmd(exec_return,
			       "../tools/intel_l3_parity -l | "
			       "grep -c 'Row 0, Bank 0, Subbank 0 "
			       "is disabled'");
		if (exec_return == IGT_EXIT_SUCCESS) {
			int val = -1;
			igt_log_buffer_inspect(check_cmd_return_value,
					       &val);
			igt_assert(val == 1);
		} else {
			igt_fail(IGT_EXIT_FAILURE);
		}

		igt_system_cmd(exec_return,
			       "../tools/intel_l3_parity -r 0 -b 0 "
			       "-s 0 -e");
		igt_assert(exec_return == IGT_EXIT_SUCCESS);

		/* Check that we can clear remaps */
		igt_system_cmd(exec_return,
			       "../tools/intel_l3_parity -l | "
			       "wc -l");
		if (exec_return == IGT_EXIT_SUCCESS) {
			int val = -1;
			igt_log_buffer_inspect(check_cmd_return_value,
					       &val);
			igt_assert(val == 1);
		} else {
			igt_fail(IGT_EXIT_FAILURE);
		}
	}

	igt_subtest("tools_test") {
		char *cmd;

		igt_assert(asprintf(&cmd,
				    "../tools/intel_reg read 0x4030")
			   != -1);
		igt_assert(igt_system_quiet(cmd) == IGT_EXIT_SUCCESS);
		free(cmd);

		igt_assert(asprintf(&cmd, "../tools/intel_reg dump")
			   != -1);
		igt_assert(igt_system_quiet(cmd) == IGT_EXIT_SUCCESS);
		free(cmd);
	}
}
