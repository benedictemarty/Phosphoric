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
        ldy #$00
clr     lda #$20
        sta ($E0),y
        iny
        bne clr
        inc $E1
        dex
        bne clr
        lda #$00
        sta $034D
        lda #$00
        sta $E2
        lda #200
        sta $E5
srow    lda $E2
        lsr
        lsr
        lsr
        sta $E3
        ldx #$00
scol    txa
        clc
        adc $E3
        and #$07
        sta $E4
        asl
        asl
        asl
        ora $E4
        sta $034E
        inx
        cpx #40
        bne scol
        inc $E2
        dec $E5
        bne srow
        lda #$02
        sta $0341
loop    jmp loop
