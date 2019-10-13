#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <locale.h>
#include <math.h>
#include <unistd.h>
#include <inttypes.h>

#include "igt_perf.h"
#include "igt_rapl.h"
#include "igt_sysfs.h"

static int rapl_parse(struct rapl *r, const char *str)
{
	locale_t locale, oldlocale;
	bool result = true;
	char buf[128];
	int dir;

	memset(r, 0, sizeof(*r));

	dir = open("/sys/devices/power", O_RDONLY);
	if (dir < 0)
		return -errno;

	/* Replace user environment with plain C to match kernel format */
	locale = newlocale(LC_ALL, "C", 0);
	oldlocale = uselocale(locale);

	result &= igt_sysfs_scanf(dir, "type", "%"PRIu64, &r->type) == 1;

	snprintf(buf, sizeof(buf), "events/energy-%s", str);
	result &= igt_sysfs_scanf(dir, buf, "event=%"PRIx64, &r->power) == 1;

	snprintf(buf, sizeof(buf), "events/energy-%s.scale", str);
	result &= igt_sysfs_scanf(dir, buf, "%lf", &r->scale) == 1;

	uselocale(oldlocale);
	freelocale(locale);

	close(dir);

	if (!result)
		return -EINVAL;

	if (isnan(r->scale) || !r->scale)
		return -ERANGE;

	return 0;
}

int rapl_open(struct rapl *r, const char *domain)
{
	r->fd = rapl_parse(r, domain);
	if (r->fd < 0)
		goto err;

	r->fd = igt_perf_open(r->type, r->power);
	if (r->fd < 0) {
		r->fd = -errno;
		goto err;
	}

	return 0;

err:
	errno = 0;
	return r->fd;
}
