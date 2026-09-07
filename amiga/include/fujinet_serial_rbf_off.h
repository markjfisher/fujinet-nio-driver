#ifndef FUJINET_SERIAL_RBF_OFF_H
#define FUJINET_SERIAL_RBF_OFF_H

/*
 * Offsets into struct fujinet_serial_base for the RBF assembler server.
 * fujinet_serial_device.c has matching C99 size checks; if those fail,
 * the struct layout changed and these values must be updated together.
 */
#define FN_SERIAL_OFF_RX_BUF     2146
#define FN_SERIAL_OFF_RX_MASK    2150
#define FN_SERIAL_OFF_RX_HEAD    2152
#define FN_SERIAL_OFF_RX_TAIL    2154
#define FN_SERIAL_OFF_RX_OVERRUN 2156

#endif
