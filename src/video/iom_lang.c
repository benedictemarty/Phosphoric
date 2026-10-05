/* SPDX-License-Identifier: EUPL-1.2 */
/**
 * @file iom_lang.c
 * @brief Langue du menu F1 : table français → anglais. Voir iom_lang.h.
 * @author bmarty <bmarty@mailo.com>
 *
 * Une entrée = la chaîne française exacte telle qu'affichée ou passée à
 * snprintf (mêmes spécificateurs %, dans le même ordre, côté anglais).
 */
#include "video/iom_lang.h"

#include <string.h>
#include <strings.h>

typedef struct { const char* fr; const char* en; } iom_tr_t;

static const iom_tr_t k_tr[] = {
    /* ── En-tête, pied, boutons ── */
    { "Périphériques E/S   v%s", "I/O devices   v%s" },
    { "Redémarrer (RESET)", "Restart (RESET)" },
    { "Enregistrer la configuration", "Save configuration" },
    { "Reprendre", "Resume" },
    { "Langue FR", "Language EN" },
    { "F1 : ouvrir / fermer ce menu", "F1: open / close this menu" },
    { " Flèches ", " Arrows " },
    { " Entrée ", " Enter " },
    { " Suppr ", " Del " },
    { " Échap ", " Esc " },
    { " Lettre ", " Letter " },
    { " Clavier ", " Keyboard " },
    { "choisir", "select" },
    { "activer", "activate" },
    { "éjecter", "eject" },
    { "reprendre", "resume" },
    { "valider", "confirm" },
    { "aller à", "jump to" },
    { "retour", "back" },
    { "détails", "details" },
    { "modifier", "change" },
    { "défaut", "default" },
    { "saisir", "type" },
    { "effacer", "clear" },
    { "annuler", "cancel" },

    /* ── Disquettes et cassette ── */
    { "Disquettes — %s", "Floppies — %s" },
    { "Disquettes (pas d'interface disque)", "Floppies (no disk interface)" },
    { "Disquettes — menu du LOCI (F8)", "Floppies — LOCI menu (F8)" },
    { "Disquettes : menu du LOCI (bouton MENU, F8)", "Floppies: LOCI menu (MENU button, F8)" },
    { "— absent —", "— absent —" },
    { "— vide —", "— empty —" },
    { "— géré par le LOCI —", "— handled by the LOCI —" },
    { "— pas de cassette —", "— no tape —" },
    { "protégée", "protected" },
    { "écriture", "writable" },
    { "Lecteur absent sur cette interface", "No such drive on this interface" },
    { "Pas d'interface disque (--disk-rom ou --jasmin-rom au lancement)",
      "No disk interface (--disk-rom or --jasmin-rom at launch)" },
    { "Disquette pour le lecteur %c", "Floppy for drive %c" },
    { "Cassette (la même : rembobinée)", "Tape (the same one: rewound)" },
    { "Instantanés (reprendre : la machine revient à cet instant)",
      "Snapshots (resume: the machine goes back to that moment)" },
    { "Éjecter la disquette", "Eject the floppy" },
    { "Éjecter la cassette", "Eject the tape" },
    { "Enregistrer un nouvel instantané", "Save a new snapshot" },
    { "Aucun fichier (vide)", "No file (empty)" },
    { "Aucun fichier (dossiers roms, roms/loci, disks, tapes, .)",
      "No file (folders roms, roms/loci, disks, tapes, .)" },
    { "Aucune image .dsk (dossiers disks, tapes, snapshots, .)",
      "No .dsk image (folders disks, tapes, snapshots, .)" },
    { "Aucune cassette .tap (dossiers tapes, disks, .)", "No .tap tape (folders tapes, disks, .)" },
    { "Aucun instantané .ost (dossiers snapshots, .)", "No .ost snapshot (folders snapshots, .)" },
    { "Image déjà dans le lecteur %c", "Image already in drive %c" },
    { "en %c", "in %c" },
    { "%u,%u Mo", "%u.%u MB" },
    { "%u Ko", "%u KB" },
    { "%s : %s", "%s: %s" },

    /* ── Périphériques ── */
    { "Périphériques", "Devices" },
    { "Instantanés", "Snapshots" },
    { "enregistrer ou reprendre la machine", "save or resume the machine" },
    { "Imprimante", "Printer" },
    { "coupée", "off" },
    { "texte (LPRINT, LLIST)", "text (LPRINT, LLIST)" },
    { "traceur MCP-40", "MCP-40 plotter" },
    { "Joystick", "Joystick" },
    { "aucun", "none" },
    { "clavier (flèches)", "keyboard (arrows)" },
    { "manette", "gamepad" },
    { "Clavier", "Keyboard" },
    { "Cassette", "Tape" },
    { "injection directe (-f)", "direct injection (-f)" },
    { "CLOAD par la ROM corrigée", "CLOAD through the patched ROM" },
    { "au lancement", "at launch" },

    /* ── Cartes : liste et page d'une carte ── */
    { "Cartes d'extension", "Expansion cards" },
    { "Cartes d'extension — Entrée : choisir, régler", "Expansion cards — Enter: choose, set up" },
    { "Cartes d'extension (lecture seule)", "Expansion cards (read-only)" },
    { "présente", "present" },
    { "absente", "absent" },
    { "toujours", "always" },
    { "toujours présente", "always present" },
    { "Carte", "Card" },
    { "La carte", "The card" },
    { "Rôle de la carte", "What the card does" },
    { "(une seule carte « %s »)", "(one « %s » card only)" },
    { "disque", "disk" },
    { "midi", "midi" },
    { "Appliquer et redémarrer", "Apply and restart" },
    { "Annuler les changements", "Cancel the changes" },
    { "Relance l'émulateur avec ces cartes (redémarrage à froid : la mémoire est "
      "effacée). Les cartes marquées * ont changé. « Enregistrer la configuration » "
      "les garde pour les prochains lancements.",
      "Restarts the emulator with these cards (cold start: memory is cleared). Cards "
      "marked * have changed. « Save configuration » keeps them for the next launches." },
    { "Revient aux cartes de la machine en cours.", "Goes back to the cards of the running machine." },
    { "Toujours présente : rien à régler ici.", "Always present: nothing to set up here." },
    { "Entrée : présente / absente.", "Enter: present / absent." },
    { "Entrée : présente / absente. Une seule carte du groupe "
      "« %s » à la fois : en choisir une retire l'autre.",
      "Enter: present / absent. Only one « %s » card at a time: choosing one removes the other." },
    { "Par défaut : %s.  Entrée : %s.  Suppr : défaut.", "Default: %s.  Enter: %s.  Del: default." },
    { "Par défaut : %s.  Entrée : %s.  Suppr : valeur par défaut.",
      "Default: %s.  Enter: %s.  Del: default value." },
    { "vide", "empty" },
    { "oui / non", "yes / no" },
    { "choisir le fichier", "choose the file" },
    { "oui", "yes" },
    { "non", "no" },
    { "%s (détecté)", "%s (detected)" },
    { "%s (détectée)", "%s (detected)" },
    { "aucun détecté", "none detected" },
    { "aucune détectée", "none detected" },
    { "Carte toujours présente", "Card always present" },
    { "Cartes : retour à la machine en cours", "Cards: back to the running machine" },
    { "Aucun changement de cartes", "No card change" },
    { "Version web : les cartes se choisissent au lancement", "Web version: cards are chosen at launch" },
    { "Adresse d'E/S hexadécimale entre 0300 et 03FF", "Hexadecimal I/O address between 0300 and 03FF" },
    /* Conflits (cards.c) */
    { "%s et %s se chevauchent en $%04X", "%s and %s overlap at $%04X" },
    { "LOCI : mode « %s » absent de ce binaire", "LOCI: mode « %s » not in this binary" },
    { "LOCI firmware : indiquer le fichier ELF du firmware", "LOCI firmware: give the firmware ELF file" },
    { "LOCI-USB : aucune détectée (indiquer le port)", "LOCI-USB: none detected (give the port)" },
    { "Modem LOCI et ACIA 6551 : une seule ligne série à la fois",
      "LOCI modem and ACIA 6551: one serial line at a time" },
    { "Modem LOCI réel : aucun picowifi USB détecté (indiquer le port)",
      "Real LOCI modem: no USB picowifi detected (give the port)" },

    /* ── Fiches des cartes (cards/card_*.c) ── */
    { "Contrôleur de disquettes Oric (WD1793), 4 lecteurs 3\". Les disquettes s'insèrent ensuite "
      "dans la section Disquettes du menu.",
      "Oric floppy controller (WD1793), 4 3\" drives. Floppies are then inserted in the Floppies "
      "section of the menu." },
    { "ROM du contrôleur", "Controller ROM" },
    { "Micrologiciel du Microdisc (microdis.rom) : démarrage du DOS et accès disque.",
      "Microdisc firmware (microdis.rom): DOS boot and disk access." },
    { "Contrôleur de disquettes Jasmin (WD177x), autre standard Oric (TDOS).",
      "Jasmin floppy controller (WD177x), the other Oric standard (TDOS)." },
    { "ROM de démarrage", "Boot ROM" },
    { "ROM de démarrage Jasmin (2 Ko, jasmin.rom), servie en $F800.",
      "Jasmin boot ROM (2 KB, jasmin.rom), served at $F800." },
    { "Cartouche LOCI : menu de fichiers, émulation Microdisc et cassette depuis une carte SD ou "
      "une clé USB, ACIA en $0380. Modèle intégré, vrai firmware co-simulé ou LOCI-USB (Feather) "
      "branchée sur ce PC.",
      "LOCI cartridge: file menu, Microdisc and tape emulation from an SD card or a USB stick, "
      "ACIA at $0380. Built-in model, co-simulated real firmware or a LOCI-USB (Feather) plugged "
      "into this PC." },
    { "Mode", "Mode" },
    { "intégré : LOCI émulée par Phosphoric. firmware : le vrai firmware RP2040 tourne dans "
      "l'émulateur (développement). usb : une LOCI-USB (Feather RP2040) branchée sur ce PC fait "
      "tourner le firmware, Phosphoric joue l'Oric par l'USB. Seuls les modes présents dans ce "
      "binaire sont proposés.",
      "built-in: LOCI emulated by Phosphoric. firmware: the real RP2040 firmware runs inside the "
      "emulator (development). usb: a LOCI-USB (Feather RP2040) plugged into this PC runs the "
      "firmware, Phosphoric plays the Oric over USB. Only the modes in this binary are offered." },
    { "intégré", "built-in" },
    { "Démarrer sur le menu LOCI", "Start on the LOCI menu" },
    { "oui : l'Oric démarre sur le menu de la carte (ROM roms/loci/locirom) ; non : BASIC direct, "
      "LOCI reste disponible.",
      "yes: the Oric starts on the card's menu (ROM roms/loci/locirom); no: straight to BASIC, "
      "LOCI stays available." },
    { "Image de carte SD", "SD card image" },
    { "Image FAT16/32 lue par LOCI : ses .dsk et .tap apparaissent dans son menu. Vide : aucune.",
      "FAT16/32 image read by LOCI: its .dsk and .tap files show up in its menu. Empty: none." },
    { "Dossier flash interne", "Internal flash folder" },
    { "Dossier de l'hôte servant de mémoire flash interne (fichiers 0: du LOCI). Vide : aucun.",
      "Host folder used as internal flash memory (LOCI 0: files). Empty: none." },
    { "Modem Wi-Fi (picowifi)", "Wi-Fi modem (picowifi)" },
    { "Modem de la carte, vu par l'Oric comme l'ACIA en $0380. simulé : picowifi émulé (commandes "
      "AT, Wi-Fi par le réseau de l'hôte) ; réel : le picowifi USB branché sur ce PC. Remplace la "
      "carte ACIA 6551.",
      "The card's modem, seen by the Oric as the ACIA at $0380. simulated: emulated picowifi (AT "
      "commands, Wi-Fi through the host network); real: the USB picowifi plugged into this PC. "
      "Replaces the ACIA 6551 card." },
    { "simulé", "simulated" },
    { "réel", "real" },
    { "Port du picowifi réel", "Real picowifi port" },
    { "Port série du picowifi réel (ex. /dev/ttyACM0). Vide : détection automatique par son nom "
      "USB « PicoWifiModemUSB » (Linux).",
      "Serial port of the real picowifi (e.g. /dev/ttyACM0). Empty: detected automatically from "
      "its USB name « PicoWifiModemUSB » (Linux)." },
    { "Firmware (ELF)", "Firmware (ELF)" },
    { "Fichier loci-firmware.elf compilé pour RP2040 (obligatoire).",
      "loci-firmware.elf file built for RP2040 (required)." },
    { "Image flash du firmware", "Firmware flash image" },
    { "Mémoire flash persistante du firmware ; vide : <ELF>.flash ; « - » : volatile.",
      "Persistent flash memory of the firmware; empty: <ELF>.flash; « - »: volatile." },
    { "Image de clé USB", "USB stick image" },
    { "Image FAT servie au firmware comme clé USB ; vide : aucune.",
      "FAT image served to the firmware as a USB stick; empty: none." },
    { "Port de la LOCI-USB", "LOCI-USB port" },
    { "Port série de la LOCI-USB (Feather, ex. /dev/ttyACM0). Vide : détection automatique par son "
      "nom USB « LOCI-USB » (Linux). Bouton MENU : F8.",
      "Serial port of the LOCI-USB (Feather, e.g. /dev/ttyACM0). Empty: detected automatically "
      "from its USB name « LOCI-USB » (Linux). MENU button: F8." },
    { "Port série (MOS 6551) : modem, terminal, Minitel, BBS. Avec LOCI, l'ACIA de la carte est en $0380.",
      "Serial port (MOS 6551): modem, terminal, Minitel, BBS. With LOCI, the card's ACIA is at $0380." },
    { "Ligne série", "Serial line" },
    { "Où va la ligne : loopback (écho local), tcp:hôte:port, modem:hôte:port (appels entrants), "
      "pty (pseudo-terminal), com:bauds,bits,parité,stop,périphérique (port série réel), "
      "file:entrée[:sortie], picowifi[:ssid[:mot_de_passe]] (modem Wi-Fi émulé).",
      "Where the line goes: loopback (local echo), tcp:host:port, modem:host:port (incoming calls), "
      "pty (pseudo-terminal), com:baud,bits,parity,stop,device (real serial port), "
      "file:input[:output], picowifi[:ssid[:password]] (emulated Wi-Fi modem)." },
    { "Adresse d'E/S", "I/O address" },
    { "031C : carte série Oric standard ; 0380 : ACIA de la carte LOCI.",
      "031C: standard Oric serial card; 0380: ACIA of the LOCI card." },
    { "Vitesse (bauds)", "Speed (baud)" },
    { "Cadence réaliste quand l'ACIA prend son horloge à l'extérieur ; vide : transfert instantané.",
      "Realistic pacing when the ACIA takes an external clock; empty: instant transfer." },
    { "Tampon de réception", "Receive buffer" },
    { "N octets mis en attente à l'arrivée (évite de perdre des octets) ; vide : aucun.",
      "N bytes held on arrival (avoids losing bytes); empty: none." },
    { "Mode V23 (1200/75)", "V23 mode (1200/75)" },
    { "oui : vitesses asymétriques du Minitel et de Prestel.",
      "yes: asymmetric speeds of the Minitel and Prestel." },
    { "Modem Digitelec DTL 2000 (PIA 6821 + ACIA 6850) : ligne V23 brute vers un serveur Minitel.",
      "Digitelec DTL 2000 modem (PIA 6821 + ACIA 6850): raw V23 line to a Minitel server." },
    { "Ligne V23", "V23 line" },
    { "Où va la ligne : loopback (écho local), tcp:hôte:port, modem:hôte:port (appels entrants), "
      "pty (pseudo-terminal), com:bauds,bits,parité,stop,périphérique (port série réel), "
      "file:entrée[:sortie].",
      "Where the line goes: loopback (local echo), tcp:host:port, modem:host:port (incoming calls), "
      "pty (pseudo-terminal), com:baud,bits,parity,stop,device (real serial port), "
      "file:input[:output]." },
    { "Adresse de base de la carte (03F8 par défaut).", "Base address of the card (03F8 by default)." },
    { "Interface MIDI Mageco (ACIA 6850 à 31250 bauds) : piloter un synthétiseur ou jouer un "
      "fichier .mid dans l'Oric.",
      "Mageco MIDI interface (ACIA 6850 at 31250 baud): drive a synthesizer or play a .mid file "
      "in the Oric." },
    { "Liaison MIDI", "MIDI link" },
    { "loopback, midi[:cible] (MIDI temps réel, build MIDI=1), smf:fichier.mid[:loop] (rejoue un "
      "fichier MIDI), tcp:hôte:port, file:entrée[:sortie].",
      "loopback, midi[:target] (real-time MIDI, MIDI=1 build), smf:file.mid[:loop] (replays a "
      "MIDI file), tcp:host:port, file:input[:output]." },
    { "Adresse de base (03FE par défaut ; la MEA8000 utilise aussi 03FE).",
      "Base address (03FE by default; the MEA8000 also uses 03FE)." },
    { "Variante MIDI ORICON (MC6850 en $031C-$031D, générateur d'horloge $031E-$031F, compatible LOCI).",
      "ORICON MIDI variant (MC6850 at $031C-$031D, clock generator $031E-$031F, LOCI compatible)." },
    { "loopback, midi[:cible], smf:fichier.mid[:loop], tcp:hôte:port, file:entrée[:sortie].",
      "loopback, midi[:target], smf:file.mid[:loop], tcp:host:port, file:input[:output]." },
    { "Synthétiseur vocal Mageco (GI SP0256-AL2, allophones), mixé au son de l'Oric.",
      "Mageco speech synthesizer (GI SP0256-AL2, allophones), mixed with the Oric sound." },
    { "ROM d'allophones", "Allophone ROM" },
    { "ROM du SP0256-AL2 (al2.bin) : les 64 sons de base de la parole.",
      "SP0256-AL2 ROM (al2.bin): the 64 basic speech sounds." },
    { "Adresse du port (03F1 par défaut).", "Port address (03F1 by default)." },
    { "Synthétiseur vocal TMPI (Philips MEA 8000, synthèse par formants, sans ROM).",
      "TMPI speech synthesizer (Philips MEA 8000, formant synthesis, no ROM)." },
    { "Adresse de base (03FE par défaut ; Mageco MIDI utilise aussi 03FE).",
      "Base address (03FE by default; Mageco MIDI also uses 03FE)." },
    { "Hôte (hostfs)", "Host (hostfs)" },
    { "Dossier de l'ordinateur monté dans l'Oric : ses fichiers sont lus et écrits directement.",
      "Computer folder mounted in the Oric: its files are read and written directly." },
    { "Dossier monté", "Mounted folder" },
    { "Dossier de l'hôte vu par l'Oric.", "Host folder seen by the Oric." },
    { "ULA de nouvelle génération (palette, modes étendus), toujours présente ; un programme la "
      "déverrouille par $0340.",
      "Next-generation ULA (palette, extended modes), always present; a program unlocks it "
      "through $0340." },

    /* ── Messages (iomenu_glue.c) ── */
    { "pas d'interface disque (--disk-rom ou --jasmin-rom)", "no disk interface (--disk-rom or --jasmin-rom)" },
    { "se monte depuis le menu du LOCI (MENU, F8)", "mounted from the LOCI menu (MENU, F8)" },
    { "lecteur absent sur cette interface", "no such drive on this interface" },
    { "déjà vide", "already empty" },
    { "fichier illisible", "unreadable file" },
    { "Imprimante texte → impression.txt (LPRINT, LLIST)", "Text printer → impression.txt (LPRINT, LLIST)" },
    { "Impossible d'ouvrir impression.txt", "Cannot open impression.txt" },
    { "Traceur MCP-40 → traceur.bmp (écrit à la coupure)", "MCP-40 plotter → traceur.bmp (written when switched off)" },
    { "Imprimante coupée", "Printer off" },
    { "Joystick : flèches du clavier + tir", "Joystick: keyboard arrows + fire" },
    { "Joystick : manette branchée", "Joystick: gamepad plugged in" },
    { "Joystick : manette (aucune détectée, en attente)", "Joystick: gamepad (none detected, waiting)" },
    { "Joystick : manette (build sans SDL2)", "Joystick: gamepad (build without SDL2)" },
    { "Joystick : aucun", "Joystick: none" },
    { "Redémarrage avec les nouvelles cartes…", "Restarting with the new cards…" },
    { "Lecteur %c : %s", "Drive %c: %s" },
    { "Lecteur %c éjecté", "Drive %c ejected" },
    { "Lecteur %c : pas de protection par lecteur sur LOCI", "Drive %c: no per-drive protection on LOCI" },
    { "protégé en écriture", "write-protected" },
    { "écriture autorisée", "writable" },
    { "Cassette : %s (CLOAD\"\")", "Tape: %s (CLOAD\"\")" },
    { "Cassette : %s", "Tape: %s" },
    { "Cassette éjectée", "Tape ejected" },
    { "Pas de cassette", "No tape" },
    { "Cassette rembobinée", "Tape rewound" },
    { "Dossier snapshots/ inaccessible", "snapshots/ folder not accessible" },
    { "Instantané enregistré : %s", "Snapshot saved: %s" },
    { "Échec de l'enregistrement : %s", "Save failed: %s" },
    { "Instantané repris : %s", "Snapshot resumed: %s" },
    { "Instantané illisible : %s", "Unreadable snapshot: %s" },
    { "Clavier %s", "Keyboard %s" },
    { "Au prochain lancement : %s (enregistrer la configuration)", "At next launch: %s (save the configuration)" },
    { "injection directe", "direct injection" },
    { "CLOAD par la ROM", "CLOAD through the ROM" },
    { "Machine redémarrée (RESET)", "Machine restarted (RESET)" },
    { "Configuration enregistrée : %s", "Configuration saved: %s" },
    { "Impossible d'écrire %s", "Cannot write %s" },
    { "Menu en français", "Menu in English" },
};

static iom_lang_t g_lang = IOM_LANG_FR;

void iom_lang_set(iom_lang_t lang) { g_lang = lang == IOM_LANG_EN ? IOM_LANG_EN : IOM_LANG_FR; }
iom_lang_t iom_lang(void) { return g_lang; }

const char* iom_lang_code(iom_lang_t lang) { return lang == IOM_LANG_EN ? "en" : "fr"; }

bool iom_lang_parse(const char* code, iom_lang_t* out) {
    if (!code) return false;
    if (strcasecmp(code, "fr") == 0) { *out = IOM_LANG_FR; return true; }
    if (strcasecmp(code, "en") == 0) { *out = IOM_LANG_EN; return true; }
    return false;
}

const char* iom_tr(const char* fr) {
    if (g_lang == IOM_LANG_FR || !fr || !*fr) return fr;
    for (size_t i = 0; i < sizeof(k_tr) / sizeof(k_tr[0]); i++)
        if (strcmp(k_tr[i].fr, fr) == 0) return k_tr[i].en;
    return fr;
}

int iom_lang_entries(void) { return (int)(sizeof(k_tr) / sizeof(k_tr[0])); }

void iom_lang_entry(int i, const char** fr, const char** en) {
    *fr = k_tr[i].fr;
    *en = k_tr[i].en;
}
