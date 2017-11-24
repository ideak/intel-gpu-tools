project('IGT gpu tests', 'c',
	version : '1.20',
        default_options: [
          'warning_level=2',
          'c_std=gnu99',
        ],
	license : 'MIT',
	meson_version : '>0.40.0')

cc = meson.get_compiler('c')

cc_args = [
	'-Wno-unused-parameter',
	'-Wno-sign-compare',
	'-Wno-missing-field-initializers',
	'-Wno-clobbered',
# Macros asserting on the range of their arguments triggers this.
	'-Wno-type-limits',
# igt_assert(0) in switch statements triggers a bunch of this.
	'-Wimplicit-fallthrough=0',
]

foreach cc_arg : cc_args
  if cc.has_argument(cc_arg)
    add_global_arguments(cc_arg, language : 'c')
  endif
endforeach

inc = include_directories('include/drm-uapi', 'lib', '.')

config = configuration_data()

libdrm = dependency('libdrm', version : '>=2.4.82')
libdrm_intel = dependency('libdrm_intel', required : false)
libdrm_nouveau = dependency('libdrm_nouveau', required : false)
libdrm_amdgpu = dependency('libdrm_amdgpu', required : false)

pciaccess = dependency('pciaccess', version : '>=0.10')
libkmod = dependency('libkmod')
libprocps = dependency('libprocps', required : true)

valgrind = dependency('valgrind', required : false)
if valgrind.found()
	config.set('HAVE_VALGRIND', 1)
endif

cairo = dependency('cairo', version : '>1.12.0', required : false)

libudev = dependency('libudev', required : false)
if libudev.found()
	config.set('HAVE_UDEV', 1)
endif

glib = dependency('glib-2.0', required : false)
if glib.found()
	config.set('HAVE_GLIB', 1)
endif

libunwind = dependency('libunwind')
gsl = dependency('gsl', required : false)
alsa = dependency('alsa', required : false)

pixman = dependency('pixman-1', required : false)
xmlrpc = dependency('xmlrpc', required : false)
xmlrpc_util = dependency('xmlrpc_util', required : false)
xmlrpc_client = dependency('xmlrpc_client', required : false)

xmlrpc_cmd = find_program('xmlrpc-c-config', required : false)
if not xmlrpc.found() and xmlrpc_cmd.found()
	libs_cmd = run_command(xmlrpc_cmd, 'client', '--libs')
	cflags_cmd = run_command(xmlrpc_cmd, 'client', '--cflags')

	if libs_cmd.returncode() == 0 and cflags_cmd.returncode() == 0
		xmlrpc = declare_dependency(compile_args: cflags_cmd.stdout().strip().split(),
					   link_args : libs_cmd.stdout().strip().split())
		xmlrpc_util = declare_dependency()
		xmlrpc_client = declare_dependency()
	endif
endif

if pixman.found() and xmlrpc.found() and xmlrpc_util.found() and xmlrpc_client.found()
	chamelium = declare_dependency(dependencies : [ pixman, xmlrpc,
	  xmlrpc_util, xmlrpc_client])
	config.set('HAVE_CHAMELIUM', 1)
else
	chamelium = dependency('', required: false)
endif

pthreads = dependency('threads')
math = cc.find_library('m')
realtime = cc.find_library('rt')
dlsym = cc.find_library('dl')
zlib = cc.find_library('z')

if cc.has_header('linux/kd.h')
	config.set('HAVE_LINUX_KD_H', 1)
endif
if cc.has_header('sys/kd.h')
	config.set('HAVE_SYS_KD_H', 1)
endif
if cc.has_header('libgen.h')
	config.set('HAVE_LIBGEN_H', 1)
endif
if cc.has_header('sys/io.h')
	config.set('HAVE_SYS_IO_H', 1)
endif
if cc.has_header('cpuid.h')
	# FIXME: Do we need the example link test from configure.ac?
	config.set('HAVE_CPUID_H', 1)
endif

if cc.has_member('struct sysinfo', 'totalram',
		prefix : '#include <sys/sysinfo.h>')
	config.set('HAVE_STRUCT_SYSINFO_TOTALRAM', 1)
endif

add_project_arguments('-D_GNU_SOURCE', language : 'c')
add_project_arguments('-include', 'config.h', language : 'c')

config.set('PACKAGE_NAME', meson.project_name())
config.set_quoted('PACKAGE_VERSION', meson.project_version())
config.set_quoted('PACKAGE', meson.project_name())
config.set('PACKAGE_STRING', meson.project_name() + ' ' + meson.project_version())
config.set_quoted('TARGET_CPU_PLATFORM', host_machine.cpu_family())

configure_file(output: 'config.h', install: false, configuration: config)

subdir('lib')
subdir('tests')
subdir('benchmarks')
subdir('tools')
if libdrm_intel.found()
	subdir('assembler')
	if ['x86', 'x86_64'].contains(host_machine.cpu_family())
		subdir('overlay')
	endif
endif
subdir('man')
