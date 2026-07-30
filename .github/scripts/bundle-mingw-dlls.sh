#!/usr/bin/env bash
# Copies every MSYS2/mingw-provided DLL an .exe in $1 depends on into $1,
# so the resulting directory is runnable without an MSYS2 install present.
# Run from an MSYS2 shell (needs `ldd` and MSYSTEM set, e.g. MINGW64).
set -euo pipefail

BIN_DIR="$1"

shopt -s nullglob
exes=("$BIN_DIR"/*.exe)
shopt -u nullglob

if [ "${#exes[@]}" -eq 0 ]; then
  echo "No .exe found in $BIN_DIR" >&2
  exit 1
fi

dlls=$(
  for exe in "${exes[@]}"; do
    # `|| true` tolerates one exe having no mingw-provided deps while
    # another does; the aggregate check below catches a total failure.
    ldd "$exe" | awk '{print $3}' | grep -iE '^/(mingw64|clang64|ucrt64)/bin/' || true
  done | sort -u
)

if [ -z "$dlls" ]; then
  echo "No mingw-provided DLL dependencies found for any .exe in $BIN_DIR." >&2
  echo "A real openocd.exe always links at least one, so this means the" >&2
  echo "dependency scan itself failed (ldd missing, wrong MSYSTEM, ...)." >&2
  exit 1
fi

while read -r dll; do
  cp -u "$dll" "$BIN_DIR/"
done <<< "$dlls"
