/* SPDX-License-Identifier: EUPL-1.2 */
/**
 * @file cli_opts.h
 * @brief Options de la ligne de commande, rassemblées dans une structure.
 * @author bmarty <bmarty@mailo.com>
 *
 * Sprint C du plan d'architecture : les ~120 variables locales que main()
 * déclarait avant la boucle getopt vivent ici, avec leurs valeurs par défaut
 * (cli_opts_init). Les noms, types, valeurs initiales et commentaires sont
 * repris à l'identique ; seuls les deux tableaux de structures anonymes ont
 * reçu un nom de type (cli_tcap_arg_t, cli_poke_arg_t).
 */
#ifndef CLI_OPTS_H
#define CLI_OPTS_H

#include <stdbool.h>
#include <stdint.h>
#include "emulator.h"            /* TIMED_CAPTURE_MAX, POKE_MAX, TYPE_KEYS_SEQ_MAX */
#include "io/microdisc.h"        /* MICRODISC_MAX_DRIVES */
#include "io/loci.h"             /* LOCI_USB_DEV_MAX */
#include "network/gdbstub.h"     /* GDB_DEFAULT_PORT */
#include "storage/disk.h"        /* FDC_MAX_BAD_SECTORS */

/* --screenshot-at / --dump-ram-at… : argument brut + type de capture. */
typedef struct { const char* arg; timed_capture_type_t type; } cli_tcap_arg_t;
/* --poke-at / --poke-when : argument brut + variante. */
typedef struct { const char* arg; bool is_when; } cli_poke_arg_t;

typedef struct cli_opts_s {

    const char* tape_file;
    const char* disk_files[MICRODISC_MAX_DRIVES];
    const char* disk_create_file;
    const char* disk_web_url;   /* loci-webdisk archi B: disque servi par HTTP */
    bool disk_writeback;
    bool disk_write_protect;
    const char* rom_file;
    const char* hostfs_path;
    bool fast_load;
    bool tape_signal;   /* --tape-signal: signal-level cassette (Sprint 90) */
    bool tape_signal_free;   /* --tape-signal-free: gate moteur sur ORB PB6 (clean-room ROM) */
    const char* tape_out_capture_arg;   /* --tape-out-capture FILE: capture PB7 -> .TAP */
    bool verbose;
    bool headless;
    int64_t max_cycles;
    const char* screenshot_file;
    /* Captures -at RÉPÉTABLES : on collecte (arg, type) au parsing, puis on
     * résout chaque "CYCLES:FILE" après la boucle getopt (comme --poke-at). */
    cli_tcap_arg_t tcap_cli[TIMED_CAPTURE_MAX];
    int tcap_cli_count;
    const char* screenshot_text_file;
    const char* screenshot_ansi_file;
    const char* frame_dump_dir;
    int frame_dump_interval;
    const char* video_avi_file;
    int video_avi_fps;
    int video_avi_quality;
    bool gdb_enabled;
    int gdb_port;
    const char* gdb_bind;        /* NULL → 127.0.0.1          */
    bool no_config_cards;        /* cartes : ignorer phosphoric.cfg (relance du menu F1) */
    const char* movie_record_file;
    const char* movie_replay_file;
    const char* keyboard_layout;

    const char* type_keys_args[TYPE_KEYS_SEQ_MAX];
    int type_keys_arg_count;
    /* --poke-at / --poke-when : arguments collectés pendant getopt puis parsés
     * après emulator_init (les pokes vivent dans emu, initialisé plus tard). */
    cli_poke_arg_t poke_args[POKE_MAX];
    int poke_arg_count;
    const char* disk_rom_file;
    const char* jasmin_rom_file;   /* --jasmin-rom : Jasmin boot ROM (2 KB) */
    void** card_cfg;        /* configuration de chaque carte en module (card_module.h) */
    bool debug_mode;
    const char* debug_break_addr;
    bool cast_server_enabled;
    uint16_t cast_server_port;
    /* HTTP control API (sprint 94) */
    bool http_api_enabled;
    uint16_t http_api_port;   /* 0 → HTTP_API_DEFAULT_PORT */
    const char* http_api_bind;   /* NULL → 127.0.0.1          */
    const char* http_api_root;   /* NULL → "." (CWD)          */
    bool cast_discover;
    bool cast_to_enabled;
    const char* cast_to_device;
    const char* save_state_file;
    const char* load_state_file;
    const char* model_arg;
    const char* joystick_mode;
    const char* printer_file;
    const char* printer_type_arg;
    int scale_factor;
    bool render_software;
    const char* trace_file;
    const char* cycle_trace_file;
    bool cpu_microseq;   /* V2-E1/US1.4 : cœur cycle-par-cycle par défaut */
    bool ula_per_cycle;   /* V2-E4/US4.2 : ULA au fetch par cycle par défaut */
    int ula_fetch_offset;
    uint64_t cycle_trace_max;
    const char* screenshot_when_arg;
    const char* dump_ram_when_arg;
    const char* screenshot_text_when_arg;
    const char* type_keys_when_arg;
    const char* bad_sector_args[FDC_MAX_BAD_SECTORS];
    int bad_sector_arg_count;
    const char* fdc_timing_arg;
    const char* loci_usb_args[LOCI_USB_DEV_MAX];
    int loci_usb_count;
    bool loci_usb_autoscan;
    const char* trace_irq_file;
    const char* psg_trace_file;
    const char* kbd_trace_file;
    const char* audio_wav_file;
    const char* symbols_file;
    bool tui_mode;
    bool control_mode;
    bool bench_mode;
    bool loci_enabled;
    const char* loci_flash_root;
    const char* loci_emu_path;   /* --loci-emu : exécute le vrai firmware RP2040 (émulateur) */
    const char* loci_hw_dev;   /* --loci-hw : VRAIE cartouche via le pont USB loci-usb (backend loci_hw.c) */
    const char* loci_emu_usb_image;   /* --loci-usb-image : image FAT servie comme disque USB émulé */
    const char* loci_emu_cdc_dev;   /* --loci-cdc : dongle CDC (ex. /dev/ttyACM0) servi comme ACIA $0380 */
    const char* loci_emu_flash;   /* --loci-flash : image flash persistante (FS interne 0:) */
    const char* loci_sdimg_path;
    const char* loci_web_url;   /* loci-webdisk archi B : disque web natif LOCI */
    const char* loci_web_base;   /* Route B : racine serveur pour le device « W: Web disks » */
    int loci_mia_win_lo; int loci_mia_win_hi;   /* -1 = not set (open window) */
    int loci_serve_subticks; int loci_latch_subtick;   /* -1 = phase model off */
    int loci_serve_jitter; unsigned loci_jitter_seed;   /* -1 = no jitter */
    int64_t trace_max;
    int64_t trace_ring;   /* --trace-ring N : garder les N DERNIÈRES instructions */
    const char* profile_file;
    const char* rom_info_file;
    bool rom_info_enabled;
    const char* serial_arg;
    const char* acia_addr_arg;
    bool serial_v23;
    int serial_buffer_size;
    int serial_baud;
    bool serial_irq_on_rdrf;
    const char* serial_trace_file;
    bool serial_tcp_backpressure;   /* --serial-tcp-backpressure */
    int serial_tcp_rcvbuf;   /* explicit SO_RCVBUF cap (0 = auto) */
    long loci_irq_latency_us;   /* --loci-irq-latency (LOCI I2C IRQ cost) */

    uint64_t loci_menu_at;
    /* Menu des périphériques (F1) et phosphoric.cfg. */
    const char* config_path;        /* --config FILE (NULL : phosphoric.cfg) */
    bool no_config;                 /* --no-config */
    bool disk_protect[4];           /* protection_x=oui (phosphoric.cfg) */
    const char* menu_screenshot;    /* --menu-screenshot FILE (PPM, fin de run) */   /* --loci-menu-at : recopié dans g_loci_menu_at (main.c) */
} cli_opts_t;

/* Valeurs par défaut (celles des anciennes variables locales de main()). */
void cli_opts_init(cli_opts_t* o);

#endif /* CLI_OPTS_H */
