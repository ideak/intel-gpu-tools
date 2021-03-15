/*
 * Copyright © 2011 Intel Corporation
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
 * Authors:
 *    Chris Wilson <chris@chris-wilson.co.uk>
 *
 */

#include <linux/userfaultfd.h>

#include <pthread.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <inttypes.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/time.h>

#include "drm.h"
#include "i915/gem.h"
#include "igt.h"
#include "igt_rand.h"
#include "igt_vgem.h"

#define MiB(x) ((x) * 1024 * 1024)

typedef void *(*mmap_fn_t)(int, uint32_t, uint64_t, uint64_t, unsigned int);

static void *wrap_gem_mmap__gtt(int i915, uint32_t handle,
				uint64_t offset, uint64_t length,
				unsigned int prot)
{
	return gem_mmap__gtt(i915, handle, length, prot);
}

static void pwrite_self(int i915)
{
	int start = gem_has_mappable_ggtt(i915) ? 0 : 1;
	static const mmap_fn_t mmap_fn[] = {
		wrap_gem_mmap__gtt,
		gem_mmap__cpu,
		gem_mmap__wc,
		NULL
	};
	for (const mmap_fn_t *fn = mmap_fn + start; *fn; fn++) {
		uint32_t handle = gem_create(i915, MiB(4));
		void *ptr = (*fn)(i915, handle, 0, MiB(4), PROT_READ);

		gem_write(i915, handle, 0, ptr + MiB(3), MiB(1));
		gem_write(i915, handle, MiB(3), ptr, MiB(1));
		gem_write(i915, handle, MiB(1), ptr + MiB(1), MiB(2));

		munmap(ptr, MiB(4));
		gem_close(i915, handle);
	}
}

#define OBJECT_SIZE 16384

#define COPY_BLT_CMD		(2<<29|0x53<<22|0x6)
#define BLT_WRITE_ALPHA		(1<<21)
#define BLT_WRITE_RGB		(1<<20)
#define BLT_SRC_TILED		(1<<15)
#define BLT_DST_TILED		(1<<11)

static void do_gem_write(int fd, uint32_t handle, void *buf, int len, int loops)
{
	while (loops--)
		gem_write(fd, handle, 0, buf, len);
}

static double elapsed(const struct timeval *start,
		      const struct timeval *end,
		      int loop)
{
	return (1e6*(end->tv_sec - start->tv_sec) + (end->tv_usec - start->tv_usec))/loop;
}

static const char *bytes_per_sec(char *buf, double v)
{
	const char *order[] = {
		"",
		"KiB",
		"MiB",
		"GiB",
		"TiB",
		NULL,
	}, **o = order;

	while (v > 1000 && o[1]) {
		v /= 1000;
		o++;
	}
	sprintf(buf, "%.1f%s/s", v, *o);
	return buf;
}

#define FORWARD 0x1
#define BACKWARD 0x2
#define RANDOM 0x4
static void test_big_cpu(int fd, int scale, unsigned flags)
{
	uint64_t offset, size;
	uint32_t handle;

	switch (scale) {
	case 0:
		size = gem_mappable_aperture_size(fd) + 4096;
		break;
	case 1:
		size = gem_global_aperture_size(fd) + 4096;
		break;
	case 2:
		size = gem_aperture_size(fd) + 4096;
		break;
	}
	intel_require_memory(1, size, CHECK_RAM);

	handle = gem_create(fd, size);
	gem_set_domain(fd, handle, I915_GEM_DOMAIN_CPU, I915_GEM_DOMAIN_CPU);

	if (flags & FORWARD) {
		igt_debug("Forwards\n");
		for (offset = 0; offset < size; offset += 4096) {
			int suboffset = (offset >> 12) % (4096 - sizeof(offset));
			uint64_t tmp;

			gem_write(fd, handle, offset + suboffset, &offset, sizeof(offset));
			gem_read(fd, handle, offset + suboffset, &tmp, sizeof(tmp));
			igt_assert_eq_u64(offset, tmp);
		}
	}

	if (flags & BACKWARD) {
		igt_debug("Backwards\n");
		for (offset = size >> 12; offset--; ) {
			int suboffset = 4096 - (offset % (4096 - sizeof(offset)));
			uint64_t tmp;

			gem_write(fd, handle, (offset<<12) + suboffset, &offset, sizeof(offset));
			gem_read(fd, handle, (offset<<12) + suboffset, &tmp, sizeof(tmp));
			igt_assert_eq_u64(offset, tmp);
		}
	}

	if (flags & RANDOM) {
		igt_debug("Random\n");
		for (offset = 0; offset < size >> 12; offset++) {
			uint64_t tmp = rand() % (size >> 12);
			int suboffset = tmp % (4096 - sizeof(offset));

			gem_write(fd, handle, (tmp << 12) + suboffset, &offset, sizeof(offset));
			gem_read(fd, handle, (tmp << 12) + suboffset, &tmp, sizeof(tmp));
			igt_assert_eq_u64(offset, tmp);
		}
	}

	gem_close(fd, handle);
}

static void test_big_gtt(int fd, int scale, unsigned flags)
{
	uint64_t offset, size;
	uint64_t *ptr;
	uint32_t handle;

	igt_require(gem_mmap__has_wc(fd));
	switch (scale) {
	case 0:
		size = gem_mappable_aperture_size(fd) + 4096;
		break;
	case 1:
		size = gem_global_aperture_size(fd) + 4096;
		break;
	case 2:
		size = gem_aperture_size(fd) + 4096;
		break;
	}
	intel_require_memory(1, size, CHECK_RAM);

	handle = gem_create(fd, size);
	gem_set_domain(fd, handle, I915_GEM_DOMAIN_GTT, I915_GEM_DOMAIN_GTT);

	ptr = gem_mmap__wc(fd, handle, 0, size, PROT_READ);

	if (flags & FORWARD) {
		igt_debug("Forwards\n");
		for (offset = 0; offset < size; offset += 4096) {
			int suboffset = (offset >> 12) % (4096 / sizeof(offset) - 1) * sizeof(offset);

			gem_write(fd, handle, offset + suboffset, &offset, sizeof(offset));
			gem_set_domain(fd, handle, I915_GEM_DOMAIN_WC, 0);
			igt_assert_eq_u64(ptr[(offset + suboffset)/sizeof(offset)], offset);
		}
	}

	if (flags & BACKWARD) {
		igt_debug("Backwards\n");
		for (offset = size >> 12; offset--; ) {
			int suboffset = (4096 - (offset % (4096 - sizeof(offset)))) & -sizeof(offset);
			gem_write(fd, handle, (offset<<12) + suboffset, &offset, sizeof(offset));
			gem_set_domain(fd, handle, I915_GEM_DOMAIN_WC, 0);
			igt_assert_eq_u64(ptr[((offset<<12) + suboffset)/sizeof(offset)], offset);
		}
	}

	if (flags & RANDOM) {
		igt_debug("Random\n");
		for (offset = 0; offset < size >> 12; offset++) {
			uint64_t tmp = rand() % (size >> 12);
			int suboffset = (tmp % 4096) & -sizeof(offset);

			tmp = (tmp << 12) + suboffset;
			gem_write(fd, handle, tmp, &offset, sizeof(offset));
			gem_set_domain(fd, handle, I915_GEM_DOMAIN_WC, 0);
			igt_assert_eq_u64(ptr[tmp/sizeof(offset)], offset);
		}
	}

	munmap(ptr, size);
	gem_close(fd, handle);
}

static void test_random(int fd)
{
	uint32_t prng = 0xdeadbeef;
	unsigned long count;
	uint32_t handle;
	uint64_t *map;
	uint64_t size;

	gem_require_mmap_wc(fd);

	size = min(intel_get_total_ram_mb() / 2,
		    gem_mappable_aperture_size(fd) + 4096);
	intel_require_memory(1, size, CHECK_RAM);

	handle = gem_create(fd, size);
	map = gem_mmap__wc(fd, handle, 0, size, PROT_WRITE);

	count = 0;
	igt_until_timeout(5) {
		uint64_t a = hars_petruska_f54_1_random64(&prng) % (size / sizeof(uint64_t));
		uint64_t x = hars_petruska_f54_1_random64(&prng);

		gem_write(fd, handle, a * sizeof(x), &x, sizeof(x));

		gem_set_domain(fd, handle, I915_GEM_DOMAIN_WC, 0);
		igt_assert_eq_u64(map[a], x);

		count++;
	}
	igt_info("Completed %lu cycles\n", count);

	munmap(map, handle);
	gem_close(fd, handle);
}

static int userfaultfd(int flags)
{
	return syscall(SYS_userfaultfd, flags);
}

struct ufd_thread {
	uint32_t *page;
	int i915;
	int vgem;
	int err;
};

static int __prime_handle_to_fd(int fd, uint32_t handle)
{
	struct drm_prime_handle args;

	memset(&args, 0, sizeof(args));
	args.handle = handle;
	args.flags = DRM_CLOEXEC;
	args.fd = -1;

	ioctl(fd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &args);
	return args.fd;
}

static uint32_t dmabuf_create_handle(int i915, int vgem)
{
	struct vgem_bo scratch;
	uint32_t handle;
	int dmabuf;

	scratch.width = 64;
	scratch.height = 64;
	scratch.bpp = 32;
	vgem_create(vgem, &scratch);

	dmabuf = __prime_handle_to_fd(vgem, scratch.handle);
	if (dmabuf < 0)
		return 0;

	handle = prime_fd_to_handle(i915, dmabuf);
	close(dmabuf);

	return handle;
}

static void *ufd_thread(void *arg)
{
	struct ufd_thread *t = arg;
	uint32_t handle = dmabuf_create_handle(t->i915, t->vgem);
	int err;

	err = -EMFILE;
	if (handle) {
		err = __gem_write(t->i915, handle, 0, t->page, 1);
		gem_close(t->i915, handle);
	}
	if (err)
		t->err = err;

	return NULL;
}

static void write_value(const char *path, int value)
{
	char buf[80];
	int fd, len;

	len = sprintf(buf, "%d", value);
	if (len < 0)
		return;

	fd = open(path, O_WRONLY);
	if (fd != -1) {
		write(fd, buf, len);
		close(fd);
	}
}

static void unlimited_processes(unsigned int limit)
{
	struct rlimit rlim;

	write_value("/proc/sys/kernel/threads-max", 150000);
	write_value("/proc/sys/vm/max_map_count", 500000);
	write_value("/proc/sys/kernel/pid_max", 200000);

	if (getrlimit(RLIMIT_NPROC, &rlim))
		return;

	rlim.rlim_cur = limit;
	rlim.rlim_max = limit;
	setrlimit(RLIMIT_NPROC, &rlim);
}

static void test_exhaustion(int i915)
{
	struct uffdio_api api = { .api = UFFD_API };
	struct uffdio_register reg;
	struct uffdio_copy copy;
	struct ufd_thread t = {
		.i915 = i915,
		.vgem = drm_open_driver(DRIVER_VGEM),
	};
	pthread_t *thread = NULL;
	struct uffd_msg msg;
	unsigned long count;
	char buf[4096];
	int ufd;

	unlimited_processes(1024 * 1024);

	ufd = userfaultfd(0);
	igt_require_f(ufd != -1, "kernel support for userfaultfd\n");
	igt_require_f(ioctl(ufd, UFFDIO_API, &api) == 0 && api.api == UFFD_API,
		      "userfaultfd API v%lld:%lld\n", UFFD_API, api.api);

	t.page = mmap(NULL, 4096, PROT_WRITE, MAP_SHARED | MAP_ANON, 0, 0);
	igt_assert(t.page != MAP_FAILED);

	/* Register our fault handler for t.page */
	memset(&reg, 0, sizeof(reg));
	reg.mode = UFFDIO_REGISTER_MODE_MISSING;
	reg.range.start = to_user_pointer(t.page);
	reg.range.len = 4096;
	do_ioctl(ufd, UFFDIO_REGISTER, &reg);

	count = 0;
	while (!READ_ONCE(t.err)) {
		if (is_power_of_two(count)) {
			unsigned long sz = count ? 2 * count : 1;
			thread = realloc(thread, sz * sizeof(*thread));
			igt_assert(thread);
		}
		if (pthread_create(&thread[count], NULL, ufd_thread, &t))
			break;

		if (count == 0) { /* Wait for the first userfault */
			igt_assert_eq(read(ufd, &msg, sizeof(msg)), sizeof(msg));
			igt_assert_eq(msg.event, UFFD_EVENT_PAGEFAULT);
			igt_assert(from_user_pointer(msg.arg.pagefault.address) == t.page);
		}

		count++;
	}
	igt_assert(count);
	if (t.err)
		igt_warn("err:%d after %lu threads\n", t.err, count);

	/* Service the fault; releasing the stuck ioctls */
	memset(&copy, 0, sizeof(copy));
	copy.dst = msg.arg.pagefault.address;
	copy.src = to_user_pointer(memset(buf, 0xc5, sizeof(buf)));
	copy.len = 4096;
	do_ioctl(ufd, UFFDIO_COPY, &copy);

	while (count--)
		pthread_join(thread[count], NULL);
	free(thread);

	munmap(t.page, 4096);
	close(ufd);

	close(t.vgem);
}

uint32_t *src, dst;
int fd;
int object_size = 0;

static int opt_handler(int opt, int opt_index, void *data)
{
	switch (opt) {
	case 's':
		object_size = atoi(optarg);
		break;
	default:
		return IGT_OPT_HANDLER_ERROR;
	}

	return IGT_OPT_HANDLER_SUCCESS;
}

const char *help_str = "  -s\tObject size in bytes\n";

igt_main_args("s:", NULL, help_str, opt_handler, NULL)
{
	double usecs;
	const char* bps;
	char buf[100];
	int count;
	const struct {
		int level;
		const char *name;
	} cache[] = {
		{ 0, "uncached" },
		{ 1, "snoop" },
		{ 2, "display" },
		{ -1 },
	}, *c;

	if (object_size == 0)
		object_size = OBJECT_SIZE;
	object_size = (object_size + 3) & -4;

	igt_fixture {
		fd = drm_open_driver(DRIVER_INTEL);
		gem_require_pread_pwrite(fd);

		dst = gem_create(fd, object_size);
		src = malloc(object_size);
	}

	igt_subtest("bench") {
		for (count = 1; count <= 1<<17; count <<= 1) {
			struct timeval start, end;

			gettimeofday(&start, NULL);
			do_gem_write(fd, dst, src, object_size, count);
			gettimeofday(&end, NULL);
			usecs = elapsed(&start, &end, count);
			bps = bytes_per_sec(buf, object_size/usecs*1e6);
			igt_info("Time to pwrite %d bytes x %6d:	%7.3fµs, %s\n",
				 object_size, count, usecs, bps);
			fflush(stdout);
		}
	}

	igt_subtest("basic-self")
		pwrite_self(fd);

	igt_subtest("basic-exhaustion")
		test_exhaustion(fd);

	for (c = cache; c->level != -1; c++) {
		igt_subtest(c->name) {
			gem_set_caching(fd, dst, c->level);

			for (count = 1; count <= 1<<17; count <<= 1) {
				struct timeval start, end;

				gettimeofday(&start, NULL);
				do_gem_write(fd, dst, src, object_size, count);
				gettimeofday(&end, NULL);
				usecs = elapsed(&start, &end, count);
				bps = bytes_per_sec(buf, object_size/usecs*1e6);
				igt_info("Time to %s pwrite %d bytes x %6d:	%7.3fµs, %s\n",
					 c->name, object_size, count, usecs, bps);
				fflush(stdout);
			}
		}
	}

	igt_fixture {
		free(src);
		gem_close(fd, dst);
	}

	igt_subtest_f("basic-random")
		test_random(fd);

	{
		const struct mode {
			const char *name;
			unsigned flags;
		} modes[] = {
			{ "forwards", FORWARD },
			{ "backwards", BACKWARD },
			{ "random", RANDOM },
			{ "fbr", FORWARD | BACKWARD | RANDOM },
			{ NULL },
		}, *m;
		for (m = modes; m->name; m++) {
			igt_subtest_f("small-cpu-%s", m->name)
				test_big_cpu(fd, 0, m->flags);
			igt_subtest_f("small-gtt-%s", m->name)
				test_big_gtt(fd, 0, m->flags);

			igt_subtest_f("big-cpu-%s", m->name)
				test_big_cpu(fd, 1, m->flags);
			igt_subtest_f("big-gtt-%s", m->name)
				test_big_gtt(fd, 1, m->flags);

			igt_subtest_f("huge-cpu-%s", m->name)
				test_big_cpu(fd, 2, m->flags);
			igt_subtest_f("huge-gtt-%s", m->name)
				test_big_gtt(fd, 2, m->flags);
		}
	}

	igt_fixture
		close(fd);
}
