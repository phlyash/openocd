#!/usr/bin/env bash
# Cross-builds libusb-1.0 as a DLL for aarch64-w64-mingw32 using llvm-mingw
# (must already be on PATH), installing it under $1/usr.
set -euo pipefail

TRIPLE=aarch64-w64-mingw32
SYSROOT="$1"

mkdir -p "$SYSROOT"
WORK=/tmp/libusb-arm64-build
mkdir -p "$WORK"
cd "$WORK"

curl -fsSL -o libusb.tar.bz2 \
  https://github.com/libusb/libusb/releases/download/v1.0.27/libusb-1.0.27.tar.bz2
tar xf libusb.tar.bz2
cd libusb-1.0.27

CC="$TRIPLE-clang" ./configure --host="$TRIPLE" --prefix=/usr
make -j"$(nproc)"
make install DESTDIR="$SYSROOT"
