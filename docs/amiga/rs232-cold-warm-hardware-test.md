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

This pass covers 9600, 19200, and 38400 only. Do not use 57600 yet.

## What you need

- FujiNet ESP already listening at the baud you will test (Amiga `SET_BAUD`
  only changes the Amiga UART).
- Resident `fujinet-nio.device` loaded. Build with `make native` from
  `amiga/` and install:

  ```text
  Copy fujinet-nio.device DEVS:
  Copy fujinet-nio-exchange C:
  C:fujinet-load-resident DEVS:fujinet-nio.device fujinet-nio.device
  ```

- **No other FujiNet serial client** during the matrix. Unload
  `fujinet-disk.device` if it is resident, and do not run `FLS`, `FHOST`, or
  `FIN` in another shell. Those would open the broker and change cold/warm
  without you seeing it.

Confirm isolation with **one** matrix command, not the no-arg prove tool:

```text
C:fujinet-nio-exchange --type clock --backend cold --baud 38400 --trials 1
```

(Use the baud the ESP is already on.) You want one `req_len=… backend=cold`
line and `exit 0`.

Do **not** run `C:fujinet-nio-exchange` with no arguments on PiStorm or real
hardware. That is the Amiberry isolation suite: it sends a malformed packet
to force a timeout, then `CreateNewProc` two extra processes for concurrent
clock commands. On PiStorm that sequence has completed with `PASS` and then
rebooted the machine (power LED flash, no Guru). Isolation is already
`disk.device` unloaded and no `FLS`/`FHOST`/`FIN` in another shell.

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
(SESSION_IO). Do not treat CIA TX→RX as the mechanism. This pass is 38400
only: no 57600, seven-wire, READY/GO, or a custom `serial.device`.

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
| `native` | `serial.device` `io_Error`. `0` is none. |
| `status` | High byte of `serial.device` `io_Status`. `1` is `IO_STATF_OVERRUN`. |
| `backend` | `cold` or `warm` as requested, not inferred. |

`--size` on `file-list` is LIST `maxPayloadBytes` (how much directory blob
you asked for). The directory must be large enough to fill that cap or
`resp_len` will be a small listing, not a large burst.

### Pass vs fail on one line

**Pass:** `result=0 cause=0 native=0 status=0` and a plausible `resp_len`
(clock and host-get are small; file-list should grow with `--size` until the
directory runs out).

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

## Commands

```text
fujinet-nio-exchange --type clock|host-get|file-list --backend cold|warm
    [--baud 9600|19200|38400]
    [--size 8|16|32|64|128|256|420|512 --uri URI]
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
   fujinet-nio-exchange --type clock --backend cold --baud 38400 --trials 20
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
- `fujinet-nio-baud 57600` or `--baud 57600` (usage error; no broker open).
- Inferring cold/warm from whether a CLI has exited. The resident broker
  keeps `serial.device` until a close/reconfigure.
