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
#include <limits.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <linux/fb.h>

#include "igt.h"

#define PANSTEP(panstep_) \
	((panstep_) ? (panstep_) : 1)

static unsigned int __panoffset(unsigned int offset, unsigned int panstep)
{
	return offset - (offset % PANSTEP(panstep));
}

#define XOFFSET(offset_) \
	__panoffset(offset_, fix_info.xpanstep)

#define YOFFSET(offset_) \
	__panoffset(offset_, fix_info.ypanstep)

static void pan_test(int fd, const struct fb_var_screeninfo *var, int expected_ret)
{
	struct fb_var_screeninfo pan_var, new_var;
	int ret;

	memcpy(&pan_var, var, sizeof(pan_var));

	ret = ioctl(fd, FBIOPAN_DISPLAY, &pan_var);
	igt_assert_f(ret == expected_ret,
		     "ioctl(FBIOPAN_DISPLAY) returned ret=%d, expected %d\n", ret, expected_ret);

	if (ret)
		return; /* panning failed; skip additional tests */

	ret = ioctl(fd, FBIOGET_VSCREENINFO, &new_var);
	igt_assert_f(ret == 0, "ioctl(FBIOGET_VSCREENINFO) failed, ret=%d\n", ret);
	igt_assert_f(pan_var.xoffset == new_var.xoffset && pan_var.yoffset == new_var.yoffset,
		     "panning to (%u, %u) moved to (%u, %u)\n",
		     pan_var.xoffset, pan_var.yoffset, new_var.xoffset, new_var.yoffset);
}

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
		unsigned long nbits, nlines;

		/* video memory configuration */
		igt_assert_f(fix_info.line_length, "line length not set\n");
		igt_assert_f(fix_info.smem_len, "size of video memory not set\n");
		igt_assert_f(fix_info.line_length <= fix_info.smem_len,
			     "line length (%u) exceeds available video memory (%u)\n",
			     fix_info.line_length, fix_info.smem_len);

		/* color format */
		igt_assert_f(var_info.bits_per_pixel, "bits-per-pixel not set\n");

		/* horizontal resolution */
		igt_assert_f(var_info.xres, "horizontal resolution not set\n");
		igt_assert_f(var_info.xres_virtual, "horizontal virtual resolution not set\n");
		igt_assert_f(var_info.xres <= var_info.xres_virtual,
			     "horizontal virtual resolution (%u) less than horizontal resolution (%u)\n",
			     var_info.xres_virtual, var_info.xres);
		igt_assert_f(var_info.xoffset <= (var_info.xres_virtual - var_info.xres),
			     "screen horizontal offset (%u) overflow\n",
			     var_info.xoffset);
		nbits = fix_info.line_length * CHAR_BIT;
		igt_assert_f((var_info.xres_virtual * var_info.bits_per_pixel) <= nbits,
			     "vertical virtual resolution (%u) with bpp %u exceeds line length %u\n",
			     var_info.yres_virtual, var_info.bits_per_pixel, fix_info.line_length);

		/* vertical resolution */
		igt_assert_f(var_info.yres, "vertical resolution not set\n");
		igt_assert_f(var_info.yres_virtual, "vertical virtual resolution not set\n");
		igt_assert_f(var_info.yres <= var_info.yres_virtual,
			     "vertical virtual resolution (%u) less than vertical resolution (%u)\n",
			     var_info.yres_virtual, var_info.yres);
		igt_assert_f((var_info.vmode & FB_VMODE_YWRAP) ||
			     (var_info.yoffset <= (var_info.yres_virtual - var_info.yres)),
			     "screen vertical offset (%u) overflow\n",
			     var_info.yoffset);
		nlines = fix_info.smem_len / fix_info.line_length;
		igt_assert_f(var_info.yres_virtual <= nlines,
			     "vertical virtual resolution (%u) with line length %u exceeds available video memory\n",
			     var_info.yres_virtual, fix_info.line_length);
	}

	igt_describe("Check panning / page flipping");
	igt_subtest("pan") {
		struct fb_var_screeninfo pan_var;
		int expected_ret;

		memset(&pan_var, 0, sizeof(pan_var));

		/*
		 * Tests that are expected to succeed.
		 */

		igt_debug("Jump to opposite end of virtual screen\n");
		pan_var.xoffset = XOFFSET(var_info.xres_virtual - var_info.xres - var_info.xoffset);
		pan_var.yoffset = YOFFSET(var_info.yres_virtual - var_info.yres - var_info.yoffset);
		pan_test(fd, &pan_var, 0);
		igt_debug("Jump to (0, 0)\n");
		pan_var.xoffset = XOFFSET(0);
		pan_var.yoffset = YOFFSET(0);
		pan_test(fd, &pan_var, 0);
		igt_debug("Jump to maximum extend\n");
		pan_var.xoffset = XOFFSET(var_info.xres_virtual - var_info.xres);
		pan_var.yoffset = YOFFSET(var_info.yres_virtual - var_info.yres);
		pan_test(fd, &pan_var, 0);

		/*
		 * Tests that are expected to fail.
		 */

		igt_debug("Jump beyond maximum horizontal extend\n");
		pan_var.xoffset = XOFFSET(var_info.xres_virtual - var_info.xres + PANSTEP(fix_info.xpanstep));
		pan_var.yoffset = YOFFSET(0);
		pan_test(fd, &pan_var, -1);
		igt_debug("Jump beyond horizontal virtual resolution\n");
		pan_var.xoffset = XOFFSET(var_info.xres_virtual);
		pan_var.yoffset = YOFFSET(0);
		pan_test(fd, &pan_var, -1);

		/*
		 * The FB_VMODE_YWRAP flag is configurable as part of ioctl(FBIOPAN_DISPLAY),
		 * but it's hard to know which drivers support it and which don't. Testing for
		 * FBINFO_HWACCEL_YWRAP does not produce meaningful results. So we got with the
		 * device's current setting.
		 *
		 * With FB_VMODE_YWRAP set, the display is expected to wrap around when
		 * reaching the limits of the vertical resolution. Otherwise, this should
		 * fail.
		 *
		 */

		if (var_info.vmode & FB_VMODE_YWRAP) {
			pan_var.vmode |= FB_VMODE_YWRAP;
			expected_ret = 0;
		} else {
			expected_ret = -1;
		}

		igt_debug("Jump beyond maximum vertical extend\n");
		pan_var.xoffset = XOFFSET(0);
		pan_var.yoffset = YOFFSET(var_info.yres_virtual - var_info.yres + PANSTEP(fix_info.ypanstep));
		pan_test(fd, &pan_var, expected_ret);
		igt_debug("Jump beyond vertical virtual resolution\n");
		pan_var.xoffset = XOFFSET(0);
		pan_var.yoffset = YOFFSET(var_info.yres_virtual);
		pan_test(fd, &pan_var, expected_ret);

		pan_var.vmode &= ~FB_VMODE_YWRAP;
	}

	igt_fixture {
		/* restore original panning offsets */
		ioctl(fd, FBIOPAN_DISPLAY, &var_info);
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

		/* allocate two additional bytes for eof test */
		buf = malloc(fix_info.smem_len + 2);
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
		igt_assert_f(!pos,
			     "found 0x55 at pos %zu, none expected\n",
			     pos - buf);
	}

	igt_describe("Check write operations on framebuffer memory");
	igt_subtest("write") {
		ssize_t ret;

		/* write to framebuffer and compare */
		for (int i = 0; i < ARRAY_SIZE(values); i++) {
			memset(buf, values[i], fix_info.smem_len);
			ret = pwrite(fd, buf, fix_info.smem_len, 0);
			igt_assert_f(ret == (ssize_t)fix_info.smem_len,
				     "pwrite failed, ret=%zd\n", ret);
			igt_assert_f(!memcmp(map, buf, fix_info.smem_len),
				     "write differs from mapped framebuffer for %x\n",
				     values[i]);
		}
	}

	igt_describe("Check write operations on unaligned locations in framebuffer memory");
	igt_subtest("unaligned-write") {
		const unsigned char *pos;
		ssize_t ret;
		size_t len;
		off_t off;

		off = pagesize + (pagesize >> 2); /* 1.25 * pagesize */
		len = (pagesize << 2) + (pagesize >> 1); /* 4.5 * pagesize */
		igt_require_f(off + len < fix_info.smem_len,
			      "framebuffer too small to test\n");

		/* read at unaligned location and compare */
		memset(map, 0xff, fix_info.smem_len);
		memset(buf, 0, fix_info.smem_len);
		memset(&buf[off], 0x55, len);

		ret = pwrite(fd, &buf[off], len, off);
		igt_assert_f(ret == (ssize_t)len,
			     "pwrite failed, ret=%zd\n", ret);

		pos = memchr(map, 0x55, fix_info.smem_len);
		igt_assert_f(pos, "0x55 not found within framebuffer\n");
		igt_assert_f(pos == &map[off],
			     "0x55 found at pos %zu, expected %lld\n",
			     pos - map, (long long)off);

		pos = memchr(&map[off], 0xff, fix_info.smem_len - off);
		igt_assert_f(pos, "0xff not found within framebuffer\n");
		igt_assert_f(pos == &map[off + len],
			     "0xff found at pos %zu, expected %lld\n",
			     pos - map, (long long)(off + len));

		pos = memchr(&map[off + len],
			     0x55,
			     fix_info.smem_len - (off + len));
		igt_assert_f(!pos,
			     "found 0x55 at pos %zu, none expected\n",
			     pos - map);
	}

	igt_describe("Check framebuffer access near EOF");
	igt_subtest("eof") {
		unsigned long lastindex = fix_info.smem_len - 1;
		unsigned char * const maplast = map + lastindex;
		unsigned char * const buflast = buf + lastindex;
		ssize_t ret;

		*buflast = 0x55;

		/* write across EOF; set remaining bytes */
		ret = pwrite(fd, buflast, 2, lastindex);
		igt_assert_f(ret == 1, "write crossed EOF, ret=%zd\n", ret);
		igt_assert_f(*maplast == *buflast,
			     "write buffer differs from mapped framebuffer at final byte, "
			     "maplast=%u buflast=%u\n", *maplast, *buflast);

		/* write at EOF; get ENOSPC */
		ret = pwrite(fd, &buflast[1], 1, lastindex + 1);
		igt_assert_f(ret == -1 && errno == ENOSPC,
			     "write at EOF, ret=%zd\n", ret);

		*maplast = 0;

		/* write final byte */
		ret = pwrite(fd, buflast, 1, lastindex);
		igt_assert_f(ret == 1, "write before EOF, ret=%zd\n", ret);
		igt_assert_f(*maplast == *buflast,
			     "write buffer differs from mapped framebuffer at final byte, "
			     "maplast=%u buflast=%u\n", *maplast, *buflast);

		/* write after EOF; get EFBIG */
		ret = pwrite(fd, &buflast[2], 1, lastindex + 2);
		igt_assert_f(ret == -1 && errno == EFBIG,
			     "write after EOF, ret=%zd\n", ret);

		*maplast = 0;

		/* read across the EOF; get remaining bytes */
		ret = pread(fd, buflast, 2, lastindex);
		igt_assert_f(ret == 1, "read before EOF, ret=%zd\n", ret);
		igt_assert_f(*maplast == *buflast,
			     "read buffer differs from mapped framebuffer at final byte, "
			     "maplast=%u buflast=%u\n", *maplast, *buflast);

		/* read after EOF; get 0 */
		ret = pread(fd, &buflast[1], 1, lastindex + 1);
		igt_assert_f(ret == 0, "read at EOF, ret=%zd\n", ret);
	}

	igt_describe("Check framebuffer access with NULL");
	igt_subtest("nullptr") {
		ssize_t ret;

		ret = pread(fd, NULL, fix_info.smem_len, 0);
		igt_assert_f(ret == -1 && errno == EFAULT,
			     "reading into NULL did not return EFAULT, ret=%zd\n",
			     ret);

		ret = pwrite(fd, NULL, fix_info.smem_len, 0);
		igt_assert_f(ret == -1 && errno == EFAULT,
			     "writing from NULL did not return EFAULT, ret=%zd\n",
			     ret);
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
