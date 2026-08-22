#ifndef FUJINET_NIO_SERIAL_CHANNEL_H
#define FUJINET_NIO_SERIAL_CHANNEL_H

#include <stdint.h>

#include "fujinet-nio.h"

/*
 * Maps a serial_read_byte() result onto fn_stream_channel_ops_t.read_byte:
 * 1 = a byte was stored, 0 = no byte this poll.
 *
 * FN_ERR_TIMEOUT / FN_ERR_NOT_READY stay non-fatal poll misses.
 * Any other non-OK result is a sticky channel error so backend_exchange()
 * can return FN_ERR_TRANSPORT after the session call.
 */
static inline uint8_t fn_serial_channel_got_byte(uint8_t serial_rc,
                                                 uint8_t *channel_error)
{
    if (serial_rc == FN_OK) return 1;
    if (serial_rc == FN_ERR_TIMEOUT || serial_rc == FN_ERR_NOT_READY)
        return 0;
    if (channel_error != NULL && *channel_error == 0)
        *channel_error = serial_rc;
    return 0;
}

static inline uint8_t fn_serial_channel_map_session_result(
    uint8_t session_rc, uint8_t *channel_error)
{
    if (channel_error != NULL && *channel_error != 0) {
        *channel_error = 0;
        return FN_ERR_TRANSPORT;
    }
    if (session_rc == FN_ERR_IO) return FN_ERR_TRANSPORT;
    return session_rc;
}

#endif
