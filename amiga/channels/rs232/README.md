# RS-232 channel binding

The first Amiga driver backend delegates to the public `fujinet-nio-lib`
DiskDevice calls. The library's Amiga transport opens `fujinet-nio.device`;
the broker serial backend owns `serial.device` and routes the byte stream
through the shared session implementation.

This directory is deliberately an adapter, not another protocol stack. A
future channel backend must preserve the same driver/client contract without
copying DiskDevice packet encoding into the native driver.
