#!/usr/bin/env bash
# Cross-builds libjaylink as a DLL for aarch64-w64-mingw32 using llvm-mingw
# (must already be on PATH) against the libusb sysroot built by
# build-libusb-mingw-arm64.sh, installing it under $1/usr. Requires meson
# and ninja on PATH (installed by the caller via apt).
set -euo pipefail

TRIPLE=aarch64-w64-mingw32
SYSROOT="$1"

WORK=/tmp/libjaylink-arm64-build
mkdir -p "$WORK"
cd "$WORK"

curl -fsSL -o libjaylink.tar.bz2 \
  https://gitlab.zapb.de/libjaylink/libjaylink/-/archive/0.4.0/libjaylink-0.4.0.tar.bz2
tar xf libjaylink.tar.bz2
cd libjaylink-0.4.0

# llvm-mingw ships triple-prefixed wrappers for ar/strip on some releases
# and not others (same uncertainty as the objdump lookup in the DLL
# bundling step); fall back to the unprefixed llvm-* tool names.
AR_TOOL=$(command -v "$TRIPLE-ar" || command -v llvm-ar)
STRIP_TOOL=$(command -v "$TRIPLE-strip" || command -v llvm-strip)

cat > cross-file.ini <<EOF
[binaries]
c = '$TRIPLE-clang'
ar = '$AR_TOOL'
strip = '$STRIP_TOOL'
pkgconfig = 'pkg-config'

[host_machine]
system = 'windows'
cpu_family = 'aarch64'
cpu = 'aarch64'
endian = 'little'
EOF

export PKG_CONFIG_LIBDIR="$SYSROOT/usr/lib/pkgconfig"
export PKG_CONFIG_SYSROOT_DIR="$SYSROOT"
# libjaylink's meson.build defaults werror=true; that governs libjaylink's
# own build only, not openocd's, but a new compiler surfacing an unrelated
# warning there shouldn't block this job the way -Werror was never meant
# to.
meson setup build --prefix=/usr --libdir=lib --cross-file cross-file.ini -Dwerror=false
meson compile -C build
DESTDIR="$SYSROOT" meson install -C build
