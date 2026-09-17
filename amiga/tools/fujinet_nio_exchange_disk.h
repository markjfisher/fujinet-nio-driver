#ifndef FUJINET_NIO_EXCHANGE_DISK_H
#define FUJINET_NIO_EXCHANGE_DISK_H
#include "fujinet_nio_exchange_opts.h"
/* Portable decision sequence; adapters perform one synchronous operation. */
enum fn_exchange_disk_op {
    FN_EXDISK_LOCAL_STATE, FN_EXDISK_REMOTE_INFO, FN_EXDISK_MOUNT,
    FN_EXDISK_GEOMETRY, FN_EXDISK_CHANGE, FN_EXDISK_READ,
    FN_EXDISK_WRITE, FN_EXDISK_FLUSH
};
struct fn_exchange_disk_state {
    unsigned trial; /* one based during transfers, zero during setup */
    unsigned mounted;
    unsigned sector_size;
    uint32_t sectors;
    uint32_t change;
    uint32_t actual;
};
typedef int (*fn_exchange_disk_io)(void *, enum fn_exchange_disk_op,
                                  struct fn_exchange_disk_state *, uint8_t *);
struct fn_exchange_disk_result {
    int mount_attempted; /* failure may have unknown remote effects */
    int fixture_mounted;
    const char *failure;
    unsigned completed_trials;
};
void fn_exchange_disk_pattern(uint8_t *buffer, unsigned trial);
int fn_exchange_disk_run(const struct fn_nio_exchange_opts *opts,
                         fn_exchange_disk_io io, void *context,
                         uint8_t *write_buffer, uint8_t *read_buffer,
                         struct fn_exchange_disk_result *result);
#endif
