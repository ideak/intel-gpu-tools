/*
 * Copyright © 2014, 2015 Intel Corporation
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
 *    Daniel Vetter <daniel.vetter@ffwll.ch>
 *
 */

#ifndef IGT_AUX_H
#define IGT_AUX_H

#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#if defined(__linux__) || defined(__FreeBSD__)
# include <sys/syscall.h>
#endif

#include <i915/gem_submission.h>

#include "igt_core.h"
#include "igt_os.h"

/* signal interrupt helpers */
#ifdef __linux__
# ifndef HAVE_GETTID
#  define gettid() (pid_t)(syscall(__NR_gettid))
# endif
# define sigev_notify_thread_id _sigev_un._tid
#endif

/* auxialiary igt helpers from igt_aux.c */
/* generally useful helpers */
void igt_fork_signal_helper(void);
void igt_stop_signal_helper(void);
void igt_suspend_signal_helper(void);
void igt_resume_signal_helper(void);

void igt_fork_shrink_helper(int fd);
void igt_stop_shrink_helper(void);

void igt_fork_hang_detector(int fd);
void igt_stop_hang_detector(void);

struct __igt_sigiter {
	unsigned pass;
};

bool __igt_sigiter_continue(struct __igt_sigiter *iter, bool interrupt);

/**
 * igt_while_interruptible:
 * @enable: enable igt_ioctl interrupting or not
 *
 * Provides control flow such that all drmIoctl() (strictly igt_ioctl())
 * within the loop are forcibly injected with signals (SIGRTMIN).
 *
 * This is useful to exercise ioctl error paths, at least where those can be
 * exercises by interrupting blocking waits, like stalling for the gpu.
 *
 * The code block attached to this macro is run in a loop with doubling the
 * interrupt timeout on each ioctl for every run, until no ioctl gets
 * interrupted any more. The starting timeout is taken to be the signal delivery
 * latency, measured at runtime. This way the any ioctls called from this code
 * block should be exhaustively tested for all signal interruption paths.
 *
 * Note that since this overloads the igt_ioctl(), this method is not useful
 * for widespread signal injection, for example providing coverage of
 * pagefaults. To interrupt everything, see igt_fork_signal_helper().
 */
#define igt_while_interruptible(enable) \
	for (struct __igt_sigiter iter__={}; __igt_sigiter_continue(&iter__, (enable)); )

/**
 * igt_until_timeout:
 * @timeout: timeout in seconds
 *
 * Convenience macro loop to run the provided code block in a loop until the
 * timeout has expired. Of course when an individual execution takes too long,
 * the actual execution time could be a lot longer.
 *
 * The code block will be executed at least once.
 */
#define igt_until_timeout(timeout) \
	for (struct timespec t__={}; igt_seconds_elapsed(&t__) < (timeout); )

/**
 * igt_for_milliseconds:
 * @time: how long to run the loop in milliseconds
 *
 * Convenience macro loop to run the provided code block in a loop until the
 * target interval has expired. Of course when an individual execution takes
 * too long, the actual execution time could be a lot longer.
 *
 * The code block will be executed at least once.
 */
#define igt_for_milliseconds(t) \
	for (struct timespec t__={}; igt_nsec_elapsed(&t__)>>20 < (t); )

void igt_exchange_int(void *array, unsigned i, unsigned j);
void igt_exchange_int64(void *array, unsigned i, unsigned j);
void igt_permute_array(void *array, unsigned size,
			   void (*exchange_func)(void *array,
						 unsigned i,
						 unsigned j));
void igt_progress(const char *header, uint64_t i, uint64_t total);
void igt_print_activity(void);
bool igt_check_boolean_env_var(const char *env_var, bool default_value);

bool igt_aub_dump_enabled(void);

/* suspend/hibernate and auto-resume system */

/**
 *  igt_suspend_state:
 *  @SUSPEND_STATE_FREEZE: Suspend-To-Idle target state, aka S0ix or freeze,
 *			   first non-hibernation state
 *  @SUSPEND_STATE_STANDBY: "Power-On Suspend" target state, aka S1, second
 *			    non-hibernation state
 *  @SUSPEND_STATE_S3: Suspend-To-RAM: It enforces a "deep" state to mem_sleep,
 *		       what forces the system to go to the third
 *		       non-hibernation state, aka S3.
 *  @SUSPEND_STATE_MEM: A memory sleep (non-hibernation) target state,
 *			respecting the system's mem_sleep default:
 *				s2idle: Suspend-To-Idle target state
 *				shallow: "Power-On Suspend"
 *				deep: Suspend-To-RAM
 *  @SUSPEND_STATE_DISK: Suspend-To-Disk target state, aka S4 or hibernation
 *
 *  Target suspend states used with igt_system_suspend_autoresume().
 *  See /sys/power/state for the available states on a given machine.
 */
enum igt_suspend_state {
	SUSPEND_STATE_FREEZE,
	SUSPEND_STATE_STANDBY,
	SUSPEND_STATE_S3, /* Forces Suspend-to-Ram (S3) */
	SUSPEND_STATE_MEM, /* Respects system default */
	SUSPEND_STATE_DISK,

	/*< private >*/
	SUSPEND_STATE_NUM,
};

/**
 * igt_suspend_test:
 * @SUSPEND_TEST_NONE: no testing, perform a full suspend/resume cycle
 * @SUSPEND_TEST_FREEZER: complete cycle after freezing all freezable threads
 * @SUSPEND_TEST_DEVICES: complete cycle after the above step and suspending
 *			  devices (before calling the drivers' suspend late and
 *			  no_irq hooks). Platform and system devices are not
 *			  suspended here, see #SUSPEND_TEST_CORE.
 * @SUSPEND_TEST_PLATFORM: complete cycle after all the above steps and calling
 *			   the ACPI platform global control methods (applies
 *			   only with /sys/power/disk set to platform)
 * @SUSPEND_TEST_PROCESSORS: complete cycle after all the above steps and
 *			     disabling non-boot CPUs
 * @SUSPEND_TEST_CORE: complete cycle after all the above steps and suspending
 *		       platform and system devices
 *
 * Test points used with igt_system_suspend_autoresume(). Specifies if and where
 * the suspend sequence is to be terminated.
 */
enum igt_suspend_test {
	SUSPEND_TEST_NONE,
	SUSPEND_TEST_FREEZER,
	SUSPEND_TEST_DEVICES,
	SUSPEND_TEST_PLATFORM,
	SUSPEND_TEST_PROCESSORS,
	SUSPEND_TEST_CORE,

	/*< private >*/
	SUSPEND_TEST_NUM,
};

/**
 * igt_mem_sleep:
 * @MEM_SLEEP_NONE: no support
 * @MEM_SLEEP_S2IDLE: suspend-to-idle target state, aka S0ix or freeze,
 * @MEM_SLEEP_SHALLOW: standby target state, aka S1
 * @MEM_SLEEP_DEEP: suspend-to-mem target state aka S3
 */
enum igt_mem_sleep {
	MEM_SLEEP_NONE,
	MEM_SLEEP_S2IDLE,
	MEM_SLEEP_SHALLOW,
	MEM_SLEEP_DEEP,

	/*<private>*/
	MEM_SLEEP_NUM,
};

void igt_system_suspend_autoresume(enum igt_suspend_state state,
				   enum igt_suspend_test test);
void igt_set_autoresume_delay(int delay_secs);
int igt_get_autoresume_delay(enum igt_suspend_state state);

/* dropping priviledges */
void igt_drop_root(void);

void igt_debug_wait_for_keypress(const char *var);
void igt_debug_interactive_mode_check(const char *var, const char *expected);

#define __typecheck(x, y) \
        (!!(sizeof((typeof(x) *)1 == (typeof(y) *)1)))

#define __cmp(x, y, op) ((x) op (y) ? (x) : (y))

#define __cmp_once(x, y, unique_x, unique_y, op) ({     \
	typeof(x) unique_x = (x);               \
	typeof(y) unique_y = (y);               \
	__cmp(unique_x, unique_y, op);		\
})

#define __is_constexpr(x) \
	(sizeof(int) == sizeof(*(8 ? ((void *)((long)(x) * 0l)) : (int *)8)))

#define __no_side_effects(x, y) \
	(__is_constexpr(x) && __is_constexpr(y))

#define __safe_cmp(x, y) \
	(__typecheck(x, y) && __no_side_effects(x, y))

#define __careful_cmp(x, y, op, prefix) \
	__builtin_choose_expr(__safe_cmp(x, y), \
			      __cmp(x, y, op), \
			      __cmp_once(x, y, igt_unique(igt_tokencat(prefix, __x)), igt_unique(igt_tokencat(prefix, __y)), op))

#define min(x, y)	__careful_cmp(x, y, <, min)
#define max(x, y)	__careful_cmp(x, y, >, max)

#define clamp(val, lo, hi) min(max(val, lo), hi)

#define min_t(t, x, y)	__careful_cmp((typeof(t))x, (typeof(t))y, <, min_t)
#define max_t(t, x, y)	__careful_cmp((typeof(t))x, (typeof(t))y, >, max_t)

#define igt_swap(a, b) do {	\
	typeof(a) _tmp = (a);	\
	_Static_assert(__typecheck(a, b), "type mismatch for swap"); \
	(a) = (b);		\
	(b) = _tmp;		\
} while (0)

void igt_lock_mem(size_t size);
void igt_unlock_mem(void);

/**
 * igt_wait:
 * @COND: condition to wait
 * @timeout_ms: timeout in milliseconds
 * @interval_ms: amount of time we try to sleep between COND checks
 *
 * Waits until COND evaluates to true or the timeout passes.
 *
 * It is safe to call this macro if the signal helper is active. The only
 * problem is that the usleep() calls will return early, making us evaluate COND
 * too often, possibly eating valuable CPU cycles.
 *
 * Returns:
 * True of COND evaluated to true, false otherwise.
 */
#define igt_wait(COND, timeout_ms, interval_ms) ({			\
	const unsigned long interval_us__ = 1000 * (interval_ms);	\
	const unsigned long timeout_ms__ = (timeout_ms);		\
	struct timespec tv__ = {};					\
	bool ret__;							\
									\
	do {								\
		uint64_t elapsed__ = igt_nsec_elapsed(&tv__) >> 20;	\
									\
		if (COND) {						\
			igt_debug("%s took %"PRIu64"ms\n", #COND, elapsed__); \
			ret__ = true;					\
			break;						\
		}							\
		if (elapsed__ > timeout_ms__) {				\
			ret__ = false;					\
			break;						\
		}							\
									\
		usleep(interval_us__);					\
	} while (1);							\
									\
	ret__;								\
})

struct igt_mean;
void igt_start_siglatency(int sig); /* 0 => SIGRTMIN (default) */
double igt_stop_siglatency(struct igt_mean *result);

bool igt_allow_unlimited_files(void);

int igt_is_process_running(const char *comm);
int igt_terminate_process(int sig, const char *comm);
void igt_lsof(const char *dpath);
int igt_lsof_kill_audio_processes(void);
int pipewire_pulse_start_reserve(void);
void pipewire_pulse_stop_reserve(void);

#define igt_hweight(x) \
	__builtin_choose_expr(sizeof(x) == 8, \
			      __builtin_popcountll(x), \
			      __builtin_popcount(x))

#define is_power_of_two(x)  (((x) & ((x)-1)) == 0)

#define igt_fls(x) ((x) ? __builtin_choose_expr(sizeof(x) == 8, \
						64 - __builtin_clzll(x), \
						32 - __builtin_clz(x)) : 0)

#define roundup_power_of_two(x) ((x) != 0 ? 1 << igt_fls((x) - 1) : 0)

#define DIV_ROUND_UP(n, d) (((n) + (d) - 1) / (d))

uint64_t vfs_file_max(void);

void *igt_memdup(const void *ptr, size_t len);

#endif /* IGT_AUX_H */
