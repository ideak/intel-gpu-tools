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

#ifndef IGT_RAPL_H
#define IGT_RAPL_H

#include <stdbool.h>
#include <stdint.h>

struct rapl {
	uint64_t power, type;
	double scale;
	int fd;
};

struct power_sample {
	uint64_t energy;
	uint64_t time;
};

int rapl_open(struct rapl *r, const char *domain);

static inline int cpu_power_open(struct rapl *r)
{
	return rapl_open(r, "cpu");
}

static inline int gpu_power_open(struct rapl *r)
{
	return rapl_open(r, "gpu");
}

static inline int pkg_power_open(struct rapl *r)
{
	return rapl_open(r, "pkg");
}

static inline int ram_power_open(struct rapl *r)
{
	return rapl_open(r, "ram");
}

static inline bool rapl_read(struct rapl *r, struct power_sample *s)
{
	return read(r->fd, s, sizeof(*s)) == sizeof(*s);
}

static inline void rapl_close(struct rapl *r)
{
	close(r->fd);
}

static inline double power_J(const struct rapl *r,
			     const struct power_sample *p0,
			     const struct power_sample *p1)
{
	return (p1->energy - p0->energy) * r->scale;
}

static inline double power_s(const struct rapl *r,
			     const struct power_sample *p0,
			     const struct power_sample *p1)
{
	return (p1->time - p0->time) * 1e-9;
}

static inline double power_W(const struct rapl *r,
			     const struct power_sample *p0,
			     const struct power_sample *p1)
{
	return power_J(r, p0, p1) / power_s(r, p0, p1);
}

#endif /* IGT_RAPL_H */
