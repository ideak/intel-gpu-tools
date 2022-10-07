/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2022, Jake Freeland <jfree@FreeBSD.org>
 */

#if !defined(IGT_FREEBSD_H)

#if !defined(__FreeBSD__)
#error "This header is only for FreeBSD platform."
#endif

#define IGT_FREEBSD_H

#include <sys/consio.h>
#include <sys/endian.h>
#include <sys/errno.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/sched.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/watchdog.h>

#include <libgen.h>
#include <limits.h>
#include <pthread.h>
#include <setjmp.h>
#include <signal.h>
#include <unistd.h>

/*
 * Proper substitutions:
 * The following macros replace Linux-specific functions
 * and macros with their FreeBSD equivalents.
 */

typedef	int32_t		__s32;
typedef	uint32_t	__u32;
typedef	uint64_t	__u64;

typedef	ino_t		ino64_t;
typedef	sig_t		sighandler_t;

#define	jmp_buf	sigjmp_buf

#define	PTRACE_TRACEME  PT_TRACE_ME
#define	PTRACE_ATTACH   PT_ATTACH
#define	PTRACE_PEEKDATA PT_READ_D
#define	PTRACE_POKEDATA PT_WRITE_D
#define	PTRACE_DETACH   PT_DETACH

#define	I2C_RDWR		I2CRDWR
#define	I2C_M_RD		IIC_M_RD
#define	i2c_msg			iic_msg
#define	i2c_rdwr_ioctl_data	iic_rdwr_data

#define	bswap_32(x)	bswap32(x)

#define	_IOC_TYPE(nr)	(((nr) >> 8) & 255)

#define	SYS_getdents64	SYS_freebsd11_getdents

#define	mount(src, dest, fstype, flags, data)	\
	mount(fstype, dest, flags, data)

/*
 * Improper substitutions:
 * The following macros are temporary replacements for functions
 * and macros that exist on Linux and do not exist on FreeBSD.
 */

#define	ETIME	ETIMEDOUT

#define	MAP_POPULATE	MAP_PREFAULT_READ

#define	MADV_HUGEPAGE	MADV_SEQUENTIAL
#define	MADV_DONTFORK	MADV_NOSYNC

#define	WDIOC_KEEPALIVE	WDIOCPATPAT

#define	SCHED_RESET_ON_FORK	0
#define	SCHED_IDLE	SCHED_OTHER

#define	gettid()	getpid()

#define	pthread_sigqueue(pid, signo, value)	\
	sigqueue(pid, signo, value)

#define	signalfd(fd, mask, flags)	-ENOSYS
#define	timerfd_create(c, f)		-ENOSYS
#define	timerfd_settime(fd, f, n, o)	-ENOSYS

/*
 * Macro conflict resolution.
 */

#undef	ALIGN
#undef	PAGE_SIZE

/*
 * Missing Linux structures.
 */

struct signalfd_siginfo {
	uint32_t ssi_signo;
	uint32_t ssi_pid;
};

struct kmod_module {
	size_t size;
};

typedef struct {
	char state;
} proc_t;

#endif /* IGT_FREEBSD_H */
