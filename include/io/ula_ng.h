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
#define ULA_NG_REG_PAL_IDX 0x0348u   /* NG_PAL_IDX : index LUT (0-15), auto-incr */
#define ULA_NG_REG_PAL_LO  0x0349u   /* NG_PAL_DATA lo : 0000RRRR */
#define ULA_NG_REG_PAL_HI  0x034Au   /* NG_PAL_DATA hi : GGGGBBBB (commit + incr) */
#define ULA_NG_REG_IDCHK   0x034Fu   /* NG_IDCHK (R) = ~NG_ID (handshake) */
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
    bool    pal_active;    /* = unlocked && NG_MODE.b0 (cache pour le hook vidéo) */
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

#endif /* ULA_NG_H */
