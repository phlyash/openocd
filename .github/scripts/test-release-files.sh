#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TEST_ROOT="$(mktemp -d)"
trap 'rm -rf "$TEST_ROOT"' EXIT

DIST="$TEST_ROOT/dist"
mkdir -p "$DIST"

for file in \
	openocd-macos-aarch64.tar.gz \
	openocd-macos-aarch64.zip \
	openocd-linux-x86_64.tar.gz \
	openocd-linux-x86_64.zip \
	openocd-windows-x86_64.tar.gz \
	openocd-windows-x86_64.zip
do
	: > "$DIST/$file"
done

"$SCRIPT_DIR/verify-release-files.sh" "$DIST"

: > "$DIST/unexpected.txt"
if "$SCRIPT_DIR/verify-release-files.sh" "$DIST" > /dev/null 2>&1; then
	echo "accepted an unexpected release file" >&2
	exit 1
fi
rm -f "$DIST/unexpected.txt"

rm -f "$DIST/openocd-windows-x86_64.zip"
if "$SCRIPT_DIR/verify-release-files.sh" "$DIST" > /dev/null 2>&1; then
	echo "accepted a release with a missing archive" >&2
	exit 1
fi
