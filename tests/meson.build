test_progs = [
	'core_auth',
	'core_get_client_auth',
	'core_getclient',
	'core_getstats',
	'core_getversion',
	'core_prop_blob',
	'core_setmaster_vs_auth',
	'debugfs_test',
	'drm_import_export',
	'drm_mm',
	'drm_read',
	'drm_vma_limiter',
	'drm_vma_limiter_cached',
	'drm_vma_limiter_cpu',
	'drm_vma_limiter_gtt',
	'drv_getparams_basic',
	'drv_hangman',
	'drv_missed_irq',
	'drv_module_reload',
	'drv_selftest',
	'drv_suspend',
	'gem_bad_reloc',
	'gem_basic',
	'gem_busy',
	'gem_caching',
	'gem_close',
	'gem_close_race',
	'gem_concurrent_blit',
	'gem_cpu_reloc',
	'gem_create',
	'gem_cs_prefetch',
	'gem_cs_tlb',
	'gem_ctx_bad_destroy',
	'gem_ctx_bad_exec',
	'gem_ctx_create',
	'gem_ctx_exec',
	'gem_ctx_param',
	'gem_ctx_switch',
	'gem_ctx_thrash',
	'gem_double_irq_loop',
	'gem_eio',
	'gem_evict_alignment',
	'gem_evict_everything',
	'gem_exec_alignment',
	'gem_exec_async',
	'gem_exec_await',
	'gem_exec_bad_domains',
	'gem_exec_basic',
	'gem_exec_big',
	'gem_exec_blt',
	'gem_exec_capture',
	'gem_exec_create',
	'gem_exec_faulting_reloc',
	'gem_exec_fence',
	'gem_exec_flush',
	'gem_exec_gttfill',
	'gem_exec_latency',
	'gem_exec_lut_handle',
	'gem_exec_nop',
	'gem_exec_parallel',
	'gem_exec_params',
	'gem_exec_parse',
	'gem_exec_reloc',
	'gem_exec_reuse',
	'gem_exec_schedule',
	'gem_exec_store',
	'gem_exec_suspend',
	'gem_exec_whisper',
	'gem_fd_exhaustion',
	'gem_fence_thrash',
	'gem_fence_upload',
	'gem_fenced_exec_thrash',
	'gem_flink_basic',
	'gem_flink_race',
	'gem_gpgpu_fill',
	'gem_gtt_cpu_tlb',
	'gem_gtt_hog',
	'gem_gtt_speed',
	'gem_largeobject',
	'gem_linear_blits',
	'gem_lut_handle',
	'gem_madvise',
	'gem_media_fill',
	'gem_mmap',
	'gem_mmap_gtt',
	'gem_mmap_offset_exhaustion',
	'gem_mmap_wc',
	'gem_mocs_settings',
	'gem_partial_pwrite_pread',
	'gem_persistent_relocs',
	'gem_pipe_control_store_loop',
	'gem_ppgtt',
	'gem_pread',
	'gem_pread_after_blit',
	'gem_pwrite',
	'gem_pwrite_pread',
	'gem_pwrite_snooped',
	'gem_read_read_speed',
	'gem_readwrite',
	'gem_reg_read',
	'gem_reloc_overflow',
	'gem_reloc_vs_gpu',
	'gem_render_copy',
	'gem_render_copy_redux',
	'gem_render_linear_blits',
	'gem_render_tiled_blits',
	'gem_request_retire',
	'gem_reset_stats',
	'gem_ring_sync_copy',
	'gem_ring_sync_loop',
	'gem_ringfill',
	'gem_set_tiling_vs_blt',
	'gem_set_tiling_vs_gtt',
	'gem_set_tiling_vs_pwrite',
	'gem_shrink',
	'gem_softpin',
	'gem_spin_batch',
	'gem_stolen',
	'gem_storedw_batches_loop',
	'gem_storedw_loop',
	'gem_streaming_writes',
	'gem_sync',
	'gem_threaded_access_tiled',
	'gem_tiled_blits',
	'gem_tiled_fence_blits',
	'gem_tiled_partial_pwrite_pread',
	'gem_tiled_pread_basic',
	'gem_tiled_pread_pwrite',
	'gem_tiled_swapping',
	'gem_tiled_wb',
	'gem_tiled_wc',
	'gem_tiling_max_stride',
	'gem_unfence_active_buffers',
	'gem_unref_active_buffers',
	'gem_userptr_blits',
	'gem_wait',
	'gem_workarounds',
	'gem_write_read_ring_switch',
	'gen3_mixed_blits',
	'gen3_render_linear_blits',
	'gen3_render_mixed_blits',
	'gen3_render_tiledx_blits',
	'gen3_render_tiledy_blits',
	'gen7_forcewake_mt',
	'gvt_basic',
	'kms_3d',
	'kms_addfb_basic',
	'kms_atomic',
	'kms_atomic_interruptible',
	'kms_atomic_transition',
	'kms_busy',
	'kms_ccs',
	'kms_chv_cursor_fail',
	'kms_color',
	'kms_concurrent',
	'kms_crtc_background_color',
	'kms_cursor_crc',
	'kms_cursor_legacy',
	'kms_draw_crc',
	'kms_fbcon_fbt',
	'kms_fence_pin_leak',
	'kms_flip',
	'kms_flip_event_leak',
	'kms_flip_tiling',
	'kms_force_connector_basic',
	'kms_frontbuffer_tracking',
	'kms_hdmi_inject',
	'kms_invalid_dotclock',
	'kms_legacy_colorkey',
	'kms_mmap_write_crc',
	'kms_mmio_vs_cs_flip',
	'kms_panel_fitting',
	'kms_pipe_b_c_ivb',
	'kms_pipe_crc_basic',
	'kms_plane',
	'kms_plane_lowres',
	'kms_plane_multiple',
	'kms_plane_scaling',
	'kms_properties',
	'kms_psr_sink_crc',
	'kms_pwrite_crc',
	'kms_render',
	'kms_rmfb',
	'kms_rotation_crc',
	'kms_setmode',
	'kms_sink_crc_basic',
	'kms_sysfs_edid_timing',
	'kms_tv_load_detect',
	'kms_universal_plane',
	'kms_vblank',
	'meta_test',
	'perf',
	'pm_backlight',
	'pm_lpsp',
	'pm_rc6_residency',
	'pm_rpm',
	'pm_rps',
	'pm_sseu',
	'prime_busy',
	'prime_mmap',
	'prime_mmap_coherency',
	'prime_mmap_kms',
	'prime_self_import',
	'prime_udl',
	'prime_vgem',
	'sw_sync',
	'syncobj_basic',
	'syncobj_wait',
	'template',
	'tools_test',
	'vc4_create_bo',
	'vc4_dmabuf_poll',
	'vc4_label_bo',
	'vc4_lookup_fail',
	'vc4_tiling',
	'vc4_wait_bo',
	'vc4_wait_seqno',
	'vgem_basic',
	'vgem_slow',
]

test_deps = [ igt_deps ]

if libdrm_amdgpu.found()
	# FIXME meson/ninja really doesn't like build targets with paths in them
	test_progs += [
		'amdgpu/amd_basic',
		'amdgpu/amd_cs_nop',
		'amdgpu/amd_prime',
	]
	test_deps += libdrm_amdgpu
endif

if libdrm_nouveau.found()
	test_progs += [
		'prime_nv_api',
		'prime_nv_pcopy',
		'prime_nv_test',
	]
	test_deps += libdrm_nouveau
endif

if chamelium.found()
	test_progs += [
		'kms_chamelium',
	]
	test_deps += chamelium
endif

if alsa.found() and gsl.found()
	test_progs += [
		'audio',
	]
	test_deps += alsa
endif

libexecdir = join_paths(get_option('prefix'), get_option('libexecdir'), 'intel-gpu-tools')

test_executables = []

foreach prog : test_progs
	test_executables += executable(prog, prog + '.c',
		   dependencies : test_deps,
		   install_dir : libexecdir,
		   install : true)
endforeach

test_executables += executable('perf_pmu', 'perf_pmu.c',
	   dependencies : test_deps + [ lib_igt_perf ],
	   install_dir : libexecdir,
	   install : true)
test_progs += 'perf_pmu'

executable('testdisplay', ['testdisplay.c', 'testdisplay_hotplug.c'],
	   dependencies : test_deps,
	   install_dir : libexecdir,
	   install : true)
test_progs += 'testdisplay'

pkgdatadir = join_paths(get_option('prefix'), get_option('datadir'), 'intel-gpu-tools')

gen_testlist = find_program('generate_testlist.sh')
test_list = custom_target('testlist',
	      output : 'test-list.txt',
	      command : [ gen_testlist, '@OUTPUT@', test_progs ],
	      install : true,
	      install_dir : libexecdir)

test_script = find_program('igt_command_line.sh')
foreach prog : test_progs
	test('testcase check: ' + prog, test_script,
			args : prog)
endforeach

executable('gem_stress', 'gem_stress.c', dependencies : igt_deps)

# IMPORTANT: These tests here are all disabled because the result in sometime
# unrecoverable gpu hangs. Don't put real testcases here.
hang_progs = [
	'gem_bad_batch',
	'gem_hang',
	'gem_bad_blit',
	'gem_bad_address',
	'gem_non_secure_batch',
]
foreach prog : hang_progs
	executable(prog, prog + '.c',
			dependencies : igt_deps,
			install : false)
endforeach

image_files = [
  '1080p-left.png',
  '1080p-right.png',
  'pass.png',
]
install_data(sources : image_files, install_dir : pkgdatadir)
