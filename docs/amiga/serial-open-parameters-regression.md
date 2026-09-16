# Serial open defaults caused the September 2026 Amiberry regression

The Amiga broker must configure its `IOExtSer` both before `OpenDevice` and
again before `SDCMD_SETPARAMS`. The custom `fujinet-serial.device` uses the
pre-open baud to program Paula when claiming the port. Stock `serial.device`
replaces the request's serial parameters with its defaults during open.
Passing those defaults unchanged to `SETPARAMS` enables XON/XOFF processing,
which consumes binary FujiBus bytes `0x11` and `0x13`.

The regression came from driver commit
`62168e9d509c20ca12a1060b74b89aceca22cdce` (2026-09-07), which moved the
parameter assignments from after open to before open for the custom device.
Both points require the requested settings; moving the assignments back alone
would lose the custom device's initial-baud behavior.

## Captured evidence

The failing test was:

```sh
scripts/amiga-tests --amiga-env wb32 --amiga-machine a1200-030 \
  test_amiga_fin_ffs_adf.py::test_fin_mounts_and_reads_ffs_adf
```

A normal failing run is retained under
`test-evidence/amiberry-20260916-231818/amiga-fin-ffs-adf/`.
Firmware mounted successfully; the guest reported `FMOUNT MOUNT_CATALOG rc=20`.

The host debugger capture under
`test-evidence/amiberry-20260916-232137/amiga-fin-ffs-adf/` contains
`session-raw.log`, `capture-session.py`, `nio-disassembly.txt`, and the usual
firmware/emulator logs. Breakpoints inspected existing guest memory, with no
guest code or memory edits. The loaded code base was `0x23f688`, validated
against Open, Close, BeginIO and AbortIO vectors. Relevant link offsets were
`0x17c0` (OpenDevice call), `0x17e4` (SETPARAMS DoIO), and `0x2af2` (SLIP decode).
These addresses apply only to that captured binary, extracted from the retained
HDF at `DEVS/fujinet-nio.device`, SHA-256:
`d93c46b1d001acc91dfc030a57cba68c258cb2cb307414c3cb9dbfdd69acb8d9`.
Driver baseline: `091010013977805b82fecd9efb84fc31f574d882`;
library baseline: `dac8bf66c4ec44841790e08021c1654379c21255`, with the inherited
empty-frame skip change still present during capture. All evidence paths here
are relative to the FujiNet NIO workspace. The field values and packet bytes
above/below are retained in this document even if local evidence is cleaned up.

| Field | Before OpenDevice | At SETPARAMS |
|---|---:|---:|
| Baud | 19200 | 9600 |
| Receive buffer | 2112 | 512 |
| Read/write bits | 8/8 | 8/8 |
| Stop bits | 1 | 1 |
| Serial flags | `0x90` | `0x00` |

The mount response should have reached SLIP decode as:

```text
c0 fc 01 13 00 04 01 00 01 03 00 00 01 04 00 02 e0 06 00 00 c0
```

The captured decoder input was:

```text
c0 fc 01    00 04 01 00 01 03 00 00 01 04 00 02 e0 06 00 00 c0
```

Only the `0x13` length byte is missing. The earlier decoder capture in
`test-evidence/amiberry-20260916-232035/amiga-fin-ffs-adf/session-raw.log`
also shows a 17-byte response losing its `0x11` length byte. This explains why
some commands worked and others failed with `SESSION_IO`. The strict length
check was correctly rejecting damaged packets.

The previous uncommitted 16-byte inspect request avoided a particular damaged
reply; it did not establish a size limitation. The empty-SLIP retry, cached
catalogue URI, changed mapping persistence, skipped FHOST, and modified FFS
boot block were not justified fixes for this fault. They have been removed.
The test's guard against running `Dir` after failed FMOUNT is retained so a
failure is reported instead of opening an insert-volume requester.

## Verification

The original FHOST/FIN/FMOUNT/Dir test passed in 11.97 seconds after restoring
the 512-byte inspect request, original FFS fixture, strict session validation,
and original mapping transaction. Its 529-byte inspect packet and 19-byte
mount packet exercise `0x11` and `0x13` respectively in their length fields.
The complete library check (Atari, BBC, BBC CLIB, all three MS-DOS transports,
Linux and Amiga), driver native tests, and Amiga application build also passed.
Final full-suite acceptance is recorded in the workspace at
`completed/amiga-test-suite-serial-regression.md`.
No debugger, packet capture, or timeout override is needed for the acceptance
runs. This diagnosis concerns stock serial.device initialization; it does not
establish a cause for the separately tracked physical high-baud RS232 failures.
