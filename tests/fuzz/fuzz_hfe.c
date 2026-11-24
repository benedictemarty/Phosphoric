/* SPDX-License-Identifier: EUPL-1.2 */
/**
 * @file fuzz_hfe.c
 * @brief Fuzzing: .dsk image (MFM_DISK) → dsk2hfe_convert (dsk2hfe tool)
 * @author bmarty <bmarty@mailo.com>
 *
 * The tool is included as is, without its main() (DSK2HFE_NO_MAIN); the HFE
 * image produced goes to /dev/null: we are looking for reads outside the .dsk.
 */
#define DSK2HFE_NO_MAIN
#include "../../tools/dsk2hfe.c"
#include "fuzz_common.h"

FUZZ_NO_SEED()

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    static FILE* sink;
    if (!sink) sink = fopen("/dev/null", "wb");
    if (!sink) return 0;
    char err[160];
    (void)dsk2hfe_convert(data, size, 250, 300, sink, err, sizeof(err));
    return 0;
}
