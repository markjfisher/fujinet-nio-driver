/*
 * KS 1.3 CreateProc accepts a BCPL seglist, not an entry point. The first
 * longword is the deliberately fake hunk length described by the CreateProc
 * autodoc; the exported label is the BPTR-visible next-segment longword.
 */
        .text
        .even
        .long   16
        .globl  _fujinet_nio_legacy_worker_seglist
_fujinet_nio_legacy_worker_seglist:
        .long   0
        jsr     _fujinet_nio_legacy_worker_entry
        rts
