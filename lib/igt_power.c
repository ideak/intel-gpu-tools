#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <locale.h>
#include <math.h>
#include <unistd.h>
#include <inttypes.h>

#include "igt.h"
#include "igt_hwmon.h"
#include "igt_perf.h"
#include "igt_power.h"
#include "igt_sysfs.h"

static const char *rapl_domains[] = { "cpu", "gpu", "pkg", "ram" };

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

static int rapl_open(struct rapl *r, const char *domain)
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

static inline bool rapl_read(struct rapl *r, struct power_sample *s)
{
	return read(r->fd, s, sizeof(*s)) == sizeof(*s);
}

static inline void rapl_close(struct rapl *r)
{
	close(r->fd);
	r->fd = -1;
}

/**
 * igt_power_open:
 * @fd : device fd
 * @igt_power : power struct
 * @domain: rapl domain
 *
 * opens the hwmon/rapl fd based on domain
 * hwmon fd - domain gpu -dgfx
 * rapl fd - all domains - igfx
 *
 * Returns
 * 0 on success, errno otherwise
 */
int igt_power_open(int fd, struct igt_power *p, const char *domain)
{
	int i;

	p->hwmon_fd = -1;
	p->rapl.fd = -1;

	if (gem_has_lmem(fd)) {
		if (strncmp(domain, "gpu", strlen("gpu")) == 0) {
			p->hwmon_fd = igt_hwmon_open(fd);
			if (p->hwmon_fd >= 0)
				return 0;
		}
	} else {
		for (i = 0; i < ARRAY_SIZE(rapl_domains); i++)
			if (strncmp(domain, rapl_domains[i], strlen(rapl_domains[i])) == 0)
				return rapl_open(&p->rapl, domain);
	}

	return -EINVAL;
}

/**
 * igt_power_get_energy:
 * @igt_power : power struct
 * @power_sample: sample of energy and time
 *
 * Reads energy from hwmon if energy1_input file is present, else read
 * from rapl interface
 *
 */
void igt_power_get_energy(struct igt_power *power, struct power_sample *s)
{
	struct timespec ts;

	s->energy = 0;
	igt_assert(!clock_gettime(CLOCK_MONOTONIC, &ts));
	s->time =  ts.tv_sec * NSEC_PER_SEC + ts.tv_nsec;

	if (power->hwmon_fd >= 0) {
		if (igt_sysfs_has_attr(power->hwmon_fd, "energy1_input"))
			s->energy = igt_sysfs_get_u64(power->hwmon_fd, "energy1_input");
	} else if (power->rapl.fd >= 0) {
		rapl_read(&power->rapl, s);
	}
}

/**
 * igt_power_get_mJ:
 * @igt_power : power struct
 * @power_sample: sample of energy and time
 *
 * Calculates energy difference between two power samples
 *
 * Returns
 * energy in mJ from hwmon/rapl
 */
double igt_power_get_mJ(const struct igt_power *power,
			const struct power_sample *p0, const struct power_sample *p1)
{
	if (power->hwmon_fd >= 0)
		return (p1->energy - p0->energy) * 1e-3;
	else if (power->rapl.fd >= 0)
		return ((p1->energy - p0->energy) *  power->rapl.scale) * 1e3;

	return 0;
}

/**
 * igt_power_get_mW:
 * @igt_power : power struct
 * @power_sample: sample of energy and time
 *
 * Calculates power
 *
 * Returns
 * power in milliWatts
 */
double igt_power_get_mW(const struct igt_power *power,
			const struct power_sample *p0, const struct power_sample *p1)
{
	return igt_power_get_mJ(power, p0, p1) / igt_power_get_s(p0, p1);
}

/**
 * igt_power_get_s:
 * @power_sample: sample of energy and time
 *
 * Returns
 * time differnce in seconds
 */
double igt_power_get_s(const struct power_sample *p0,
		       const struct power_sample *p1)
{
	return (p1->time - p0->time) * 1e-9;
}

/**
 * igt_power_close:
 * @igt_power : power struct
 *
 * closes hwmon/rapl fd
 *
 */
void igt_power_close(struct igt_power *power)
{
	if (power->hwmon_fd >= 0) {
		close(power->hwmon_fd);
		power->hwmon_fd = -1;
	} else if (power->rapl.fd >= 0) {
		rapl_close(&power->rapl);
	}
}
