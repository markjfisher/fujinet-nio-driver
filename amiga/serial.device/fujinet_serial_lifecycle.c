#include "fujinet_serial_lifecycle.h"

#include <string.h>

static void *const k_fujinet_handler = (void *)(uintptr_t)0xF1;
static void *const k_kickstart_handler = (void *)(uintptr_t)0x5C;

static void trace_add(int *trace, unsigned *n, int value)
{
    if (*n < FN_SERIAL_LC_TRACE) {
        trace[*n] = value;
        *n += 1U;
    }
}

static int take_pending(fn_serial_lc_t *lc, fn_serial_lc_read_state_t next)
{
    if (lc->read_state != FN_SERIAL_LC_READ_PENDING) return 0;
    lc->read_state = next;
    return 1;
}

static void reply_read(fn_serial_lc_t *lc, int error, unsigned actual)
{
    lc->read_error = error;
    lc->read_actual = actual;
    lc->read_need = 0;
    lc->reply_count += 1;
    lc->read_state = FN_SERIAL_LC_READ_REPLIED;
}

static void ack_rbf_once(fn_serial_lc_t *lc)
{
    if (!lc->last_op_was_sample) lc->ack_before_sample = 1;
    lc->ack_count += 1;
    lc->last_op_was_sample = 0;
    if (lc->paula_len > 0U) {
        memmove(&lc->paula_bytes[0], &lc->paula_bytes[1],
                (lc->paula_len - 1U) * sizeof(lc->paula_bytes[0]));
        lc->paula_len -= 1U;
    }
    lc->rbf_intreq = lc->paula_len > 0U;
}

static void sample_retain_ack(fn_serial_lc_t *lc)
{
    uint16_t serdatr = 0;

    if (lc->paula_len > 0U) serdatr = lc->paula_bytes[0];
    lc->sample_count += 1;
    lc->last_op_was_sample = 1;
    fujinet_paula_rx_ingest(&lc->rx, serdatr);
    ack_rbf_once(lc);
}

static void drain_rbf(fn_serial_lc_t *lc)
{
    if (!lc->inten_master) return;
    while (lc->rbf_intreq) sample_retain_ack(lc);
}

static int read_satisfied(const fn_serial_lc_t *lc)
{
    return lc->read_state == FN_SERIAL_LC_READ_PENDING &&
           lc->read_need > 0U &&
           fujinet_paula_rx_count(&lc->rx) >= lc->read_need;
}

static void rearm_receive(fn_serial_lc_t *lc)
{
    if (lc->receive_armed) return;
    drain_rbf(lc);
    lc->rbf_intena = 1;
    lc->receive_armed = 1;
}

static void quiesce_receive(fn_serial_lc_t *lc)
{
    lc->rbf_intena = 0;
    lc->receive_armed = 0;
}

static void release_resources(fn_serial_lc_t *lc)
{
    if (lc->bits_owned) {
        lc->bits_owned = 0;
        trace_add(lc->release_trace, &lc->release_trace_n, FN_SERIAL_LC_MISC_BITS);
    }
    if (lc->port_owned) {
        lc->port_owned = 0;
        trace_add(lc->release_trace, &lc->release_trace_n, FN_SERIAL_LC_MISC_PORT);
    }
}

static void teardown_paula(fn_serial_lc_t *lc)
{
    lc->closing = 1;
    if (take_pending(lc, FN_SERIAL_LC_READ_ABORTING))
        reply_read(lc, FN_SERIAL_LC_ABORTED, 0);
    quiesce_receive(lc);
    drain_rbf(lc);
    if (lc->rbf_vector == lc->fujinet_handler && lc->vector_installed) {
        lc->rbf_vector = lc->saved_vector;
        lc->rbf_intena = lc->rbf_was_enabled;
        lc->vector_installed = 0;
    }
    release_resources(lc);
    lc->paula_mutated = 0;
}

void fn_serial_lc_init(fn_serial_lc_t *lc)
{
    memset(lc, 0, sizeof(*lc));
    lc->inten_master = 1;
    lc->rbf_vector = k_kickstart_handler;
    lc->fujinet_handler = k_fujinet_handler;
    lc->saved_vector = k_kickstart_handler;
    (void)fujinet_paula_rx_init(&lc->rx, lc->ring_buf, FN_SERIAL_LC_RING_SIZE);
}

int fn_serial_lc_open_unit(fn_serial_lc_t *lc, unsigned unit)
{
    if (unit != 0U) return FN_SERIAL_LC_OPENFAIL;
    return fn_serial_lc_open(lc);
}

int fn_serial_lc_setparams(fn_serial_lc_t *lc, uint32_t baud, unsigned read_len,
                           unsigned write_len, unsigned stop_bits,
                           int parity_on)
{
    if (lc->open_cnt == 0U) return FN_SERIAL_LC_OPENFAIL;
    if (lc->read_state == FN_SERIAL_LC_READ_PENDING) return FN_SERIAL_LC_NOCMD;
    if (baud < FUJINET_SERIAL_BAUD_MIN || baud > FUJINET_SERIAL_BAUD_MAX)
        return FN_SERIAL_LC_NOCMD;
    if (!fujinet_serial_params_valid(baud, read_len, write_len, stop_bits,
                                     parity_on))
        return FN_SERIAL_LC_NOCMD;
    lc->last_serper = fujinet_paula_serper(baud, 1);
    lc->serper_writes += 1;
    lc->paula_mutated = 1;
    return FN_SERIAL_LC_OK;
}

int fn_serial_lc_open(fn_serial_lc_t *lc)
{
    if (lc->open_cnt != 0U) return FN_SERIAL_LC_BUSY;

    lc->claim_trace_n = 0;
    lc->release_trace_n = 0;
    lc->paula_mutated = 0;

    if (lc->port_busy_external) return FN_SERIAL_LC_OPENFAIL;
    lc->port_owned = 1;
    trace_add(lc->claim_trace, &lc->claim_trace_n, FN_SERIAL_LC_MISC_PORT);

    if (lc->bits_busy_external) {
        lc->port_owned = 0;
        trace_add(lc->release_trace, &lc->release_trace_n, FN_SERIAL_LC_MISC_PORT);
        return FN_SERIAL_LC_OPENFAIL;
    }
    lc->bits_owned = 1;
    trace_add(lc->claim_trace, &lc->claim_trace_n, FN_SERIAL_LC_MISC_BITS);

    lc->saved_vector = lc->rbf_vector;
    lc->rbf_was_enabled = lc->rbf_intena;
    lc->last_serper = 183U;
    lc->serper_writes += 1;
    lc->paula_mutated = 1;
    lc->rbf_vector = lc->fujinet_handler;
    lc->vector_installed = 1;
    lc->rbf_intena = 1;
    lc->receive_armed = 1;
    lc->closing = 0;
    lc->open_cnt = 1U;
    lc->lib_flags &= ~FN_SERIAL_LC_DELEXP;
    (void)fujinet_paula_rx_init(&lc->rx, lc->ring_buf, FN_SERIAL_LC_RING_SIZE);
    return FN_SERIAL_LC_OK;
}

int fn_serial_lc_close(fn_serial_lc_t *lc)
{
    if (lc->open_cnt != 0U) lc->open_cnt -= 1U;
    if (lc->open_cnt == 0U) teardown_paula(lc);
    if (lc->open_cnt == 0U && (lc->lib_flags & FN_SERIAL_LC_DELEXP) != 0U)
        return fn_serial_lc_expunge(lc);
    return FN_SERIAL_LC_OK;
}

int fn_serial_lc_expunge(fn_serial_lc_t *lc)
{
    if (lc->open_cnt != 0U) {
        lc->lib_flags |= FN_SERIAL_LC_DELEXP;
        return FN_SERIAL_LC_OK;
    }
    if (lc->port_owned || lc->bits_owned) teardown_paula(lc);
    lc->lib_flags &= ~FN_SERIAL_LC_DELEXP;
    lc->expunged = 1;
    return FN_SERIAL_LC_OK;
}

void fn_serial_lc_hw_rx(fn_serial_lc_t *lc, uint8_t byte, int hardware_overrun)
{
    uint16_t serdatr;

    if (lc->paula_len >= FN_SERIAL_LC_PAULA_DEPTH) return;
    serdatr = (uint16_t)(FUJINET_PAULA_SERDATR_RBF | (uint16_t)byte);
    if (hardware_overrun) serdatr |= FUJINET_PAULA_SERDATR_OVRUN;
    lc->paula_bytes[lc->paula_len++] = serdatr;
    lc->rbf_intreq = 1;
}

void fn_serial_lc_rbf_handler(fn_serial_lc_t *lc)
{
    if (!lc->inten_master) return;
    while (lc->rbf_intreq) sample_retain_ack(lc);
    if (read_satisfied(lc)) {
        lc->softint_pending = 1;
        lc->cause_count += 1;
    }
}

int fn_serial_lc_softint(fn_serial_lc_t *lc)
{
    unsigned need;
    unsigned copied;

    lc->softint_pending = 0;
    if (lc->read_state != FN_SERIAL_LC_READ_PENDING) return 0;
    need = lc->read_need;
    if (!take_pending(lc, FN_SERIAL_LC_READ_COMPLETING)) return 0;
    copied = fujinet_paula_rx_read(&lc->rx, lc->read_dst, (uint16_t)need);
    reply_read(lc, FN_SERIAL_LC_OK, copied);
    return 1;
}

int fn_serial_lc_read(fn_serial_lc_t *lc, unsigned need)
{
    if (lc->closing || lc->open_cnt == 0U) return FN_SERIAL_LC_OPENFAIL;
    if (need == 0U) {
        reply_read(lc, FN_SERIAL_LC_OK, 0);
        return FN_SERIAL_LC_OK;
    }
    if (need > lc->rx.mask) return FN_SERIAL_LC_NOCMD;
    if (lc->read_state == FN_SERIAL_LC_READ_PENDING) return FN_SERIAL_LC_BUSY;
    rearm_receive(lc);
    lc->read_state = FN_SERIAL_LC_READ_NEW;
    lc->read_need = need;
    lc->read_actual = 0;
    lc->read_error = FN_SERIAL_LC_OK;
    memset(lc->read_dst, 0, sizeof(lc->read_dst));
    lc->read_state = FN_SERIAL_LC_READ_PENDING;
    if (read_satisfied(lc)) {
        lc->softint_pending = 1;
        lc->cause_count += 1;
    }
    return FN_SERIAL_LC_OK;
}

int fn_serial_lc_abort(fn_serial_lc_t *lc)
{
    if (!take_pending(lc, FN_SERIAL_LC_READ_ABORTING)) return 0;
    reply_read(lc, FN_SERIAL_LC_ABORTED, 0);
    return 1;
}

int fn_serial_lc_clear(fn_serial_lc_t *lc)
{
    int aborted = take_pending(lc, FN_SERIAL_LC_READ_ABORTING);

    if (aborted) reply_read(lc, FN_SERIAL_LC_ABORTED, 0);
    drain_rbf(lc);
    fujinet_paula_rx_clear(&lc->rx);
    return aborted ? FN_SERIAL_LC_ABORTED : FN_SERIAL_LC_OK;
}

int fn_serial_lc_flush(fn_serial_lc_t *lc)
{
    int aborted = take_pending(lc, FN_SERIAL_LC_READ_ABORTING);

    if (aborted) reply_read(lc, FN_SERIAL_LC_ABORTED, 0);
    quiesce_receive(lc);
    drain_rbf(lc);
    fujinet_paula_rx_clear(&lc->rx);
    return aborted ? FN_SERIAL_LC_ABORTED : FN_SERIAL_LC_OK;
}

int fn_serial_lc_query(fn_serial_lc_t *lc, unsigned *count, int *overrun)
{
    rearm_receive(lc);
    if (count != NULL) *count = fujinet_paula_rx_count(&lc->rx);
    if (overrun != NULL) *overrun = fn_serial_lc_public_overrun(lc);
    return FN_SERIAL_LC_OK;
}

int fn_serial_lc_write_byte(fn_serial_lc_t *lc, uint8_t byte)
{
    int armed_before;

    (void)byte;
    if (lc->open_cnt == 0U) return FN_SERIAL_LC_OPENFAIL;
    armed_before = lc->receive_armed;
    rearm_receive(lc);
    if (!armed_before && !lc->receive_armed) lc->tx_while_masked = 1;
    if (!lc->receive_armed) lc->tx_while_masked = 1;
    lc->tx_count += 1;
    return FN_SERIAL_LC_OK;
}

int fn_serial_lc_public_overrun(const fn_serial_lc_t *lc)
{
    return fujinet_paula_rx_public_overrun(&lc->rx) != 0;
}
