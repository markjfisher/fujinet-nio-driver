#include "fujinet_paula_uart.h"

#include <stdio.h>
#include <string.h>

static unsigned failures;

#define CHECK(name, expression) do {                                      \
    if (!(expression)) {                                                  \
        fprintf(stderr, "FAIL: %s (line %d)\n", name, __LINE__);          \
        ++failures;                                                       \
    }                                                                     \
} while (0)

int main(void)
{
    uint8_t buf[8];
    fujinet_paula_rx_t rx;
    uint8_t out[8];
    unsigned i;

    CHECK("serper-ntsc-9600", fujinet_paula_serper(9600UL, 0) == 371U);
    CHECK("serper-pal-38400", fujinet_paula_serper(38400UL, 1) == 91U);
    CHECK("serper-pal-19200", fujinet_paula_serper(19200UL, 1) == 183U);
    CHECK("serdat-A", fujinet_paula_serdat_word(0x41) == 0x0141U);
    CHECK("serdat-zero", fujinet_paula_serdat_word(0) == 0x0100U);

    CHECK("rx-init", fujinet_paula_rx_init(&rx, buf, 8) == 0);
    CHECK("rx-init-bad-size", fujinet_paula_rx_init(&rx, buf, 7) != 0);
    CHECK("rx-init-ok-again", fujinet_paula_rx_init(&rx, buf, 8) == 0);
    CHECK("empty", fujinet_paula_rx_count(&rx) == 0);

    fujinet_paula_rx_ingest(&rx, FUJINET_PAULA_SERDATR_RBF | 0x11);
    fujinet_paula_rx_ingest(&rx, FUJINET_PAULA_SERDATR_RBF | 0x22);
    CHECK("count-2", fujinet_paula_rx_count(&rx) == 2);
    CHECK("read-1", fujinet_paula_rx_read(&rx, out, 1) == 1 && out[0] == 0x11);
    CHECK("count-1", fujinet_paula_rx_count(&rx) == 1);
    CHECK("read-rest", fujinet_paula_rx_read(&rx, out, 8) == 1 && out[0] == 0x22);
    CHECK("empty-again", fujinet_paula_rx_count(&rx) == 0);

    fujinet_paula_rx_clear(&rx);
    for (i = 0; i < 7; ++i)
        fujinet_paula_rx_ingest(&rx, (uint16_t)(FUJINET_PAULA_SERDATR_RBF | i));
    CHECK("almost-full", fujinet_paula_rx_count(&rx) == 7);
    CHECK("no-overrun-yet", rx.overrun == 0);
    fujinet_paula_rx_ingest(&rx, FUJINET_PAULA_SERDATR_RBF | 0x99);
    CHECK("soft-overrun", rx.overrun == 1);
    CHECK("still-7", fujinet_paula_rx_count(&rx) == 7);

    fujinet_paula_rx_clear(&rx);
    fujinet_paula_rx_ingest(&rx, FUJINET_PAULA_SERDATR_OVRUN | FUJINET_PAULA_SERDATR_RBF | 0xAB);
    CHECK("hw-overrun", rx.overrun == 1);
    CHECK("kept-byte", fujinet_paula_rx_read(&rx, out, 1) == 1 && out[0] == 0xAB);

    fujinet_paula_rx_clear(&rx);
    fujinet_paula_rx_ingest(&rx, FUJINET_PAULA_SERDATR_TBE);
    CHECK("tbe-only-ignored", fujinet_paula_rx_count(&rx) == 0 && rx.overrun == 0);

    if (failures != 0) {
        fprintf(stderr, "%u fujinet_paula_uart checks failed\n", failures);
        return 1;
    }
    return 0;
}
