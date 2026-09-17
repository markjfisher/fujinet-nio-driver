#include "fujinet_nio_exchange_disk.h"
#include <string.h>

void fn_exchange_disk_pattern(uint8_t *buffer, unsigned trial)
{
    unsigned i;
    /* Trial is zero based, encoded in full so trials 1 and 257 differ. */
    for (i = 0; i < 512; ++i)
        buffer[i] = (uint8_t)((i ^ 0x5aU) ^ (trial >> ((i % 4) * 8)));
}

int fn_exchange_disk_run(const struct fn_nio_exchange_opts *opts,
                         fn_exchange_disk_io io, void *context,
                         uint8_t *write_buffer, uint8_t *read_buffer,
                         struct fn_exchange_disk_result *result)
{
    struct fn_exchange_disk_state state;
    unsigned trial;
    int steps[2];
    memset(result, 0, sizeof(*result));
    memset(&state, 0, sizeof(state));
    result->failure = "authorization";
    if (opts->provocation || fn_nio_exchange_opts_plan(opts, steps, 2) != 1 ||
        (opts->type != FN_NIO_EXCHANGE_TYPE_DISK_READ &&
         opts->type != FN_NIO_EXCHANGE_TYPE_DISK_WRITE)) return -1;
#define OP(code, text, buffer) do { result->failure = text; \
    if (io(context, code, &state, buffer) != 0) return -1; } while (0)
    OP(FN_EXDISK_LOCAL_STATE, "local-state", 0);
    result->failure = "local-slot-occupied";
    if (state.mounted) return -1;
    OP(FN_EXDISK_REMOTE_INFO, "remote-info", 0);
    result->failure = "remote-slot-occupied";
    if (state.mounted) return -1;
    result->mount_attempted = 1;
    OP(FN_EXDISK_MOUNT, "mount", 0);
    result->fixture_mounted = 1;
    OP(FN_EXDISK_GEOMETRY, "geometry", 0);
    result->failure = "geometry-bounds";
    if (state.sector_size != 512 || opts->lba >= state.sectors) return -1;
    OP(FN_EXDISK_CHANGE, "change-count", 0);
    for (trial = 0; trial < opts->trials; ++trial) {
        state.trial = trial + 1;
        if (opts->type == FN_NIO_EXCHANGE_TYPE_DISK_WRITE) {
            fn_exchange_disk_pattern(write_buffer, trial);
            OP(FN_EXDISK_WRITE, "write", write_buffer);
            result->failure = "short-write";
            if (state.actual != 512) return -1;
            OP(FN_EXDISK_FLUSH, "flush", 0);
        }
        OP(FN_EXDISK_READ, "read", read_buffer);
        result->failure = "short-read";
        if (state.actual != 512) return -1;
        result->failure = "read-back-mismatch";
        if (opts->type == FN_NIO_EXCHANGE_TYPE_DISK_WRITE &&
            memcmp(write_buffer, read_buffer, 512) != 0) return -1;
        result->completed_trials++;
    }
#undef OP
    result->failure = 0;
    return 0;
}
