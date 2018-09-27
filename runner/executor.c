#include <errno.h>
#include <fcntl.h>
#include <linux/watchdog.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/signalfd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "igt_core.h"
#include "executor.h"
#include "output_strings.h"

static struct {
	int *fds;
	size_t num_dogs;
} watchdogs;

static void close_watchdogs(struct settings *settings)
{
	size_t i;

	if (settings && settings->log_level >= LOG_LEVEL_VERBOSE)
		printf("Closing watchdogs\n");

	for (i = 0; i < watchdogs.num_dogs; i++) {
		write(watchdogs.fds[i], "V", 1);
		close(watchdogs.fds[i]);
	}
}

static void close_watchdogs_atexit(void)
{
	close_watchdogs(NULL);
}

static void init_watchdogs(struct settings *settings)
{
	int i;
	char name[32];
	int fd;

	memset(&watchdogs, 0, sizeof(watchdogs));

	if (!settings->use_watchdog || settings->inactivity_timeout <= 0)
		return;

	if (settings->log_level >= LOG_LEVEL_VERBOSE) {
		printf("Initializing watchdogs\n");
	}

	atexit(close_watchdogs_atexit);

	for (i = 0; ; i++) {
		snprintf(name, sizeof(name), "/dev/watchdog%d", i);
		if ((fd = open(name, O_RDWR | O_CLOEXEC)) < 0)
			break;

		watchdogs.num_dogs++;
		watchdogs.fds = realloc(watchdogs.fds, watchdogs.num_dogs * sizeof(int));
		watchdogs.fds[i] = fd;

		if (settings->log_level >= LOG_LEVEL_VERBOSE)
			printf(" %s\n", name);
	}
}

static int watchdogs_set_timeout(int timeout)
{
	size_t i;
	int orig_timeout = timeout;

	for (i = 0; i < watchdogs.num_dogs; i++) {
		if (ioctl(watchdogs.fds[i], WDIOC_SETTIMEOUT, &timeout)) {
			write(watchdogs.fds[i], "V", 1);
			close(watchdogs.fds[i]);
			watchdogs.fds[i] = -1;
			continue;
		}

		if (timeout < orig_timeout) {
			/*
			 * Timeout of this caliber refused. We want to
			 * use the same timeout for all devices.
			 */
			return watchdogs_set_timeout(timeout);
		}
	}

	return timeout;
}

static void ping_watchdogs(void)
{
	size_t i;

	for (i = 0; i < watchdogs.num_dogs; i++) {
		ioctl(watchdogs.fds[i], WDIOC_KEEPALIVE, 0);
	}
}

static void prune_subtest(struct job_list_entry *entry, char *subtest)
{
	char *excl;

	/*
	 * Subtest pruning is done by adding exclusion strings to the
	 * subtest list. The last matching item on the subtest
	 * selection command line flag decides whether to run a
	 * subtest, see igt_core.c for details.  If the list is empty,
	 * the expected subtest set is unknown, so we need to add '*'
	 * first so we can start excluding.
	 */

	if (entry->subtest_count == 0) {
		entry->subtest_count++;
		entry->subtests = realloc(entry->subtests, entry->subtest_count * sizeof(*entry->subtests));
		entry->subtests[0] = strdup("*");
	}

	excl = malloc(strlen(subtest) + 2);
	excl[0] = '!';
	strcpy(excl + 1, subtest);

	entry->subtest_count++;
	entry->subtests = realloc(entry->subtests, entry->subtest_count * sizeof(*entry->subtests));
	entry->subtests[entry->subtest_count - 1] = excl;
}

static bool prune_from_journal(struct job_list_entry *entry, int fd)
{
	char *subtest;
	FILE *f;
	bool any_pruned = false;

	/*
	 * Each journal line is a subtest that has been started, or
	 * the line 'exit:$exitcode (time)', or 'timeout:$exitcode (time)'.
	 */

	f = fdopen(fd, "r");
	if (!f)
		return false;

	while (fscanf(f, "%ms", &subtest) == 1) {
		if (!strncmp(subtest, EXECUTOR_EXIT, strlen(EXECUTOR_EXIT))) {
			/* Fully done. Mark that by making the binary name invalid. */
			fscanf(f, " (%*fs)");
			entry->binary[0] = '\0';
			free(subtest);
			continue;
		}

		if (!strncmp(subtest, EXECUTOR_TIMEOUT, strlen(EXECUTOR_TIMEOUT))) {
			fscanf(f, " (%*fs)");
			free(subtest);
			continue;
		}

		prune_subtest(entry, subtest);

		free(subtest);
		any_pruned = true;
	}

	fclose(f);
	return any_pruned;
}

static const char *filenames[_F_LAST] = {
	[_F_JOURNAL] = "journal.txt",
	[_F_OUT] = "out.txt",
	[_F_ERR] = "err.txt",
	[_F_DMESG] = "dmesg.txt",
};

static int open_at_end(int dirfd, const char *name)
{
	int fd = openat(dirfd, name, O_RDWR | O_CREAT | O_CLOEXEC, 0666);
	char last;

	if (fd >= 0) {
		if (lseek(fd, -1, SEEK_END) >= 0 &&
		    read(fd, &last, 1) == 1 &&
		    last != '\n') {
			write(fd, "\n", 1);
		}
		lseek(fd, 0, SEEK_END);
	}

	return fd;
}

static int open_for_reading(int dirfd, const char *name)
{
	return openat(dirfd, name, O_RDONLY);
}

bool open_output_files(int dirfd, int *fds, bool write)
{
	int i;
	int (*openfunc)(int, const char*) = write ? open_at_end : open_for_reading;

	for (i = 0; i < _F_LAST; i++) {
		if ((fds[i] = openfunc(dirfd, filenames[i])) < 0) {
			while (--i >= 0)
				close(fds[i]);
			return false;
		}
	}

	return true;
}

void close_outputs(int *fds)
{
	int i;

	for (i = 0; i < _F_LAST; i++) {
		close(fds[i]);
	}
}

static void dump_dmesg(int kmsgfd, int outfd)
{
	/*
	 * Write kernel messages to the log file until we reach
	 * 'now'. Unfortunately, /dev/kmsg doesn't support seeking to
	 * -1 from SEEK_END so we need to use a second fd to read a
	 * message to match against, or stop when we reach EAGAIN.
	 */

	int comparefd = open("/dev/kmsg", O_RDONLY | O_NONBLOCK);
	unsigned flags;
	unsigned long long seq, cmpseq, usec;
	char cont;
	char buf[2048];
	ssize_t r;

	if (comparefd < 0)
		return;
	lseek(comparefd, 0, SEEK_END);

	if (fcntl(kmsgfd, F_SETFL, O_NONBLOCK)) {
		close(comparefd);
		return;
	}

	while (1) {
		if (comparefd >= 0) {
			r = read(comparefd, buf, sizeof(buf) - 1);
			if (r < 0) {
				if (errno != EAGAIN && errno != EPIPE) {
					close(comparefd);
					return;
				}
			} else {
				buf[r] = '\0';
				if (sscanf(buf, "%u,%llu,%llu,%c;",
					   &flags, &cmpseq, &usec, &cont) == 4) {
					/* Reading comparison record done. */
					close(comparefd);
					comparefd = -1;
				}
			}
		}

		r = read(kmsgfd, buf, sizeof(buf));
		if (r <= 0) {
			if (errno == EPIPE)
				continue;

			/*
			 * If EAGAIN, we're done. If some other error,
			 * we can't do anything anyway.
			 */
			close(comparefd);
			return;
		}

		write(outfd, buf, r);

		if (comparefd < 0 && sscanf(buf, "%u,%llu,%llu,%c;",
					    &flags, &seq, &usec, &cont) == 4) {
			/*
			 * Comparison record has been read, compare
			 * the sequence number to see if we have read
			 * enough.
			 */
			if (seq >= cmpseq)
				return;
		}
	}
}

static bool kill_child(int sig, pid_t child)
{
	/*
	 * Send the signal to the child directly, and to the child's
	 * process group.
	 */
	kill(-child, sig);
	if (kill(child, sig) && errno == ESRCH) {
		fprintf(stderr, "Child process does not exist. This shouldn't happen.\n");
		return false;
	}

	return true;
}

/*
 * Returns:
 *  =0 - Success
 *  <0 - Failure executing
 *  >0 - Timeout happened, need to recreate from journal
 */
static int monitor_output(pid_t child,
			   int outfd, int errfd, int kmsgfd, int sigfd,
			   int *outputs,
			   struct settings *settings)
{
	fd_set set;
	char buf[2048];
	char *outbuf = NULL;
	size_t outbufsize = 0;
	char current_subtest[256] = {};
	struct signalfd_siginfo siginfo;
	ssize_t s;
	int n, status;
	int nfds = outfd;
	int timeout = settings->inactivity_timeout;
	int timeout_intervals = 1, intervals_left;
	int wd_extra = 10;
	int killed = 0; /* 0 if not killed, signal number otherwise */
	struct timespec time_beg, time_end;
	bool aborting = false;

	igt_gettime(&time_beg);

	if (errfd > nfds)
		nfds = errfd;
	if (kmsgfd > nfds)
		nfds = kmsgfd;
	if (sigfd > nfds)
		nfds = sigfd;
	nfds++;

	if (timeout > 0) {
		/*
		 * Use original timeout plus some leeway. If we're still
		 * alive, we want to kill the test process instead of cutting
		 * power.
		 */
		int wd_timeout = watchdogs_set_timeout(timeout + wd_extra);

		if (wd_timeout < timeout + wd_extra) {
			/* Watchdog timeout smaller, so ping it more often */
			if (wd_timeout - wd_extra < 0)
				wd_extra = wd_timeout / 2;
			timeout_intervals = timeout / (wd_timeout - wd_extra);
			intervals_left = timeout_intervals;
			timeout /= timeout_intervals;

			if (settings->log_level >= LOG_LEVEL_VERBOSE) {
				printf("Watchdog doesn't support the timeout we requested (shortened to %d seconds).\n"
				       "Using %d intervals of %d seconds.\n",
				       wd_timeout, timeout_intervals, timeout);
			}
		}
	}

	while (outfd >= 0 || errfd >= 0 || sigfd >= 0) {
		struct timeval tv = { .tv_sec = timeout };

		FD_ZERO(&set);
		if (outfd >= 0)
			FD_SET(outfd, &set);
		if (errfd >= 0)
			FD_SET(errfd, &set);
		if (kmsgfd >= 0)
			FD_SET(kmsgfd, &set);
		if (sigfd >= 0)
			FD_SET(sigfd, &set);

		n = select(nfds, &set, NULL, NULL, timeout == 0 ? NULL : &tv);
		if (n < 0) {
			/* TODO */
			return -1;
		}

		if (n == 0) {
			intervals_left--;
			if (intervals_left) {
				continue;
			}

			ping_watchdogs();

			switch (killed) {
			case 0:
				if (settings->log_level >= LOG_LEVEL_NORMAL) {
					printf("Timeout. Killing the current test with SIGTERM.\n");
				}

				killed = SIGTERM;
				if (!kill_child(killed, child))
					return -1;

				/*
				 * Now continue the loop and let the
				 * dying child be handled normally.
				 */
				timeout = 2; /* Timeout for waiting selected by fair dice roll. */
				watchdogs_set_timeout(20);
				intervals_left = timeout_intervals = 1;
				break;
			case SIGTERM:
				if (settings->log_level >= LOG_LEVEL_NORMAL) {
					printf("Timeout. Killing the current test with SIGKILL.\n");
				}

				killed = SIGKILL;
				if (!kill_child(killed, child))
					return -1;

				intervals_left = timeout_intervals = 1;
				break;
			case SIGKILL:
				/* Nothing that can be done, really. Let's tell the caller we want to abort. */
				if (settings->log_level >= LOG_LEVEL_NORMAL) {
					fprintf(stderr, "Child refuses to die. Aborting.\n");
				}
				close_watchdogs(settings);
				free(outbuf);
				close(outfd);
				close(errfd);
				close(kmsgfd);
				close(sigfd);
				return -1;
			}

			continue;
		}

		intervals_left = timeout_intervals;
		ping_watchdogs();

		/* TODO: Refactor these handlers to their own functions */
		if (outfd >= 0 && FD_ISSET(outfd, &set)) {
			char *newline;

			s = read(outfd, buf, sizeof(buf));
			if (s <= 0) {
				if (s < 0) {
					fprintf(stderr, "Error reading test's stdout: %s\n",
						strerror(errno));
				}

				close(outfd);
				outfd = -1;
				goto out_end;
			}

			write(outputs[_F_OUT], buf, s);
			if (settings->sync) {
				fdatasync(outputs[_F_OUT]);
			}

			outbuf = realloc(outbuf, outbufsize + s);
			memcpy(outbuf + outbufsize, buf, s);
			outbufsize += s;

			while ((newline = memchr(outbuf, '\n', outbufsize)) != NULL) {
				size_t linelen = newline - outbuf + 1;

				if (linelen > strlen(STARTING_SUBTEST) &&
				    !memcmp(outbuf, STARTING_SUBTEST, strlen(STARTING_SUBTEST))) {
					write(outputs[_F_JOURNAL], outbuf + strlen(STARTING_SUBTEST),
					      linelen - strlen(STARTING_SUBTEST));
					memcpy(current_subtest, outbuf + strlen(STARTING_SUBTEST),
					       linelen - strlen(STARTING_SUBTEST));
					current_subtest[linelen - strlen(STARTING_SUBTEST)] = '\0';

					if (settings->log_level >= LOG_LEVEL_VERBOSE) {
						fwrite(outbuf, 1, linelen, stdout);
					}
				}
				if (linelen > strlen(SUBTEST_RESULT) &&
				    !memcmp(outbuf, SUBTEST_RESULT, strlen(SUBTEST_RESULT))) {
					char *delim = memchr(outbuf, ':', linelen);

					if (delim != NULL) {
						size_t subtestlen = delim - outbuf - strlen(SUBTEST_RESULT);
						if (memcmp(current_subtest, outbuf + strlen(SUBTEST_RESULT),
							   subtestlen)) {
							/* Result for a test that didn't ever start */
							write(outputs[_F_JOURNAL],
							      outbuf + strlen(SUBTEST_RESULT),
							      subtestlen);
							write(outputs[_F_JOURNAL], "\n", 1);
							if (settings->sync) {
								fdatasync(outputs[_F_JOURNAL]);
							}
							current_subtest[0] = '\0';
						}

						if (settings->log_level >= LOG_LEVEL_VERBOSE) {
							fwrite(outbuf, 1, linelen, stdout);
						}
					}
				}

				memmove(outbuf, newline + 1, outbufsize - linelen);
				outbufsize -= linelen;
			}
		}
	out_end:

		if (errfd >= 0 && FD_ISSET(errfd, &set)) {
			s = read(errfd, buf, sizeof(buf));
			if (s <= 0) {
				if (s < 0) {
					fprintf(stderr, "Error reading test's stderr: %s\n",
						strerror(errno));
				}
				close(errfd);
				errfd = -1;
			} else {
				write(outputs[_F_ERR], buf, s);
				if (settings->sync) {
					fdatasync(outputs[_F_ERR]);
				}
			}
		}

		if (kmsgfd >= 0 && FD_ISSET(kmsgfd, &set)) {
			s = read(kmsgfd, buf, sizeof(buf));
			if (s < 0) {
				if (errno != EPIPE && errno != EINVAL) {
					fprintf(stderr, "Error reading from kmsg, stopping monitoring: %s\n",
						strerror(errno));
					close(kmsgfd);
					kmsgfd = -1;
				} else if (errno == EINVAL) {
					fprintf(stderr, "Warning: Buffer too small for kernel log record, record lost.\n");
				}
			} else {
				write(outputs[_F_DMESG], buf, s);
				if (settings->sync) {
					fdatasync(outputs[_F_DMESG]);
				}
			}
		}

		if (sigfd >= 0 && FD_ISSET(sigfd, &set)) {
			double time;

			s = read(sigfd, &siginfo, sizeof(siginfo));
			if (s < 0) {
				fprintf(stderr, "Error reading from signalfd: %s\n",
					strerror(errno));
				continue;
			} else if (siginfo.ssi_signo == SIGCHLD) {
				if (child != waitpid(child, &status, WNOHANG)) {
					fprintf(stderr, "Failed to reap child\n");
					status = 9999;
				} else if (WIFEXITED(status)) {
					status = WEXITSTATUS(status);
					if (status >= 128) {
						status = 128 - status;
					}
				} else if (WIFSIGNALED(status)) {
					status = -WTERMSIG(status);
				} else {
					status = 9999;
				}
			} else {
				/* We're dying, so we're taking them with us */
				if (settings->log_level >= LOG_LEVEL_NORMAL)
					printf("Abort requested, terminating children\n");

				aborting = true;
				timeout = 2;
				killed = SIGTERM;
				if (!kill_child(killed, child))
					return -1;

				continue;
			}

			igt_gettime(&time_end);

			time = igt_time_elapsed(&time_beg, &time_end);
			if (time < 0.0)
				time = 0.0;

			if (!aborting) {
				dprintf(outputs[_F_JOURNAL], "%s%d (%.3fs)\n",
					killed ? EXECUTOR_TIMEOUT : EXECUTOR_EXIT,
					status, time);
				if (settings->sync) {
					fdatasync(outputs[_F_JOURNAL]);
				}
			}

			close(sigfd);
			sigfd = -1;
			child = 0;
		}
	}

	dump_dmesg(kmsgfd, outputs[_F_DMESG]);
	if (settings->sync)
		fdatasync(outputs[_F_DMESG]);

	free(outbuf);
	close(outfd);
	close(errfd);
	close(kmsgfd);
	close(sigfd);

	if (aborting)
		return -1;

	return killed;
}

static void execute_test_process(int outfd, int errfd,
				 struct settings *settings,
				 struct job_list_entry *entry)
{
	char *argv[4] = {};
	size_t rootlen;

	dup2(outfd, STDOUT_FILENO);
	dup2(errfd, STDERR_FILENO);

	setpgid(0, 0);

	rootlen = strlen(settings->test_root);
	argv[0] = malloc(rootlen + strlen(entry->binary) + 2);
	strcpy(argv[0], settings->test_root);
	argv[0][rootlen] = '/';
	strcpy(argv[0] + rootlen + 1, entry->binary);

	if (entry->subtest_count) {
		size_t argsize;
		size_t i;

		argv[1] = strdup("--run-subtest");
		argsize = strlen(entry->subtests[0]);
		argv[2] = malloc(argsize + 1);
		strcpy(argv[2], entry->subtests[0]);

		for (i = 1; i < entry->subtest_count; i++) {
			char *sub = entry->subtests[i];
			size_t sublen = strlen(sub);

			argv[2] = realloc(argv[2], argsize + sublen + 2);
			argv[2][argsize] = ',';
			strcpy(argv[2] + argsize + 1, sub);
			argsize += sublen + 1;
		}
	}

	execv(argv[0], argv);
	fprintf(stderr, "Cannot execute %s\n", argv[0]);
	exit(IGT_EXIT_INVALID);
}

static int digits(size_t num)
{
	int ret = 0;
	while (num) {
		num /= 10;
		ret++;
	}

	if (ret == 0) ret++;
	return ret;
}

/*
 * Returns:
 *  =0 - Success
 *  <0 - Failure executing
 *  >0 - Timeout happened, need to recreate from journal
 */
static int execute_entry(size_t idx,
			  size_t total,
			  struct settings *settings,
			  struct job_list_entry *entry,
			  int testdirfd, int resdirfd)
{
	int dirfd;
	int outputs[_F_LAST];
	int kmsgfd;
	int sigfd;
	sigset_t mask;
	int outpipe[2] = { -1, -1 };
	int errpipe[2] = { -1, -1 };
	char name[32];
	pid_t child;
	int result;

	snprintf(name, sizeof(name), "%zd", idx);
	mkdirat(resdirfd, name, 0777);
	if ((dirfd = openat(resdirfd, name, O_DIRECTORY | O_RDONLY | O_CLOEXEC)) < 0) {
		fprintf(stderr, "Error accessing individual test result directory\n");
		return -1;
	}

	if (!open_output_files(dirfd, outputs, true)) {
		close(dirfd);
		fprintf(stderr, "Error opening output files\n");
		return -1;
	}

	if (settings->sync) {
		fsync(dirfd);
		fsync(resdirfd);
	}

	if (pipe(outpipe) || pipe(errpipe)) {
		close_outputs(outputs);
		close(dirfd);
		close(outpipe[0]);
		close(outpipe[1]);
		close(errpipe[0]);
		close(errpipe[1]);
		fprintf(stderr, "Error creating pipes: %s\n", strerror(errno));
		return -1;
	}

	if ((kmsgfd = open("/dev/kmsg", O_RDONLY | O_CLOEXEC)) < 0) {
		fprintf(stderr, "Warning: Cannot open /dev/kmsg\n");
	} else {
		/* TODO: Checking of abort conditions in pre-execute dmesg */
		lseek(kmsgfd, 0, SEEK_END);
	}

	sigemptyset(&mask);
	sigaddset(&mask, SIGCHLD);
	sigaddset(&mask, SIGINT);
	sigaddset(&mask, SIGTERM);
	sigaddset(&mask, SIGQUIT);
	sigprocmask(SIG_BLOCK, &mask, NULL);
	sigfd = signalfd(-1, &mask, O_CLOEXEC);

	if (sigfd < 0) {
		/* TODO: Handle better */
		fprintf(stderr, "Cannot monitor child process with signalfd\n");
		close(outpipe[0]);
		close(errpipe[0]);
		close(outpipe[1]);
		close(errpipe[1]);
		close(kmsgfd);
		close_outputs(outputs);
		close(dirfd);
		return -1;
	}

	if (settings->log_level >= LOG_LEVEL_NORMAL) {
		int width = digits(total);
		printf("[%0*zd/%0*zd] %s", width, idx + 1, width, total, entry->binary);
		if (entry->subtest_count > 0) {
			size_t i;
			const char *delim = "";

			printf(" (");
			for (i = 0; i < entry->subtest_count; i++) {
				printf("%s%s", delim, entry->subtests[i]);
				delim = ", ";
			}
			printf(")");
		}
		printf("\n");
	}

	/*
	 * Flush outputs before forking so our (buffered) output won't
	 * end up in the test outputs.
	 */

	fflush(stdout);
	fflush(stderr);

	if ((child = fork())) {
		int outfd = outpipe[0];
		int errfd = errpipe[0];
		close(outpipe[1]);
		close(errpipe[1]);

		result = monitor_output(child, outfd, errfd, kmsgfd, sigfd,
					outputs, settings);
	} else {
		int outfd = outpipe[1];
		int errfd = errpipe[1];
		close(outpipe[0]);
		close(errpipe[0]);

		sigprocmask(SIG_UNBLOCK, &mask, NULL);

		setenv("IGT_SENTINEL_ON_STDERR", "1", 1);

		execute_test_process(outfd, errfd, settings, entry);
	}

	/* TODO: Refactor this whole function to use onion teardown */
	close(outpipe[1]);
	close(errpipe[1]);
	close(kmsgfd);
	close_outputs(outputs);
	close(dirfd);

	return result;
}

static int remove_file(int dirfd, const char *name)
{
	return unlinkat(dirfd, name, 0) && errno != ENOENT;
}

static bool clear_test_result_directory(int dirfd)
{
	int i;

	for (i = 0; i < _F_LAST; i++) {
		if (remove_file(dirfd, filenames[i])) {
			fprintf(stderr, "Error deleting %s from test result directory: %s\n",
				filenames[i],
				strerror(errno));
			return false;
		}
	}

	return true;
}

static bool clear_old_results(char *path)
{
	int dirfd;
	size_t i;

	if ((dirfd = open(path, O_DIRECTORY | O_RDONLY)) < 0) {
		if (errno == ENOENT) {
			/* Successfully cleared if it doesn't even exist */
			return true;
		}

		fprintf(stderr, "Error clearing old results: %s\n", strerror(errno));
		return false;
	}

	if (unlinkat(dirfd, "uname.txt", 0) && errno != ENOENT) {
		close(dirfd);
		fprintf(stderr, "Error clearing old results: %s\n", strerror(errno));
		return false;
	}

	for (i = 0; true; i++) {
		char name[32];
		int resdirfd;

		snprintf(name, sizeof(name), "%zd", i);
		if ((resdirfd = openat(dirfd, name, O_DIRECTORY | O_RDONLY)) < 0)
			break;

		if (!clear_test_result_directory(resdirfd)) {
			close(resdirfd);
			close(dirfd);
			return false;
		}
		close(resdirfd);
		if (unlinkat(dirfd, name, AT_REMOVEDIR)) {
			fprintf(stderr,
				"Warning: Result directory %s contains extra files\n",
				name);
		}
	}

	close(dirfd);

	return true;
}

bool initialize_execute_state_from_resume(int dirfd,
					  struct execute_state *state,
					  struct settings *settings,
					  struct job_list *list)
{
	struct job_list_entry *entry;
	int resdirfd, fd, i;

	free_settings(settings);
	free_job_list(list);
	memset(state, 0, sizeof(*state));

	if (!read_settings(settings, dirfd) ||
	    !read_job_list(list, dirfd)) {
		close(dirfd);
		return false;
	}

	for (i = list->size; i >= 0; i--) {
		char name[32];

		snprintf(name, sizeof(name), "%d", i);
		if ((resdirfd = openat(dirfd, name, O_DIRECTORY | O_RDONLY)) >= 0)
			break;
	}

	if (i < 0)
		/* Nothing has been executed yet, state is fine as is */
		goto success;

	entry = &list->entries[i];
	state->next = i;
	if ((fd = openat(resdirfd, filenames[_F_JOURNAL], O_RDONLY)) >= 0) {
		if (!prune_from_journal(entry, fd)) {
			/*
			 * The test does not have subtests, or
			 * incompleted before the first subtest
			 * began. Either way, not suitable to
			 * re-run.
			 */
			state->next = i + 1;
		} else if (entry->binary[0] == '\0') {
			/* This test is fully completed */
			state->next = i + 1;
		}

		close(fd);
	}

 success:
	close(resdirfd);
	close(dirfd);

	return true;
}

bool initialize_execute_state(struct execute_state *state,
			      struct settings *settings,
			      struct job_list *job_list)
{
	memset(state, 0, sizeof(*state));

	if (!validate_settings(settings))
		return false;

	if (!serialize_settings(settings) ||
	    !serialize_job_list(job_list, settings))
		return false;

	if (settings->overwrite &&
	    !clear_old_results(settings->results_path))
		return false;

	return true;
}

bool execute(struct execute_state *state,
	     struct settings *settings,
	     struct job_list *job_list)
{
	struct utsname unamebuf;
	int resdirfd, testdirfd, unamefd;

	if ((resdirfd = open(settings->results_path, O_DIRECTORY | O_RDONLY)) < 0) {
		/* Initialize state should have done this */
		fprintf(stderr, "Error: Failure opening results path %s\n",
			settings->results_path);
		return false;
	}

	if ((testdirfd = open(settings->test_root, O_DIRECTORY | O_RDONLY)) < 0) {
		fprintf(stderr, "Error: Failure opening test root %s\n",
			settings->test_root);
		close(resdirfd);
		return false;
	}

	/* TODO: On resume, don't rewrite, verify that content matches current instead */
	if ((unamefd = openat(resdirfd, "uname.txt", O_CREAT | O_WRONLY | O_TRUNC, 0666)) < 0) {
		fprintf(stderr, "Error: Failure creating opening uname.txt: %s\n",
			strerror(errno));
		close(testdirfd);
		close(resdirfd);
		return false;
	}

	init_watchdogs(settings);

	if (!uname(&unamebuf)) {
		dprintf(unamefd, "%s %s %s %s %s\n",
			unamebuf.sysname,
			unamebuf.nodename,
			unamebuf.release,
			unamebuf.version,
			unamebuf.machine);
	} else {
		dprintf(unamefd, "uname() failed\n");
	}
	close(unamefd);

	for (; state->next < job_list->size;
	     state->next++) {
		int result = execute_entry(state->next,
					   job_list->size,
					   settings,
					   &job_list->entries[state->next],
					   testdirfd, resdirfd);
		if (result != 0) {
			close(testdirfd);
			close_watchdogs(settings);
			if (result > 0) {
				initialize_execute_state_from_resume(resdirfd, state, settings, job_list);
				return execute(state, settings, job_list);
			}
			close(resdirfd);
			return false;
		}
	}

	close(testdirfd);
	close(resdirfd);
	close_watchdogs(settings);
	return true;
}
