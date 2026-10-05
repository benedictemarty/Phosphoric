/* SPDX-License-Identifier: EUPL-1.2 */
/**
 * @file cli_usage.c
 * @brief CLI --help / usage banner — moved verbatim from main.c (Epic 7/US3).
 * @author bmarty <bmarty@mailo.com>
 *
 * Pure printf output; the only dependency is EMU_VERSION (emulator.h). Keeping
 * the ~150-line help text here stops it inflating main.c. Byte-identical to the
 * former static print_usage() (verified by diffing --help before/after).
 */
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include "emulator.h"       /* EMU_VERSION */
#include "cli/cli_usage.h"
#include "card_module.h"    /* help of the cards as modules */

/* Every help line goes through here: before the line announcing an option
 * (« -x, --name » or « --name » at its start), the cards as modules anchored on
 * that option write their block (card_modules_print_help_before). */
static void usage_printf(const char* fmt, ...) {
    char line[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    const char* p = NULL;
    if (strncmp(line, "      --", 8) == 0) p = line + 8;
    else if (line[0] == ' ' && line[1] == ' ' && line[2] == '-' && line[3] != '-' &&
             strncmp(line + 4, ", --", 4) == 0) p = line + 8;
    if (p) {
        char name[64];
        size_t n = strcspn(p, " =\n");
        if (n >= sizeof(name)) n = sizeof(name) - 1;
        memcpy(name, p, n);
        name[n] = '\0';
        card_modules_print_help_before(name);
    }
    fputs(line, stdout);
}

void cli_print_usage(const char* program_name) {
    usage_printf("Phosphoric v%s\n", EMU_VERSION);
    usage_printf("Usage: %s [options]\n\n", program_name);
    usage_printf("Options:\n");
    usage_printf("  -t, --tape FILE            Load .TAP tape file\n");
    usage_printf("      --tape-signal          Signal-level tape (VIA CB1 waveform, real ROM\n");
    usage_printf("                             read) — for custom/protected loaders; excludes -f\n");
    usage_printf("      --tape-signal-free     Comme --tape-signal, mais le moteur est piloté par\n");
    usage_printf("                             ORB PB6 (free-gate) — loaders clean-room sans ROM\n");
    usage_printf("      --tape-out-capture FILE Capture CSAVE waveform (PB7/Timer1) and decode\n");
    usage_printf("                             it to a .TAP (voie A CSAVE; disables CSAVE hooks)\n");
    usage_printf("  -d, --disk FILE            Load .DSK disk file in drive A\n");
    usage_printf("      --disk1 FILE           Load .DSK disk file in drive B\n");
    usage_printf("      --disk2 FILE           Load .DSK disk file in drive C\n");
    usage_printf("      --disk3 FILE           Load .DSK disk file in drive D\n");
    usage_printf("      --disk-rom FILE        Load Microdisc ROM (microdis.rom)\n");
    usage_printf("      --jasmin-rom FILE      Enable the Jasmin disk interface (WD177x $03F4-$03FF,\n");
    usage_printf("                             2 KB boot ROM at $F800). Alternative to the Microdisc;\n");
    usage_printf("                             boots Jasmin/TDOS disks. Shares -d/--disk1..3. Mutually\n");
    usage_printf("                             exclusive with --disk-rom/--dtl2000/--mageco.\n");
    usage_printf("      --disk-writeback       Persist in-game disk writes back to the .dsk files on exit\n");
    usage_printf("                             (overwrites in place; only drives actually written are saved)\n");
    usage_printf("      --disk-create FILE     Create a blank Sedoric disk in drive A and write it to FILE\n");
    usage_printf("                             (then INIT/format inside; changes are saved back on exit)\n");
    usage_printf("      --disk-web URL         Drive A servi par un serveur web (loci-webdisk archi B) :\n");
    usage_printf("                             pistes MFM lues par HTTP a la demande, via le Microdisc.\n");
    usage_printf("                             Ex: --disk-rom microdis.rom --disk-web http://h:8091/disk/x.dsk\n");
    usage_printf("  -r, --rom FILE             Load custom ROM file (default: roms/basic10.rom,\n");
    usage_printf("                             bare ORIC-1 minimal profile; basic11b.rom with -m atmos)\n");
    usage_printf("      --no-rom               No default system ROM ($C000-$FFFF empty)\n");
    usage_printf("  -h, --hostfs PATH          Mount host directory\n");
    usage_printf("  -f, --fast-load            Fast tape loading (inject directly, no CLOAD needed)\n");
    usage_printf("  -n, --headless             Run without display (headless mode)\n");
    usage_printf("      --realtime             Pace to 50 Hz PAL even in headless (nanosleep, no SDL);\n");
    usage_printf("                             needed for network serial timing (modem/XMODEM) and\n");
    usage_printf("                             deterministic --type-keys without a display\n");
    usage_printf("  -c, --cycles NUM           Run for N cycles then exit\n");
    usage_printf("  -v, --verbose              Verbose logging\n");
    usage_printf("      --screenshot FILE      Take screenshot at exit (.ppm or .bmp)\n");
    usage_printf("      --screenshot-at C:FILE Screenshot after C cycles to FILE\n");
    usage_printf("                             (the -at family is REPEATABLE: pass several\n");
    usage_printf("                             --screenshot-at/-text-at/-ansi-at/--dump-ram-at\n");
    usage_printf("                             to capture many instants in one run)\n");
    usage_printf("      --screenshot-text FILE Dump screen text ($BB80, 40x28) as ASCII at exit\n");
    usage_printf("      --screenshot-ansi FILE Dump framebuffer as ANSI true-color text at exit\n");
    usage_printf("      --screenshot-text-at C:FILE  Dump screen text after C cycles to FILE\n");
    usage_printf("      --screenshot-ansi-at C:FILE  Dump ANSI framebuffer after C cycles to FILE\n");
    usage_printf("      --screenshot-when A:V:FILE   Screenshot when RAM[A]==V (A,V hex; exit 2 if never)\n");
    usage_printf("      --screenshot-text-when A:V:FILE  Text screenshot when RAM[A]==V (A,V hex)\n");
    usage_printf("      --dump-ram-when A:V:FILE     Dump 64KB when RAM[A]==V (A,V hex; exit 2 if never)\n");
    usage_printf("      --poke-at C:ADDR=VAL        Write RAM[ADDR]=VAL once after C cycles (hex ADDR/VAL; repeatable)\n");
    usage_printf("      --poke-when A:V:ADDR=VAL     Write RAM[ADDR]=VAL once when RAM[A]==V (all hex; repeatable)\n");
    usage_printf("      --frame-dump DIR       Dump frames to directory\n");
    usage_printf("      --frame-dump-interval N  Dump every Nth frame (default: 50)\n");
    usage_printf("      --record FILE          Record keyboard input to a movie (deterministic replay)\n");
    usage_printf("      --replay FILE          Replay a recorded input movie (ignores live keys)\n");
    usage_printf("      --video FILE           Record video to a Motion-JPEG AVI file\n");
    usage_printf("      --video-fps N          Recording frame rate (default: 50)\n");
    usage_printf("      --video-quality N      JPEG quality 1..100 (default: 85)\n");
    usage_printf("  -m, --model MODEL          Machine model: oric1 or atmos (default: auto-detect)\n");
    usage_printf("  -k, --keyboard LAYOUT      Keyboard layout: qwerty (default) or azerty\n");
    usage_printf("  -j, --joystick MODE        Joystick: keys (arrow keys), gamepad (SDL2 controller)\n");
    usage_printf("  -p, --printer FILE         Capture printer output to FILE (LPRINT/LLIST)\n");
    usage_printf("      --printer-type TYPE    Printer type: text (default) or mcp40 (4-color plotter)\n");
    usage_printf("      --scale N              Display scale factor: 1, 2, 3 (default), or 4\n");
    usage_printf("      --render-software      Force the SDL software renderer (fixes a black window\n");
    usage_printf("                             on some GPU/driver setups; same as SDL_RENDER_DRIVER=software)\n");
    usage_printf("      --no-border            Disable the overscan border in the window (on by default)\n");
    usage_printf("      --config FILE          Settings saved by the F1 peripherals menu (default:\n");
    usage_printf("                             phosphoric.cfg, read unless --headless; the command\n");
    usage_printf("                             line always wins). --config also applies in headless.\n");
    usage_printf("      --no-config            Do not read phosphoric.cfg\n");
    usage_printf("      --menu-screenshot FILE Render the F1 peripherals menu to a PPM (end of run)\n");
    usage_printf("      --export-border        Include the overscan border in image/AVI exports (off by default)\n");
    usage_printf("      --type-keys C:TEXT     Auto-type TEXT after C cycles. Escapes:\n");
    usage_printf("                             \\n=Return \\e=Esc \\b=Del \\u \\d \\l \\r=arrows\n");
    usage_printf("                             \\Cx=Ctrl+x \\Fx=Funct+x \\Lx=LShift+x\n");
    usage_printf("                             \\Rx=RShift+x \\pN=pause N sec (cycles emules)\n");
    usage_printf("                             Repetable : plusieurs --type-keys sont\n");
    usage_printf("                             sequences par cycle d'armement croissant.\n");
    usage_printf("                             Pacing synchronise sur le scan clavier reel\n");
    usage_printf("                             (aucune touche perdue meme si le programme\n");
    usage_printf("                             scrute lentement).\n");
    usage_printf("      --type-keys-when A:V:TEXT  Arme --type-keys quand RAM[A]==V (A,V hex)\n");
    usage_printf("                             au lieu de deviner le cycle de boot.\n");
    usage_printf("  -b, --breakpoint ADDR      Break when PC reaches address (hex, e.g. ED8A)\n");
    usage_printf("  -D, --debug                Start in debugger mode (break at first instruction)\n");
    usage_printf("      --break ADDR           Set initial debugger breakpoint (hex)\n");
    usage_printf("      --cast-server[=PORT]   Start MJPEG cast server (default port: 8080)\n");
    usage_printf("      --cast-to[=DEVICE]     Cast to Chromecast (native CASTV2 protocol)\n");
    usage_printf("      --cast-discover        Discover Chromecast devices on network\n");
    usage_printf("      --http-api[=PORT]      HTTP control API (REST) on PORT (default 8888, HTTPAPI=1 build)\n");
    usage_printf("      --http-api-bind ADDR   Bind address for the HTTP API (default 127.0.0.1)\n");
    usage_printf("      --http-api-root DIR    Sandbox root for HTTP file ops /tape,/disk (default CWD)\n");
    usage_printf("      --trace FILE           Log CPU instruction trace to FILE\n");
    usage_printf("      --trace-max N          Max instructions to trace (keeps the FIRST N)\n");
    usage_printf("      --cpu-microseq         Cycle-stepped 6502 core — now the DEFAULT (kept for\n");
    usage_printf("                             scripts; every cycle emits its own bus access)\n");
    usage_printf("      --ula-cycle            ULA fetches one cell per cycle — now the DEFAULT\n");
    usage_printf("                             (kept for scripts)\n");
    usage_printf("      --ula-line             Render a whole scanline at once (pre-V2 behaviour:\n");
    usage_printf("                             no mid-line raster split possible)\n");
    usage_printf("      --ula-fetch-offset N   Cycle within the line at which column 0 is fetched\n");
    usage_printf("                             (default 0; NOT calibrated against real hardware)\n");
    usage_printf("      --cpu-legacy           Fall back to the historical core (cycle totals exact,\n");
    usage_printf("                             no dummy accesses, IRQ decided at instruction boundary)\n");
    usage_printf("      --cycle-trace FILE     Log ONE LINE PER CYCLE (bus address, data, R/W,\n");
    usage_printf("                             registers) to FILE — V2 accuracy instrument;\n");
    usage_printf("                             internal cycles appear as 'i' (see docs/ACCURACY.md)\n");
    usage_printf("      --cycle-trace-max N    Max lines for --cycle-trace (0 = unlimited)\n");
    usage_printf("      --trace-ring N         Keep only the LAST N instructions (ring buffer,\n");
    usage_printf("                             saved to --trace FILE at exit; ideal for a hang)\n");
    usage_printf("      --trace-irq FILE       Log every IRQ entry + RTI to FILE (debug IRQ handlers)\n");
    usage_printf("      --psg-trace FILE       Log AY sound-register writes (reg 0-13) with CPU cycle\n");
    usage_printf("      --kbd-scan-trace FILE  Log every VIA Port B read (col, reg7, reg14, matrix, PB3)\n");
    usage_printf("      --audio-wav FILE       Capture PSG audio to a 16-bit stereo 44.1 kHz WAV\n");
    usage_printf("                             (headless only; renders per frame via ay_generate)\n");
    usage_printf("      --profile FILE         Write CPU performance profile to FILE on exit\n");
    usage_printf("      --dump-ram-at C:FILE   Dump 64KB RAM to FILE when cycle >= C\n");
    usage_printf("      --bad-sector [D:]S:T:N Mark drive D (default A) side S track T sector N\n");
    usage_printf("                             unreadable (RNF), repeatable; damage follows the media\n");
    usage_printf("      --disk-write-protect   Write-protect tab on: writes are refused with status\n");
    usage_printf("                             bit 6, as on a real drive (also implied when the .dsk\n");
    usage_printf("                             file itself is read-only)\n");
    usage_printf("      --fdc-timing MODE      Microdisc WD1793 timing: real (default, mechanical\n");
    usage_printf("                             3\" drive) or fast (legacy short delays)\n");
    usage_printf("      --rom-info [FILE]      Analyze ROM and print report (or write to FILE)\n");
    usage_printf("      --symbols FILE         Load symbol table (.sym / .lab / .sym65)\n");
    usage_printf("      --tui                  Use ncurses TUI debugger (requires TUI=1 build)\n");
    usage_printf("      --gdb[=PORT]           GDB remote stub on TCP PORT (default 1234).\n");
    usage_printf("                             Waits for `gdb` ... `target remote :PORT`.\n");
    usage_printf("      --no-config-cards      Ignore the expansion cards of phosphoric.cfg (used\n");
    usage_printf("                             when the F1 menu restarts with other cards)\n");
    usage_printf("      --no-auto-loci         Do not start on a plugged LOCI-USB (by default it is\n");
    usage_printf("                             used when no disk card is chosen and its LOCI\n");
    usage_printf("                             firmware answers 'L' at $0319)\n");
    usage_printf("      --gdb-bind ADDR        Bind address for the GDB stub (default 127.0.0.1;\n");
    usage_printf("                             0.0.0.0 = every interface, no authentication)\n");
    usage_printf("      --control              IPC control mode for IDE integration (stdin protocol,\n");
    usage_printf("                             logs to stderr, see docs/control_protocol.md)\n");
    usage_printf("      --bench                Headless throughput bench: prints `BENCH cycles=... mhz_eq=... ...`\n");
    usage_printf("                             on stdout at exit. Use with -c N for fixed-cycle run.\n");
    usage_printf("      --loci                 Enable LOCI MIA at $03A0-$03BF\n");
    usage_printf("      --loci-flash DIR       Sandbox root for LOCI file ops (implies --loci)\n");
    usage_printf("      --loci-sdimg PATH      Raw FAT16/32 SD image (read-only, implies --loci)\n");
    usage_printf("                             Mutually exclusive with --loci-flash\n");
    usage_printf("      --loci-usb DIR|none    Attach DIR as a LOCI USB key (repeatable, 4 max);\n");
    usage_printf("                             host media in /media/$USER auto-attach — 'none' disables\n");
    usage_printf("      --loci-web URL         Drive A LOCI servi par un serveur web (loci-webdisk archi B) :\n");
    usage_printf("                             pistes MFM par HTTP a la demande + autoboot Sedoric natif LOCI.\n");
    usage_printf("                             Ex: --loci --loci-web http://h:8092/disk/x.dsk (implies --loci)\n");
    usage_printf("      --loci-web-base URL    Ajoute un pseudo-device « W: Web disks » au menu LOCI :\n");
    usage_printf("                             opendir/readdir listent GET URL/disks ; monter un .dsk\n");
    usage_printf("                             sert URL/disk/<nom>. Menu locirom NON modifie. (implies --loci)\n");
    usage_printf("      --loci-mia-window LO-HI  Model the reliable MIA tior range (0-31).\n");
    usage_printf("                             picowifi ACIA $0380 accesses corrupt when tior\n");
    usage_printf("                             is outside it (reproduces real-HW modem block;\n");
    usage_printf("                             software tunes via MAP_TUNE_TIOR / ADJ_SCAN)\n");
    usage_printf("      --loci-serve-timing SERVE[,TDSR]  Sub-cycle PHI2 race model: LOCI serves a\n");
    usage_printf("                             read in SERVE core cycles (120 MHz; 23 measured on\n");
    usage_printf("                             hardware); data lands at max(ready, PHI2 rise)+out,\n");
    usage_printf("                             clean iff before cycle end - TDSR ns (default 100)\n");
    usage_printf("      --loci-serve-jitter AMP[,SEED]  Seeded jitter (+/-AMP serve cycles) on the\n");
    usage_printf("                             PHASE model: near the latch, misses become\n");
    usage_printf("                             occasional yet reproducible for a given SEED\n");
    usage_printf("      --loci-emu FILE       Co-simulation : exécute le VRAI firmware LOCI\n");
    usage_printf("                            (ELF RP2040) au lieu du modèle interne (implies --loci)\n");
    usage_printf("      --loci-usb-image FILE Image FAT servie au firmware co-simulé comme\n");
    usage_printf("                            clé USB émulée (avec --loci-emu)\n");
    usage_printf("      --loci-cdc DEV        Dongle CDC réel (ex. /dev/ttyACM0) servi par le\n");
    usage_printf("                            firmware co-simulé comme ACIA $0380 (avec --loci-emu)\n");
    usage_printf("      --loci-emu-flash FILE Image flash persistante du firmware co-simulé (FS\n");
    usage_printf("                            interne 0:) ; défaut <ELF>.flash, « - » = volatile\n");
    usage_printf("      --loci-hw DEV         LOCI-USB (Feather RP2040 qui fait tourner le firmware\n");
    usage_printf("                            LOCI) branchée en USB, ex. /dev/ttyACM0 : le 6502\n");
    usage_printf("                            émulé accède au silicium\n");
    usage_printf("                            (binaire construit avec ~/loci/loci-usb ; implies --loci)\n");
    usage_printf("      --loci-menu-at N      Déclenche le menu LOCI une seule fois au cycle N\n");
    usage_printf("      --save-state FILE      Save emulator state to FILE on exit\n");
    usage_printf("      --load-state FILE      Load emulator state from FILE at startup\n");
    usage_printf("  -?, --help                 Show this help\n");
    card_modules_print_help_before(NULL);   /* cards without an anchor */
    usage_printf("\n");
    usage_printf("Controls:\n");
    usage_printf("  F1  - Peripherals menu: floppies, tape, snapshots, printer, joystick,\n");
    usage_printf("        keyboard; saves phosphoric.cfg (machine paused while open)\n");
    usage_printf("  F2  - Quick save state\n");
    usage_printf("  F3  - Cycle display scale (x1 → x2 → x3 → x4)\n");
    usage_printf("  F4  - Quick load state\n");
    usage_printf("  F5  - Reset (with --loci : also resets MIA state, keeps mounts)\n");
    usage_printf("  Shift+F5 - NMI (the button under the Oric)\n");
    usage_printf("  F6  - OSD : changer la cassette/disquette a chaud (fleches, RET, ESC)\n");
    usage_printf("  F8  - LOCI Action button (warm short press / release on key up)\n");
    usage_printf("  F9  - Enter debugger\n");
    usage_printf("  F10 - Quit\n");
    usage_printf("  F11 - Fullscreen\n");
    usage_printf("  F12 - Screenshot\n");
    usage_printf("\n");
}
