#ifndef FUJINET_NIO_BACKEND_H
#define FUJINET_NIO_BACKEND_H

#include <stdint.h>

/* Request-local exchange diagnostic returned through fn_pad[2].  The public
 * fn_nio_error remains the authoritative exchange result. */
#define FUJINET_NIO_DETAIL_NONE        0
#define FUJINET_NIO_DETAIL_BACKEND_OPEN 1
#define FUJINET_NIO_DETAIL_SERIAL_IO   2
#define FUJINET_NIO_DETAIL_SESSION_IO  3
#define FUJINET_NIO_DETAIL_TIMEOUT     4
#define FUJINET_NIO_DETAIL_SERIAL_WRITE 5
#define FUJINET_NIO_DETAIL_SERIAL_QUERY 6
#define FUJINET_NIO_DETAIL_SERIAL_READ  7
#define FUJINET_NIO_DETAIL_TIMER_WAIT   8
/* CMD_READ failed after session_flush drained IO_STATF_OVERRUN via a
 * sacrificial CMD_READ — the flag was visible to flush but the subsequent
 * CMD_READ still hit an error.  Distinguishes from SERIAL_READ (cause=7)
 * where the flush drain never triggered. */
#define FUJINET_NIO_DETAIL_FLUSH_DRAINED_THEN_READ_FAILED 9

uint8_t backend_open(void);
void backend_close(void);
/* If the last transport error was IO_STATF_OVERRUN, performs a soft reset:
 * keeps serial/timer open (warm backend; Paula RX remains independent) and
 * reinitialises the SLIP session. Returns 1 if recovery applied (do NOT call
 * backend_close); 0 otherwise. */
uint8_t backend_recover_from_overrun(void);
uint8_t backend_exchange(
    const uint8_t *request,
    uint16_t request_len,
    uint8_t *response,
    uint16_t response_capacity,
    uint16_t *response_len,
    uint8_t *detail,
    uint8_t *native_io_error,
    uint16_t *native_status);
uint8_t backend_set_baud(uint32_t baud);
uint32_t backend_get_baud(void);

#endif
