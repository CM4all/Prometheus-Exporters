project('CM4all Prometheus Exporters', ['cpp'], version: '0.17',
  meson_version: '>= 0.56',
  default_options: [
    'cpp_std=c++2a',
    'warning_level=3',

    'fmt:cpp_std=c++2a',

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

libcommon_enable_DefaultFifoBuffer = false
libcommon_require_curl = get_option('curl')
libcommon_require_pcre = get_option('pcre')

subdir('libcommon/src/util')
subdir('libcommon/src/lib/fmt')
subdir('libcommon/src/time')
subdir('libcommon/src/http')
subdir('libcommon/src/lib/pcre')
subdir('libcommon/src/lib/nlohmann_json')
subdir('libcommon/src/io')
subdir('libcommon/src/lib/curl')
subdir('libcommon/src/lib/zlib')
subdir('libcommon/src/system')
subdir('libcommon/src/net')
subdir('libcommon/src/event')
subdir('libcommon/src/event/net')

frontend = static_library(
  'frontend',
  'src/Frontend.cxx',
  'src/Syntax.cxx',
  include_directories: inc,
  dependencies: [
    libsystemd,
    io_dep,
    zlib_dep,
    http_dep,
  ],
)

frontend_dep = declare_dependency(
  link_with: frontend,
  dependencies: [
    libsystemd,
    zlib_dep,
  ],
)

efrontend = static_library(
  'efrontend',
  'src/EFrontend.cxx',
  include_directories: inc,
  dependencies: [
    event_net_dep,
    libsystemd,
  ],
)

efrontend_dep = declare_dependency(
  link_with: efrontend,
  dependencies: [
    event_net_dep,
  ],
)

if pcre_dep.found()
  executable(
    'cm4all-process-exporter',
    'src/ProcessExporter.cxx',
    'src/ProcessConfig.cxx',
    include_directories: inc,
    dependencies: [
      libyamlcpp,
      pcre_dep,
      frontend_dep,
    ],
    install: true,
    install_dir: 'sbin',
  )
endif

executable(
  'cm4all-fs-exporter',
  'src/FsExporter.cxx',
  include_directories: inc,
  dependencies: [
    frontend_dep,
  ],
  install: true,
  install_dir: 'sbin',
)

executable(
  'cm4all-kernel-exporter',
  'src/KernelExporter.cxx',
  'src/Pressure.cxx',
  include_directories: inc,
  dependencies: [
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
    libyamlcpp,
    frontend_dep,
  ],
  install: true,
  install_dir: 'sbin',
)

executable(
  'cm4all-ping-exporter',
  'src/PingExporter.cxx',
  'src/PingConfig.cxx',
  include_directories: inc,
  dependencies: [
    libyamlcpp,
    fmt_dep,
    efrontend_dep,
  ],
  install: true,
  install_dir: 'sbin',
)

executable(
  'cm4all-openbgpd-exporter',
  'src/BgpExporter.cxx',
  include_directories: inc,
  dependencies: [
    nlohmann_json_dep,
    frontend_dep,
  ],
  install: true,
  install_dir: 'sbin',
)

if curl_dep.found()
  executable(
    'cm4all-multi-exporter',
    'src/MultiExporter.cxx',
    'src/MultiConfig.cxx',
    include_directories: inc,
    dependencies: [
      libyamlcpp,
      curl_dep,
      frontend_dep,
      util_dep,
    ],
    install: true,
    install_dir: 'sbin',
  )
endif
