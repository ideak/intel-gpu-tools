bin_PROGRAMS =			\
	i915-perf-configs	\
	i915-perf-control	\
	i915-perf-recorder	\
	i915-perf-reader

AM_CFLAGS = $(CWARNFLAGS) -Wno-unused-result $(DEBUG_CFLAGS)\
	-I$(top_srcdir)/include/drm-uapi \
	-I$(top_srcdir)/lib \
	-D_GNU_SOURCE \
	$(DRM_CFLAGS) $(WERROR_CFLAGS) \
	$(NULL)

LDADD = $(top_builddir)/lib/libintel_tools.la

i915_perf_configs_SOURCES = i915_perf_configs.c
i915_perf_configs_CFLAGS = $(AM_CFLAGS) -I$(top_srcdir)/lib/i915
i915_perf_configs_LDADD = $(LDADD) $(top_builddir)/lib/libi915_perf.la

i915_perf_control_SOURCES = i915_perf_control.c
i915_perf_control_CFLAGS = $(AM_CFLAGS)

i915_perf_recorder_SOURCES = i915_perf_recorder.c
i915_perf_recorder_CFLAGS = $(AM_CFLAGS) -I$(top_srcdir)/lib/i915
i915_perf_recorder_LDADD = $(LDADD) $(top_builddir)/lib/libi915_perf.la

i915_perf_reader_SOURCES = i915_perf_reader.c
i915_perf_reader_CFLAGS = $(AM_CFLAGS) -I$(top_srcdir)/lib/i915
i915_perf_reader_LDADD = ../../lib/libintel_tools.la $(top_builddir)/lib/libi915_perf.la
