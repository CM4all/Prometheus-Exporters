Prometheus Exporters
====================

A collection of Prometheus Exporters written in efficient C++.


Building
--------

You need:

- a C++20 compiler (GCC 10 or clang)
- `meson 0.56 <http://mesonbuild.com/>`__
- `systemd <https://www.freedesktop.org/wiki/Software/systemd/>`__
- `yaml-cpp <https://github.com/jbeder/yaml-cpp>`__
- `pcre <https://www.pcre.org/>`__

Get the source code::

 git clone --recursive https://github.com/CM4all/Prometheus-Exporters

To build it, type::

  meson . build
  ninja -C build
