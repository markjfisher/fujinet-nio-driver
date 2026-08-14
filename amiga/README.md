# Amiga FujiNet disk device

This directory contains the Amiga `fujinet-disk.device` implementation. It
provides eight Amiga units over the RS-232 client binding and
has been integration-tested with a standard 880 KiB ADF.

## Current standard-ADF contract

- Amiga units 0–7 map to DiskDevice slots 1–8 and MountLists `DN0`–`DN7`.
- Read-only and writable Mount send `type=FN_DISK_TYPE_AUTO` and a 512-byte
  sector-size hint. The configured URI is passed unchanged; read-only is the
  default.
- Reads must have 512-byte-aligned offsets and lengths. Each block becomes one
  context request for the unit's slot.
- The RS-232 binding calls the typed `fujinet-nio-lib` API. That library's
  Amiga transport owns the shared stream session and FujiBus framing; the
  driver does not encode packets itself.
- The unit owns an explicit DiskDevice client context containing packet
  request/response and codec scratch storage. The context API does not use the
  legacy process-global raw request, response, or parser state.
- `BeginIO()` uses a device-owned FIFO. One caller drains runnable requests in
  order, stopped units remain queued, and the single physical RS-232 session
  is never used concurrently. No permanent worker task is required.
- Native linkage uses the `fujinet-nio-amiga-driver.a` library variant. It
  omits application `atexit()` registration because a resident Exec device
  owns its lifecycle and has no process-exit startup code.
- Each serial exchange uses a reply port owned by the current calling task.
  The CLI that configures the resident device exits before AmigaDOS performs
  later reads, so retaining that CLI's message port would leave the driver
  waiting on a dead task.
- Large packet and codec buffers therefore live in the resident device base,
  not caller-owned filesystem stacks or mutable library statics.

Normal Amiga users select catalogue media with the `nio-core-apps` tools:

```text
FMOUNT CATALOG-SLOT DN0: [RO|RW]
FUMOUNT DN0:
```

`FMOUNT` resolves the persistent Slot Catalog entry, mounts it into the
selected drive, and updates the shared `config-nio/mappings` record.
`FUMOUNT` performs the driver-mediated eject and removes the mapping.

The separately built `fujinet-mount` program is diagnostic-only. It remains
available for private driver tests such as status and geometry inspection,
boundary and malformed-request checks, direct URI injection, and explicit
update/eject diagnostics. It is not a normal installation or end-user mount
application.

The native device exposes `FUJINET_DISK_CMD_MOUNT` as its private read-only
Mount command and `FUJINET_DISK_CMD_MOUNT_WRITABLE` as the writable variant;
`io_Data` points to a NUL-terminated URI. Both are outside the trackdisk
command range. Reads and writes accept standard byte offsets and lengths
subject to the 512-byte alignment contract. See
[`WRITE_MEDIA_POLICY.md`](WRITE_MEDIA_POLICY.md) for update, queue-flush, ETD,
and media-change semantics.

## Standard ADF profile

After Mount, the driver issues Info and accepts the selected unit's slot, raw
mounted media, 512-byte sectors, and 1760 sectors. It reports the
corresponding 80-cylinder, two-head, 11-sector geometry to AmigaDOS. Malformed
Info responses, short sector responses, transport errors, unmounted reads,
and out-of-range reads are rejected by named host contract tests.

The workspace Amiberry `diskdevice-adf` test creates deterministic standard
ADFs, accesses simultaneous `DN0:`/`DN1:` media, and validates native block
reads through AmigaDOS `Dir` and `Type`. Its writable path creates a file on
`DN2:`, performs `CMD_UPDATE`, cleanly dismounts the old DOS handler, replaces
the image, mounts a fresh DN2 handler, and verifies persisted content. An
unused unit also exercises explicit eject without opening a DOS requester.

Current limitations are deliberate Phase 1 boundaries: standard 880 KiB ADF
only, unit-mediated hot swap only, RS-232 only, static DD MountLists, and an
explicit DOS-handler dismount/remount around replacement. The
workspace Phase 2 media architecture owns inferred DD/HD/nonstandard geometry,
RDB/HDF media, dynamic DOS nodes, seamless handler coordination, and
consolidation onto standard tools.

Run the portable contract tests with `make tests` from this directory. Run
`make native` to build `build/amiga/fujinet-disk.device`; this additionally
requires the Amiga GCC toolchain, readable NDK headers, and
`fujinet-nio-amiga-driver.a`.

The host `test_fujinet_exec_boundary` test is the first resident-device
boundary contract harness. It covers queue, request-removal, and retained
change-registration rules without requiring Exec or an Amiga emulator. It is
not a substitute for the next native harness, which must validate real
message ports, `AbortIO()`, `Cause()`, and task/request lifetimes inside
Amiberry.
