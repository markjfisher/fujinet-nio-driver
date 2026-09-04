#ifndef FUJINET_NIO_EXCHANGE_OPTS_H
#define FUJINET_NIO_EXCHANGE_OPTS_H

#include <stdint.h>

enum fn_nio_exchange_type {
    FN_NIO_EXCHANGE_TYPE_CLOCK = 1,
    FN_NIO_EXCHANGE_TYPE_HOST_GET = 2,
    FN_NIO_EXCHANGE_TYPE_FILE_LIST = 3
};

enum fn_nio_exchange_backend {
    FN_NIO_EXCHANGE_BACKEND_COLD = 1,
    FN_NIO_EXCHANGE_BACKEND_WARM = 2
};

enum fn_nio_exchange_step {
    FN_NIO_EXCHANGE_STEP_GET_BAUD = 1,
    FN_NIO_EXCHANGE_STEP_SET_BAUD = 2,
    FN_NIO_EXCHANGE_STEP_WARMUP = 3,
    FN_NIO_EXCHANGE_STEP_MEASURE = 4
};

struct fn_nio_exchange_opts {
    int type;
    int backend;
    unsigned long baud; /* 0 if --baud omitted */
    int has_size;
    unsigned size;
    const char *uri;
    unsigned trials;
};

int fn_nio_exchange_opts_parse(int argc, char **argv,
                               struct fn_nio_exchange_opts *out);
int fn_nio_exchange_opts_plan(const struct fn_nio_exchange_opts *opts,
                              int *steps, int max_steps);
int fn_nio_exchange_warm_baud_ok(unsigned long want, unsigned long got);
int fn_nio_exchange_step_failure_aborts(int step);
int fn_nio_exchange_format_elapsed(int timer_ok, unsigned long us,
                                   char *buf, unsigned cap);
int fn_nio_exchange_format_trial_log(char *buf, unsigned cap,
                                     unsigned req_len, unsigned resp_len,
                                     const char *elapsed, unsigned result,
                                     unsigned cause, unsigned native,
                                     unsigned status, int backend);
int fn_nio_exchange_build_clock_get(uint8_t *buf, unsigned cap);
int fn_nio_exchange_build_host_get(uint8_t *buf, unsigned cap);
int fn_nio_exchange_build_file_list(uint8_t *buf, unsigned cap,
                                    const char *uri, unsigned max_payload_bytes);

#endif
