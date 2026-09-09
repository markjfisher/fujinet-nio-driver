# Hardware test: RS-232 cold/warm and response size

Use this on a real Amiga plus FujiNet ESP. It tells you whether failures
follow **first open after a baud change** (cold), **an already-open serial
backend** (warm), or **how large the FujiBus reply is**.

The Amiberry `nio-broker-isolated` case only proves the broker on a small
clock exchange. This procedure is the field version of that idea: you choose
the first measured request and you log every trial.

Do **not** treat CIA TX→RX as the mechanism. Paula receive and transmit are
independent. An overrun here means the prior RX character was not serviced
before the next one completed. See [`Serial-IO-Interface.md`](Serial-IO-Interface.md)
if you want the hardware background.

This pass covers 9600, 19200, and 38400 as the product gate. `fujinet-nio-exchange
--baud` now accepts the same 300..230400 range as `fujinet-nio-baud` and
`fujinet-serial.device` so 57600+ can be tried on PiStorm. The ESP must already
be at that rate. Disk-provocation uses the same `--baud` range; 38400 remains
the product gate, 57600 is opt-in soak with ESP pacing `0/0/0`.

Pacing matrix numbers and the 16/2000 product choice:
[`rs232-38400-pacing-evidence.md`](rs232-38400-pacing-evidence.md).

## What you need

- FujiNet ESP already listening at the baud you will test (Amiga `SET_BAUD`
  only changes the Amiga UART).
- Resident `fujinet-nio.device` loaded. Build with `make native` from
  `amiga/` and install:

  ```text
  Copy fujinet-nio.device DEVS:
  Copy fujinet-serial.device DEVS:
  Copy fujinet-nio-exchange C:
  Copy fujinet-nio-serial C:
  C:fujinet-load-resident DEVS:fujinet-serial.device fujinet-serial.device
  C:fujinet-load-resident DEVS:fujinet-nio.device fujinet-nio.device
```

The broker opens `serial.device` unit 0 unless you select another
IOExtSer-compatible driver on the Amiga (no rebuild). For hardware above
9600 baud use the FujiNet Paula driver:

```text
C:fujinet-nio-serial fujinet-serial.device
```

Or pass `--serial-device fujinet-serial.device` on a matrix command. Cold still
closes and reopens that driver before the measured request. Omit the flag to
keep whatever `fujinet-nio-serial` last set.

- **No other FujiNet serial client** during the matrix. Unload
  `fujinet-disk.device` if it is resident, and do not run `FLS`, `FHOST`, or
  `FIN` in another shell. Those would open the broker and change cold/warm
  without you seeing it.

Confirm isolation with **one** matrix command, not the no-arg prove tool:

```text
C:fujinet-nio-exchange --type clock --backend cold --baud 38400 --trials 1
```

(Use the baud the ESP is already on.) You want one `req_len=… backend=cold`
line and `exit 0`. `--help` must print usage and return. CLI stack and
OpenDevice-only `WaitIO` pitfalls:
[`docs/amiga/cli-stack-and-iorequest.md`](../../../../docs/amiga/cli-stack-and-iorequest.md).

Do **not** run `C:fujinet-nio-exchange` with no arguments on PiStorm or real
hardware. That is the Amiberry isolation suite: it sends a malformed packet
to force a timeout, then `CreateNewProc` two extra processes for concurrent
clock commands. On PiStorm that sequence has completed with `PASS` and then
rebooted the machine (power LED flash, no Guru). Isolation is already
`disk.device` unloaded and no `FLS`/`FHOST`/`FIN` in another shell.

## FujiNet Paula device lifecycle (this cut)

`fujinet-serial.device` claims `misc.resource` as `MR_SERIALPORT` then
`MR_SERIALBITS` before touching Paula or `INTB_RBF`. `CMD_READ` waits until
the requested byte count is available. The RBF handler only samples
`SERDATR`, retains into the private ring, and acknowledges once per byte; a
device-owned software interrupt completes a pending READ. `CMD_FLUSH` aborts
a retained READ (`IOERR_ABORTED`) and clears the software queue **without**
masking RBF. `CMD_WRITE` drains leftover `SERDATR` and discards the software
queue immediately before TX so idle bytes are not parsed as the next frame.
`OpenDevice` programs `SERPER` from the request `io_Baud` (the broker fills
this before `OpenDevice`). `SETPARAMS` waits for TX idle, applies `SERPER`,
discards RX garbage, and leaves RBF armed.

PiStorm 38400: FLUSH-quiesce lost the first request after idle (`cause=4`,
~13.7 s, FLS single-shot fail) with and without ESP 16/2000 pacing. Later
trials in the same command succeeded. Always-armed is the production path.
Always-armed did not fix a clean-reboot first 38400 open: claim wrote 19200
then `SETPARAMS` jumped to 38400. 19200 first-open never took that jump.

**CAP-5:** PiStorm is the sole hardware-stability gate. A 19200 cold clock
through `fujinet-serial.device` must print `result=0` / `status=0` and return
to the Shell with no power-LED flash and no PiStorm reboot screen. Amiberry
does not prove that. 38400 FLS / first file-list after idle must not sit in
the 5 s QUERY timeout. Confirm the guest `fujinet-serial.device` is 0.7
(`$VER`) and `fujinet-nio.device` is 0.7. Serial 0.7 drains stacked RBF
during TX (first-after-idle host-get was 1–7 bytes short). Serial 0.5 polled `INTREQ` TBE
while that interrupt was masked; WRITE failed `SerErr_LineErr` (`cause=5
native=6`), froze the mouse, and the ESP saw no request. Serial 0.6 waits
for `SERDATR` TBE again and keeps any RBF byte from that same read. First
38400 file-list stayed `6/4/255/255` through serial 0.6 and broker 0.3.
`native=255` is a **log-field cap**, not the ring size: Paula RX is 2048
bytes, so a ~513-byte FujiBus + SLIP (opening `C0`, payload, closing `C0`)
fits. Broker 0.4 completes a frame whose opening END was lost if the
trailing END was among the bytes read. Broker 0.5 adds a `peek=` hex line
on `cause` 4/7/9: the first 32 bytes the session actually received (or the
unread ring head if QUERY never delivered). Compare that to the ESP
`payload` dump — a leading `c0` vs `01 01 00 00…`. Cause=4
`native`/`status` remain session-bytes / ingest.

## ESP response pacing (rank 2)

Product default on ESP `UartGpio` is **16-byte chunks, 2000 µs between
chunks, no inter-byte gap** (`tx_chunk_size=16`, `tx_chunk_gap_us=2000`,
`tx_byte_gap_us=0`). Requests stay 38400; ESP→Amiga long replies average
about **26 kbaud** (16 × ~262 µs + 2 ms idle). Atari SIO clears pacing.

If `fujinet.yaml` already has explicit `tx_chunk_size: 0` from an earlier
save, set and persist:

```text
uart.set tx_byte_gap_us 0
uart.set tx_chunk_size 16
uart.set tx_chunk_gap_us 2000
uart.save
uart.status
```

`tx_gap_us` only delays the start of a UART write. Byte pacing waits until
each byte has shifted out, then idles. Chunk pacing (only if
`tx_byte_gap_us` is 0) bursts `tx_chunk_size` at full baud and idles
`tx_chunk_gap_us` between chunks.

After a Paula overrun (`status=1`, `cause=7`), the broker drains RX until
30 ms of idle (longer than the 2 ms chunk gap) then closes `serial.device`.
That stops leftover ESP chunks from turning the next trial into `cause=3`
(SESSION_IO). Do not treat CIA TX→RX as the mechanism. Product gate is 38400.
57600+ is opt-in via `--baud` / `fujinet-nio-baud` with the ESP already there;
no seven-wire, READY/GO, or a custom `serial.device`.

## Cold vs warm (what the flags actually do)

| `--backend` | What happens before the measured request |
| --- | --- |
| `cold` | `SET_BAUD` (even if the rate is already that value). That closes the serial backend. The measured EXCHANGE is the first FujiBus after reopen. |
| `warm` | Checks `--baud` against the broker (omit `--baud` to skip the check), then always sends one **unmeasured** clock GET (`WARMUP`), then the measured type. A baud mismatch prints `WARM baud mismatch` and stops. A failed WARMUP prints a `WARMUP` diagnostic line and **skips that trial's measured request**, then continues the remaining `--trials`. No `SET_BAUD`. |

You cannot “detect warm” from OpenCnt. If you just booted and have never
exchanged, `warm` still does the clock WARMUP first, so the measured request
is never the first FujiBus of that session.

Typical pairing: run a **cold** cell, then the same cell as **warm** without
unloading the device.

## One log line per trial

Example:

```text
req_len=6 resp_len=14 elapsed_us=4120 ttfb_us=- result=0 cause=0 native=0 status=0 backend=cold
```

| Field | Meaning |
| --- | --- |
| `req_len` | FujiBus request length (not SLIP). |
| `resp_len` | Actual FujiBus response length. **This is the size to record**, not `--size`. |
| `elapsed_us` | Time around the measured `DoIO(EXCHANGE)` only, or `-` if `timer.device` could not be used. |
| `ttfb_us` | Always `-` in this build (no first-bit stamp). |
| `result` | Broker result pad. `0` is a clean completion. |
| `cause` | Where a transport fault was classified (see below). `0` is none. |
| `native` | `serial.device` `io_Error`. `0` is none. **On `cause=4` with `fujinet-nio.device` 0.3 this is bytes `serial_read_byte` returned to the session (capped 255), not an io_Error. 0.4–0.6 packed RBF-fire here instead.** |
| `status` | High byte of `serial.device` `io_Status`. `1` is `IO_STATF_OVERRUN`. **On `cause=4` this is bytes ingested into the ring, or WRITE-discarded count if ingest was 0 (capped 255).** |
| `backend` | `cold` or `warm` as requested, not inferred. |

`--size` on `file-list` is LIST `maxPayloadBytes` (how much directory blob
you asked for). The directory must be large enough to fill that cap or
`resp_len` will be a small listing, not a large burst.

### Pass vs fail on one line

**Pass:** `result=0 cause=0 native=0 status=0`, no `fujibus=bad` line, and a
plausible `resp_len` (clock and host-get are small; file-list should grow
with `--size` until the directory runs out). A broker `result=0` is no
longer enough: MEASURE now also requires the decoded bytes to be a FujiBus
packet that echoes the request device/command, whose length field matches
`resp_len`, and whose checksum is valid. That is the same bar `FLS` /
`FHOST` apply in `fn_raw_call`. If the soak stays green and `FLS` still
fails, the gap is the **request shape** (LIST `SORT_BY_NAME` / paging) or
the CLI, not a silent corrupt frame.

The process return code is `0` only if every measured trial on that command
passed. Failures are still printed; there is no retry.

**Likely Paula receive overrun:** `status=1` and `cause` `7` or `9`.

| `cause` | Meaning |
| ---: | --- |
| 0 | No request-local serial detail. |
| 7 | `CMD_READ` failed; flush never drained an overrun flag. |
| 9 | Flush saw and drained `IO_STATF_OVERRUN`, then the real `CMD_READ` still failed. |

`native` is often `6` (`SerErr_LineErr`) on those rows. Other `cause` values
(write, query, timeout, timer) are different faults; note them, but they are
not the burst-overrun signature.

On **`cause=4`** with `fujinet-nio.device` 0.3+, read `native` / `status` as
session-delivered bytes vs ring ingest from after `CMD_WRITE` until QUERY
gave up (not serial.device errors). `fujinet-serial.device` 0.4–0.6 packed
RBF-fire into `native`; a first-timeout `255/255` could not split the two
cases below. Broker 0.4 still reports these counters on timeout; a first
38400 pass is `result=0`. On `cause` 3, 4, 7, or 9, `fujinet-nio-exchange`
prints `slip class=… raw=… decoded=… pkt=… c0=… first=… leftover=…` then
`peek=` (first 32 captured SLIP bytes), and on file-list `mismatch=` against
the last successful trial (or `c0 fe 02` if none yet). `ring=` is leftover
RX still in the serial queue **before** drain. `class=prefix` is a discarded
opening END; `extra-c0` closed on the wrong delimiter; `len-mismatch` is a
short body with a plausible header. That dump does not grow
`FujiNetNIORequest`; FLS still sees `fn_response_length=0` on error. Broker
`$VER` 0.7.

| `native` (bytes to session) | `status` (ingested, or WRITE discards if ingested=0) | Meaning |
| ---: | ---: | --- |
| 0 | 0 | ISR never stored a byte. Paula never interrupted, or a task-level `SERDATR` poll stole it before INTREQ latched. |
| 0 | N≠0 | ISR (or WRITE discard) counted N bytes, but QUERY never handed the session even one. Ring occupancy and QUERY are the next place to look. |
| N≠0 | 0 | Session got N bytes; ISR ingest was 0 (unexpected if those bytes came from this device). |
| N≠0 | M≠0 | Session consumed bytes (N) and the ring was filled (M). SLIP never closed — likely lost opening `0xC0`, hunt-sync on the trailing END. |

## Commands

```text
fujinet-nio-exchange --type clock|host-get|file-list --backend cold|warm
    [--baud 300..230400]
    [--serial-device NAME] [--serial-unit 0..255]
    [--size 8|16|32|64|128|256|420|512 --uri URI]
    [--list-flags 0..255]
    [--trials N]
```

Redirect a long run:

```text
C:fujinet-nio-exchange --type clock --backend cold --baud 9600 --trials 20 >RAM:t.log
Type RAM:t.log
```

`file-list` needs `--uri`. Use a directory that actually has many names, for
example the same TNFS tree you use with `FLS`:

```text
C:fujinet-nio-exchange --type file-list --backend cold --baud 38400 --size 420 --uri tnfs://192.168.1.101/amiga --trials 20 >RAM:t.log
```

`--trials` defaults to 1. Twenty is enough to see a pattern; 100 per cell is
the research target if a cell is interesting. Do not exceed 100000.

## Suggested order

Keep Workbench as quiet as you can for the first pass (no extra copies,
scrolling, or demos). If 38400 still fails, repeat the failing cell with
deliberate load (move windows, type in another shell).

For each baud in **9600, then 19200, then 38400**:

1. **Clock cold** — smallest useful first FujiBus after reopen.

   ```text
   fujinet-nio-exchange --type clock --backend cold --baud 38400 \
       --serial-device fujinet-serial.device --trials 20
   ```

2. **Clock warm** — same request on the retained backend.

   ```text
   fujinet-nio-exchange --type clock --backend warm --baud 38400 --trials 20
   ```

3. **Host-get warm**, then **host-get cold** (FHOST-shaped reply; `--size` is
   not allowed). Current host/path should already be set if you care about a
   longer string; an unset host still counts as a small reply.

   ```text
   fujinet-nio-exchange --type host-get --backend warm --baud 38400 --trials 20
   fujinet-nio-exchange --type host-get --backend cold --baud 38400 --trials 20
   ```

4. **File-list cold** at `--size 8`, then `64`, `256`, `420`, `512`. Compare
   logged `resp_len`. If `resp_len` stays tiny while `--size` grows, pick a
   fuller directory and redo that size.

   ```text
   fujinet-nio-exchange --type file-list --backend cold --baud 38400 --size 8 --uri tnfs://HOST/path --trials 20
   ```

5. Repeat the file-list sizes as **warm** at the same baud.

6. **FLS-shaped file-list** — same URI you use with `FLS`, `--size 420`,
   `--list-flags 2` (sort-by-name, the extra flags byte `FLS` always sends).
   This is the packet `FLS` sends, not the unsorted `--size 512` soak.

   ```text
   fujinet-nio-exchange --type file-list --backend cold --baud 38400 \
       --size 420 --list-flags 2 --uri tnfs://HOST/path \
       --serial-device fujinet-serial.device --trials 50
   ```

7. **Host-get soak** at the same baud as the file-list soak (20, then 200 if
   clean). `FHOST` with no args is this packet; do not treat `HOST: (none)`
   on an old binary as a listing-size problem.

Only after those cells stay `result=0` with no `fujibus=bad`, copy the new
`FLS` / `FHOST` and retry the CLI. Failures now print
`broker stage=… result=… cause=… native=… status=… raw=…` so they can be
compared to the exchange line. Then disk-read/write provocation
(`--type disk-read|disk-write --provocation`, same `--baud` as the soak,
ESP pacing `0/0/0`; product gate 38400, 57600 opt-in).

If 9600 is clean for clock and size 8 but 38400 fails as size grows, that is
the result this diagnostic is for: burst length / service time, not “serial
is dead.” If cold fails and warm at the same size/baud does not, the first
open after `SET_BAUD` is implicated. If warm also fails, it is not a
cold-only start problem.

Write down, per command: baud, type, backend, `--size` if any, trial count,
how many log lines had `cause=0`, how many had `status=1`, min/max `resp_len`,
and whether the machine was idle.

## Things that are not this test

- No-arg `fujinet-nio-exchange` (prove / Amiberry isolation).
- `FLS` / `FHOST` / `FIN` as the measured command (they share the broker and
  will not give you a controlled first EXCHANGE).
- `fujinet-nio-baud` / `--baud` outside 300..230400 (usage error; no broker open).
- Inferring cold/warm from whether a CLI has exited. The resident broker
  keeps `serial.device` until a close/reconfigure.
