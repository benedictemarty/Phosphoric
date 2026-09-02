/**
 * @file test_jasmin.c
 * @brief Jasmin disk interface tests (WD177x FDC + .dsk write-back wiring)
 * @author bmarty <bmarty@mailo.com>
 *
 * Covers the fix reported by the bbsoric team: a guest sector write on a
 * Jasmin machine must (1) mark the Jasmin's own dirty flag and (2) be picked
 * up by the write-back path, which previously consulted only the Microdisc.
 * The write-back sites (osd_writeback_drive/control_writeback_drive/quit flush)
 * are static functions, but they now all delegate to the emu_disk_* helpers in
 * emulator.h — so testing those helpers + sedoric_save round-trip proves the
 * whole chain persists a Jasmin guest save byte-exact.
 */

#define _DEFAULT_SOURCE   /* mkstemp under -std=c11 -pedantic */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>

#include "emulator.h"
#include "io/jasmin.h"
#include "storage/sedoric.h"
#include "storage/disk.h"

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name) static void name(void)
#define RUN(name) do { \
    printf("  %-50s", #name); \
    name(); \
    tests_passed++; \
    printf("PASS\n"); \
} while(0)

#define ASSERT_EQ(a, b) do { \
    if ((a) != (b)) { \
        printf("FAIL\n    %s:%d: expected 0x%X, got 0x%X\n", __FILE__, __LINE__, (unsigned)(b), (unsigned)(a)); \
        tests_failed++; return; \
    } \
} while(0)

#define ASSERT_TRUE(x) do { \
    if (!(x)) { \
        printf("FAIL\n    %s:%d: expected true\n", __FILE__, __LINE__); \
        tests_failed++; return; \
    } \
} while(0)

#define ASSERT_FALSE(x) do { \
    if ((x)) { \
        printf("FAIL\n    %s:%d: expected false\n", __FILE__, __LINE__); \
        tests_failed++; return; \
    } \
} while(0)

/* ── Helper-level: the active interface's dirty flag is consulted ──────────
 * emu_disk_dirty/emu_disk_clear_dirty/emu_disk_max_drives must follow
 * has_jasmin, not hard-code the Microdisc. This is the exact predicate the
 * three write-back sites now gate on. */
TEST(test_emu_disk_helpers_follow_active_iface) {
    emulator_t* emu = calloc(1, sizeof(*emu));
    ASSERT_TRUE(emu != NULL);

    /* Jasmin machine: only jasmin.disk_dirty must count. */
    emu->has_jasmin = true;
    emu->has_microdisc = false;
    ASSERT_EQ(emu_disk_max_drives(emu), JASMIN_MAX_DRIVES);

    emu->microdisc.disk_dirty[2] = true;   /* stale Microdisc flag — must be ignored */
    ASSERT_FALSE(emu_disk_dirty(emu, 2));

    emu->jasmin.disk_dirty[2] = true;
    ASSERT_TRUE(emu_disk_dirty(emu, 2));
    ASSERT_FALSE(emu_disk_dirty(emu, 0));

    emu_disk_clear_dirty(emu, 2);
    ASSERT_FALSE(emu_disk_dirty(emu, 2));
    ASSERT_TRUE(emu->jasmin.disk_dirty[2] == false);

    /* Out-of-range guard. */
    ASSERT_FALSE(emu_disk_dirty(emu, -1));
    ASSERT_FALSE(emu_disk_dirty(emu, JASMIN_MAX_DRIVES));

    /* Microdisc machine: the Microdisc flag counts, Jasmin's is ignored. */
    emu->has_jasmin = false;
    emu->has_microdisc = true;
    ASSERT_EQ(emu_disk_max_drives(emu), MICRODISC_MAX_DRIVES);
    emu->jasmin.disk_dirty[1] = true;      /* leftover — must be ignored now */
    ASSERT_FALSE(emu_disk_dirty(emu, 1));
    emu->microdisc.disk_dirty[1] = true;
    ASSERT_TRUE(emu_disk_dirty(emu, 1));
    emu_disk_clear_dirty(emu, 1);
    ASSERT_FALSE(emu_disk_dirty(emu, 1));

    free(emu);
}

/* ── End-to-end: a guest sector write through the Jasmin FDC marks the Jasmin
 * dirty, mutates the shared emu->disks[]->data buffer, and survives a
 * sedoric_save/sedoric_load round-trip (the persistence the bbsoric sink needs).
 * The write is driven through jasmin_write() exactly as the CPU would, mirroring
 * test_fdc_write_sector but via the Jasmin register window. */
TEST(test_jasmin_guest_write_persists) {
    char path[] = "/tmp/phos_jasmin_wb_XXXXXX";
    int fd = mkstemp(path);
    ASSERT_TRUE(fd >= 0);
    close(fd);

    sedoric_disk_t* disk = sedoric_create();     /* standard Sedoric geometry */
    ASSERT_TRUE(disk != NULL);

    jasmin_t j;
    memset(&j, 0, sizeof(j));
    jasmin_init(&j);
    j.fdc.timing_mode = FDC_TIMING_FAST;         /* deterministic DRQ cadence */
    jasmin_set_disk(&j, 0, disk->data, disk->size, disk->tracks, disk->sectors);

    /* Position on track 0, sector 1 and issue a Write Sector ($A0). */
    j.fdc.c_track = 0;
    j.fdc.track = 0;
    j.fdc.sector = 1;
    jasmin_write(&j, JASMIN_FDC_BASE, 0xA0);
    ASSERT_EQ(j.fdc.currentop, FDC_OP_WRITE_SECTOR);

    fdc_ticktock(&j.fdc, 505);                   /* wait for first DRQ */
    ASSERT_EQ(j.drq, 0x00);                       /* DRQ active on the Jasmin line */

    for (int i = 0; i < 256; i++) {
        jasmin_write(&j, JASMIN_FDC_BASE + 3, (uint8_t)i);
        if (i < 255) fdc_ticktock(&j.fdc, 35);
    }
    ASSERT_EQ(j.fdc.currentop, FDC_OP_NONE);

    /* (1) Jasmin dirty flag set by jasmin_write's disk_modified consumption. */
    ASSERT_TRUE(j.disk_dirty[0]);
    /* (2) The write landed in the SAME buffer sedoric_save will flush. */
    ASSERT_EQ(disk->data[0], 0x00);
    ASSERT_EQ(disk->data[1], 0x01);
    ASSERT_EQ(disk->data[255], 0xFF);

    /* (3) Persist and reload: the guest save must be on the .dsk file. */
    ASSERT_TRUE(sedoric_save(disk, path));
    sedoric_disk_t* reloaded = sedoric_load(path);
    ASSERT_TRUE(reloaded != NULL);
    ASSERT_EQ(reloaded->data[1], 0x01);
    ASSERT_EQ(reloaded->data[255], 0xFF);

    sedoric_destroy(reloaded);
    sedoric_destroy(disk);
    unlink(path);
}

/* ── Hot-swap routing: emu_disk_wire() installs/ejects on the ACTIVE controller.
 * The OSD (F6/Suppr) and control (load-disk/eject-disk) hot-swap paths now route
 * through this helper, so on a Jasmin machine the media must land in the Jasmin
 * controller (not the Microdisc), and an eject (nd==NULL) must clear it. */
TEST(test_emu_disk_wire_routes_to_active_iface) {
    emulator_t* emu = calloc(1, sizeof(*emu));
    ASSERT_TRUE(emu != NULL);
    emu->has_jasmin = true;
    emu->has_microdisc = false;
    jasmin_init(&emu->jasmin);

    sedoric_disk_t* disk = sedoric_create();
    ASSERT_TRUE(disk != NULL);

    /* Install into drive B: the Jasmin drive-B slot must point at the buffer,
     * and the Microdisc must stay untouched. */
    emu_disk_wire(emu, 1, disk);
    ASSERT_TRUE(emu->jasmin.disk_data[1] == disk->data);
    ASSERT_EQ(emu->jasmin.disk_size[1], disk->size);
    ASSERT_TRUE(emu->microdisc.disk_data[1] == NULL);

    /* Eject: the Jasmin slot must be cleared. */
    emu_disk_wire(emu, 1, NULL);
    ASSERT_TRUE(emu->jasmin.disk_data[1] == NULL);
    ASSERT_EQ(emu->jasmin.disk_size[1], 0u);

    sedoric_destroy(disk);
    free(emu);
}

/* ── Bad-sector injection (--bad-sector on a Jasmin machine). The damage lives
 * with the media (per-drive map) and is synced into the live FDC map when the
 * target is the selected drive. Mirrors the Microdisc. */
TEST(test_jasmin_bad_sector_injection) {
    jasmin_t j;
    memset(&j, 0, sizeof(j));
    jasmin_init(&j);                 /* selects drive 0 */

    /* Inject on the selected drive: recorded in the media map AND synced live. */
    ASSERT_EQ(jasmin_add_bad_sector(&j, 0, 0, 10, 3), 0);
    ASSERT_EQ(j.bad_map[0].count, 1);
    ASSERT_EQ(j.bad_map[0].entry[0].track, 10);
    ASSERT_EQ(j.bad_map[0].entry[0].sector, 3);
    ASSERT_EQ(j.fdc.bad.count, 1);   /* live FDC map re-pointed (selected drive) */

    /* Inject on a non-selected drive: recorded on its media, live map untouched. */
    ASSERT_EQ(jasmin_add_bad_sector(&j, 2, 0, 5, 7), 0);
    ASSERT_EQ(j.bad_map[2].count, 1);
    ASSERT_EQ(j.fdc.bad.count, 1);   /* still drive 0's map under the head */

    /* Out-of-range drive is rejected. */
    ASSERT_EQ(jasmin_add_bad_sector(&j, JASMIN_MAX_DRIVES, 0, 1, 1), -1);
}

int main(void) {
    printf("Jasmin disk interface tests\n");
    printf("═══════════════════════════════════════════════════════\n");
    RUN(test_emu_disk_helpers_follow_active_iface);
    RUN(test_emu_disk_wire_routes_to_active_iface);
    RUN(test_jasmin_bad_sector_injection);
    RUN(test_jasmin_guest_write_persists);
    printf("═══════════════════════════════════════════════════════\n");
    printf("  %d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
