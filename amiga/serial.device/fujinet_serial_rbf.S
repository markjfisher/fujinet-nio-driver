/* Exec RBF server: A1 = device base, scratch D0-D1/A0-A1 only, RTS.
 * AHRM Table 8-9: after reading SERDATR, INTF_RBF in INTREQ must be cleared
 * even when the receive register is already empty. 68040/Emu68 can retire
 * that custom-chip write after RTS, so INTREQ is written twice and NOP
 * precedes RTS.
 */
#include "fujinet_serial_rbf_off.h"

#define SERDATR 0x00dff018
#define INTREQ  0x00dff09c
#define INTF_RBF 0x0800
#define SERDATR_OVRUN 15
#define SERDATR_RBF 14

        .text
        .align  2
        .globl  _fujinet_serial_rbf_server
_fujinet_serial_rbf_server:
        move.w  SERDATR,%d0
        move.w  #INTF_RBF,INTREQ
        move.w  #INTF_RBF,INTREQ
        move.l  %a1,%d1
        beq.s   .done
        btst    #SERDATR_OVRUN,%d0
        beq.s   .check_rbf
        move.b  #1,FN_SERIAL_OFF_RX_OVERRUN(%a1)
.check_rbf:
        btst    #SERDATR_RBF,%d0
        beq.s   .done
        move.w  FN_SERIAL_OFF_RX_HEAD(%a1),%d1
        addq.w  #1,%d1
        and.w   FN_SERIAL_OFF_RX_MASK(%a1),%d1
        cmp.w   FN_SERIAL_OFF_RX_TAIL(%a1),%d1
        beq.s   .soft_overrun
        move.l  FN_SERIAL_OFF_RX_BUF(%a1),%a0
        adda.w  FN_SERIAL_OFF_RX_HEAD(%a1),%a0
        move.b  %d0,(%a0)
        move.w  %d1,FN_SERIAL_OFF_RX_HEAD(%a1)
.done:
        nop
        rts
.soft_overrun:
        move.b  #1,FN_SERIAL_OFF_RX_OVERRUN(%a1)
        nop
        rts
