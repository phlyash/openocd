#!/usr/bin/env bash
set -euo pipefail

if [ "$#" -ne 3 ]; then
	echo "usage: $0 STAGE_ROOT OUTPUT_DIR PACKAGE_NAME" >&2
	exit 2
fi

STAGE_ROOT="$(cd "$1" && pwd -P)"
mkdir -p "$2"
OUTPUT_DIR="$(cd "$2" && pwd -P)"
PACKAGE_NAME="$3"

if [[ ! "$PACKAGE_NAME" =~ ^[A-Za-z0-9][A-Za-z0-9._-]*$ ]]; then
	echo "invalid package name: $PACKAGE_NAME" >&2
	exit 2
fi

for required_dir in bin share; do
	if [ ! -d "$STAGE_ROOT/$required_dir" ]; then
		echo "missing required package directory: $required_dir" >&2
		exit 1
	fi
done

shopt -s dotglob nullglob
for entry in "$STAGE_ROOT"/*; do
	case "$(basename "$entry")" in
		bin|share)
			;;
		*)
			echo "unexpected package root entry: $entry" >&2
			exit 1
			;;
	esac
done

TAR_ARCHIVE="$OUTPUT_DIR/$PACKAGE_NAME.tar.gz"
ZIP_ARCHIVE="$OUTPUT_DIR/$PACKAGE_NAME.zip"

rm -f "$TAR_ARCHIVE" "$ZIP_ARCHIVE"
tar -C "$STAGE_ROOT" -czf "$TAR_ARCHIVE" bin share
(
	cd "$STAGE_ROOT"
	zip -q -r "$ZIP_ARCHIVE" bin share
)

echo "Created $TAR_ARCHIVE"
echo "Created $ZIP_ARCHIVE"
