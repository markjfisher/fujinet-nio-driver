#ifndef FUJINET_NIO_DIRECTORY_BACKEND_H
#define FUJINET_NIO_DIRECTORY_BACKEND_H

#include <stdint.h>

#include "fujinet_nio_packet_backend.h"

/* Test-only Client-role shared-directory packet I/O. Not a Zorro, SLIP,
 * serial, or production backend. File size is the record boundary. */

#define FN_DIRECTORY_PACKET_CAPACITY 65535U
#define FN_DIRECTORY_TO_HOST_NAME "to-host.pkt"
#define FN_DIRECTORY_TO_GUEST_NAME "to-guest.pkt"
#define FN_DIRECTORY_IDENTITY_NAME "IDENTITY"
#define FN_DIRECTORY_IDENTITY_TOKEN "native-test"
#define FN_DIRECTORY_IDENTITY_BODY "native-test\n"
#define FN_DIRECTORY_DEFAULT_VOLUME "NATIVE:"

enum {
    FN_DIR_OK = 0,
    FN_DIR_NODATA,
    FN_DIR_EMPTY,
    FN_DIR_OVERSIZED,
    FN_DIR_BACKPRESSURE,
    FN_DIR_UNAVAILABLE,
    FN_DIR_FAILED,
    FN_DIR_TRUNCATED
};

extern const fn_packet_io_t fujinet_nio_directory_io;

int fujinet_nio_directory_client_send(const uint8_t *packet, uint32_t size);
int fujinet_nio_directory_client_receive(uint8_t *buffer, uint16_t capacity,
                                         uint16_t *length);
int fujinet_nio_directory_client_reset(void);
int fujinet_nio_directory_records_absent(void);

#endif
