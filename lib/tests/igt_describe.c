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

#include <sys/wait.h>
#include <string.h>
#include <stdlib.h>

#include "drmtest.h"
#include "igt_tests_common.h"

char prog[] = "igt_describe";
char fake_arg[100];
char *fake_argv[] = {prog, fake_arg};
int fake_argc = ARRAY_SIZE(fake_argv);

IGT_TEST_DESCRIPTION("the top level description");
__noreturn static void fake_main(void)
{
	igt_subtest_init(fake_argc, fake_argv);

	igt_describe("Basic A");
	igt_subtest("A")
		;

	igt_fixture
		printf("should not be executed!\n");

	igt_describe("Group with B, C & D");
	igt_subtest_group {
		igt_describe("Basic B");
		igt_subtest("B")
			;

		if (!igt_only_list_subtests())
			printf("should not be executed!\n");

		igt_describe("Group with C & D");
		igt_subtest_group {
			igt_describe("Basic C");
			igt_subtest("C")
				printf("should not be executed!\n");

			// NO DOC
			igt_subtest("D")
				;
		}
	}

	// NO DOC
	igt_subtest_group {
		// NO DOC
		igt_subtest("E")
			;
	}

	// NO DOC
	igt_subtest("F")
		;

	igt_describe("this description should be so long that it wraps itself nicely in the terminal "
		     "this description should be so long that it wraps itself nicely in the terminal "
		     "this description should be so long that it wraps itself nicely in the terminal "
		     "this description should be so long that it wraps itself nicely in the terminal "
		     "this description should be so long that it wraps itself nicely in the terminal "
		     "this description should be so long that it wraps itself nicely in the terminal");
	igt_subtest("G")
		;

	igt_describe("verylongwordthatshoudlbeprintedeventhoughitspastthewrppinglimit"
		     "verylongwordthatshoudlbeprintedeventhoughitspastthewrappinglimit "
		     "verylongwordthatshoudlbeprintedeventhoughitspastthewrappinglimit"
		     "verylongwordthatshoudlbeprintedeventhoughitspastthewrappinglimit");
	igt_subtest("F")
		;

	igt_describe("Subtest with dynamic subsubtests");
	igt_subtest_with_dynamic("G") {
		printf("should not be executed!\n");
		igt_describe("should assert on execution");
		igt_dynamic("should-not-list")
			printf("should not be executed!\n");
	}

	igt_exit();
}

static const char DESCRIBE_ALL_OUTPUT[] = \
	"the top level description\n"
	"\n"
	"SUB A " __FILE__ ":42:\n"
	"  Basic A\n"
	"\n"
	"SUB B " __FILE__ ":51:\n"
	"  Group with B, C & D\n"
	"\n"
	"  Basic B\n"
	"\n"
	"SUB C " __FILE__ ":60:\n"
	"  Group with B, C & D\n"
	"\n"
	"  Group with C & D\n"
	"\n"
	"  Basic C\n"
	"\n"
	"SUB D " __FILE__ ":64:\n"
	"  Group with B, C & D\n"
	"\n"
	"  Group with C & D\n"
	"\n"
	"SUB E " __FILE__ ":72:\n"
	"  NO DOCUMENTATION!\n"
	"\n"
	"SUB F " __FILE__ ":77:\n"
	"  NO DOCUMENTATION!\n"
	"\n"
	"SUB G " __FILE__ ":86:\n"
	"  this description should be so long that it wraps itself nicely in the terminal this\n"
	"  description should be so long that it wraps itself nicely in the terminal this description\n"
	"  should be so long that it wraps itself nicely in the terminal this description should be so\n"
	"  long that it wraps itself nicely in the terminal this description should be so long that it\n"
	"  wraps itself nicely in the terminal this description should be so long that it wraps itself\n"
	"  nicely in the terminal\n"
	"\n"
	"SUB F " __FILE__ ":93:\n"
	"  verylongwordthatshoudlbeprintedeventhoughitspastthewrppinglimitverylongwordthatshoudlbeprintedeventhoughitspastthewrappinglimit\n"
	"  verylongwordthatshoudlbeprintedeventhoughitspastthewrappinglimitverylongwordthatshoudlbeprintedeventhoughitspastthewrappinglimit\n"
	"\n"
	"SUB G " __FILE__ ":97:\n"
	"  Subtest with dynamic subsubtests\n\n";

static const char JUST_C_OUTPUT[] = \
	"the top level description\n"
	"\n"
	"SUB C " __FILE__ ":60:\n"
	"  Group with B, C & D\n"
	"\n"
	"  Group with C & D\n"
	"\n"
	"  Basic C\n"
	"\n";

int main(int argc, char **argv)
{
	int status;
	int outfd, errfd;
	pid_t pid;

	/* describe all subtest */ {
		static char out[4096];
		strncpy(fake_arg, "--describe", sizeof(fake_arg));

		pid = do_fork_bg_with_pipes(fake_main, &outfd, &errfd);

		read_whole_pipe(outfd, out, sizeof(out));
		assert_pipe_empty(errfd);

		internal_assert(safe_wait(pid, &status) != -1);
		internal_assert(WIFEXITED(status));
		internal_assert(WEXITSTATUS(status) == IGT_EXIT_SUCCESS);
		internal_assert(0 == strcmp(DESCRIBE_ALL_OUTPUT, out));

		close(outfd);
		close(errfd);
	}

	/* describe C using a pattern */ {
		static char out[4096];
		strncpy(fake_arg, "--describe=C", sizeof(fake_arg));

		pid = do_fork_bg_with_pipes(fake_main, &outfd, &errfd);

		read_whole_pipe(outfd, out, sizeof(out));
		assert_pipe_empty(errfd);

		internal_assert(safe_wait(pid, &status) != -1);
		internal_assert(WIFEXITED(status));
		internal_assert(WEXITSTATUS(status) == IGT_EXIT_SUCCESS);
		internal_assert(0 == strcmp(JUST_C_OUTPUT, out));

		close(outfd);
		close(errfd);
	}

	/* fail describing with a bad pattern */ {
		static char err[4096];
		strncpy(fake_arg, "--describe=Z", sizeof(fake_arg));

		pid = do_fork_bg_with_pipes(fake_main, &outfd, &errfd);

		read_whole_pipe(errfd, err, sizeof(err));

		internal_assert(safe_wait(pid, &status) != -1);
		internal_assert(WIFEXITED(status));
		internal_assert(WEXITSTATUS(status) == IGT_EXIT_INVALID);
		internal_assert(strstr(err, "Unknown subtest: Z"));

		close(outfd);
		close(errfd);
	}

	/* trying to igt_describe a dynamic subsubtest should assert */ {
		static char err[4096];
		strncpy(fake_arg, "--run-subtest=G", sizeof(fake_arg));

		pid = do_fork_bg_with_pipes(fake_main, &outfd, &errfd);

		read_whole_pipe(errfd, err, sizeof(err));

		internal_assert(safe_wait(pid, &status) != -1);
		internal_assert_wsignaled(status, SIGABRT);

		close(outfd);
		close(errfd);
	}

	return 0;
}
