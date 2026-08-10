# Amiga FujiNet disk device

This directory contains the Amiga `fujinet-disk.device` implementation. Stage
5 establishes one read-only Amiga unit and the RS-232 client binding; ADF
integration validation follows in Stage 6.

## Stage 5 contract

- Amiga unit 0 is the only accepted unit and maps to DiskDevice slot 1.
- Mount always sends `readonly=1`, `type=FN_DISK_TYPE_AUTO`, and a 512-byte
  sector-size hint. The configured URI is passed unchanged.
- Reads must have 512-byte-aligned offsets and lengths. Each block becomes one
  `fn_disk_read_sector()` request for slot 1.
- The RS-232 binding calls the typed `fujinet-nio-lib` API. That library's
  Amiga transport owns the shared stream session and FujiBus framing; the
  driver does not encode packets itself.
- The client is initialized once and requests are synchronous. This is the
  explicit one-client/one-outstanding-request invariant for the read-only
  skeleton. The Stage 7 architecture gate must revisit that invariant before
  writes, request queuing, additional units, or hot swap are added.
- Native linkage uses the `fujinet-nio-amiga-driver.a` library variant. It
  omits application `atexit()` registration because a resident Exec device
  owns its lifecycle and has no process-exit startup code.

The native device exposes `CMD_NONSTD` as the initial Mount command: `io_Data`
points to a NUL-terminated URI. `CMD_READ` accepts standard byte offsets and
lengths subject to the 512-byte alignment contract; `CMD_WRITE` remains
unsupported.

Run the portable contract tests with `make tests` from this directory. Run
`make native` to build `build/amiga/fujinet-disk.device`; this additionally
requires the Amiga GCC toolchain, readable NDK headers, and
`fujinet-nio-amiga-driver.a`.
