/**
 * @file loci_glue.c
 * @brief LOCI ↔ emulator adapter callbacks — moved verbatim from main.c (Epic 9).
 * @author bmarty <bmarty@mailo.com>
 */
#include "io/loci_glue.h"
#include "cpu/cpu6502.h"   /* cpu_irq_set/clear, IRQF_DISK, cpu_reset */
#include "memory/memory.h" /* memory_load_rom */
#include "utils/logging.h"
#include "savestate.h"      /* savestate_load (resume session) */
#include "rom_patches.h"  /* get_rom_patches (ROM swap) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>        /* access */
#include <dirent.h>        /* opendir/readdir/closedir (USB scan) */
#include <sys/stat.h>      /* stat */
#include "utils/oscompat.h" /* oscompat_statvfs */

void loci_dsk_cpu_irq_set(void* ctx) {
    emulator_t* emu = (emulator_t*)ctx;
    cpu_irq_set(&emu->cpu, IRQF_DISK);
}

void loci_dsk_cpu_irq_clr(void* ctx) {
    emulator_t* emu = (emulator_t*)ctx;
    cpu_irq_clear(&emu->cpu, IRQF_DISK);
}

void loci_dsk_sync_overlay(void* ctx, bool basic_disabled, bool overlay_active) {
    emulator_t* emu = (emulator_t*)ctx;
    emu->memory.basic_rom_disabled = basic_disabled;
    emu->memory.overlay_active     = overlay_active;
}

void loci_rom_poke_hook(void* ctx, uint16_t addr, uint8_t val) {
    emulator_t* emu = (emulator_t*)ctx;
    if (emu && addr >= 0xC000)
        emu->memory.rom[addr - 0xC000] = val;
}

/* --- Epic 9 / US3 : ROM / tape / resume (moved verbatim from main.c) --- */

/* Host path of the LOCI warm-session snapshot (firmware: the session is
 * captured so the menu can resume it). Lives in the flash root. */
void loci_resume_snapshot_path(emulator_t* emu, char* out, size_t outsz) {
    const char* root = emu->loci.flash_root[0] ? emu->loci.flash_root : ".";
    snprintf(out, outsz, "%s/loci_resume.ost", root);
}

/* Locate a LOCI system ROM by name: flash root (the internal storage,
 * where the firmware's LittleFS keeps them), then roms/loci/ next to the
 * loaded BASIC ROM (works from any CWD), then relative to the CWD. */
bool loci_find_rom_file(emulator_t* emu, const char* name,
                               char* out, size_t outsz) {
    const char* root = emu->loci.flash_root[0] ? emu->loci.flash_root : ".";
    snprintf(out, outsz, "%s/%s", root, name);
    if (access(out, R_OK) == 0) return true;
    if (emu->rom_path) {
        const char* slash = strrchr(emu->rom_path, '/');
        if (slash) {
            snprintf(out, outsz, "%.*s/loci/%s",
                     (int)(slash - emu->rom_path), emu->rom_path, name);
            if (access(out, R_OK) == 0) return true;
        }
    }
    snprintf(out, outsz, "roms/loci/%s", name);
    return access(out, R_OK) == 0;
}

/* Locate the LOCI menu ROM, mirroring the firmware's boot priority
 * (ext_boot_loci: locirom.rp6502 on USB → LOCIROM in internal flash →
 * embedded copy). The .rp6502 container is not parsed — raw 16 KB
 * images only; roms/loci/locirom is the repo's "embedded" copy. */
bool loci_find_menu_rom(emulator_t* emu, char* out, size_t outsz) {
    return loci_find_rom_file(emu, "LOCIROM", out, outsz) ||
           loci_find_rom_file(emu, "locirom", out, outsz);
}

/* Firmware ext_patch_version / ext_patch_timings: after a ROM lands at
 * $C000, patch the placeholder bytes the LOCI menu ROM reserves for the
 * firmware version (VERSIONS segment, $FFF7-9 = F0 F1 F2) and the current
 * bus timings (TIMINGS segment, $FFEF-F3 = FA FB FC FD FE). Placeholder
 * guards mean BASIC ROMs pass through untouched. */
void loci_patch_rom_info(emulator_t* emu) {
    uint8_t* rom = emu->memory.rom;
    if (rom[0x3FF7] == 0xF0 && rom[0x3FF8] == 0xF1 && rom[0x3FF9] == 0xF2) {
        rom[0x3FF7] = LOCI_FW_VERSION_PATCH;
        rom[0x3FF8] = LOCI_FW_VERSION_MINOR;
        rom[0x3FF9] = LOCI_FW_VERSION_MAJOR;
    }
    if (rom[0x3FEF] == 0xFA && rom[0x3FF0] == 0xFB && rom[0x3FF1] == 0xFC &&
        rom[0x3FF2] == 0xFD && rom[0x3FF3] == 0xFE) {
        rom[0x3FEF] = emu->loci.mia_tmap;
        rom[0x3FF0] = emu->loci.mia_tior;
        rom[0x3FF1] = emu->loci.mia_tiow;
        rom[0x3FF2] = emu->loci.mia_tiod;
        rom[0x3FF3] = emu->loci.mia_tadr;
        log_info("LOCI: menu ROM patched (FW %d.%d.%d, timings %u/%u/%u/%u/%u)",
                 LOCI_FW_VERSION_MAJOR, LOCI_FW_VERSION_MINOR, LOCI_FW_VERSION_PATCH,
                 emu->loci.mia_tmap, emu->loci.mia_tior, emu->loci.mia_tiow,
                 emu->loci.mia_tiod, emu->loci.mia_tadr);
    }
}

/* Sprint 34ao: LOCI tape-mount hook. Loads a TAP into emu.tapebuf so
 * the CLOAD ROM patches find data. Path is the already-extracted
 * /tmp/loci_extract_* file produced by sdimg_extract_to_temp. */
bool loci_tape_mount_cb(void* ctx, const char* host_tape_path) {
    emulator_t* emu = (emulator_t*)ctx;
    if (!emu || !host_tape_path) return false;
    FILE* f = fopen(host_tape_path, "rb");
    if (!f) {
        log_warning("LOCI tape mount: cannot open %s", host_tape_path);
        return false;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) { fclose(f); return false; }

    if (emu->tapebuf) { free(emu->tapebuf); emu->tapebuf = NULL; }
    emu->tapebuf = (uint8_t*)malloc((size_t)sz);
    if (!emu->tapebuf) { fclose(f); return false; }
    size_t rd = fread(emu->tapebuf, 1, (size_t)sz, f);
    fclose(f);
    if ((long)rd != sz) {
        free(emu->tapebuf); emu->tapebuf = NULL;
        return false;
    }
    emu->tapelen = (int)sz;
    emu->tapeoffs = 0;
    emu->tape_loaded = true;
    emu->tape_syncstack = -1;
    /* Do NOT trigger auto-CLOAD here: when LOCI mounts the tape, the
     * LOCI ROM is still in control. Auto-typed keystrokes would land
     * in the LOCI TUI, not BASIC. The user will type CLOAD"" after
     * MIA_BOOT swaps in BASIC. */
    emu->tape_auto_cload_pending = false;
    log_info("LOCI tape mount: %s buffered (%ld bytes, type CLOAD\"\" in BASIC)",
             host_tape_path, sz);
    return true;
}

bool loci_rom_swap_cb(void* ctx, const char* rom_path, uint16_t base_addr) {
    emulator_t* emu = (emulator_t*)ctx;
    if (!emu || !rom_path || !*rom_path) return false;

    if (base_addr == 0xA000) {
        /* Sprint 34aw : LOCI MIA_BOOT FDC flag → microdis.rom overlay.
         * Le mapping réel Microdisc place l'overlay à $E000-$FFFF (8 KB).
         * On charge le fichier dans un buffer persistant et on active
         * l'overlay du système mémoire (même mécanisme que Microdisc
         * card avec --disk-rom). */
        FILE* fp = fopen(rom_path, "rb");
        if (!fp) {
            log_error("LOCI ROM swap: cannot open %s", rom_path);
            return false;
        }
        fseek(fp, 0, SEEK_END);
        long sz = ftell(fp);
        fseek(fp, 0, SEEK_SET);
        if (sz <= 0 || sz > 16384) { fclose(fp); return false; }
        /* Sprint 34c hardening : buffer owned by emulator_t now (was a
         * function-scope static with an "acceptable leak at shutdown"
         * comment). Freed by emulator_cleanup. */
        if (emu->loci_overlay_buf) {
            free(emu->loci_overlay_buf);
            emu->loci_overlay_buf = NULL;
        }
        emu->loci_overlay_buf = (uint8_t*)malloc((size_t)sz);
        if (!emu->loci_overlay_buf) { fclose(fp); return false; }
        if (fread(emu->loci_overlay_buf, 1, (size_t)sz, fp) != (size_t)sz) {
            free(emu->loci_overlay_buf); emu->loci_overlay_buf = NULL;
            fclose(fp);
            return false;
        }
        fclose(fp);
        emu->memory.overlay_rom         = emu->loci_overlay_buf;
        emu->memory.overlay_rom_size    = (uint32_t)sz;
        emu->memory.overlay_active      = true;
        emu->memory.basic_rom_disabled  = true;   /* romdis = ROM disable signal */
        log_info("LOCI ROM swap: microdisc overlay activated ($E000+, %ld bytes from %s)",
                 sz, rom_path);
        /* Re-reset CPU so $FFFC reset vector is fetched from Microdisc
         * overlay instead of the BASIC ROM loaded in the prior $C000 call. */
        cpu_reset(&emu->cpu);
        return true;
    }

    if (base_addr != 0xC000) {
        log_info("LOCI ROM swap: ignored base $%04X (only $C000 / $A000 supported)",
                 base_addr);
        return true;
    }
    log_info("LOCI ROM swap: loading %s at $C000", rom_path);
    /* Firmware bootstrap seeds basic11b.rom/basic10.rom/microdis.rom into
     * its internal LittleFS; our flash root may not carry them. Fall back
     * to the directory of the -r ROM (the repo's roms/) so the menu's
     * "boot Atmos / Oric-1" entries work without --loci-flash tweaking. */
    char fallback[512];
    const char* load_path = rom_path;
    if (access(rom_path, R_OK) != 0 && emu->rom_path) {
        const char* dirend = strrchr(emu->rom_path, '/');
        const char* base = strrchr(rom_path, '/');
        base = base ? base + 1 : rom_path;
        if (dirend) {
            snprintf(fallback, sizeof(fallback), "%.*s/%s",
                     (int)(dirend - emu->rom_path), emu->rom_path, base);
            if (access(fallback, R_OK) == 0) {
                log_info("LOCI ROM swap: %s not in flash root, using %s",
                         base, fallback);
                load_path = fallback;
            }
        }
    }
    if (!memory_load_rom(&emu->memory, load_path, 0)) {
        log_error("LOCI ROM swap: failed to load %s", load_path);
        return false;
    }
    /* Any successful $C000 swap unmaps the menu (MIA_BOOT into BASIC,
     * resume...). The warm-boot path re-arms the flag right after. */
    emu->loci_menu_active = false;
    /* Firmware behaviour: version + timing bytes are patched into the
     * freshly loaded ROM (only the LOCI menu ROM has the placeholders). */
    loci_patch_rom_info(emu);
    /* Sprint 34ao: when LOCI swaps to BASIC 1.1 (Atmos) the previous
     * BASIC 1.0 CLOAD patches no longer match — re-detect from the
     * filename so cassette interception keeps working. */
    const char* base = strrchr(rom_path, '/');
    base = base ? base + 1 : rom_path;
    bool is_b11 = (strstr(base, "11") != NULL) ||
                  (strstr(base, "atmos") != NULL) ||
                  (strstr(base, "ATMOS") != NULL);
    const rom_patches_t* new_patches = get_rom_patches(
        is_b11 ? ORIC_MODEL_ATMOS : ORIC_MODEL_ORIC1);
    if (new_patches != emu->rom_patches) {
        emu->rom_patches = new_patches;
        emu->model = is_b11 ? ORIC_MODEL_ATMOS : ORIC_MODEL_ORIC1;
        log_info("LOCI ROM swap: patches → %s", emu->rom_patches->name);
    }
    /* Reset the 6502 so it re-reads the new $FFFC reset vector. */
    cpu_reset(&emu->cpu);
    return true;
}

/* LOCI session-resume callback (menu "resume" entry → MIA_BOOT with
 * LOCI_BOOT_RESUME): swap the pre-warm BASIC ROM back, then restore the
 * snapshot taken when the Action button was pressed. */
bool loci_resume_session_cb(void* ctx) {
    emulator_t* emu = (emulator_t*)ctx;
    if (!emu) return false;
    char snap[512];
    loci_resume_snapshot_path(emu, snap, sizeof(snap));
    if (access(snap, R_OK) != 0) return false;
    if (emu->rom_path && !loci_rom_swap_cb(emu, emu->rom_path, 0xC000))
        return false;
    if (!savestate_load(emu, snap)) return false;
    emu->loci_menu_active = false;
    log_info("LOCI: session resumed from %s", snap);
    return true;
}
/* --- Epic 9 / US4 : USB host scan + IRQ-trap (moved verbatim from main.c) --- */

/* Attach a host directory as a LOCI USB mass-storage device: it appears
 * in the menu's device list with the volume label and its "N:" paths
 * resolve inside the directory — a real USB key plugged into the host,
 * served to the Oric like on real hardware. */
void loci_attach_usb_dir(emulator_t* emu, const char* dir) {
    struct stat st;
    if (stat(dir, &st) != 0 || !S_ISDIR(st.st_mode)) {
        log_warning("LOCI USB: %s is not a directory — ignored", dir);
        return;
    }
    const char* label = strrchr(dir, '/');
    label = (label && label[1]) ? label + 1 : dir;
    char status[64];
    struct oscompat_statvfs vs;
    double gb = (oscompat_statvfs(dir, &vs) == 0)
              ? (double)vs.f_blocks * (double)vs.f_frsize
                / (1024.0 * 1024.0 * 1024.0) : 0.0;
    if (gb >= 1.0)
        snprintf(status, sizeof(status), "MSC %.1f GB %.40s", gb, label);
    else
        snprintf(status, sizeof(status), "MSC %.1f MB %.40s", gb * 1024.0, label);
    int n = loci_add_usb_storage(&emu->loci, status, dir);
    if (n > 0)
        log_info("LOCI: USB storage %d: (%s) -> %s", n, label, dir);
    else
        log_warning("LOCI USB: device table full, %s ignored", dir);
}

/* Auto-detect removable media mounted on the host (udisks convention:
 * /media/$USER and /run/media/$USER) and attach them as USB devices —
 * plug a real key in, it shows up in the LOCI menu. */
void loci_scan_host_usb(emulator_t* emu) {
    const char* user = getenv("USER");
    if (!user || !user[0]) user = getenv("USERNAME");   /* Windows */
    if (!user || !user[0]) return;
    const char* bases[] = { "/media", "/run/media" };
    for (size_t b = 0; b < sizeof(bases) / sizeof(bases[0]); b++) {
        char root[300];
        snprintf(root, sizeof(root), "%s/%s", bases[b], user);
        DIR* d = opendir(root);
        if (!d) continue;
        struct dirent* de;
        while ((de = readdir(d)) != NULL) {
            if (de->d_name[0] == '.') continue;
            char vol[560];
            snprintf(vol, sizeof(vol), "%s/%s", root, de->d_name);
            struct stat st;
            if (stat(vol, &st) != 0 || !S_ISDIR(st.st_mode)) continue;
            loci_attach_usb_dir(emu, vol);
        }
        closedir(d);
    }
}

/* LOCI action-button install hook (Sprint 34ai + 85).
 * Snapshots the session (the menu's "resume" needs the machine exactly
 * as it was — press time is a clean instruction boundary, before the
 * trap hijacks the vectors), saves the current IRQ vector at $FFFE/F,
 * redirects it to the trap at $03BA, then pulses the CPU IRQ line. The
 * trap bytes themselves were already mirrored into the MIA register
 * file by loci_action_button_short. */
void loci_action_install_irq_trap(void* ctx) {
    emulator_t* emu = (emulator_t*)ctx;
    if (!emu) return;
    if (!emu->loci_menu_active) {   /* inside the menu: keep the session snapshot */
        char snap[512];
        loci_resume_snapshot_path(emu, snap, sizeof(snap));
        if (savestate_save(emu, snap))
            log_info("LOCI: session snapshot -> %s (menu resume)", snap);
    }
    /* Save current vector. The ORIC IRQ vector lives in ROM at $FFFE/F,
     * backed by mem->rom (offset $3FFE/F since rom starts at $C000). */
    uint8_t lo = emu->memory.rom[0x3FFE];
    uint8_t hi = emu->memory.rom[0x3FFF];
    emu->loci.saved_irq_vector = (uint16_t)lo | ((uint16_t)hi << 8);
    /* Redirect to the trap at $03BA. */
    emu->memory.rom[0x3FFE] = 0xBA;
    emu->memory.rom[0x3FFF] = 0x03;
    /* Pulse the IRQ line. Source bit is arbitrary — VIA works because
     * the CPU handler doesn't introspect the source for this trap. */
    cpu_irq_set(&emu->cpu, IRQF_VIA);
}

/* LOCI action-button release hook (Sprint 34ai + 85).
 * Sets the 6502 V flag so the BVC -2 spin falls through, restores the
 * original IRQ vector, then performs the firmware's EXT_CAPTURE_IRQ →
 * EXT_BOOT_LOCI sequence: boot the LOCI menu ROM. On real hardware the
 * trap's JMP ($FFFA) lands in the freshly mapped LOCI ROM whose
 * save-state routine runs before the menu; our snapshot was taken at
 * press time, so we go straight to the menu via the ROM's reset vector. */
void loci_action_release_irq_trap(void* ctx) {
    emulator_t* emu = (emulator_t*)ctx;
    if (!emu) return;
    emu->cpu.P |= FLAG_OVERFLOW;
    uint16_t v = emu->loci.saved_irq_vector;
    emu->memory.rom[0x3FFE] = (uint8_t)(v & 0xFF);
    emu->memory.rom[0x3FFF] = (uint8_t)(v >> 8);
    /* Clear the IRQ source so it doesn't re-fire on the next instruction. */
    cpu_irq_clear(&emu->cpu, IRQF_VIA);

    if (emu->loci_button_long) {
        /* Firmware warm long hold (≥ 2 s): EXT_BOOT_DIAG — boot Mike
         * Brown's diagnostic ROM (test108k, embedded in real firmware
         * builds with his permission). No resume path: it is a hardware
         * test reboot; F5/menu brings the machine back afterwards. */
        emu->loci_button_long = false;
        char diag[512];
        if (!loci_find_rom_file(emu, "test108k.rom", diag, sizeof(diag))) {
            log_warning("LOCI: diag ROM introuvable (test108k.rom dans le flash "
                        "root ou roms/loci/) — appui long sans effet");
            return;
        }
        if (loci_rom_swap_cb(emu, diag, 0xC000)) {
            emu->loci_menu_active = false;
            cpu_reset(&emu->cpu);
            log_info("LOCI: long press -> diag ROM %s", diag);
        }
        return;
    }
    if (emu->loci_menu_active) {
        log_info("LOCI: Action button inside the menu — ignored");
        return;
    }
    char rom[512];
    if (!loci_find_menu_rom(emu, rom, sizeof(rom))) {
        log_warning("LOCI: menu ROM introuvable (LOCIROM/locirom dans le flash "
                    "root, ou roms/loci/locirom) — bouton Action sans effet");
        return;
    }
    if (loci_rom_swap_cb(emu, rom, 0xC000)) {
        emu->loci_menu_active = true;
        cpu_reset(&emu->cpu);
        log_info("LOCI: warm boot -> menu ROM %s", rom);
    }
}
/* --- Epic 9 / US5 : SDL keyboard sync (only SDL dependency of the glue) --- */
#ifdef HAS_SDL2
#include <SDL2/SDL.h>
/* Sync the LOCI keyboard report from the current SDL keyboard state: SDL
 * scancodes map 1:1 to HID usage IDs, so collect the first six down keys +
 * pack the SDL modifiers into the HID modifier byte. Moved from main.c. */
void loci_sync_kbd_from_sdl(emulator_t* emu) {
    if (!emu || !emu->has_loci) return;

    int numkeys = 0;
    const Uint8* state = SDL_GetKeyboardState(&numkeys);
    if (!state) return;

    SDL_Keymod m = SDL_GetModState();
    uint8_t hid_mod = 0;
    if (m & KMOD_LCTRL)  hid_mod |= 0x01;
    if (m & KMOD_LSHIFT) hid_mod |= 0x02;
    if (m & KMOD_LALT)   hid_mod |= 0x04;
    if (m & KMOD_LGUI)   hid_mod |= 0x08;
    if (m & KMOD_RCTRL)  hid_mod |= 0x10;
    if (m & KMOD_RSHIFT) hid_mod |= 0x20;
    if (m & KMOD_RALT)   hid_mod |= 0x40;
    if (m & KMOD_RGUI)   hid_mod |= 0x80;

    uint8_t keys[6] = {0};
    int kn = 0;
    /* HID modifier keys live at 0xE0+ — skip those, they're already in
     * hid_mod. Standard usage page tops out around 0xE7; clamp. */
    int max = numkeys < 0xE0 ? numkeys : 0xE0;
    for (int sc = SDL_SCANCODE_A; sc < max && kn < 6; sc++) {
        if (state[sc]) {
            keys[kn++] = (uint8_t)sc;
        }
    }
    loci_kbd_set_report(&emu->loci, hid_mod, keys);
}
#endif /* HAS_SDL2 */
