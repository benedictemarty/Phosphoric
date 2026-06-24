        * = $0500
        lda #$4F
        sta $FB00
        lda #$43
        sta $FB00
        lda #$4F
        sta $BFE8
        lda #$43
        sta $BFE9
        lda #$03
        sta $BFE0
        lda #$00
        sta $E0
        lda #$A0
        sta $E1
        lda #$C7
        sta $E2
        ldx #$C8
rowlp   ldy #$00
        lda $E2
        ora #$20
bytlp   sta ($E0),y
        iny
        cpy #$28
        bne bytlp
        clc
        lda $E0
        adc #$28
        sta $E0
        lda $E1
        adc #$00
        sta $E1
        inc $E2
        dex
        bne rowlp
        ldx #$00
clrtxt  lda #$20
        sta $BF40,x
        inx
        cpx #$A0
        bne clrtxt
        lda #$1D
        sta $BB80
        sta $A000
        lda #$00
        sta $E3
anim    lda $E3
        sta $BFE7
        eor #$FF
        sta $BFE0
        inc $E3
        ldy #$40
dl1     ldx #$00
dl2     dex
        bne dl2
        dey
        bne dl1
        jmp anim
