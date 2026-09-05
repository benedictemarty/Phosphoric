/* SPDX-License-Identifier: EUPL-1.2 */
/**
 * @file mktap_serial.c
 * @brief Generate a tokenized ORIC BASIC .TAP for serial port test
 *
 * Builds the BASIC program in memory using ORIC tokens, then wraps
 * it in TAP format. This avoids the need for a BASIC tokenizer.
 *
 * ORIC BASIC tokens (1.0):
 *   REM=$9D PRINT=$B2 POKE=$B9 PEEK=$C2 FOR=$8B TO=$CA
 *   NEXT=$89 IF=$8D THEN=$C8 GOTO=$91 GET=$A5 END=$84
 *   CLS=$9F LET (implicit) CHR$=$E6 ASC=$FF KEY$=$A4 AND=$AC
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* ORIC BASIC 1.0 tokens */
#define TOK_END     0x84
#define TOK_FOR     0x8B
#define TOK_NEXT    0x89
#define TOK_GOTO    0x91
#define TOK_IF      0x8D
#define TOK_REM     0x9D
#define TOK_CLS     0x9F
#define TOK_PRINT   0xB2
#define TOK_POKE    0xB9
#define TOK_GET     0xA5
#define TOK_THEN    0xC8
#define TOK_TO      0xCA
#define TOK_PEEK    0xC2
#define TOK_AND     0xAC
#define TOK_CHRS    0xE6
#define TOK_ASC     0xFF
#define TOK_KEY     0xA4

#define BASE_ADDR   0x0501
#define MAX_SIZE    8192

static uint8_t mem[MAX_SIZE];
static int pos = 0;

/* Emit raw byte */
static void em(uint8_t b) { mem[pos++] = b; }
/* Emit string (no terminator) */
static void es(const char* s) { while (*s) em((uint8_t)*s++); }
/* Emit quoted string */
static void eq(const char* s) { em('"'); es(s); em('"'); }

/* Begin a BASIC line: next-ptr placeholder (2 bytes) + line number (2 bytes) */
static int begin_line(int linenum) {
    int line_start = pos;
    em(0); em(0);  /* next-line pointer placeholder */
    em((uint8_t)(linenum & 0xFF));
    em((uint8_t)((linenum >> 8) & 0xFF));
    return line_start;
}

/* End a BASIC line: zero terminator + fix next-line pointer */
static void end_line(int line_start) {
    em(0);  /* line terminator */
    uint16_t next_addr = (uint16_t)(BASE_ADDR + pos);
    mem[line_start]     = (uint8_t)(next_addr & 0xFF);
    mem[line_start + 1] = (uint8_t)((next_addr >> 8) & 0xFF);
}

/* Emit : separator */
static void colon(void) { em(':'); }

int main(void) {
    int ls;

    /* 10 REM ACIA 6551 TEST */
    ls = begin_line(10); em(TOK_REM); es(" ACIA 6551 TEST"); end_line(ls);

    /* 20 CLS */
    ls = begin_line(20); em(TOK_CLS); end_line(ls);

    /* 30 PRINT"*** ACIA 6551 TEST ***" */
    ls = begin_line(30); em(TOK_PRINT); eq("*** ACIA 6551 TEST ***"); end_line(ls);

    /* 40 PRINT */
    ls = begin_line(40); em(TOK_PRINT); end_line(ls);

    /* 50 DA=796:ST=797:CM=798:CT=799 */
    ls = begin_line(50); es("DA=796"); colon(); es("ST=797"); colon();
    es("CM=798"); colon(); es("CT=799"); end_line(ls);

    /* 60 POKE ST,0 */
    ls = begin_line(60); em(TOK_POKE); es("ST,0"); end_line(ls);

    /* 70 S=PEEK(ST) */
    ls = begin_line(70); es("S="); em(TOK_PEEK); es("(ST)"); end_line(ls);

    /* 80 T=(S AND 16)/16 */
    ls = begin_line(80); es("T=(S"); em(TOK_AND); es("16)/16"); end_line(ls);

    /* 90 PRINT"TEST 1 RESET: TDRE=";T */
    ls = begin_line(90); em(TOK_PRINT); eq("TEST 1 RESET: TDRE="); es(";T"); end_line(ls);

    /* 100 POKE CT,30 */
    ls = begin_line(100); em(TOK_POKE); es("CT,30"); end_line(ls);

    /* 110 V=PEEK(CT) */
    ls = begin_line(110); es("V="); em(TOK_PEEK); es("(CT)"); end_line(ls);

    /* 120 PRINT"TEST 2 CTRL REG: ";V */
    ls = begin_line(120); em(TOK_PRINT); eq("TEST 2 CTRL REG: "); es(";V"); end_line(ls);

    /* 130 POKE CM,9 */
    ls = begin_line(130); em(TOK_POKE); es("CM,9"); end_line(ls);

    /* 140 V=PEEK(CM) */
    ls = begin_line(140); es("V="); em(TOK_PEEK); es("(CM)"); end_line(ls);

    /* 150 PRINT"TEST 3 CMD REG: ";V */
    ls = begin_line(150); em(TOK_PRINT); eq("TEST 3 CMD REG: "); es(";V"); end_line(ls);

    /* 160 POKE ST,0 */
    ls = begin_line(160); em(TOK_POKE); es("ST,0"); end_line(ls);

    /* 170 POKE CT,31 */
    ls = begin_line(170); em(TOK_POKE); es("CT,31"); end_line(ls);

    /* 180 POKE CM,1 */
    ls = begin_line(180); em(TOK_POKE); es("CM,1"); end_line(ls);

    /* 190 POKE DA,66 */
    ls = begin_line(190); em(TOK_POKE); es("DA,66"); end_line(ls);

    /* 200 FOR W=1 TO 500 */
    ls = begin_line(200); em(TOK_FOR); es("W=1"); em(TOK_TO); es("500"); end_line(ls);

    /* 210 NEXT W */
    ls = begin_line(210); em(TOK_NEXT); es("W"); end_line(ls);

    /* 220 S=PEEK(ST) */
    ls = begin_line(220); es("S="); em(TOK_PEEK); es("(ST)"); end_line(ls);

    /* 230 IF (S AND 8)=0 THEN PRINT"TEST 4 LOOPBACK: NO DATA":GOTO 270 */
    ls = begin_line(230); em(TOK_IF); es("(S"); em(TOK_AND); es("8)=0");
    em(TOK_THEN); em(TOK_PRINT); eq("TEST 4 LOOPBACK: NO DATA"); colon();
    em(TOK_GOTO); es("270"); end_line(ls);

    /* 240 V=PEEK(DA) */
    ls = begin_line(240); es("V="); em(TOK_PEEK); es("(DA)"); end_line(ls);

    /* 250 IF V=66 THEN PRINT"TEST 4 LOOPBACK: OK" */
    ls = begin_line(250); em(TOK_IF); es("V=66"); em(TOK_THEN);
    em(TOK_PRINT); eq("TEST 4 LOOPBACK: OK"); end_line(ls);

    /* 260 IF V<>66 THEN PRINT"TEST 4 LOOPBACK: GOT ";V */
    ls = begin_line(260); em(TOK_IF); es("V<>66"); em(TOK_THEN);
    em(TOK_PRINT); eq("TEST 4 LOOPBACK: GOT "); es(";V"); end_line(ls);

    /* 270 POKE ST,0 */
    ls = begin_line(270); em(TOK_POKE); es("ST,0"); end_line(ls);

    /* 280 POKE CT,31 */
    ls = begin_line(280); em(TOK_POKE); es("CT,31"); end_line(ls);

    /* 290 POKE CM,1 */
    ls = begin_line(290); em(TOK_POKE); es("CM,1"); end_line(ls);

    /* 300 OK=0 */
    ls = begin_line(300); es("OK=0"); end_line(ls);

    /* 310 FOR I=0 TO 9 */
    ls = begin_line(310); em(TOK_FOR); es("I=0"); em(TOK_TO); es("9"); end_line(ls);

    /* 320 POKE DA,65+I */
    ls = begin_line(320); em(TOK_POKE); es("DA,65+I"); end_line(ls);

    /* 330 FOR W=1 TO 500 */
    ls = begin_line(330); em(TOK_FOR); es("W=1"); em(TOK_TO); es("500"); end_line(ls);

    /* 340 NEXT W */
    ls = begin_line(340); em(TOK_NEXT); es("W"); end_line(ls);

    /* 350 S=PEEK(ST) */
    ls = begin_line(350); es("S="); em(TOK_PEEK); es("(ST)"); end_line(ls);

    /* 360 IF (S AND 8)=0 THEN GOTO 400 */
    ls = begin_line(360); em(TOK_IF); es("(S"); em(TOK_AND); es("8)=0");
    em(TOK_THEN); em(TOK_GOTO); es("400"); end_line(ls);

    /* 370 V=PEEK(DA) */
    ls = begin_line(370); es("V="); em(TOK_PEEK); es("(DA)"); end_line(ls);

    /* 380 IF V=65+I THEN OK=OK+1 */
    ls = begin_line(380); em(TOK_IF); es("V=65+I"); em(TOK_THEN);
    es("OK=OK+1"); end_line(ls);

    /* 390 REM */
    ls = begin_line(390); em(TOK_REM); end_line(ls);

    /* 400 NEXT I */
    ls = begin_line(400); em(TOK_NEXT); es("I"); end_line(ls);

    /* 410 PRINT"TEST 5 MULTI: ";OK;"/10" */
    ls = begin_line(410); em(TOK_PRINT); eq("TEST 5 MULTI: "); es(";OK;");
    eq("/10"); end_line(ls);

    /* 420 PRINT */
    ls = begin_line(420); em(TOK_PRINT); end_line(ls);

    /* 430 PRINT"PRESS T FOR TERMINAL" */
    ls = begin_line(430); em(TOK_PRINT); eq("PRESS T FOR TERMINAL"); end_line(ls);

    /* 440 GET A$ */
    ls = begin_line(440); em(TOK_GET); es("A$"); end_line(ls);

    /* 450 IF A$<>"T" THEN END */
    ls = begin_line(450); em(TOK_IF); es("A$<>"); eq("T"); em(TOK_THEN);
    em(TOK_END); end_line(ls);

    /* 460 CLS */
    ls = begin_line(460); em(TOK_CLS); end_line(ls);

    /* 470 PRINT"SERIAL TERMINAL (ESC=QUIT)" */
    ls = begin_line(470); em(TOK_PRINT); eq("SERIAL TERMINAL (ESC=QUIT)"); end_line(ls);

    /* 480 PRINT */
    ls = begin_line(480); em(TOK_PRINT); end_line(ls);

    /* 490 POKE ST,0 */
    ls = begin_line(490); em(TOK_POKE); es("ST,0"); end_line(ls);

    /* 500 POKE CT,31 */
    ls = begin_line(500); em(TOK_POKE); es("CT,31"); end_line(ls);

    /* 510 POKE CM,1 */
    ls = begin_line(510); em(TOK_POKE); es("CM,1"); end_line(ls);

    /* 520 S=PEEK(ST) */
    ls = begin_line(520); es("S="); em(TOK_PEEK); es("(ST)"); end_line(ls);

    /* 530 IF (S AND 8)=0 THEN GOTO 570 */
    ls = begin_line(530); em(TOK_IF); es("(S"); em(TOK_AND); es("8)=0");
    em(TOK_THEN); em(TOK_GOTO); es("570"); end_line(ls);

    /* 540 C=PEEK(DA) */
    ls = begin_line(540); es("C="); em(TOK_PEEK); es("(DA)"); end_line(ls);

    /* 550 IF C=13 THEN PRINT */
    ls = begin_line(550); em(TOK_IF); es("C=13"); em(TOK_THEN); em(TOK_PRINT); end_line(ls);

    /* 560 IF C>31 THEN IF C<127 THEN PRINT CHR$(C); */
    ls = begin_line(560); em(TOK_IF); es("C>31"); em(TOK_THEN); em(TOK_IF);
    es("C<127"); em(TOK_THEN); em(TOK_PRINT); em(TOK_CHRS); es("(C);"); end_line(ls);

    /* 570 K$=KEY$ */
    ls = begin_line(570); es("K$="); em(TOK_KEY); end_line(ls);

    /* 580 IF K$="" THEN GOTO 520 */
    ls = begin_line(580); em(TOK_IF); es("K$="); eq(""); em(TOK_THEN);
    em(TOK_GOTO); es("520"); end_line(ls);

    /* 590 IF ASC(K$)=27 THEN END */
    ls = begin_line(590); em(TOK_IF); em(TOK_ASC); es("(K$)=27"); em(TOK_THEN);
    em(TOK_END); end_line(ls);

    /* 600 POKE DA,ASC(K$) */
    ls = begin_line(600); em(TOK_POKE); es("DA,"); em(TOK_ASC); es("(K$)"); end_line(ls);

    /* 610 GOTO 520 */
    ls = begin_line(610); em(TOK_GOTO); es("520"); end_line(ls);

    /* End of program marker */
    em(0); em(0);

    /* ── Write TAP file ── */
    int data_size = pos;
    uint16_t end_addr = (uint16_t)(BASE_ADDR + data_size - 1);

    FILE* fp = fopen("examples/serial_test.tap", "wb");
    if (!fp) { perror("fopen"); return 1; }

    /* Sync bytes */
    fputc(0x16, fp); fputc(0x16, fp); fputc(0x16, fp);
    fputc(0x24, fp);  /* sync marker */

    /* Header: 9 bytes */
    fputc(0x00, fp);  /* unused */
    fputc(0x80, fp);  /* auto-run flag */
    fputc((end_addr >> 8) & 0xFF, fp);  /* end addr high */
    fputc(end_addr & 0xFF, fp);         /* end addr low */
    fputc((BASE_ADDR >> 8) & 0xFF, fp); /* start addr high */
    fputc(BASE_ADDR & 0xFF, fp);        /* start addr low */
    fputc(0x00, fp);  /* unused */

    /* Name (null-terminated) */
    fputs("SERTEST", fp); fputc(0x00, fp);

    /* Data */
    fwrite(mem, 1, (size_t)data_size, fp);
    fclose(fp);

    printf("Generated examples/serial_test.tap (%d bytes BASIC, %d bytes TAP)\n",
           data_size, (int)(3 + 1 + 7 + 8 + data_size));
    printf("Address: $%04X-$%04X\n", BASE_ADDR, end_addr);
    printf("Lines: 10-610\n");
    return 0;
}
