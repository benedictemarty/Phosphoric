/*
 * loci_emu.h — backend LOCI par ÉMULATION du vrai firmware RP2040.
 *
 * Alternative au backend comportemental (loci_core.c) : au lieu de ré-implémenter
 * chaque opcode MIA en C, on exécute le VRAI firmware LOCI dans un émulateur
 * RP2040 (bibliothèque ~/loci/emul, libemul.a). Activé par --loci-emu ELF.
 *
 * Étape 1 (ici) : OVERLAY ROM. Après amorçage opérationnel (BTN_A simulé), le
 * firmware désactive la ROM BASIC de l'Oric (nROMDIS) et SERT lui-même
 * $C000-$FFFF via son read-serve PIO+DMA. Phosphoric appelle loci_emu_rom_read()
 * depuis memory_read() pour laisser le VRAI firmware fournir la ROM de boot →
 * le 6502 démarre dans le menu LOCI réel (vecteur reset $FF4C).
 *
 * Étape 2 : fenêtre API MIA $0300-$03FF co-simulée (loci_emu_api_read/write) —
 * l'écriture est un dialogue bus complet (action SM + act_loop core1) ; la lecture
 * sert en O(1) l'octet iopage[addr] que servirait la DMA de read-serve.
 */
#ifndef LOCI_EMU_H
#define LOCI_EMU_H

#include <stdint.h>
#include <stdbool.h>

/* Instancie l'émulateur, charge le .elf et lance un THREAD ÉMULATEUR DÉDIÉ qui
 * boote le firmware puis le fait tourner en FREE-RUN continu (cœur hôte distinct
 * du 6502 de Phosphoric). Rend la main immédiatement (zéro attente au lancement) ;
 * LOCI reste TRANSPARENT tant que le bouton MENU n'est pas pressé. Renvoie 0 si OK.
 * loci_emu_active() passe à true quand le boot est terminé. L'accès à l'émulateur
 * est sérialisé par un mutex (les fonctions ci-dessous sont sûres depuis le thread
 * principal). */
int loci_emu_start(const char *elf_path);

/* Déclare l'image FAT servie comme disque USB émulé (drive « 1: »). À appeler AVANT
 * loci_emu_start (le montage a lieu juste après le boot). */
void loci_emu_set_usb_image(const char *path);

/* Arrête proprement le thread émulateur (optionnel — sûr de ne pas l'appeler). */
void loci_emu_stop(void);

/* Vrai quand le firmware a fini de booter (idle). Faux pendant le boot arrière-plan
 * → l'hôte laisse l'Oric servir sa propre ROM. */
bool loci_emu_active(void);

/* Simule l'appui sur le BOUTON MENU physique de LOCI : le firmware charge sa ROM
 * de boot, arme le service ($C000-$FFFF via nROMDIS) et pilote nRESET. Renvoie
 * true si le service est armé. L'appelant doit ENSUITE réinitialiser le 6502
 * (cpu_reset) pour qu'il redémarre dans le menu LOCI servi. */
bool loci_emu_menu_button(void);

/** @brief Appui LONG (≥ 2 s) sur le bouton MENU : le firmware boote sa ROM de
 *  diagnostic embarquée (test108k, EXT_BOOT_DIAG). Renvoie true si le service ROM
 *  est armé — l'hôte doit alors réinitialiser son 6502. false si le firmware n'a
 *  pas de ROM de diagnostic embarquée (build sans EMBEDDED_TEST108K_ROM). */
bool loci_emu_diag_button(void);

/* Overlay ROM co-sim : si le firmware sert cette adresse (nROMDIS actif +
 * $C000-$FFFF), écrit l'octet servi dans *out et renvoie true ; sinon renvoie
 * false (memory_read() garde alors la ROM/RAM Oric). À appeler AVANT de servir
 * la ROM interne dans memory_read(). */
bool loci_emu_rom_read(uint16_t address, uint8_t *out);

/* État des lignes de contrôle pilotées par le firmware (via l'expandeur I²C) :
 * renvoie 1 quand la ligne est active. Permet à l'hôte de refléter nROMDIS/
 * nRESET/nIRQ sur son 6502/ULA. */
void loci_emu_ext_lines(int *nirq, int *nreset, int *nromdis);

/* API MIA $03xx co-simulée (étape 2). À appeler quand loci_emu_active() : le VRAI
 * firmware traite l'accès (action SM pio0 sm1 + act_loop core1). L'écriture est un
 * dialogue bus complet ; la lecture rend l'octet servi (O(1)). Tant que le firmware
 * n'a pas fini de booter, write est ignorée et read renvoie 0xFF (bus flottant). */
void    loci_emu_api_write(uint16_t address, uint8_t value);
uint8_t loci_emu_api_read(uint16_t address);

/* Nombre de pulses nIRQ assertés par le firmware depuis le dernier appel (remis à
 * zéro). À appeler une fois par frame ; délivrer autant d'IRQ EDGE au 6502. */
int loci_emu_irq_take(void);

/* ── ACIA $0380-$0383 servie par le firmware réel (Phase 2 CDC) ──────
 * Quand un dongle CDC est attaché (--loci-cdc <dev>), l'ACIA 6551 de l'Oric est
 * servie par le VRAI firmware (oric/acia.c ↔ modem USB) au lieu du 6551
 * comportemental. `loci_emu_set_cdc_device` doit être appelé AVANT loci_emu_start
 * (l'attache a lieu après le boot). `loci_emu_acia_active` = booté ET dongle attaché.
 * `loci_emu_acia_tick` pompe l'échange CDC↔registres (RX asynchrone) — à appeler une
 * fois par frame. */
void    loci_emu_set_cdc_device(const char *path);
bool    loci_emu_acia_active(void);
void    loci_emu_acia_write(uint16_t address, uint8_t value);
uint8_t loci_emu_acia_read(uint16_t address);
uint8_t loci_emu_acia_peek(uint16_t address);   /* observation non destructive */
void    loci_emu_acia_tick(void);

/* Free-run BORNÉ (déterministe, thread principal) : avance le firmware de `steps`
 * pas RP2040 SANS piloter le bus (Phi2 maintenu HAUT → l'action SM reste parquée),
 * pour que les tâches de fond progressent et que les pulses nIRQ ASYNCHRONES
 * (timers, trap bouton) naissent entre deux transactions. À appeler une fois par
 * frame quand loci_emu_active(), AVANT loci_emu_irq_take(). No-op tant que le boot
 * n'est pas terminé. */
void loci_emu_tick(long steps);

/* Souris USB HID (co-sim). Le firmware co-simulé n'énumère pas l'USB : sans
 * ce pont, la souris de l'hôte n'atteint jamais `mou_report()` et le 6502 lit
 * un `mou_xram` vide (le chemin `loci_mou_report()` du modèle interne écrit
 * dans un xram que le 6502 ne lit plus dès que loci_emu_active()). Deltas
 * accumulés côté firmware, exactement comme un vrai rapport HID.
 * No-op si le boot n'est pas terminé. Renvoie true si le rapport a été appliqué. */
bool loci_emu_mou_report(uint8_t buttons, int8_t dx, int8_t dy,
                         int8_t wheel, int8_t pan);

/* true si le 6502 a armé la souris via XREG (sinon le firmware n'écrit rien). */
bool loci_emu_mou_armed(void);

/* Clavier USB HID (co-sim). Appelle le VRAI kbd_report() du firmware — il fait
 * bien plus que remplir un bitmap (layouts, file stdio, répétition, LED), le
 * répliquer serait fragile. `keycodes` = 6 usages HID (0 = vide). */
bool loci_emu_kbd_report(uint8_t modifier, const uint8_t keycodes[6]);
bool loci_emu_kbd_armed(void);

#endif /* LOCI_EMU_H */
