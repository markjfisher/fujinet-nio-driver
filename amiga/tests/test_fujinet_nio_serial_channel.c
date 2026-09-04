#include "fujinet_nio_serial_channel.h"

#include <stdio.h>

static unsigned failures;

#define CHECK(name, expression) do {                                      \
    if (!(expression)) {                                                  \
        fprintf(stderr, "FAIL: %s (line %d)\n", name, __LINE__);          \
        ++failures;                                                       \
    }                                                                     \
} while (0)

int main(void)
{
    uint8_t channel_error = 0;

    CHECK("FN_OK is a byte",
          fn_serial_channel_got_byte(FN_OK, &channel_error) == 1);
    CHECK("FN_OK leaves channel clean", channel_error == 0);

    CHECK("poll timeout is not a byte",
          fn_serial_channel_got_byte(FN_ERR_TIMEOUT, &channel_error) == 0);
    CHECK("poll timeout is not sticky", channel_error == 0);
    CHECK("poll timeout maps as timeout",
          fn_serial_channel_map_session_result(FN_ERR_TIMEOUT, &channel_error) ==
              FN_ERR_TIMEOUT);

    CHECK("not-ready is a poll miss",
          fn_serial_channel_got_byte(FN_ERR_NOT_READY, &channel_error) == 0);
    CHECK("not-ready is not sticky", channel_error == 0);

    CHECK("serial IO is not a byte",
          fn_serial_channel_got_byte(FN_ERR_IO, &channel_error) == 0);
    CHECK("serial IO is sticky", channel_error == FN_ERR_IO);
    CHECK("sticky IO maps session timeout to TRANSPORT",
          fn_serial_channel_map_session_result(FN_ERR_TIMEOUT, &channel_error) ==
              FN_ERR_TRANSPORT);
    CHECK("sticky consumed", channel_error == 0);

    CHECK("session FN_ERR_IO maps to TRANSPORT",
          fn_serial_channel_map_session_result(FN_ERR_IO, &channel_error) ==
              FN_ERR_TRANSPORT);

    {
        uint16_t empty_ms = 0;
        uint16_t waited_ms = 0;
        unsigned i;

        CHECK("RX keeps drain going",
              fn_serial_drain_after_fault_step(1, &empty_ms, &waited_ms) ==
                  FN_SERIAL_DRAIN_KEEP);
        CHECK("RX resets idle counter", empty_ms == 0);

        for (i = 0; i < 3; ++i) {
            CHECK("empty slice keeps waiting",
                  fn_serial_drain_after_fault_step(0, &empty_ms, &waited_ms) ==
                      FN_SERIAL_DRAIN_KEEP);
        }
        CHECK("30 ms empty is idle", empty_ms == FN_SERIAL_DRAIN_IDLE_MS);
        CHECK("idle after 30 ms empty",
              fn_serial_drain_after_fault_step(0, &empty_ms, &waited_ms) ==
                  FN_SERIAL_DRAIN_DONE);

        empty_ms = 0;
        waited_ms = FN_SERIAL_DRAIN_CAP_MS;
        CHECK("cap stops drain",
              fn_serial_drain_after_fault_step(0, &empty_ms, &waited_ms) ==
                  FN_SERIAL_DRAIN_DONE);
        CHECK("null pointers stop drain",
              fn_serial_drain_after_fault_step(0, NULL, &waited_ms) ==
                  FN_SERIAL_DRAIN_DONE);
    }

    if (failures) {
        fprintf(stderr, "%u serial-channel tests failed\n", failures);
        return 1;
    }
    return 0;
}
