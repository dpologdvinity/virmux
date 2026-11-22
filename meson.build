project(
  'virmux',
  ['cpp', 'c'],
  version: '0.1.0',
  default_options: [
    'cpp_std=c++20',
    'c_std=c17',
    'warning_level=3',
  ],
)

inc = include_directories('include')
tests_inc = include_directories('tests', 'tests/unity')

threads_dep = dependency('threads')
ncurses_dep = dependency('ncursesw', required: true)
util_dep = meson.get_compiler('cpp').find_library('util', required: false)

# ---------------------------------------------------------------------------
# libFuzzer support (-Dfuzzing=true). Off by default; default builds are
# unaffected. When enabled, the core library is built with coverage
# instrumentation (-fsanitize=fuzzer-no-link) so libFuzzer can steer inputs
# through it, and every harness in tests/fuzz links the libFuzzer runtime.
# ASan/UBSan come from meson's b_sanitize so that the core library, unit tests,
# and harnesses all agree on the sanitizer runtime.
# ---------------------------------------------------------------------------
fuzzing = get_option('fuzzing')
fuzz_instrument_args = []
if fuzzing
  foreach lang : ['cpp', 'c']
    compiler_id = meson.get_compiler(lang).get_id()
    if compiler_id != 'clang'
      error(
        '-Dfuzzing=true requires clang for @0@ (found \'@1@\'). '.format(
          lang,
          compiler_id,
        )
        + 'Reconfigure with CC=clang CXX=clang++.',
      )
    endif
  endforeach
  # b_sanitize is a string on older meson and an array on newer meson;
  # formatting it covers both representations.
  sanitize_str = '@0@'.format(get_option('b_sanitize'))
  if not (sanitize_str.contains('address') and sanitize_str.contains('undefined'))
    error(
      '-Dfuzzing=true requires -Db_sanitize=address,undefined (found \'@0@\').'.format(
        sanitize_str,
      ),
    )
  endif
  # Abort on the first UBSan report so libFuzzer records it as a crash
  # instead of printing a diagnostic and continuing.
  fuzz_instrument_args = ['-fsanitize=fuzzer-no-link', '-fno-sanitize-recover=all']
endif

virmux_core_lib = static_library(
  'virmux_core',
  [
    'src/core/version.cpp',
    'src/core/cgroup.cpp',
    'src/sched/gate.cpp',
    'src/core/sandbox.cpp',
    'src/core/checkpoint.cpp',
    'src/bench/bench.cpp',
    'src/memory/cleanse.cpp',
    'src/memory/ksm.cpp',
    'src/memory/compactor.cpp',
    'src/memory/telemetry.cpp',
    'src/memory/memory_manager.cpp',
    'src/cli/cli_stats.cpp',
    'src/cli/cli_bench.cpp',
    'src/cli/cli_parse.cpp',
    'src/cli/cli_cas.cpp',
    'src/cli/cli_daemon.cpp',
    'src/storage/sha256.cpp',
    'src/storage/cas.cpp',
    'src/storage/scratch.cpp',
    'src/storage/overlay.cpp',
    'src/ipc/ipc.cpp',
    'src/daemon/session.cpp',
    'src/daemon/protocol.cpp',
    'src/daemon/daemon.cpp',
    'src/tui/tui.cpp',
  ],
  include_directories: inc,
  cpp_args: fuzz_instrument_args + [
    # Where `meson install` puts the interceptor (prefix + distro libdir, e.g.
    # lib/x86_64-linux-gnu on Debian/Ubuntu), so an installed virmux finds it.
    '-DVIRMUX_INTERCEPT_INSTALL_PATH="@0@"'.format(
      get_option('prefix') / get_option('libdir') / 'libvirmux_intercept.so',
    ),
  ],
  dependencies: [threads_dep, ncurses_dep, util_dep],
  install: false,
)

virmux_core_dep = declare_dependency(
  include_directories: inc,
  link_with: virmux_core_lib,
  dependencies: [threads_dep, ncurses_dep, util_dep],
)

unity_lib = static_library(
  'unity',
  'tests/unity/unity.c',
  include_directories: tests_inc,
  install: false,
)

unity_dep = declare_dependency(
  include_directories: tests_inc,
  link_with: unity_lib,
)

dl_dep = meson.get_compiler('cpp').find_library('dl', required: false)

browser_trim_lib = static_library(
  'browser_trim',
  'src/intercept/browser_trim.cpp',
  include_directories: inc,
  install: false,
)

browser_trim_dep = declare_dependency(
  include_directories: inc,
  link_with: browser_trim_lib,
)

virmux_intercept_lib = shared_library(
  'virmux_intercept',
  ['src/intercept/interceptor.cpp', 'src/intercept/browser_trim.cpp'],
  include_directories: inc,
  dependencies: [dl_dep],
  override_options: ['b_sanitize=none'],
  install: true,
)

test_version_exe = executable(
  'test_version',
  'tests/unit/test_version.cpp',
  dependencies: [virmux_core_dep, unity_dep],
)

test('test_version', test_version_exe, suite: 'unit')

test_interceptor_exe = executable(
  'test_interceptor',
  'tests/unit/test_interceptor.cpp',
  dependencies: [unity_dep],
)

test(
  'test_interceptor',
  test_interceptor_exe,
  env: ['LD_PRELOAD=' + virmux_intercept_lib.full_path()],
  depends: [virmux_intercept_lib],
  suite: 'unit',
)

test_interceptor_mem_exe = executable(
  'test_interceptor_mem',
  'tests/unit/test_interceptor_mem.cpp',
  include_directories: inc,
  dependencies: [unity_dep],
)

test(
  'test_interceptor_mem',
  test_interceptor_mem_exe,
  env: ['LD_PRELOAD=' + virmux_intercept_lib.full_path(), 'VIRMUX_KSM_AUTO_MERGE=1'],
  depends: [virmux_intercept_lib],
  suite: 'unit',
)

test_browser_trim_exe = executable(
  'test_browser_trim',
  'tests/unit/test_browser_trim.cpp',
  dependencies: [browser_trim_dep, unity_dep],
)

test(
  'test_browser_trim',
  test_browser_trim_exe,
  env: ['LD_PRELOAD=' + virmux_intercept_lib.full_path()],
  depends: [virmux_intercept_lib],
  suite: 'unit',
)

test_cgroup_exe = executable(
  'test_cgroup',
  'tests/unit/test_cgroup.cpp',
  dependencies: [virmux_core_dep, unity_dep],
)

test('test_cgroup', test_cgroup_exe, suite: 'unit')

test_gate_exe = executable(
  'test_gate',
  'tests/unit/test_gate.cpp',
  dependencies: [virmux_core_dep, unity_dep],
)

test('test_gate', test_gate_exe, suite: 'unit')

test_sandbox_exe = executable(
  'test_sandbox',
  'tests/unit/test_sandbox.cpp',
  dependencies: [virmux_core_dep, unity_dep],
)

test(
  'test_sandbox',
  test_sandbox_exe,
  depends: [virmux_intercept_lib],
  suite: 'unit',
)

test_cleanse_exe = executable(
  'test_cleanse',
  'tests/unit/test_cleanse.cpp',
  dependencies: [virmux_core_dep, unity_dep],
)

test('test_cleanse', test_cleanse_exe, suite: 'unit')

test_ksm_exe = executable(
  'test_ksm',
  'tests/unit/test_ksm.cpp',
  dependencies: [virmux_core_dep, unity_dep],
)

test('test_ksm', test_ksm_exe, suite: 'unit')

test_compactor_exe = executable(
  'test_compactor',
  'tests/unit/test_compactor.cpp',
  dependencies: [virmux_core_dep, unity_dep],
)

test('test_compactor', test_compactor_exe, suite: 'unit')

test_telemetry_exe = executable(
  'test_telemetry',
  'tests/unit/test_telemetry.cpp',
  dependencies: [virmux_core_dep, unity_dep],
)

test('test_telemetry', test_telemetry_exe, suite: 'unit')

test_memory_manager_exe = executable(
  'test_memory_manager',
  'tests/unit/test_memory_manager.cpp',
  dependencies: [virmux_core_dep, unity_dep],
)

test('test_memory_manager', test_memory_manager_exe, suite: 'unit')

test_cli_stats_exe = executable(
  'test_cli_stats',
  'tests/unit/test_cli_stats.cpp',
  dependencies: [virmux_core_dep, unity_dep],
)

test('test_cli_stats', test_cli_stats_exe, suite: 'unit')

test_cli_cas_exe = executable(
  'test_cli_cas',
  'tests/unit/test_cli_cas.cpp',
  dependencies: [virmux_core_dep, unity_dep],
)

test('test_cli_cas', test_cli_cas_exe, suite: 'unit')

test_cas_exe = executable(
  'test_cas',
  'tests/unit/test_cas.cpp',
  dependencies: [virmux_core_dep, unity_dep],
)

test('test_cas', test_cas_exe, suite: 'unit')

test_scratch_exe = executable(
  'test_scratch',
  'tests/unit/test_scratch.cpp',
  dependencies: [virmux_core_dep, unity_dep],
)

test('test_scratch', test_scratch_exe, suite: 'unit')

test_overlay_exe = executable(
  'test_overlay',
  'tests/unit/test_overlay.cpp',
  dependencies: [virmux_core_dep, unity_dep],
)

test('test_overlay', test_overlay_exe, suite: 'unit')

test_sandbox_overlay_exe = executable(
  'test_sandbox_overlay',
  'tests/unit/test_sandbox_overlay.cpp',
  dependencies: [virmux_core_dep, unity_dep],
)

test('test_sandbox_overlay', test_sandbox_overlay_exe, suite: 'unit')

test_ipc_exe = executable(
  'test_ipc',
  'tests/unit/test_ipc.cpp',
  dependencies: [virmux_core_dep, unity_dep],
)

test('test_ipc', test_ipc_exe, suite: 'unit')

test_session_exe = executable(
  'test_session',
  'tests/unit/test_session.cpp',
  dependencies: [virmux_core_dep, unity_dep],
)

test('test_session', test_session_exe, suite: 'unit')

test_daemon_exe = executable(
  'test_daemon',
  'tests/unit/test_daemon.cpp',
  dependencies: [virmux_core_dep, unity_dep],
)

test('test_daemon', test_daemon_exe, suite: 'unit')

test_daemon_protocol_exe = executable(
  'test_daemon_protocol',
  'tests/unit/test_daemon_protocol.cpp',
  dependencies: [virmux_core_dep, unity_dep],
)

test('test_daemon_protocol', test_daemon_protocol_exe, suite: 'unit')

test_cli_daemon_exe = executable(
  'test_cli_daemon',
  'tests/unit/test_cli_daemon.cpp',
  dependencies: [virmux_core_dep, unity_dep],
)

test('test_cli_daemon', test_cli_daemon_exe, suite: 'unit')

test_tui_exe = executable(
  'test_tui',
  'tests/unit/test_tui.cpp',
  dependencies: [virmux_core_dep, unity_dep],
)

test('test_tui', test_tui_exe, suite: 'unit')

test_fault_isolation_exe = executable(
  'test_fault_isolation',
  'tests/unit/test_fault_isolation.cpp',
  dependencies: [virmux_core_dep, unity_dep],
)

test('test_fault_isolation', test_fault_isolation_exe, suite: 'unit')

test_idle_autofreeze_exe = executable(
  'test_idle_autofreeze',
  'tests/unit/test_idle_autofreeze.cpp',
  dependencies: [virmux_core_dep, unity_dep],
)

test('test_idle_autofreeze', test_idle_autofreeze_exe, suite: 'unit', timeout: 60)

test_checkpoint_exe = executable(
  'test_checkpoint',
  'tests/unit/test_checkpoint.cpp',
  dependencies: [virmux_core_dep, unity_dep],
)

test('test_checkpoint', test_checkpoint_exe, suite: 'unit')

test_bench_exe = executable(
  'test_bench',
  'tests/unit/test_bench.cpp',
  dependencies: [virmux_core_dep, unity_dep],
)

test('test_bench', test_bench_exe, suite: 'unit', timeout: 60)

test_cli_bench_exe = executable(
  'test_cli_bench',
  'tests/unit/test_cli_bench.cpp',
  dependencies: [virmux_core_dep, unity_dep],
)

test('test_cli_bench', test_cli_bench_exe, suite: 'unit')

virmux_exe = executable(
  'virmux',
  'cli/main.cpp',
  dependencies: [virmux_core_dep],
  install: true,
)

test(
  'test_cli_smoke',
  virmux_exe,
  args: ['run', '--cores', '3', '--', '/bin/sh', '-c', 'echo "hello from virmux"'],
  depends: [virmux_intercept_lib],
  suite: 'cli',
)

test(
  'test_cli_version',
  virmux_exe,
  args: ['--version'],
  suite: 'cli',
)

test(
  'test_cli_help',
  virmux_exe,
  args: ['--help'],
  suite: 'cli',
)

test(
  'test_cli_run_help',
  virmux_exe,
  args: ['run', '--help'],
  suite: 'cli',
)

test(
  'test_cli_stats_help',
  virmux_exe,
  args: ['stats', '--help'],
  suite: 'cli',
)

test(
  'test_cli_pack_help',
  virmux_exe,
  args: ['pack', '--help'],
  suite: 'cli',
)

test(
  'test_cli_cas_help',
  virmux_exe,
  args: ['cas', '--help'],
  suite: 'cli',
)

test(
  'test_cli_checkpoint_help',
  virmux_exe,
  args: ['checkpoint', '--help'],
  suite: 'cli',
)

test(
  'test_cli_restore_help',
  virmux_exe,
  args: ['restore', '--help'],
  suite: 'cli',
)

test(
  'test_cli_bench_help',
  virmux_exe,
  args: ['bench', '--help'],
  suite: 'cli',
)

test(
  'test_cli_bench_run_help',
  virmux_exe,
  args: ['bench', 'run', '--help'],
  suite: 'cli',
)

test(
  'test_cli_bench_report_help',
  virmux_exe,
  args: ['bench', 'report', '--help'],
  suite: 'cli',
)

test(
  'test_cli_top_rejects_invalid_refresh',
  virmux_exe,
  args: ['top', '--headless', '--refresh', 'abc'],
  should_fail: true,
  suite: 'cli',
)

test(
  'test_cli_top_rejects_trailing_garbage_refresh',
  virmux_exe,
  args: ['top', '--headless', '--refresh=100ms'],
  should_fail: true,
  suite: 'cli',
)

test(
  'integration_virmux_run',
  find_program('bash'),
  args: [
    meson.current_source_dir() / 'tests/integration/test_virmux_run.sh',
    meson.current_build_dir(),
  ],
  depends: [virmux_exe, virmux_intercept_lib],
  timeout: 120,
  suite: 'integration',
)

test(
  'integration_memory_pack',
  find_program('bash'),
  args: [
    meson.current_source_dir() / 'tests/integration/test_memory_pack.sh',
    meson.current_build_dir(),
  ],
  depends: [virmux_exe, virmux_intercept_lib],
  timeout: 120,
  suite: 'integration',
)

test(
  'integration_ephemeral_workspaces',
  find_program('bash'),
  args: [
    meson.current_source_dir() / 'tests/integration/test_ephemeral_workspaces.sh',
    meson.current_build_dir(),
  ],
  depends: [virmux_exe, virmux_intercept_lib],
  timeout: 120,
  suite: 'integration',
)

test(
  'integration_daemon_tui',
  find_program('bash'),
  args: [
    meson.current_source_dir() / 'tests/integration/test_daemon_tui.sh',
    meson.current_build_dir(),
  ],
  depends: [virmux_exe, virmux_intercept_lib],
  timeout: 120,
  suite: 'integration',
)

test(
  'integration_fault_isolation',
  find_program('bash'),
  args: [
    meson.current_source_dir() / 'tests/integration/test_fault_isolation.sh',
    meson.current_build_dir(),
  ],
  depends: [virmux_exe, virmux_intercept_lib],
  timeout: 120,
  suite: 'integration',
)

test(
  'integration_bench_gauntlet',
  find_program('bash'),
  args: [
    meson.current_source_dir() / 'tests/integration/test_bench_gauntlet.sh',
    meson.current_build_dir(),
  ],
  depends: [virmux_exe, virmux_intercept_lib],
  timeout: 120,
  suite: 'integration',
)

# ---------------------------------------------------------------------------
# libFuzzer harnesses (only with -Dfuzzing=true). Each tests/fuzz/fuzz_<name>.cpp
# becomes ./fuzz_<name>. The matching meson test (suite 'fuzz') replays the
# checked-in seed corpus tests/fuzz/corpus/<name> once with -runs=0: no
# mutation happens and the source corpus directory is never written to.
# ---------------------------------------------------------------------------
if fuzzing
  fuzz_targets = [
    'ipc_frame',
    'daemon_request',
    'session_list_json',
    'bench_report',
    'size_bytes',
  ]
  foreach name : fuzz_targets
    fuzz_exe = executable(
      'fuzz_' + name,
      'tests/fuzz/fuzz_' + name + '.cpp',
      cpp_args: ['-fsanitize=fuzzer', '-fno-sanitize-recover=all'],
      link_args: ['-fsanitize=fuzzer'],
      dependencies: [virmux_core_dep],
    )
    test(
      'fuzz_' + name,
      fuzz_exe,
      args: ['-runs=0', meson.current_source_dir() / 'tests/fuzz/corpus' / name],
      env: ['ASAN_OPTIONS=detect_leaks=1', 'UBSAN_OPTIONS=print_stacktrace=1'],
      suite: 'fuzz',
      timeout: 120,
    )
  endforeach
endif
