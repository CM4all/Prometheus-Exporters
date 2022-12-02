project('CM4all Prometheus Exporters', ['cpp'], version: '0.6',
  meson_version: '>= 0.49',
  default_options: [
    'cpp_std=c++20',
    'warning_level=3',

    # Not interested in compiler warnings from subprojects.
    'yaml-cpp:werror=false',
    'yaml-cpp:warning_level=0',
  ],
)

debug = get_option('b_ndebug') != 'true'

compiler = meson.get_compiler('cpp')

common_flags = [
  '-D_GNU_SOURCE',
]

test_common_flags = [
  '-Wcast-align',
  '-Wcast-qual',
  '-Wdouble-promotion',
  '-Wfloat-equal',
  '-Wmissing-declarations',
  '-Wmissing-format-attribute',
  '-Wmissing-noreturn',
  '-Wredundant-decls',
  '-Wshadow',
  '-Wundef',
  '-Wunused',
  '-Wvla',
  '-Wwrite-strings',

  # clang specific warning options:
  '-Wunreachable-code-aggressive',
  '-Wused-but-marked-unused',
]

test_global_common_flags = [
  '-fvisibility=hidden',
]

if not debug
  test_global_common_flags += [
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
endif

test_global_cxxflags = test_global_common_flags + [
]

test_cxxflags = test_common_flags + [
  '-fno-threadsafe-statics',
  '-fmerge-all-constants',

  '-Wcomma-subscript',
  '-Wextra-semi',
  '-Wmismatched-tags',
  '-Woverloaded-virtual',
  '-Wsign-promo',
  '-Wvolatile',
  '-Wvirtual-inheritance',

  '-Wno-missing-field-initializers',

  # a vtable without a dtor is just fine
  '-Wno-non-virtual-dtor',

  # clang specific warning options:
  '-Wcomma',
  '-Wheader-hygiene',
  '-Winconsistent-missing-destructor-override',
]

add_global_arguments(common_flags, language: 'cpp')

add_global_arguments(compiler.get_supported_arguments(test_global_cxxflags), language: 'cpp')
add_project_arguments(compiler.get_supported_arguments(test_cxxflags), language: 'cpp')

libsystemd = dependency('libsystemd')

libyamlcpp = dependency('yaml-cpp',
                        fallback: ['yaml-cpp', 'libyamlcpp_dep'])

inc = include_directories('src', 'libcommon/src')

libcommon_enable_c = false
libcommon_enable_libcap = false
libcommon_enable_DefaultFifoBuffer = false
libcommon_enable_net_log = false
libcommon_enable_event = false

subdir('libcommon/src/util')
subdir('libcommon/src/time')
subdir('libcommon/src/http')
subdir('libcommon/src/lib/pcre')
subdir('libcommon/src/io')
subdir('libcommon/src/lib/curl')
subdir('libcommon/src/lib/zlib')

frontend = static_library(
  'frontend',
  'src/Frontend.cxx',
  include_directories: inc,
  dependencies: [
    io_dep,
    zlib_dep,
    http_dep,
  ],
)

frontend_dep = declare_dependency(
  link_with: frontend,
  dependencies: [
    zlib_dep,
  ],
)

executable(
  'cm4all-process-exporter',
  'src/ProcessExporter.cxx',
  'src/ProcessConfig.cxx',
  include_directories: inc,
  dependencies: [
    libsystemd,
    libyamlcpp,
    pcre_dep,
    frontend_dep,
  ],
  install: true,
  install_dir: 'sbin',
)

executable(
  'cm4all-kernel-exporter',
  'src/KernelExporter.cxx',
  include_directories: inc,
  dependencies: [
    libsystemd,
    frontend_dep,
  ],
  install: true,
  install_dir: 'sbin',
)

executable(
  'cm4all-cgroup-exporter',
  'src/CgroupExporter.cxx',
  'src/CgroupConfig.cxx',
  'src/Pressure.cxx',
  include_directories: inc,
  dependencies: [
    libsystemd,
    libyamlcpp,
    frontend_dep,
  ],
  install: true,
  install_dir: 'sbin',
)

executable(
  'cm4all-multi-exporter',
  'src/MultiExporter.cxx',
  'src/MultiConfig.cxx',
  include_directories: inc,
  dependencies: [
    libsystemd,
    libyamlcpp,
    curl_dep,
    frontend_dep,
    util_dep,
  ],
  install: true,
  install_dir: 'sbin',
)
