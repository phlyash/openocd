#!/usr/bin/env bash
# Installed as bin/openocd in the Linux release archive; the real binary
# is bin/openocd.bin. Baking a $ORIGIN-relative RPATH through this
# project's autotools+libtool link step proved unreliable (libtool's own
# rpath handling expects real absolute paths, and a follow-up patchelf
# fix corrupted the ELF and segfaulted). Runtime libraries live beside
# openocd.bin so release archives need only bin/ and share/ at their root.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
export LD_LIBRARY_PATH="$HERE${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
exec "$HERE/openocd.bin" "$@"
