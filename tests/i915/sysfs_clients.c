/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2021 Intel Corporation
 */

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <math.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "drmtest.h"
#include "i915/gem.h"
#include "i915/gem_context.h"
#include "i915/gem_engine_topology.h"
#include "i915/gem_mman.h"
#include "igt_aux.h"
#include "igt_dummyload.h"
#include "igt_sysfs.h"
#include "ioctl_wrappers.h"

#define __assert_within_epsilon(x, ref, tol_up, tol_down) \
	igt_assert_f((double)(x) <= (1.0 + (tol_up)) * (double)(ref) && \
		     (double)(x) >= (1.0 - (tol_down)) * (double)(ref), \
		     "'%s' != '%s' (%.3f not within +%.1f%%/-%.1f%% tolerance of %.3f)\n",\
		     #x, #ref, (double)(x), \
		     (tol_up) * 100.0, (tol_down) * 100.0, \
		     (double)(ref))

#define assert_within_epsilon(x, ref, tolerance) \
	__assert_within_epsilon(x, ref, tolerance / 100., tolerance / 100.)

#define MI_BATCH_BUFFER_START (0x31 << 23)
#define MI_BATCH_BUFFER_END (0xa << 23)
#define MI_ARB_CHECK (0x5 << 23)

#define MI_SEMAPHORE_WAIT		(0x1c << 23)
#define   MI_SEMAPHORE_POLL             (1 << 15)
#define   MI_SEMAPHORE_SAD_GT_SDD       (0 << 12)
#define   MI_SEMAPHORE_SAD_GTE_SDD      (1 << 12)
#define   MI_SEMAPHORE_SAD_LT_SDD       (2 << 12)
#define   MI_SEMAPHORE_SAD_LTE_SDD      (3 << 12)
#define   MI_SEMAPHORE_SAD_EQ_SDD       (4 << 12)
#define   MI_SEMAPHORE_SAD_NEQ_SDD      (5 << 12)

static void strterm(char *s, int len)
{
	igt_assert(len > 0);

	s[len] = '\0';
	if (s[len - 1] == '\n')
		s[len - 1] = '\0';
}

static void pidname(int i915, int clients)
{
	struct dirent *de;
	int sv[2], rv[2];
	char buf[280];
	int me = -1;
	long count;
	pid_t pid;
	DIR *dir;
	int len;

	dir = fdopendir(dup(clients));
	igt_assert(dir);
	rewinddir(dir);

	count = 0;
	while ((de = readdir(dir))) {
		if (!isdigit(de->d_name[0]))
			continue;

		snprintf(buf, sizeof(buf), "%s/name", de->d_name);
		len = igt_sysfs_read(clients, buf, buf, sizeof(buf) - 1);
		igt_assert_f(len > 0, "failed to open '%s/name'\n", de->d_name);
		strterm(buf, len);
		igt_debug("%s: %s\n", de->d_name, buf);

		/* Ignore closed clients created by drm_driver_open() */
		if (*buf == '<')
			continue;

		close(me);
		me = openat(clients, de->d_name, O_DIRECTORY | O_RDONLY);
		count++;
	}
	closedir(dir);

	/* We expect there to be only the single client (us) running */
	igt_assert_eq(count, 1);
	igt_assert(me >= 0);

	strterm(buf, igt_sysfs_read(me, "name", buf, sizeof(buf) - 1));

	igt_info("My name: %s\n", buf);
	igt_assert(strcmp(buf, igt_test_name()) == 0);

	if (!gem_has_contexts(i915))
		return;

	igt_assert(pipe(sv) == 0);
	igt_assert(pipe(rv) == 0);

	/* If give our fd to someone else, they take over ownership of client */
	igt_fork(child, 1) {
		read(sv[0], &pid, sizeof(pid));

		/*
		 * This transfer is based upon the assumption that the
		 * transfer is complete ala DRI3, where the parent will
		 * close the fd after sending it to the client. That is
		 * it is expected that the client be only active in a single
		 * process at any time.
		 */
		gem_context_destroy(i915, gem_context_create(i915));

		pid = getpid();
		write(rv[1], &pid, sizeof(pid));
	}
	close(sv[0]);
	close(rv[1]);

	/* Child exists, but not yet running, we still own the client */
	strterm(buf, igt_sysfs_read(me, "pid", buf, sizeof(buf) - 1));

	pid = getpid();
	igt_info("My pid: %s\n", buf);
	igt_assert_eq(atoi(buf), pid);

	/* Release and wait for the child */
	igt_assert_eq(write(sv[1], &pid, sizeof(pid)), sizeof(pid));
	igt_assert_eq(read(rv[0], &pid, sizeof(pid)), sizeof(pid));

	/* Now child owns the client and pid should be updated to match */
	strterm(buf, igt_sysfs_read(me, "pid", buf, sizeof(buf) - 1));

	igt_info("New pid: %s\n", buf);
	igt_assert_eq(atoi(buf), pid);
	igt_waitchildren();

	/* Child has definitely gone, but the client should remain */
	strterm(buf, igt_sysfs_read(me, "pid", buf, sizeof(buf) - 1));

	igt_info("Old pid: %s\n", buf);
	igt_assert_eq(atoi(buf), pid);

	/* And if we create a new context, ownership transfers back to us */
	gem_context_destroy(i915, gem_context_create(i915));
	strterm(buf, igt_sysfs_read(me, "pid", buf, sizeof(buf) - 1));

	igt_info("Our pid: %s\n", buf);
	igt_assert_eq(atoi(buf), getpid());

	/* Let battle commence. */

	close(sv[1]);
	close(rv[0]);
	close(me);
}

static long count_clients(int clients)
{
	struct dirent *de;
	long count = 0;
	char buf[280];
	DIR *dir;

	dir = fdopendir(dup(clients));
	igt_assert(dir);
	rewinddir(dir);

	while ((de = readdir(dir))) {
		int len;

		if (!isdigit(de->d_name[0]))
			continue;

		snprintf(buf, sizeof(buf), "%s/name", de->d_name);
		len = igt_sysfs_read(clients, buf, buf, sizeof(buf));
		if (len < 0)
			continue;

		count += *buf != '<';
	}
	closedir(dir);

	return count;
}

static void create(int i915, int clients)
{
	int fd[16];

	/* Each new open("/dev/dri/cardN") is a new client */
	igt_assert_eq(count_clients(clients), 1);
	for (int i = 0; i < ARRAY_SIZE(fd); i++) {
		fd[i] = gem_reopen_driver(i915);
		igt_assert_eq(count_clients(clients), i + 2);
	}

	for (int i = 0; i < ARRAY_SIZE(fd); i++)
		close(fd[i]);

	/* Cleanup delayed behind rcu */
	igt_until_timeout(30) {
		usleep(0);
		if (count_clients(clients) == 1)
			break;
		usleep(10000);
	}
	igt_assert_eq(count_clients(clients), 1);
}

static int find_me(int clients, pid_t pid)
{
	struct dirent *de;
	char buf[280];
	int me = -1;
	DIR *dir;

	dir = fdopendir(dup(clients));
	igt_assert(dir);
	rewinddir(dir);

	while ((de = readdir(dir))) {
		if (!isdigit(de->d_name[0]))
			continue;

		snprintf(buf, sizeof(buf), "%s/pid", de->d_name);
		igt_sysfs_read(clients, buf, buf, sizeof(buf));
		if (atoi(buf) != pid)
			continue;

		me = openat(clients, de->d_name, O_DIRECTORY | O_RDONLY);
		break;
	}

	closedir(dir);
	return me;
}

static int64_t read_runtime(int client, int class)
{
	char buf[80];

	snprintf(buf, sizeof(buf), "busy/%d", class);
	return igt_sysfs_get_u64(client, buf);
}

#define MAX_CLASS 64
static int read_runtimes(int client, int64_t *runtime)
{
	int fd = openat(client, "busy", O_DIRECTORY | O_RDONLY);
	DIR *dir = fdopendir(fd);
	struct dirent *de;
	int count = 0;

	memset(runtime, 0, sizeof(*runtime) * MAX_CLASS);
	if (!dir)
		return -1;

	while ((de = readdir(dir))) {
		int class;

		if (!isdigit(de->d_name[0]))
			continue;

		class = atoi(de->d_name);
		igt_assert(class < MAX_CLASS);
		runtime[class] = igt_sysfs_get_u64(fd, de->d_name);

		count += runtime[class] != 0;
	}
	closedir(dir);

	return count;
}

static uint64_t measured_usleep(unsigned int usec)
{
	struct timespec tv;
	unsigned int slept;

	slept = igt_nsec_elapsed(memset(&tv, 0, sizeof(tv)));
	igt_assert(slept == 0);
	do {
		usleep(usec - slept);
		slept = igt_nsec_elapsed(&tv) / 1000;
	} while (slept < usec);

	return igt_nsec_elapsed(&tv);
}

static int reopen_client(int i915)
{
	int clone;

	clone = gem_reopen_driver(i915);
	gem_context_copy_engines(i915, 0, clone, 0);
	close(i915);

	return clone;
}

static void
busy_one(int i915, int clients, const struct intel_execution_engine2 *e)
{
	int64_t active, idle, old, other[MAX_CLASS];
	struct timespec tv;
	igt_spin_t *spin;
	uint64_t delay;
	int me;

	/* Create a fresh client with 0 runtime */
	i915 = reopen_client(i915);

	me = find_me(clients, getpid());
	igt_assert(me != -1);

	spin = igt_spin_new(i915,
			    gem_context_clone_with_engines(i915, 0),
			    .engine = e->flags,
			    .flags = IGT_SPIN_POLL_RUN);

	igt_nsec_elapsed(memset(&tv, 0, sizeof(tv)));
	igt_spin_busywait_until_started(spin);

	for (int pass = 0; pass <= 10; pass++) {
		usleep(1500 >> pass);
		delay = igt_nsec_elapsed(&tv);
		igt_debug("delay: %'"PRIu64"ns\n", delay);

		/* Check that we accumulate the runtime, while active */
		active = read_runtime(me, e->class);
		igt_info("active1[%d]: %'"PRIu64"ns\n", pass, active);
		igt_assert(active > old); /* monotonic */
		assert_within_epsilon(active, delay, 20);

		old = active;
	}

	gem_quiescent_gpu(i915);

	/* And again now idle */
	idle = read_runtime(me, e->class);
	igt_info("idle: %'"PRIu64"ns\n", idle);
	igt_assert(idle >= active);

	gem_context_destroy(i915, spin->execbuf.rsvd1);

	/* And finally after the executing context is no more */
	old = read_runtime(me, e->class);
	igt_info("old: %'"PRIu64"ns\n", old);
	igt_assert_eq_u64(old, idle);

	/* Once more on the default context for good luck */
	igt_spin_reset(spin);
	spin->execbuf.rsvd1 = 0;
	gem_execbuf(i915, &spin->execbuf);
	igt_spin_busywait_until_started(spin);
	igt_nsec_elapsed(memset(&tv, 0, sizeof(tv)));

	for (int pass = 0; pass <= 10; pass++) {
		usleep(1000 >> pass);
		delay = igt_nsec_elapsed(&tv) + idle;
		igt_debug("delay: %'"PRIu64"ns\n", delay);

		/* Check that we accumulate the runtime, while active */
		active = read_runtime(me, e->class);
		igt_info("active0[%d]: %'"PRIu64"ns\n", pass, active);
		igt_assert(active > old); /* monotonic */
		assert_within_epsilon(active, delay, 20);

		old = active;
	}

	gem_quiescent_gpu(i915);
	igt_assert_eq(read_runtimes(me, other), 1);

	igt_spin_free(i915, spin);
	close(i915);
}

static void busy_all(int i915, int clients)
{
	const struct intel_execution_engine2 *e;
	int64_t active[MAX_CLASS];
	int64_t idle[MAX_CLASS];
	int64_t old[MAX_CLASS];
	uint64_t classes = 0;
	igt_spin_t *spin;
	int expect = 0;
	int64_t delay;
	int me;

	/* Create a fresh client with 0 runtime */
	i915 = reopen_client(i915);

	me = find_me(clients, getpid());
	igt_assert(me != -1);

	spin = igt_spin_new(i915,
			    gem_context_clone_with_engines(i915, 0),
			    .flags = IGT_SPIN_POLL_RUN);
	__for_each_physical_engine(i915, e) {
		spin->execbuf.flags &= ~63;
		spin->execbuf.flags |= e->flags;
		gem_execbuf(i915, &spin->execbuf);

		if (!(classes & (1ull << e->class)))
			expect++;
		classes |= 1ull << e->class;
	}
	igt_spin_busywait_until_started(spin);

	delay = -500000; /* 500us slack */
	memset(old, 0, sizeof(old));
	for (int pass = 0; pass < 5; pass++) {
		delay += measured_usleep(1000 >> pass);
		igt_debug("delay: %'"PRIu64"ns\n", delay);

		/* Check that we accumulate the runtime, while active */
		igt_assert_eq(read_runtimes(me, active), expect);
		for (int i = 0; i < ARRAY_SIZE(active); i++) {
			if (!active[i])
				continue;

			igt_info("active[%d]: %'"PRIu64"ns\n", i, active[i]);
			igt_assert(active[i] > old[i]); /* monotonic */
			igt_assert(active[i] > delay); /* within reason */
		}

		memcpy(old, active, sizeof(old));
	}

	gem_quiescent_gpu(i915);

	/* And again now idle */
	igt_assert_eq(read_runtimes(me, idle), expect);
	for (int i = 0; i < ARRAY_SIZE(idle); i++) {
		if (!idle[i])
			continue;

		igt_info("idle[%d]: %'"PRIu64"ns\n", i, idle[i]);
		igt_assert(idle[i] >= active[i]);
	}

	gem_context_destroy(i915, spin->execbuf.rsvd1);
	igt_spin_free(i915, spin);

	/* And finally after the executing context is no more */
	igt_assert_eq(read_runtimes(me, old), expect);
	for (int i = 0; i < ARRAY_SIZE(old); i++) {
		if (!old[i])
			continue;

		igt_info("old[%d]: %'"PRIu64"ns\n", i, old[i]);
		igt_assert_eq_u64(old[i], idle[i]);
	}

	close(i915);
}

static void
split_child(int i915, int clients,
	    const struct intel_execution_engine2 *e,
	    int sv)
{
	int64_t runtime[2] = {};
	igt_spin_t *spin;
	int go = 1;

	i915 = reopen_client(i915);

	spin = igt_spin_new(i915, .engine = e->flags);
	igt_spin_end(spin);
	gem_sync(i915, spin->handle);

	write(sv, &go, sizeof(go));
	read(sv, &go, sizeof(go));
	while (go) {
		struct timespec tv = {};

		igt_spin_reset(spin);
		gem_execbuf(i915, &spin->execbuf);
		igt_nsec_elapsed(&tv);
		read(sv, &go, sizeof(go));
		igt_spin_end(spin);
		runtime[1] += igt_nsec_elapsed(&tv);
		read(sv, &go, sizeof(go));
	}
	igt_spin_free(i915, spin);

	runtime[0] = read_runtime(find_me(clients, getpid()), e->class);
	write(sv, runtime, sizeof(runtime));
}

static void
split(int i915, int clients, const struct intel_execution_engine2 *e, int f)
{
	struct client {
		int64_t active[2];
		int sv[2];
		int f;
	} client[2];
	uint64_t total;
	int go = 1;
	int i;

	for (i = 0; i < ARRAY_SIZE(client); i++) {
		struct client *c = memset(&client[i], 0, sizeof(*c));

		c->f = f;
		f = 100 - f;

		igt_assert(socketpair(AF_UNIX, SOCK_DGRAM, 0, c->sv) == 0);
		igt_fork(child, 1)
			split_child(i915, clients, e, c->sv[1]);

		read(c->sv[0], &go, sizeof(go));
	}
	igt_assert(go == 1);

	i = 0;
	write(client[i].sv[0], &go, sizeof(go));
	igt_until_timeout(2) {
		measured_usleep(100 * client[i].f);
		write(client[!i].sv[0], &go, sizeof(go));
		write(client[i].sv[0], &go, sizeof(go));

		i = !i;
	}
	write(client[i].sv[0], &go, sizeof(go));

	go = 0;
	total = 1;
	for (i = 0; i < ARRAY_SIZE(client); i++) {
		struct client *c = &client[i];

		write(c->sv[0], &go, sizeof(go));
		igt_assert_eq(read(c->sv[0], c->active, sizeof(c->active)),
			      sizeof(c->active));

		total += c->active[1];
	}

	for (i = 0; i < ARRAY_SIZE(client); i++) {
		struct client *c = &client[i];

		igt_info("active[%d]: %'"PRIu64"ns (%'"PRIu64"ns), %.1f%%\n", i,
			 c->active[0], c->active[1],
			 c->active[0] * 100. / total);
		assert_within_epsilon(c->active[0], c->f * total / 100., 20);
		assert_within_epsilon(c->active[0], c->active[1], 10);
	}

	igt_waitchildren();
}

static void
sema_child(int i915, int clients,
	   const struct intel_execution_engine2 *e,
	   int sv)
{
	int64_t runtime[2] = {};
	struct drm_i915_gem_exec_object2 obj = {
		.flags = EXEC_OBJECT_SUPPORTS_48B_ADDRESS
	};
	struct drm_i915_gem_execbuffer2 execbuf = {
		.buffers_ptr = to_user_pointer(&obj),
		.buffer_count = 1,
		.flags = e->flags,
	};
	uint32_t *cs, *sema;

	i915 = reopen_client(i915);

	obj.handle = gem_create(i915, 4096);
	obj.offset = obj.handle << 12;
	sema = cs = gem_mmap__device_coherent(i915, obj.handle,
					      0, 4096, PROT_WRITE);

	*cs = MI_BATCH_BUFFER_END;
	gem_execbuf(i915, &execbuf);
	gem_sync(i915, obj.handle);
	obj.flags |= EXEC_OBJECT_PINNED;

	cs += 16;

	*cs++ = MI_ARB_CHECK;
	*cs++ = MI_SEMAPHORE_WAIT |
		MI_SEMAPHORE_POLL |
		MI_SEMAPHORE_SAD_NEQ_SDD |
		(4 - 2);
	*cs++ = 0;
	*cs++ = obj.offset;
	*cs++ = obj.offset >> 32;

	*cs++ = MI_BATCH_BUFFER_START | 1 << 8 | 1;
	*cs++ = obj.offset + 64;
	*cs++ = obj.offset >> 32;

	*sema = 0;
	gem_execbuf(i915, &execbuf);
	gem_close(i915, obj.handle);

	write(sv, sema, sizeof(*sema));
	read(sv, sema, sizeof(*sema));
	while (*sema != -1) {
		struct timespec tv = {};

		__sync_synchronize();
		igt_nsec_elapsed(&tv);
		read(sv, sema, sizeof(*sema));

		__sync_synchronize();
		runtime[1] += igt_nsec_elapsed(&tv);
		read(sv, sema, sizeof(*sema));
	}

	runtime[0] = read_runtime(find_me(clients, getpid()), e->class);
	write(sv, runtime, sizeof(runtime));

	sema[16] = MI_BATCH_BUFFER_END;
	__sync_synchronize();
}

static void
sema(int i915, int clients, const struct intel_execution_engine2 *e, int f)
{
	struct client {
		int64_t active[2];
		int sv[2];
		int f;
	} client[2];
	uint64_t total;
	int go, stop;
	int i;

	for (i = 0; i < ARRAY_SIZE(client); i++) {
		struct client *c = memset(&client[i], 0, sizeof(*c));

		c->f = f;
		f = 100 - f;

		igt_assert(socketpair(AF_UNIX, SOCK_DGRAM, 0, c->sv) == 0);
		igt_fork(child, 1)
			sema_child(i915, clients, e, c->sv[1]);

		read(c->sv[0], &go, sizeof(go));
	}

	i = 0;
	go = 1;
	stop = 0;
	write(client[i].sv[0], &go, sizeof(go));
	igt_until_timeout(2) {
		measured_usleep(100 * client[i].f);
		write(client[!i].sv[0], &go, sizeof(go));
		write(client[i].sv[0], &stop, sizeof(stop));

		i = !i;
	}
	write(client[i].sv[0], &stop, sizeof(stop));

	go = -1;
	total = 1;
	for (i = 0; i < ARRAY_SIZE(client); i++) {
		struct client *c = &client[i];

		write(c->sv[0], &go, sizeof(go));
		igt_assert_eq(read(c->sv[0], c->active, sizeof(c->active)),
			      sizeof(c->active));

		total += c->active[1];
	}

	for (i = 0; i < ARRAY_SIZE(client); i++) {
		struct client *c = &client[i];

		igt_info("active[%d]: %'"PRIu64"ns (%'"PRIu64"ns), %.1f%%\n", i,
			 c->active[0], c->active[1],
			 c->active[0] * 100. / total);
		assert_within_epsilon(c->active[0], c->f * total / 100., 20);
		assert_within_epsilon(c->active[0], c->active[1], 10);
	}

	igt_waitchildren();
}

static int read_all(int clients, pid_t pid, int class, uint64_t *runtime)
{
	struct dirent *de;
	char buf[280];
	int count = 0;
	DIR *dir;

	dir = fdopendir(dup(clients));
	igt_assert(dir);
	rewinddir(dir);

	while ((de = readdir(dir))) {
		int me;

		if (!isdigit(de->d_name[0]))
			continue;

		snprintf(buf, sizeof(buf), "%s/pid", de->d_name);
		igt_sysfs_read(clients, buf, buf, sizeof(buf));
		if (atoi(buf) != pid)
			continue;

		me = openat(clients, de->d_name, O_DIRECTORY | O_RDONLY);
		runtime[count++] = read_runtime(me, class);
		close(me);
	}

	closedir(dir);
	return count;
}

static int cmp_u64(const void *A, const void *B)
{
	const uint64_t *a = A, *b = B;

	if (*a < *b)
		return -1;
	else if (*a > *b)
		return 1;
	else
		return 0;
}

static void __fair(int i915, int clients,
		   int class, const char *name,
		   int extra, int duration)
{
	const double timeslice_duration_ns = 5e6;
	unsigned int count;
	uint64_t *runtime;
	double threshold;
	double expect;
	int i;

	i = 0;
	do {
		int client = gem_reopen_driver(i915);
		uint32_t ctx;

		ctx = gem_context_create_for_class(client, class, &count);
		__igt_spin_new(client, ctx);
	} while (++i < count + extra);
	extra = i;

	sleep(duration); /* over the course of many timeslices */

	runtime = calloc(extra, sizeof(*runtime));
	igt_assert_eq(read_all(clients, getpid(), class, runtime), extra);

	/*
	 * If we imagine that the timeslices are randomly distributed to
	 * the clients, we would expect the variance to be modelled
	 * by a drunken walk; ergo sqrt(num_timeslices).
	 */
	threshold = sqrt(1e9 * duration / timeslice_duration_ns * (extra - count));
	threshold *= timeslice_duration_ns;
	threshold *= 2; /* CI safety factor before crying wolf */
	threshold += 5e6; /* tolerance for 5ms measuring error */

	expect = 1e9 * count * duration / extra;

	qsort(runtime, extra, sizeof(*runtime), cmp_u64);
	igt_info("%s: [%.1f, %.1f, %.1f] ms, expect %1.f +- %.1fms\n",
		 name,
		 1e-6 * runtime[0],
		 1e-6 * runtime[extra / 2],
		 1e-6 * runtime[extra - 1],
		 1e-6 * expect,
		 1e-6 * threshold);

	assert_within_epsilon(runtime[extra / 2], expect, 20);
	igt_assert_f(runtime[extra - 1] - runtime[0] <= 2 * threshold,
		     "Range of timeslices greater than tolerable: %.2fms > %.2fms; unfair!\n",
		     1e-6 * (runtime[extra - 1] - runtime[0]),
		     1e-6 * threshold * 2);
}

static void fair(int i915, int clients, int extra, int duration)
{
	static const char *names[] = {
		[I915_ENGINE_CLASS_RENDER]	  = "rcs",
		[I915_ENGINE_CLASS_COPY]	  = "bcs",
		[I915_ENGINE_CLASS_VIDEO]	  = "vcs",
		[I915_ENGINE_CLASS_VIDEO_ENHANCE] = "vecs",
	};

	gem_quiescent_gpu(i915);

	for (int class = 0; class < ARRAY_SIZE(names); class++) {
		unsigned int count;
		uint32_t ctx;

		ctx = gem_context_create_for_class(i915, class, &count);
		if (!ctx)
			continue;
		gem_context_destroy(i915, ctx);

		igt_dynamic_f("%s", names[class]) {
			igt_drop_caches_set(i915, DROP_RESET_ACTIVE);
			igt_fork(child, 1)
				__fair(i915, clients, class, names[class],
				       extra, duration);
			igt_waitchildren();
			gem_quiescent_gpu(i915);
		}
		igt_drop_caches_set(i915, DROP_RESET_ACTIVE);
	}
}

static bool has_busy(int clients)
{
	bool ok;
	int me;

	me = find_me(clients, getpid());
	ok = faccessat(me, "busy", 0, F_OK) == 0;
	close(me);

	return ok;
}

static void test_busy(int i915, int clients)
{
	const struct intel_execution_engine2 *e;
	const int frac[] = { 10, 25, 50 };

	igt_fixture {
		igt_require(gem_has_contexts(i915));
		igt_require(has_busy(clients));
	}

	igt_subtest_with_dynamic("busy") {
		__for_each_physical_engine(i915, e) {
			igt_dynamic_f("%s", e->name) {
				gem_quiescent_gpu(i915);
				igt_fork(child, 1)
					busy_one(i915, clients, e);
				igt_waitchildren();
				gem_quiescent_gpu(i915);
			}
		}

		igt_dynamic("all") {
			gem_quiescent_gpu(i915);
			igt_fork(child, 1)
				busy_all(i915, clients);
			igt_waitchildren();
			gem_quiescent_gpu(i915);
		}
	}

	for (int i = 0; i < ARRAY_SIZE(frac); i++) {
		igt_subtest_with_dynamic_f("split-%d", frac[i]) {
			__for_each_physical_engine(i915, e) {
				igt_dynamic_f("%s", e->name) {
					gem_quiescent_gpu(i915);
					split(i915, clients, e, frac[i]);
					gem_quiescent_gpu(i915);
				}
			}
		}
	}

	igt_subtest_group {
		igt_fixture {
			//igt_require(gem_scheduler_has_timeslicing(i915));
			igt_require(gem_scheduler_has_preemption(i915));
		}

		for (int i = 0; i < ARRAY_SIZE(frac); i++) {
			igt_subtest_with_dynamic_f("sema-%d", frac[i]) {
				__for_each_physical_engine(i915, e) {
					if (!gem_class_has_mutable_submission(i915, e->class))
						continue;

					igt_dynamic_f("%s", e->name) {
						igt_drop_caches_set(i915, DROP_RESET_ACTIVE);
						sema(i915, clients, e, frac[i]);
						gem_quiescent_gpu(i915);
					}
					igt_drop_caches_set(i915, DROP_RESET_ACTIVE);
				}
			}
		}

		for (int i = 0; i < 4; i++) {
			igt_subtest_with_dynamic_f("fair-%d", (1 << i) - 1)
				fair(i915, clients, (1 << i) - 1, 5);
		}
	}
}

igt_main
{
	int i915 = -1, clients = -1;

	igt_fixture {
		int sys;

		/* Don't allow [too many] extra clients to be opened */
		i915 = __drm_open_driver(DRIVER_INTEL);
		igt_require_gem(i915);

		sys = igt_sysfs_open(i915);
		igt_require(sys != -1);

		clients = openat(sys, "clients", O_RDONLY);
		igt_require(clients != -1);

		close(sys);
	}

	igt_subtest("pidname")
		pidname(i915, clients);

	igt_subtest("create")
		create(i915, clients);

	igt_subtest_group
		test_busy(i915, clients);

	igt_fixture {
		close(clients);
		close(i915);
	}
}
