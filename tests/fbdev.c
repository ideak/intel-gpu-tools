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

#include "config.h"

#include "igt.h"

#include <fcntl.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <linux/fb.h>

#include "igt.h"

igt_main
{
	struct fb_var_screeninfo var_info;
	struct fb_fix_screeninfo fix_info;
	int fd = -1;

	igt_fixture {
		fd = open("/dev/fb0", O_RDWR);
		igt_require_f(fd != -1, "/dev/fb0\n");

		igt_require(ioctl(fd, FBIOGET_VSCREENINFO, &var_info) == 0);
		igt_require(ioctl(fd, FBIOGET_FSCREENINFO, &fix_info) == 0);
	}

	igt_subtest("info") {
		unsigned long size;

		size = var_info.yres * fix_info.line_length;
		igt_assert_f(size <= fix_info.smem_len,
			     "screen size (%d x %d) of pitch %d does not fit within mappable area of size %u\n",
			     var_info.xres, var_info.yres,
			     fix_info.line_length,
			     fix_info.smem_len);
	}

	igt_subtest("mmap") {
		void *map;

		igt_require(fix_info.smem_len);

		map = mmap(NULL, fix_info.smem_len,
			   PROT_WRITE, MAP_SHARED, fd, 0);
		igt_assert(map != MAP_FAILED);

		memset(map, 0, fix_info.smem_len);
		munmap(map, fix_info.smem_len);
	}

	igt_fixture {
		close(fd);
	}
}
