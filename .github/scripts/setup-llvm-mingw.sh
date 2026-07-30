#!/usr/bin/env bash
# Downloads and extracts a pinned llvm-mingw release (Ubuntu x86_64 host
# build) into $1, and prints $1/bin on stdout.
#
# The tag is pinned deliberately: every other dependency in this workflow is
# pinned to an exact version, so a CI failure means an OpenOCD regression or
# an intentional bump, not silent upstream drift. To move to a newer
# toolchain, bump LLVM_MINGW_TAG to a tag from
# https://github.com/mstorsjo/llvm-mingw/releases
set -euo pipefail

DEST="$1"

LLVM_MINGW_TAG="20241030"
ASSET_URL="https://github.com/mstorsjo/llvm-mingw/releases/download/${LLVM_MINGW_TAG}/llvm-mingw-${LLVM_MINGW_TAG}-ucrt-ubuntu-20.04-x86_64.tar.xz"

curl -fsSL -o /tmp/llvm-mingw.tar.xz "$ASSET_URL"
mkdir -p "$DEST"
tar xf /tmp/llvm-mingw.tar.xz -C "$DEST" --strip-components=1

echo "$DEST/bin"
