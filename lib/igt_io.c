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
 *
 */

#include <errno.h>
#include <stdlib.h>
#include <unistd.h>

#include "igt_io.h"

/**
 * SECTION:igt_io
 * @short_description: Helpers for file I/O
 * @title: io
 * @include: igt_io.h
 *
 * This library provides helpers for file I/O
 */

/**
 * igt_readn:
 * @fd: the file descriptor
 * @buf: buffer where the contents will be stored, allocated by the caller
 * @size: size of the buffer
 *
 * Read from fd into provided buffer until the buffer is full or EOF
 *
 * Returns:
 * -errno on failure or bytes read on success
 */
ssize_t igt_readn(int fd, char *buf, size_t len)
{
	ssize_t ret;
	size_t total = 0;

	do {
		ret = read(fd, buf + total, len - total);
		if (ret < 0)
			ret = -errno;
		if (ret == -EINTR || ret == -EAGAIN)
			continue;
		if (ret <= 0)
			break;
		total += ret;
	} while (total != len);
	return total ?: ret;
}

/**
 * igt_writen:
 * @fd: the file descriptor
 * @buf: the block with the contents to write
 * @len: the length to write
 *
 * This writes @len bytes from @data to the sysfs file.
 *
 * Returns:
 * -errno on failure or bytes written on success
 */
ssize_t igt_writen(int fd, const char *buf, size_t len)
{
	ssize_t ret;
	size_t total = 0;

	do {
		ret = write(fd, buf + total, len - total);
		if (ret < 0)
			ret = -errno;
		if (ret == -EINTR || ret == -EAGAIN)
			continue;
		if (ret <= 0)
			break;
		total += ret;
	} while (total != len);
	return total ?: ret;
}
