; passthrough.asm
; LED program that just shifts 64 bits from data RAM 0xA0..0xA7 to the LED chain.
; The userspace driver writes the bit pattern to data RAM; this program is only
; the cheapest possible adapter from RAM bytes to the serial output register.
;
; Bit layout: data RAM byte 0xA0+N, bit i -> LED chain bit (N*8 + i).
; Bit 0 is the first bit shifted out (LED slot 0 amber on the AS5610-52X).

NUM_LEDS  equ  64
DATA_BASE equ  0xA0
DATA_END  equ  0xA8       ; one past last byte (loop terminator)
PTR_P     equ  0xFE       ; scratch: current data RAM pointer

main:
    ld   a,DATA_BASE
    ld   (PTR_P),a
loop:
    ld   a,(PTR_P)
    ld   b,a              ; b := pointer
    ld   b,(b)            ; b := *pointer

    tst  b,0
    push cy
    pack
    tst  b,1
    push cy
    pack
    tst  b,2
    push cy
    pack
    tst  b,3
    push cy
    pack
    tst  b,4
    push cy
    pack
    tst  b,5
    push cy
    pack
    tst  b,6
    push cy
    pack
    tst  b,7
    push cy
    pack

    ld   a,(PTR_P)
    inc  a
    ld   (PTR_P),a
    cmp  a,DATA_END
    jnz  loop

    send NUM_LEDS
