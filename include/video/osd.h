/**
 * @file osd.h
 * @brief On-Screen Display — overlay de changement de média à chaud (cassette/disquette)
 * @author bmarty <bmarty@mailo.com>
 *
 * Overlay en surimpression (inspiré du file-requester d'Oricutron) permettant
 * de changer la cassette (.tap) ou la disquette (.dsk du lecteur A) sans
 * quitter l'émulateur. Découplé de SDL : dessine dans video_t.framebuffer et
 * reçoit des codes touches abstraits, donc testable en headless.
 */
#ifndef OSD_H
#define OSD_H

#include <stdint.h>
#include <stdbool.h>
#include "video/video.h"

#define OSD_MAX_ENTRIES 256
#define OSD_NAME_MAX    48
#define OSD_PATH_MAX    256
#define OSD_VISIBLE     16   /* lignes de liste affichées */

/* Codes touches abstraits (mappés depuis SDL par l'appelant) */
#define OSD_KEY_UP    1
#define OSD_KEY_DOWN  2
#define OSD_KEY_ENTER 3
#define OSD_KEY_ESC   4
#define OSD_KEY_LEFT  5   /* lecteur cible précédent (D <- A) */
#define OSD_KEY_RIGHT 6   /* lecteur cible suivant   (A -> D) */
#define OSD_KEY_EJECT 7   /* éjecter le disque du lecteur cible (Suppr) */

#define OSD_DRIVES    4   /* lecteurs disque cibles A..D */

typedef struct {
    char name[OSD_NAME_MAX];
    char path[OSD_PATH_MAX];
    bool is_disk;            /* true = .dsk (lecteur A), false = .tap (cassette) */
} osd_entry_t;

typedef struct {
    bool open;
    osd_entry_t entries[OSD_MAX_ENTRIES];
    int  count;
    int  selected;
    int  scroll;
    int  disk_drive;         /* lecteur cible pour un .dsk (0=A .. 3=D) */
    uint8_t font[128 * 8];   /* snapshot du charset Oric ($B400) */
    bool font_ready;
    char status[64];         /* message de la dernière action */
} osd_t;

/* Résultat d'une touche : ce que l'appelant doit faire ensuite. */
typedef enum {
    OSD_NONE = 0,
    OSD_ACTIVATE,            /* l'utilisateur a validé : charger entries[selected] */
    OSD_EJECT,               /* éjecter le disque du lecteur cible (disk_drive) */
    OSD_CLOSED               /* l'overlay vient de se fermer */
} osd_action_t;

void osd_init(osd_t* osd);

/* Copie le charset Oric depuis la RAM ($B400, mode texte) vers osd->font.
 * À appeler une fois par trame quand on est en mode texte (charset valide). */
void osd_snapshot_font(osd_t* osd, const uint8_t* mem);

/* Ouvre l'overlay et (re)scanne les dossiers de média. */
void osd_open(osd_t* osd);
void osd_close(osd_t* osd);
void osd_toggle(osd_t* osd);

/* Traite une touche (OSD_KEY_*). Retourne l'action à effectuer. */
osd_action_t osd_key(osd_t* osd, int key);

/* Dessine l'overlay dans le framebuffer. Sans effet si !osd->open. */
void osd_render(osd_t* osd, video_t* vid);

/* Construit la liste des médias depuis les dossiers donnés (NULL-terminé).
 * Exposé pour les tests ; osd_open() l'appelle avec les dossiers par défaut. */
void osd_scan(osd_t* osd, const char* const* dirs);

#endif /* OSD_H */
