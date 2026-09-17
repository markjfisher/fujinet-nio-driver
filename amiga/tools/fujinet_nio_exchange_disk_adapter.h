#ifndef FUJINET_NIO_EXCHANGE_DISK_ADAPTER_H
#define FUJINET_NIO_EXCHANGE_DISK_ADAPTER_H
#include <devices/trackdisk.h>
#include "fujinet_nio_device.h"
#include "fujinet_disk_device.h"
#include "fujinet-nio.h"
#include "fujinet_nio_exchange_disk.h"
struct ordinary_disk_context {
    struct MsgPort *port;
    struct IORequest nio_open;
    struct FujiNetNIORequest nio;
    struct IOExtTD disk;
    const struct fn_nio_exchange_opts *opts;
    fn_disk_client_context_t codec;
    uint8_t write_buffer[512];
    uint8_t read_buffer[512];
    struct fujinet_disk_trace trace;
};
uint8_t fn_exchange_disk_adapter_exchange(void *, const uint8_t *, uint16_t,
                                          uint8_t *, uint16_t, uint16_t *);
int fn_exchange_disk_adapter_io(void *, enum fn_exchange_disk_op,
                                struct fn_exchange_disk_state *, uint8_t *);
#endif
