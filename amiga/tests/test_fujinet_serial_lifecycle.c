#include "fujinet_serial_lifecycle.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned failures;

#define CHECK(name, expression) do {                                      \
    if (!(expression)) {                                                  \
        fprintf(stderr, "FAIL: %s (line %d)\n", name, __LINE__);          \
        ++failures;                                                       \
    }                                                                     \
} while (0)

static char *read_file(const char *path, size_t *out_len)
{
    FILE *fp;
    long size;
    char *buf;

    fp = fopen(path, "rb");
    if (fp == NULL) return NULL;
    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return NULL;
    }
    size = ftell(fp);
    if (size < 0) {
        fclose(fp);
        return NULL;
    }
    if (fseek(fp, 0, SEEK_SET) != 0) {
        fclose(fp);
        return NULL;
    }
    buf = malloc((size_t)size + 1U);
    if (buf == NULL) {
        fclose(fp);
        return NULL;
    }
    if (fread(buf, 1, (size_t)size, fp) != (size_t)size) {
        free(buf);
        fclose(fp);
        return NULL;
    }
    fclose(fp);
    buf[size] = '\0';
    if (out_len != NULL) *out_len = (size_t)size;
    return buf;
}

static char *rbf_handler_body(char *src)
{
    char *start;
    char *end;

    start = strstr(src, "_fujinet_serial_rbf_server:");
    if (start == NULL) return NULL;
    end = strstr(start + 1, "_fujinet_serial_softint");
    if (end == NULL) end = start + strlen(start);
    *end = '\0';
    return start;
}

static int token_present(const char *body, const char *token)
{
    const char *p = body;
    size_t n = strlen(token);

    while ((p = strstr(p, token)) != NULL) {
        char before = p == body ? '\n' : p[-1];
        char after = p[n];
        if (!isalnum((unsigned char)before) && before != '_' &&
            !isalnum((unsigned char)after) && after != '_')
            return 1;
        p += n;
    }
    return 0;
}

static void test_rbf_handler_register_contract(void)
{
    char *src;
    char *body;

    src = read_file("../serial.device/fujinet_serial_rbf.S", NULL);
    if (src == NULL)
        src = read_file("serial.device/fujinet_serial_rbf.S", NULL);
    if (src == NULL)
        src = read_file("fujinet_serial_rbf.S", NULL);
    CHECK("rbf-asm-readable", src != NULL);
    if (src == NULL) return;
    body = rbf_handler_body(src);
    CHECK("rbf-asm-label", body != NULL);
    if (body == NULL) {
        free(src);
        return;
    }
    CHECK("rbf-rts-not-rte", strstr(body, "rts") != NULL &&
          strstr(body, "rte") == NULL && strstr(body, "RTE") == NULL);
    CHECK("rbf-no-movem", strstr(body, "movem") == NULL);
    CHECK("rbf-no-d2", !token_present(body, "%d2"));
    CHECK("rbf-no-d3", !token_present(body, "%d3"));
    CHECK("rbf-no-d4", !token_present(body, "%d4"));
    CHECK("rbf-no-d5", !token_present(body, "%d5"));
    CHECK("rbf-no-d6", !token_present(body, "%d6"));
    CHECK("rbf-no-d7", !token_present(body, "%d7"));
    CHECK("rbf-no-a2", !token_present(body, "%a2"));
    CHECK("rbf-no-a3", !token_present(body, "%a3"));
    CHECK("rbf-no-a4", !token_present(body, "%a4"));
    CHECK("rbf-no-a5", !token_present(body, "%a5"));
    CHECK("rbf-inten-check", strstr(body, "INTENAR") != NULL);
    CHECK("rbf-sample-serdatr", strstr(body, "SERDATR") != NULL);
    CHECK("rbf-cause", strstr(body, "_LVOCause") != NULL);
    CHECK("rbf-no-a6", !token_present(body, "%a6") &&
          strstr(body, "_LVOCause(%a6)") == NULL);
    CHECK("rbf-cause-via-a0", strstr(body, "_LVOCause(%a0)") != NULL);
    CHECK("rbf-single-ack-pattern",
          strstr(body, "move.w  #INTF_RBF,INTREQ\n        move.w  #INTF_RBF,INTREQ") == NULL);
    CHECK("rbf-no-nop-rts", strstr(body, "nop") == NULL);
    CHECK("rbf-no-replymsg", strstr(body, "ReplyMsg") == NULL &&
          strstr(body, "_LVOReplyMsg") == NULL);
    free(src);
}

static void test_misc_resource(void)
{
    fn_serial_lc_t lc;
    void *kick;

    fn_serial_lc_init(&lc);
    CHECK("unit-nonzero-fresh", fn_serial_lc_open_unit(&lc, 1U) == FN_SERIAL_LC_OPENFAIL);
    CHECK("unit-nonzero-no-claim", lc.port_owned == 0 && lc.paula_mutated == 0);

    fn_serial_lc_init(&lc);
    kick = lc.rbf_vector;
    lc.port_busy_external = 1;
    CHECK("ownership-conflict-open", fn_serial_lc_open(&lc) == FN_SERIAL_LC_OPENFAIL);
    CHECK("ownership-conflict-no-port", lc.port_owned == 0 && lc.bits_owned == 0);
    CHECK("ownership-conflict-no-paula", lc.paula_mutated == 0);
    CHECK("ownership-conflict-vector", lc.rbf_vector == kick);
    CHECK("ownership-conflict-no-remdevice", lc.remdevice_called == 0);

    fn_serial_lc_init(&lc);
    CHECK("claim-open", fn_serial_lc_open(&lc) == FN_SERIAL_LC_OK);
    CHECK("claim-order-n", lc.claim_trace_n == 2U);
    CHECK("claim-port-then-bits",
          lc.claim_trace[0] == FN_SERIAL_LC_MISC_PORT &&
          lc.claim_trace[1] == FN_SERIAL_LC_MISC_BITS);
    CHECK("claim-both-owned", lc.port_owned == 1 && lc.bits_owned == 1);
    CHECK("no-cia-writes", lc.cia_b_writes == 0);
    CHECK("unit-nonzero-rejected",
          fn_serial_lc_open_unit(&lc, 1U) == FN_SERIAL_LC_OPENFAIL);
    CHECK("setparams-8n1",
          fn_serial_lc_setparams(&lc, 19200UL, 8U, 8U, 1U, 0) == FN_SERIAL_LC_OK);
    CHECK("setparams-baud-low",
          fn_serial_lc_setparams(&lc, 299UL, 8U, 8U, 1U, 0) != FN_SERIAL_LC_OK);
    CHECK("setparams-baud-high",
          fn_serial_lc_setparams(&lc, 230401UL, 8U, 8U, 1U, 0) != FN_SERIAL_LC_OK);
    CHECK("setparams-parity",
          fn_serial_lc_setparams(&lc, 9600UL, 8U, 8U, 1U, 1) != FN_SERIAL_LC_OK);
    CHECK("setparams-wordlen",
          fn_serial_lc_setparams(&lc, 9600UL, 7U, 8U, 1U, 0) != FN_SERIAL_LC_OK);
    CHECK("second-open-rejected", fn_serial_lc_open(&lc) == FN_SERIAL_LC_BUSY);
    CHECK("second-open-unchanged", lc.open_cnt == 1U);
    CHECK("stock-blocked-by-misc", lc.port_owned == 1 && lc.bits_owned == 1);

    CHECK("close-ok", fn_serial_lc_close(&lc) == FN_SERIAL_LC_OK);
    CHECK("release-order-n", lc.release_trace_n == 2U);
    CHECK("release-bits-then-port",
          lc.release_trace[0] == FN_SERIAL_LC_MISC_BITS &&
          lc.release_trace[1] == FN_SERIAL_LC_MISC_PORT);
    CHECK("released", lc.port_owned == 0 && lc.bits_owned == 0);

    fn_serial_lc_init(&lc);
    lc.bits_busy_external = 1;
    CHECK("partial-open-fail", fn_serial_lc_open(&lc) == FN_SERIAL_LC_OPENFAIL);
    CHECK("partial-open-port-freed", lc.port_owned == 0 && lc.bits_owned == 0);
    CHECK("partial-open-release-port-only",
          lc.release_trace_n == 1U &&
          lc.release_trace[0] == FN_SERIAL_LC_MISC_PORT);
    CHECK("partial-open-no-paula", lc.paula_mutated == 0);
    CHECK("partial-open-claim-port-only",
          lc.claim_trace_n == 1U &&
          lc.claim_trace[0] == FN_SERIAL_LC_MISC_PORT);
}

static void test_rbf_drain(void)
{
    fn_serial_lc_t lc;

    fn_serial_lc_init(&lc);
    CHECK("open-drain", fn_serial_lc_open(&lc) == FN_SERIAL_LC_OK);
    fn_serial_lc_hw_rx(&lc, 0x11, 0);
    fn_serial_lc_hw_rx(&lc, 0x22, 0);
    fn_serial_lc_hw_rx(&lc, 0x33, 0);
    fn_serial_lc_rbf_handler(&lc);
    CHECK("drain-count", fujinet_paula_rx_count(&lc.rx) == 3);
    CHECK("drain-rbf-clear", lc.rbf_intreq == 0);
    CHECK("one-ack-per-byte", lc.sample_count == 3 && lc.ack_count == 3);
    CHECK("no-ack-before-sample", lc.ack_before_sample == 0);
    CHECK("no-duplicate-ack", lc.duplicate_ack == 0);
    CHECK("handler-no-reply", lc.rbf_replied == 0 && lc.reply_count == 0);
    CHECK("handler-no-iorequest", lc.iorequest_mutated_by_rbf == 0);
    CHECK("handler-no-read-state",
          lc.read_state == FN_SERIAL_LC_READ_IDLE);

    lc.inten_master = 0;
    fn_serial_lc_hw_rx(&lc, 0x44, 0);
    {
        int samples = lc.sample_count;
        int acks = lc.ack_count;
        fn_serial_lc_rbf_handler(&lc);
        CHECK("inten-clear-no-ack",
              lc.sample_count == samples && lc.ack_count == acks &&
              lc.rbf_intreq == 1);
    }
}

static void test_read_cause_and_abort(void)
{
    fn_serial_lc_t lc;
    int abort_won;
    int soft_won;

    fn_serial_lc_init(&lc);
    CHECK("open-read", fn_serial_lc_open(&lc) == FN_SERIAL_LC_OK);
    CHECK("blocking-read-pend", fn_serial_lc_read(&lc, 2) == FN_SERIAL_LC_OK);
    CHECK("blocking-pending", lc.read_state == FN_SERIAL_LC_READ_PENDING);
    CHECK("blocking-no-reply-yet", lc.reply_count == 0);
    fn_serial_lc_hw_rx(&lc, 0xAA, 0);
    fn_serial_lc_rbf_handler(&lc);
    CHECK("not-yet-satisfied", lc.reply_count == 0 && lc.softint_pending == 0);
    fn_serial_lc_hw_rx(&lc, 0xBB, 0);
    fn_serial_lc_rbf_handler(&lc);
    CHECK("cause-deferred", lc.softint_pending == 1 && lc.cause_count >= 1);
    CHECK("rbf-still-not-owner",
          lc.read_state == FN_SERIAL_LC_READ_PENDING && lc.reply_count == 0);
    CHECK("softint-completes", fn_serial_lc_softint(&lc) == 1);
    CHECK("read-replied-once", lc.reply_count == 1 &&
          lc.read_state == FN_SERIAL_LC_READ_REPLIED);
    CHECK("read-bytes", lc.read_actual == 2 && lc.read_dst[0] == 0xAA &&
          lc.read_dst[1] == 0xBB);

    fn_serial_lc_init(&lc);
    CHECK("open-abort", fn_serial_lc_open(&lc) == FN_SERIAL_LC_OK);
    CHECK("abort-read", fn_serial_lc_read(&lc, 1) == FN_SERIAL_LC_OK);
    CHECK("abort-before-data", fn_serial_lc_abort(&lc) == 1);
    CHECK("abort-error", lc.read_error == FN_SERIAL_LC_ABORTED);
    CHECK("abort-one-reply", lc.reply_count == 1);
    CHECK("abort-then-softint-noop", fn_serial_lc_softint(&lc) == 0);
    CHECK("abort-still-one-reply", lc.reply_count == 1);

    fn_serial_lc_init(&lc);
    CHECK("open-race", fn_serial_lc_open(&lc) == FN_SERIAL_LC_OK);
    CHECK("race-read", fn_serial_lc_read(&lc, 1) == FN_SERIAL_LC_OK);
    fn_serial_lc_hw_rx(&lc, 0xCC, 0);
    fn_serial_lc_rbf_handler(&lc);
    CHECK("race-cause", lc.softint_pending == 1);
    abort_won = fn_serial_lc_abort(&lc);
    soft_won = fn_serial_lc_softint(&lc);
    CHECK("race-one-owner", abort_won + soft_won == 1);
    CHECK("race-one-reply", lc.reply_count == 1);
    CHECK("race-terminal", lc.read_state == FN_SERIAL_LC_READ_REPLIED);

    fn_serial_lc_init(&lc);
    CHECK("open-race-soft", fn_serial_lc_open(&lc) == FN_SERIAL_LC_OK);
    CHECK("race-soft-read", fn_serial_lc_read(&lc, 1) == FN_SERIAL_LC_OK);
    fn_serial_lc_hw_rx(&lc, 0xDD, 0);
    fn_serial_lc_rbf_handler(&lc);
    soft_won = fn_serial_lc_softint(&lc);
    abort_won = fn_serial_lc_abort(&lc);
    CHECK("race-soft-wins", soft_won == 1 && abort_won == 0);
    CHECK("race-soft-one-reply", lc.reply_count == 1 &&
          lc.read_error == FN_SERIAL_LC_OK);

    fn_serial_lc_init(&lc);
    CHECK("open-second-read", fn_serial_lc_open(&lc) == FN_SERIAL_LC_OK);
    CHECK("first-pending", fn_serial_lc_read(&lc, 1) == FN_SERIAL_LC_OK);
    CHECK("second-read-busy", fn_serial_lc_read(&lc, 1) == FN_SERIAL_LC_BUSY);
    CHECK("first-still-pending", lc.read_state == FN_SERIAL_LC_READ_PENDING &&
          lc.reply_count == 0);
    CHECK("read-too-big", fn_serial_lc_read(&lc, 8) == FN_SERIAL_LC_NOCMD);
    CHECK("setparams-while-pending",
          fn_serial_lc_setparams(&lc, 9600UL, 8U, 8U, 1U, 0) != FN_SERIAL_LC_OK);
    CHECK("clear-aborts-pending", fn_serial_lc_clear(&lc) == FN_SERIAL_LC_ABORTED);
    CHECK("clear-one-reply", lc.reply_count == 1 &&
          lc.read_error == FN_SERIAL_LC_ABORTED);
    CHECK("clear-keeps-armed", lc.receive_armed == 1);
}

static void test_flush_rearm_and_close(void)
{
    fn_serial_lc_t lc;
    unsigned count = 99;
    int overrun = 0;
    int serper_at_close;

    fn_serial_lc_init(&lc);
    CHECK("open-flush", fn_serial_lc_open(&lc) == FN_SERIAL_LC_OK);
    CHECK("flush-read", fn_serial_lc_read(&lc, 2) == FN_SERIAL_LC_OK);
    fn_serial_lc_hw_rx(&lc, 0x01, 0);
    fn_serial_lc_rbf_handler(&lc);
    CHECK("flush-pending", fn_serial_lc_flush(&lc) == FN_SERIAL_LC_ABORTED);
    CHECK("flush-read-aborted", lc.read_error == FN_SERIAL_LC_ABORTED);
    CHECK("flush-one-reply", lc.reply_count == 1);
    CHECK("flush-queue-cleared", fujinet_paula_rx_count(&lc.rx) == 0);
    CHECK("flush-stays-armed", lc.receive_armed == 1 && lc.rbf_intena == 1);
    CHECK("flush-keeps-paula", lc.port_owned == 1 && lc.bits_owned == 1);
    CHECK("flush-keeps-vector", lc.rbf_vector == lc.fujinet_handler);

    fn_serial_lc_hw_rx(&lc, 0x5A, 0);
    CHECK("idle-byte-pending", lc.rbf_intreq == 1);
    CHECK("write-keeps-armed", fn_serial_lc_write_byte(&lc, 0x42) == FN_SERIAL_LC_OK);
    CHECK("write-not-masked", lc.receive_armed == 1 && lc.tx_while_masked == 0);
    CHECK("write-discards-idle",
          fujinet_paula_rx_count(&lc.rx) == 0 && lc.rbf_intreq == 0);
    CHECK("query-count", fn_serial_lc_query(&lc, &count, &overrun) == FN_SERIAL_LC_OK);
    CHECK("query-empty-after-write-clear", count == 0U && overrun == 0);

    fn_serial_lc_flush(&lc);
    CHECK("query-rearms", fn_serial_lc_query(&lc, &count, &overrun) == FN_SERIAL_LC_OK);
    CHECK("query-rearm-armed", lc.receive_armed == 1);

    fn_serial_lc_init(&lc);
    CHECK("open-close-read", fn_serial_lc_open(&lc) == FN_SERIAL_LC_OK);
    CHECK("close-read", fn_serial_lc_read(&lc, 4) == FN_SERIAL_LC_OK);
    serper_at_close = lc.serper_writes;
    CHECK("close-with-pending", fn_serial_lc_close(&lc) == FN_SERIAL_LC_OK);
    CHECK("close-resolved", lc.read_state == FN_SERIAL_LC_READ_REPLIED &&
          lc.read_error == FN_SERIAL_LC_ABORTED);
    CHECK("close-no-pending-need", lc.read_need == 0);
    CHECK("close-no-serper-restore", lc.serper_writes == serper_at_close);
    CHECK("close-vector-restored", lc.rbf_vector == lc.saved_vector);
    CHECK("close-resources-free", lc.port_owned == 0 && lc.bits_owned == 0);

    fn_serial_lc_init(&lc);
    CHECK("open-stolen", fn_serial_lc_open(&lc) == FN_SERIAL_LC_OK);
    lc.rbf_vector = (void *)(uintptr_t)0xDEAD;
    CHECK("close-stolen", fn_serial_lc_close(&lc) == FN_SERIAL_LC_OK);
    CHECK("stolen-vector-untouched", lc.rbf_vector == (void *)(uintptr_t)0xDEAD);
    CHECK("stolen-still-freed", lc.port_owned == 0 && lc.bits_owned == 0);
}

static void test_overrun_and_expunge(void)
{
    fn_serial_lc_t lc;
    unsigned i;
    unsigned count = 0;
    int overrun = 0;

    fn_serial_lc_init(&lc);
    CHECK("open-hw-ov", fn_serial_lc_open(&lc) == FN_SERIAL_LC_OK);
    fn_serial_lc_hw_rx(&lc, 0xAB, 1);
    fn_serial_lc_rbf_handler(&lc);
    CHECK("hw-latch", lc.rx.hardware_overrun_latched == 1);
    CHECK("hw-not-soft", lc.rx.software_ring_overflow_latched == 0);
    CHECK("hw-public", fn_serial_lc_public_overrun(&lc) == 1);

    fn_serial_lc_init(&lc);
    CHECK("open-soft-ov", fn_serial_lc_open(&lc) == FN_SERIAL_LC_OK);
    for (i = 0; i < 7U; ++i)
        fn_serial_lc_hw_rx(&lc, (uint8_t)i, 0);
    fn_serial_lc_rbf_handler(&lc);
    CHECK("ring-almost-full", fujinet_paula_rx_count(&lc.rx) == 7);
    fn_serial_lc_hw_rx(&lc, 0x99, 0);
    fn_serial_lc_rbf_handler(&lc);
    CHECK("soft-latch", lc.rx.software_ring_overflow_latched == 1);
    CHECK("soft-not-hw", lc.rx.hardware_overrun_latched == 0);
    CHECK("soft-public", fn_serial_lc_public_overrun(&lc) == 1);
    CHECK("query-collapses",
          fn_serial_lc_query(&lc, &count, &overrun) == FN_SERIAL_LC_OK &&
          overrun == 1);

    fn_serial_lc_init(&lc);
    CHECK("open-expunge", fn_serial_lc_open(&lc) == FN_SERIAL_LC_OK);
    CHECK("expunge-delayed", fn_serial_lc_expunge(&lc) == FN_SERIAL_LC_OK);
    CHECK("expunge-not-yet", lc.expunged == 0 &&
          (lc.lib_flags & FN_SERIAL_LC_DELEXP) != 0U);
    CHECK("expunge-still-open", lc.open_cnt == 1U && lc.port_owned == 1);
    CHECK("delayed-close", fn_serial_lc_close(&lc) == FN_SERIAL_LC_OK);
    CHECK("one-time-teardown", lc.expunged == 1 && lc.port_owned == 0 &&
          lc.bits_owned == 0);
}

int main(void)
{
    test_rbf_handler_register_contract();
    test_misc_resource();
    test_rbf_drain();
    test_read_cause_and_abort();
    test_flush_rearm_and_close();
    test_overrun_and_expunge();

    if (failures != 0) {
        fprintf(stderr, "%u fujinet_serial_lifecycle checks failed\n", failures);
        return 1;
    }
    return 0;
}
