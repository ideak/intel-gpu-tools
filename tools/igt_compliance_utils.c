/*
 * Copyright (c) 2020, The Linux Foundation. All rights reserved.
 * Copyright 2017 Intel Corporation
 *   Jesse Barnes <jesse.barnes@intel.com>
 *   Manasi Navare <manasi.d.navare@intel.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include "igt.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <signal.h>
#include <termios.h>

#include <sys/stat.h>

#include "igt_dp_compliance.h"

static int tio_fd;
struct termios saved_tio;

void enter_exec_path(char **argv)
{
	char *exec_path = NULL;
	char *pos = NULL;
	short len_path = 0;
	int ret;

	len_path = strlen(argv[0]);
	exec_path = (char *) malloc(len_path);

	memcpy(exec_path, argv[0], len_path);
	pos = strrchr(exec_path, '/');
	if (pos != NULL)
		*(pos+1) = '\0';

	ret = chdir(exec_path);
	igt_assert_eq(ret, 0);
	free(exec_path);
}

static void restore_termio_mode(int sig)
{
	tcsetattr(tio_fd, TCSANOW, &saved_tio);
	close(tio_fd);
}

void set_termio_mode(void)
{
	struct termios tio;

	/* don't attempt to set terminal attributes if not in the foreground
	 * process group
	 */
	if (getpgrp() != tcgetpgrp(STDOUT_FILENO))
		return;

	tio_fd = dup(STDIN_FILENO);
	tcgetattr(tio_fd, &saved_tio);
	igt_install_exit_handler(restore_termio_mode);
	tio = saved_tio;
	tio.c_lflag &= ~(ICANON | ECHO);
	tcsetattr(tio_fd, TCSANOW, &tio);
}
