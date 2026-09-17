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

## Loading the resident device

`make native` also builds `build/amiga/fujinet-load-resident`. Install that
program in `C:` and the device in `DEVS:`, then register the device during
startup with:

```text
C:fujinet-load-resident DEVS:fujinet-disk.device fujinet-disk.device
```

Idle Expunge of `fujinet-disk.device` (when `OpenCnt` is 0 and no I/O is
queued or in progress) returns the `InitResident` segment list so Exec can
unload the binary. Busy Expunge sets `LIBF_DELEXP` and returns 0 until a later
non-worker Expunge or last `CloseDevice` completes teardown.

## Unloading the resident device

`make native` also builds `build/amiga/fujinet-unload-resident`. Install that
program in `C:` and request unload of a resident device with:

```text
C:fujinet-unload-resident fujinet-disk.device
```

The command calls `RemDevice()` on the named device if found, then checks
whether it actually left the device list. Output:

- `Unloaded: <name>` — device is no longer resident (RETURN_OK)
- `Still resident: <name>` — unload was deferred due to open count or
  in-progress I/O; retry after idle (RETURN_FAIL)
- `Not resident: <name>` — device was not on the device list (RETURN_FAIL)

`Still resident` means the device set `LIBF_DELEXP` and will complete unload
when its last client calls `CloseDevice()` or when the I/O worker drains and
performs a deferred Expunge. A second unload attempt after the device becomes
idle should succeed.

The loader uses the OS 1.3-compatible `LoadSeg()` and `InitResident()` APIs.
The loader itself has been validated on Workbench 3.1: it registers the
device, permits `OpenDevice()`, and serves basic trackdisk status commands
without `LoadModule` or a warm start. Complete `DNx:` filesystem access on
Workbench 3.1 is not yet validated. Its filesystem handler uses the classic
synchronous `TD_REMOVE` interface for its single change interrupt; newer
handlers use retained `TD_ADDCHANGEINT` requests instead.

The utility validates the module's bounded first-hunk resident structure and
name, accepts only `NT_DEVICE`, refuses to initialize a duplicate device, and
retains the loaded segment after successful registration because Exec
continues to reference it. Name mismatch, non-resident input, unsupported
resident type, and initialization failure release the segment.

The workspace disk builder's `--with-driver` path installs both files and adds
this command to `S:Startup-Sequence` automatically. The focused Amiberry
`diskdevice-loader` case verifies first load, harmless duplicate invocation,
name mismatch, non-resident rejection, registration, `OpenDevice()`, and
standard trackdisk status commands on both the normal test OS and Workbench
3.1.

## RS-232 baud rate and serial driver

`make native` also builds `build/amiga/fujinet-nio-baud` and
`build/amiga/fujinet-nio-serial`. With the resident `fujinet-nio.device`
loaded, use them to inspect or select the rate and Exec serial driver used
when the RS-232 backend next opens:

```text
fujinet-nio-baud
fujinet-nio-baud 38400
fujinet-nio-serial
fujinet-nio-serial fujinet-serial.device
fujinet-nio-serial serial.device
```

Supported baud values are 300–230400. The serial driver name is an Exec
device (`serial.device` by default). `fujinet-serial.device` is the FujiNet
Paula UART driver: 8N1 only, exclusive open, a receive-buffer-full interrupt
that copies `SERDATR` into a ring, and polled `TBE` on transmit. Load it
before selecting it:

```text
C:fujinet-load-resident DEVS:fujinet-serial.device fujinet-serial.device
C:fujinet-nio-serial fujinet-serial.device
```

Do not rename stock `serial.device`; select `fujinet-serial.device` instead.
The selection is held by the resident broker until it is unloaded or the
machine reboots; put the desired commands in `S:Startup-Sequence` to make
them persistent. This affects only the RS-232 byte-stream backend.
Packet-native transports such as Zorro or floppy use their own transport
configuration and are unaffected.

`fujinet-nio-exchange` accepts the same choice for a single matrix run
without changing the resident default:

```text
fujinet-nio-exchange --type clock --backend cold --baud 38400 \
    --serial-device fujinet-serial.device --trials 20
```

Omitting `--serial-device` uses whatever `fujinet-nio-serial` last set.

`--installed-backend serial|native` declares the already installed backend
(default `serial`); it does not discover or switch hardware. `--backend
cold|warm` remains the separate lifecycle choice. Native installations support
warm clock and file-list operations using EXCHANGE only, plus ordinary resident
disk diagnostics:

```text
fujinet-nio-exchange --installed-backend native --backend warm --type clock --trials 2
fujinet-nio-exchange --installed-backend native --backend warm --type file-list --uri host:/ --size 128 --trials 2
```

Native cold is unsupported because there is no generic reset command. Native
commands reject baud, serial-device/unit, host-get and provocation before opening
the device. Clock/file-list reject explicit slot/LBA options (including zero). File-list requires a
nonempty URI and supported size. The context/lifecycle line precedes unchanged
trial fields; `native=` in a trial remains its historical error-detail field.


Ordinary disk diagnostics require explicitly supplied disposable standard DD/HD
ADF fixtures, warm lifecycle, slot 1–8 and LBA. Writing also requires
`--write-intent`; a read rejects that flag. The slot maps to resident unit
`slot - 1`. These commands work with the installed native or serial broker
without baud, pacing or serial configuration:

```text
fujinet-nio-exchange --installed-backend native --backend warm --type disk-read --slot 1 --lba 17 --fixture-uri host:/disposable-read.adf --disposable-fixture
fujinet-nio-exchange --installed-backend native --backend warm --type disk-write --slot 8 --lba 17 --fixture-uri host:/disposable-write.adf --disposable-fixture --write-intent --trials 3
```

Run only in an isolated diagnostic session. Local resident state and remote INFO
must both report an unused slot before mount; these checks do not atomically
exclude another client racing the mount. Existing mounted media is refused.
Geometry bounds are checked before transfers. Each write uses a deterministic
trial-distinct 512-byte pattern, flushes, then compares every read-back byte.
The zero-based trial pattern byte `i` is
`((i ^ 0x5a) ^ (trial >> ((i % 4) * 8))) & 255`. A failed operation or mismatch
stops the command with nonzero status; the tool does not replay it. Exec errors
and available broker/transport/service trace details are reported separately.
Mount, geometry and flush have no exchange-detail trace; the tool reports their
actual resident completion only.

Reads report the one-based trial and a 32-bit FNV-1a checksum over all 512
returned bytes so an independent fixture can verify what the tool received.

The fixture remains mounted after success **and after any post-mount failure**.
The tool prints `FIXTURE LEFT MOUNTED`. If mount itself fails, remote completion
may be uncertain: it reports `FIXTURE STATE UNKNOWN` and that the fixture may
remain mounted, without retrying or unmounting. It does not restore overwritten data, does
not raw-unmount behind the resident, and does not change saved mappings.
Automatic eject is deliberately absent because `TD_EJECT` changes saved mappings.
When finished, end the disposable session or deliberately use the standard
mount/eject workflow.

Legacy serial provocation remains separate and explicitly selected with
`--provocation --backend cold --baud ... --slot ... --lba ...`. It cannot be
combined with ordinary fixture/declaration/write-intent flags.


To separate cold vs warm serial and response size on real hardware, use
`fujinet-nio-exchange` as described in
[`docs/amiga/rs232-cold-warm-hardware-test.md`](../docs/amiga/rs232-cold-warm-hardware-test.md).

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
`make native` to build `build/amiga/fujinet-disk.device`,
`build/amiga/fujinet-load-resident`, and `build/amiga/fujinet-unload-resident`;
this additionally requires the Amiga GCC toolchain, readable NDK headers, and
`fujinet-nio-amiga-driver.a`.

The host `test_fujinet_exec_boundary` test is the first resident-device
boundary contract harness. It covers queue, request-removal, and retained
change-registration rules without requiring Exec or an Amiga emulator. It is
not a substitute for the next native harness, which must validate real
message ports, `AbortIO()`, `Cause()`, and task/request lifetimes inside
Amiberry.

## Native-test directory recovery

The native-test artifact requires the matching host test runner. It retains the
public broker ABI and raw FujiBus packets; the following files are test harness
controls, not a hardware bridge ABI or packet correlation fields.

The host exclusively locks `PEER.lock`, creates a fresh 128-bit `CHALLENGE`, and
publishes `IDENTITY`. The guest sends `BARRIER.<challenge>` containing that challenge. Between
synchronous core ticks, the host drains old delivery, clears packet records,
rotates the challenge, then publishes `ACK` with the requested value. The guest
accepts only that invocation's exact acknowledgment and a rotated challenge.
Missing/stale proof fails closed. Host restart changes the challenge and removes
stale controls without clearing uncertainty.

Before sending each request the guest publishes `AMBIGUOUS`. It clears this
marker only after validated completion or proven pre-send rejection. Unknown
completion therefore survives close/open, device reload and host process restart.
This is process-lifecycle protection, not a host power-loss persistence claim. Ordinary retry
callers cannot authorize recovery. If cleanup fails after a validated response,
the current call keeps its known success and future calls stay quarantined;
cleanup must not turn a completed write into a retry.

For an isolated test session, wait for all affected calls and their retries to
finish. Inspect independent backing state: a timed-out write may have happened.
Then authorize one recovery from the guest Shell:

```text
Copy NATIVE:CHALLENGE NATIVE:RECOVER
```

The next new operation consumes this permission and requests a peer barrier.
It does not replay an old operation. A failed/interrupted attempt requires fresh
permission; never delete `AMBIGUOUS` manually to resume. Do not issue permission
while old callers can still retry or while another client owns the directory.
The isolated probe enforces this by joining every queued caller and waiting for
the resident retry loop to finish before creating permission. The adapter cannot
police an operator who violates this test-session precondition. Healthy traffic
retires unused permission so it cannot authorize a later failure.

The runner's optional `FAULT` controls (`hold` or `drop`, newline terminated)
affect the next actual service response. `RELEASE` containing `release` plus
newline publishes a held response; a completed barrier discards it instead.
Startup and barriers retire fault/release controls, and an unused release is
consumed immediately so it cannot release a future fault.
These controls do not synthesize service replies. Software/guest evidence does
not validate a physical Zorro bridge or guarantee exactly-once application
semantics after a known-complete operation.

## Undeployed whole-packet containment component

`nio.device/fujinet_nio_packet_backend.[ch]` is a portable raw FujiBus backend
guard for a future packet adapter. It is not linked into the deployed serial
broker. It changes no public broker ABI, caller retry policy, or wire bytes.
The native harness binds it behind the broker's existing backend callbacks.

Initialize one guard per remote endpoint lifetime, with exclusive borrowed
scratch storage and opaque packet-I/O callbacks. Scratch capacity is explicitly
bounded (6–65535 bytes); this harness uses 1024 bytes, matching the broker and
library packet limit, not a physical mailbox size. The same bound limits sent
requests. Requests and caller output buffers must be disjoint from scratch.
The adapter may borrow buffers only during a callback and must never write
past capacity, even when reporting a larger received packet. Failed exchanges
report zero length and never expose scratch bytes to caller output buffers.

All operations belong to the same serialized worker ownership domain. The
`active` flag rejects callback reentry; it is not a thread synchronization
primitive. Adapter callbacks must finish in bounded time. The guard allocates
no memory and performs no retries. An adapter reports one of three outcomes:

- `FN_PACKET_REJECTED`: definite pre-send rejection, with no remote delivery.
  Existing bounded caller retries remain safe.
- `FN_PACKET_COMPLETE`: this exchange finished, with no further execution or
  response delivery possible. The guard additionally validates whole-packet
  size, encoded length, checksum, descriptor bounds, and device/command before
  copying the unchanged response.
- `FN_PACKET_UNKNOWN`: transmission may have occurred. Partial/local acceptance
  is not completion. Unknown outcomes and invalid/oversized/mismatched responses
  quarantine the endpoint before a retry can transmit.

Quarantine survives backend close/open and transport close/open. Every local
reset attempt enters quarantine, whether it succeeds or fails, including a reset
from a healthy state. Initialization also starts quarantined: first use requires
proof, so constructing a guard cannot silently assume a clean remote endpoint.
Never reinitialize or discard its state to recover a live endpoint. Only
`fn_packet_backend_recover()` clears quarantine after the adapter's independent
quiescence callback proves that prior work cannot execute and old responses
cannot arrive. Missing/failed proof keeps it quarantined. Local clears, reopen,
timeouts, and elapsed time do not supply this guarantee. A future adapter must
preserve uncertainty across unload/reload or establish this proof again.

This contains **unknown transport completion**, not every application replay.
The unchanged `fn_raw_call` still replays once after a fully valid matching
response when its payload exceeds the application's `reply_capacity`. The
transport receives into its own 1024-byte buffer and cannot observe that smaller
application limit. Both valid exchanges can have effects; the guard correctly
remains unquarantined. Likewise, an active abort is not rollback, and the raw
caller may retry an aborted but completed exchange. Valid U8 remote statuses,
including timeout/error and unknown values, pass through without status-driven
backend replay. Disk service status mapping also stays in the unchanged caller.
No promise of exactly-once application effects or hardware readiness is made.

Run the focused integration test from the workspace root:

```sh
source scripts/env.sh && make -C repos/fujinet-nio-driver/amiga/tests build/test_fujinet_nio_packet_backend && repos/fujinet-nio-driver/amiga/tests/build/test_fujinet_nio_packet_backend
```

It links actual disk read/write callers and `fn_raw_call`, actual Amiga transport,
and actual broker against an independent bounded peer. The peer accepts sends
even while remote work remains pending; backend quarantine must prevent those
calls. Tests separately count caller attempts, backend entries, transfer calls,
transmissions, effects, remote replies, pending/max pending work and local
ReplyMsg completions. Cases cover pre-send failures, delivered/unknown work,
lost/corrupt/truncated/oversized/mismatched responses, lifecycle, late replies,
failed/absent proof, explicit recovery, queued/active abort, FIFO ownership,
request immutability, buffer sentinels, status preservation, and the known
completed-response replay limit. Reset/quiescence are software callback
contracts; physical implementation and validation remain future adapter work.
