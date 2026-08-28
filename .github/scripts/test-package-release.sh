#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TEST_ROOT="$(mktemp -d)"
trap 'rm -rf "$TEST_ROOT"' EXIT

STAGE="$TEST_ROOT/stage"
DIST="$TEST_ROOT/dist"
PACKAGE_NAME="openocd-test-x86_64"

mkdir -p "$STAGE/bin" "$STAGE/share/openocd/scripts"
printf 'binary\n' > "$STAGE/bin/openocd"
printf 'adapter config\n' > "$STAGE/share/openocd/scripts/adapter.cfg"

"$SCRIPT_DIR/package-release.sh" "$STAGE" "$DIST" "$PACKAGE_NAME"

test -f "$DIST/$PACKAGE_NAME.tar.gz"
test -f "$DIST/$PACKAGE_NAME.zip"

mkdir -p "$TEST_ROOT/from-tar" "$TEST_ROOT/from-zip"
tar -xzf "$DIST/$PACKAGE_NAME.tar.gz" -C "$TEST_ROOT/from-tar"
python3 -m zipfile -e \
	"$DIST/$PACKAGE_NAME.zip" \
	"$TEST_ROOT/from-zip"

for unpacked in "$TEST_ROOT/from-tar" "$TEST_ROOT/from-zip"; do
	mapfile -t roots < <(
		find "$unpacked" -mindepth 1 -maxdepth 1 -printf '%f\n' | sort
	)

	if [ "${roots[*]}" != "bin share" ]; then
		echo "unexpected archive roots in $unpacked: ${roots[*]}" >&2
		exit 1
	fi

	cmp "$STAGE/bin/openocd" "$unpacked/bin/openocd"
	cmp \
		"$STAGE/share/openocd/scripts/adapter.cfg" \
		"$unpacked/share/openocd/scripts/adapter.cfg"
done

mkdir -p "$STAGE/lib"
if "$SCRIPT_DIR/package-release.sh" \
	"$STAGE" \
	"$DIST" \
	rejected-package \
	> "$TEST_ROOT/rejected-package.log" 2>&1
then
	echo "packager accepted an unexpected top-level directory" >&2
	exit 1
fi
