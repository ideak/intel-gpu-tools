
amdgpu_progs = []
amdgpu_deps = test_deps
if libdrm_amdgpu.found()
	amdgpu_progs += [ 'amd_abm',
			  'amd_assr',
			  'amd_basic',
			  'amd_bypass',
			  'amd_deadlock',
			  'amd_pci_unplug',
			  'amd_color',
			  'amd_cs_nop',
			  'amd_hotplug',
			  'amd_info',
			  'amd_prime',
			  'amd_max_bpc',
			  'amd_module_load',
			  'amd_mem_leak',
			  'amd_link_settings',
			  'amd_vrr_range',
			  'amd_mode_switch',
			  'amd_dp_dsc',
			  'amd_psr',
			  'amd_plane',
			  'amd_ilr',
			]
	amdgpu_deps += libdrm_amdgpu
endif

foreach prog : amdgpu_progs
	test_executables += executable(prog, prog + '.c',
				       dependencies : amdgpu_deps,
				       install_dir : amdgpudir,
				       install_rpath : amdgpudir_rpathdir,
				       install : true)
	test_list += join_paths('amdgpu', prog)
endforeach
