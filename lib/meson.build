lib_headers = [
	'debug.h',
	'drmtest.h',
	'i830_reg.h',
	'i915/gem_context.h',
	'i915/gem_scheduler.h',
	'i915/gem_submission.h',
	'i915_3d.h',
	'i915_reg.h',
	'i915_pciids.h',
	'igt.h',
	'igt_debugfs.h',
	'igt_aux.h',
	'igt_edid_template.h',
	'igt_gt.h',
	'igt_gvt.h',
	'igt_primes.h',
	'igt_rand.h',
	'igt_rc.h',
	'igt_stats.h',
	'igt_syncobj.h',
	'igt_sysfs.h',
	'igt_x86.h',
	'igt_vgem.h',
	'instdone.h',
	'intel_batchbuffer.h',
	'intel_chipset.h',
	'intel_io.h',
	'intel_reg.h',
	'ioctl_wrappers.h',
	'media_fill.h',
	'media_spin.h',
	'gpgpu_fill.h',
	'gen7_media.h',
	'gen8_media.h',
	'gen6_render.h',
	'gen7_render.h',
	'gen8_render.h',
	'gen9_render.h',
	'rendercopy.h',
	'sw_sync.h',
	'igt_kms.h',
	'igt_fb.h',
	'igt_core.h',
	'igt_draw.h',
	'igt_pm.h',
	'igt_dummyload.h',
	'uwildmat/uwildmat.h',
	'igt_kmod.h',
	'igt_vc4.h',
]

lib_sources = [
	'drmtest.c',
	'i915/gem_context.c',
	'i915/gem_scheduler.c',
	'i915/gem_submission.c',
	'igt_debugfs.c',
	'igt_aux.c',
	'igt_gt.c',
	'igt_gvt.c',
	'igt_primes.c',
	'igt_rand.c',
	'igt_stats.c',
	'igt_syncobj.c',
	'igt_sysfs.c',
	'igt_vgem.c',
	'igt_x86.c',
	'instdone.c',
	'intel_batchbuffer.c',
	'intel_chipset.c',
	'intel_device_info.c',
	'intel_os.c',
	'intel_mmio.c',
	'ioctl_wrappers.c',
	'media_fill_gen7.c',
	'media_fill_gen8.c',
	'media_fill_gen8lp.c',
	'media_fill_gen9.c',
	'media_spin.c',
	'gpgpu_fill.c',
	'rendercopy_i915.c',
	'rendercopy_i830.c',
	'rendercopy_gen6.c',
	'rendercopy_gen7.c',
	'rendercopy_gen8.c',
	'rendercopy_gen9.c',
	'sw_sync.c',
	'intel_reg_map.c',
	'intel_iosf.c',
	'igt_kms.c',
	'igt_fb.c',
	'igt_core.c',
	'igt_draw.c',
	'igt_pm.c',
	'igt_dummyload.c',
	'uwildmat/uwildmat.c',
	'igt_kmod.c',
	'igt_vc4.c',
]

lib_deps = [
	cairo,
	glib,
	libdrm,
	libkmod,
	libprocps,
	libudev,
	libunwind,
	pciaccess,
	pthreads,
	math,
	realtime,
]

if libdrm_intel.found()
	lib_deps += libdrm_intel
else
	lib_headers += 'stubs/drm/intel_bufmgr.h'
	lib_sources += 'stubs/drm/intel_bufmgr.c'
endif

if valgrind.found()
	lib_deps += valgrind
endif

if gsl.found() and pixman.found()
	lib_deps += [ gsl, pixman ]
	lib_headers += [ 'igt_frame.h', 'igt_audio.h' ]
	lib_sources += [ 'igt_frame.c', 'igt_audio.c' ]
endif

if alsa.found()
	lib_deps += alsa
	lib_headers += 'igt_alsa.h'
	lib_sources += 'igt_alsa.c'
endif

if chamelium.found()
	lib_deps += chamelium
	lib_headers += 'igt_chamelium.h'
	lib_sources += 'igt_chamelium.c'
endif

install_headers(lib_headers)

pkgdatadir = join_paths(get_option('prefix'), get_option('datadir'), 'intel-gpu-tools')
srcdir = join_paths(meson.source_root(), 'tests')

lib_version = vcs_tag(input : 'version.h.in', output : 'version.h',
		      fallback : 'NO-GIT',
		      command : [ 'git', 'log', '-n1', '--pretty=format:g%h' ] )

lib_intermediates = []
foreach f: lib_sources
    name = f.underscorify(f)
    lib = static_library('igt-' + name,
	[ f, lib_version ],
	include_directories: inc,
	dependencies : lib_deps,
	c_args : [
	    '-DIGT_DATADIR="@0@"'.format(pkgdatadir),
	    '-DIGT_SRCDIR="@0@"'.format(srcdir),
	    '-DIGT_LOG_DOMAIN="@0@"'.format(f.split('.')[0]),
	])

    lib_intermediates += lib
endforeach

lib_igt_build = shared_library('igt',
    ['dummy.c'],
    link_whole: lib_intermediates,
    dependencies: lib_deps,
    install : true
)

lib_igt = declare_dependency(link_with : lib_igt_build,
			    include_directories : inc)

igt_deps = [ lib_igt ] + lib_deps

subdir('tests')
