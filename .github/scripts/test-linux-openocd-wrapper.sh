#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TEST_ROOT="$(mktemp -d)"
trap 'rm -rf "$TEST_ROOT"' EXIT

mkdir -p "$TEST_ROOT/bin"
cp "$SCRIPT_DIR/linux-openocd-wrapper.sh" "$TEST_ROOT/bin/openocd"
chmod +x "$TEST_ROOT/bin/openocd"

cat > "$TEST_ROOT/bin/openocd.bin" <<'EOF'
#!/usr/bin/env bash
printf '%s\n' "$LD_LIBRARY_PATH"
EOF
chmod +x "$TEST_ROOT/bin/openocd.bin"

actual="$("$TEST_ROOT/bin/openocd")"
expected="$TEST_ROOT/bin"

if [ "$actual" != "$expected" ]; then
	echo "expected LD_LIBRARY_PATH=$expected, got $actual" >&2
	exit 1
fi
