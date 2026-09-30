/* SPDX-License-Identifier: EUPL-1.2 */
/**
 * @file osd.h
 * @brief On-Screen Display — hot media-swap overlay (tape/floppy)
 * @author bmarty <bmarty@mailo.com>
 *
 * Superimposed overlay (inspired by Oricutron's file requester) that allows
 * changing the tape (.tap) or the floppy (.dsk in drive A) without
 * leaving the emulator. Decoupled from SDL: draws into video_t.framebuffer and
 * receives abstract key codes, hence testable headless.
 */
#ifndef OSD_H
#define OSD_H

#include <stdint.h>
#include <stdbool.h>
#include "video/video.h"

#define OSD_MAX_ENTRIES 256
#define OSD_NAME_MAX    48
#define OSD_PATH_MAX    256
#define OSD_VISIBLE     16   /* list lines displayed */

/* Abstract key codes (mapped from SDL by the caller) */
#define OSD_KEY_UP    1
#define OSD_KEY_DOWN  2
#define OSD_KEY_ENTER 3
#define OSD_KEY_ESC   4
#define OSD_KEY_LEFT  5   /* previous target drive (D <- A) */
#define OSD_KEY_RIGHT 6   /* next target drive     (A -> D) */
#define OSD_KEY_EJECT 7   /* eject the disk from the target drive (Del) */

#define OSD_DRIVES    4   /* target disk drives A..D */

typedef struct {
    char name[OSD_NAME_MAX];
    char path[OSD_PATH_MAX];
    bool is_disk;            /* true = .dsk (drive A), false = .tap (tape) */
} osd_entry_t;

typedef struct {
    bool open;
    osd_entry_t entries[OSD_MAX_ENTRIES];
    int  count;
    int  selected;
    int  scroll;
    int  disk_drive;         /* target drive for a .dsk (0=A .. 3=D) */
    uint8_t font[128 * 8];   /* snapshot of the Oric charset ($B400) */
    bool font_ready;
    char status[64];         /* message of the last action */
} osd_t;

/* Result of a key press: what the caller must do next. */
typedef enum {
    OSD_NONE = 0,
    OSD_ACTIVATE,            /* the user confirmed: load entries[selected] */
    OSD_EJECT,               /* eject the disk from the target drive (disk_drive) */
    OSD_EJECT_TAPE,          /* eject the tape (highlighted medium of type .tap) */
    OSD_CLOSED               /* the overlay has just closed */
} osd_action_t;

void osd_init(osd_t* osd);

/* Copies the Oric charset from RAM ($B400, text mode) into osd->font.
 * To be called once per frame while in text mode (valid charset). */
void osd_snapshot_font(osd_t* osd, const uint8_t* mem);

/* Opens the overlay and (re)scans the media folders. */
void osd_open(osd_t* osd);
void osd_close(osd_t* osd);
void osd_toggle(osd_t* osd);

/* Handles a key (OSD_KEY_*). Returns the action to perform. */
osd_action_t osd_key(osd_t* osd, int key);

/* Draws the overlay into the framebuffer. No effect if !osd->open. */
void osd_render(osd_t* osd, video_t* vid);

/* Builds the media list from the given folders (NULL-terminated).
 * Exposed for tests; osd_open() calls it with the default folders. */
void osd_scan(osd_t* osd, const char* const* dirs);

#endif /* OSD_H */
