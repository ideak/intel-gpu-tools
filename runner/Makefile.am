
if BUILD_RUNNER

runnerlib = librunner.la
noinst_LTLIBRARIES = $(runnerlib)
librunner_la_SOURCES =	\
	settings.c	\
	job_list.c	\
	executor.c	\
	resultgen.c	\
	$(NULL)

bin_PROGRAMS =		\
	igt_runner	\
	igt_resume	\
	igt_results	\
	$(NULL)

LDADD = $(runnerlib) $(JSONC_LIBS) ../lib/libintel_tools.la

igt_runner_SOURCES = runner.c
igt_resume_SOURCES = resume.c
igt_results_SOURCES = results.c

AM_CFLAGS = $(JSONC_CFLAGS) \
	$(CWARNFLAGS) -Wno-unused-result $(DEBUG_CFLAGS) \
	-I$(srcdir)/.. \
	-I$(srcdir)/../lib \
	-D_GNU_SOURCE

endif
