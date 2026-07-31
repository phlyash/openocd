#!/usr/bin/env bash
# Cross-builds hidapi's Windows (winapi) backend for aarch64-w64-mingw32
# using llvm-mingw (must already be on PATH), installing it under $1/usr.
#
# Built as a static library deliberately: hidapi's Windows backend loads
# hid.dll and cfgmgr32.dll dynamically via LoadLibrary/GetProcAddress at
# runtime rather than linking them at build time (confirmed by reading
# windows/hid.c), so a static build has no missing-system-lib risk and no
# DLL to bundle. It also sidesteps needing a working windows resource
# compiler, which a shared build would require to embed hidapi.rc.
set -euo pipefail

TRIPLE=aarch64-w64-mingw32
SYSROOT="$1"

WORK=/tmp/hidapi-arm64-build
mkdir -p "$WORK"
cd "$WORK"

curl -fsSL -o hidapi.tar.gz \
  https://github.com/libusb/hidapi/archive/refs/tags/hidapi-0.14.0.tar.gz
tar xf hidapi.tar.gz
mkdir -p hidapi-hidapi-0.14.0/build
(
  cd hidapi-hidapi-0.14.0/build
  cmake \
    -DCMAKE_SYSTEM_NAME=Windows \
    -DCMAKE_SYSTEM_PROCESSOR=aarch64 \
    -DCMAKE_C_COMPILER="$TRIPLE-clang" \
    -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
    -DCMAKE_INSTALL_PREFIX=/usr \
    -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_SHARED_LIBS=OFF \
    -DHIDAPI_WITH_TESTS=OFF \
    -DHIDAPI_BUILD_HIDTEST=OFF \
    -DHIDAPI_BUILD_PP_DATA_DUMP=OFF \
    ..
  cmake --build . -j"$(nproc)"
  DESTDIR="$SYSROOT" cmake --install .
)
