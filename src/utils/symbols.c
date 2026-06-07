/**
 * @file symbols.c
 * @brief Symbol table loader for the debugger
 * @author bmarty <bmarty@mailo.com>
 * @date 2026-06-06
 */

#include "utils/symbols.h"
#include "utils/logging.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

void symbol_table_init(symbol_table_t* tbl) {
    tbl->count = 0;
}

/* Parse a hex token of form `$XXXX`, `0xXXXX`, `0XXXXX` or bare `XXXX`.
 * Returns the TOTAL number of characters consumed (prefix + digits), so
 * the caller can use `p + n` directly to advance past the token —
 * regardless of which prefix (if any) was present. Previously Format A
 * hardcoded `+ 1` for `$` and missed `0x`, causing the addr-first parser
 * to silently reject lines like `0xF000 reset`. */
static int hex_parse(const char* s, uint16_t* out) {
    if (!s || !*s) return 0;
    const char* start = s;
    if (*s == '$') s++;
    else if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
    char* end = NULL;
    unsigned long v = strtoul(s, &end, 16);
    if (end == s || v > 0xFFFF) return 0;
    *out = (uint16_t)v;
    return (int)(end - start);
}

/* Skip leading whitespace; return pointer to first non-space char. */
static char* lstrip(char* s) {
    while (*s && isspace((unsigned char)*s)) s++;
    return s;
}

/* Trim trailing whitespace / newline in place. */
static void rstrip(char* s) {
    size_t n = strlen(s);
    while (n > 0 && isspace((unsigned char)s[n - 1])) s[--n] = '\0';
}

/* Copy a name token (alnum + '_' + '.') into buf with sanitization.
 * Leading '.' or '_' is dropped. Returns length copied. */
static int copy_name(char* buf, const char* src, size_t maxlen) {
    if (*src == '.' || *src == '_') src++;
    size_t i = 0;
    while (src[i] && (isalnum((unsigned char)src[i]) ||
                      src[i] == '_' || src[i] == '.') && i < maxlen) {
        buf[i] = src[i];
        i++;
    }
    buf[i] = '\0';
    return (int)i;
}

static void add_entry(symbol_table_t* tbl, const char* name, uint16_t addr) {
    if (tbl->count >= SYMBOL_MAX_COUNT) return;
    if (!name || !*name) return;
    symbol_entry_t* e = &tbl->entries[tbl->count++];
    strncpy(e->name, name, SYMBOL_MAX_NAME);
    e->name[SYMBOL_MAX_NAME] = '\0';
    e->addr = addr;
}

/* Try to parse a single line. Returns true if a symbol was added. */
static bool parse_line(symbol_table_t* tbl, char* line) {
    /* Strip comments after ';' (but keep names that contain '.'). */
    char* semi = strchr(line, ';');
    if (semi) *semi = '\0';
    char* hash = strchr(line, '#');
    if (hash) *hash = '\0';

    char* p = lstrip(line);
    rstrip(p);
    if (!*p) return false;

    char name[SYMBOL_MAX_NAME + 1];
    uint16_t addr = 0;

    /* Format C: "al C XXXX .NAME" — VICE / xa65 .lab style */
    if (p[0] == 'a' && p[1] == 'l' && isspace((unsigned char)p[2])) {
        char* q = lstrip(p + 2);
        /* Optional bank letter */
        if (isalpha((unsigned char)q[0]) && isspace((unsigned char)q[1]))
            q = lstrip(q + 1);
        int n = hex_parse(q, &addr);
        if (n > 0) {
            q = lstrip(q + n);
            if (copy_name(name, q, SYMBOL_MAX_NAME) > 0) {
                add_entry(tbl, name, addr);
                return true;
            }
        }
        return false;
    }

    /* Format A: "[$]XXXX NAME"  — addr-first.
     * Only valid if the hex token is followed by whitespace/separator,
     * NOT by another alphanumeric (else "CLOAD_END" would parse as $C). */
    if (*p == '$' || isxdigit((unsigned char)*p)) {
        const char* save = p;
        int n = hex_parse(p, &addr);
        if (n > 0) {
            const char* after = p + n;   /* hex_parse returns total consumed */
            if (*after == '\0' || isspace((unsigned char)*after) ||
                *after == ',' || *after == ':' || *after == '=') {
                char* q = lstrip((char*)after);
                while (*q == ',' || *q == ':' || *q == '=') q = lstrip(q + 1);
                if (copy_name(name, q, SYMBOL_MAX_NAME) > 0) {
                    add_entry(tbl, name, addr);
                    return true;
                }
            }
        }
        p = (char*)save;
    }

    /* Format B / D: "NAME = $XXXX"  or  "NAME $XXXX"  or  "NAME EQU $XXXX" */
    if (isalpha((unsigned char)*p) || *p == '_' || *p == '.') {
        int nlen = copy_name(name, p, SYMBOL_MAX_NAME);
        if (nlen <= 0) return false;
        char* q = p + nlen + ((*p == '.' || *p == '_') ? 1 : 0);
        q = lstrip(q);
        /* Skip "=" or "EQU" */
        if (*q == '=') q = lstrip(q + 1);
        else if ((q[0] == 'E' || q[0] == 'e') &&
                 (q[1] == 'Q' || q[1] == 'q') &&
                 (q[2] == 'U' || q[2] == 'u')) q = lstrip(q + 3);
        else if (*q == ':') q = lstrip(q + 1);
        if (hex_parse(q, &addr) > 0) {
            add_entry(tbl, name, addr);
            return true;
        }
    }

    return false;
}

int symbol_table_load(symbol_table_t* tbl, const char* path) {
    FILE* fp = fopen(path, "r");
    if (!fp) {
        log_error("symbols: cannot open %s", path);
        return -1;
    }
    int before = tbl->count;
    char line[512];
    while (fgets(line, sizeof(line), fp)) {
        parse_line(tbl, line);
    }
    fclose(fp);
    int added = tbl->count - before;
    log_info("symbols: %d entries loaded from %s (total %d)", added, path, tbl->count);
    return added;
}

const char* symbol_lookup(const symbol_table_t* tbl, uint16_t addr) {
    for (int i = 0; i < tbl->count; i++) {
        if (tbl->entries[i].addr == addr)
            return tbl->entries[i].name;
    }
    return NULL;
}

static int ci_cmp(const char* a, const char* b) {
    while (*a && *b) {
        int ca = tolower((unsigned char)*a++);
        int cb = tolower((unsigned char)*b++);
        if (ca != cb) return ca - cb;
    }
    return *a - *b;
}

bool symbol_resolve(const symbol_table_t* tbl, const char* name, uint16_t* out) {
    if (!name || !*name) return false;
    if (*name == '.' || *name == '_') name++;
    for (int i = 0; i < tbl->count; i++) {
        if (ci_cmp(tbl->entries[i].name, name) == 0) {
            *out = tbl->entries[i].addr;
            return true;
        }
    }
    return false;
}
