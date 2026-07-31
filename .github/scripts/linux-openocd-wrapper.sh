#!/usr/bin/env bash
# Installed as bin/openocd in the Linux release archive; the real binary
# is bin/openocd.bin. Baking a $ORIGIN-relative RPATH through this
# project's autotools+libtool link step proved unreliable (libtool's own
# rpath handling expects real absolute paths, and a follow-up patchelf
# fix corrupted the ELF and segfaulted). Setting LD_LIBRARY_PATH from a
# plain wrapper script instead works regardless of where the archive is
# extracted, with no ELF surgery involved.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
export LD_LIBRARY_PATH="$HERE/../lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
exec "$HERE/openocd.bin" "$@"
