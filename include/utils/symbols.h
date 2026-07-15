/**
 * @file symbols.h
 * @brief Symbol table for debugger — load .sym / .lab / .sym65 files
 * @author bmarty <bmarty@mailo.com>
 * @date 2026-06-06
 *
 * Auto-detected formats:
 *   Oricutron .sym   :  $E5BD .RDBYTE         (addr first)
 *                    :  RDBYTE = $E5BD        (name first, EQU)
 *   xa65 / VICE .lab :  al C E5BD .RDBYTE
 *   cc65 .sym65      :  rdbyte $E5BD          (lowercase, no prefix)
 *
 * Comments: lines starting with ';' or '#' are skipped.
 */

#ifndef SYMBOLS_H
#define SYMBOLS_H

#include <stdint.h>
#include <stdbool.h>

#define SYMBOL_MAX_COUNT 8192
#define SYMBOL_MAX_NAME  47

typedef struct {
    char     name[SYMBOL_MAX_NAME + 1];
    uint16_t addr;
    uint8_t  group;   /* symbol group 0-255 (US 4) — scope/enable by group */
} symbol_entry_t;

typedef struct {
    symbol_entry_t entries[SYMBOL_MAX_COUNT];
    int            count;
    bool           group_disabled[256];  /* US 4: per-group scope (0 = enabled, so a
                                           * zero-initialised table has all groups on) */
} symbol_table_t;

void        symbol_table_init(symbol_table_t* tbl);

/* Load symbols from FILE. Auto-detects format. Returns number of symbols
 * added (>=0) or -1 on file-open failure. Existing entries are preserved
 * (multiple loads accumulate). Loaded symbols are tagged with group 0. */
int         symbol_table_load(symbol_table_t* tbl, const char* path);

/* Like symbol_table_load but tags the loaded symbols with @p group (0-255),
 * so they can be scoped on/off independently (parity with b2 symbol groups). */
int         symbol_table_load_group(symbol_table_t* tbl, const char* path, uint8_t group);

/* Enable/disable a symbol group. Disabled groups are skipped by lookup/resolve. */
void        symbol_set_group_enabled(symbol_table_t* tbl, uint8_t group, bool enabled);
bool        symbol_group_enabled(const symbol_table_t* tbl, uint8_t group);

/* Number of symbols tagged with @p group. */
int         symbol_group_count(const symbol_table_t* tbl, uint8_t group);

/* Lookup name from address. Returns NULL if not found.
 * If multiple symbols share an address, returns the first inserted. */
const char* symbol_lookup(const symbol_table_t* tbl, uint16_t addr);

/* Resolve a name to an address. Returns true on hit. Case-insensitive.
 * Accepts an optional leading '.' or '_' which is stripped. */
bool        symbol_resolve(const symbol_table_t* tbl, const char* name, uint16_t* out);

#endif /* SYMBOLS_H */
