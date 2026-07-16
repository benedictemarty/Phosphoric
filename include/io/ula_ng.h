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
#define ULA_NG_REG_SCROLLX 0x0344u   /* NG_SCROLLX : décalage fin X (0-5 pixels) */
#define ULA_NG_REG_SCROLLY 0x0345u   /* NG_SCROLLY : décalage fin Y (0-7 pixels) */
#define ULA_NG_REG_RASTER  0x0346u   /* NG_RASTERLINE : ligne déclenchant l'IRQ */
#define ULA_NG_REG_STATUS  0x0347u   /* NG_STATUS : R b7=IRQ raster en attente ;
                                        W = acquit + b0 = enable IRQ raster */
#define ULA_NG_REG_PAL_IDX 0x0348u   /* NG_PAL_IDX : index LUT (0-15), auto-incr */
#define ULA_NG_REG_PAL_LO  0x0349u   /* NG_PAL_DATA lo : 0000RRRR */
#define ULA_NG_REG_PAL_HI  0x034Au   /* NG_PAL_DATA hi : GGGGBBBB (commit + incr) */
#define ULA_NG_REG_COP_CTRL 0x034Bu  /* NG_COP_CTRL : écriture = reset liste copper */
#define ULA_NG_REG_COP_DATA 0x034Cu  /* NG_COP_DATA : flux 3 o/entrée (§5.4) :
                                        [0]=ligne [1]=(index<<4)|R [2]=(G<<4)|B */
#define ULA_NG_REG_ATTR_FILL 0x034Du /* NG_ATTR_FILL : remplit tout le plan (§5.6) */
#define ULA_NG_REG_ATTR_DATA 0x034Eu /* NG_ATTR_DATA : flux 1 o/cellule (auto-incr) */
#define ULA_NG_REG_IDCHK   0x034Fu   /* NG_IDCHK (R) = ~NG_ID (handshake) */
#define ULA_NG_REG_SPR_CTRL 0x0350u  /* NG_SPR_CTRL : b0 = enable global sprites (§5.7) */
#define ULA_NG_REG_SPR_SEL  0x0351u  /* NG_SPR_SEL : sprite sélectionné 0-15 + reset ptr motif */
#define ULA_NG_REG_SPR_X    0x0352u  /* NG_SPR_X : position X (0-255) du sprite sélectionné */
#define ULA_NG_REG_SPR_Y    0x0353u  /* NG_SPR_Y : position Y (0-255) */
#define ULA_NG_REG_SPR_ATTR 0x0354u  /* NG_SPR_ATTR : b0 = sprite visible */
#define ULA_NG_REG_SPR_DATA 0x0355u  /* NG_SPR_DATA : flux motif 1 o/px (0=transparent, 1-7=index) */
#define ULA_NG_REG_SPR_STATUS 0x0356u/* NG_SPR_STATUS (R) : b7 = collision (clear on read) */
#define ULA_NG_COP_MAX     64        /* entrées max de la liste copper */
#define ULA_NG_MODE_ATTR   0x02u     /* NG_MODE b1 : attributs parallèles actifs */
#define ULA_NG_MODE_VIDMASK 0x0Cu    /* NG_MODE b2-3 : mode vidéo (§5.8) */
#define ULA_NG_VIDMODE_CHUNKY 0x04u  /* b2-3 = 01 : chunky 4bpp 160×200 */
#define ULA_NG_VIDMODE_TEXT80 0x08u  /* b2-3 = 10 : texte 80 colonnes */
#define ULA_NG_ATTR_SIZE   8192      /* plan d'attributs : 8 Ko (encre+papier/cellule) */
#define ULA_NG_STATUS_IRQ  0x80u     /* NG_STATUS b7 (R) : IRQ raster en attente */
#define ULA_NG_STATUS_EN   0x01u     /* NG_STATUS b0 (W) : enable IRQ raster */
#define ULA_NG_FRAME_LINES 312       /* lignes trame PAL complète (0-311) */
#define ULA_NG_VERSION     0x1Eu     /* NG_ID quand déverrouillé (v1.0) */
#define ULA_NG_UNLOCK_N    0x4Eu     /* 'N' */
#define ULA_NG_UNLOCK_G    0x47u     /* 'G' */
#define ULA_NG_MODE_ENABLE 0x01u     /* NG_MODE b0 */
#define ULA_NG_PAL_ENTRIES 16        /* LUT 16 entrées × 12 bits (RGB444) */
#define ULA_NG_SPRITES     16        /* nombre de sprites matériels (§5.7) */
#define ULA_NG_SPR_DIM     16        /* sprites 16×16 */
#define ULA_NG_SPR_PIXELS  (ULA_NG_SPR_DIM * ULA_NG_SPR_DIM) /* 256 px/sprite */
#define ULA_NG_SPR_MAXW    512       /* largeur framebuffer max (occupancy collision) */
#define ULA_NG_SPR_STATUS_COL 0x80u  /* NG_SPR_STATUS b7 : collision détectée */

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

    /* Scroll fin (§5.5) : décalage pixel-près à la composition. */
    uint8_t scrollx;       /* NG_SCROLLX : 0-5 (largeur cellule = 6 px) */
    uint8_t scrolly;       /* NG_SCROLLY : 0-7 (hauteur cellule = 8 px) */

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

    /* Attributs parallèles (§5.6) : plan encre+papier par cellule, hors des
     * 64 Ko du 6502 (miroir DDR3 FPGA). Actif si NG_MODE.b1. Indexé
     * (scanline*40 + col) ; octet = (paper<<3)|ink. */
    uint8_t attr[ULA_NG_ATTR_SIZE];
    uint16_t attr_wp;       /* pointeur d'écriture (auto-incrément) */
    bool    attr_active;    /* = unlocked && NG_MODE.b1 (cache pour le hook vidéo) */

    /* Sprites matériels (§5.7) : jusqu'à 16 sprites 16×16, motif indexé palette
     * (0 = transparent, 1-7 = index LUT). Table hors des 64 Ko du 6502 (miroir
     * DDR3 FPGA), programmée par flux via la fenêtre de registres. Composition
     * dans le pipeline de sortie (après le fond) ; priorité par index (0 devant) ;
     * détection de collision sprite-sprite. */
    struct {
        uint8_t x, y;                        /* position écran (px) */
        bool    enable;                      /* NG_SPR_ATTR.b0 : visible */
        uint8_t pattern[ULA_NG_SPR_PIXELS];  /* 0=transparent, 1-7=index palette */
    } sprites[ULA_NG_SPRITES];
    uint8_t  spr_sel;       /* sprite sélectionné pour la programmation */
    uint16_t spr_wp;        /* pointeur d'écriture motif (0-255, auto-incrément) */
    bool     spr_enable;    /* NG_SPR_CTRL.b0 : enable global */
    bool     spr_collision; /* collision sprite-sprite (b7 status), clear on read */
    bool     spr_active;    /* = unlocked && spr_enable (cache pour le hook vidéo) */

    /* Modes vidéo étendus (§5.8), sélectionnés par NG_MODE.b2-3 (caches pour le
     * latch vidéo de début de trame). chunky 4bpp = 160×200 16 couleurs (LUT
     * NG) ; texte 80 colonnes = charset RAM redéfinissable. Données lues depuis
     * NG_SCRSTART (défaut $A000). */
    bool     chunky_active; /* = active && NG_MODE.b2-3 == 01 */
    bool     text80_active; /* = active && NG_MODE.b2-3 == 10 */
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

/** Composite les sprites (§5.7) sur la scanline `y` du framebuffer RGB888
 *  (largeur `w`, hauteur `h`). Transparent = index 0 ; couleur = LUT palette NG.
 *  Priorité par index (sprite 0 devant). Met `spr_collision` sur recouvrement
 *  sprite-sprite. No-op si `!spr_active`. À appeler après le rendu du fond. */
void ula_ng_composite_scanline(ula_ng_t* u, uint8_t* fb, int w, int h, int y);

#endif /* ULA_NG_H */
