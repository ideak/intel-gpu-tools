bin_PROGRAMS = i915-perf-configs

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
