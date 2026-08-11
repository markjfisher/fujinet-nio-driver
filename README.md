# FujiNet NIO Driver

This repository contains native operating-system drivers for the FujiNet NIO
protocol:

- the MS-DOS `FUJINET.SYS` driver; and
- the Amiga `fujinet-disk.device` driver for standard 880 KiB ADF media.

The Amiga driver consumes the typed DiskDevice client and RS-232 session from
the separate `fujinet-nio-lib` repository. It does not require the larger
FujiNet NIO workspace.

## Repository layout

```text
msdos/
  include/            DOS-facing public headers
  src/                MS-DOS driver and NIO protocol implementation
  tests/              host-side protocol tests
common/               reserved for future shared driver-side interfaces
amiga/                Amiga Exec device, MountList, tools, and tests
```

Generated artifacts are placed under `build/dos/` and `build/amiga/`.

## Repository dependency layout

The default Amiga build expects the driver and library to be sibling
directories. This is suitable for adding both as submodules at the root of
another repository:

```text
parent-repository/
  fujinet-nio-driver/
  fujinet-nio-lib/
```

Example:

```bash
git submodule add https://github.com/markjfisher/fujinet-nio-lib.git fujinet-nio-lib
git submodule add https://github.com/markjfisher/fujinet-nio-driver.git fujinet-nio-driver
git submodule update --init --recursive
```

The driver revision containing this contract is compatible with
`fujinet-nio-lib` commit `247340f2` or later. A parent repository should pin
both submodules to reviewed revisions.

For another layout, pass an absolute or relative library path:

```bash
make amiga LIB_ROOT=/path/to/fujinet-nio-lib
```

## MS-DOS build

The DOS driver build uses Open Watcom, so ensure it is on the path, e.g.

```sh
export WATCOM=/opt/watcom
export EDPATH=$WATCOM/eddat
export INCLUDE=$WATCOM/h
export PATH=$PATH:$WATCOM/binl64:$WATCOM/binl

make
```

The generated driver is `build/dos/fujinet.sys`.

## Amiga build

Requirements:

- [amiga-gcc](https://github.com/bebbo/amiga-gcc), including
  `m68k-amigaos-gcc` and binutils;
- readable NDK headers, `amiga.lib`, and clib2 in that toolchain;
- GNU Make; and
- host GCC for the portable driver tests.

For a typical `/opt/amiga` installation:

```bash
export PATH=/opt/amiga/bin:$PATH
make amiga
```

This builds the required resident library variant from the sibling
`fujinet-nio-lib` checkout, runs the portable tests, and produces:

```text
build/amiga/fujinet-disk.device
build/amiga/fujinet-mount
```

The root `make` builds both MS-DOS and Amiga and therefore requires both
cross-toolchains. Consumers interested only in Amiga should use `make amiga`.

## Tests

Run all portable driver tests with:

```sh
make tests
```

Run only the Amiga tests with:

```sh
make amiga-tests
```

The Amiga tests build the production adapter against
`fujinet-nio-lib/build/fujinet-nio-linux.a`, so host GCC is required. Native
Amiga compilation is performed by `make amiga`.

## Amiga installation and first mount

Install the generated files and supplied MountList entry on the Amiga:

```text
build/amiga/fujinet-disk.device  -> DEVS:fujinet-disk.device
build/amiga/fujinet-mount        -> C:fujinet-mount
amiga/config/DN0 .. DN7          -> DEVS:DN0 .. DEVS:DN7
```

The initial startup sequence is:

```text
C:LoadModule DEVS:fujinet-disk.device
C:fujinet-mount host:/standard.adf
C:Mount DN0: FROM DEVS:DN0
```

`host:/standard.adf` is a FujiNet NIO URI and must exist beneath the NIO
server's configured `host:` root. A successful mount reports slot 1,
read-only mode, 512-byte sectors, and 1760 sectors. `DN0:` can then be used
through normal AmigaDOS commands such as `Dir DN0:` and `Type DN0:file`.
The normal FujiNet form resolves a persistent catalogue slot into an active
drive and records the shared mapping:

```text
fujinet-mount 12 0 RO
fujinet-mount 37 1 RW
Mount DN0: FROM DEVS:DN0
Mount DN1: FROM DEVS:DN1
```

Amiga drive N maps to DiskDevice slot N+1. `fujinet-mount --eject N` ejects
that drive and clears its mapping. Direct URI mounting remains available as
`fujinet-mount --uri DRIVE URI [RW|RO]`; the legacy one-URI form targets drive
0 read-only.

Current Amiga limitations:

- eight native units (`unit 0..7`) mapped to DiskDevice slots 1..8;
- standard 880 KiB raw ADF geometry only;
- writable media requires DiskDevice `Flush (0x0E)` support;
- RS-232/`serial.device` correctness backend;
- driver-mediated mount/eject only (out-of-band slot changes are not seen); and
- a silent peer can still block the first serial receive byte until the
  timer-backed native deadline work is complete.

The workspace Amiberry harness is additional integration coverage, not a
build dependency of either repository.

## Formatting

The repository uses `.editorconfig` for line endings and whitespace, and
`.clang-format` for C/C++ formatting. To format source manually:

```sh
clang-format -i msdos/include/*.h msdos/src/driver/*.[ch] \
  msdos/src/nio/*.[ch] msdos/src/serial/*.[ch] msdos/src/util/*.[ch] \
  msdos/tests/*.cpp
```

There is also an optional pre-commit hook which formats staged C/C++ files and
re-stages them before Git creates the commit:

```sh
git config core.hooksPath scripts/git-hooks
```

The hook requires `clang-format` on `PATH`. It formats project files under
`msdos/include/`, `msdos/src/`, and `msdos/tests/*.cpp`, and intentionally
leaves vendored `msdos/tests/doctest.h` untouched.

## MS-DOS Setup

Copy `build/dos/fujinet.sys` to the DOS boot disk and add a line like this to
`CONFIG.SYS`:

```dos
DEVICE=FUJINET.SYS FUJI_PORT=1 FUJI_BPS=115200
```

Useful driver options:

- `FUJI_PORT=1` or `FUJI_PORT=0x3F8,4`
- `FUJI_BPS=115200`
- `FUJI_BATCH_SECTORS=16`
- `FUJI_IO_RETRIES=2`
- `FUJI_NIO_RETRIES=2`
- `FUJI_NET_TIMEOUT_MS=15000`
