# RS-232 channel binding

The first Amiga driver backend delegates to the public `fujinet-nio-lib`
DiskDevice calls. The library's Amiga transport opens `fujinet-nio.device`;
the broker serial backend owns `serial.device` and routes the byte stream
through the shared session implementation.

This directory is deliberately an adapter, not another protocol stack. The
only packet-aware exception is bounded recovery for complete DiskService
`READ_SECTOR` and `WRITE_SECTOR` exchanges. After a raw transport timeout or
error, the adapter may replay a structurally valid, checksum-valid request at
most twice. The complete encoded request must describe slot 1--8 and exactly
one 512-byte sector, so every replay keeps the same slot, LBA, and full write
body. Failed attempts publish no response length.

No other command is retried here. In particular, Mount, Info, Flush, Unmount,
ClearChanged, Inspect, network, application, malformed, and incomplete sector
requests remain single-attempt operations. A future channel backend must
preserve the same driver/client contract and this narrow replay boundary
without growing another general protocol stack in the native driver.
