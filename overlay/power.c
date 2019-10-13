/*
 * Copyright © 2013 Intel Corporation
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

#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <errno.h>
#include <ctype.h>
#include <locale.h>
#include <math.h>

#include "igt_perf.h"

#include "power.h"
#include "debugfs.h"

static int
filename_to_buf(const char *filename, char *buf, unsigned int bufsize)
{
	int fd;
	ssize_t ret;

	fd = open(filename, O_RDONLY);
	if (fd < 0)
		return -1;

	ret = read(fd, buf, bufsize - 1);
	close(fd);
	if (ret < 1)
		return -1;

	buf[ret] = '\0';

	return 0;
}

static uint64_t filename_to_u64(const char *filename, int base)
{
	char buf[64], *b;

	if (filename_to_buf(filename, buf, sizeof(buf)))
		return 0;

	/*
	 * Handle both single integer and key=value formats by skipping
	 * leading non-digits.
	 */
	b = buf;
	while (*b && !isdigit(*b))
		b++;

	return strtoull(b, NULL, base);
}

static uint64_t rapl_type_id(void)
{
	return filename_to_u64("/sys/devices/power/type", 10);
}

static uint64_t rapl_gpu_power(void)
{
	return filename_to_u64("/sys/devices/power/events/energy-gpu", 0);
}

static uint64_t rapl_pkg_power(void)
{
	return filename_to_u64("/sys/devices/power/events/energy-pkg", 0);
}

static double filename_to_double(const char *filename)
{
	char *oldlocale;
	char buf[80];
	double v;

	if (filename_to_buf(filename, buf, sizeof(buf)))
		return 0;

	oldlocale = setlocale(LC_ALL, "C");
	v = strtod(buf, NULL);
	setlocale(LC_ALL, oldlocale);

	return v;
}

static double rapl_gpu_power_scale(void)
{
	return filename_to_double("/sys/devices/power/events/energy-gpu.scale");
}

static double rapl_pkg_power_scale(void)
{
	return filename_to_double("/sys/devices/power/events/energy-pkg.scale");
}

int power_init(struct power *power)
{
	memset(power, 0, sizeof(*power));

	power->gpu.fd = igt_perf_open(rapl_type_id(), rapl_gpu_power());
	if (power->gpu.fd < 0)
		return power->error = ENOENT;
	power->gpu.scale = rapl_gpu_power_scale() * 1e3; /* to milli */

	power->pkg.fd = igt_perf_open(rapl_type_id(), rapl_pkg_power());
	power->pkg.scale = rapl_pkg_power_scale() *1e3; /* to milli */

	return 0;
}

static void __power_update(struct power_domain *pd, int count)
{
	struct power_stat *s = &pd->stat[count & 1];
	struct power_stat *d = &pd->stat[(count + 1) & 1];
	uint64_t data[2], d_time;
	int len;

	len = read(pd->fd, data, sizeof(data));
	if (len != sizeof(data))
		return;

	s->energy = llround((double)data[0] * pd->scale);
	s->timestamp = data[1];

	if (!count)
		return;

	d_time = s->timestamp - d->timestamp;
	pd->power_mW = round((s->energy - d->energy) * 1e9 / d_time);
	pd->new_sample = 1;
}

int power_update(struct power *power)
{
	if (power->error)
		return power->error;

	__power_update(&power->gpu, power->count);
	__power_update(&power->pkg, power->count);

	if (!power->count++)
		return EAGAIN;

	return 0;
}
