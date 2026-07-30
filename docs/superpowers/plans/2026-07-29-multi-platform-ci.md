# Multi-platform Build CI Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a GitHub Actions workflow (`.github/workflows/build.yml`) that builds this OpenOCD fork for macOS (arm64), Linux (x86_64 + arm64), and Windows (x86_64 + arm64), uploading each binary as a downloadable workflow artifact on every push to `master`, every PR, and manual dispatch.

**Architecture:** One workflow file with five jobs (macOS, a 2-way Linux matrix, and two independent Windows jobs). Platform-specific shell logic that's non-trivial (Linux from-source dependency builds, Windows DLL bundling, the Windows-arm64 cross toolchain) lives in standalone scripts under `.github/scripts/` so it can be run and debugged outside of a GitHub Actions run.

**Tech Stack:** GitHub Actions, bash, autotools (this project's existing `./bootstrap`/`./configure`/`make`), CMake (for libftdi1/hidapi), Docker (for local validation of the Linux jobs, which run inside containers), `msys2/setup-msys2` action (Windows x86_64), `llvm-mingw` (Windows arm64 cross-compiler).

## Global Constraints

These are copied verbatim from `docs/superpowers/specs/2026-07-29-multi-platform-ci-design.md` and apply to every task below:

- Triggers: `push` to `master`, `pull_request` (all branches), `workflow_dispatch`. No GitHub Release/tag publishing — artifacts only.
- `fail-fast: false` on any matrix; **no** job uses `continue-on-error` — every platform, including Windows arm64, is a required blocking check.
- No explicit `--enable-*`/`--disable-*` OpenOCD adapter flags anywhere — rely on `configure`'s auto-detection of available libraries.
- Linux jobs build inside `quay.io/pypa/manylinux2014_x86_64` / `quay.io/pypa/manylinux2014_aarch64` (CentOS 7 base, glibc 2.17) for maximum binary portability.
- macOS target is Apple Silicon (arm64) only, via `macos-14`.
- Windows arm64: cross-compiled from `ubuntu-latest` via `llvm-mingw` targeting `aarch64-w64-mingw32`; only `libusb-1.0` is built for this target (no hidapi/libftdi) — HID-only and FTDI-based adapters are unavailable there by design.
- Windows artifacts (both x86_64 and arm64) must be self-contained: bundle the runtime DLLs the built `openocd.exe` depends on.
- Checkout uses `submodules: recursive` (the `jimtcl` submodule is a mandatory build dependency) and `fetch-depth: 0` (so `git describe` — used by `guess-rev.sh` to stamp `openocd --version` — has tag history to work with).
- Artifact names: `openocd-macos-arm64`, `openocd-linux-x86_64`, `openocd-linux-arm64`, `openocd-windows-x86_64`, `openocd-windows-arm64`.

---

### Task 1: Workflow scaffold + macOS (arm64) job

**Files:**
- Create: `.github/workflows/build.yml`

**Interfaces:**
- Produces: the workflow file with top-level `on:` triggers and a `jobs.macos-arm64` job. Later tasks append sibling jobs (`jobs.linux`, `jobs.windows-x86_64`, `jobs.windows-arm64`) to this same file, each inserted as a new top-level key under `jobs:`.

This is the only job fully buildable and testable on this development machine (native arm64 macOS, and Homebrew already has every dependency this job needs), so it's the first task and also the template the later jobs follow.

- [ ] **Step 1: Install actionlint for local workflow validation**

```bash
brew install actionlint
```

- [ ] **Step 2: Create the workflow file**

Create `.github/workflows/build.yml`:

```yaml
name: Build

on:
  push:
    branches: [master]
  pull_request:
  workflow_dispatch:

jobs:
  macos-arm64:
    name: macOS (arm64)
    runs-on: macos-14
    steps:
      - uses: actions/checkout@v4
        with:
          submodules: recursive
          fetch-depth: 0

      - name: Install dependencies
        run: brew install libtool automake autoconf pkg-config texinfo libusb libftdi hidapi

      - name: Bootstrap
        run: ./bootstrap nosubmodule

      - name: Configure
        run: ./configure --prefix=/usr/local

      - name: Build
        run: make -j"$(sysctl -n hw.ncpu)"

      - name: Smoke test
        run: ./src/openocd --version

      - name: Stage install
        run: make install-strip DESTDIR="$(pwd)/stage"

      - name: Upload artifact
        uses: actions/upload-artifact@v4
        with:
          name: openocd-macos-arm64
          path: stage/usr/local
```

- [ ] **Step 3: Lint the workflow**

Run: `actionlint .github/workflows/build.yml`
Expected: no output (no findings).

- [ ] **Step 4: Run the macOS job's steps locally**

This repo's submodules aren't checked out yet in a fresh clone — initialize them, then run exactly what the job runs:

```bash
git submodule update --init --recursive
brew install libtool automake autoconf pkg-config texinfo libusb libftdi hidapi
./bootstrap nosubmodule
./configure --prefix=/usr/local
make -j"$(sysctl -n hw.ncpu)"
./src/openocd --version
```

Expected: `make` completes without error, and `./src/openocd --version` prints an `Open On-Chip Debugger` version line (the exact string doesn't matter — what matters is it runs and exits 0, proving the configure/build/link chain works end-to-end).

- [ ] **Step 5: Clean build artifacts before committing**

Autotools build output shouldn't be committed. Confirm `.gitignore` already covers it:

```bash
git status --short
```

Expected: only `.github/workflows/build.yml` shows as untracked/new — no `Makefile`, `*.o`, `src/openocd`, or `jimtcl/` build output listed. If build artifacts do show up, add the missing paths to `.gitignore` before committing (check `.gitignore` first — most autotools output is already covered).

- [ ] **Step 6: Commit**

```bash
git add .github/workflows/build.yml
git commit -m "ci: add macOS (arm64) build job"
```

---

### Task 2: Linux job (x86_64 + arm64 matrix, manylinux2014)

**Files:**
- Create: `.github/scripts/build-linux-deps.sh`
- Modify: `.github/workflows/build.yml` — add `jobs.linux`

**Interfaces:**
- Consumes: nothing from Task 1 beyond the existing workflow file structure.
- Produces: `.github/scripts/build-linux-deps.sh` — takes no arguments, run as `bash .github/scripts/build-linux-deps.sh` from any working directory inside a manylinux2014 container. Leaves `libusb-1.0`, `libftdi1`, and `hidapi` (hidraw backend) built and installed under `/usr/local` with their `.pc` files on `PKG_CONFIG_PATH=/usr/local/lib/pkgconfig`.

**Local validation for this task requires Docker Desktop running.** Start it now if it isn't:

```bash
open -a Docker
until docker info >/dev/null 2>&1; do sleep 2; done
```

- [ ] **Step 1: Write the dependency-build script**

Create `.github/scripts/build-linux-deps.sh`:

```bash
#!/usr/bin/env bash
# Builds libusb-1.0, libftdi1, and hidapi from source into /usr/local
# inside a manylinux2014 (CentOS 7 / glibc 2.17) container, since that
# image's own package repos don't carry current-enough versions of these.
set -euo pipefail

if [ -f /opt/rh/devtoolset-10/enable ]; then
  # shellcheck disable=SC1091
  source /opt/rh/devtoolset-10/enable
fi

yum install -y epel-release
yum install -y \
  autoconf automake libtool pkgconfig texinfo cmake3 \
  libudev-devel git wget curl tar bzip2 xz \
  gcc gcc-c++ make

export PKG_CONFIG_PATH=/usr/local/lib/pkgconfig
export LD_LIBRARY_PATH=/usr/local/lib

WORK=/tmp/linux-deps-build
mkdir -p "$WORK"
cd "$WORK"

# libusb 1.0.27
curl -fsSL -o libusb.tar.bz2 \
  https://github.com/libusb/libusb/releases/download/v1.0.27/libusb-1.0.27.tar.bz2
tar xf libusb.tar.bz2
(
  cd libusb-1.0.27
  ./configure --prefix=/usr/local
  make -j"$(nproc)"
  make install
)

# libftdi1 1.5
curl -fsSL -o libftdi1.tar.bz2 \
  https://www.intra2net.com/en/developer/libftdi/download/libftdi1-1.5.tar.bz2
tar xf libftdi1.tar.bz2
mkdir -p libftdi1-1.5/build
(
  cd libftdi1-1.5/build
  cmake3 -DCMAKE_INSTALL_PREFIX=/usr/local -DCMAKE_BUILD_TYPE=Release -DFTDI_EEPROM=OFF ..
  make -j"$(nproc)"
  make install
)

# hidapi 0.14.0
curl -fsSL -o hidapi.tar.gz \
  https://github.com/libusb/hidapi/archive/refs/tags/hidapi-0.14.0.tar.gz
tar xf hidapi.tar.gz
mkdir -p hidapi-hidapi-0.14.0/build
(
  cd hidapi-hidapi-0.14.0/build
  cmake3 -DCMAKE_INSTALL_PREFIX=/usr/local -DCMAKE_BUILD_TYPE=Release ..
  make -j"$(nproc)"
  make install
)

ldconfig
```

```bash
chmod +x .github/scripts/build-linux-deps.sh
```

- [ ] **Step 2: Run the script inside a manylinux2014 x86_64 container to verify it works**

```bash
docker run --rm --platform linux/amd64 \
  -v "$(pwd)":/src -w /src \
  quay.io/pypa/manylinux2014_x86_64 \
  bash .github/scripts/build-linux-deps.sh
```

Expected: exits 0. If a `yum install` package name is wrong for this image (package names on CentOS 7 do shift over EPEL versions), the error will name the missing package — fix it in the script and re-run.

- [ ] **Step 3: Run the same script on arm64 to verify it's arch-independent**

This machine is Apple Silicon, so this runs natively — no emulation:

```bash
docker run --rm --platform linux/arm64 \
  -v "$(pwd)":/src -w /src \
  quay.io/pypa/manylinux2014_aarch64 \
  bash .github/scripts/build-linux-deps.sh
```

Expected: exits 0.

- [ ] **Step 4: Add the Linux job to the workflow**

Add this as a new top-level entry under `jobs:` in `.github/workflows/build.yml`, after `macos-arm64:`:

```yaml
  linux:
    name: Linux (${{ matrix.arch }}, glibc 2.17)
    runs-on: ${{ matrix.runner }}
    container: ${{ matrix.image }}
    strategy:
      fail-fast: false
      matrix:
        include:
          - arch: x86_64
            runner: ubuntu-latest
            image: quay.io/pypa/manylinux2014_x86_64
          - arch: arm64
            runner: ubuntu-24.04-arm
            image: quay.io/pypa/manylinux2014_aarch64
    steps:
      - uses: actions/checkout@v4
        with:
          submodules: recursive
          fetch-depth: 0

      - name: Install build dependencies
        run: bash .github/scripts/build-linux-deps.sh

      - name: Bootstrap
        run: ./bootstrap nosubmodule

      - name: Configure
        run: |
          export PKG_CONFIG_PATH=/usr/local/lib/pkgconfig
          export LDFLAGS='-Wl,-rpath,$ORIGIN/../lib'
          ./configure --prefix=/usr/local

      - name: Build
        run: make -j"$(nproc)"

      - name: Smoke test
        run: |
          export LD_LIBRARY_PATH=/usr/local/lib
          ./src/openocd --version

      - name: Stage install
        run: make install-strip DESTDIR="$(pwd)/stage"

      - name: Bundle custom-built runtime libraries
        run: |
          mkdir -p stage/usr/local/lib
          cp -a /usr/local/lib/libusb-1.0.so* stage/usr/local/lib/
          cp -a /usr/local/lib/libftdi1.so* stage/usr/local/lib/
          cp -a /usr/local/lib/libhidapi-hidraw.so* stage/usr/local/lib/

      - name: Verify staged binary runs without external library help
        run: |
          cd stage/usr/local/bin
          env -u LD_LIBRARY_PATH ./openocd --version

      - name: Upload artifact
        uses: actions/upload-artifact@v4
        with:
          name: openocd-linux-${{ matrix.arch }}
          path: stage/usr/local
```

The `LDFLAGS='-Wl,-rpath,$ORIGIN/../lib'` (single-quoted — `$ORIGIN` is a linker runtime token, not a shell variable, and must reach the linker literally) makes the built binary find its bundled `.so` files relative to its own location, so downloaders don't need to set `LD_LIBRARY_PATH` themselves. The "Verify staged binary runs without external library help" step proves this by explicitly unsetting `LD_LIBRARY_PATH` before running the staged binary.

- [ ] **Step 5: Lint the workflow**

Run: `actionlint .github/workflows/build.yml`
Expected: no output.

- [ ] **Step 6: Run the full job end-to-end locally (x86_64 leg)**

```bash
docker run --rm --platform linux/amd64 \
  -v "$(pwd)":/src -w /src \
  quay.io/pypa/manylinux2014_x86_64 \
  bash -c '
    set -e
    bash .github/scripts/build-linux-deps.sh
    ./bootstrap nosubmodule
    export PKG_CONFIG_PATH=/usr/local/lib/pkgconfig
    export LDFLAGS="-Wl,-rpath,\$ORIGIN/../lib"
    ./configure --prefix=/usr/local
    make -j"$(nproc)"
    make install-strip DESTDIR=/src/stage
    mkdir -p /src/stage/usr/local/lib
    cp -a /usr/local/lib/libusb-1.0.so* /src/stage/usr/local/lib/
    cp -a /usr/local/lib/libftdi1.so* /src/stage/usr/local/lib/
    cp -a /usr/local/lib/libhidapi-hidraw.so* /src/stage/usr/local/lib/
    cd /src/stage/usr/local/bin
    env -u LD_LIBRARY_PATH ./openocd --version
  '
```

Expected: the last line of output is an `Open On-Chip Debugger` version banner, proving the staged, rpath-linked binary runs standalone.

- [ ] **Step 7: Clean up local build output**

```bash
rm -rf stage
git status --short
```

Expected: only the two new/modified files from this task show up.

- [ ] **Step 8: Commit**

```bash
git add .github/workflows/build.yml .github/scripts/build-linux-deps.sh
git commit -m "ci: add Linux x86_64/arm64 build job (manylinux2014, glibc 2.17)"
```

---

### Task 3: Windows x86_64 job (native, MSYS2)

**Files:**
- Create: `.github/scripts/bundle-mingw-dlls.sh`
- Modify: `.github/workflows/build.yml` — add `jobs.windows-x86_64`

**Interfaces:**
- Produces: `.github/scripts/bundle-mingw-dlls.sh <bin-dir>` — run from an MSYS2 mingw64 shell after `openocd.exe` is built; copies every non-system DLL `openocd.exe` (or any other `.exe` in `<bin-dir>`) depends on into `<bin-dir>` itself.

This job runs on Windows via MSYS2, which isn't available on this macOS development machine — it can't be executed locally. Validation for this task is limited to YAML linting and a careful read-through; full validation happens in Task 5 once this is actually pushed and run on a `windows-latest` runner.

- [ ] **Step 1: Write the DLL-bundling script**

Create `.github/scripts/bundle-mingw-dlls.sh`:

```bash
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

for exe in "${exes[@]}"; do
  ldd "$exe" | awk '{print $3}' | grep -iE '^/(mingw64|clang64|ucrt64)/bin/' || true
done | sort -u | while read -r dll; do
  cp -u "$dll" "$BIN_DIR/"
done
```

```bash
chmod +x .github/scripts/bundle-mingw-dlls.sh
```

- [ ] **Step 2: Add the Windows x86_64 job to the workflow**

Add this as a new top-level entry under `jobs:` in `.github/workflows/build.yml`, after `linux:`:

```yaml
  windows-x86_64:
    name: Windows (x86_64)
    runs-on: windows-latest
    defaults:
      run:
        shell: msys2 {0}
    steps:
      - uses: actions/checkout@v4
        with:
          submodules: recursive
          fetch-depth: 0

      - uses: msys2/setup-msys2@v2
        with:
          msystem: MINGW64
          update: true
          install: >-
            mingw-w64-x86_64-toolchain
            mingw-w64-x86_64-libusb
            mingw-w64-x86_64-libusb-compat-git
            mingw-w64-x86_64-libftdi
            mingw-w64-x86_64-hidapi
            mingw-w64-x86_64-libjaylink-git
            mingw-w64-x86_64-pkg-config
            autoconf
            autoconf-archive
            automake
            automake-wrapper
            binutils
            gettext
            git
            libtool
            m4
            make
            tcl
            texinfo

      - name: Bootstrap
        run: ./bootstrap nosubmodule

      - name: Configure
        run: ./configure --prefix=/mingw64

      - name: Build
        run: make -j"$(nproc)"

      - name: Smoke test
        run: ./src/openocd.exe --version

      - name: Stage install
        run: make install-strip DESTDIR="$(pwd)/stage"

      - name: Bundle runtime DLLs
        run: bash .github/scripts/bundle-mingw-dlls.sh "$(pwd)/stage/mingw64/bin"

      - name: Upload artifact
        uses: actions/upload-artifact@v4
        with:
          name: openocd-windows-x86_64
          path: stage/mingw64
```

- [ ] **Step 3: Lint the workflow**

Run: `actionlint .github/workflows/build.yml`
Expected: no output.

- [ ] **Step 4: Review the bundling script logic by hand**

Since this can't run locally, trace it manually against a known `ldd` output shape. MSYS2's `ldd` on a mingw64 binary prints lines like:

```
        libusb-1.0.dll => /mingw64/bin/libusb-1.0.dll (0x...)
        KERNEL32.dll => /c/Windows/System32/KERNEL32.dll (0x...)
```

Confirm: `awk '{print $3}'` extracts column 3 (the path) from each such line, the `grep -iE '^/(mingw64|clang64|ucrt64)/bin/'` keeps only the `/mingw64/bin/...` line and drops the `/c/Windows/System32/...` line, and `cp -u` only copies files newer/missing versus the destination (safe to run more than once). This matches the intent: bundle mingw-provided DLLs, skip Windows system DLLs.

- [ ] **Step 5: Commit**

```bash
git add .github/workflows/build.yml .github/scripts/bundle-mingw-dlls.sh
git commit -m "ci: add Windows x86_64 build job (native, MSYS2)"
```

---

### Task 4: Windows arm64 job (cross-compiled, libusb only)

**Files:**
- Create: `.github/scripts/setup-llvm-mingw.sh`
- Create: `.github/scripts/build-libusb-mingw-arm64.sh`
- Modify: `.github/workflows/build.yml` — add `jobs.windows-arm64`

**Interfaces:**
- Produces: `.github/scripts/setup-llvm-mingw.sh <dest-dir>` — downloads and extracts the latest `llvm-mingw` release into `<dest-dir>`, prints `<dest-dir>/bin` on stdout (the directory to add to `PATH`).
- Produces: `.github/scripts/build-libusb-mingw-arm64.sh <sysroot-dir>` — cross-builds libusb-1.0 for `aarch64-w64-mingw32` (requires the llvm-mingw toolchain already on `PATH`), installing it under `<sysroot-dir>/usr`.

Both scripts only touch a `linux/amd64` toolchain cross-compiling to a *different* target (Windows arm64) — the produced binaries are never executed during the build, only compiled and, where possible, sanity-checked with `file`. This means both scripts are fully testable in a plain Ubuntu container on this Mac, no emulation needed for the parts that run.

- [ ] **Step 1: Write the llvm-mingw setup script**

Create `.github/scripts/setup-llvm-mingw.sh`:

```bash
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
```

```bash
chmod +x .github/scripts/setup-llvm-mingw.sh
```

- [ ] **Step 2: Verify the toolchain download and target support locally**

```bash
docker run --rm -v "$(pwd)":/src -w /src ubuntu:24.04 bash -c '
  set -e
  apt-get update -qq && apt-get install -y -qq curl xz-utils >/dev/null
  BIN_DIR=$(bash .github/scripts/setup-llvm-mingw.sh /opt/llvm-mingw)
  export PATH="$BIN_DIR:$PATH"
  aarch64-w64-mingw32-clang --version
'
```

Expected: prints a clang version banner (proves the download, extraction, and that an `aarch64-w64-mingw32` target compiler exists on `PATH`).

- [ ] **Step 3: Write the libusb cross-build script**

Create `.github/scripts/build-libusb-mingw-arm64.sh`:

```bash
#!/usr/bin/env bash
# Cross-builds libusb-1.0 as a DLL for aarch64-w64-mingw32 using llvm-mingw
# (must already be on PATH), installing it under $1/usr.
set -euo pipefail

TRIPLE=aarch64-w64-mingw32
SYSROOT="$1"

mkdir -p "$SYSROOT"
WORK=/tmp/libusb-arm64-build
mkdir -p "$WORK"
cd "$WORK"

curl -fsSL -o libusb.tar.bz2 \
  https://github.com/libusb/libusb/releases/download/v1.0.27/libusb-1.0.27.tar.bz2
tar xf libusb.tar.bz2
cd libusb-1.0.27

CC="$TRIPLE-clang" ./configure --host="$TRIPLE" --prefix=/usr
make -j"$(nproc)"
make install DESTDIR="$SYSROOT"
```

```bash
chmod +x .github/scripts/build-libusb-mingw-arm64.sh
```

- [ ] **Step 4: Verify the libusb cross-build locally**

```bash
docker run --rm -v "$(pwd)":/src -w /src ubuntu:24.04 bash -c '
  set -e
  apt-get update -qq && apt-get install -y -qq curl xz-utils autoconf automake libtool file >/dev/null
  BIN_DIR=$(bash .github/scripts/setup-llvm-mingw.sh /opt/llvm-mingw)
  export PATH="$BIN_DIR:$PATH"
  bash .github/scripts/build-libusb-mingw-arm64.sh /tmp/sysroot
  file /tmp/sysroot/usr/bin/libusb-1.0.dll
'
```

Expected: `file` reports a `PE32+ executable ... Aarch64 ... (DLL)`.

- [ ] **Step 5: Add the Windows arm64 job to the workflow**

Add this as a new top-level entry under `jobs:` in `.github/workflows/build.yml`, after `windows-x86_64:`:

```yaml
  windows-arm64:
    name: Windows (arm64, cross-compiled, libusb only)
    runs-on: ubuntu-latest
    env:
      TRIPLE: aarch64-w64-mingw32
      SYSROOT: /tmp/arm64-sysroot
    steps:
      - uses: actions/checkout@v4
        with:
          submodules: recursive
          fetch-depth: 0

      - name: Install cross-compile prerequisites
        run: sudo apt-get update && sudo apt-get install -y autoconf automake libtool pkg-config texinfo

      - name: Install llvm-mingw
        run: |
          BIN_DIR=$(bash .github/scripts/setup-llvm-mingw.sh /tmp/llvm-mingw)
          echo "$BIN_DIR" >> "$GITHUB_PATH"

      - name: Cross-build libusb-1.0
        run: bash .github/scripts/build-libusb-mingw-arm64.sh "$SYSROOT"

      - name: Bootstrap
        run: ./bootstrap nosubmodule

      - name: Configure
        run: |
          export LIBUSB1_CFLAGS="-I$SYSROOT/usr/include/libusb-1.0"
          export LIBUSB1_LIBS="-L$SYSROOT/usr/lib -lusb-1.0"
          export CC="$TRIPLE-clang"
          ./configure --host="$TRIPLE" --prefix=/usr

      - name: Build
        run: make -j"$(nproc)"

      - name: Verify binary architecture
        run: file src/openocd.exe | tee /dev/stderr | grep -q 'Aarch64'

      - name: Stage install
        run: make install-strip DESTDIR="$(pwd)/stage"

      - name: Bundle libusb DLL
        run: cp "$SYSROOT/usr/bin/libusb-1.0.dll" "$(pwd)/stage/usr/bin/"

      - name: Upload artifact
        uses: actions/upload-artifact@v4
        with:
          name: openocd-windows-arm64
          path: stage/usr
```

`LIBUSB1_CFLAGS`/`LIBUSB1_LIBS` are set directly (bypassing `pkg-config`) because `configure.ac`'s `PKG_CHECK_MODULES([LIBUSB1], [libusb-1.0], ...)` uses these pre-set variables instead of invoking `pkg-config` when both are present — this avoids needing a cross pkg-config wrapper for a single dependency. `hidapi`/`libftdi` aren't installed for this target, so `configure` auto-detects their absence and silently disables the adapters that need them (per the "no explicit enable/disable flags" global constraint).

- [ ] **Step 6: Lint the workflow**

Run: `actionlint .github/workflows/build.yml`
Expected: no output.

- [ ] **Step 7: Run the full job's build steps locally (everything short of running the Windows binary)**

```bash
docker run --rm -v "$(pwd)":/src -w /src ubuntu:24.04 bash -c '
  set -e
  apt-get update -qq
  apt-get install -y -qq curl xz-utils autoconf automake libtool pkg-config texinfo file git >/dev/null
  git config --global --add safe.directory /src
  git submodule update --init --recursive
  BIN_DIR=$(bash .github/scripts/setup-llvm-mingw.sh /opt/llvm-mingw)
  export PATH="$BIN_DIR:$PATH"
  export TRIPLE=aarch64-w64-mingw32
  export SYSROOT=/tmp/arm64-sysroot
  bash .github/scripts/build-libusb-mingw-arm64.sh "$SYSROOT"
  ./bootstrap nosubmodule
  export LIBUSB1_CFLAGS="-I$SYSROOT/usr/include/libusb-1.0"
  export LIBUSB1_LIBS="-L$SYSROOT/usr/lib -lusb-1.0"
  export CC="$TRIPLE-clang"
  ./configure --host="$TRIPLE" --prefix=/usr
  make -j"$(nproc)"
  file src/openocd.exe | grep -q "Aarch64"
  make install-strip DESTDIR=/src/stage
  cp "$SYSROOT/usr/bin/libusb-1.0.dll" /src/stage/usr/bin/
  ls -la /src/stage/usr/bin
  echo CROSS BUILD OK
'
```

Expected: ends with `CROSS BUILD OK`, and the `ls` output shows `openocd.exe` and `libusb-1.0.dll` side by side in `stage/usr/bin`.

- [ ] **Step 8: Clean up local build output**

```bash
rm -rf stage
git status --short
```

Expected: only this task's three new/modified files show up.

- [ ] **Step 9: Commit**

```bash
git add .github/workflows/build.yml .github/scripts/setup-llvm-mingw.sh .github/scripts/build-libusb-mingw-arm64.sh
git commit -m "ci: add Windows arm64 build job (cross-compiled via llvm-mingw, libusb only)"
```

---

### Task 5: Push and verify all five jobs on GitHub Actions

**Files:** none (verification only).

This repo's only current remote is GitFlic (`gitflic.ru`) — there is no GitHub remote yet, and this environment has no `gh` CLI installed or authenticated. This task cannot be carried out inside this sandboxed session; it requires the repo owner's own GitHub access. Hand this task to whoever has that access (may be you, running these commands yourself in a terminal with `gh` installed and authenticated).

- [ ] **Step 1: Ensure the repo has a GitHub remote**

```bash
git remote -v
```

If there's no `github` remote, add one pointing at wherever this fork should live on GitHub, e.g.:

```bash
git remote add github git@github.com:<org>/<repo>.git
```

- [ ] **Step 2: Push the branch containing this plan's commits**

```bash
git push github HEAD
```

- [ ] **Step 3: Trigger the workflow**

Either open a pull request against this branch, or dispatch it manually:

```bash
gh workflow run build.yml --repo <org>/<repo> --ref <branch>
```

- [ ] **Step 4: Watch the run and confirm all five jobs pass**

```bash
gh run watch --repo <org>/<repo>
```

Expected: `macos-arm64`, `linux (x86_64, glibc 2.17)`, `linux (arm64, glibc 2.17)`, `windows-x86_64`, and `windows-arm64` all report success. Per the global constraint, none of these are `continue-on-error` — a red job here is a real failure to fix, not something to wave through.

- [ ] **Step 5: Download and spot-check each artifact**

```bash
gh run download --repo <org>/<repo> <run-id>
```

For the macOS, Linux, and Windows-x86_64 artifacts (all runnable on hardware you plausibly have access to): run the `openocd` / `openocd.exe` binary with `--version` and confirm it prints a version banner rather than erroring on a missing shared library.

For `openocd-windows-arm64`, since it can only run on actual Windows-on-arm64 hardware: at minimum confirm the archive contains both `openocd.exe` and `libusb-1.0.dll` side by side. If Windows-on-arm64 hardware is available, run `openocd.exe --version` there too as the final confirmation.

- [ ] **Step 6: Record any deviations**

If any package name, download URL, or tool version pinned in the scripts turned out to be stale or unavailable on the actual GitHub-hosted runners (a real possibility — package repos and release assets move over time), fix it in the relevant script or job step, commit the fix, and re-run from Step 3.
