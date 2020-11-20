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

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <linux/fb.h>

#include "igt.h"

static void mode_tests(int fd)
{
	struct fb_var_screeninfo var_info;
	struct fb_fix_screeninfo fix_info;

	igt_fixture {
		igt_require(ioctl(fd, FBIOGET_VSCREENINFO, &var_info) == 0);
		igt_require(ioctl(fd, FBIOGET_FSCREENINFO, &fix_info) == 0);
	}

	igt_describe("Check if screeninfo is valid");
	igt_subtest("info") {
		unsigned long size;

		size = var_info.yres * fix_info.line_length;
		igt_assert_f(size <= fix_info.smem_len,
			     "screen size (%d x %d) of pitch %d does not fit within mappable area of size %u\n",
			     var_info.xres, var_info.yres,
			     fix_info.line_length,
			     fix_info.smem_len);
	}
}

static void framebuffer_tests(int fd)
{
	const int values[] = { 0, 0x55, 0xaa, 0xff };
	struct fb_fix_screeninfo fix_info;
	unsigned char * volatile map;
	unsigned char * volatile buf;
	volatile size_t pagesize;

	igt_fixture {
		long ret;

		igt_require(ioctl(fd, FBIOGET_FSCREENINFO, &fix_info) == 0);
		igt_assert(fix_info.smem_len);

		map = mmap(NULL, fix_info.smem_len,
			   PROT_WRITE, MAP_SHARED, fd, 0);
		igt_assert(map != MAP_FAILED);

		buf = malloc(fix_info.smem_len);
		igt_require(buf);

		ret = sysconf(_SC_PAGESIZE);
		igt_require(ret != -1);
		pagesize = ret;
	}

	igt_describe("Check read operations on framebuffer memory");
	igt_subtest("read") {
		ssize_t ret;

		/* fill framebuffer and compare */
		for (int i = 0; i < ARRAY_SIZE(values); i++) {
			memset(map, values[i], fix_info.smem_len);
			ret = pread(fd, buf, fix_info.smem_len, 0);
			igt_assert_f(ret == (ssize_t)fix_info.smem_len,
				     "pread failed, ret=%zd\n", ret);
			igt_assert_f(!memcmp(map, buf, fix_info.smem_len),
				     "read differs from mapped framebuffer for %x\n",
				     values[i]);
		}
	}

	igt_describe("Check read operations on unaligned locations in framebuffer memory");
	igt_subtest("unaligned-read") {
		const unsigned char *pos;
		ssize_t ret;
		size_t len;
		off_t off;

		off = pagesize + (pagesize >> 2); /* 1.25 * pagesize */
		len = (pagesize << 2) + (pagesize >> 1); /* 4.5 * pagesize */
		igt_require_f(off + len < fix_info.smem_len,
			      "framebuffer too small to test\n");

		/* read at unaligned location and compare */
		memset(map, 0, fix_info.smem_len);
		memset(&map[off], 0x55, len);
		memset(buf, 0xff, fix_info.smem_len);

		ret = pread(fd, &buf[off], len, off);
		igt_assert_f(ret == (ssize_t)len,
			     "pread failed, ret=%zd\n", ret);

		pos = memchr(buf, 0x55, fix_info.smem_len);
		igt_assert_f(pos, "0x55 not found within read buffer\n");
		igt_assert_f(pos == &buf[off],
			     "0x55 found at pos %zu, expected %lld\n",
			     pos - buf, (long long)off);

		pos = memchr(&buf[off], 0xff, fix_info.smem_len - off);
		igt_assert_f(pos, "0xff not found within read buffer\n");
		igt_assert_f(pos == &buf[off + len],
			     "0xff found at pos %zu, expected %lld\n",
			     pos - buf, (long long)(off + len));

		pos = memchr(&buf[off + len],
			     0x55,
			     fix_info.smem_len - (off + len));
		igt_assert_f(pos,
			     "found 0x55 at pos %zu, none expected\n",
			     pos - buf);
	}

	igt_fixture {
		free(buf);
		/* don't leave garbage on the screen */
		memset(map, 0, fix_info.smem_len);
		munmap(map, fix_info.smem_len);
	}
}

igt_main
{
	volatile int fd = -1;

	/*
	 * Should this test focus on the fbdev independent of any drm driver,
	 * or should it look for fbdev of a particular device?
	 */
	igt_fixture {
		fd = open("/dev/fb0", O_RDWR);
		if (fd < 0) {
			drm_load_module(DRIVER_ANY);
			fd = open("/dev/fb0", O_RDWR);
		}
		igt_require_f(fd != -1, "/dev/fb0\n");
	}

	igt_describe("Check modesetting");
	igt_subtest_group {
		mode_tests(fd);
	}

	igt_describe("Check framebuffer access");
	igt_subtest_group {
		framebuffer_tests(fd);
	}

	igt_fixture {
		close(fd);
	}
}
