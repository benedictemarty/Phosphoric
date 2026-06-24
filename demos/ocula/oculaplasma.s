        * = $0500
        lda #$4F
        sta $FB00
        lda #$43
        sta $FB00
        lda #$4F
        sta $BFE8
        lda #$43
        sta $BFE9
        ldx #$00
loop    txa
        sta $BFE0
        sta $BFE1
        sta $BFE2
        sta $BFE3
        sta $BFE4
        sta $BFE5
        sta $BFE6
        sta $BFE7
        inx
        jmp loop
