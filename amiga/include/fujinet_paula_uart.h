#ifndef FUJINET_PAULA_UART_H
#define FUJINET_PAULA_UART_H

#include <stdint.h>

/* AHRM Ch. 8: color clocks used by SERPER. NTSC 3.579545 MHz, PAL 3.546895 MHz. */
#define FUJINET_PAULA_NTSC_CLOCK     3579545UL
#define FUJINET_PAULA_PAL_CLOCK      3546895UL
#define FUJINET_PAULA_SERPER_MAX     0x7FFFU

/* SERDAT: 8 data bits plus a stop-bit 1 in bit 8. Hardware adds the start bit. */
#define FUJINET_PAULA_SERDAT_STOP    0x0100U

/* SERDATR status bits (AHRM Table 8-9). */
#define FUJINET_PAULA_SERDATR_OVRUN  0x8000U
#define FUJINET_PAULA_SERDATR_RBF    0x4000U
#define FUJINET_PAULA_SERDATR_TBE    0x2000U
#define FUJINET_PAULA_SERDATR_TSRE   0x1000U

#define FUJINET_PAULA_RX_DEFAULT_SIZE 2048U
#define FUJINET_SERIAL_BAUD_MIN       300UL
#define FUJINET_SERIAL_BAUD_MAX       230400UL

typedef struct fujinet_paula_rx {
    uint8_t *buf;
    uint16_t mask;
    uint16_t head;
    uint16_t tail;
    uint8_t hardware_overrun_latched;
    uint8_t software_ring_overflow_latched;
} fujinet_paula_rx_t;

uint16_t fujinet_paula_serper(uint32_t baud, int pal);
uint16_t fujinet_paula_serdat_word(uint8_t data);

/* size must be a power of two and at least 2. Returns 0 on success. */
int fujinet_paula_rx_init(fujinet_paula_rx_t *rx, uint8_t *buf, uint16_t size);
void fujinet_paula_rx_clear(fujinet_paula_rx_t *rx);
uint16_t fujinet_paula_rx_count(const fujinet_paula_rx_t *rx);
void fujinet_paula_rx_ingest(fujinet_paula_rx_t *rx, uint16_t serdatr);
uint16_t fujinet_paula_rx_read(fujinet_paula_rx_t *rx, uint8_t *dst, uint16_t n);
uint8_t fujinet_paula_rx_public_overrun(const fujinet_paula_rx_t *rx);
int fujinet_serial_params_valid(uint32_t baud, unsigned read_len,
                                unsigned write_len, unsigned stop_bits,
                                int parity_on);

#endif
