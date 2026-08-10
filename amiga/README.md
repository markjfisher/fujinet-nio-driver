# Amiga FujiNet disk device

This directory contains the Amiga `fujinet-disk.device` implementation. It
currently provides one read-only Amiga unit over the RS-232 client binding and
has been integration-tested with a standard 880 KiB ADF.

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
- Each serial exchange uses a reply port owned by the current calling task.
  The CLI that configures the resident device exits before AmigaDOS performs
  later reads, so retaining that CLI's message port would leave the driver
  waiting on a dead task.
- The resident library variant defines `FN_DISK_STATIC_BUFFERS`. Disk codec
  calls are synchronous, so this safely keeps their 1 KiB scratch buffer out
  of caller-owned filesystem task stacks. Ordinary Amiga application builds
  retain stack-local codec storage. This distinction alone does not make the
  complete library path reentrant because raw transport/parser state remains
  shared; Stage 7 tracks replacement with explicit driver-owned contexts.

The native device exposes `FUJINET_DISK_CMD_MOUNT` as its private Mount
command: `io_Data` points to a NUL-terminated URI. The command is deliberately
outside the trackdisk command range. `CMD_READ` accepts standard byte offsets
and lengths subject to the 512-byte alignment contract; `CMD_WRITE` remains
unsupported.

## Standard ADF profile

After Mount, the driver issues Info and accepts only slot 1, raw media,
read-only and mounted flags, 512-byte sectors, and 1760 sectors. It reports the
corresponding 80-cylinder, two-head, 11-sector geometry to AmigaDOS. Malformed
Info responses, short sector responses, transport errors, unmounted reads,
and out-of-range reads are rejected by named host contract tests.

The workspace Amiberry `diskdevice-adf` test creates a deterministic standard
ADF, mounts it through `fujinet-disk.device`, and validates native block reads
through AmigaDOS `Dir` and `Type` commands.

Run the portable contract tests with `make tests` from this directory. Run
`make native` to build `build/amiga/fujinet-disk.device`; this additionally
requires the Amiga GCC toolchain, readable NDK headers, and
`fujinet-nio-amiga-driver.a`.
