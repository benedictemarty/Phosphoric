        * = $0500
        lda #$4E
        sta $0340
        lda #$47
        sta $0340
        lda #16
        sta $0357
        lda #$00
        sta $E2
top     lda $E2
        lsr
        lsr
        lsr
        and #$0F
        sta $E3
        lda #17
        sta $0357
        lda $E3
        sta $0357
        lda #26
        sta $0357
        lda #80
        sta $0357
        lda #112
        sta $0357
        lda $E2
        sta $0357
        lda #0
        sta $0357
        lda $E2
        clc
        adc #8
        sta $E2
        cmp #160
        bcc top
        lda #$00
        sta $E2
bot     lda $E2
        lsr
        lsr
        lsr
        and #$0F
        sta $E3
        lda #17
        sta $0357
        lda $E3
        sta $0357
        lda #26
        sta $0357
        lda #80
        sta $0357
        lda #112
        sta $0357
        lda $E2
        sta $0357
        lda #223
        sta $0357
        lda $E2
        clc
        adc #8
        sta $E2
        cmp #160
        bcc bot
loop    jmp loop
