#!/usr/bin/env bash
set -euo pipefail

if [ "$#" -ne 2 ]; then
	echo "usage: $0 BUILD_NAME OUTPUT_HEADER" >&2
	exit 2
fi

BUILD_NAME="$1"
OUTPUT_HEADER="$2"

if [[ "$BUILD_NAME" == *\"* || "$BUILD_NAME" == *\\* ]]; then
	echo "build name must not contain quotes or backslashes" >&2
	exit 1
fi

if [[ "$BUILD_NAME" == *$'\n'* || \
	"$BUILD_NAME" == *$'\r'* || \
	"$BUILD_NAME" == *$'\t'* ]]; then
	echo "build name must not contain control characters" >&2
	exit 1
fi

if [ -n "$BUILD_NAME" ]; then
	printf '#define OPENOCD_BUILD_NAME " [%s]"\n' \
		"$BUILD_NAME" \
		> "$OUTPUT_HEADER"
else
	printf '%s\n' '#define OPENOCD_BUILD_NAME ""' > "$OUTPUT_HEADER"
fi
