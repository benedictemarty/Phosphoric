/* SPDX-License-Identifier: EUPL-1.2 */
/**
 * @file fuzz_tap.c
 * @brief Fuzzing: .tap cassette → block reader (storage/tap.c)
 * @author bmarty <bmarty@mailo.com>
 *
 * Same sequence as inserting a tape in the web build (web_insert_tap):
 * header, then data from start to end, block after block.
 */
#include "fuzz_common.h"
#include "utils/logging.h"
#include "storage/tap.h"

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    FUZZ_QUIET_LOGS();
    tap_file_t* tap = tap_open_read(fuzz_tmpfile(data, size), true);
    if (!tap) return 0;
    static uint8_t buf[65536];
    for (int blk = 0; blk < 16 && !tap_eof(tap); blk++) {
        tap_header_t h;
        if (!tap_read_header(tap, &h)) break;
        uint16_t n = (uint16_t)(h.end_addr - h.start_addr + 1);
        if (tap_read_data(tap, buf, n ? n : 1) <= 0) break;
    }
    (void)tap_tell(tap);
    tap_close(tap);
    return 0;
}
