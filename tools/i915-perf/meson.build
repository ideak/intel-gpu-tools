executable('i915-perf-configs',
           [ 'i915_perf_configs.c' ],
           include_directories: inc,
           dependencies: [lib_igt_chipset, lib_igt_i915_perf],
           install: true)

executable('i915-perf-recorder',
           [ 'i915_perf_recorder.c' ],
           include_directories: inc,
           dependencies: [lib_igt, lib_igt_i915_perf],
           install: true)

executable('i915-perf-control',
           [ 'i915_perf_control.c' ],
           include_directories: inc,
           install: true)

executable('i915-perf-reader',
           [ 'i915_perf_reader.c' ],
           include_directories: inc,
           dependencies: [lib_igt, lib_igt_i915_perf],
           install: true)
