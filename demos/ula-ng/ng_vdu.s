        * = $0500
        lda #$4E
        sta $0340
        lda #$47
        sta $0340
        lda #$80
        sta $E0
        lda #$BB
        sta $E1
        ldx #$05
fillspc ldy #$00
        lda #$20
fs2     sta ($E0),y
        iny
        bne fs2
        inc $E1
        dex
        bne fillspc
        lda #$00
        sta $E2
mrow    lda #$00
        sta $E3
mcol    lda #31
        sta $0357
        lda $E3
        sta $0357
        lda $E2
        sta $0357
        lda $E2
        clc
        adc $E3
        and #$07
        sta $E4
        asl
        asl
        asl
        ora $E4
        sta $0357
        inc $E3
        lda $E3
        cmp #40
        bne mcol
        inc $E2
        lda $E2
        cmp #25
        bne mrow
loop    jmp loop
