/* KS 1.3 CreateProc starts a BCPL seglist.  Each fake seglist dispatches to
 * one of the two concurrent diagnostic jobs. */
        .text
        .even
        .long   16
        .globl  _fujinet_nio_exchange_legacy_job_a_seglist
_fujinet_nio_exchange_legacy_job_a_seglist:
        .long   0
        jsr     _fujinet_nio_exchange_legacy_job_a_entry
        rts

        .even
        .long   16
        .globl  _fujinet_nio_exchange_legacy_job_b_seglist
_fujinet_nio_exchange_legacy_job_b_seglist:
        .long   0
        jsr     _fujinet_nio_exchange_legacy_job_b_entry
        rts
