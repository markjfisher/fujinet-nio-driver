/* Exclusive Exec INTB_RBF handler: A1 = rbf_data, RTS not RTE.
 * Scratch is D0-D1/A0-A1 only. A0 holds SysBase solely for _LVOCause.
 * Sample SERDATR, retain, ack INTF_RBF once; repeat while RBF stays asserted.
 */
#define SERDATR 0x00dff018
#define INTENAR 0x00dff01c
#define INTREQR 0x00dff01e
#define INTREQ  0x00dff09c
#define INTF_RBF 0x0800
#define INTB_INTEN 14
#define INTB_RBF 11
#define SERDATR_OVRUN 15
#define SERDATR_RBF 14
#define _LVOCause -180

#define OFF_BUF     0
#define OFF_MASK    4
#define OFF_HEAD    6
#define OFF_TAIL    8
#define OFF_HW_OV   10
#define OFF_SW_OV   11
#define OFF_NEED    12
#define OFF_SOFTINT 16
#define OFF_SYSBASE 20

        .text
        .align  2
        .globl  _fujinet_serial_rbf_server
_fujinet_serial_rbf_server:
        move.w  INTENAR,%d0
        btst    #INTB_INTEN,%d0
        beq.s   .exit
        move.l  %a1,%d0
        beq.s   .exit
.drain:
        move.w  INTREQR,%d0
        btst    #INTB_RBF,%d0
        beq.s   .maybe_cause
        move.w  SERDATR,%d0
        btst    #SERDATR_OVRUN,%d0
        beq.s   .check_byte
        move.b  #1,OFF_HW_OV(%a1)
.check_byte:
        btst    #SERDATR_RBF,%d0
        beq.s   .ack
        move.w  OFF_HEAD(%a1),%d1
        addq.w  #1,%d1
        and.w   OFF_MASK(%a1),%d1
        cmp.w   OFF_TAIL(%a1),%d1
        beq.s   .soft_overrun
        move.l  OFF_BUF(%a1),%a0
        adda.w  OFF_HEAD(%a1),%a0
        move.b  %d0,(%a0)
        move.w  %d1,OFF_HEAD(%a1)
        bra.s   .ack
.soft_overrun:
        move.b  #1,OFF_SW_OV(%a1)
.ack:
        move.w  #INTF_RBF,INTREQ
        bra.s   .drain
.maybe_cause:
        move.w  OFF_NEED(%a1),%d0
        beq.s   .exit
        move.w  OFF_HEAD(%a1),%d1
        sub.w   OFF_TAIL(%a1),%d1
        and.w   OFF_MASK(%a1),%d1
        cmp.w   %d0,%d1
        bcs.s   .exit
        move.l  OFF_SYSBASE(%a1),%a0
        move.l  %a0,%d0
        beq.s   .exit
        move.l  OFF_SOFTINT(%a1),%a1
        beq.s   .exit
        jsr     _LVOCause(%a0)
.exit:
        rts

        .globl  _fujinet_serial_softint
_fujinet_serial_softint:
        movem.l %d2-%d7/%a2-%a6,-(%sp)
        jsr     _fujinet_serial_complete_read
        movem.l (%sp)+,%d2-%d7/%a2-%a6
        rts
