/* SPDX-License-Identifier: EUPL-1.2 */
/**
 * @file fuzz_sym.c
 * @brief Fuzzing: symbol file (--symbols) → symbol_table_load
 * @author bmarty <bmarty@mailo.com>
 */
#include "fuzz_common.h"
#include "utils/logging.h"
#include "utils/symbols.h"

FUZZ_NO_SEED()

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    FUZZ_QUIET_LOGS();
    static symbol_table_t tbl;
    symbol_table_init(&tbl);
    (void)symbol_table_load(&tbl, fuzz_tmpfile(data, size));
    (void)symbol_lookup(&tbl, 0xF900);
    uint16_t a;
    (void)symbol_resolve(&tbl, "RESET", &a);
    return 0;
}
