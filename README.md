# FujiNet NIO Driver

This repository currently contains the MS-DOS `FUJINET.SYS` driver for the
FujiNet NIO protocol. It contains the native MS-DOS NIO driver and its
protocol tests, without carrying unrelated firmware transports, bundled DOS
apps, or transport-selection ifdefs. It is now the home for additional native
FujiNet drivers, beginning with the MS-DOS implementation. The MS-DOS source
and tests now live under `msdos/`; the root build entry points and generated
artifact path remains the workspace standard.

## Repository layout

```text
msdos/
  include/            DOS-facing public headers
  src/                MS-DOS driver and NIO protocol implementation
  tests/              host-side protocol tests
common/               reserved for future shared driver-side interfaces
amiga/                reserved for the later Amiga driver implementation
```

The generated driver remains `build/dos/fujinet.sys`. Future native drivers
will be added only after the MS-DOS relocation and its validation checks are
reviewed.

## Build

The DOS driver build uses Open Watcom, so ensure it is on the path, e.g.

```sh
export WATCOM=/opt/watcom
export EDPATH=$WATCOM/eddat
export INCLUDE=$WATCOM/h
export PATH=$PATH:$WATCOM/binl64:$WATCOM/binl

make
```

The generated driver is `build/dos/fujinet.sys`.

## Tests

Host-side protocol tests use doctest:

```sh
make -C tests test
```

These tests cover the portable NIO packet, timeout, and disk protocol helpers.

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
