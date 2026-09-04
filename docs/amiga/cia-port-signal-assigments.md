# CIA Register - Port Signal Assignments for 8520 CIAS

```
PA7 .. game port 1, pin 6 (fire button*)
PA6 .. game port 0, pin 6 (fire button*)
PA5 .. RDY*       disk ready*
PA4 .. TK0*       disk track 00*
PA3 .. WPRO*      write protect*
PA2 .. CHNG*      disk change*
PA1 .. LED*       led light (O=bright) / audio filter control (A5OO & A2000)
PAO .. OVL        ROM/RAM overlay bit
SP ... KDAT       keyboard data
CNT .. KCLK       keyboard clock
PB7 .. P7         data 7
PB6 .. P6         data 6
PB5 .. P5         data 5    Centronics parallel interface
PB4 .. P4         data 4          data
PB3 .. P3         data 3
PB2 .. P2         data 2
PB1 .. P1         data 1
PBO .. PO         data 0

PC ... drdy*                Centronics control
F .... ack* 
```

## CIA-B Address BFDxOO data bits lS-8 (A13*) (INT6)

```
PA7 .. com line DTR*, driven output
PA6 .. com line RTS*, driven output
PA5 .. com line carrier detect*
PA4 .. com line CTS*
PA3 .. com line DSR*
PA2 .. SEL          Centronics control
PA1 .. POUT   +---  paper out  -------------+
PAO .. BUSY   | +-- busy  ----------------+ |
              | |                         | |
SP ... BUSY   | +-- commodore serial bus -+ |
CNT .. POUT   +---  commodore serial bus ---+

PB7 .. MTR*       motor
PB6 .. SEL3*      select external 3rd drive
PB5 .. SEL2*      select external 2nd drive
PB4 .. SEL1*      select external 1st drive
PB3 .. SELO*      select internal drive
PB2 .. SIDE*      side select *
PB1 .. DIR        direction
PBO .. STEP*      step* (3.0 milliseconds minimum)

PC ... not used
F .... INDEX*     disk index*
```
