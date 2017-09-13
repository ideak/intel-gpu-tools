gpu_overlay_src = [
	'chart.c',
	'config.c',
	'cpu-top.c',
	'debugfs.c',
	'gem-interrupts.c',
	'gem-objects.c',
	'gpu-top.c',
	'gpu-perf.c',
	'gpu-freq.c',
	'igfx.c',
	'overlay.c',
	'power.c',
	'rc6.c',
]

xv = dependency('xv', required : false)
x11 = dependency('x11', required : false)
xext = dependency('xext', required : false)
dri2proto = dependency('dri2proto', version : '>= 2.6', required : false)
cairo_xlib = dependency('cairo-xlib', required : false)
xrandr = dependency('xrandr', version : '>=1.3', required : false)

gpu_overlay_deps = [ realtime, cairo, pciaccess, libdrm, libdrm_intel ]

both_x11_src = ''

gpu_overlay_cflags = []
if xv.found() and x11.found() and xext.found() and dri2proto.found()
	both_x11_src = 'x11/position.c'
	gpu_overlay_src += [
		'x11/dri2.c',
		'x11/dri2.h',
		'x11/rgb2yuv.c',
		'x11/rgb2yuv.h',
		'x11/x11-overlay.c',
	]
	gpu_overlay_deps += [ xv, x11, xext, dri2proto ]
	gpu_overlay_cflags += [ '-DHAVE_OVERLAY_XVLIB' ]
endif

if cairo_xlib.found() and xrandr.found() and dri2proto.found()
	both_x11_src = 'x11/position.c'
	gpu_overlay_src += 'x11/x11-window.c'
	gpu_overlay_deps += [ cairo_xlib, dri2proto ]
	gpu_overlay_cflags += [ '-DHAVE_OVERLAY_XLIB' ]
endif

gpu_overlay_src += both_x11_src

gpu_overlay_src += 'kms/kms-overlay.c'

if xrandr.found() and cairo.found()
	executable('intel-gpu-overlay', gpu_overlay_src,
			include_directories : inc,
			c_args : gpu_overlay_cflags,
			dependencies : gpu_overlay_deps,
			link_with : lib_igt_perf,
			install : true)
endif
