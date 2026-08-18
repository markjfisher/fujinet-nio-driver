#ifndef FUJINET_DISK_STUB_DOS_DOS_H
#define FUJINET_DISK_STUB_DOS_DOS_H

/* The real NDK header provides BPTR. The native-test exec/types.h stub
 * already defines it, so this compatibility header only needs to expose that
 * existing definition. */
#include <exec/types.h>

#endif
