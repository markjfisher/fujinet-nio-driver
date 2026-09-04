# 8520 Complex Interface Adapters — Chip Register Map

(Taken from Amiga Hardware Reference Manual 3rd Edition, Appendix F —
8520 Complex Interface Adapters. Opening assignment paragraph, CIAA/CIAB
address maps, and Chip Register Map only. ≈ pp. 339–340.)

Print-validated workspace extract. Use this file, not the AHRM PDF.
Timer/ICR/CRA programming and the Serial Shift Register
input/output/bidirectional subsections are omitted on purpose.

The three-sentence SDR note at the end is from Appendix F “Serial Shift
Register (SDR)” **introduction only** — not the programming how-to.

Pin wiring for the same CIA ports is in
`cia-port-signal-assigments.md` (Appendix E Part 4, not this appendix).

---

This appendix contains information about the 8520 Complex Interface
Adapter (CIA) chips which handle the serial, parallel, keyboard and other
Amiga I/O activities. Each Amiga system contains two 8520 Complex
Interface Adapter (CIA) chips. Each chip has 16 general purpose
input/output pins, plus a serial shift register, three timers, an output
pulse pin and an edge detection input. In the Amiga system various tasks
are assigned to the chip's capabilities as follows:

## CIAA Address Map

```
---------------------------------------------------------------------------
 Byte    Register                  Data bits
Address    Name     7     6     5     4     3     2     1    0
---------------------------------------------------------------------------
BFE001    pra     /FIR1 /FIR0  /RDY /TK0  /WPRO /CHNG /LED  OVL
BFE101    prb     Parallel port
BFE201    ddra    Direction for port A (BFE001);1=output (set to 0x03)
BFE301    ddrb    Direction for port B (BFE101);1=output (can be in or out)
BFE401    talo    CIAA timer A low byte (.715909 Mhz NTSC; .709379 Mhz PAL)
BFE501    tahi    CIAA timer A high byte
BFE601    tblo    CIAA timer B low byte (.715909 Mhz NTSC; .709379 Mhz PAL)
BFE701    tbhi    CIAA timer B high byte
BFE801    todlo   50/60 Hz event counter bits 7-0 (VSync or line tick)
BFE901    todmid  50/60 Hz event counter bits 15-8
BFEA01    todhi   50/60 Hz event counter bits 23-16
BFEB01            not used
BFEC01    sdr     CIAA serial data register (connected to keyboard)
BFED01    icr     CIAA interrupt control register
BFEE01    cra     CIAA control register A
BFEF01    crb     CIAA control register B
```

Note: CIAA can generate interrupt INT2.

## CIAB Address Map

```
---------------------------------------------------------------------------
 Byte     Register                   Data bits
Address     Name     7     6     5     4     3     2     1     0
---------------------------------------------------------------------------
BFD000    pra     /DTR  /RTS  /CD   /CTS  /DSR   SEL   POUT  BUSY
BFD100    prb     /MTR  /SEL3 /SEL2 /SEL1 /SEL0 /SIDE  DIR  /STEP
BFD200    ddra    Direction for Port A (BFD000);1 = output (set to 0xFF)
BFD300    ddrb    Direction for Port B (BFD100);1 = output (set to 0xFF)
BFD400    talo    CIAB timer A low byte (.715909 Mhz NTSC; .709379 Mhz PAL)
BFD500    tahi    CIAB timer A high byte
BFD600    tblo    CIAB timer B low byte (.715909 Mhz NTSC; .709379 Mhz PAL)
BFD700    tbhi    CIAB timer B high byte
BFD800    todlo   Horizontal sync event counter bits 7-0
BFD900    todmid  Horizontal sync event counter bits 15-8
BFDA00    todhi   Horizontal sync event counter bits 23-16
BFDB00            not used
BFDC00    sdr     CIAB serial data register (unused)
BFDD00    icr     CIAB interrupt control register
BFDE00    cra     CIAB Control register A
BFDF00    crb     CIAB Control register B
```

Note: CIAB can generate INT6.

## Chip Register Map

Each 8520 has 16 registers that you may read or write. Here is the list
of registers and the access address of each within the memory space
dedicated to the 8520:

```
                       Register
   RS3  RS2  RS1  RS0  #(hex)  NAME      MEANING
   -----------------------------------------------------------------
    0    0    0    0     0     pra       peripheral data register a
    0    0    0    1     1     prb       peripheral data register b
    0    0    1    0     2     ddra      Data  direction register a
    0    0    1    1     3     ddrb      direction register b
    0    1    0    0     4     talo      timer a  low register
    0    1    0    1     5     tahi      timer a  high register
    0    1    1    0     6     tblo      timer b  low register
    0    1    1    1     7     tbhi      timer b  high register
    1    0    0    0     8     todlow    event lsb
    1    0    0    1     9     todmid    event 8-15
    1    0    1    0     A     todhi     event msb
    1    0    1    1     B               No connect
    1    1    0    0     C     sdr       serial data register
    1    1    0    1     D     icr       interrupt control register
    1    1    1    0     E     cra       control register a
    1    1    1    1     F     crb       control register b
   -----------------------------------------------------------------
```

## Serial Shift Register — assignment note only

(Appendix F, Serial Shift Register (SDR), opening paragraph. Input mode,
output mode, and bidirectional feature subsections not transcribed.)

The serial port is a buffered, 8-bit synchronous shift register. A control
bit (CRA6) selects input or output mode. In the Amiga system one shift
register is used for the keyboard, and the other is unassigned. Note that
the RS-232 compatible serial port is controlled by the Paula chip; see
chapter 8 for details.
