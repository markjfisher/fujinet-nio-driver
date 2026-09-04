# Serial Interface Connector Specification

(Taken from Amiga Hardware Reference Manual 3rd Edition, Appendix E —
I/O Connectors and Interfaces. Serial Port only: Part 1 RS232/MIDI pin
table, plus Part 2 Serial Interface Specification, Timing, and Electrical
Characteristics. ≈ pp. 318, 325–327.)

Print-validated workspace extract. Use this file, not the AHRM PDF.

---

## Part 1 — RS232 and MIDI Port pin numbers

```
RS232 and MIDI Port
-------------------

                   A500/
                   A2000/ CBM
PIN  RS232  A1000  A3000  PCs   HAYES  DESCRIPTION
---------------------------------------------------
1    GND    GND    GND    GND   GND    frame ground
2    TXD    TXD    TXD    TXD   TXD    TRANSMIT DATA
3    RXD    RXD    RXD    RXD   RXD    RECEIVE DATA
4    RTS    RTS    RTS    RTS   -      REQUEST TO SEND
5    CTS    CTS    CTS    CTS   CTS    CLEAR TO SEND
6    DSR    DSR    DSR    DSR   DSR    DATA SET READY
7    GND    GND    GND    GND   GND    system ground
8    CD     CD     CD     DCD   DCD    CARRIER DETECT
9    -      -      +12v   +12v  -      + 12 VOLT POWER
10   -      -      -12v   -12v  -      - 12 VOLT POWER
11   -      -      AUDO   -     -      audio output  (a500, a2000, a3000)
12   S.SD   -      -      -     SI     SPEED INDICATE
13   S.CTS  -      -      -     -
14   S.TXD  -5Vdc  -      -     -      - 5 volt power
15   TXC    AUDO   -      -     -      audio output  (a1000)
16   S.RXD  AUDI   -      -     -      audio input  (a1000)
17   RXC    EB     -      -     -      BUFFERED PORT CLOCK 716kHz
18   -      INT2*  AUDI   -     -      INTERRUPT LINE A1000/AUDIO INPUT (A500, 2000, 3000)
19   S.RTS  -      -      -     -
20   DTR    DTR    DTR    DTR   DTR    DATA TERMINAL READY
21   SQD    +5            -     -      + 5 VOLT POWER
22   RI     -      RI     RI    RI     ring indicator
23   SS     +12Vdc -      -     -      +12 VOLT POWER
24   TXC1   C2*    -      -     -      3.58 MHZ CLOCK
25   -      RESB*  -      -     -      BUFFERED SYSTEM RESET
```

---

## Part 2 — Serial Interface Specification

This 25-pin D-type connector with sockets (DB25S=female) is used to
interface to RS-232-C standard signals. Signal names correspond to those
used in other places in this appendix, when possible.

WARNING:

Pins on the RS232 connector other than these standard ones described
below may be connected to power or other non-RS232 standard signals.
When making up RS232 cables, connect only those pins actually used
for a particular application. Avoid generic 25-connector "straight-
through" cables.

### Pin Assignment (J6)

```
              RS-232-C

   NAME   DIR  STD  NOTES
   ----   ---  ---  --------------------------
   FGND         y   Frame ground -- do not tie to signal ground
   TXD     O    y   Transmit data
   RXD     I    y   Receive data
   RTS     O    y   Request to send
   CTS     I    y   Clear to send
   DSR     I    y   Data set ready
   GND          y   Signal ground -- do not tie to frame ground
   CD      I    y   Carrier detect
   -5V          n*  50 ma maximum   *** WARNING -5V ***
   AUDO    O    n*  Audio output from left (channels 0, 3) port,
                    intended to send audio to the modem.
   AUDI    I    n*  Audio input to right (channels 1, 2) port,
                    intended to receive audio from the modem; this
                    input is mixed with the analog output of the
                    right (channels 1, 2). It is not digitized or
                    used by the computer in any way.
   DTR     O    y   Data terminal ready.
   RI      I    y   Ring Indicator (A500/A2000 only) shared with printer
                    "select" signal.
   RESB*   O    n*  Amiga system reset.


NOTES:
     n*:  See  warning  above
     See part 1 of this appendix for  pin numbers .
```

### Timing

Maximum operating frequency is 19.2 KHz. Refer to EIA standard RS-232-C
for operating and installation specifications. A rate of 31.25 KHz will
be supported through the use of a MIDI adapter.

Modem control signals (cts, rts, dtr, dsr, cd) are completely under
software control. The modem control lines have no hardware affect on and
are completely asynchronous to txd and rxd.

### Electrical Characteristics

```
   OUTPUTS    MIN   TYP     MAX
   -------    ---   ---     ---
   Vo(-):   -13.2   -x-    -2.5    V      Negative output voltage range
   Vo(+):     8.0   -x-    13.2    V      Positive output voltage range
   Io:        -x-   -x-    10.0    ma     Output current

   INPUTS     MIN   TYP     MAX
   -------    ---   ---     ---
   Vi(+):     3.0   -x-    25.0    V      Positive input voltage range
   Vi(-):   -25.0   -x-     0.5    V      Negative input voltage range
   Vhys:      -x-   1.0     -x-    V      Input hysteresis voltage
   Ii:        0.3   -x-    10.0    ma     Input current
```

Unconnected inputs are interpreted the same as positive input voltages.
