/* SPDX-License-Identifier: EUPL-1.2 */
/**
 * @file fuzz_hfe.c
 * @brief Fuzzing : image .dsk (MFM_DISK) → dsk2hfe_convert (outil dsk2hfe)
 * @author bmarty <bmarty@mailo.com>
 *
 * L'outil est inclus tel quel, sans son main() (DSK2HFE_NO_MAIN) ; l'image HFE
 * produite part dans /dev/null : on cherche les lectures hors du .dsk.
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
