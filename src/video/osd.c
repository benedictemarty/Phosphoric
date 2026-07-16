/**
 * @file osd.c
 * @brief On-Screen Display — overlay de changement de média à chaud.
 * @author bmarty <bmarty@mailo.com>
 */
#include "video/osd.h"

#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <ctype.h>
#include <stdlib.h>
#include <dirent.h>

/* ── Liste de fichiers ────────────────────────────────────────────── */

static bool has_ext(const char* name, const char* ext) {
    size_t n = strlen(name), e = strlen(ext);
    if (n < e) return false;
    for (size_t i = 0; i < e; i++) {
        if (tolower((unsigned char)name[n - e + i]) != tolower((unsigned char)ext[i]))
            return false;
    }
    return true;
}

static int entry_cmp(const void* a, const void* b) {
    const osd_entry_t* ea = (const osd_entry_t*)a;
    const osd_entry_t* eb = (const osd_entry_t*)b;
    if (ea->is_disk != eb->is_disk) return ea->is_disk - eb->is_disk; /* cassettes d'abord */
    return strcasecmp(ea->name, eb->name);
}

void osd_scan(osd_t* osd, const char* const* dirs) {
    osd->count = 0;
    osd->selected = 0;
    osd->scroll = 0;
    for (int d = 0; dirs && dirs[d]; d++) {
        DIR* dp = opendir(dirs[d]);
        if (!dp) continue;
        struct dirent* de;
        while ((de = readdir(dp)) != NULL && osd->count < OSD_MAX_ENTRIES) {
            bool disk = has_ext(de->d_name, ".dsk");
            bool tape = has_ext(de->d_name, ".tap");
            if (!disk && !tape) continue;
            osd_entry_t* e = &osd->entries[osd->count];
            /* Chemin "dir/nom" : ignore l'entrée si elle ne tient pas. */
            if (snprintf(e->path, OSD_PATH_MAX, "%s/%s", dirs[d], de->d_name) >= OSD_PATH_MAX)
                continue;
            /* Nom d'affichage : troncature propre (sans warning format). */
            size_t nl = strlen(de->d_name);
            if (nl >= OSD_NAME_MAX) nl = OSD_NAME_MAX - 1;
            memcpy(e->name, de->d_name, nl);
            e->name[nl] = '\0';
            e->is_disk = disk;
            osd->count++;
        }
        closedir(dp);
    }
    if (osd->count > 0)
        qsort(osd->entries, (size_t)osd->count, sizeof(osd_entry_t), entry_cmp);
}

/* ── Cycle de vie ─────────────────────────────────────────────────── */

void osd_init(osd_t* osd) {
    memset(osd, 0, sizeof(*osd));
}

void osd_snapshot_font(osd_t* osd, const uint8_t* mem) {
    if (!mem) return;
    memcpy(osd->font, &mem[0xB400], 128 * 8);
    osd->font_ready = true;
}

void osd_open(osd_t* osd) {
    static const char* const dirs[] = { "tapes", "disks", "demos/ula-ng", ".", NULL };
    osd_scan(osd, dirs);
    osd->open = true;
}

void osd_close(osd_t* osd) { osd->open = false; }

void osd_toggle(osd_t* osd) {
    if (osd->open) osd_close(osd);
    else osd_open(osd);
}

/* ── Navigation ───────────────────────────────────────────────────── */

osd_action_t osd_key(osd_t* osd, int key) {
    if (!osd->open) return OSD_NONE;
    switch (key) {
        case OSD_KEY_UP:
            if (osd->selected > 0) osd->selected--;
            break;
        case OSD_KEY_DOWN:
            if (osd->selected < osd->count - 1) osd->selected++;
            break;
        case OSD_KEY_LEFT:
            osd->disk_drive = (osd->disk_drive + OSD_DRIVES - 1) % OSD_DRIVES;
            break;
        case OSD_KEY_RIGHT:
            osd->disk_drive = (osd->disk_drive + 1) % OSD_DRIVES;
            break;
        case OSD_KEY_ESC:
            osd_close(osd);
            return OSD_CLOSED;
        case OSD_KEY_ENTER:
            if (osd->count > 0) return OSD_ACTIVATE;
            break;
        case OSD_KEY_EJECT:
            /* Éjecte selon le média surligné : cassette si l'entrée est un .tap,
             * sinon le disque du lecteur cible (défaut si liste vide). */
            if (osd->count > 0 && !osd->entries[osd->selected].is_disk)
                return OSD_EJECT_TAPE;
            return OSD_EJECT;
        default: break;
    }
    /* garde la sélection visible */
    if (osd->selected < osd->scroll) osd->scroll = osd->selected;
    if (osd->selected >= osd->scroll + OSD_VISIBLE) osd->scroll = osd->selected - OSD_VISIBLE + 1;
    return OSD_NONE;
}

/* ── Rendu ────────────────────────────────────────────────────────── */

static void put_px(video_t* vid, int x, int y, uint8_t r, uint8_t g, uint8_t b) {
    if (x < 0 || x >= vid->native_w || y < 0 || y >= vid->native_h) return;
    int off = (y * vid->native_w + x) * 3;
    vid->framebuffer[off] = r; vid->framebuffer[off + 1] = g; vid->framebuffer[off + 2] = b;
}

/* Assombrit un rectangle (panneau translucide). */
static void dim_rect(video_t* vid, int x0, int y0, int x1, int y1) {
    for (int y = y0; y < y1; y++) {
        if (y < 0 || y >= vid->native_h) continue;
        for (int x = x0; x < x1; x++) {
            if (x < 0 || x >= vid->native_w) continue;
            int off = (y * vid->native_w + x) * 3;
            vid->framebuffer[off]     = (uint8_t)(vid->framebuffer[off]     / 5);
            vid->framebuffer[off + 1] = (uint8_t)(vid->framebuffer[off + 1] / 5);
            vid->framebuffer[off + 2] = (uint8_t)(vid->framebuffer[off + 2] / 5);
        }
    }
}

/* Dessine un glyphe 6x8 depuis le charset Oric (bits 5..0). */
static void draw_char(osd_t* osd, video_t* vid, int x, int y, char c,
                      uint8_t fr, uint8_t fg, uint8_t fb) {
    unsigned ch = (unsigned char)c;
    if (ch >= 128) ch = '?';
    const uint8_t* gl = &osd->font[ch * 8];
    for (int row = 0; row < 8; row++) {
        uint8_t bits = gl[row];
        for (int bx = 5; bx >= 0; bx--) {
            if (bits & (1 << bx)) put_px(vid, x + (5 - bx), y + row, fr, fg, fb);
        }
    }
}

static void draw_str(osd_t* osd, video_t* vid, int x, int y, const char* s,
                     uint8_t fr, uint8_t fg, uint8_t fb) {
    for (; *s; s++, x += 6) draw_char(osd, vid, x, y, *s, fr, fg, fb);
}

void osd_render(osd_t* osd, video_t* vid) {
    if (!osd->open) return;

    int W = vid->native_w, H = vid->native_h;
    int x0 = 6, y0 = 6, x1 = W - 6, y1 = H - 6;
    dim_rect(vid, x0, y0, x1, y1);

    int tx = x0 + 6, ty = y0 + 4;
    draw_str(osd, vid, tx, ty, "CHANGER LE MEDIA  (F6)", 255, 255, 0);
    ty += 12;
    draw_str(osd, vid, tx, ty, "HAUT/BAS=CHOISIR  GCH/DRT=LECTEUR", 160, 160, 160);
    ty += 9;
    draw_str(osd, vid, tx, ty, "RET=CHARGER  SUPPR=EJECTER  ESC=FERMER", 160, 160, 160);
    ty += 12;

    char drv[40];
    snprintf(drv, sizeof(drv), "LECTEUR DISQUE CIBLE: %c", 'A' + osd->disk_drive);
    draw_str(osd, vid, tx, ty, drv, 120, 200, 255);
    ty += 12;

    if (osd->count == 0) {
        draw_str(osd, vid, tx, ty, "AUCUN .TAP / .DSK TROUVE", 255, 120, 120);
    } else {
        int end = osd->scroll + OSD_VISIBLE;
        if (end > osd->count) end = osd->count;
        for (int i = osd->scroll; i < end; i++) {
            bool sel = (i == osd->selected);
            uint8_t r = sel ? 255 : 200, g = sel ? 255 : 200, b = sel ? 0 : 200;
            char line[64];
            snprintf(line, sizeof(line), "%c %c %.40s",
                     sel ? '>' : ' ', osd->entries[i].is_disk ? 'D' : 'K',
                     osd->entries[i].name);
            draw_str(osd, vid, tx, ty, line, r, g, b);
            ty += 9;
        }
    }

    if (osd->status[0]) {
        char st[64];
        snprintf(st, sizeof(st), "%.60s", osd->status);
        draw_str(osd, vid, tx, y1 - 12, st, 120, 255, 120);
    }
}
