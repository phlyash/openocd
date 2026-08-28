#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TEST_ROOT="$(mktemp -d)"
trap 'rm -rf "$TEST_ROOT"' EXIT

OUTPUT="$TEST_ROOT/openocd_build_name.h"
EXPECTED="$TEST_ROOT/expected.h"

"$SCRIPT_DIR/set-build-name.sh" \
	"CH32 production + probe (rev.2)" \
	"$OUTPUT"

printf '%s\n' \
	'#define OPENOCD_BUILD_NAME " [CH32 production + probe (rev.2)]"' \
	> "$EXPECTED"
cmp "$EXPECTED" "$OUTPUT"

"$SCRIPT_DIR/set-build-name.sh" "" "$OUTPUT"
printf '%s\n' '#define OPENOCD_BUILD_NAME ""' > "$EXPECTED"
cmp "$EXPECTED" "$OUTPUT"

for invalid_name in 'quote"inside' 'backslash\inside' $'line\nbreak'; do
	if "$SCRIPT_DIR/set-build-name.sh" \
		"$invalid_name" \
		"$OUTPUT" \
		> /dev/null 2>&1
	then
		echo "accepted unsafe build name: $invalid_name" >&2
		exit 1
	fi
done
