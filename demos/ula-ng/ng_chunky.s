        * = $0500
        lda #$4E
        sta $0340
        lda #$47
        sta $0340
        lda #$00
        sta $0342
        lda #$40
        sta $0343
        lda #$00
        sta $0348
        ldx #$00
pini    lda pallo,x
        sta $0349
        lda palhi,x
        sta $034A
        inx
        cpx #$10
        bne pini
        lda #$00
        sta $E0
        lda #$40
        sta $E1
        lda #$00
        sta $E4
        lda #224
        sta $E5
frow    ldx #$00
        lda $E4
        sta $E6
fcol    lda $E6
        and #$0F
        tay
        lda valtab,y
        ldy #$00
        sta ($E0),y
        inc $E0
        bne fnc
        inc $E1
fnc     inc $E6
        inx
        cpx #80
        bne fcol
        inc $E4
        dec $E5
        bne frow
        lda #$05
        sta $0341
        lda #$00
        sta $E7
anim    lda #$00
        sta $0348
        ldx #$00
arot    txa
        clc
        adc $E7
        and #$0F
        tay
        lda pallo,y
        sta $0349
        lda palhi,y
        sta $034A
        inx
        cpx #$10
        bne arot
        inc $E7
        ldy #$40
d1      ldx #$00
d2      dex
        bne d2
        dey
        bne d1
        jmp anim
valtab  .byte $00,$11,$22,$33,$44,$55,$66,$77
        .byte $88,$99,$AA,$BB,$CC,$DD,$EE,$FF
pallo   .byte $00,$04,$08,$0F,$0F,$0F,$08,$00
        .byte $00,$00,$00,$00,$08,$0F,$0F,$0F
palhi   .byte $00,$00,$00,$00,$80,$F0,$F0,$F0
        .byte $F8,$FF,$8F,$0F,$0F,$0F,$8F,$FF
