# Multi-platform build CI — Design

Date: 2026-07-29
Status: Approved

## Goal

Add a GitHub Actions pipeline that builds this OpenOCD fork for five targets
— macOS, Linux x86_64, Linux arm64, Windows x86_64, Windows arm64 — on every
push to `master`, every pull request, and on manual dispatch, and publishes
each resulting binary as a downloadable workflow artifact.

The repo's only current remote is GitFlic (`gitflic.ru`); there is no GitHub
remote today. This workflow is added on the assumption the repo will be
pushed/mirrored to GitHub so the Actions definitions have somewhere to run.
No GitFlic-native CI is added by this design.

## Non-goals

- No GitHub Release / tag-based publishing — artifacts only.
- No code signing / notarization for macOS or Windows binaries.
- No test-suite execution (`make check` / board-in-the-loop tests) — this is
  build verification + binary distribution only.

## Trigger

- `push` to `master`
- `pull_request` (all branches)
- `workflow_dispatch` (manual)

## Job matrix

One workflow file: `.github/workflows/build.yml`, with five independent
jobs. `fail-fast: false` everywhere a matrix is used, so one platform
breaking does not cancel the others.

All jobs start with `actions/checkout@v4` using `submodules: recursive`
(the `jimtcl` submodule is a mandatory build dependency; `libjaylink` is
optional but built internally when present), then run:

```
./bootstrap nosubmodule
./configure [--host=<triplet>] [--prefix=/usr]
make -j
make install-strip DESTDIR=<stage-dir>
```

No explicit `--enable-*`/`--disable-*` adapter flags are passed anywhere —
`configure.ac`'s `PKG_CHECK_MODULES` calls auto-detect available libraries
and silently drop adapters whose dependency isn't present. Missing a
dependency is not a configure error unless a specific adapter is explicitly
force-enabled, which this design never does.

### Linux x86_64

- Runner: `ubuntu-latest`
- Container: `quay.io/pypa/manylinux2014_x86_64` (CentOS 7 base, glibc 2.17)
- Rationale: the oldest glibc baseline that still ships both x86_64 and
  arm64 images, so the resulting binary runs on the broadest range of
  modern Linux distros (same portability goal as a cross-compiler release
  build).
- Deps: `libusb-1.0`, `libftdi1`, `hidapi` are built from source inside the
  container at pinned versions (CentOS 7 / EPEL packages for these are
  either absent or too old); `autoconf`, `automake`, `libtool`,
  `pkgconfig`, `texinfo` come from the base CentOS 7 + EPEL repos.

### Linux arm64

- Runner: `ubuntu-24.04-arm` (GitHub-hosted free arm64 Linux runner)
- Container: `quay.io/pypa/manylinux2014_aarch64` — same base and same
  from-source dependency build as Linux x86_64.
- Note: `ubuntu-24.04-arm` free hosted runners currently require the repo
  to be public (or an org/plan with arm64 runner access). If unavailable,
  the fallback is cross-compiling from `ubuntu-latest` using an
  `aarch64-linux-gnu` toolchain instead of a native runner — not built by
  default in this design, called out here for future reference.

### macOS (Apple Silicon)

- Runner: `macos-14` (arm64-native)
- Deps via Homebrew: `libtool automake libusb libftdi hidapi texinfo pkg-config`
- Intel (x86_64) macOS is explicitly out of scope per your selection.

### Windows x86_64

- Runner: `windows-latest`
- Native build via MSYS2 mingw64 environment (`msys2/setup-msys2` action),
  mirroring the working recipe already in `.travis.yml`:
  `mingw-w64-x86_64-toolchain` plus `-libusb`, `-libftdi`, `-hidapi`,
  `-libjaylink-git`, and the usual autotools/pkg-config/texinfo packages.
- After build: `ldd` the resulting `openocd.exe`, copy any non-system
  (MSYS2-provided) DLLs it depends on into the same directory as the exe,
  so the artifact is a self-contained, ready-to-run zip.

### Windows arm64

- Runner: `ubuntu-latest` (cross-compile host)
- Toolchain: `llvm-mingw` targeting `aarch64-w64-mingw32`.
- Dependencies: only `libusb-1.0` is cross-built from source for this
  target (per your explicit choice) — `hidapi` and `libftdi` are *not*
  built, so HID-only and FTDI-based adapters are unavailable on this one
  platform; libusb-1.x-based adapters (ST-Link, CMSIS-DAP-v2, J-Link via
  libjaylink, most others) remain available.
- `libjaylink` is an internal submodule and cross-compiles as part of the
  normal OpenOCD build (`AX_CONFIG_SUBDIR`), no separate step needed.
- This is a real, non-`continue-on-error` job — a build failure here fails
  CI like any other platform, matching your "libusb only, but not
  best-effort" choice.
- Same DLL-bundling treatment as Windows x86_64 for the cross-built
  `openocd.exe` + `libusb-1.0.dll`.

## Artifacts

Each job stages its install into a directory and uploads it via
`actions/upload-artifact`, named:

- `openocd-macos-arm64`
- `openocd-linux-x86_64`
- `openocd-linux-arm64`
- `openocd-windows-x86_64`
- `openocd-windows-arm64`

Each artifact contains the staged `bin/` (and `share/openocd/scripts/`
config files needed at runtime) for that platform; Windows artifacts also
include the bundled runtime DLLs next to the exe.

## Error handling

- `fail-fast: false` on any matrix so independent platform failures are
  visible individually rather than cancelling sibling jobs.
- No job uses `continue-on-error` — every platform, including Windows
  arm64, is a required, blocking check.
- Dependency source builds (Linux from-source libusb/libftdi/hidapi,
  Windows arm64 libusb cross-build) pin exact upstream release versions/
  tags rather than tracking a moving branch, so a job failure means either
  an OpenOCD-side regression or an intentional version bump — not upstream
  drift breaking CI unannounced.

## Testing / validation

Since this is infrastructure with no existing CI to compare against,
validation is: push the workflow, confirm all five jobs go green on a
manual `workflow_dispatch` run, download each artifact, and run
`openocd --version` (or the platform-appropriate equivalent, e.g. via
Wine or an actual ARM64 Windows machine for that target if available) to
confirm the binary actually executes and reports adapter support matching
what each job's dependency set implies (e.g. Windows arm64 binary should
list libusb-based adapters and should *not* list hidapi/ftdi-only ones).
