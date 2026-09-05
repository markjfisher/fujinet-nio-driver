# Evidence: Amiga RS-232 38400 response pacing (research rank 2)

PiStorm + ESP FujiBus, `fujinet-nio-exchange`, 38400 8N1. Paula RBF overrun
signature: `result=16 cause=7 native=6 status=1`. Do not treat CIA TX→RX as
the mechanism. Recorded 2026-09-04.

Product ESP→host profile: `tx_byte_gap_us=0`, `tx_chunk_size=16`,
`tx_chunk_gap_us=2000`. Line baud remains 38400 both ways. Long ESP→Amiga
payloads average about **26 kbaud** (16 × ~262 µs + 2 ms idle; last chunk
has no trailing gap). Amiga→ESP requests stay unpaced 38400.

## Rank 1 unpaced (20-trial files in workspace `exchange-results/`)

Cold file-list, `resp_len` is the logged FujiBus size:

| `--size` | `resp_len` | Pass | Overrun | Typical `elapsed_us` |
| ---: | ---: | ---: | ---: | ---: |
| 8 | 7 | 20/20 | 0 | ~23 ms |
| 256 | 266 | 20/20 | 0 | ~101 ms |
| 420 | 434 | 20/20 | 0 | ~151 ms |
| 512 | 513 | 18/20 | **2** | ~173 ms (success) |

Warm host-get unpaced was about 1/100. Clock at 38400 was clean. Same LIST
sizes were clean at 9600.

## Inter-byte gap (38400)

Warm host-get was 0 errors at every gap ≥ 125 µs (hundreds of trials).

| Gap µs | Cold LIST 256 | Cold LIST 512 | Warm host-get | 256 ms | 512 ms |
| ---: | --- | --- | --- | ---: | ---: |
| 750 | 0/100 | 0/200 | 0/300 then 0/200 | 310 | 575 |
| 500 | 1/200 | 0/200 | 0/200 | 245 | 446 |
| 250 | 1/100 | 1/200 | 0/200 | 175 | 316 |
| 125 | 1/100 | 1/200 | 0/200 | 142 | 255 |

750 µs is 9600-shaped on the return. Residual ~0.5–1% on cold LIST at
125–500 µs did not track payload size.

## Chunk pacing (`tx_byte_gap_us=0`)

Long 200-trial cells (ignore short follow-up tens; those clustered after an
overrun or load):

| size / gap µs | Cold 256 | Cold 512 | Warm host-get | 256 ms | 512 ms |
| --- | --- | --- | --- | ---: | ---: |
| 16 / 1000 | 1/200 | 0/200 | 0/200 | 118 | 207 |
| 16 / 500 | 1/200 | 0/200 | 0/200 | 110 | 190 |
| 8 / 1000 | 1/200 | 1/200 | 0/200 | 136 | 240 |
| 32 / 1000 | 1/200 | 1/200 | 1 crash then 0/200 | 110 | 192 |
| 32 / 500 | 2/200 | 0/200 | 0/200 | 105 | 182 |
| 8 / 2000 | 0/200 | 0/200 | 0/200 | 168 | 303 |
| 16 / 2000 | 0/200 | 0/200† | 0/200 | 134 | 238 |
| 16 / 2000 soak | **0/1000** | **1/500** | **0/1000** | 134 | 238 |
| 12 / 1200 | 0/300 | 1/300 | 0/300 | 128 | 225 |

† One 16/2000 size-512 start had 1 overrun then sticky `cause=3` (killed).
That was **before** drain-until-idle recovery.

16/500 extra tens after a clean 200 showed 1–2/10; not i.i.d. One 32/1000
warm host-get crashed the PiStorm (power LED); retry was 0/200.

## Stream recovery (drain-until-idle, then `CloseDevice`)

After `cause=7`, ESP can still be sending the rest of a 16/2000 frame.
Mixing that leftover SLIP into the next EXCHANGE produced
`resp_len=0 result=16 cause=3 native=0 status=0` at about half the usual
elapsed time, every trial, until idle.

With drain until 30 ms consecutive empty RX (longer than the 2 ms chunk
gap), then close and lazy-reopen:

| Cell | Trials | Overrun | `cause=3` after `cause=7` | ms |
| --- | ---: | ---: | --- | ---: |
| Cold LIST 256 | 50 | 0 | — | 135 |
| Cold LIST 512 | 50 | 1 | none | 238 |
| Cold LIST 512 | 50 | 1 | none | 238 |
| Cold LIST 256 | 50 | 3 | none | (same profile) |

`cause=7` still occurs (cold LIST floor, clustered). The **next** trial is
not `cause=3`. That is stream recovery, not a zero-overrun claim.

## Fail-closed on the failed packet

On `FN_ERR_TRANSPORT` / `FN_ERR_TIMEOUT`:

- Broker sets `fn_response_length = 0` (no payload published).
- Disk `CMD_READ`/`CMD_WRITE` start with `io_Actual = 0`;
  `FN_ERR_TRANSPORT` maps to `TDERR_NotSpecified`.
- `fn_disk_*` returns before copying a truncated FujiBus body into the
  caller buffer.

A lost byte does not become a successful 512-byte block. The RS-232
DiskDevice adapter now retries only structurally valid, byte-identical sector
READ/WRITE requests up to two times after the initial attempt. Each retry
follows the broker's drain-until-idle and close/reopen recovery. Non-sector
and non-idempotent commands are not retried. A write that the ESP already
committed before the ACK was lost can therefore be replayed as the same
512-byte ADF sector.

The retry implementation is covered by deterministic native tests, but real
38400-baud hardware proof remains open: deliberately observe a `cause=7`
during `CMD_READ`/`CMD_WRITE` and record either full success after retry or a
bounded persistent error, never a short successful block.

## Hardware-proof procedure (added 2026-09-05)

The deterministic provocation path is the resident DiskDevice diagnostic, not
the generic raw exchange matrix. With a mounted slot, run both operations:

```text
fujinet-nio-exchange --type disk-read --provocation --backend cold \
  --baud 38400 --slot 1 --lba 0 --trials 100
fujinet-nio-exchange --type disk-write --provocation --backend cold \
  --baud 38400 --slot 1 --lba 0 --trials 100
```

Configure the ESP only for this run as
`tx_byte_gap_us=0 tx_chunk_size=0 tx_chunk_gap_us=0`. The mode is rejected
unless `--provocation`, cold backend, 38400 baud, slot, and LBA are all
explicit. It sends exactly one valid 512-byte DiskService sector request per
trial; WRITE uses the deterministic byte pattern `byte[i] = i ^ 0x5a`.
Each trace line records operation, slot/LBA, request and response length,
elapsed time, result, cause, native error, status, attempt ordinal, final
`io_Error`, and `io_Actual`. The tool clears its diagnostic trace before the
run. Restore the product profile (16-byte chunks and 2000-us gaps) after the
provocation.

Execution count in this workspace is 0 READ + 0 WRITE trials: no physical
Amiga/ESP session was available when this procedure was added, so there is no
new measured `cause=7` result to claim. The hardware-proof checkbox remains
open pending a real Amiga/ESP run with both READ and same-sector WRITE.
