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

The transitional CLI form is `fujinet-mount CATALOG-SLOT DRIVE [RW|RO]`. It resolves
the persistent Slot Catalog entry, mounts it into active drive `DRIVE`, and
updates the shared `config-nio/mappings` record. Direct URI mounting is
retained as `fujinet-mount --uri DRIVE URI [RW|RO]`.
Phase 2 moves the normal user workflow to `nio-core-apps` `FMOUNT`/`FUMOUNT`;
this program must not remain a competing application.

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
only, unit-mediated hot swap only, RS-232 only, static DD MountLists, and a
DOS-handler dismount/remount around replacement. The
workspace Phase 2 media architecture owns inferred DD/HD/nonstandard geometry,
RDB/HDF media, dynamic DOS nodes, seamless handler coordination, and
consolidation onto standard tools.

Run the portable contract tests with `make tests` from this directory. Run
`make native` to build `build/amiga/fujinet-disk.device`; this additionally
requires the Amiga GCC toolchain, readable NDK headers, and
`fujinet-nio-amiga-driver.a`.
