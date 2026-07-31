#!/usr/bin/env bash
# Builds libusb-1.0, libftdi1, hidapi, and libjaylink from source into
# /usr/local inside a manylinux2014 (CentOS 7 / glibc 2.17) container,
# since that image's own package repos don't carry current-enough
# versions of these (or, for libjaylink, don't carry it at all).
set -euo pipefail

if [ -f /opt/rh/devtoolset-10/enable ]; then
  # devtoolset-10's enable script references MANPATH without a default,
  # which set -u treats as fatal.
  set +u
  # shellcheck disable=SC1091
  source /opt/rh/devtoolset-10/enable
  set -u
fi

yum install -y \
  autoconf automake libtool pkgconfig texinfo \
  systemd-devel git wget curl tar bzip2 xz \
  gcc gcc-c++ make python3 python3-pip

export PKG_CONFIG_PATH=/usr/local/lib/pkgconfig
export LD_LIBRARY_PATH=/usr/local/lib

# libjaylink builds with Meson, not autotools/CMake; CentOS 7's own repos
# don't carry it, but both are pure-Python-installable via pip.
python3 -m pip install --quiet --upgrade pip
python3 -m pip install --quiet meson ninja

WORK=/tmp/linux-deps-build
mkdir -p "$WORK"
cd "$WORK"

# libusb 1.0.27
curl -fsSL -o libusb.tar.bz2 \
  https://github.com/libusb/libusb/releases/download/v1.0.27/libusb-1.0.27.tar.bz2
tar xf libusb.tar.bz2
(
  cd libusb-1.0.27
  ./configure --prefix=/usr/local
  make -j"$(nproc)"
  make install
)

# libftdi1 1.5
curl -fsSL -o libftdi1.tar.bz2 \
  https://www.intra2net.com/en/developer/libftdi/download/libftdi1-1.5.tar.bz2
tar xf libftdi1.tar.bz2
mkdir -p libftdi1-1.5/build
(
  cd libftdi1-1.5/build
  # libftdi1 does not use GNUInstallDirs; it has its own LIB_SUFFIX
  # variable that defaults to "64" on any 64-bit RHEL/CentOS-family
  # host (manylinux2014 qualifies on both x86_64 and arm64), so
  # CMAKE_INSTALL_LIBDIR above has no effect on it.
  cmake -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DCMAKE_INSTALL_PREFIX=/usr/local -DCMAKE_INSTALL_LIBDIR=lib -DLIB_SUFFIX= -DCMAKE_BUILD_TYPE=Release -DFTDI_EEPROM=OFF ..
  make -j"$(nproc)"
  make install
)

# hidapi 0.14.0
curl -fsSL -o hidapi.tar.gz \
  https://github.com/libusb/hidapi/archive/refs/tags/hidapi-0.14.0.tar.gz
tar xf hidapi.tar.gz
mkdir -p hidapi-hidapi-0.14.0/build
(
  cd hidapi-hidapi-0.14.0/build
  cmake -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DCMAKE_INSTALL_PREFIX=/usr/local -DCMAKE_INSTALL_LIBDIR=lib -DCMAKE_BUILD_TYPE=Release ..
  make -j"$(nproc)"
  make install
)

# libjaylink 0.4.0
curl -fsSL -o libjaylink.tar.bz2 \
  https://gitlab.zapb.de/libjaylink/libjaylink/-/archive/0.4.0/libjaylink-0.4.0.tar.bz2
tar xf libjaylink.tar.bz2
(
  cd libjaylink-0.4.0
  # Meson has the same lib64-on-RHEL-family default as libftdi1's own
  # LIB_SUFFIX logic; pin --libdir explicitly rather than rediscover that.
  meson setup build --prefix=/usr/local --libdir=lib -Dwerror=false
  meson compile -C build
  meson install -C build
)

ldconfig
