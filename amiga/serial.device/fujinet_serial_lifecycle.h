#ifndef FUJINET_SERIAL_LIFECYCLE_H
#define FUJINET_SERIAL_LIFECYCLE_H

#include "fujinet_paula_uart.h"

#include <stdint.h>

#define FN_SERIAL_LC_MISC_PORT 0
#define FN_SERIAL_LC_MISC_BITS 1
#define FN_SERIAL_LC_OK 0
#define FN_SERIAL_LC_OPENFAIL 1
#define FN_SERIAL_LC_BUSY 2
#define FN_SERIAL_LC_ABORTED 3
#define FN_SERIAL_LC_NOCMD 4

#define FN_SERIAL_LC_DELEXP 1U

#define FN_SERIAL_LC_RING_SIZE 8U
#define FN_SERIAL_LC_PAULA_DEPTH 8U
#define FN_SERIAL_LC_TRACE 8U

typedef enum {
    FN_SERIAL_LC_READ_IDLE = 0,
    FN_SERIAL_LC_READ_NEW,
    FN_SERIAL_LC_READ_PENDING,
    FN_SERIAL_LC_READ_COMPLETING,
    FN_SERIAL_LC_READ_ABORTING,
    FN_SERIAL_LC_READ_REPLIED
} fn_serial_lc_read_state_t;

typedef enum {
    FN_SERIAL_LC_WRITE_IDLE = 0,
    FN_SERIAL_LC_WRITE_PENDING,
    FN_SERIAL_LC_WRITE_COMPLETING,
    FN_SERIAL_LC_WRITE_ABORTING,
    FN_SERIAL_LC_WRITE_REPLIED
} fn_serial_lc_write_state_t;

#define FN_SERIAL_LC_WRITE_OWNER_NONE 0
#define FN_SERIAL_LC_WRITE_OWNER_TBE 1
#define FN_SERIAL_LC_WRITE_OWNER_ABORT 2
#define FN_SERIAL_LC_WRITE_OWNER_FLUSH 3
#define FN_SERIAL_LC_WRITE_OWNER_CLOSE 4

typedef struct fn_serial_lc {
    int port_busy_external;
    int bits_busy_external;
    int port_owned;
    int bits_owned;

    int inten_master;
    int rbf_intena;
    int rbf_intreq;
    int tbe_intena;
    int tbe_intreq;
    uint16_t last_serper;
    int serper_writes;
    int cia_b_writes;
    int paula_mutated;

    uint16_t paula_bytes[FN_SERIAL_LC_PAULA_DEPTH];
    unsigned paula_len;

    void *rbf_vector;
    void *saved_vector;
    void *fujinet_handler;
    int rbf_was_enabled;
    int vector_installed;

    uint8_t ring_buf[FN_SERIAL_LC_RING_SIZE];
    fujinet_paula_rx_t rx;

    int receive_armed;
    int closing;
    unsigned open_cnt;
    unsigned lib_flags;
    int expunged;

    fn_serial_lc_read_state_t read_state;
    unsigned read_need;
    uint8_t read_dst[FN_SERIAL_LC_RING_SIZE];
    unsigned read_actual;
    int read_error;
    int reply_count;
    int iorequest_mutated_by_rbf;
    int rbf_replied;

    int softint_pending;
    int cause_count;
    int sample_count;
    int ack_count;
    int last_op_was_sample;
    int ack_before_sample;
    int duplicate_ack;
    int tx_count;
    int tx_while_masked;
    int remdevice_called;

    fn_serial_lc_write_state_t write_state;
    int pending_write;
    const uint8_t *write_buf;
    unsigned write_remaining;
    unsigned write_committed;
    unsigned write_length;
    int write_accepted;
    int write_softint_pending;
    unsigned write_actual;
    int write_error;
    int last_write_owner;
    unsigned last_write_length;
    unsigned last_write_committed;
    unsigned last_write_actual;
    int last_write_error;
    unsigned tbe_fire;
    unsigned last_tbe_fire;
    int write_resolved_before_vector;
    int write_pending_at_vector_restore;

    int claim_trace[FN_SERIAL_LC_TRACE];
    unsigned claim_trace_n;
    int release_trace[FN_SERIAL_LC_TRACE];
    unsigned release_trace_n;
} fn_serial_lc_t;

void fn_serial_lc_init(fn_serial_lc_t *lc);
int fn_serial_lc_open(fn_serial_lc_t *lc);
int fn_serial_lc_open_at_baud(fn_serial_lc_t *lc, uint32_t baud);
int fn_serial_lc_open_unit(fn_serial_lc_t *lc, unsigned unit);
int fn_serial_lc_setparams(fn_serial_lc_t *lc, uint32_t baud, unsigned read_len,
                           unsigned write_len, unsigned stop_bits,
                           int parity_on);
int fn_serial_lc_close(fn_serial_lc_t *lc);
int fn_serial_lc_expunge(fn_serial_lc_t *lc);

void fn_serial_lc_hw_rx(fn_serial_lc_t *lc, uint8_t byte, int hardware_overrun);
void fn_serial_lc_rbf_handler(fn_serial_lc_t *lc);
int fn_serial_lc_softint(fn_serial_lc_t *lc);

int fn_serial_lc_read(fn_serial_lc_t *lc, unsigned need);
int fn_serial_lc_abort(fn_serial_lc_t *lc);
int fn_serial_lc_clear(fn_serial_lc_t *lc);
int fn_serial_lc_flush(fn_serial_lc_t *lc);
int fn_serial_lc_query(fn_serial_lc_t *lc, unsigned *count, int *overrun);
int fn_serial_lc_write(fn_serial_lc_t *lc, const uint8_t *data, unsigned length);
void fn_serial_lc_tbe_handler(fn_serial_lc_t *lc);
int fn_serial_lc_write_softint(fn_serial_lc_t *lc);
int fn_serial_lc_abort_write(fn_serial_lc_t *lc);
int fn_serial_lc_write_byte(fn_serial_lc_t *lc, uint8_t byte);

int fn_serial_lc_public_overrun(const fn_serial_lc_t *lc);

#endif
