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
 *
 */

#ifndef IGT_LIB_TESTS_COMMON_H
#define IGT_LIB_TESTS_COMMON_H

#include <assert.h>
#include <sys/wait.h>
#include <errno.h>
#include <sys/stat.h>
#include <fcntl.h>

/*
 * We need to hide assert from the cocci igt test refactor spatch.
 *
 * IMPORTANT: Test infrastructure tests are the only valid places where using
 * assert is allowed.
 */
#define internal_assert assert

static inline void internal_assert_wexited(int wstatus, int exitcode)
{
	internal_assert(WIFEXITED(wstatus) &&
			WEXITSTATUS(wstatus) == exitcode);
}

static inline void internal_assert_wsignaled(int wstatus, int signal)
{
	internal_assert(WIFSIGNALED(wstatus) &&
			WTERMSIG(wstatus) == signal);
}

static inline void internal_assert_not_wsignaled(int wstatus)
{
	internal_assert(!WIFSIGNALED(wstatus));
}

static inline int do_fork(void (*test_to_run)(void))
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

static inline pid_t do_fork_bg_with_pipes(void (*test_to_run)(void), int *out, int *err)
{
	int outfd[2], errfd[2];
	pid_t pid;

	if (out != NULL)
		internal_assert(pipe(outfd) != -1);

	if (err != NULL)
		internal_assert(pipe(errfd) != -1);

	pid = fork();
	internal_assert(pid != -1);

	if (pid == 0) {
		/* we'll leak the /dev/null fds, let them die with the forked
		 * process, also close reading ends if they are any */
		if (out == NULL)
			outfd[1] = open("/dev/null", O_WRONLY);
		else
			close(outfd[0]);

		if (err == NULL)
			errfd[1] = open("/dev/null", O_WRONLY);
		else
			close(errfd[0]);

		while (dup2(outfd[1], STDOUT_FILENO) == -1 && errno == EINTR) {}
		while (dup2(errfd[1], STDERR_FILENO) == -1 && errno == EINTR) {}

		close(outfd[1]);
		close(errfd[1]);

		test_to_run();

		exit(-1);
	} else {
		/* close the writing ends */
		if (out != NULL) {
			close(outfd[1]);
			*out = outfd[0];
		}

		if (err != NULL) {
			close(errfd[1]);
			*err = errfd[0];
		}

		return pid;
	}
}

static inline int safe_wait(pid_t pid, int *status) {
	int ret;

	do {
		ret = waitpid(pid, status, 0);
	} while (ret == -1 && errno == EINTR);

	return ret;
}

static inline void assert_pipe_empty(int fd)
{
	char buf[5];
	internal_assert(0 == read(fd, buf, sizeof(buf)));
}

static inline void read_whole_pipe(int fd, char *buf, size_t buflen)
{
	ssize_t readlen;
	off_t offset;

	offset = 0;
	while ((readlen = read(fd, buf+offset, buflen-offset))) {
		if (readlen == -1) {
			if (errno == EINTR) {
				continue;
			} else {
				printf("read failed with %s\n", strerror(errno));
				exit(1);
			}
		}
		internal_assert(readlen != -1);
		offset += readlen;
	}
}
#endif
