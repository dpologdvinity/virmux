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

virmux_core_lib = static_library(
  'virmux_core',
  ['src/core/version.cpp', 'src/core/cgroup.cpp'],
  include_directories: inc,
  install: false,
)

virmux_core_dep = declare_dependency(
  include_directories: inc,
  link_with: virmux_core_lib,
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
  install: true,
)

test_version_exe = executable(
  'test_version',
  'tests/unit/test_version.cpp',
  dependencies: [virmux_core_dep, unity_dep],
)

test('test_version', test_version_exe)

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
)

test_cgroup_exe = executable(
  'test_cgroup',
  'tests/unit/test_cgroup.cpp',
  dependencies: [virmux_core_dep, unity_dep],
)

test('test_cgroup', test_cgroup_exe)

