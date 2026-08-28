#!/usr/bin/env bash
set -euo pipefail

if [ "$#" -ne 1 ]; then
	echo "usage: $0 DIST_DIR" >&2
	exit 2
fi

DIST_DIR="$1"
EXPECTED_FILES=(
	openocd-macos-aarch64.tar.gz
	openocd-macos-aarch64.zip
	openocd-linux-x86_64.tar.gz
	openocd-linux-x86_64.zip
	openocd-windows-x86_64.tar.gz
	openocd-windows-x86_64.zip
)

if [ ! -d "$DIST_DIR" ]; then
	echo "release directory not found: $DIST_DIR" >&2
	exit 1
fi

shopt -s dotglob nullglob
release_files=("$DIST_DIR"/*)

if [ "${#release_files[@]}" -ne "${#EXPECTED_FILES[@]}" ]; then
	echo "expected exactly six release files, found ${#release_files[@]}" >&2
	exit 1
fi

for expected_file in "${EXPECTED_FILES[@]}"; do
	if [ ! -f "$DIST_DIR/$expected_file" ]; then
		echo "missing release file: $expected_file" >&2
		exit 1
	fi
done

echo "Verified six release archives in $DIST_DIR"
