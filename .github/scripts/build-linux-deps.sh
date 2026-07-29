#!/usr/bin/env bash
# Builds libusb-1.0, libftdi1, and hidapi from source into /usr/local
# inside a manylinux2014 (CentOS 7 / glibc 2.17) container, since that
# image's own package repos don't carry current-enough versions of these.
set -euo pipefail

if [ -f /opt/rh/devtoolset-10/enable ]; then
  # shellcheck disable=SC1091
  source /opt/rh/devtoolset-10/enable
fi

yum install -y epel-release
yum install -y \
  autoconf automake libtool pkgconfig texinfo cmake3 \
  libudev-devel git wget curl tar bzip2 xz \
  gcc gcc-c++ make

export PKG_CONFIG_PATH=/usr/local/lib/pkgconfig
export LD_LIBRARY_PATH=/usr/local/lib

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
  cmake3 -DCMAKE_INSTALL_PREFIX=/usr/local -DCMAKE_BUILD_TYPE=Release -DFTDI_EEPROM=OFF ..
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
  cmake3 -DCMAKE_INSTALL_PREFIX=/usr/local -DCMAKE_BUILD_TYPE=Release ..
  make -j"$(nproc)"
  make install
)

ldconfig
