/* Exclusive SetIntVector handlers. Exec entry:
 *   D1 = INTENA & INTREQ, A0 = custom base, A1 = is_Data, A6 = SysBase.
 * A5 is handler scratch (RX/TX store pointer). RTS, not RTE.
 */
#define SERDATR_OFF 0x018
#define INTREQR_OFF 0x01e
#define INTREQ_OFF  0x09c
#define INTENA_OFF  0x09a
#define SERDAT_OFF  0x030
#define INTF_RBF    0x0800
#define INTF_TBE    0x0001
#define INTB_RBF    11
#define INTB_TBE    0
#define SERDATR_OVRUN 15
#define _LVOCause   -180

#define OFF_BUF         0
#define OFF_MASK        4
#define OFF_HEAD        6
#define OFF_TAIL        8
#define OFF_HW_OV       10
#define OFF_SW_OV       11
#define OFF_NEED        12
#define OFF_SOFTINT     16
#define OFF_RBF_FIRE    20
#define OFF_INGEST      22
#define OFF_CAPTURE     24
#define OFF_FIRST_BYTE  25
#define OFF_FIRST_SER   26

#define OFF_TX_BUF  0
#define OFF_TX_REM  4
#define OFF_TX_DONE 8

        .text
        .align  2
        .globl  _fujinet_serial_rbf_server
_fujinet_serial_rbf_server:
        btst    #INTB_RBF,%d1
        bne.s   .rbf_enter
        rts
.rbf_enter:
        move.w  OFF_HEAD(%a1),%d1
        move.l  OFF_BUF(%a1),%a5
        adda.w  %d1,%a5

.rbf_drain:
        move.w  SERDATR_OFF(%a0),%d0
        move.w  #INTF_RBF,INTREQ_OFF(%a0)
        addq.w  #1,OFF_RBF_FIRE(%a1)

        tst.b   OFF_CAPTURE(%a1)
        beq.s   .no_cap
        clr.b   OFF_CAPTURE(%a1)
        move.b  %d0,OFF_FIRST_BYTE(%a1)
        move.w  %d0,OFF_FIRST_SER(%a1)
.no_cap:
        btst    #SERDATR_OVRUN,%d0
        beq.s   .no_hw_overrun
        move.b  #1,OFF_HW_OV(%a1)
.no_hw_overrun:
        addq.w  #1,%d1
        and.w   OFF_MASK(%a1),%d1
        cmp.w   OFF_TAIL(%a1),%d1
        beq.s   .soft_overrun

        move.b  %d0,(%a5)+
        tst.w   %d1
        bne.s   .stored
        move.l  OFF_BUF(%a1),%a5
.stored:
        addq.w  #1,OFF_INGEST(%a1)
        bra.s   .check_more

.soft_overrun:
        subq.w  #1,%d1
        and.w   OFF_MASK(%a1),%d1
        move.b  #1,OFF_SW_OV(%a1)

.check_more:
        move.w  INTREQR_OFF(%a0),%d0
        btst    #INTB_RBF,%d0
        bne.s   .rbf_drain

        move.w  %d1,OFF_HEAD(%a1)

        move.w  OFF_NEED(%a1),%d0
        beq.s   .rbf_exit
        sub.w   OFF_TAIL(%a1),%d1
        and.w   OFF_MASK(%a1),%d1
        cmp.w   %d0,%d1
        bcs.s   .rbf_exit

        move.l  OFF_SOFTINT(%a1),%a1
        beq.s   .rbf_exit
        jsr     _LVOCause(%a6)

.rbf_exit:
        rts

        .globl  _fujinet_serial_softint
_fujinet_serial_softint:
        movem.l %d2-%d7/%a2-%a6,-(%sp)
        jsr     _fujinet_serial_complete_read
        movem.l (%sp)+,%d2-%d7/%a2-%a6
        rts

        .globl  _fujinet_serial_tbe_server
_fujinet_serial_tbe_server:
        btst    #INTB_TBE,%d1
        beq.s   .tbe_exit

        move.w  #INTF_TBE,INTREQ_OFF(%a0)

        move.l  OFF_TX_REM(%a1),%d0
        beq.s   .tbe_done

        subq.l  #1,%d0
        move.l  %d0,OFF_TX_REM(%a1)
        move.l  OFF_TX_BUF(%a1),%a5
        moveq   #0,%d0
        move.b  (%a5)+,%d0
        move.l  %a5,OFF_TX_BUF(%a1)
        ori.w   #0x0100,%d0
        move.w  %d0,SERDAT_OFF(%a0)
        rts

.tbe_done:
        move.w  #INTF_TBE,INTENA_OFF(%a0)
        move.b  #1,OFF_TX_DONE(%a1)
.tbe_exit:
        rts
