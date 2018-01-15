benchmark_progs = [
	'gem_blt',
	'gem_busy',
	'gem_create',
	'gem_exec_ctx',
	'gem_exec_fault',
	'gem_exec_nop',
	'gem_exec_reloc',
	'gem_exec_trace',
	'gem_latency',
	'gem_mmap',
	'gem_prw',
	'gem_set_domain',
	'gem_syslatency',
	'kms_vblank',
	'prime_lookup',
	'vgem_mmap',
]

if libdrm_intel.found()
	benchmark_progs += [
		'intel_upload_blit_large',
		'intel_upload_blit_large_gtt',
		'intel_upload_blit_large_map',
		'intel_upload_blit_small',
		'gem_userptr_benchmark',
	]
endif

benchmarksdir = join_paths(libexecdir, 'benchmarks')

foreach prog : benchmark_progs
	# FIXME meson doesn't like binaries with the same name
	# meanwhile just suffix with _bench
	executable(prog + '_bench', prog + '.c',
		   install : true,
		   install_dir : benchmarksdir,
		   dependencies : test_deps)
endforeach

executable('gem_wsim_bench', 'gem_wsim.c',
	   install : true,
	   install_dir : benchmarksdir,
	   dependencies : test_deps + [ lib_igt_perf ])
