/**
 * @file ula_ng.c
 * @brief ULA-NG étape 1 : déverrouillage NG_LOCK/NG_ID + garde verrou.
 * @author bmarty <bmarty@mailo.com>
 *
 * Cf docs/ula-ng/AUDIT.md (§0) et ULA-NG-SPEC.md (§3, §4.1).
 */

#include "io/ula_ng.h"
#include <string.h>

void ula_ng_reset(ula_ng_t* u) {
    u->unlocked = false;
    u->unlock_step = 0;
    memset(u->regs, 0, sizeof(u->regs));
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
     * seule ; les autres registres sont stockés (effets par registre ajoutés
     * aux étapes suivantes). */
    if (addr != ULA_NG_REG_LOCK && addr != ULA_NG_REG_IDCHK) {
        u->regs[addr - ULA_NG_WINDOW_LO] = value;
    }
    return 1;                     /* consommée */
}
