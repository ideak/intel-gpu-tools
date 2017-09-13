#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include "igt_perf.h"

uint64_t i915_type_id(void)
{
	char buf[1024];
	int fd, n;

	fd = open("/sys/bus/event_source/devices/i915/type", 0);
	if (fd < 0) {
		n = -1;
	} else {
		n = read(fd, buf, sizeof(buf)-1);
		close(fd);
	}
	if (n < 0)
		return 0;

	buf[n] = '\0';
	return strtoull(buf, 0, 0);
}

static int _perf_open(int config, int group, int format)
{
	struct perf_event_attr attr;

	memset(&attr, 0, sizeof (attr));

	attr.type = i915_type_id();
	if (attr.type == 0)
		return -ENOENT;

	attr.config = config;

	if (group >= 0)
		format &= ~PERF_FORMAT_GROUP;

	attr.read_format = format;

	return perf_event_open(&attr, -1, 0, group, 0);
}

int perf_i915_open(int config)
{
	return _perf_open(config, -1, PERF_FORMAT_TOTAL_TIME_ENABLED);
}

int perf_i915_open_group(int config, int group)
{
	return _perf_open(config, group,
			  PERF_FORMAT_TOTAL_TIME_ENABLED | PERF_FORMAT_GROUP);
}
