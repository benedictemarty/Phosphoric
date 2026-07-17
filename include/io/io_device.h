/**
 * @file io_device.h
 * @brief Contrat « périphérique de bus I/O » — abstraction du dispatch page 3.
 *
 * Objectif architectural : sortir de main.c la cascade de `if (has_X &&
 * X_addr_in_range(addr)) return X_read(...)` (25 périphériques câblés en dur)
 * au profit d'une **table de périphériques** que le dispatch parcourt.
 * Ajouter/retirer un périphérique = enregistrer/retirer une entrée, sans
 * toucher au cœur (cf docs/architecture/io-bus.md).
 *
 * Les fonctions reçoivent le contexte `emulator_t*` (et non un simple `self`)
 * parce que certains `claims` sont conditionnels et croisés (ACIA↔Microdisc,
 * fiabilité MIA du LOCI, etc.) : ils ont besoin de voir les autres sous-systèmes.
 *
 * Ne concerne QUE les périphériques qui revendiquent une plage d'adresses
 * (Microdisc, ACIA, LOCI, DTL2000, Mageco, ULA-NG). Les périphériques
 * « attachés à un port » (joystick/PSG, imprimante/VIA, cassette/VIA) gardent
 * leur modèle de callback de port ; le cœur (CPU/mémoire/VIA/ULA/PSG) n'est
 * pas un périphérique.
 */
#ifndef IO_DEVICE_H
#define IO_DEVICE_H

#include <stdint.h>
#include <stdbool.h>

struct emulator_s;   /* contexte complet (forward-decl : évite le cycle d'includes) */

typedef struct io_device_s {
    const char* name;
    /** Vrai si le périphérique possède cette adresse *maintenant* (présence +
     *  plage + éventuelles conditions croisées). */
    bool    (*claims)(struct emulator_s* emu, uint16_t addr);
    /** Lecture d'un octet à `addr` (appelée uniquement si claims() a renvoyé vrai). */
    uint8_t (*read)(struct emulator_s* emu, uint16_t addr);
    /** Écriture d'un octet à `addr` (idem). */
    void    (*write)(struct emulator_s* emu, uint16_t addr, uint8_t value);
} io_device_t;

#endif /* IO_DEVICE_H */
