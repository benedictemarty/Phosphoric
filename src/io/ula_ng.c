/**
 * @file ula_ng.c
 * @brief ULA-NG étape 1 : déverrouillage NG_LOCK/NG_ID + garde verrou.
 * @author bmarty <bmarty@mailo.com>
 *
 * Cf docs/ula-ng/AUDIT.md (§0) et ULA-NG-SPEC.md (§3, §4.1).
 */

#include "io/ula_ng.h"
#include <string.h>

/* 8 couleurs Oric (RGB888), identité de la LUT au reset → compatibilité. */
static const uint8_t oric_palette[8][3] = {
    {0x00,0x00,0x00},{0xFF,0x00,0x00},{0x00,0xFF,0x00},{0xFF,0xFF,0x00},
    {0x00,0x00,0xFF},{0xFF,0x00,0xFF},{0x00,0xFF,0xFF},{0xFF,0xFF,0xFF},
};

void ula_ng_reset(ula_ng_t* u) {
    u->unlocked = false;
    u->unlock_step = 0;
    memset(u->regs, 0, sizeof(u->regs));
    /* LUT : entrées 0-7 = couleurs Oric (identité), 8-15 = noir. */
    memset(u->pal, 0, sizeof(u->pal));
    memcpy(u->pal, oric_palette, sizeof(oric_palette));
    u->pal_idx = 0;
    u->pal_r = 0;
    u->active = false;
    u->raster_line = 0;
    u->raster_enable = false;
    u->raster_pending = false;
    u->scrstart = 0;
    u->scrollx = 0;
    u->scrolly = 0;
    u->copper_count = 0;
    u->copper_phase = 0;
    memset(u->attr, 0, sizeof(u->attr));
    u->attr_wp = 0;
    u->attr_active = false;
    memset(u->sprites, 0, sizeof(u->sprites));
    u->spr_sel = 0;
    u->spr_wp = 0;
    u->spr_enable = false;
    u->spr_collision = false;
    u->spr_active = false;
    u->chunky_active = false;
    u->text80_active = false;
    u->vdu_cmd = 0;
    u->vdu_need = 0;
    u->vdu_got = 0;
    memset(u->vdu_params, 0, sizeof(u->vdu_params));
    memset(u->vram, 0, sizeof(u->vram));
    u->vram_active = false;
    u->vdu_gcol = 0;
    u->vdu_upload = 0;
}

void ula_ng_init(ula_ng_t* u) {
    ula_ng_reset(u);
}

bool ula_ng_active(const ula_ng_t* u) {
    return u->unlocked;
}

uint8_t ula_ng_read(ula_ng_t* u, uint16_t addr) {
    /* Le dispatcher n'appelle cette fonction que déverrouillé. */
    switch (addr) {
        case ULA_NG_REG_LOCK:  return ULA_NG_VERSION;                 /* NG_ID */
        case ULA_NG_REG_IDCHK: return (uint8_t)(~ULA_NG_VERSION);     /* NG_IDCHK = ~NG_ID */
        case ULA_NG_REG_STATUS: return u->raster_pending ? ULA_NG_STATUS_IRQ : 0x00;
        case ULA_NG_REG_SPR_STATUS: {           /* b7 = collision, clear on read */
            uint8_t s = u->spr_collision ? ULA_NG_SPR_STATUS_COL : 0x00;
            u->spr_collision = false;
            return s;
        }
        default:               return u->regs[addr - ULA_NG_WINDOW_LO];
    }
}

/* ── VDU intégré (docs/ula-ng/VDU.md) ───────────────────────────────────────
 * Interpréteur de flux de commandes (stand-in du firmware soft-core FPGA).
 * L'exécution se contente d'appeler la logique de registres déjà existante. */

static int iabs(int v) { return v < 0 ? -v : v; }

/* Trace un pixel dans la VRAM chunky (x 0-159, y 0-223, c index 0-15). */
static void vram_plot(ula_ng_t* u, int x, int y, uint8_t c) {
    if (x < 0 || x >= ULA_NG_VRAM_W || y < 0 || y >= ULA_NG_VRAM_H) return;
    int idx = y * ULA_NG_VRAM_STRIDE + (x >> 1);
    if (x & 1) u->vram[idx] = (uint8_t)((u->vram[idx] & 0xF0) | (c & 0x0F));
    else       u->vram[idx] = (uint8_t)((u->vram[idx] & 0x0F) | ((c & 0x0F) << 4));
}

/* Trace une ligne (Bresenham) dans la VRAM chunky. */
static void vram_line(ula_ng_t* u, int x0, int y0, int x1, int y1, uint8_t c) {
    int dx = iabs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -iabs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        vram_plot(u, x0, y0, c);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

static uint8_t vdu_need_for(uint8_t cmd) {
    switch (cmd) {
        case 16: case 20: return 0;   /* CLG / reset */
        case 17: case 18: case 22: return 1;  /* GCOL / fill / MODE */
        case 23: return 1;            /* begin upload motif sprite (id) */
        case 25: return 2;            /* PLOT point (x,y) */
        case 31: return 3;            /* colorer une cellule (col,row,attr) */
        case 19: case 24: case 26: return 4;  /* palette / sprite pos (id,x,y,f) / DRAW */
        default: return 0;            /* inconnu : ignoré */
    }
}

static void vdu_exec(ula_ng_t* u) {
    switch (u->vdu_cmd) {
        case 20:                                    /* reset -> rendu normal */
            u->vram_active = false;
            ula_ng_write(u, ULA_NG_REG_MODE, 0x00);
            break;
        case 16:                                    /* CLG : chunky + VRAM ULA-NG, efface */
            memset(u->vram, 0, sizeof(u->vram));
            u->vram_active = true;
            ula_ng_write(u, ULA_NG_REG_MODE, 0x05); /* chunky */
            break;
        case 17:                                    /* couleur de tracé courante (0-15) */
            u->vdu_gcol = u->vdu_params[0] & 0x0F;
            break;
        case 25:                                    /* PLOT point (x,y) */
            vram_plot(u, u->vdu_params[0], u->vdu_params[1], u->vdu_gcol);
            break;
        case 26:                                    /* DRAW ligne (x0,y0,x1,y1) */
            vram_line(u, u->vdu_params[0], u->vdu_params[1],
                      u->vdu_params[2], u->vdu_params[3], u->vdu_gcol);
            break;
        case 23:                                    /* begin upload motif sprite (id) */
            u->spr_sel = u->vdu_params[0] & (ULA_NG_SPRITES - 1);
            u->spr_wp = 0;
            u->vdu_upload = ULA_NG_SPR_PIXELS;      /* 256 octets suivants = motif */
            break;
        case 24: {                                  /* sprite : position + enable (id,x,y,f) */
            uint8_t id = u->vdu_params[0] & (ULA_NG_SPRITES - 1);
            u->sprites[id].x = u->vdu_params[1];
            u->sprites[id].y = u->vdu_params[2];
            u->sprites[id].enable = (u->vdu_params[3] & 0x01u) != 0;
            u->spr_enable = true;                   /* active les sprites (cache recalc. après) */
            break;
        }
        case 22: {                                  /* MODE : 0 std/1 chunky/2 80col */
            uint8_t n = u->vdu_params[0];
            uint8_t m = (n == 1) ? 0x05u : (n == 2) ? 0x09u : 0x01u;
            ula_ng_write(u, ULA_NG_REG_MODE, m);
            break;
        }
        case 19:                                    /* palette LUT[l] = RGB444 */
            ula_ng_write(u, ULA_NG_REG_PAL_IDX, (uint8_t)(u->vdu_params[0] & 0x0F));
            ula_ng_write(u, ULA_NG_REG_PAL_LO,  (uint8_t)(u->vdu_params[1] & 0x0F));
            ula_ng_write(u, ULA_NG_REG_PAL_HI,
                (uint8_t)(((u->vdu_params[2] & 0x0F) << 4) | (u->vdu_params[3] & 0x0F)));
            break;
        case 18: {                                  /* fond couleur par cellule : attr // + fill */
            uint8_t mode = u->regs[ULA_NG_REG_MODE - ULA_NG_WINDOW_LO] | ULA_NG_MODE_ATTR;
            ula_ng_write(u, ULA_NG_REG_MODE, mode);
            ula_ng_write(u, ULA_NG_REG_ATTR_FILL, u->vdu_params[0]);
            break;
        }
        case 31: {                                  /* colorer une cellule (col,row,attr) */
            uint8_t col = u->vdu_params[0], row = u->vdu_params[1], a = u->vdu_params[2];
            uint8_t mode = u->regs[ULA_NG_REG_MODE - ULA_NG_WINDOW_LO] | ULA_NG_MODE_ATTR;
            ula_ng_write(u, ULA_NG_REG_MODE, mode);
            if (col < 40 && row < 25) {
                for (int sc = row * 8; sc < row * 8 + 8 && sc < 200; sc++)
                    u->attr[sc * 40 + col] = a;
            }
            break;
        }
        default: break;                             /* commande inconnue : ignorée */
    }
}

static void vdu_feed(ula_ng_t* u, uint8_t b) {
    if (u->vdu_upload > 0) {                         /* upload en cours : octet = motif sprite */
        u->sprites[u->spr_sel].pattern[u->spr_wp] = (uint8_t)(b & 0x07);
        u->spr_wp = (uint16_t)((u->spr_wp + 1) % ULA_NG_SPR_PIXELS);
        u->vdu_upload--;
        return;
    }
    if (u->vdu_need == 0) {                          /* attend un code de commande */
        u->vdu_cmd = b;
        u->vdu_got = 0;
        u->vdu_need = vdu_need_for(b);
        if (u->vdu_need == 0) vdu_exec(u);           /* commande sans paramètre */
    } else {                                         /* octet de paramètre */
        if (u->vdu_got < ULA_NG_VDU_MAXPARAMS)
            u->vdu_params[u->vdu_got] = b;
        u->vdu_got++;
        if (u->vdu_got >= u->vdu_need) {
            vdu_exec(u);
            u->vdu_need = 0;
        }
    }
}

int ula_ng_write(ula_ng_t* u, uint16_t addr, uint8_t value) {
    if (!u->unlocked) {
        /* Verrouillé : surveiller la séquence 'N','G' sur $0340. Toute autre
         * écriture dans la fenêtre entre les deux octets casse la séquence
         * (SPEC §3). Ne JAMAIS consommer l'écriture → passthrough VIA
         * (indiscernable, bit-à-bit). */
        if (addr == ULA_NG_REG_LOCK) {
            if (value == ULA_NG_UNLOCK_N) {
                u->unlock_step = 1;
            } else if (value == ULA_NG_UNLOCK_G && u->unlock_step == 1) {
                u->unlocked = true;
                u->unlock_step = 0;
            } else {
                u->unlock_step = 0;
            }
        } else {
            u->unlock_step = 0;   /* autre écriture fenêtre : séquence rompue */
        }
        return 0;                 /* passthrough VIA */
    }

    /* Déverrouillé : ULA-NG possède la fenêtre. NG_ID/NG_IDCHK sont en lecture
     * seule. */
    if (addr != ULA_NG_REG_LOCK && addr != ULA_NG_REG_IDCHK) {
        u->regs[addr - ULA_NG_WINDOW_LO] = value;

        switch (addr) {
        case ULA_NG_REG_PAL_IDX:
            u->pal_idx = value & 0x0F;
            break;
        case ULA_NG_REG_PAL_LO:                 /* 0000RRRR : latch R */
            u->pal_r = value & 0x0F;
            break;
        case ULA_NG_REG_PAL_HI: {               /* GGGGBBBB : commit + auto-incr */
            uint8_t g = (uint8_t)((value >> 4) & 0x0F);
            uint8_t b = (uint8_t)(value & 0x0F);
            /* RGB444 → RGB888 par réplication de quartet (c8 = c4*0x11) */
            u->pal[u->pal_idx][0] = (uint8_t)(u->pal_r * 0x11);
            u->pal[u->pal_idx][1] = (uint8_t)(g * 0x11);
            u->pal[u->pal_idx][2] = (uint8_t)(b * 0x11);
            u->pal_idx = (uint8_t)((u->pal_idx + 1) & 0x0F);
            break;
        }
        case ULA_NG_REG_SCR_LO:                 /* NG_SCRSTART LSB */
            u->scrstart = (uint16_t)((u->scrstart & 0xFF00) | value);
            break;
        case ULA_NG_REG_SCR_HI:                 /* NG_SCRSTART MSB */
            u->scrstart = (uint16_t)((u->scrstart & 0x00FF) | (value << 8));
            break;
        case ULA_NG_REG_SCROLLX:                /* décalage fin X, cellule 6 px */
            u->scrollx = (uint8_t)(value > 5 ? 5 : value);
            break;
        case ULA_NG_REG_SCROLLY:                /* décalage fin Y, cellule 8 px */
            u->scrolly = (uint8_t)(value & 0x07);
            break;
        case ULA_NG_REG_COP_CTRL:               /* reset de la liste copper */
            u->copper_count = 0;
            u->copper_phase = 0;
            break;
        case ULA_NG_REG_COP_DATA:               /* flux 3 o/entrée (§5.4) */
            if (u->copper_phase == 0) {
                u->cop_line = value;
                u->copper_phase = 1;
            } else if (u->copper_phase == 1) {
                u->cop_index = (uint8_t)((value >> 4) & 0x0F);
                u->cop_r = (uint8_t)(value & 0x0F);
                u->copper_phase = 2;
            } else {                            /* GGGGBBBB : commit de l'entrée */
                uint8_t g = (uint8_t)((value >> 4) & 0x0F);
                uint8_t b = (uint8_t)(value & 0x0F);
                if (u->copper_count < ULA_NG_COP_MAX) {
                    u->copper[u->copper_count].line  = u->cop_line;
                    u->copper[u->copper_count].index = u->cop_index;
                    u->copper[u->copper_count].r = (uint8_t)(u->cop_r * 0x11);
                    u->copper[u->copper_count].g = (uint8_t)(g * 0x11);
                    u->copper[u->copper_count].b = (uint8_t)(b * 0x11);
                    u->copper_count++;
                }
                u->copper_phase = 0;
            }
            break;
        case ULA_NG_REG_ATTR_FILL:              /* remplit tout le plan + reset ptr */
            memset(u->attr, value, sizeof(u->attr));
            u->attr_wp = 0;
            break;
        case ULA_NG_REG_ATTR_DATA:              /* flux 1 o/cellule, auto-incrément */
            u->attr[u->attr_wp] = value;
            u->attr_wp = (uint16_t)((u->attr_wp + 1) % ULA_NG_ATTR_SIZE);
            break;
        case ULA_NG_REG_SPR_CTRL:               /* enable global sprites (§5.7) */
            u->spr_enable = (value & 0x01u) != 0;
            break;
        case ULA_NG_REG_SPR_SEL:                /* sélection sprite + reset ptr motif */
            u->spr_sel = value & (ULA_NG_SPRITES - 1);
            u->spr_wp = 0;
            break;
        case ULA_NG_REG_SPR_X:
            u->sprites[u->spr_sel].x = value;
            break;
        case ULA_NG_REG_SPR_Y:
            u->sprites[u->spr_sel].y = value;
            break;
        case ULA_NG_REG_SPR_ATTR:               /* b0 = visible */
            u->sprites[u->spr_sel].enable = (value & 0x01u) != 0;
            break;
        case ULA_NG_REG_SPR_DATA:               /* flux motif : 0=transparent, 1-7=index */
            u->sprites[u->spr_sel].pattern[u->spr_wp] = value & 0x07u;
            u->spr_wp = (uint16_t)((u->spr_wp + 1) % ULA_NG_SPR_PIXELS);
            break;
        case ULA_NG_REG_VDU:                    /* flux de commandes VDU (§VDU) */
            vdu_feed(u, value);
            break;
        case ULA_NG_REG_RASTER:                 /* NG_RASTERLINE */
            u->raster_line = value;
            break;
        case ULA_NG_REG_STATUS:                 /* acquit (b7=0) + enable (b0) */
            u->raster_enable = (value & ULA_NG_STATUS_EN) != 0;
            u->raster_pending = false;
            break;
        default:
            break;
        }
    }
    /* Recalcule les caches d'activation (unlocked && NG_MODE.bx). */
    {
        uint8_t mode = u->regs[ULA_NG_REG_MODE - ULA_NG_WINDOW_LO];
        u->active = u->unlocked && (mode & ULA_NG_MODE_ENABLE);   /* b0 : palette/copper/scroll/scrstart */
        u->attr_active = u->unlocked && (mode & ULA_NG_MODE_ATTR);/* b1 : attributs parallèles */
        u->spr_active = u->unlocked && u->spr_enable;             /* §5.7 : NG_SPR_CTRL.b0 */
        uint8_t vidmode = mode & ULA_NG_MODE_VIDMASK;             /* §5.8 : b2-3 */
        u->chunky_active = u->active && (vidmode == ULA_NG_VIDMODE_CHUNKY);
        u->text80_active = u->active && (vidmode == ULA_NG_VIDMODE_TEXT80);
    }
    return 1;                     /* consommée */
}

void ula_ng_scanline(ula_ng_t* u, int line) {
    /* Actif = déverrouillé + extensions actives (NG_MODE.b0). */
    if (!u->active) return;

    /* IRQ raster (§5.2) : niveau, reste jusqu'à acquittement. */
    if (u->raster_enable && line == (int)u->raster_line) {
        u->raster_pending = true;
    }

    /* Palette par scanline (§5.4) : applique les entrées copper de cette ligne
     * à la LUT (le hook vidéo relit u->pal la ligne suivante). */
    for (uint8_t i = 0; i < u->copper_count; i++) {
        if (u->copper[i].line == (uint8_t)line) {
            uint8_t idx = u->copper[i].index & 0x0F;
            u->pal[idx][0] = u->copper[i].r;
            u->pal[idx][1] = u->copper[i].g;
            u->pal[idx][2] = u->copper[i].b;
        }
    }
}

bool ula_ng_irq(const ula_ng_t* u) {
    return u->raster_pending;
}

void ula_ng_composite_scanline(ula_ng_t* u, uint8_t* fb, int w, int h, int y) {
    if (!u || !u->spr_active || !fb) return;
    if (y < 0 || y >= h) return;
    if (w > ULA_NG_SPR_MAXW) w = ULA_NG_SPR_MAXW;

    /* Occupancy de la scanline : marque les pixels déjà couverts par un sprite
     * opaque → collision sprite-sprite quand deux sprites se recouvrent. */
    uint8_t occ[ULA_NG_SPR_MAXW];
    memset(occ, 0, (size_t)w);

    /* Dessin des sprites 15→0 : le sprite 0 est écrit en dernier → au-dessus
     * (priorité par index). L'occupancy détecte le recouvrement quel que soit
     * l'ordre. */
    for (int s = ULA_NG_SPRITES - 1; s >= 0; s--) {
        if (!u->sprites[s].enable) continue;
        int sy = (int)u->sprites[s].y;
        int row = y - sy;                         /* ligne du sprite couvrant y */
        if (row < 0 || row >= ULA_NG_SPR_DIM) continue;
        int sx = (int)u->sprites[s].x;
        const uint8_t* pat = &u->sprites[s].pattern[row * ULA_NG_SPR_DIM];
        for (int col = 0; col < ULA_NG_SPR_DIM; col++) {
            uint8_t idx = pat[col];
            if (idx == 0) continue;               /* transparent */
            int x = sx + col;
            if (x < 0 || x >= w) continue;        /* clip horizontal */
            if (occ[x]) u->spr_collision = true;  /* recouvrement sprite-sprite */
            occ[x] = 1;
            int off = (y * w + x) * 3;
            fb[off + 0] = u->pal[idx][0];
            fb[off + 1] = u->pal[idx][1];
            fb[off + 2] = u->pal[idx][2];
        }
    }
}
