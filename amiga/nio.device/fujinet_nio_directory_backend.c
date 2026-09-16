#ifndef __AMIGA__
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#endif

#include "fujinet_nio_directory_backend.h"

#include "fujinet_nio_backend.h"
#include "fujinet-nio.h"
#include "fn_protocol.h"

#include <string.h>

#ifdef __AMIGA__
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <dos/var.h>
#include <exec/memory.h>
#include <exec/types.h>
#include <proto/dos.h>
#include <proto/exec.h>
#else
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#endif

#ifndef FUJINET_NIO_DIRECTORY_TRANSFER_TIMEOUT_MS
#define FUJINET_NIO_DIRECTORY_TRANSFER_TIMEOUT_MS 5000
#endif
#ifndef FUJINET_NIO_DIRECTORY_POLL_MS
#define FUJINET_NIO_DIRECTORY_POLL_MS 5
#endif

#define FN_DIR_PATH_MAX 280
#define FN_DIR_DIR_MAX 256

/* Version finds this; filename and token must say native-test. */
#ifdef __GNUC__
__attribute__((used))
#endif
const char fujinet_nio_native_test_ver[] =
    "$VER: fujinet-nio-native-test.device 0.1 (16.9.2026) native-test\r\n";

#ifdef __AMIGA__
extern struct ExecBase *SysBase;
struct DosLibrary *DOSBase;
#endif

static fn_packet_backend_t guard;
static uint8_t scratch[FN_DIRECTORY_PACKET_CAPACITY];
static uint8_t guard_inited;
static char directory[FN_DIR_DIR_MAX];

static int join_path(char *out, unsigned out_cap, const char *dir,
                     const char *name)
{
    unsigned dir_len;
    unsigned name_len;
    unsigned need;
    int add_slash;

    if (out == NULL || dir == NULL || name == NULL || out_cap == 0)
        return -1;
    dir_len = (unsigned)strlen(dir);
    name_len = (unsigned)strlen(name);
    if (dir_len == 0)
        return -1;
    add_slash = (dir[dir_len - 1U] != ':' && dir[dir_len - 1U] != '/');
    need = dir_len + (add_slash ? 1U : 0U) + name_len + 1U;
    if (need > out_cap)
        return -1;
    memcpy(out, dir, dir_len);
    if (add_slash)
        out[dir_len++] = '/';
    memcpy(out + dir_len, name, name_len + 1U);
    return 0;
}

#ifdef __AMIGA__
#define FN_DIR_SLEEP_MS 20
#else
#define FN_DIR_SLEEP_MS FUJINET_NIO_DIRECTORY_POLL_MS
#endif

#ifdef __AMIGA__
static uint8_t ensure_dos(void)
{
    if (DOSBase != NULL)
        return FN_OK;
    DOSBase = (struct DosLibrary *)OpenLibrary("dos.library", 37);
    return DOSBase != NULL ? FN_OK : FN_ERR_TRANSPORT;
}

static void sleep_poll(void)
{
    if (DOSBase != NULL)
        Delay(1);
}

static LONG get_env_dir(char *out, LONG cap)
{
    if (ensure_dos() != FN_OK)
        return -1;
    return GetVar((CONST_STRPTR)"FN_NATIVE_TEST_DIR", out, cap, 0);
}

static int directory_usable(const char *path)
{
    BPTR lock;
    struct FileInfoBlock *fib;
    int ok = 0;

    if (path == NULL || path[0] == '\0' || ensure_dos() != FN_OK)
        return 0;
    lock = Lock((CONST_STRPTR)path, ACCESS_READ);
    if (lock == 0)
        return 0;
    fib = (struct FileInfoBlock *)AllocMem(sizeof(*fib),
                                           MEMF_PUBLIC | MEMF_CLEAR);
    if (fib != NULL && Examine(lock, fib) && fib->fib_DirEntryType > 0)
        ok = 1;
    if (fib != NULL)
        FreeMem(fib, sizeof(*fib));
    UnLock(lock);
    return ok;
}

static int file_exists(const char *path)
{
    BPTR lock;

    if (path == NULL || ensure_dos() != FN_OK)
        return 0;
    lock = Lock((CONST_STRPTR)path, ACCESS_READ);
    if (lock == 0)
        return 0;
    UnLock(lock);
    return 1;
}

static int file_remove(const char *path)
{
    LONG err;

    if (path == NULL || ensure_dos() != FN_OK)
        return -1;
    if (DeleteFile((CONST_STRPTR)path) != 0)
        return 0;
    err = IoErr();
    if (err == ERROR_OBJECT_NOT_FOUND)
        return 0;
    return -1;
}

static int file_size_of(const char *path, uint32_t *size)
{
    BPTR lock;
    struct FileInfoBlock *fib;
    int rc = -1;

    if (path == NULL || size == NULL || ensure_dos() != FN_OK)
        return -1;
    lock = Lock((CONST_STRPTR)path, ACCESS_READ);
    if (lock == 0)
        return IoErr() == ERROR_OBJECT_NOT_FOUND ? 1 : -1;
    fib = (struct FileInfoBlock *)AllocMem(sizeof(*fib),
                                           MEMF_PUBLIC | MEMF_CLEAR);
    if (fib != NULL && Examine(lock, fib) && fib->fib_DirEntryType < 0) {
        *size = (uint32_t)fib->fib_Size;
        rc = 0;
    }
    if (fib != NULL)
        FreeMem(fib, sizeof(*fib));
    UnLock(lock);
    return rc;
}

static int file_read_all(const char *path, uint8_t *buffer, uint32_t size)
{
    BPTR fh;
    uint32_t got = 0;

    if (path == NULL || buffer == NULL || ensure_dos() != FN_OK)
        return -1;
    fh = Open((CONST_STRPTR)path, MODE_OLDFILE);
    if (fh == 0)
        return -1;
    while (got < size) {
        LONG n = Read(fh, buffer + got, size - got);
        if (n <= 0) {
            Close(fh);
            return -1;
        }
        got += (uint32_t)n;
    }
    Close(fh);
    return 0;
}

static int file_write_tmp(const char *tmp, const uint8_t *packet, uint32_t size)
{
    BPTR fh;
    uint32_t put = 0;

    if (tmp == NULL || packet == NULL || ensure_dos() != FN_OK)
        return -1;
    (void)file_remove(tmp);
    fh = Open((CONST_STRPTR)tmp, MODE_NEWFILE);
    if (fh == 0)
        return -1;
    while (put < size) {
        LONG n = Write(fh, (APTR)(packet + put), size - put);
        if (n <= 0) {
            Close(fh);
            (void)file_remove(tmp);
            return -1;
        }
        put += (uint32_t)n;
    }
    Close(fh);
    return 0;
}

static int file_rename(const char *tmp, const char *dest)
{
    if (tmp == NULL || dest == NULL || ensure_dos() != FN_OK)
        return -1;
    if (Rename((CONST_STRPTR)tmp, (CONST_STRPTR)dest) != 0)
        return 0;
    return -1;
}

#else /* POSIX host tests */

static void sleep_poll(void)
{
    {
        struct timespec ts;

        ts.tv_sec = 0;
        ts.tv_nsec = (long)FUJINET_NIO_DIRECTORY_POLL_MS * 1000000L;
        (void)nanosleep(&ts, NULL);
    }
}

static int directory_usable(const char *path)
{
    struct stat st;

    if (path == NULL || path[0] == '\0')
        return 0;
    if (stat(path, &st) != 0 || !S_ISDIR(st.st_mode))
        return 0;
    return access(path, R_OK | W_OK | X_OK) == 0;
}

static int file_exists(const char *path)
{
    struct stat st;

    if (path == NULL)
        return 0;
    return stat(path, &st) == 0;
}

static int file_remove(const char *path)
{
    if (path == NULL)
        return -1;
    if (unlink(path) == 0 || errno == ENOENT)
        return 0;
    return -1;
}

static int file_size_of(const char *path, uint32_t *size)
{
    struct stat st;

    if (path == NULL || size == NULL)
        return -1;
    if (stat(path, &st) != 0)
        return errno == ENOENT ? 1 : -1;
    if (!S_ISREG(st.st_mode)) {
        (void)file_remove(path);
        return -1;
    }
    *size = (uint32_t)st.st_size;
    return 0;
}

static int file_read_all(const char *path, uint8_t *buffer, uint32_t size)
{
    int fd;
    uint32_t got = 0;

    if (path == NULL || buffer == NULL)
        return -1;
    fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;
    while (got < size) {
        ssize_t n = read(fd, buffer + got, size - got);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0) {
            close(fd);
            return -1;
        }
        got += (uint32_t)n;
    }
    close(fd);
    return 0;
}

static int file_write_tmp(const char *tmp, const uint8_t *packet, uint32_t size)
{
    int fd;
    uint32_t put = 0;

    if (tmp == NULL || packet == NULL)
        return -1;
    (void)file_remove(tmp);
    fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL | O_TRUNC, 0644);
    if (fd < 0)
        return -1;
    while (put < size) {
        ssize_t n = write(fd, packet + put, size - put);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0) {
            close(fd);
            (void)file_remove(tmp);
            return -1;
        }
        put += (uint32_t)n;
    }
    if (fsync(fd) != 0) {
        close(fd);
        (void)file_remove(tmp);
        return -1;
    }
    close(fd);
    return 0;
}

static int file_rename(const char *tmp, const char *dest)
{
    if (tmp == NULL || dest == NULL)
        return -1;
    if (rename(tmp, dest) == 0)
        return 0;
    return -1;
}

#endif

static int resolve_directory(char *out, unsigned cap)
{
#ifdef __AMIGA__
    char env[FN_DIR_DIR_MAX];
    LONG n;

    memset(env, 0, sizeof(env));
    n = get_env_dir(env, (LONG)sizeof(env) - 1);
    if (n > 0 && (unsigned)n < sizeof(env)) {
        env[n] = '\0';
        if ((unsigned)strlen(env) >= cap)
            return -1;
        strcpy(out, env);
        return 0;
    }
#else
    const char *env = getenv("FN_NATIVE_TEST_DIR");

    if (env != NULL && env[0] != '\0') {
        if (strlen(env) >= cap)
            return -1;
        strcpy(out, env);
        return 0;
    }
#endif
    if (strlen(FN_DIRECTORY_DEFAULT_VOLUME) >= cap)
        return -1;
    strcpy(out, FN_DIRECTORY_DEFAULT_VOLUME);
    return 0;
}

static int refresh_directory(void)
{
    if (resolve_directory(directory, sizeof(directory)) != 0)
        return -1;
    return directory_usable(directory) ? 0 : -1;
}

static int identity_is_native_test(void)
{
    char path[FN_DIR_PATH_MAX];
    uint8_t body[16];
    uint32_t size = 0;
    int rc;

    if (join_path(path, sizeof(path), directory, FN_DIRECTORY_IDENTITY_NAME) != 0)
        return 0;
    rc = file_size_of(path, &size);
    if (rc != 0)
        return 0;
    if (size != (uint32_t)(sizeof(FN_DIRECTORY_IDENTITY_BODY) - 1U))
        return 0;
    if (file_read_all(path, body, size) != 0)
        return 0;
    return memcmp(body, FN_DIRECTORY_IDENTITY_BODY, size) == 0;
}

static int discard_named(const char *name)
{
    char path[FN_DIR_PATH_MAX];
    char tmp[FN_DIR_PATH_MAX];
    int rc = 0;

    if (join_path(path, sizeof(path), directory, name) != 0)
        return -1;
    if (join_path(tmp, sizeof(tmp), directory, name) != 0)
        return -1;
    if (strlen(tmp) + 4U >= sizeof(tmp))
        return -1;
    strcat(tmp, ".tmp");
    if (file_remove(path) != 0)
        rc = -1;
    if (file_remove(tmp) != 0)
        rc = -1;
    return rc;
}

static int discard_records(void)
{
    int rc = 0;

    if (!directory_usable(directory))
        return -1;
    if (discard_named(FN_DIRECTORY_TO_HOST_NAME) != 0)
        rc = -1;
    if (discard_named(FN_DIRECTORY_TO_GUEST_NAME) != 0)
        rc = -1;
    return rc;
}

static int named_absent(const char *name)
{
    char path[FN_DIR_PATH_MAX];
    char tmp[FN_DIR_PATH_MAX];

    if (join_path(path, sizeof(path), directory, name) != 0)
        return 0;
    if (join_path(tmp, sizeof(tmp), directory, name) != 0)
        return 0;
    if (strlen(tmp) + 4U >= sizeof(tmp))
        return 0;
    strcat(tmp, ".tmp");
    return !file_exists(path) && !file_exists(tmp);
}

int fujinet_nio_directory_records_absent(void)
{
    if (refresh_directory() != 0)
        return 0;
    return named_absent(FN_DIRECTORY_TO_HOST_NAME) &&
           named_absent(FN_DIRECTORY_TO_GUEST_NAME);
}

int fujinet_nio_directory_client_reset(void)
{
    if (refresh_directory() != 0)
        return FN_DIR_UNAVAILABLE;
    if (discard_records() != 0)
        return FN_DIR_FAILED;
    return FN_DIR_OK;
}

int fujinet_nio_directory_client_send(const uint8_t *packet, uint32_t size)
{
    char dest[FN_DIR_PATH_MAX];
    char tmp[FN_DIR_PATH_MAX];

    if (refresh_directory() != 0)
        return FN_DIR_UNAVAILABLE;
    if (size == 0)
        return FN_DIR_EMPTY;
    if (size > FN_DIRECTORY_PACKET_CAPACITY || packet == NULL)
        return FN_DIR_OVERSIZED;
    if (join_path(dest, sizeof(dest), directory, FN_DIRECTORY_TO_HOST_NAME) != 0)
        return FN_DIR_UNAVAILABLE;
    if (file_exists(dest))
        return FN_DIR_BACKPRESSURE;
    if (strlen(dest) + 4U >= sizeof(tmp))
        return FN_DIR_UNAVAILABLE;
    strcpy(tmp, dest);
    strcat(tmp, ".tmp");
    if (file_write_tmp(tmp, packet, size) != 0)
        return directory_usable(directory) ? FN_DIR_FAILED : FN_DIR_UNAVAILABLE;
    if (file_exists(dest)) {
        (void)file_remove(tmp);
        return FN_DIR_BACKPRESSURE;
    }
    if (file_rename(tmp, dest) != 0) {
        (void)file_remove(tmp);
        if (file_exists(dest))
            return FN_DIR_BACKPRESSURE;
        return directory_usable(directory) ? FN_DIR_FAILED : FN_DIR_UNAVAILABLE;
    }
    return FN_DIR_OK;
}

int fujinet_nio_directory_client_receive(uint8_t *buffer, uint16_t capacity,
                                         uint16_t *length)
{
    char path[FN_DIR_PATH_MAX];
    uint32_t size = 0;
    int rc;

    if (length != NULL)
        *length = 0;
    if (refresh_directory() != 0)
        return FN_DIR_UNAVAILABLE;
    if (join_path(path, sizeof(path), directory, FN_DIRECTORY_TO_GUEST_NAME) != 0)
        return FN_DIR_UNAVAILABLE;
    rc = file_size_of(path, &size);
    if (rc == 1)
        return FN_DIR_NODATA;
    if (rc != 0)
        return FN_DIR_UNAVAILABLE;
    if (size == 0) {
        (void)file_remove(path);
        return FN_DIR_EMPTY;
    }
    if (size > FN_DIRECTORY_PACKET_CAPACITY || size > capacity ||
        buffer == NULL) {
        (void)file_remove(path);
        return FN_DIR_OVERSIZED;
    }
    if (file_read_all(path, buffer, size) != 0) {
        (void)file_remove(path);
        return FN_DIR_TRUNCATED;
    }
    if (file_remove(path) != 0)
        return FN_DIR_UNAVAILABLE;
    if (length != NULL)
        *length = (uint16_t)size;
    return FN_DIR_OK;
}

static uint8_t directory_open(void *context)
{
    (void)context;
    if (refresh_directory() != 0)
        return FN_ERR_TRANSPORT;
    if (!identity_is_native_test())
        return FN_ERR_INVALID;
    return FN_OK;
}

static void directory_close(void *context)
{
    (void)context;
    if (refresh_directory() == 0)
        (void)discard_records();
}

static uint8_t directory_local_reset(void *context)
{
    int status;

    (void)context;
    status = fujinet_nio_directory_client_reset();
    if (status == FN_DIR_OK)
        return FN_OK;
    if (status == FN_DIR_UNAVAILABLE)
        return FN_ERR_TRANSPORT;
    return FN_ERR_IO;
}

static uint8_t directory_quiesce(void *context)
{
    (void)context;
    if (refresh_directory() != 0)
        return FN_ERR_TRANSPORT;
    if (discard_records() != 0)
        return FN_ERR_TRANSPORT;
    if (!named_absent(FN_DIRECTORY_TO_HOST_NAME) ||
        !named_absent(FN_DIRECTORY_TO_GUEST_NAME))
        return FN_ERR_TRANSPORT;
    return FN_OK;
}

static fn_packet_outcome_t directory_transfer(void *context,
    const uint8_t *request, uint16_t request_length,
    uint8_t *response, uint16_t capacity, uint16_t *length)
{
    int sent;
    unsigned elapsed = 0;

    (void)context;
    if (length != NULL)
        *length = 0;
    sent = fujinet_nio_directory_client_send(request, request_length);
    if (sent == FN_DIR_BACKPRESSURE || sent == FN_DIR_EMPTY ||
        sent == FN_DIR_OVERSIZED || sent == FN_DIR_UNAVAILABLE ||
        sent == FN_DIR_FAILED)
        return FN_PACKET_REJECTED;

    while (elapsed <= (unsigned)FUJINET_NIO_DIRECTORY_TRANSFER_TIMEOUT_MS) {
        uint16_t got = 0;
        int rec = fujinet_nio_directory_client_receive(response, capacity, &got);

        if (rec == FN_DIR_OK) {
            if (length != NULL)
                *length = got;
            return FN_PACKET_COMPLETE;
        }
        if (rec != FN_DIR_NODATA) {
            (void)discard_named(FN_DIRECTORY_TO_HOST_NAME);
            return FN_PACKET_UNKNOWN;
        }
        if (elapsed == (unsigned)FUJINET_NIO_DIRECTORY_TRANSFER_TIMEOUT_MS)
            break;
        sleep_poll();
        elapsed += (unsigned)FN_DIR_SLEEP_MS;
    }
    (void)discard_named(FN_DIRECTORY_TO_HOST_NAME);
    return FN_PACKET_UNKNOWN;
}

const fn_packet_io_t fujinet_nio_directory_io = {
    directory_open,
    directory_close,
    directory_transfer,
    directory_local_reset,
    directory_quiesce
};

uint8_t backend_open(void)
{
    uint8_t result;

    if (refresh_directory() != 0)
        return FN_ERR_TRANSPORT;
    if (!identity_is_native_test())
        return FN_ERR_INVALID;
    if (!guard_inited) {
        result = fn_packet_backend_init(&guard, &fujinet_nio_directory_io, NULL,
                                        scratch, FN_DIRECTORY_PACKET_CAPACITY);
        if (result != FN_OK)
            return result;
        guard_inited = 1;
    }
    result = fn_packet_backend_open(&guard);
    if (result != FN_OK)
        return result;
    result = fn_packet_backend_recover(&guard);
    if (result != FN_OK) {
        fn_packet_backend_close(&guard);
        return result;
    }
    return FN_OK;
}

void backend_close(void)
{
    fn_packet_backend_close(&guard);
    if (refresh_directory() == 0)
        (void)discard_records();
#ifdef __AMIGA__
    if (DOSBase != NULL) {
        CloseLibrary((struct Library *)DOSBase);
        DOSBase = NULL;
    }
#endif
}

uint8_t backend_exchange(
    const uint8_t *request,
    uint16_t request_len,
    uint8_t *response,
    uint16_t response_capacity,
    uint16_t *response_len,
    uint8_t *detail,
    uint8_t *native_io_error,
    uint16_t *native_status)
{
    if (detail != NULL)
        *detail = FUJINET_NIO_DETAIL_NONE;
    if (native_io_error != NULL)
        *native_io_error = 0;
    if (native_status != NULL)
        *native_status = 0;
    return fn_packet_backend_exchange(&guard, request, request_len, response,
                                      response_capacity, response_len);
}

uint8_t backend_set_baud(uint32_t baud)
{
    (void)baud;
    return FN_ERR_UNSUPPORTED;
}

uint32_t backend_get_baud(void)
{
    return 0;
}

uint8_t backend_set_serial(uint32_t unit, const char *name)
{
    (void)unit;
    (void)name;
    return FN_ERR_UNSUPPORTED;
}

void backend_get_serial(uint32_t *unit, char *name, uint16_t name_cap)
{
    if (unit != NULL)
        *unit = 0;
    if (name != NULL && name_cap > 0)
        name[0] = '\0';
}
