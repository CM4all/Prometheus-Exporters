Prometheus Exporters
====================

A collection of Prometheus Exporters for Linux written in efficient C++.


Building
--------

You need:

- a C++20 compiler (e.g. GCC or clang)
- `meson 1.2 <http://mesonbuild.com/>`__
- `libfmt <https://fmt.dev/>`__
- `systemd <https://www.freedesktop.org/wiki/Software/systemd/>`__
- `yaml-cpp <https://github.com/jbeder/yaml-cpp>`__
- `zlib <https://zlib.net/>`__

Optional dependencies:

- `CURL <https://curl.haxx.se/>`__
- `pcre <https://www.pcre.org/>`__

Get the source code::

 git clone --recursive https://github.com/CM4all/Prometheus-Exporters

Run ``meson``::

 meson setup output

Compile and install::

 ninja -C output
 ninja -C output install

The exporters are designed to be run with systemd socket activation;
after installing the software, you have to install the systemd units
from the ``debian/`` directory manually and start the socket units.
Some exporters need a configuration file in
``/etc/cm4all/prometheus-exporters/`` which you may need to copy from
``debian/``, too.  All of this is automated with our Debian package,
and that is how we use them; we never install manually as described
here.


Debian
------

To build the Debian package, install the build dependencies
(``dpkg-checkbuilddeps``) and run::

 dpkg-buildpackage -rfakeroot -b -uc -us

There is an APT repository with pre-built packages for Debian Bookworm
(amd64 and arm64); add to ``/etc/apt/sources.list``::

 deb [trusted=yes] https://deb.cm4all.net/debian/ bookworm-default main

(Unfortunately, it is not yet signed, therefore the ``trusted=yes``
option is needed currently.)


Using
-----

By default, all socket units are bound to
``/run/cm4all/prometheus-exporters/*.socket``.  For example, to test
the kernel exporter, run::

  curl --unix-socket /run/cm4all/prometheus-exporters/kernel.socket http://localhost/
