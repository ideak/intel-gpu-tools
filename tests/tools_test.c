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

struct line_check {
	bool found;
	const char *substr;
};

/**
 * Our igt_log_buffer_inspect handler. Checks the output of the
 * intel_l3_parity tool and returns line_check::found to true if
 * a specific substring is found.
 */
static bool check_cmd_return_value(const char *line, void *data)
{
	struct line_check *check = data;

	if (!strstr(line, check->substr)) {
		check->found = false;
		return false;
	}

	check->found = true;
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
		igt_skip_on_f(exec_return == IGT_EXIT_SKIP,
			      "intel_l3_parity not supported\n");
		igt_assert_eq(exec_return, IGT_EXIT_SUCCESS);

		igt_system_cmd(exec_return, "../tools/intel_l3_parity -l");
		if (exec_return == IGT_EXIT_SUCCESS) {
			struct line_check line;
			line.substr = "Row 0, Bank 0, Subbank 0 is disabled";
			igt_log_buffer_inspect(check_cmd_return_value,
					       &line);
			igt_assert_eq(line.found, true);
		}

		igt_system_cmd(exec_return,
			       "../tools/intel_l3_parity -r 0 -b 0 "
			       "-s 0 -e");
		igt_skip_on_f(exec_return == IGT_EXIT_SKIP,
			      "intel_l3_parity not supported\n");
		igt_assert_eq(exec_return, IGT_EXIT_SUCCESS);

		/* Check that we can clear remaps:
		 * In the original shell script, the output of intel_l3_parity -l
		 * was piped thru wc -l to check if the tool would at least
		 * return a line. Just watch for one of the expected output
		 * string as an alternative.
		 * ("is disabled" unique only to intel_l3_parity.c:dumpit())
		 */
		igt_system_cmd(exec_return,
			       "../tools/intel_l3_parity -l");
		if (exec_return == IGT_EXIT_SUCCESS) {
			struct line_check line;
			line.substr = "is disabled";
			igt_log_buffer_inspect(check_cmd_return_value,
					       &line);
			igt_assert_eq(line.found, true);
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
