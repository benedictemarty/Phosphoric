/* SPDX-License-Identifier: EUPL-1.2 */
/**
 * @file fuzz_tapenat.c
 * @brief Fuzzing: native tape loading (CLOAD through the patched ROM,
 *        tape_patches.c)
 * @author bmarty <bmarty@mailo.com>
 *
 * Without ROM: we play what the ROM does during a CLOAD by putting the PC on
 * the patched entry points (sync search, byte read, sync recovery) and
 * tape_patches() reads the fuzzed tape, until it is exhausted and beyond.
 * Byte 0: model (bit 0: Atmos / Oric-1); the rest: the .tap. The tape is
 * copied into an exactly-sized buffer: any read past it is seen by ASan.
 */
#include "fuzz_common.h"
#include "utils/logging.h"
#include "emulator.h"
#include "rom_patches.h"
#include "io/tape_patches.h"
#include <string.h>

FUZZ_NO_SEED()

static emulator_t emu;

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    FUZZ_QUIET_LOGS();
    if (size < 1) return 0;
    static bool ready;
    if (!ready) { memory_init(&emu.memory); ready = true; }
    const rom_patches_t* p = get_rom_patches((data[0] & 1) ? ORIC_MODEL_ATMOS : ORIC_MODEL_ORIC1);
    if (!p) return 0;
    emu.rom_patches = p;
    emu.tapelen = (int)(size - 1);
    emu.tapebuf = (uint8_t*)malloc(size > 1 ? size - 1 : 1);
    if (!emu.tapebuf) return 0;
    memcpy(emu.tapebuf, data + 1, size - 1);
    emu.tapeoffs = 0;
    emu.tape_loaded = true;
    emu.tape_syncstack = -1;
    emu.cpu.SP = 0xFF;

    /* Sync, 12 bytes, sync recovery… while the tape has bytes, then a few
     * more rounds on an exhausted tape. */
    int extra = 8;
    for (int step = 0; step < 20000 && emu.tape_loaded && extra > 0; step++) {
        if (emu.tapeoffs >= emu.tapelen) extra--;
        emu.cpu.PC = p->getsync_entry;
        tape_patches(&emu);
        for (int k = 0; k < 12; k++) {
            emu.cpu.PC = p->readbyte_entry;
            tape_patches(&emu);
        }
        emu.cpu.PC = p->getsync_loop;
        tape_patches(&emu);
    }
    free(emu.tapebuf);
    emu.tapebuf = NULL;
    return 0;
}
