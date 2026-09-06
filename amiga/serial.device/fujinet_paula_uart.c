#include "fujinet_paula_uart.h"

#include <stddef.h>

uint16_t fujinet_paula_serper(uint32_t baud, int pal)
{
    uint32_t clock;
    uint32_t period;

    if (baud < 110UL) baud = 110UL;
    clock = pal ? FUJINET_PAULA_PAL_CLOCK : FUJINET_PAULA_NTSC_CLOCK;
    period = clock / baud;
    if (period == 0UL) period = 1UL;
    period -= 1UL;
    if (period > FUJINET_PAULA_SERPER_MAX) period = FUJINET_PAULA_SERPER_MAX;
    return (uint16_t)period;
}

uint16_t fujinet_paula_serdat_word(uint8_t data)
{
    return (uint16_t)(FUJINET_PAULA_SERDAT_STOP | (uint16_t)data);
}

int fujinet_paula_rx_init(fujinet_paula_rx_t *rx, uint8_t *buf, uint16_t size)
{
    if (rx == NULL || buf == NULL || size < 2U) return -1;
    if ((size & (uint16_t)(size - 1U)) != 0U) return -1;
    rx->buf = buf;
    rx->mask = (uint16_t)(size - 1U);
    rx->head = 0;
    rx->tail = 0;
    rx->overrun = 0;
    return 0;
}

void fujinet_paula_rx_clear(fujinet_paula_rx_t *rx)
{
    if (rx == NULL) return;
    rx->head = 0;
    rx->tail = 0;
    rx->overrun = 0;
}

uint16_t fujinet_paula_rx_count(const fujinet_paula_rx_t *rx)
{
    if (rx == NULL) return 0;
    return (uint16_t)((rx->head - rx->tail) & rx->mask);
}

void fujinet_paula_rx_ingest(fujinet_paula_rx_t *rx, uint16_t serdatr)
{
    uint16_t next;

    if (rx == NULL) return;
    if (serdatr & FUJINET_PAULA_SERDATR_OVRUN) rx->overrun = 1;
    if ((serdatr & FUJINET_PAULA_SERDATR_RBF) == 0) return;
    next = (uint16_t)((rx->head + 1U) & rx->mask);
    if (next == rx->tail) {
        rx->overrun = 1;
        return;
    }
    rx->buf[rx->head] = (uint8_t)serdatr;
    rx->head = next;
}

uint16_t fujinet_paula_rx_read(fujinet_paula_rx_t *rx, uint8_t *dst, uint16_t n)
{
    uint16_t copied = 0;

    if (rx == NULL || (dst == NULL && n != 0)) return 0;
    while (copied < n && rx->tail != rx->head) {
        dst[copied++] = rx->buf[rx->tail];
        rx->tail = (uint16_t)((rx->tail + 1U) & rx->mask);
    }
    return copied;
}
