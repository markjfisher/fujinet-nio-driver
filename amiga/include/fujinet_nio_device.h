#ifndef FUJINET_NIO_DEVICE_H
#define FUJINET_NIO_DEVICE_H

#include <exec/io.h>

#define FUJINET_NIO_DEVICE_NAME    "fujinet-nio.device"
#define FUJINET_NIO_DEVICE_UNIT    0

/*
 * CloseDevice requires AbortIO/WaitIO first on this IORequest. The
 * device does not abort an in-progress exchange. A delayed expunge
 * (LIBF_DELEXP) completes when the last CloseDevice drops OpenCnt to 0
 * and the queue and in-progress slot are idle.
 */

/* Broker commands. Control commands affect only its RS-232 backend. */
#define FUJINET_NIO_CMD_EXCHANGE   (CMD_NONSTD + 0)
#define FUJINET_NIO_CMD_SET_BAUD   (CMD_NONSTD + 1)
#define FUJINET_NIO_CMD_GET_BAUD   (CMD_NONSTD + 2)

/* Current struct size; callers must set fn_struct_size to this value */
#define FUJINET_NIO_REQUEST_SIZE   (sizeof(struct FujiNetNIORequest))

struct FujiNetNIORequest {
    /*
     * Exec IORequest header. io_Command = FUJINET_NIO_CMD_EXCHANGE.
     * io_Error is Exec/device only (see architecture §2.1). BeginIO
     * clears IOF_QUICK before any ReplyMsg.
     */
    struct IORequest fn_io;

    /* ABI size guard; must be FUJINET_NIO_REQUEST_SIZE (see architecture §2.1 / §2.2) */
    UWORD        fn_struct_size;

    UWORD        fn_flags;          /* reserved; must be zero */

    /* Opaque FujiBus frame; caller-owned until reply (see architecture §5) */
    const UBYTE *fn_request_data;
    UWORD        fn_request_length;  /* oversize vs platform FN_MAX_PACKET_SIZE */

    /* Caller-owned response buffer; fn_response_length valid only on FN_OK */
    UBYTE       *fn_response_data;
    UWORD        fn_response_capacity;
    UWORD        fn_response_length;

    UBYTE        fn_nio_error;      /* FN-space result; see architecture §2.1 */

    UBYTE        fn_pad[3];         /* alignment; must be zero */
};

#endif
