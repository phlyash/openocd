#!/usr/bin/env bash
# Cross-builds libftdi1 as a DLL for aarch64-w64-mingw32 using llvm-mingw
# (must already be on PATH) against the libusb sysroot built by
# build-libusb-mingw-arm64.sh, installing it under $1/usr.
set -euo pipefail

TRIPLE=aarch64-w64-mingw32
SYSROOT="$1"

WORK=/tmp/libftdi1-arm64-build
mkdir -p "$WORK"
cd "$WORK"

curl -fsSL -o libftdi1.tar.bz2 \
  https://www.intra2net.com/en/developer/libftdi/download/libftdi1-1.5.tar.bz2
tar xf libftdi1.tar.bz2
mkdir -p libftdi1-1.5/build
(
  cd libftdi1-1.5/build
  export PKG_CONFIG_LIBDIR="$SYSROOT/usr/lib/pkgconfig"
  export PKG_CONFIG_SYSROOT_DIR="$SYSROOT"
  # libftdi1's own FindUSB1.cmake locates libusb via pkg-config; the two
  # PKG_CONFIG_* vars above point it at the cross-built libusb instead of
  # anything on the ubuntu-latest host. -DCMAKE_POLICY_VERSION_MINIMUM is
  # the same cmake_minimum_required<3.5 workaround needed on Linux.
  cmake \
    -DCMAKE_SYSTEM_NAME=Windows \
    -DCMAKE_SYSTEM_PROCESSOR=aarch64 \
    -DCMAKE_C_COMPILER="$TRIPLE-clang" \
    -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
    -DCMAKE_INSTALL_PREFIX=/usr \
    -DCMAKE_BUILD_TYPE=Release \
    -DFTDI_EEPROM=OFF \
    -DEXAMPLES=OFF \
    -DDOCUMENTATION=OFF \
    ..
  cmake --build . -j"$(nproc)"
  DESTDIR="$SYSROOT" cmake --install .
)
