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
	'gem_wsim',
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

foreach prog : benchmark_progs
	# FIXME meson doesn't like binaries with the same name
	# meanwhile just suffix with _bench
	link = []
	if prog == 'gem_wsim'
		link += lib_igt_perf
	endif
	executable(prog + '_bench', prog + '.c',
		   link_with : link,
		   dependencies : test_deps)
endforeach
