# Amiga write, cache, and media-change policy

This is the durable contract for writable `fujinet-disk.device` media. It is
based on the AmigaOS [Trackdisk Device](https://wiki.amigaos.net/wiki/Trackdisk_Device),
[Exec Device I/O](https://wiki.amigaos.net/wiki/Exec_Device_I/O), and the
virtual-ADF precedent in
[`trackfile.device`](https://developer.amigaos3.net/autodocs/trackfile.device/).

## Data and queue semantics

The resident driver owns no block-data cache. `CMD_WRITE` and `ETD_WRITE`
send aligned 512-byte sectors synchronously to NIO. NIO owns file buffering
and the dirty flag. Successful writes are accepted but are durable only after
a successful update, eject, or replacement.

`CMD_UPDATE` and `ETD_UPDATE` are ordering barriers and issue DiskDevice
`Flush`. `CMD_CLEAR` and `ETD_CLEAR` invalidate driver cache state without
writing it; with no driver block cache they are successful no-ops after ETD
validation. Exec `CMD_FLUSH` has its queue meaning: it aborts requests waiting
in the device FIFO and never flushes media data. `AbortIO` removes a queued
request but cannot cancel the active transport operation. `CMD_STOP` holds
queued work and `CMD_START` resumes FIFO draining.

## Extended trackdisk requests

An ETD request runs only when the device's current change count is no greater
than `iotd_Count`; `0xFFFFFFFF` disables stale-media rejection. A non-null
sector-label buffer is rejected with `IOERR_NOCMD`, since raw ADF storage has
no label area.

## Media state

Only mounts and ejects issued through this device are observed. First
insertion increments the 32-bit counter once, ejection once, and replacement
is removal plus insertion (twice). A failed pre-removal flush changes neither
media nor count. `TD_CHANGENUM`, `TD_CHANGESTATE`, and `TD_PROTSTATUS` report
this committed local state.

`TD_REMOVE` synchronously installs the unit's single legacy change interrupt
from `io_Data`, or removes it when `io_Data` is null; its request is never
retained. Every `TD_ADDCHANGEINT` registration is retained and its interrupt is
`Cause()`d for every committed transition. `TD_REMCHANGEINT` removes and
completes that same request exactly once. After adopting NIO state the device calls `ClearChanged`; an
acknowledgement failure remains pending for retry and never rolls back the
local transition.

Out-of-band changes to NIO slots are unsupported. Units 0–7 map to DiskDevice
slots 1–8; each accepts only standard 512-byte, 1760-block ADF media and owns
its protection, change count, notification, and queued-request state.
