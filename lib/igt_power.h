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

#ifndef IGT_POWER_H
#define IGT_POWER_H

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

struct igt_power {
	struct rapl rapl;
	int hwmon_fd;
};

int igt_power_open(int i915, struct igt_power *p, const char *domain);
void igt_power_close(struct igt_power *p);

static inline bool igt_power_valid(struct igt_power *p)
{
	return (p->rapl.fd >= 0) || (p->hwmon_fd >= 0);
}

void igt_power_get_energy(struct igt_power *p, struct power_sample *s);

double igt_power_get_mJ(const struct igt_power *power,
			const struct power_sample *p0,
			const struct power_sample *p1);
double igt_power_get_mW(const struct igt_power *power,
			const struct power_sample *p0,
			const struct power_sample *p1);
double igt_power_get_s(const struct power_sample *p0,
		       const struct power_sample *p1);
#endif /* IGT_POWER_H */
