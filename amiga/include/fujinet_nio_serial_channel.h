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

/*
 * After CMD_READ overrun (cause=7), the ESP may still be transmitting the
 * rest of a paced response (16-byte bursts with 2 ms gaps). One empty
 * SDCMD_QUERY is only an inter-chunk gap. Require this much consecutive
 * empty RX before CloseDevice so the next EXCHANGE does not parse leftover
 * SLIP as SESSION_IO (cause=3).
 */
#define FN_SERIAL_DRAIN_SLICE_MS 10
#define FN_SERIAL_DRAIN_IDLE_MS  30
#define FN_SERIAL_DRAIN_CAP_MS   1000

#define FN_SERIAL_DRAIN_KEEP 0
#define FN_SERIAL_DRAIN_DONE 1

static inline uint8_t fn_serial_drain_after_fault_step(uint8_t saw_rx,
                                                       uint16_t *empty_ms,
                                                       uint16_t *waited_ms)
{
    if (empty_ms == NULL || waited_ms == NULL) return FN_SERIAL_DRAIN_DONE;
    if (saw_rx) {
        *empty_ms = 0;
        return FN_SERIAL_DRAIN_KEEP;
    }
    if (*empty_ms >= FN_SERIAL_DRAIN_IDLE_MS ||
        *waited_ms >= FN_SERIAL_DRAIN_CAP_MS)
        return FN_SERIAL_DRAIN_DONE;
    *empty_ms = (uint16_t)(*empty_ms + FN_SERIAL_DRAIN_SLICE_MS);
    *waited_ms = (uint16_t)(*waited_ms + FN_SERIAL_DRAIN_SLICE_MS);
    return FN_SERIAL_DRAIN_KEEP;
}

#endif
