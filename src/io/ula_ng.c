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
    u->copper_count = 0;
    u->copper_phase = 0;
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
        default:               return u->regs[addr - ULA_NG_WINDOW_LO];
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
    /* Recalcule le cache d'activation palette (unlocked && NG_MODE.b0). */
    u->active = u->unlocked &&
                    (u->regs[ULA_NG_REG_MODE - ULA_NG_WINDOW_LO] & ULA_NG_MODE_ENABLE);
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
