project('CM4all Prometheus Exporters', ['cpp'], version: '0.1',
  meson_version: '>= 0.49',
  default_options: [
    'cpp_std=c++2a',
    'warning_level=3',
  ],
)

# TODO: use get_option('b_ndebug'), but that doesn't work with Meson 0.37.1
if get_option('buildtype') == 'debug'
  debug = true
else
  debug = false
endif

compiler = meson.get_compiler('cpp')

common_flags = [
  '-D_GNU_SOURCE',
  '-Wall',
  '-Wextra',
  '-Wwrite-strings', '-Wcast-qual', '-Wcast-align', '-Wfloat-equal',
  '-Wshadow', '-Wpointer-arith', '-Wsign-compare',
  '-Wmissing-declarations', '-Wmissing-noreturn', '-Wmissing-format-attribute',
  '-Wredundant-decls', '-Wno-long-long', '-Wundef',
  '-Wunused',
  '-Wundef',
]

test_common_flags = [
  '-fvisibility=hidden',
]

test_common_flags += [
  '-ffunction-sections',
  '-fdata-sections',
]

add_global_link_arguments(
  compiler.get_supported_link_arguments(
    '-Wl,--gc-sections',
    '-Wl,--icf=all',
  ),
  language: 'cpp'
)

test_cxxflags = test_common_flags + [
  '-fno-threadsafe-statics',
  '-fmerge-all-constants',

  '-Wno-missing-field-initializers',
  '-Wno-non-virtual-dtor',
]

add_global_arguments(common_flags, language: 'cpp')

add_global_arguments(compiler.get_supported_arguments(test_cxxflags), language: 'cpp')

libsystemd = dependency('libsystemd')

libyamlcpp = dependency('yaml-cpp',
                        fallback: ['yaml-cpp', 'libyamlcpp_dep'])

inc = include_directories('src', 'libcommon/src')

libcommon_enable_c = false
libcommon_enable_libcap = false
libcommon_enable_DefaultFifoBuffer = false
libcommon_enable_net_log = false

subdir('libcommon/src/util')
subdir('libcommon/src/io')

executable(
  'cm4all-process-exporter',
  'src/ProcessExporter.cxx',
  'src/ProcessConfig.cxx',
  include_directories: inc,
  dependencies: [
    libsystemd,
    libyamlcpp,
    io_dep,
  ],
  install: true,
  install_dir: 'sbin',
)
