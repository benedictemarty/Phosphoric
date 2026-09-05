/* SPDX-License-Identifier: EUPL-1.2 */
/**
 * @file gen_test_tap.c
 * @brief Generate test .TAP files for CLOAD testing
 *
 * Creates minimal but valid ORIC-1 BASIC .TAP files
 * with proper sync/header/data structure for ROM patching.
 *
 * Usage: gen_test_tap [output.tap]
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* ORIC BASIC 1.0 tokens */
/* ORIC BASIC 1.0 tokens (from ROM keyword table at $C0EA) */
#define TOK_PRINT   0xBA
#define TOK_FOR     0x8D
#define TOK_TO      0xC9  /* actually THEN; TO needs checking */
#define TOK_NEXT    0x90
#define TOK_REM     0x9D

#define TAP_SYNC    0x16
#define TAP_MARKER  0x24

static void write_byte(FILE* f, uint8_t b) {
    fwrite(&b, 1, 1, f);
}

/**
 * Build a tokenized BASIC program in a buffer.
 * Returns the number of bytes written.
 *
 * Program generated:
 *   10 REM ** ORIC-1 TEST **
 *   20 PRINT "HELLO ORIC!"
 *   30 PRINT "CLOAD OK"
 */
static int build_basic_program(uint8_t* buf, uint16_t base_addr) {
    int pos = 0;
    uint16_t addr = base_addr;

    /* --- Line 10: REM ** ORIC-1 TEST ** --- */
    {
        const char* comment = " ** ORIC-1 TEST **";
        int line_len = 2 + 2 + 1 + (int)strlen(comment) + 1; /* ptr + linenum + token + text + nul */
        uint16_t next = addr + (uint16_t)line_len;

        buf[pos++] = (uint8_t)(next & 0xFF);
        buf[pos++] = (uint8_t)(next >> 8);
        buf[pos++] = 10; /* line number lo */
        buf[pos++] = 0;  /* line number hi */
        buf[pos++] = TOK_REM;
        memcpy(&buf[pos], comment, strlen(comment));
        pos += (int)strlen(comment);
        buf[pos++] = 0x00; /* end of line */
        addr = next;
    }

    /* --- Line 20: PRINT "HELLO ORIC!" --- */
    {
        const char* str = "HELLO ORIC!";
        int line_len = 2 + 2 + 1 + 1 + (int)strlen(str) + 1 + 1; /* ptr + linenum + PRINT + " + str + " + nul */
        uint16_t next = addr + (uint16_t)line_len;

        buf[pos++] = (uint8_t)(next & 0xFF);
        buf[pos++] = (uint8_t)(next >> 8);
        buf[pos++] = 20;
        buf[pos++] = 0;
        buf[pos++] = TOK_PRINT;
        buf[pos++] = '"';
        memcpy(&buf[pos], str, strlen(str));
        pos += (int)strlen(str);
        buf[pos++] = '"';
        buf[pos++] = 0x00;
        addr = next;
    }

    /* --- Line 30: PRINT "CLOAD OK" --- */
    {
        const char* str = "CLOAD OK";
        int line_len = 2 + 2 + 1 + 1 + (int)strlen(str) + 1 + 1;
        uint16_t next = addr + (uint16_t)line_len;

        buf[pos++] = (uint8_t)(next & 0xFF);
        buf[pos++] = (uint8_t)(next >> 8);
        buf[pos++] = 30;
        buf[pos++] = 0;
        buf[pos++] = TOK_PRINT;
        buf[pos++] = '"';
        memcpy(&buf[pos], str, strlen(str));
        pos += (int)strlen(str);
        buf[pos++] = '"';
        buf[pos++] = 0x00;
        addr = next;
        (void)addr;
    }

    /* End of program marker */
    buf[pos++] = 0x00;
    buf[pos++] = 0x00;

    return pos;
}

int main(int argc, char* argv[]) {
    const char* output = "test_cload.tap";
    if (argc > 1) output = argv[1];

    uint16_t start_addr = 0x0501; /* Standard ORIC BASIC start */
    uint8_t program[256];
    int prog_len = build_basic_program(program, start_addr);
    uint16_t end_addr = start_addr + (uint16_t)prog_len - 1;

    FILE* f = fopen(output, "wb");
    if (!f) {
        fprintf(stderr, "Error: cannot create %s\n", output);
        return 1;
    }

    /* Sync bytes */
    for (int i = 0; i < 3; i++)
        write_byte(f, TAP_SYNC);

    /* Header marker */
    write_byte(f, TAP_MARKER);

    /*
     * 9-byte header block (ROM reads into $66..$5E via LDX #9; STA $5D,X; DEX):
     *   Byte 1 → $66 (unused)
     *   Byte 2 → $65 (unused)
     *   Byte 3 → $64 (type: $00=BASIC, $80=machine code)
     *   Byte 4 → $63 (autorun: $00=no, $C7=yes BASIC, $80=yes M/C)
     *   Byte 5 → $62 (end address high)
     *   Byte 6 → $61 (end address low)
     *   Byte 7 → $60 (start address high)
     *   Byte 8 → $5F (start address low)
     *   Byte 9 → $5E (unused)
     */
    write_byte(f, 0x00);                          /* $66 unused */
    write_byte(f, 0x00);                          /* $65 unused */
    write_byte(f, 0x00);                          /* $64 type = BASIC */
    write_byte(f, 0x00);                          /* $63 autorun = no */
    write_byte(f, (uint8_t)(end_addr >> 8));      /* $62 end_hi */
    write_byte(f, (uint8_t)(end_addr & 0xFF));    /* $61 end_lo */
    write_byte(f, (uint8_t)(start_addr >> 8));    /* $60 start_hi */
    write_byte(f, (uint8_t)(start_addr & 0xFF));  /* $5F start_lo */
    write_byte(f, 0x00);                          /* $5E unused */

    /* Program name (null-terminated) */
    const char* name = "TEST";
    fwrite(name, 1, strlen(name), f);
    write_byte(f, 0x00);

    /* Data: tokenized BASIC program */
    fwrite(program, 1, (size_t)prog_len, f);

    fclose(f);

    printf("Generated %s:\n", output);
    printf("  Name:    TEST\n");
    printf("  Type:    BASIC\n");
    printf("  Start:   $%04X\n", start_addr);
    printf("  End:     $%04X\n", end_addr);
    printf("  Size:    %d bytes\n", prog_len);
    printf("  Program:\n");
    printf("    10 REM ** ORIC-1 TEST **\n");
    printf("    20 PRINT \"HELLO ORIC!\"\n");
    printf("    30 PRINT \"CLOAD OK\"\n");
    printf("\nUsage:\n");
    printf("  ./oric1-emu -r basic10.rom -t %s        # then type CLOAD\n", output);
    printf("  ./oric1-emu -r basic10.rom -t %s -f     # fast-load (direct)\n", output);

    return 0;
}
