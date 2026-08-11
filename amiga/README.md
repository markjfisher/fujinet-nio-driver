# Amiga FujiNet disk device

This directory contains the Amiga `fujinet-disk.device` implementation. It
provides eight Amiga units over the RS-232 client binding and
has been integration-tested with a standard 880 KiB ADF.

## Stage 5 contract

- Amiga units 0–7 map to DiskDevice slots 1–8 and MountLists `DN0`–`DN7`.
- Mount always sends `readonly=1`, `type=FN_DISK_TYPE_AUTO`, and a 512-byte
  sector-size hint. The configured URI is passed unchanged.
- Reads must have 512-byte-aligned offsets and lengths. Each block becomes one
  `fn_disk_read_sector()` request for slot 1.
- The RS-232 binding calls the typed `fujinet-nio-lib` API. That library's
  Amiga transport owns the shared stream session and FujiBus framing; the
  driver does not encode packets itself.
- The unit owns an explicit DiskDevice client context containing packet
  request/response and codec scratch storage. The context API does not use the
  legacy process-global raw request, response, or parser state.
- `BeginIO()` runs in its caller's task context. A unit-owned Exec
  `SignalSemaphore` serializes all commands across callers and protects the
  single physical RS-232 session; requests do not overlap or queue in an
  internal device task in this read-only implementation.
- Native linkage uses the `fujinet-nio-amiga-driver.a` library variant. It
  omits application `atexit()` registration because a resident Exec device
  owns its lifecycle and has no process-exit startup code.
- Each serial exchange uses a reply port owned by the current calling task.
  The CLI that configures the resident device exits before AmigaDOS performs
  later reads, so retaining that CLI's message port would leave the driver
  waiting on a dead task.
- Large packet and codec buffers therefore live in the resident device base,
  not caller-owned filesystem stacks or mutable library statics.

The normal CLI form is `fujinet-mount CATALOG-SLOT DRIVE [RW|RO]`. It resolves
the persistent Slot Catalog entry, mounts it into active drive `DRIVE`, and
updates the shared `config-nio/mappings` record. Direct URI mounting is
retained as `fujinet-mount --uri DRIVE URI [RW|RO]`.

The native device exposes `FUJINET_DISK_CMD_MOUNT` as its private read-only
Mount command and `FUJINET_DISK_CMD_MOUNT_WRITABLE` as the writable variant;
`io_Data` points to a NUL-terminated URI. Both are outside the trackdisk
command range. Reads and writes accept standard byte offsets and lengths
subject to the 512-byte alignment contract. See
[`WRITE_MEDIA_POLICY.md`](WRITE_MEDIA_POLICY.md) for update, queue-flush, ETD,
and media-change semantics.

## Standard ADF profile

After Mount, the driver issues Info and accepts only slot 1, raw media,
read-only and mounted flags, 512-byte sectors, and 1760 sectors. It reports the
corresponding 80-cylinder, two-head, 11-sector geometry to AmigaDOS. Malformed
Info responses, short sector responses, transport errors, unmounted reads,
and out-of-range reads are rejected by named host contract tests.

The workspace Amiberry `diskdevice-adf` test creates a deterministic standard
ADF, mounts it through `fujinet-disk.device`, and validates native block reads
through AmigaDOS `Dir` and `Type` commands. It also launches two independent
CLI processes that issue simultaneous reads, validating the unit's native
cross-task serialization rule.

Run the portable contract tests with `make tests` from this directory. Run
`make native` to build `build/amiga/fujinet-disk.device`; this additionally
requires the Amiga GCC toolchain, readable NDK headers, and
`fujinet-nio-amiga-driver.a`.
