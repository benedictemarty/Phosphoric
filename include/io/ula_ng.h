/**
 * @file ula_ng.h
 * @brief ULA-NG — ULA "next-gen" pour Oric (référence logicielle d'un futur
 *        portage Verilog sur Tang Primer 20K). Étape 1 : verrou/identité +
 *        plomberie page 3. Voir docs/ula-ng/AUDIT.md et ULA-NG-SPEC.md.
 * @author bmarty <bmarty@mailo.com>
 *
 * Module isolé aux frontières « miroir FPGA » : 3 interfaces seulement
 * (registres page 3, tick scanline — étapes ultérieures —, ligne d'IRQ).
 *
 * Fenêtre registres : $0340-$035F (libre dans la cartographie communautaire).
 * Au reset, ULA-NG est indiscernable d'une HCS10017 : verrouillée, la fenêtre
 * passe au VIA (lecture) et les écritures y tombent aussi (le module surveille
 * juste $0340 pour la séquence de déverrouillage) → non-régression bit-à-bit.
 * Après la séquence 'N','G' ($4E,$47) sur $0340, le module arme les extensions
 * et possède la fenêtre.
 */

#ifndef ULA_NG_H
#define ULA_NG_H

#include <stdint.h>
#include <stdbool.h>

#define ULA_NG_WINDOW_LO   0x0340u
#define ULA_NG_WINDOW_HI   0x035Fu
#define ULA_NG_REG_LOCK    0x0340u   /* NG_LOCK (W) / NG_ID (R) */
#define ULA_NG_REG_MODE    0x0341u   /* NG_MODE : b0 = extensions actives */
#define ULA_NG_REG_SCR_LO  0x0342u   /* NG_SCRSTART lo : base fetch vidéo (LSB) */
#define ULA_NG_REG_SCR_HI  0x0343u   /* NG_SCRSTART hi : base fetch vidéo (MSB) */
#define ULA_NG_REG_RASTER  0x0346u   /* NG_RASTERLINE : ligne déclenchant l'IRQ */
#define ULA_NG_REG_STATUS  0x0347u   /* NG_STATUS : R b7=IRQ raster en attente ;
                                        W = acquit + b0 = enable IRQ raster */
#define ULA_NG_REG_PAL_IDX 0x0348u   /* NG_PAL_IDX : index LUT (0-15), auto-incr */
#define ULA_NG_REG_PAL_LO  0x0349u   /* NG_PAL_DATA lo : 0000RRRR */
#define ULA_NG_REG_PAL_HI  0x034Au   /* NG_PAL_DATA hi : GGGGBBBB (commit + incr) */
#define ULA_NG_REG_COP_CTRL 0x034Bu  /* NG_COP_CTRL : écriture = reset liste copper */
#define ULA_NG_REG_COP_DATA 0x034Cu  /* NG_COP_DATA : flux 3 o/entrée (§5.4) :
                                        [0]=ligne [1]=(index<<4)|R [2]=(G<<4)|B */
#define ULA_NG_REG_IDCHK   0x034Fu   /* NG_IDCHK (R) = ~NG_ID (handshake) */
#define ULA_NG_COP_MAX     64        /* entrées max de la liste copper */
#define ULA_NG_STATUS_IRQ  0x80u     /* NG_STATUS b7 (R) : IRQ raster en attente */
#define ULA_NG_STATUS_EN   0x01u     /* NG_STATUS b0 (W) : enable IRQ raster */
#define ULA_NG_FRAME_LINES 312       /* lignes trame PAL complète (0-311) */
#define ULA_NG_VERSION     0x1Eu     /* NG_ID quand déverrouillé (v1.0) */
#define ULA_NG_UNLOCK_N    0x4Eu     /* 'N' */
#define ULA_NG_UNLOCK_G    0x47u     /* 'G' */
#define ULA_NG_MODE_ENABLE 0x01u     /* NG_MODE b0 */
#define ULA_NG_PAL_ENTRIES 16        /* LUT 16 entrées × 12 bits (RGB444) */

typedef struct ula_ng_s {
    bool    unlocked;      /* extensions armées (après la séquence) */
    uint8_t unlock_step;   /* 0 = repos, 1 = 'N' vu, attend 'G' */
    uint8_t regs[0x20];    /* fichier de registres $0340-$035F */

    /* Palette-indirection (§5.1) : LUT 16 entrées, stockée déjà étendue en
     * RGB888 (expansion RGB444→888 par réplication de quartet à l'écriture).
     * Au reset, les 8 premières entrées = couleurs Oric (identité → compat). */
    uint8_t pal[ULA_NG_PAL_ENTRIES][3];
    uint8_t pal_idx;       /* NG_PAL_IDX courant (0-15) */
    uint8_t pal_r;         /* quartet R latché, en attente de G/B */
    bool    active;    /* = unlocked && NG_MODE.b0 (gate général NG : hooks vidéo) */

    /* Start-address (§5.3) : remplace la base du fetch vidéo ($A000/$BB80).
     * 0 = utiliser la base par défaut du mode (compat). */
    uint16_t scrstart;

    /* IRQ raster (§5.2) */
    uint8_t raster_line;   /* NG_RASTERLINE : ligne (trame 0-311) déclenchant l'IRQ */
    bool    raster_enable; /* NG_STATUS.b0 : IRQ raster armée */
    bool    raster_pending;/* IRQ raster en attente (b7), jusqu'à acquittement */

    /* Palette par scanline (§5.4) : mini-copper. Liste d'entrées (ligne, index
     * LUT, couleur RGB888) appliquée par ula_ng_scanline pendant le hblank. */
    struct { uint8_t line, index, r, g, b; } copper[ULA_NG_COP_MAX];
    uint8_t copper_count;   /* nombre d'entrées programmées */
    uint8_t copper_phase;   /* 0/1/2 : octet courant de l'entrée en flux */
    uint8_t cop_line, cop_index, cop_r;  /* entrée partielle en cours d'assemblage */
} ula_ng_t;

/** Initialise (= reset : état verrouillé HCS10017, registres à 0). */
void ula_ng_init(ula_ng_t* u);

/** Reset matériel : re-verrouille tout et remet les registres à 0. */
void ula_ng_reset(ula_ng_t* u);

/** Vrai si l'adresse est dans la fenêtre registres $0340-$035F. */
static inline int ula_ng_addr_in_window(uint16_t addr) {
    return addr >= ULA_NG_WINDOW_LO && addr <= ULA_NG_WINDOW_HI;
}

/** Vrai si le module intercepte la fenêtre (déverrouillé). Sinon la lecture
 *  doit retomber sur le VIA (indiscernable). */
bool ula_ng_active(const ula_ng_t* u);

/** Lecture d'un registre (appelée uniquement quand ula_ng_active()). */
uint8_t ula_ng_read(ula_ng_t* u, uint16_t addr);

/** Écriture d'un registre. Renvoie 1 si CONSOMMÉE (ne pas retomber sur le VIA),
 *  0 sinon (verrouillé → passthrough VIA + surveillance silencieuse du verrou). */
int ula_ng_write(ula_ng_t* u, uint16_t addr, uint8_t value);

/** Tick de scanline (ligne trame complète 0-311). Lève l'IRQ raster (pending)
 *  quand line == NG_RASTERLINE, si armée (unlocked && NG_MODE.b0 && enable).
 *  À appeler par la boucle vidéo pour chaque ligne. */
void ula_ng_scanline(ula_ng_t* u, int line);

/** État de la ligne d'IRQ vers le 6502 (vrai = IRQ raster active). */
bool ula_ng_irq(const ula_ng_t* u);

#endif /* ULA_NG_H */
