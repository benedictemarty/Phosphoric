/* SPDX-License-Identifier: EUPL-1.2 */
/**
 * @file fuzz_disk.c
 * @brief Fuzzing: disk image (.dsk MFM_DISK or raw) → sedoric_load
 * @author bmarty <bmarty@mailo.com>
 *
 * Loads the image as the Microdisc and the Jasmin do, reads every sector of
 * the announced geometry, then a few sectors through the FDC registers
 * (geometry inconsistent with the size: read past the image), and finally
 * writes it back (re-injection into the MFM container, like --disk-writeback).
 */
#include "fuzz_common.h"
#include "utils/logging.h"
#include "storage/sedoric.h"
#include "storage/disk.h"

FUZZ_NO_SEED()

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    FUZZ_QUIET_LOGS();
    sedoric_disk_t* d = sedoric_load(fuzz_tmpfile(data, size));
    if (!d) return 0;

    uint8_t sec[256];
    for (unsigned t = 0; t < d->tracks && t < 100; t++)
        for (unsigned s = 1; s <= d->sectors && s <= 32; s++)
            (void)sedoric_read_sector(d, (uint8_t)t, (uint8_t)s, sec);

    fdc_t fdc;
    fdc_init(&fdc);
    fdc_set_disk(&fdc, d->data, d->size);
    fdc.tracks = d->tracks;
    fdc.sectors_per_track = d->sectors;
    const uint8_t tracks[] = { 0, 1, (uint8_t)(d->tracks ? d->tracks - 1 : 0), d->tracks };
    for (unsigned i = 0; i < sizeof tracks; i++) {
        fdc_write(&fdc, 3, tracks[i]);          /* data register: target track */
        fdc_write(&fdc, 0, 0x10);               /* SEEK */
        fdc_ticktock(&fdc, 200000);
        for (unsigned s = 1; s <= (unsigned)d->sectors + 1 && s <= 33; s++) {
            fdc_write(&fdc, 2, (uint8_t)s);
            fdc_write(&fdc, 0, 0x80);           /* READ SECTOR */
            for (int n = 0; n < 300; n++) {
                fdc_ticktock(&fdc, 40);
                (void)fdc_read(&fdc, 3);
            }
            fdc_ticktock(&fdc, 5000);
        }
    }
    d->modified = true;
    (void)sedoric_save(d, fuzz_tmpfile(NULL, 0));
    sedoric_destroy(d);
    return 0;
}
