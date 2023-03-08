xe_progs = [
	'xe_compute',
	'xe_dma_buf_sync',
	'xe_debugfs',
	'xe_evict',
	'xe_exec_balancer',
	'xe_exec_basic',
	'xe_exec_compute_mode',
	'xe_exec_fault_mode',
	'xe_exec_reset',
	'xe_exec_threads',
	'xe_guc_pc',
	'xe_huc_copy',
	'xe_mmap',
	'xe_mmio',
	'xe_pm',
	'xe_prime_self_import',
	'xe_query',
	'xe_vm',
	'xe_waitfence',
]
xe_deps = test_deps

xe_test_config = meson.current_source_dir() + '/xe_test_config.json'

foreach prog : xe_progs
	test_executables += executable(prog, prog + '.c',
				       dependencies : xe_deps,
				       install_dir : xedir,
				       install_rpath : xedir_rpathdir,
				       install : true)
	test_list += join_paths('xe', prog)
endforeach
