/* SPDX-License-Identifier: EUPL-1.2 */
/**
 * @file fuzz_smf.c
 * @brief Fuzzing: Standard MIDI File (.mid) → smf_parse (smf: transport)
 * @author bmarty <bmarty@mailo.com>
 */
#include "fuzz_common.h"
#include "io/smf.h"
#include <string.h>

FUZZ_NO_SEED()

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    smf_t s;
    memset(&s, 0, sizeof s);
    if (smf_parse(data, size, &s)) {
        for (size_t i = 0; i < s.count; i++) (void)smf_event_bytes(&s, i)[0];
    }
    smf_free(&s);
    return 0;
}
