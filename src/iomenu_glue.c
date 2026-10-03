/* SPDX-License-Identifier: EUPL-1.2 */
/**
 * @file iomenu_glue.c
 * @brief Liaison menu des périphériques (F1) ↔ émulateur ; phosphoric.cfg.
 * @author bmarty <bmarty@mailo.com>
 *
 * Voir include/iomenu_glue.h. Les opérations sur les médias viennent de l'OSD
 * F6 (main.c), déplacées ici pour être partagées, sans changement de fond.
 */
#define _POSIX_C_SOURCE 200809L   /* strdup */
#include "iomenu_glue.h"
#include "savestate.h"
#include "storage/sedoric.h"
#include "io/keyboard.h"
#include "io/joystick.h"
#include "io/printer.h"
#include "io/loci_emu.h"
#include "io/loci_internal.h"   /* loci_dsk_open / loci_dsk_close */
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
 *  Médias
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

/* Règle de routage : emu_loci_disks() (emulator.h). */
#define loci_disks emu_loci_disks

media_result_t media_disk_insert(emulator_t* emu, int drv, const char* path) {
    if (loci_disks(emu)) {
        if (drv < 0 || drv >= 4) return MEDIA_BAD_DRIVE;
        if (!loci_dsk_open(&emu->loci, (uint8_t)drv, path)) return MEDIA_LOAD_FAILED;
        log_info("OSD: disque %c <- %s (LOCI)", 'A' + drv, path);
        return MEDIA_OK;
    }
    if (!emu_has_disk_iface(emu)) return MEDIA_NO_IFACE;
    if (drv < 0 || drv >= emu_disk_max_drives(emu)) return MEDIA_BAD_DRIVE;
    sedoric_disk_t* nd = sedoric_load(path);
    if (!nd) return MEDIA_LOAD_FAILED;
    /* Sauve l'ancien disque s'il a été modifié, avant de l'écraser. */
    media_disk_writeback(emu, drv);
    if (emu->disks[drv]) sedoric_destroy(emu->disks[drv]);
    emu->disks[drv] = nd;
    emu_disk_clear_dirty(emu, drv);
    emu_disk_wire(emu, drv, nd);
    /* Suivi du chemin par lecteur (write-back/éjection ultérieurs). */
    emu_set_disk_path(emu, drv, path);
    log_info("OSD: disque %c <- %s", 'A' + drv, path);
    return MEDIA_OK;
}

media_result_t media_disk_eject(emulator_t* emu, int drv) {
    if (loci_disks(emu)) {
        if (drv < 0 || drv >= 4) return MEDIA_BAD_DRIVE;
        if (!emu->loci.dsk_host_path[drv][0] && !emu->loci.dsk_image[drv]) return MEDIA_EMPTY;
        loci_dsk_close(&emu->loci, (uint8_t)drv);   /* écrit les secteurs modifiés */
        return MEDIA_OK;
    }
    if (!emu_has_disk_iface(emu)) return MEDIA_NO_IFACE;
    if (drv < 0 || drv >= emu_disk_max_drives(emu)) return MEDIA_BAD_DRIVE;
    if (!emu->disks[drv]) return MEDIA_EMPTY;
    media_disk_writeback(emu, drv);
    sedoric_destroy(emu->disks[drv]);
    emu->disks[drv] = NULL;
    emu_set_disk_path(emu, drv, NULL);
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
    emu_set_tape_path(emu, path);
    log_info("OSD: cassette <- %s", path);
    return MEDIA_OK;
}

media_result_t media_tape_eject(emulator_t* emu) {
    if (!emu->tape_loaded && !emu->tapebuf) return MEDIA_EMPTY;
    if (emu->tapebuf) { free(emu->tapebuf); emu->tapebuf = NULL; }
    emu->tapelen = 0;
    emu->tapeoffs = 0;
    emu->tape_loaded = false;
    emu_set_tape_path(emu, NULL);
    log_info("OSD: cassette ejectee");
    return MEDIA_OK;
}

/* ═══════════════════════════════════════════════════════════════════════
 *  État affiché
 * ═══════════════════════════════════════════════════════════════════════ */

static const char* base_name(const char* path) {
    if (!path) return "";
    const char* s = strrchr(path, '/');
    return s ? s + 1 : path;
}

void iomenu_refresh(emulator_t* emu) {
    iom_state_t* st = &emu->iomenu.st;
    st->version = EMU_VERSION;
    st->machine = emu->model == ORIC_MODEL_ATMOS ? "Oric Atmos" : "Oric-1";
    st->disk_iface = emu->card_on[CARD_IDX_jasmin] ? "Jasmin" : emu->card_on[CARD_IDX_microdisc] ? "Microdisc"
                   : loci_disks(emu) ? "LOCI" : NULL;
    st->drives = emu_has_disk_iface(emu) ? emu_disk_max_drives(emu) : loci_disks(emu) ? 4 : 0;
    st->no_drive_protect = loci_disks(emu);
    if (st->drives > 4) st->drives = 4;
    for (int d = 0; d < 4; d++) {
        if (loci_disks(emu)) {
            const char* p = emu->loci.dsk_host_path[d];
            snprintf(st->drive[d], sizeof(st->drive[d]), "%s",
                     p[0] ? base_name(p) : emu->loci.dsk_image[d] ? "(image)" : "");
            st->drive_ro[d] = false;
            continue;
        }
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

    /* Cartes d'extension : liste du registre (cards.h), état de la machine en
     * cours. Les choix (page des cartes) sont chargés à l'ouverture du menu. */
    iom_menu_t* m = &emu->iomenu;
    if (!m->open) {
        cards_state_from(&m->cards_orig, emu, emu->argc, emu->argv);
        m->cards = m->cards_orig;
#ifdef __EMSCRIPTEN__
        m->cards_readonly = true;     /* pas de relance dans le navigateur */
#else
        m->cards_readonly = emu->argv == NULL;
#endif
    }
    st->cards = 0;
    for (int i = 0; i < cards_count() && st->cards < IOM_CARDS; i++) {
        const card_desc_t* d = cards_get(i);
        const card_choice_t* c = &m->cards_orig.card[i];
        iom_card_t* k = &st->card[st->cards++];
        snprintf(k->name, sizeof(k->name), "%s", d->name);
        k->present = c->on;
        k->detail[0] = '\0';
        if (!c->on) continue;
        unsigned addr = d->io_param >= 0 ? (unsigned)strtoul(c->value[d->io_param], NULL, 16) : d->io_base;
        const char* main_v = d->enable_param >= 0 ? c->value[d->enable_param] : "";
        if (addr) snprintf(k->detail, sizeof(k->detail), "$%04X  %s", addr, base_name(main_v));
        else snprintf(k->detail, sizeof(k->detail), "%s", base_name(main_v));
    }
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

/* Prochain nom libre snapshots/etatNNNN.ost. */
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
    if (!active) {                                   /* coupée → texte */
        p->type = PRINTER_TEXT;
        if (oric_printer_open(p, "impression.txt"))
            msg(emu, false, "Imprimante texte → impression.txt (LPRINT, LLIST)");
        else
            msg(emu, true, "Impossible d'ouvrir impression.txt");
    } else if (p->type == PRINTER_TEXT) {            /* texte → traceur */
        oric_printer_close(p);
        p->type = PRINTER_MCP40;
        oric_printer_open(p, "traceur.bmp");
        msg(emu, false, "Traceur MCP-40 → traceur.bmp (écrit à la coupure)");
    } else {                                         /* traceur → coupée */
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
    case IOM_ACT_CARDS_APPLY: {
        /* Redémarrage à froid : la boucle s'arrête, main() relance le processus
         * avec ces options (cards_exec). */
        cards_argv_free(emu->restart_argv);
        emu->restart_argv = cards_build_argv(&emu->iomenu.cards, emu->argc, emu->argv, NULL);
        emu->running = false;
        msg(emu, false, "Redémarrage avec les nouvelles cartes…");
        return true;
    }
    case IOM_ACT_DISK_INSERT:
        r = media_disk_insert(emu, a->target, a->path);
        if (r == MEDIA_OK) msg(emu, false, "Lecteur %c : %s", 'A' + a->target, base_name(a->path));
        else if (r == MEDIA_NO_IFACE && emu->card_on[CARD_IDX_loci] && emu->loci_external)
            msg(emu, true, "LOCI : les disquettes se montent depuis son menu (bouton MENU, F8)");
        else msg(emu, true, "Lecteur %c : %s", 'A' + a->target, media_error(r));
        return false;
    case IOM_ACT_DISK_EJECT:
        r = media_disk_eject(emu, a->target);
        if (r == MEDIA_OK) msg(emu, false, "Lecteur %c éjecté", 'A' + a->target);
        else msg(emu, r == MEDIA_EMPTY ? false : true, "Lecteur %c : %s", 'A' + a->target, media_error(r));
        return false;
    case IOM_ACT_DISK_PROTECT: {
        if (loci_disks(emu)) {
            msg(emu, true, "Lecteur %c : pas de protection par lecteur sur LOCI", 'A' + a->target);
            return false;
        }
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
            return true;                             /* retour immédiat à la machine */
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
        /* -f n'agit qu'au démarrage (injection du 1er bloc au boot) : le choix
         * vaut pour le prochain lancement, via « Enregistrer la configuration ». */
        emu->fast_load = !emu->fast_load;
        msg(emu, false, "Au prochain lancement : %s (enregistrer la configuration)",
            emu->fast_load ? "injection directe" : "CLOAD par la ROM");
        return false;
    case IOM_ACT_RESET:
        /* Même effet que F5 : reset du 6502 ; bouton reset du LOCI (montages gardés). */
        cpu_reset(&emu->cpu);
        if (emu->card_on[CARD_IDX_loci]) loci_reset(&emu->loci);
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
 *  phosphoric.cfg — une ligne « clé=valeur » par réglage ; « # » = commentaire
 * ═══════════════════════════════════════════════════════════════════════ */

/* Clés gérées par le menu (les autres lignes du fichier sont conservées). */
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
    return cards_cfg_key(line, n);   /* carte.<id> et <id>.<paramètre> */
}

bool iomenu_config_save(emulator_t* emu, const char* path) {
    /* Lignes à conserver (tout ce qui n'est pas géré par le menu). */
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
    /* Cartes : le choix du menu (page des cartes), en clés carte.* ; les
     * anciennes clés interface_disque / rom_disque restent lues au lancement. */
    if (emu->iomenu.open) {
        cards_cfg_write(&emu->iomenu.cards, out);
    } else {   /* menu jamais ouvert : les cartes de la machine en cours */
        cards_state_t cur;
        cards_state_from(&cur, emu, emu->argc, emu->argv);
        cards_cfg_write(&cur, out);
    }
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
        /* Chaîne persistante (les options pointent dessus jusqu'à la fin). */
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
            /* Appliqué seulement si l'imprimante vient de ce fichier (pas de la CLI). */
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
    /* Interface disque : seulement si la ligne de commande n'en choisit aucune. */
    if (rom && !cfg->disk_rom_file && !cfg->jasmin_rom_file) {
        if (strcasecmp(iface, "jasmin") == 0) { cfg->jasmin_rom_file = rom; applied++; rom = NULL; }
        else if (strcasecmp(iface, "microdisc") == 0) { cfg->disk_rom_file = rom; applied++; rom = NULL; }
    }
    free(rom);
    return applied;
}

/* ═══════════════════════════════════════════════════════════════════════
 *  Capture PPM du menu
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
