Prometheus Exporters
====================

A collection of Prometheus Exporters written in efficient C++.


Building
--------

You need:

- a C++20 compiler (e.g. GCC or clang)
- `meson 0.56 <http://mesonbuild.com/>`__
- `libfmt <https://fmt.dev/>`__
- `systemd <https://www.freedesktop.org/wiki/Software/systemd/>`__
- `yaml-cpp <https://github.com/jbeder/yaml-cpp>`__
- `pcre <https://www.pcre.org/>`__
- `CURL <https://curl.haxx.se/>`__
- `zlib <https://zlib.net/>`__

Get the source code::

 git clone --recursive https://github.com/CM4all/Prometheus-Exporters

Run ``meson``::

 meson setup output

Compile and install::

 ninja -C output
 ninja -C output install
