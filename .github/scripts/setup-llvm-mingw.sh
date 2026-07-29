#!/usr/bin/env bash
# Downloads and extracts the latest llvm-mingw release (Ubuntu x86_64 host
# build) into $1, and prints $1/bin on stdout.
set -euo pipefail

DEST="$1"

ASSET_URL=$(curl -fsSL https://api.github.com/repos/mstorsjo/llvm-mingw/releases/latest \
  | grep browser_download_url \
  | grep 'ucrt-ubuntu-20.04-x86_64.tar.xz' \
  | cut -d '"' -f4)

if [ -z "$ASSET_URL" ]; then
  echo "Could not find an llvm-mingw ubuntu-20.04-x86_64 release asset" >&2
  exit 1
fi

curl -fsSL -o /tmp/llvm-mingw.tar.xz "$ASSET_URL"
mkdir -p "$DEST"
tar xf /tmp/llvm-mingw.tar.xz -C "$DEST" --strip-components=1

echo "$DEST/bin"
