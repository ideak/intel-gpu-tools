tools_progs_noisnt = [
	'hsw_compute_wrpll',
	'skl_compute_wrpll',
	'skl_ddb_allocation',
]

foreach prog : tools_progs_noisnt
	executable(prog, prog + '.c',
			dependencies : igt_deps,
			install : false)
endforeach

tools_progs = [
	# FIXME we already have a libtestcase with this name as target
	#'igt_stats',
	'intel_audio_dump',
	'intel_backlight',
	'intel_bios_dumper',
	'intel_display_crc',
	'intel_display_poller',
	'intel_forcewaked',
	'intel_gpu_frequency',
	'intel_firmware_decode',
	'intel_gpu_time',
	'intel_gpu_top',
	'intel_gtt',
	'intel_guc_logger',
	'intel_infoframes',
	'intel_lid',
	'intel_opregion_decode',
	'intel_panel_fitter',
	'intel_reg_checker',
	'intel_residency',
	'intel_stepping',
	'intel_vbt_decode',
	'intel_watermark',
	'intel_gem_info',
	'intel_gvtg_test',
]
tool_deps = igt_deps

if libdrm_intel.found()
	tools_progs += [
		'intel_dump_decode',
		'intel_error_decode',
		'intel_framebuffer_dump',
		'intel_perf_counters',
	]
	tool_deps += zlib
endif

foreach prog : tools_progs
	executable(prog, prog + '.c',
		   dependencies : tool_deps,
		   install : true)
endforeach

pkgdatadir = join_paths(get_option('prefix'), get_option('datadir'), 'intel-gpu-tools')

if libudev.found()
	intel_dp_compliance_src = [
		'intel_dp_compliance.c',
		'intel_dp_compliance_hotplug.c'
	]
	executable('intel_dp_compliance', sources : intel_dp_compliance_src,
		   dependencies : [tool_deps, libudev],
		   install : true)
endif

intel_l3_parity_src = [ 'intel_l3_parity.c', 'intel_l3_udev_listener.c' ]
executable('intel_l3_parity', sources : intel_l3_parity_src,
	   dependencies : tool_deps,
	   install : true)

intel_reg_src = [ 'intel_reg.c', 'intel_reg_decode.c', 'intel_reg_spec.c' ]
executable('intel_reg', sources : intel_reg_src,
	   dependencies : tool_deps,
	   install : true,
	   c_args : [
	     '-DIGT_DATADIR="@0@"'.format(pkgdatadir),
	   ])

bindir = join_paths(get_option('prefix'), get_option('bindir'))

install_data('intel_gpu_abrt', install_dir : bindir)

install_subdir('registers', install_dir : pkgdatadir,
	       exclude_files : [
		 'Makefile', 'Makefile.in', 'Makefile.am',
	       ])

shared_library('intel_aubdump', 'aubdump.c',
	       dependencies : [ igt_deps, dlsym ],
	       name_prefix : '',
	       install : true)

conf_data = configuration_data()
conf_data.set('prefix', get_option('prefix'))
conf_data.set('exec_prefix', '${prefix}')
conf_data.set('libdir', join_paths('${prefix}', get_option('libdir')))
configure_file(input : 'intel_aubdump.in', output : 'intel_aubdump',
               configuration : conf_data, install_dir : get_option('bindir'))

subdir('null_state_gen')
