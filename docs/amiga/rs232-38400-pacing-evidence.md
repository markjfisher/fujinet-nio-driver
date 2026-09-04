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

A lost byte does not become a successful 512-byte block. There is **no**
NIO-layer retry of that DiskDevice command; AmigaDOS/trackdisk may retry
or put up a requester. Write that the ESP already committed, then lost the
ACK, can be replayed as the same 512-byte sector (idempotent for ADF).
