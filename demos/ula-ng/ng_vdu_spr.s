        * = $0500
        lda #$4E
        sta $0340
        lda #$47
        sta $0340
        lda #23
        sta $0357
        lda #$00
        sta $0357
        lda #$00
        sta $E2
drow    lda #$00
        sta $E3
dcol    lda $E2
        sec
        sbc #7
        bpl rpos
        eor #$FF
        clc
        adc #1
rpos    sta $E4
        lda $E3
        sec
        sbc #7
        bpl cpos
        eor #$FF
        clc
        adc #1
cpos    clc
        adc $E4
        cmp #8
        bcs dtrans
        lda #3
        jmp dput
dtrans  lda #0
dput    sta $0357
        inc $E3
        lda $E3
        cmp #16
        bne dcol
        inc $E2
        lda $E2
        cmp #16
        bne drow
        lda #24
        sta $0357
        lda #$00
        sta $0357
        lda #100
        sta $0357
        lda #80
        sta $0357
        lda #1
        sta $0357
loop    jmp loop
