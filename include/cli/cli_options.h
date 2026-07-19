/**
 * @file cli_options.h
 * @brief Déclarations des options CLI (enum de codes longs + table getopt).
 * @author bmarty <bmarty@mailo.com>
 *
 * Extrait de main.c (Epic 7 / US3 partiel, Sprint 127). SÉPARE les *déclarations*
 * d'options (données) de la *logique de parsing* (le switch, resté dans main()).
 *
 * Note d'honnêteté : la réécriture déclarative complète (parser générique +
 * `cli_args_t`) n'est PAS faite — 43 des 63 `case` du switch portent des effets
 * de bord au parsing (fopen/malloc/exit/init) et ~89 variables locales seraient
 * à router à la main, ce qui n'est pas prouvable iso-comportement avec le filet
 * de tests actuel. Seule cette relocation de données (sans risque) est livrée.
 *
 * Inclus par main.c uniquement.
 */
#ifndef CLI_OPTIONS_H
#define CLI_OPTIONS_H

#include <getopt.h>

/* Codes des options longues sans équivalent court (>= 256 pour ne pas entrer en
 * collision avec les caractères d'options courtes). */
enum {
    OPT_SCREENSHOT = 256, OPT_SCREENSHOT_AT, OPT_FRAME_DUMP, OPT_FRAME_DUMP_INTERVAL,
    OPT_TYPE_KEYS, OPT_DISK_ROM, OPT_DISK1, OPT_DISK2, OPT_DISK3, OPT_BREAKPOINT,
    OPT_DEBUG_BREAK, OPT_CAST_SERVER, OPT_CAST_DISCOVER, OPT_CAST_TO, OPT_SAVE_STATE,
    OPT_LOAD_STATE, OPT_MODEL, OPT_JOYSTICK, OPT_PRINTER, OPT_PRINTER_TYPE, OPT_SCALE,
    OPT_TRACE, OPT_TRACE_MAX, OPT_PROFILE, OPT_ROM_INFO, OPT_SERIAL, OPT_SERIAL_V23,
    OPT_ACIA_ADDR, OPT_SERIAL_BUFFER, OPT_SERIAL_BAUD, OPT_SERIAL_IRQ_RDRF,
    OPT_SERIAL_TRACE, OPT_DTL2000, OPT_DTL2000_ADDR, OPT_MAGECO, OPT_MAGECO_ADDR,
    OPT_ORICON, OPT_DISK_WRITEBACK, OPT_DUMP_RAM_AT, OPT_TRACE_IRQ, OPT_SYMBOLS,
    OPT_TUI, OPT_LOCI, OPT_LOCI_FLASH, OPT_LOCI_SDIMG, OPT_LOCI_MIA_WINDOW,
    OPT_CONTROL, OPT_BENCH, OPT_RENDER_SOFTWARE, OPT_VIDEO, OPT_VIDEO_FPS,
    OPT_VIDEO_QUALITY, OPT_GDB, OPT_RECORD, OPT_REPLAY, OPT_NO_BORDER,
    OPT_EXPORT_BORDER, OPT_REALTIME, OPT_DISK_CREATE, OPT_BAD_SECTOR, OPT_FDC_TIMING,
    OPT_LOCI_USB, OPT_TAPE_SIGNAL, OPT_HTTP_API, OPT_HTTP_API_BIND, OPT_HTTP_API_ROOT,
    OPT_ULA_NG_POKE, OPT_PSG_TRACE, OPT_AUDIO_WAV, OPT_SCREENSHOT_TEXT,
    OPT_SCREENSHOT_ANSI, OPT_SCREENSHOT_TEXT_AT, OPT_SCREENSHOT_ANSI_AT
};

/* Chaîne d'options courtes passée à getopt_long. */
#define CLI_SHORT_OPTIONS "t:d:r:h:fnc:vm:k:j:p:b:D?"

/* Table getopt des options longues (terminée par {0,0,0,0}). */
static const struct option long_options[] = {
    {"tape",                required_argument, 0, 't'},
    {"disk",                required_argument, 0, 'd'},
    {"disk1",               required_argument, 0, OPT_DISK1},
    {"disk2",               required_argument, 0, OPT_DISK2},
    {"disk3",               required_argument, 0, OPT_DISK3},
    {"disk-writeback",      no_argument,       0, OPT_DISK_WRITEBACK},
    {"disk-create",         required_argument, 0, OPT_DISK_CREATE},
    {"rom",                 required_argument, 0, 'r'},
    {"hostfs",              required_argument, 0, 'h'},
    {"fast-load",           no_argument,       0, 'f'},
    {"headless",            no_argument,       0, 'n'},
    {"realtime",            no_argument,       0, OPT_REALTIME},
    {"tape-signal",         no_argument,       0, OPT_TAPE_SIGNAL},
    {"cycles",              required_argument, 0, 'c'},
    {"verbose",             no_argument,       0, 'v'},
    {"screenshot",          required_argument, 0, OPT_SCREENSHOT},
    {"screenshot-at",       required_argument, 0, OPT_SCREENSHOT_AT},
    {"screenshot-text",     required_argument, 0, OPT_SCREENSHOT_TEXT},
    {"screenshot-ansi",     required_argument, 0, OPT_SCREENSHOT_ANSI},
    {"screenshot-text-at",  required_argument, 0, OPT_SCREENSHOT_TEXT_AT},
    {"screenshot-ansi-at",  required_argument, 0, OPT_SCREENSHOT_ANSI_AT},
    {"frame-dump",          required_argument, 0, OPT_FRAME_DUMP},
    {"frame-dump-interval", required_argument, 0, OPT_FRAME_DUMP_INTERVAL},
    {"video",               required_argument, 0, OPT_VIDEO},
    {"video-fps",           required_argument, 0, OPT_VIDEO_FPS},
    {"video-quality",       required_argument, 0, OPT_VIDEO_QUALITY},
    {"gdb",                 optional_argument, 0, OPT_GDB},
    {"record",              required_argument, 0, OPT_RECORD},
    {"replay",              required_argument, 0, OPT_REPLAY},
    {"keyboard",            required_argument, 0, 'k'},
    {"type-keys",           required_argument, 0, OPT_TYPE_KEYS},
    {"disk-rom",            required_argument, 0, OPT_DISK_ROM},
    {"breakpoint",          required_argument, 0, 'b'},
    {"debug",               no_argument,       0, 'D'},
    {"break",               required_argument, 0, OPT_DEBUG_BREAK},
    {"cast-server",         optional_argument, 0, OPT_CAST_SERVER},
    {"http-api",            optional_argument, 0, OPT_HTTP_API},
    {"http-api-bind",       required_argument, 0, OPT_HTTP_API_BIND},
    {"http-api-root",       required_argument, 0, OPT_HTTP_API_ROOT},
    {"cast-to",             optional_argument, 0, OPT_CAST_TO},
    {"cast-discover",       no_argument,       0, OPT_CAST_DISCOVER},
    {"save-state",          required_argument, 0, OPT_SAVE_STATE},
    {"load-state",          required_argument, 0, OPT_LOAD_STATE},
    {"model",               required_argument, 0, 'm'},
    {"joystick",            required_argument, 0, 'j'},
    {"printer",             required_argument, 0, 'p'},
    {"printer-type",        required_argument, 0, OPT_PRINTER_TYPE},
    {"scale",               required_argument, 0, OPT_SCALE},
    {"render-software",     no_argument,       0, OPT_RENDER_SOFTWARE},
    {"no-border",           no_argument,       0, OPT_NO_BORDER},
    {"export-border",       no_argument,       0, OPT_EXPORT_BORDER},
    {"trace",               required_argument, 0, OPT_TRACE},
    {"trace-max",           required_argument, 0, OPT_TRACE_MAX},
    {"profile",             required_argument, 0, OPT_PROFILE},
    {"rom-info",            optional_argument, 0, OPT_ROM_INFO},
    {"serial",              required_argument, 0, OPT_SERIAL},
    {"serial-v23",          no_argument,       0, OPT_SERIAL_V23},
    {"serial-buffer",       required_argument, 0, OPT_SERIAL_BUFFER},
    {"serial-baud",         required_argument, 0, OPT_SERIAL_BAUD},
    {"serial-irq-on-rdrf",  no_argument,       0, OPT_SERIAL_IRQ_RDRF},
    {"serial-trace",        required_argument, 0, OPT_SERIAL_TRACE},
    {"acia-addr",           required_argument, 0, OPT_ACIA_ADDR},
    {"dtl2000",             required_argument, 0, OPT_DTL2000},
    {"dtl2000-addr",        required_argument, 0, OPT_DTL2000_ADDR},
    {"mageco",              required_argument, 0, OPT_MAGECO},
    {"mageco-addr",         required_argument, 0, OPT_MAGECO_ADDR},
    {"oricon",              required_argument, 0, OPT_ORICON},
    {"dump-ram-at",         required_argument, 0, OPT_DUMP_RAM_AT},
    {"bad-sector",          required_argument, 0, OPT_BAD_SECTOR},
    {"fdc-timing",          required_argument, 0, OPT_FDC_TIMING},
    {"trace-irq",           required_argument, 0, OPT_TRACE_IRQ},
    {"psg-trace",           required_argument, 0, OPT_PSG_TRACE},
    {"audio-wav",           required_argument, 0, OPT_AUDIO_WAV},
    {"symbols",             required_argument, 0, OPT_SYMBOLS},
    {"tui",                 no_argument,       0, OPT_TUI},
    {"loci",                no_argument,       0, OPT_LOCI},
    {"loci-flash",          required_argument, 0, OPT_LOCI_FLASH},
    {"loci-sdimg",          required_argument, 0, OPT_LOCI_SDIMG},
    {"loci-usb",            required_argument, 0, OPT_LOCI_USB},
    {"loci-mia-window",     required_argument, 0, OPT_LOCI_MIA_WINDOW},
    {"ula-ng-poke",         required_argument, 0, OPT_ULA_NG_POKE},
    {"control",             no_argument,       0, OPT_CONTROL},
    {"bench",               no_argument,       0, OPT_BENCH},
    {"help",                no_argument,       0, '?'},
    {0, 0, 0, 0}
};

#endif /* CLI_OPTIONS_H */
