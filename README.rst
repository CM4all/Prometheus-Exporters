Prometheus Exporters
====================

A collection of Prometheus Exporters written in efficient C++.


Building
--------

You need:

- a C++20 compiler (gcc 8 or clang)
- `meson 0.49 <http://mesonbuild.com/>`__
- `Boost <http://boost.org/>`__
- `liburing <https://github.com/axboe/liburing>`__

To build it, type::

  meson . build
  ninja -C build
