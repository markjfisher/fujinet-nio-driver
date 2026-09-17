#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "fujinet_nio_exchange_disk.h"
struct fake {
    int fail_op, occupied_local, occupied_remote, mismatch, short_op;
    unsigned writes, reads, flushes, mounts, calls;
    unsigned sectors, sector_size, fail_trial;
    int accepted_mount_failure;
    unsigned ops[64];
    uint8_t sector[512];
};
static int perform(void *context, enum fn_exchange_disk_op op,
                   struct fn_exchange_disk_state *s, uint8_t *data)
{
    struct fake *f = context;
    f->ops[f->calls++] = op;
    if (f->fail_op == (int)op && (!f->fail_trial || s->trial == f->fail_trial)) return -1;
    if (op == FN_EXDISK_MOUNT && f->accepted_mount_failure) {
        ++f->mounts; /* Peer accepted, but its reply was lost. */
        return -1;
    }
    switch (op) {
    case FN_EXDISK_LOCAL_STATE: s->mounted = f->occupied_local; break;
    case FN_EXDISK_REMOTE_INFO: s->mounted = f->occupied_remote; break;
    case FN_EXDISK_MOUNT: ++f->mounts; break;
    case FN_EXDISK_GEOMETRY: s->sector_size = f->sector_size; s->sectors = f->sectors; break;
    case FN_EXDISK_CHANGE: s->change = 7; break;
    case FN_EXDISK_WRITE:
        assert(s->change == 7); ++f->writes; memcpy(f->sector, data, 512); s->actual = 512; break;
    case FN_EXDISK_READ:
        assert(s->change == 7); ++f->reads; memcpy(data, f->sector, 512);
        if (f->mismatch) data[511] ^= 1;
        s->actual = 512; break;
    case FN_EXDISK_FLUSH: ++f->flushes; break;
    }
    if (f->short_op == (int)op) s->actual = 511;
    return 0;
}
static void reset(struct fake *f)
{
    memset(f, 0, sizeof(*f)); f->fail_op = f->short_op = -1; f->sectors = 1760; f->sector_size = 512;
}
int main(void)
{
    char *argv[] = {"tool", "--type", "disk-write", "--backend", "warm",
        "--slot", "8", "--lba", "17", "--fixture-uri", "host:/fixture.adf",
        "--disposable-fixture", "--write-intent", "--trials", "3"};
    struct fn_nio_exchange_opts opts;
    struct fn_exchange_disk_result result;
    struct fake f;
    uint8_t write[512], read[512], first[512];
    int op;
    assert(fn_nio_exchange_opts_parse(15, argv, &opts) == 0);
#define RUN() fn_exchange_disk_run(&opts, perform, &f, write, read, &result)
    reset(&f); assert(RUN() == 0); assert(f.writes == 3 && f.reads == 3 && f.flushes == 3);
    assert(result.fixture_mounted && result.completed_trials == 3);
    assert(f.ops[5] == FN_EXDISK_WRITE && f.ops[6] == FN_EXDISK_FLUSH && f.ops[7] == FN_EXDISK_READ);
    fn_exchange_disk_pattern(first, 0); fn_exchange_disk_pattern(read, 256);
    assert(memcmp(first, read, 512)); assert(first[0] == 0x5a && first[511] == 0xa5);
    for (op = FN_EXDISK_LOCAL_STATE; op <= FN_EXDISK_FLUSH; ++op) {
        reset(&f); f.fail_op = op; assert(RUN() != 0);
        assert(f.writes == (op == FN_EXDISK_READ || op == FN_EXDISK_FLUSH ? 1U : 0U));
        assert(result.fixture_mounted == (op > FN_EXDISK_MOUNT));
    }
    reset(&f); f.accepted_mount_failure = 1;
    assert(RUN() != 0 && f.mounts == 1 && f.writes == 0);
    assert(result.mount_attempted && !result.fixture_mounted);
    assert(f.calls == 3); /* No retry, geometry or cleanup/unmount. */
    for (op = 2; op <= 3; ++op) {
        reset(&f); f.fail_op = FN_EXDISK_WRITE; f.fail_trial = (unsigned)op;
        assert(RUN() != 0 && result.completed_trials == (unsigned)op - 1);
        assert(f.writes == (unsigned)op - 1 && f.reads == f.writes);
        reset(&f); f.fail_op = FN_EXDISK_READ; f.fail_trial = (unsigned)op;
        assert(RUN() != 0 && result.completed_trials == (unsigned)op - 1);
        assert(f.writes == (unsigned)op && f.reads == (unsigned)op - 1);
    }
    reset(&f); f.sector_size = 1024;
    assert(RUN() != 0 && f.writes == 0 && result.fixture_mounted);
    reset(&f); f.occupied_local = 1; assert(RUN() != 0 && f.mounts == 0 && f.writes == 0);
    reset(&f); f.occupied_remote = 1; assert(RUN() != 0 && f.mounts == 0 && f.writes == 0);
    reset(&f); f.sectors = 17; assert(RUN() != 0 && f.writes == 0 && result.fixture_mounted);
    reset(&f); f.mismatch = 1; assert(RUN() != 0 && f.writes == 1 && f.reads == 1);
    assert(strcmp(result.failure, "read-back-mismatch") == 0);
    reset(&f); f.short_op = FN_EXDISK_WRITE; assert(RUN() != 0 && f.writes == 1 && f.flushes == 0);
    reset(&f); f.short_op = FN_EXDISK_READ; assert(RUN() != 0 && f.writes == 1 && f.reads == 1);
    reset(&f); opts.write_intent = 0; assert(RUN() != 0 && f.calls == 0);
    opts.type = FN_NIO_EXCHANGE_TYPE_DISK_READ;
    reset(&f); assert(RUN() == 0 && f.writes == 0 && f.reads == 3);
    puts("ordinary disk workflow tests passed");
    return 0;
}
