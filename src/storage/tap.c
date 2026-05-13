/**
 * @file tap.c
 * @brief ORIC .TAP tape format - complete implementation
 * @author bmarty <bmarty@mailo.com>
 * @date 2026-02-22
 * @version 0.6.0-alpha
 */

#include "storage/tap.h"
#include <stdlib.h>
#include <string.h>

tap_file_t* tap_open_read(const char* filename, bool fast_load) {
    FILE* fp = fopen(filename, "rb");
    if (!fp) return NULL;

    tap_file_t* tap = (tap_file_t*)calloc(1, sizeof(tap_file_t));
    if (!tap) { fclose(fp); return NULL; }

    fseek(fp, 0, SEEK_END);
    tap->size = (uint32_t)ftell(fp);
    fseek(fp, 0, SEEK_SET);

    tap->file = fp;
    tap->writing = false;
    tap->position = 0;
    tap->fast_load = fast_load;

    if (fast_load && tap->size > 0) {
        tap->data = (uint8_t*)malloc(tap->size);
        if (tap->data) {
            if (fread(tap->data, 1, tap->size, fp) != tap->size) {
                free(tap->data);
                tap->data = NULL;
                tap->fast_load = false;
            }
        }
    }

    return tap;
}

tap_file_t* tap_open_write(const char* filename) {
    FILE* fp = fopen(filename, "wb");
    if (!fp) return NULL;

    tap_file_t* tap = (tap_file_t*)calloc(1, sizeof(tap_file_t));
    if (!tap) { fclose(fp); return NULL; }

    tap->file = fp;
    tap->writing = true;
    tap->position = 0;
    tap->size = 0;
    tap->fast_load = false;
    tap->data = NULL;

    return tap;
}

void tap_close(tap_file_t* tap) {
    if (tap) {
        if (tap->file) fclose(tap->file);
        if (tap->data) free(tap->data);
        free(tap);
    }
}

/* Skip sync bytes ($16), return number skipped */
static int skip_sync(tap_file_t* tap) {
    int count = 0;
    while (!tap_eof(tap)) {
        uint8_t byte;
        if (tap->fast_load && tap->data) {
            byte = tap->data[tap->position];
        } else {
            if (fread(&byte, 1, 1, tap->file) != 1) break;
        }
        if (byte == TAP_SYNC_BYTE) {
            tap->position++;
            count++;
        } else {
            /* Not a sync byte, push back */
            if (!tap->fast_load) fseek(tap->file, -1, SEEK_CUR);
            break;
        }
    }
    return count;
}

static uint8_t read_byte(tap_file_t* tap) {
    uint8_t byte = 0;
    if (tap->fast_load && tap->data) {
        if (tap->position < tap->size) byte = tap->data[tap->position];
    } else {
        if (fread(&byte, 1, 1, tap->file) != 1) return 0;
    }
    tap->position++;
    return byte;
}

bool tap_read_header(tap_file_t* tap, tap_header_t* header) {
    if (!tap || !header) return false;

    /* Skip sync bytes */
    skip_sync(tap);

    /* Read marker byte */
    uint8_t marker = read_byte(tap);
    if (marker != TAP_MARKER) return false;

    /* Read raw bytes for format auto-detection.
     * TAP headers come in two variants:
     *   9-byte (ROM format): type auto extra extra end_hi end_lo start_hi start_lo unused
     *   7-byte (tool format): type auto end_hi end_lo start_hi start_lo separator
     * Some TAP files also have 1-4 padding bytes between the $24 marker
     * and the actual header (e.g. poker-asn.tap has $52 $54 padding). */
    uint32_t save_pos = tap->position;
    uint8_t raw[32];
    int raw_len = 0;
    for (int i = 0; i < 32 && !tap_eof(tap); i++) {
        raw[i] = read_byte(tap);
        raw_len++;
    }

    int best_off = 0, best_sz = 9;
    bool found = false;

    /* Try no-padding 9-byte first (standard ROM format, handles TYRANN) */
    if (raw_len >= 9) {
        uint8_t t = raw[0];
        if (t == 0x00 || t == 0x80 || t == 0xC0) {
            uint16_t end = (uint16_t)((raw[4] << 8) | raw[5]);
            uint16_t start = (uint16_t)((raw[6] << 8) | raw[7]);
            if (start < end && start >= 0x0100 && end < 0xC000) {
                best_off = 0; best_sz = 9; found = true;
            }
        }
    }

    /* If not found, try padding (0-4) + 7-byte, then padding + 9-byte */
    if (!found) {
        for (int skip = 0; skip <= 4 && !found; skip++) {
            if (skip + 7 > raw_len) break;
            uint8_t t = raw[skip];
            if (t != 0x00 && t != 0x80 && t != 0xC0) continue;

            /* Try 7-byte format */
            if (skip + 7 <= raw_len) {
                uint16_t end = (uint16_t)((raw[skip + 2] << 8) | raw[skip + 3]);
                uint16_t start = (uint16_t)((raw[skip + 4] << 8) | raw[skip + 5]);
                if (start < end && start >= 0x0100 && end < 0xC000) {
                    best_off = skip; best_sz = 7; found = true;
                    break;
                }
            }
            /* Try 9-byte format with padding */
            if (skip > 0 && skip + 9 <= raw_len) {
                uint16_t end = (uint16_t)((raw[skip + 4] << 8) | raw[skip + 5]);
                uint16_t start = (uint16_t)((raw[skip + 6] << 8) | raw[skip + 7]);
                if (start < end && start >= 0x0100 && end < 0xC000) {
                    best_off = skip; best_sz = 9; found = true;
                    break;
                }
            }
        }
    }

    /* Fallback: 9-byte at offset 0 (original behavior) */

    /* Parse header from raw buffer.
     * 9-byte ROM format: raw[0..1] = reserved ($00 $00), raw[2] = type,
     *                    raw[3] = auto-run, raw[4..5] = end (BE),
     *                    raw[6..7] = start (BE), raw[8] = separator.
     * 7-byte tool format: raw[0] = type, raw[1] = auto-run,
     *                     raw[2..3] = end (BE), raw[4..5] = start (BE),
     *                     raw[6] = separator. */
    int p = best_off;
    if (best_sz == 9) {
        p += 2;  /* skip reserved bytes raw[0..1] = $00 $00 */
    }
    header->type = raw[p++];
    header->auto_run = raw[p++];
    header->end_addr = (uint16_t)((raw[p] << 8) | raw[p + 1]);
    p += 2;
    header->start_addr = (uint16_t)((raw[p] << 8) | raw[p + 1]);
    p += 2;
    p++;  /* skip separator (7-byte) or unused byte (9-byte) */

    /* Read program name from raw buffer */
    memset(header->name, 0, TAP_NAME_LEN);
    for (int i = 0; i < TAP_NAME_LEN - 1 && p < raw_len; i++, p++) {
        if (raw[p] == 0) { p++; break; }
        header->name[i] = (char)raw[p];
    }

    /* Reposition stream to just after name's null terminator */
    tap->position = save_pos + p;
    if (!tap->fast_load && tap->file) {
        fseek(tap->file, (long)tap->position, SEEK_SET);
    }

    return true;
}

bool tap_write_header(tap_file_t* tap, const tap_header_t* header) {
    if (!tap || !header || !tap->writing) return false;

    /* Write sync bytes */
    uint8_t sync = TAP_SYNC_BYTE;
    int sync_count = header->sync_len ? header->sync_len : 3;
    for (int i = 0; i < sync_count; i++) {
        fwrite(&sync, 1, 1, tap->file);
        tap->position++;
    }

    /* Marker */
    uint8_t marker = TAP_MARKER;
    fwrite(&marker, 1, 1, tap->file); tap->position++;

    /* 9-byte ROM-format header: 2 reserved bytes before type/auto-run.
     * Required for Oricutron / Euphoric / hardware compatibility — without
     * them the strict parsers read end/start addresses 2 bytes too early
     * and crash at CLOAD "". Phosphoric's reader handles both 7- and 9-byte
     * variants (tap_read_header). */
    uint8_t reserved = 0x00;
    fwrite(&reserved, 1, 1, tap->file); tap->position++;
    fwrite(&reserved, 1, 1, tap->file); tap->position++;

    /* Type and auto-run */
    fwrite(&header->type, 1, 1, tap->file); tap->position++;
    fwrite(&header->auto_run, 1, 1, tap->file); tap->position++;

    /* End address (big-endian) */
    uint8_t hi = (uint8_t)(header->end_addr >> 8);
    uint8_t lo = (uint8_t)(header->end_addr & 0xFF);
    fwrite(&hi, 1, 1, tap->file); tap->position++;
    fwrite(&lo, 1, 1, tap->file); tap->position++;

    /* Start address (big-endian) */
    hi = (uint8_t)(header->start_addr >> 8);
    lo = (uint8_t)(header->start_addr & 0xFF);
    fwrite(&hi, 1, 1, tap->file); tap->position++;
    fwrite(&lo, 1, 1, tap->file); tap->position++;

    /* Null separator */
    uint8_t null_byte = 0;
    fwrite(&null_byte, 1, 1, tap->file); tap->position++;

    /* Name (null-terminated) */
    size_t namelen = strlen(header->name);
    if (namelen > TAP_NAME_LEN - 1) namelen = TAP_NAME_LEN - 1;
    fwrite(header->name, 1, namelen, tap->file); tap->position += (uint32_t)namelen;
    fwrite(&null_byte, 1, 1, tap->file); tap->position++;

    tap->size = tap->position;
    return true;
}

int tap_read_data(tap_file_t* tap, uint8_t* buffer, size_t size) {
    if (!tap || !buffer) return -1;

    size_t read_count = 0;
    for (size_t i = 0; i < size && !tap_eof(tap); i++) {
        buffer[i] = read_byte(tap);
        read_count++;
    }
    return (int)read_count;
}

bool tap_write_data(tap_file_t* tap, const uint8_t* buffer, size_t size) {
    if (!tap || !buffer || !tap->writing) return false;

    size_t written = fwrite(buffer, 1, size, tap->file);
    tap->position += (uint32_t)written;
    tap->size = tap->position;
    return written == size;
}

void tap_rewind(tap_file_t* tap) {
    if (tap) {
        if (tap->file) rewind(tap->file);
        tap->position = 0;
    }
}

uint32_t tap_tell(const tap_file_t* tap) {
    return tap ? tap->position : 0;
}

uint32_t tap_size(const tap_file_t* tap) {
    return tap ? tap->size : 0;
}

bool tap_eof(const tap_file_t* tap) {
    if (!tap) return true;
    return tap->position >= tap->size;
}

uint8_t tap_checksum(const uint8_t* data, size_t size) {
    uint8_t sum = 0;
    for (size_t i = 0; i < size; i++) sum ^= data[i];
    return sum;
}

/* ═══════════════════════════════════════════════════════════════════════
 *  ORIC BASIC tokenizer — converts ASCII BASIC source to tokenized form
 * ═══════════════════════════════════════════════════════════════════════ */

/* ORIC BASIC 1.0 keyword table (token value = 0x80 + index) */
/* Token table extracted from ORIC BASIC 1.0 ROM at $C0EA.
 * Keywords stored with bit 7 set on last character.
 * Token value = 0x80 + array index. */
static const char* const basic_keywords[] = {
    "END",      /* $80 */  "EDIT",     /* $81 */  "INVERSE",  /* $82 */
    "NORMAL",   /* $83 */  "TRON",     /* $84 */  "TROFF",    /* $85 */
    "POP",      /* $86 */  "PLOT",     /* $87 */  "PULL",     /* $88 */
    "LORES",    /* $89 */  "DOKE",     /* $8A */  "REPEAT",   /* $8B */
    "UNTIL",    /* $8C */  "FOR",      /* $8D */  "LLIST",    /* $8E */
    "LPRINT",   /* $8F */  "NEXT",     /* $90 */  "DATA",     /* $91 */
    "INPUT",    /* $92 */  "DIM",      /* $93 */  "CLS",      /* $94 */
    "READ",     /* $95 */  "LET",      /* $96 */  "GOTO",     /* $97 */
    "RUN",      /* $98 */  "IF",       /* $99 */  "RESTORE",  /* $9A */
    "GOSUB",    /* $9B */  "RETURN",   /* $9C */  "REM",      /* $9D */
    "HIMEM",    /* $9E */  "GRAB",     /* $9F */  "RELEASE",  /* $A0 */
    "TEXT",     /* $A1 */  "HIRES",    /* $A2 */  "SHOOT",    /* $A3 */
    "EXPLODE",  /* $A4 */  "ZAP",      /* $A5 */  "PING",     /* $A6 */
    "SOUND",    /* $A7 */  "MUSIC",    /* $A8 */  "PLAY",     /* $A9 */
    "CURSET",   /* $AA */  "CURMOV",   /* $AB */  "DRAW",     /* $AC */
    "CIRCLE",   /* $AD */  "PATTERN",  /* $AE */  "FILL",     /* $AF */
    "CHAR",     /* $B0 */  "PAPER",    /* $B1 */  "INK",      /* $B2 */
    "STOP",     /* $B3 */  "ON",       /* $B4 */  "WAIT",     /* $B5 */
    "CLOAD",    /* $B6 */  "CSAVE",    /* $B7 */  "DEF",      /* $B8 */
    "POKE",     /* $B9 */  "PRINT",    /* $BA */  "CONT",     /* $BB */
    "LIST",     /* $BC */  "CLEAR",    /* $BD */  "GET",      /* $BE */
    "CALL",     /* $BF */  "!",        /* $C0 */  "NEW",      /* $C1 */
    "TAB(",     /* $C2 */  "TO",       /* $C3 */  "FN",       /* $C4 */
    "SPC(",     /* $C5 */  "@",        /* $C6 */  "AUTO",     /* $C7 */
    "ELSE",     /* $C8 */  "THEN",     /* $C9 */  "NOT",      /* $CA */
    "STEP",     /* $CB */  "+",        /* $CC */  "-",        /* $CD */
    "*",        /* $CE */  "/",        /* $CF */  "^",        /* $D0 */
    "AND",      /* $D1 */  "OR",       /* $D2 */  ">",        /* $D3 */
    "=",        /* $D4 */  "<",        /* $D5 */  "SGN",      /* $D6 */
    "INT",      /* $D7 */  "ABS",      /* $D8 */  "USR",      /* $D9 */
    "FRE",      /* $DA */  "POS",      /* $DB */  "HEX$",     /* $DC */
    "&",        /* $DD */  "SQR",      /* $DE */  "RND",      /* $DF */
    "LN",       /* $E0 */  "EXP",      /* $E1 */  "COS",      /* $E2 */
    "SIN",      /* $E3 */  "TAN",      /* $E4 */  "ATN",      /* $E5 */
    "PEEK",     /* $E6 */  "DEEK",     /* $E7 */  "LOG",      /* $E8 */
    "LEN",      /* $E9 */  "STR$",     /* $EA */  "VAL",      /* $EB */
    "ASC",      /* $EC */  "CHR$",     /* $ED */  "PI",       /* $EE */
    "TRUE",     /* $EF */  "FALSE",    /* $F0 */  "KEY$",     /* $F1 */
    "SCRN",     /* $F2 */  "POINT",    /* $F3 */  "LEFT$",    /* $F4 */
    "RIGHT$",   /* $F5 */  "MID$",     /* $F6 */  "GO",       /* $F7 */
    NULL
};

#define BASIC_TOKEN_BASE 0x80
#define BASIC_START_ADDR 0x0501
#define BASIC_MAX_SIZE   32768

/**
 * @brief Try to match a BASIC keyword at the current position
 * @param src  Source text pointer
 * @param[out] token  Token value if matched
 * @return Length of matched keyword, or 0 if no match
 */
static int basic_match_keyword(const char* src, uint8_t* token)
{
    /* Try longest match first — scan all keywords */
    int best_len = 0;
    int best_idx = -1;

    for (int i = 0; basic_keywords[i] != NULL; i++) {
        int klen = (int)strlen(basic_keywords[i]);
        if (klen <= best_len) continue;

        /* Case-insensitive compare */
        bool match = true;
        for (int j = 0; j < klen; j++) {
            char sc = src[j];
            char kc = basic_keywords[i][j];
            if (sc >= 'a' && sc <= 'z') sc = (char)(sc - 32);
            if (sc != kc) { match = false; break; }
        }
        if (match) {
            /* For alpha keywords, ensure next char is not alphanumeric
             * (avoids matching "TO" inside "TOTAL") */
            if (klen > 1 && basic_keywords[i][0] >= 'A') {
                char next = src[klen];
                if ((next >= 'A' && next <= 'Z') ||
                    (next >= 'a' && next <= 'z') ||
                    (next >= '0' && next <= '9') ||
                    next == '$') {
                    continue;  /* Partial match — skip */
                }
            }
            best_len = klen;
            best_idx = i;
        }
    }

    if (best_idx >= 0) {
        *token = (uint8_t)(BASIC_TOKEN_BASE + best_idx);
        return best_len;
    }
    return 0;
}

/**
 * @brief Tokenize one line of BASIC source
 * @param src    Source line (without line number, null-terminated)
 * @param out    Output buffer for tokenized data
 * @param maxlen Max output size
 * @return Number of bytes written to out
 */
static int basic_tokenize_line(const char* src, uint8_t* out, int maxlen)
{
    int opos = 0;
    int spos = 0;
    bool in_string = false;
    bool after_rem = false;

    while (src[spos] != '\0' && opos < maxlen - 1) {
        char c = src[spos];

        /* Inside a string literal — copy verbatim */
        if (in_string) {
            out[opos++] = (uint8_t)c;
            if (c == '"') in_string = false;
            spos++;
            continue;
        }

        /* After REM — copy rest of line verbatim */
        if (after_rem) {
            out[opos++] = (uint8_t)c;
            spos++;
            continue;
        }

        /* Start of string */
        if (c == '"') {
            in_string = true;
            out[opos++] = (uint8_t)c;
            spos++;
            continue;
        }

        /* Try keyword match (only for uppercase or matching chars) */
        uint8_t token;
        int klen = basic_match_keyword(&src[spos], &token);
        if (klen > 0) {
            out[opos++] = token;
            spos += klen;
            /* If REM token, copy rest verbatim */
            if (token == 0x9D) after_rem = true;
            continue;
        }

        /* Regular character — copy as-is (uppercase letters) */
        if (c >= 'a' && c <= 'z') {
            out[opos++] = (uint8_t)(c - 32);  /* Uppercase */
        } else {
            out[opos++] = (uint8_t)c;
        }
        spos++;
    }

    return opos;
}

bool tap_from_basic(const char* basic_file, const char* tap_filename, bool auto_run) {
    FILE* fp = fopen(basic_file, "r");
    if (!fp) return false;

    uint8_t* mem = (uint8_t*)calloc(1, BASIC_MAX_SIZE);
    if (!mem) { fclose(fp); return false; }

    int pos = 0;
    char line[1024];
    uint8_t tok_buf[1024];

    while (fgets(line, (int)sizeof(line), fp)) {
        /* Strip trailing newline/CR */
        int len = (int)strlen(line);
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r'))
            line[--len] = '\0';
        if (len == 0) continue;

        /* Parse line number */
        char* p = line;
        while (*p == ' ') p++;
        int linenum = 0;
        while (*p >= '0' && *p <= '9') {
            linenum = linenum * 10 + (*p - '0');
            p++;
        }
        if (linenum == 0) continue;  /* Skip lines without number */

        /* Skip space after line number */
        if (*p == ' ') p++;

        /* ── Build tokenized BASIC line ── */
        int line_start = pos;

        /* Next-line pointer placeholder (2 bytes, little-endian) */
        mem[pos++] = 0;
        mem[pos++] = 0;

        /* Line number (2 bytes, little-endian) */
        mem[pos++] = (uint8_t)(linenum & 0xFF);
        mem[pos++] = (uint8_t)((linenum >> 8) & 0xFF);

        /* Tokenize line body */
        int tok_len = basic_tokenize_line(p, tok_buf, (int)sizeof(tok_buf));
        memcpy(&mem[pos], tok_buf, (size_t)tok_len);
        pos += tok_len;

        /* Line terminator */
        mem[pos++] = 0x00;

        /* Fix next-line pointer */
        uint16_t next_addr = (uint16_t)(BASIC_START_ADDR + pos);
        mem[line_start]     = (uint8_t)(next_addr & 0xFF);
        mem[line_start + 1] = (uint8_t)((next_addr >> 8) & 0xFF);

        if (pos >= BASIC_MAX_SIZE - 256) break;
    }
    fclose(fp);

    /* End-of-program marker */
    mem[pos++] = 0x00;
    mem[pos++] = 0x00;

    /* ── Write TAP file ── */
    uint16_t end_addr = (uint16_t)(BASIC_START_ADDR + pos - 1);

    tap_file_t* tap = tap_open_write(tap_filename);
    if (!tap) { free(mem); return false; }

    tap_header_t header;
    memset(&header, 0, sizeof(header));
    header.sync_len = 3;
    header.type = TAP_BASIC;
    header.auto_run = auto_run ? 0x80 : 0x00;
    header.start_addr = BASIC_START_ADDR;
    header.end_addr = end_addr;

    /* Extract name from filename */
    const char* base = strrchr(basic_file, '/');
    base = base ? base + 1 : basic_file;
    strncpy(header.name, base, TAP_NAME_LEN - 1);
    char* dot = strrchr(header.name, '.');
    if (dot) *dot = '\0';
    /* Uppercase the name */
    for (int i = 0; header.name[i]; i++) {
        if (header.name[i] >= 'a' && header.name[i] <= 'z')
            header.name[i] = (char)(header.name[i] - 32);
    }

    tap_write_header(tap, &header);
    tap_write_data(tap, mem, (size_t)pos);

    tap_close(tap);
    free(mem);
    return true;
}

bool tap_from_binary(const char* bin_file, const char* tap_filename,
                    uint16_t start_addr, uint16_t exec_addr, const char* name) {
    FILE* fp = fopen(bin_file, "rb");
    if (!fp) return false;

    fseek(fp, 0, SEEK_END);
    long fsize = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    if (fsize <= 0 || fsize > 48000) { fclose(fp); return false; }

    uint8_t* data = (uint8_t*)malloc((size_t)fsize);
    if (!data) { fclose(fp); return false; }
    if (fread(data, 1, (size_t)fsize, fp) != (size_t)fsize) {
        free(data); fclose(fp); return false;
    }
    fclose(fp);

    tap_file_t* tap = tap_open_write(tap_filename);
    if (!tap) { free(data); return false; }

    tap_header_t header;
    memset(&header, 0, sizeof(header));
    header.sync_len = 3;
    header.type = TAP_MACHINE;
    /* Machine-code auto-run uses $C7 (CSAVE "...",A,E,J convention), not $80.
     * $80 is the BASIC auto-run flag and triggers a RUN on the loaded bytes —
     * on a machine-code program Oricutron then reports an ?ILLEGAL... error
     * because the binary is parsed as BASIC tokens. $C7 instructs the loader
     * to JMP start_addr after loading. */
    header.auto_run = (exec_addr != 0) ? 0xC7 : 0x00;
    header.start_addr = start_addr;
    header.end_addr = start_addr + (uint16_t)fsize - 1;
    if (name) strncpy(header.name, name, TAP_NAME_LEN - 1);

    tap_write_header(tap, &header);
    tap_write_data(tap, data, (size_t)fsize);

    tap_close(tap);
    free(data);
    return true;
}
