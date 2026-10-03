/* SPDX-License-Identifier: EUPL-1.2 */
/**
 * @file fuzz_ost.c
 * @brief Fuzzing : sauvegarde d'état (.ost) → savestate_load
 * @author bmarty <bmarty@mailo.com>
 *
 * Machine minimale (même montage que tests/unit/test_savestate.c), Microdisc
 * présent pour que les sections MDC/FDC/DSK/BAD soient lues. La graine est
 * l'état de cette machine sauvegardé (fuzz_make_seed).
 *
 * L'en-tête (signature, version, CRC32 des données) est recalculé sur chaque
 * entrée : sans cela, presque toute mutation échouerait au CRC et les sections
 * ne seraient jamais analysées.
 */
#include "fuzz_common.h"
#include "utils/logging.h"
#include "emulator.h"
#include "savestate.h"
#include "storage/sedoric.h"
#include <string.h>

static emulator_t emu;

static uint32_t fuzz_crc32(const uint8_t* p, size_t n) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) {
        crc ^= p[i];
        for (int b = 0; b < 8; b++) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return crc ^ 0xFFFFFFFFu;
}

static void fuzz_emu_init(void) {
    memset(&emu, 0, sizeof emu);
    memory_init(&emu.memory);
    cpu_init(&emu.cpu, &emu.memory);
    via_init(&emu.via);
    ay_init(&emu.psg, 1000000);
    video_init(&emu.video);
    oric_keyboard_init(&emu.keyboard);
    microdisc_init(&emu.microdisc);
    emu.tape_syncstack = -1;
    emu.card_on[CARD_IDX_microdisc] = true;
}

int fuzz_make_seed(const char* path) {
    fuzz_emu_init();
    /* Une petite disquette dans A: pour que la graine porte une section DSK. */
    emu.disks[0] = sedoric_create_blank(3, 1);
    if (!emu.disks[0]) return 1;
    microdisc_set_disk(&emu.microdisc, 0, emu.disks[0]->data, emu.disks[0]->size,
                       emu.disks[0]->tracks, emu.disks[0]->sectors);
    int rc = savestate_save(&emu, path) ? 0 : 1;
    sedoric_destroy(emu.disks[0]);
    emu.disks[0] = NULL;
    return rc;
}

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    FUZZ_QUIET_LOGS();
    fuzz_emu_init();
    if (size < SAVESTATE_HEADER_SIZE) {
        (void)savestate_load(&emu, fuzz_tmpfile(data, size));
    } else {
        uint8_t* buf = malloc(size);
        if (!buf) return 0;
        memcpy(buf, data, size);
        memcpy(buf, SAVESTATE_MAGIC, 4);
        buf[4] = SAVESTATE_VERSION; buf[5] = buf[6] = buf[7] = 0;
        uint32_t crc = fuzz_crc32(buf + SAVESTATE_HEADER_SIZE, size - SAVESTATE_HEADER_SIZE);
        for (int i = 0; i < 4; i++) buf[12 + i] = (uint8_t)(crc >> (8 * i));
        (void)savestate_load(&emu, fuzz_tmpfile(buf, size));
        free(buf);
    }
    for (int i = 0; i < MICRODISC_MAX_DRIVES; i++) {   /* disques créés par la section DSK */
        if (emu.disks[i]) { free(emu.disks[i]->data); free(emu.disks[i]); }
    }
    memory_cleanup(&emu.memory);
    return 0;
}
