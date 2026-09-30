/* SPDX-License-Identifier: EUPL-1.2 */
/**
 * @file iomenu_glue.c
 * @brief Peripherals menu (F1) ↔ emulator glue; phosphoric.cfg.
 * @author bmarty <bmarty@mailo.com>
 *
 * See include/iomenu_glue.h. The media operations come from the F6 OSD
 * (main.c), moved here to be shared, with no change in substance.
 */
#define _POSIX_C_SOURCE 200809L   /* strdup */
#include "iomenu_glue.h"
#include "savestate.h"
#include "storage/sedoric.h"
#include "io/keyboard.h"
#include "io/joystick.h"
#include "io/printer.h"
#include "io/loci_emu.h"
#include "utils/logging.h"
#include "utils/oscompat.h"   /* mkdir portable (MinGW : un seul argument) */
#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef HAS_SDL2
#include <SDL2/SDL.h>
#endif

/* ═══════════════════════════════════════════════════════════════════════
 *  Media
 * ═══════════════════════════════════════════════════════════════════════ */

bool media_disk_writeback(emulator_t* emu, int drv) {
    if (!emu->disk_writeback || drv < 0 || drv >= emu_disk_max_drives(emu)) return false;
    if (!emu_disk_dirty(emu, drv) || !emu->disks[drv] || !emu->disk_paths[drv])
        return false;
    bool ok = sedoric_save(emu->disks[drv], emu->disk_paths[drv]);
    log_info("OSD: write-back lecteur %c -> %s (%s)", 'A' + drv, emu->disk_paths[drv],
             ok ? "OK" : "ECHEC");
    emu_disk_clear_dirty(emu, drv);
    return ok;
}

media_result_t media_disk_insert(emulator_t* emu, int drv, const char* path) {
    if (!emu_has_disk_iface(emu)) return MEDIA_NO_IFACE;
    if (drv < 0 || drv >= emu_disk_max_drives(emu)) return MEDIA_BAD_DRIVE;
    sedoric_disk_t* nd = sedoric_load(path);
    if (!nd) return MEDIA_LOAD_FAILED;
    /* Save the old disk if it was modified, before overwriting it. */
    media_disk_writeback(emu, drv);
    if (emu->disks[drv]) sedoric_destroy(emu->disks[drv]);
    emu->disks[drv] = nd;
    emu_disk_clear_dirty(emu, drv);
    emu_disk_wire(emu, drv, nd);
    /* Per-drive path tracking (later write-back/eject). The initial
     * pointers come from argv (not freeable) → reassign. */
    emu->disk_paths[drv] = strdup(path);
    if (drv == 0)
        emu->disk_path = emu->disk_paths[drv];
    log_info("OSD: disque %c <- %s", 'A' + drv, path);
    return MEDIA_OK;
}

media_result_t media_disk_eject(emulator_t* emu, int drv) {
    if (!emu_has_disk_iface(emu)) return MEDIA_NO_IFACE;
    if (drv < 0 || drv >= emu_disk_max_drives(emu)) return MEDIA_BAD_DRIVE;
    if (!emu->disks[drv]) return MEDIA_EMPTY;
    media_disk_writeback(emu, drv);
    sedoric_destroy(emu->disks[drv]);
    emu->disks[drv] = NULL;
    emu->disk_paths[drv] = NULL;
    emu_disk_wire(emu, drv, NULL);
    if (drv == 0) emu->disk_path = NULL;
    log_info("OSD: lecteur %c ejecte", 'A' + drv);
    return MEDIA_OK;
}

media_result_t media_tape_insert(emulator_t* emu, const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return MEDIA_LOAD_FAILED;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > (1 << 20)) { fclose(f); return MEDIA_LOAD_FAILED; }
    uint8_t* buf = (uint8_t*)malloc((size_t)sz);
    if (!buf) { fclose(f); return MEDIA_LOAD_FAILED; }
    if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) { free(buf); fclose(f); return MEDIA_LOAD_FAILED; }
    fclose(f);
    if (emu->tapebuf) free(emu->tapebuf);
    emu->tapebuf = buf;
    emu->tapelen = (int)sz;
    emu->tapeoffs = 0;
    emu->tape_loaded = true;
    emu->tape_path = strdup(path);
    log_info("OSD: cassette <- %s", path);
    return MEDIA_OK;
}

media_result_t media_tape_eject(emulator_t* emu) {
    if (!emu->tape_loaded && !emu->tapebuf) return MEDIA_EMPTY;
    if (emu->tapebuf) { free(emu->tapebuf); emu->tapebuf = NULL; }
    emu->tapelen = 0;
    emu->tapeoffs = 0;
    emu->tape_loaded = false;
    emu->tape_path = NULL;
    log_info("OSD: cassette ejectee");
    return MEDIA_OK;
}

/* ═══════════════════════════════════════════════════════════════════════
 *  Displayed state
 * ═══════════════════════════════════════════════════════════════════════ */

static const char* base_name(const char* path) {
    if (!path) return "";
    const char* s = strrchr(path, '/');
    return s ? s + 1 : path;
}

static void add_card(iom_state_t* st, const char* name, bool present, const char* fmt, ...)
    __attribute__((format(printf, 4, 5)));
static void add_card(iom_state_t* st, const char* name, bool present, const char* fmt, ...) {
    if (st->cards >= IOM_CARDS) return;
    iom_card_t* k = &st->card[st->cards++];
    snprintf(k->name, sizeof(k->name), "%s", name);
    k->present = present;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(k->detail, sizeof(k->detail), fmt, ap);
    va_end(ap);
}

void iomenu_refresh(emulator_t* emu) {
    iom_state_t* st = &emu->iomenu.st;
    st->version = EMU_VERSION;
    st->machine = emu->model == ORIC_MODEL_ATMOS ? "Oric Atmos" : "Oric-1";
    st->disk_iface = emu->has_jasmin ? "Jasmin" : emu->has_microdisc ? "Microdisc" : NULL;
    st->drives = emu_has_disk_iface(emu) ? emu_disk_max_drives(emu) : 0;
    if (st->drives > 4) st->drives = 4;
    for (int d = 0; d < 4; d++) {
        const bool in = d < st->drives && emu->disks[d];
        snprintf(st->drive[d], sizeof(st->drive[d]), "%s",
                 in ? (emu->disk_paths[d] ? base_name(emu->disk_paths[d]) : "(image)") : "");
        st->drive_ro[d] = d < st->drives && emu_disk_protected(emu, d);
    }
    if (emu->tape_loaded) {
        snprintf(st->tape, sizeof(st->tape), "%s", emu->tape_path ? base_name(emu->tape_path) : "(cassette)");
        st->tape_percent = emu->tapelen > 0 ? (int)((int64_t)emu->tapeoffs * 100 / emu->tapelen) : 0;
        if (st->tape_percent > 100) st->tape_percent = 100;
    } else {
        st->tape[0] = '\0';
        st->tape_percent = 0;
    }
    st->tape_motor = emu->cassette.motor_on;
    st->tape_fast = emu->fast_load;
    st->printer = !oric_printer_is_active(&emu->printer) ? IOM_PRINTER_OFF
                : emu->printer.type == PRINTER_MCP40 ? IOM_PRINTER_MCP40 : IOM_PRINTER_TEXT;
    snprintf(st->printer_file, sizeof(st->printer_file), "%s",
             emu->printer.filename ? base_name(emu->printer.filename) : "");
    st->joystick = emu->joystick.mode == ORIC_JOY_KEYBOARD ? IOM_JOY_KEYS
                 : emu->joystick.mode == ORIC_JOY_SDL_GAMEPAD ? IOM_JOY_GAMEPAD : IOM_JOY_NONE;
    st->azerty = emu->keyboard.layout == ORIC_KB_AZERTY;

    /* Expansion cards (state at startup; read-only in the menu). */
    st->cards = 0;
    add_card(st, "Microdisc", emu->has_microdisc, "$0310  %s", base_name(emu->diskrom_path));
    add_card(st, "Jasmin", emu->has_jasmin, "$03F4  %s", base_name(emu->jasmin_rom_path));
    add_card(st, "LOCI", emu->has_loci, "$03A0  %s", loci_emu_active() ? "firmware" : "modèle HLE");
    add_card(st, "ACIA 6551", emu->has_serial, "$%04X  %s", emu->acia_base_addr,
             emu->serial_spec ? emu->serial_spec : "");
    add_card(st, "DTL 2000", emu->has_dtl2000, "$%04X  %s", emu->dtl2000.base_addr,
             emu->dtl2000_spec ? emu->dtl2000_spec : "");
    add_card(st, emu->mageco.oricon ? "ORICON" : "Mageco MIDI", emu->has_mageco, "$%04X  %s",
             emu->mageco.base_addr, emu->mageco_spec ? emu->mageco_spec : "");
    add_card(st, "SP0256", emu->has_sp0256, "$%04X  %s", emu->sp0256.base_addr,
             base_name(emu->sp0256_rom_path));
    add_card(st, "MEA8000", emu->has_mea8000, "$%04X  formants", emu->mea8000.base_addr);
    add_card(st, "ULA-NG", ula_ng_active(&emu->ula_ng), "$0340  déverrouillée");
    add_card(st, "Hôte (hostfs)", emu->hostfs.mounted, "%s", emu->hostfs.mount_path);
}

/* ═══════════════════════════════════════════════════════════════════════
 *  Actions
 * ═══════════════════════════════════════════════════════════════════════ */

static void msg(emulator_t* emu, bool err, const char* fmt, ...) __attribute__((format(printf, 3, 4)));
static void msg(emulator_t* emu, bool err, const char* fmt, ...) {
    char buf[96];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    iom_message(&emu->iomenu, err, buf);
}

static const char* media_error(media_result_t r) {
    switch (r) {
    case MEDIA_NO_IFACE:    return "pas d'interface disque (--disk-rom ou --jasmin-rom)";
    case MEDIA_BAD_DRIVE:   return "lecteur absent sur cette interface";
    case MEDIA_EMPTY:       return "déjà vide";
    case MEDIA_LOAD_FAILED: return "fichier illisible";
    default:                return "";
    }
}

/* Next free name snapshots/etatNNNN.ost. */
static bool next_snapshot_path(char* out, size_t n) {
    if (oscompat_mkdir("snapshots", 0755) != 0 && errno != EEXIST) return false;
    for (int i = 1; i < 10000; i++) {
        snprintf(out, n, "snapshots/etat%04d.ost", i);
        if (access(out, F_OK) != 0) return true;
    }
    return false;
}

static void printer_cycle(emulator_t* emu) {
    oric_printer_t* p = &emu->printer;
    const bool active = oric_printer_is_active(p);
    if (!active) {                                   /* off → text */
        p->type = PRINTER_TEXT;
        if (oric_printer_open(p, "impression.txt"))
            msg(emu, false, "Imprimante texte → impression.txt (LPRINT, LLIST)");
        else
            msg(emu, true, "Impossible d'ouvrir impression.txt");
    } else if (p->type == PRINTER_TEXT) {            /* text → plotter */
        oric_printer_close(p);
        p->type = PRINTER_MCP40;
        oric_printer_open(p, "traceur.bmp");
        msg(emu, false, "Traceur MCP-40 → traceur.bmp (écrit à la coupure)");
    } else {                                         /* plotter → off */
        oric_printer_close(p);
        p->type = PRINTER_NONE;
        msg(emu, false, "Imprimante coupée");
    }
}

static void joystick_cycle(emulator_t* emu) {
    oric_joystick_t* j = &emu->joystick;
    if (j->mode == ORIC_JOY_DISABLED) {
        oric_joystick_set_mode(j, ORIC_JOY_KEYBOARD);
        msg(emu, false, "Joystick : flèches du clavier + tir");
    } else if (j->mode == ORIC_JOY_KEYBOARD) {
        oric_joystick_set_mode(j, ORIC_JOY_SDL_GAMEPAD);
#ifdef HAS_SDL2
        if (SDL_NumJoysticks() > 0 && oric_joystick_open_sdl(j, 0))
            msg(emu, false, "Joystick : manette branchée");
        else
            msg(emu, false, "Joystick : manette (aucune détectée, en attente)");
#else
        msg(emu, false, "Joystick : manette (build sans SDL2)");
#endif
    } else {
#ifdef HAS_SDL2
        oric_joystick_close_sdl(j);
#endif
        oric_joystick_set_mode(j, ORIC_JOY_DISABLED);
        msg(emu, false, "Joystick : aucun");
    }
}

bool iomenu_apply(emulator_t* emu, const iom_action_t* a) {
    media_result_t r;
    switch (a->type) {
    case IOM_ACT_NONE:
        return false;
    case IOM_ACT_RESUME:
        return true;
    case IOM_ACT_DISK_INSERT:
        r = media_disk_insert(emu, a->target, a->path);
        if (r == MEDIA_OK) msg(emu, false, "Lecteur %c : %s", 'A' + a->target, base_name(a->path));
        else msg(emu, true, "Lecteur %c : %s", 'A' + a->target, media_error(r));
        return false;
    case IOM_ACT_DISK_EJECT:
        r = media_disk_eject(emu, a->target);
        if (r == MEDIA_OK) msg(emu, false, "Lecteur %c éjecté", 'A' + a->target);
        else msg(emu, r == MEDIA_EMPTY ? false : true, "Lecteur %c : %s", 'A' + a->target, media_error(r));
        return false;
    case IOM_ACT_DISK_PROTECT: {
        const bool on = !emu_disk_protected(emu, a->target);
        emu_disk_set_protected(emu, a->target, on);
        msg(emu, false, "Lecteur %c : %s", 'A' + a->target, on ? "protégé en écriture" : "écriture autorisée");
        return false;
    }
    case IOM_ACT_TAPE_INSERT:
        r = media_tape_insert(emu, a->path);
        if (r == MEDIA_OK) msg(emu, false, "Cassette : %s (CLOAD\"\")", base_name(a->path));
        else msg(emu, true, "Cassette : %s", media_error(r));
        return false;
    case IOM_ACT_TAPE_EJECT:
        r = media_tape_eject(emu);
        msg(emu, false, r == MEDIA_OK ? "Cassette éjectée" : "Pas de cassette");
        return false;
    case IOM_ACT_TAPE_REWIND:
        if (!emu->tape_loaded) { msg(emu, true, "Pas de cassette"); return false; }
        emu->tapeoffs = 0;
        msg(emu, false, "Cassette rembobinée");
        return false;
    case IOM_ACT_SNAPSHOT_SAVE: {
        char path[64];
        if (!next_snapshot_path(path, sizeof(path))) { msg(emu, true, "Dossier snapshots/ inaccessible"); return false; }
        if (savestate_save(emu, path)) {
            snprintf(emu->iomenu.st.snapshot_last, sizeof(emu->iomenu.st.snapshot_last), "%s", base_name(path));
            msg(emu, false, "Instantané enregistré : %s", path);
        } else {
            msg(emu, true, "Échec de l'enregistrement : %s", path);
        }
        return false;
    }
    case IOM_ACT_SNAPSHOT_LOAD:
        if (savestate_load(emu, a->path)) {
            snprintf(emu->iomenu.st.snapshot_last, sizeof(emu->iomenu.st.snapshot_last), "%.60s", base_name(a->path));
            msg(emu, false, "Instantané repris : %s", base_name(a->path));
            return true;                             /* immediate return to the machine */
        }
        msg(emu, true, "Instantané illisible : %s", base_name(a->path));
        return false;
    case IOM_ACT_PRINTER_CYCLE:
        printer_cycle(emu);
        return false;
    case IOM_ACT_JOYSTICK_CYCLE:
        joystick_cycle(emu);
        return false;
    case IOM_ACT_KEYBOARD_TOGGLE: {
        const bool az = emu->keyboard.layout != ORIC_KB_AZERTY;
        oric_keyboard_set_layout(&emu->keyboard, az ? ORIC_KB_AZERTY : ORIC_KB_QWERTY);
        msg(emu, false, "Clavier %s", az ? "AZERTY" : "QWERTY");
        return false;
    }
    case IOM_ACT_TAPE_FAST_TOGGLE:
        /* -f only acts at startup (1st block injected at boot): the choice
         * applies to the next launch, via « Enregistrer la configuration » (Save configuration). */
        emu->fast_load = !emu->fast_load;
        msg(emu, false, "Au prochain lancement : %s (enregistrer la configuration)",
            emu->fast_load ? "injection directe" : "CLOAD par la ROM");
        return false;
    case IOM_ACT_RESET:
        /* Same effect as F5: 6502 reset; LOCI reset button (mounts kept). */
        cpu_reset(&emu->cpu);
        if (emu->has_loci) loci_reset(&emu->loci);
        msg(emu, false, "Machine redémarrée (RESET)");
        return true;
    case IOM_ACT_SAVE_CONFIG: {
        const char* path = emu->config_path ? emu->config_path : IOMENU_CONFIG_DEFAULT;
        if (iomenu_config_save(emu, path)) msg(emu, false, "Configuration enregistrée : %s", path);
        else msg(emu, true, "Impossible d'écrire %s", path);
        return false;
    }
    }
    return false;
}

/* ═══════════════════════════════════════════════════════════════════════
 *  phosphoric.cfg — one « key=value » line per setting; « # » = comment
 * ═══════════════════════════════════════════════════════════════════════ */

/* Keys managed by the menu (the file's other lines are kept). */
static const char* const managed_keys[] = {
    "a", "b", "c", "d", "protection_a", "protection_b", "protection_c", "protection_d",
    "interface_disque", "rom_disque", "cassette", "cassette_rapide",
    "imprimante", "imprimante_fichier", "joystick", "clavier", NULL
};

static bool is_managed(const char* line) {
    const char* eq = strchr(line, '=');
    if (!eq) return false;
    size_t n = (size_t)(eq - line);
    while (n > 0 && isspace((unsigned char)line[n - 1])) n--;
    for (int i = 0; managed_keys[i]; i++)
        if (strlen(managed_keys[i]) == n && strncmp(line, managed_keys[i], n) == 0) return true;
    return false;
}

bool iomenu_config_save(emulator_t* emu, const char* path) {
    /* Lines to keep (everything not managed by the menu). */
    char* keep = NULL;
    size_t keep_len = 0;
    FILE* in = fopen(path, "r");
    if (in) {
        char line[1024];
        while (fgets(line, sizeof(line), in)) {
            if (is_managed(line)) continue;
            size_t l = strlen(line);
            char* nk = realloc(keep, keep_len + l + 1);
            if (!nk) break;
            keep = nk;
            memcpy(keep + keep_len, line, l + 1);
            keep_len += l;
        }
        fclose(in);
    }
    char tmp[1100];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE* out = fopen(tmp, "w");
    if (!out) { free(keep); return false; }
    if (keep_len) fputs(keep, out);
    else fputs("# phosphoric.cfg — réglages du menu des périphériques (F1).\n"
               "# Relu au lancement (sauf --headless / --no-config) ; la ligne de\n"
               "# commande reste prioritaire. Les lignes inconnues sont conservées.\n", out);
    free(keep);
    const bool jas = emu->has_jasmin, md = emu->has_microdisc;
    fprintf(out, "interface_disque=%s\n", jas ? "jasmin" : md ? "microdisc" : "aucune");
    if (jas && emu->jasmin_rom_path) fprintf(out, "rom_disque=%s\n", emu->jasmin_rom_path);
    else if (md && emu->diskrom_path) fprintf(out, "rom_disque=%s\n", emu->diskrom_path);
    for (int d = 0; d < 4; d++) {
        if (d < emu_disk_max_drives(emu) && emu->disks[d] && emu->disk_paths[d])
            fprintf(out, "%c=%s\n", 'a' + d, emu->disk_paths[d]);
        if (emu_has_disk_iface(emu) && emu_disk_protected(emu, d))
            fprintf(out, "protection_%c=oui\n", 'a' + d);
    }
    if (emu->tape_loaded && emu->tape_path) fprintf(out, "cassette=%s\n", emu->tape_path);
    fprintf(out, "cassette_rapide=%s\n", emu->fast_load ? "oui" : "non");
    const bool pr = oric_printer_is_active(&emu->printer);
    fprintf(out, "imprimante=%s\n", !pr ? "non" : emu->printer.type == PRINTER_MCP40 ? "mcp40" : "texte");
    if (pr && emu->printer.filename) fprintf(out, "imprimante_fichier=%s\n", emu->printer.filename);
    fprintf(out, "joystick=%s\n", emu->joystick.mode == ORIC_JOY_KEYBOARD ? "clavier"
                                : emu->joystick.mode == ORIC_JOY_SDL_GAMEPAD ? "manette" : "aucun");
    fprintf(out, "clavier=%s\n", emu->keyboard.layout == ORIC_KB_AZERTY ? "azerty" : "qwerty");
    bool ok = fclose(out) == 0;
#ifdef _WIN32
    if (ok) remove(path);   /* rename() de Windows n'écrase pas un fichier existant */
#endif
    if (ok) ok = rename(tmp, path) == 0;
    if (!ok) remove(tmp);
    log_info("Configuration %s : %s", ok ? "enregistrée" : "NON enregistrée", path);
    return ok;
}

static char* trim(char* s) {
    while (isspace((unsigned char)*s)) s++;
    char* e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) *--e = '\0';
    return s;
}

int iomenu_config_load(const char* path, cli_opts_t* cfg) {
    FILE* f = fopen(path, "r");
    if (!f) return -1;
    int applied = 0;
    char line[1024];
    char* rom = NULL;
    char iface[16] = "";
    while (fgets(line, sizeof(line), f)) {
        char* s = trim(line);
        if (!*s || *s == '#') continue;
        char* eq = strchr(s, '=');
        if (!eq) continue;
        *eq = '\0';
        char* key = trim(s);
        char* val = trim(eq + 1);
        const bool oui = strcasecmp(val, "oui") == 0;
        /* Persistent string (the options point to it until the end). */
        #define KEEP(v) strdup(v)
        if (strlen(key) == 1 && key[0] >= 'a' && key[0] <= 'd' && *val) {
            int d = key[0] - 'a';
            if (!cfg->disk_files[d]) { cfg->disk_files[d] = KEEP(val); applied++; }
        } else if (strncmp(key, "protection_", 11) == 0 && key[11] >= 'a' && key[11] <= 'd' && !key[12]) {
            if (oui) { cfg->disk_protect[key[11] - 'a'] = true; applied++; }
        } else if (strcmp(key, "interface_disque") == 0) {
            snprintf(iface, sizeof(iface), "%s", val);
        } else if (strcmp(key, "rom_disque") == 0 && *val) {
            free(rom);
            rom = KEEP(val);
        } else if (strcmp(key, "cassette") == 0 && *val) {
            if (!cfg->tape_file) { cfg->tape_file = KEEP(val); applied++; }
        } else if (strcmp(key, "cassette_rapide") == 0) {
            if (oui && !cfg->fast_load) { cfg->fast_load = true; applied++; }
        } else if (strcmp(key, "imprimante") == 0) {
            if (!cfg->printer_type_arg && strcasecmp(val, "mcp40") == 0) { cfg->printer_type_arg = "mcp40"; applied++; }
            if (!cfg->printer_file && (strcasecmp(val, "texte") == 0 || strcasecmp(val, "mcp40") == 0)) {
                cfg->printer_file = strcasecmp(val, "mcp40") == 0 ? "traceur.bmp" : "impression.txt";
                applied++;
            }
        } else if (strcmp(key, "imprimante_fichier") == 0 && *val) {
            /* Applied only if the printer comes from this file (not from the CLI). */
            if (cfg->printer_file && (strcmp(cfg->printer_file, "impression.txt") == 0 ||
                                      strcmp(cfg->printer_file, "traceur.bmp") == 0))
                cfg->printer_file = KEEP(val);
        } else if (strcmp(key, "joystick") == 0) {
            if (!cfg->joystick_mode && strcasecmp(val, "clavier") == 0) { cfg->joystick_mode = "keys"; applied++; }
            if (!cfg->joystick_mode && strcasecmp(val, "manette") == 0) { cfg->joystick_mode = "gamepad"; applied++; }
        } else if (strcmp(key, "clavier") == 0) {
            if (!cfg->keyboard_layout && strcasecmp(val, "azerty") == 0) { cfg->keyboard_layout = "azerty"; applied++; }
        }
        #undef KEEP
    }
    fclose(f);
    /* Disk interface: only if the command line chooses none. */
    if (rom && !cfg->disk_rom_file && !cfg->jasmin_rom_file) {
        if (strcasecmp(iface, "jasmin") == 0) { cfg->jasmin_rom_file = rom; applied++; rom = NULL; }
        else if (strcasecmp(iface, "microdisc") == 0) { cfg->disk_rom_file = rom; applied++; rom = NULL; }
    }
    free(rom);
    return applied;
}

/* ═══════════════════════════════════════════════════════════════════════
 *  PPM capture of the menu
 * ═══════════════════════════════════════════════════════════════════════ */

bool iomenu_screenshot(emulator_t* emu, const char* path) {
    static iom_surface_t surf;
    static uint8_t rgb[IOM_WIDTH * IOM_HEIGHT * 3];
    const bool was_open = emu->iomenu.open;
    iomenu_refresh(emu);
    if (!was_open) iom_open(&emu->iomenu);
    iom_draw(&emu->iomenu, &surf);
    iom_rasterize(&surf, rgb);
    if (!was_open) iom_close(&emu->iomenu);
    FILE* f = fopen(path, "wb");
    if (!f) return false;
    fprintf(f, "P6\n%d %d\n255\n", IOM_WIDTH, IOM_HEIGHT);
    bool ok = fwrite(rgb, 1, sizeof(rgb), f) == sizeof(rgb);
    return fclose(f) == 0 && ok;
}
