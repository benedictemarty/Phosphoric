/* _DEFAULT_SOURCE: cfmakeraw + B115200 (raw config of the CDC dongle, glibc). */
#define _DEFAULT_SOURCE
/* loci_emu.c — LOCI backend emulating the real RP2040 firmware. See loci_emu.h.
 *
 * Step 1: ROM OVERLAY. The real firmware serves the LOCI boot ROM ($C000-$FFFF)
 * when nROMDIS is active; Phosphoric's memory_read() delegates to loci_emu_rom_read().
 * Step 2: co-simulated MIA $03xx API (loci_emu_api_read/write).
 *
 * EXECUTION MODEL — SINGLE-THREADED (deterministic): the firmware boots in a
 * thread at startup (zero wait), then that thread HANDS CONTROL BACK. After that,
 * all access to the emulator happens from Phosphoric's main thread:
 *  - `loci_emu_tick()`: BOUNDED free-run (n RP2040 steps) once per frame, so that
 *    the firmware makes progress (background tasks, nIRQ) without driving the bus;
 *  - `loci_emu_api_*` / `loci_emu_rom_read`: co-simulated bus accesses.
 * A DEDICATED emulator thread (continuous free-run) was tried but REMOVED: on this
 * target it causes heavy contention and destabilizes the shared lines
 * (×100 regression + broken BASIC) — the emulator throughput (~1/10 of real) does
 * not justify it. The bounded tick is enough and stays deterministic. */
#include "io/loci_emu.h"
#include "utils/logging.h"
#include "emul_lib.h"          /* ~/loci/emul/src (via -I in the Makefile) */
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <fcntl.h>
#include <unistd.h>
#include <termios.h>
#include <stdlib.h>
#include <time.h>

static emul_t    g_emul;
static pthread_t g_boot_thread;
static volatile int g_boot_done;   /* 1 when the firmware has finished booting (idle) */
static int       g_boot_started, g_joined;
static char      g_snap_path[512]; /* "boot finished" state cache = <elf>.snap  */
static char      g_flash_path[1024]; /* persistent flash image = <elf>.flash ("-" = volatile) */
static char      g_usb_image[1024]; /* FAT image served as emulated USB disk (optional) */
static char      g_cdc_dev[1024];  /* CDC dongle (e.g. /dev/ttyACM0 or PTY) served as $0380 modem */
static int       g_cdc_fd = -1;    /* open descriptor of the dongle (>=0 = ACIA routed to the firmware) */

/* Firmware UART0 output -> Phosphoric log (one line at a time). */
static char g_line[256];
static int  g_len;
static void uart_cb(int ch, void *user)
{
    (void)user;
    if (ch == '\n' || g_len >= (int)sizeof(g_line) - 1) {
        g_line[g_len] = '\0';
        if (g_len) log_info("LOCI-emu: %s", g_line);
        g_len = 0;
    } else if (ch != '\r') {
        g_line[g_len++] = (char)ch;
    }
}

/* Opens the CDC dongle. If it is a TTY (e.g. /dev/ttyACM0), switches it to RAW B115200
 * (the real PicoWifiModemUSB); otherwise (PTY, socket) uses it as is. Non-blocking.
 * Returns the fd (>=0) or -1. */
static int cdc_open(const char *path)
{
    int fd = open(path, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) return -1;
    struct termios tio;
    if (tcgetattr(fd, &tio) == 0) {          /* TTY: raw B115200 configuration */
        cfmakeraw(&tio);
        cfsetispeed(&tio, B115200);
        cfsetospeed(&tio, B115200);
        tio.c_cc[VMIN] = 0; tio.c_cc[VTIME] = 0;
        tcsetattr(fd, TCSANOW, &tio);
    }
    return fd;
}

static void *boot_thread_fn(void *arg)
{
    (void)arg;
    /* The snapshot (emul_lib, SNAP_VERSION 3) persists SRAM+bootrom + the FLASH PAGES
     * written by the firmware (littlefs) → consistent FS on restore (no more "Corrupted
     * dir pair" on the 2nd launch). Instant boot preserved. */
    int cached = emul_loci_boot_cached(&g_emul, g_snap_path[0] ? g_snap_path : NULL);
    log_info("LOCI-emu: %s", cached ? "état restauré depuis le snapshot (boot instantané)"
                                    : "boot complet effectué (snapshot écrit pour la prochaine fois)");
    /* Mounts the emulated USB image (drive "1:") if provided — after boot/restore
     * (the snapshot precedes the mount). Without an image, no USB disk (unchanged). */
    if (g_usb_image[0]) {
        int mounted = emul_loci_usb_mount(&g_emul);
        log_info("LOCI-emu: disque USB émulé « %s » -> %s", g_usb_image,
                 mounted ? "monté (drive 1:)" : "échec du montage");
    }
    /* CDC modem (real /dev/ttyACM0 dongle or PTY): attach + mount AFTER boot.
     * The firmware's $0380 ACIA will talk to this descriptor (instead of the behavioral 6551). */
    if (g_cdc_dev[0]) {
        g_cdc_fd = cdc_open(g_cdc_dev);
        if (g_cdc_fd >= 0 && emul_loci_cdc_attach_fd(&g_emul, g_cdc_fd)) {
            int modem = emul_loci_cdc_mount(&g_emul);
            log_info("LOCI-emu: modem CDC « %s » -> %s (ACIA $0380 servie par le firmware)",
                     g_cdc_dev, modem ? "monté" : "attaché (pas d'AT — vérifier le dongle)");
        } else {
            if (g_cdc_fd >= 0) { close(g_cdc_fd); g_cdc_fd = -1; }
            log_warning("LOCI-emu: modem CDC « %s » indisponible — ACIA $0380 reste comportementale",
                        g_cdc_dev);
        }
    }
    g_boot_done = 1;
    int nromdis = 0;
    emul_ext_lines(&g_emul, NULL, NULL, &nromdis);
    log_info("LOCI-emu: boot terminé (core0 pc=%08x, core1 %s) — LOCI TRANSPARENT (nROMDIS=%d) ; "
             "bouton MENU (F8) pour entrer dans LOCI",
             g_emul.cpu0.r[CM0_PC], g_emul.cpu1.running ? "lancé" : "non lancé", nromdis);
    return NULL;
}

/* Waits for the background boot to finish (idempotent). */
static void ensure_booted(void)
{
    if (g_boot_started && !g_joined) { pthread_join(g_boot_thread, NULL); g_joined = 1; }
}

int loci_emu_start(const char *elf_path)
{
    g_emul.uart_tx = uart_cb; g_emul.uart_user = NULL;
    if (emul_init(&g_emul, elf_path, 0x10000100u) != 0) {   /* fast: loads the ELF, does not step */
        log_error("LOCI-emu: emul_init a échoué (%s)", elf_path);
        return -1;
    }
    snprintf(g_snap_path, sizeof(g_snap_path), "%s.snap", elf_path);
    /* Persistent flash: re-applies the previous session's image BEFORE
     * boot/restore. emul_flash_persist_load recomputes the signature → a snapshot
     * taken on another flash image is rejected (full boot, then rewritten). */
    if (!g_flash_path[0]) snprintf(g_flash_path, sizeof(g_flash_path), "%s.flash", elf_path);
    if (strcmp(g_flash_path, "-") != 0) {
        if (emul_flash_persist_load(&g_emul, g_flash_path))
            log_info("LOCI-emu: flash restaurée depuis « %s » (FS interne 0: de la session précédente)", g_flash_path);
        else
            log_info("LOCI-emu: pas d'image flash « %s » — FS interne vierge (sera persisté en fin de session)", g_flash_path);
    }
    /* Registers the USB MSC HLE BEFORE boot (the hooks must be installed before
     * the mount; the snapshot restore does not overwrite them — outside cpu_snap_t). */
    if (g_usb_image[0] && !emul_loci_set_usb_image(&g_emul, g_usb_image)) {
        log_warning("LOCI-emu: image USB « %s » illisible — disque USB désactivé", g_usb_image);
        g_usb_image[0] = '\0';
    }
    log_info("LOCI-emu: firmware RP2040 (%s) — boot en arrière-plan (LOCI transparent)…", elf_path);

    if (pthread_create(&g_boot_thread, NULL, boot_thread_fn, NULL) == 0) {
        g_boot_started = 1;        /* asynchronous boot: loci_emu_start returns immediately */
    } else {
        boot_thread_fn(NULL);      /* synchronous fallback if the thread fails */
    }
    return 0;
}

/* Declares the FAT image to serve as emulated USB disk (drive "1:"). To be called
 * BEFORE loci_emu_start (the mount happens right after boot). */
void loci_emu_set_usb_image(const char *path)
{
    if (path) snprintf(g_usb_image, sizeof(g_usb_image), "%s", path);
}

void loci_emu_set_flash_image(const char *path)
{
    if (path) snprintf(g_flash_path, sizeof(g_flash_path), "%s", path);
}

/* End of session: the emulated flash (pages written by littlefs) is persisted to
 * the image — like the cartridge's NOR, drive 0: survives to the next launch.
 * No persistent thread left to stop (single-threaded after boot). */
void loci_emu_stop(void)
{
    if (!g_boot_started) return;
    ensure_booted();
    if (!g_flash_path[0] || !strcmp(g_flash_path, "-")) return;
    if (emul_flash_persist_save(&g_emul, g_flash_path))
        log_info("LOCI-emu: flash persistée dans « %s » (FS interne 0: conservé)", g_flash_path);
    else
        log_warning("LOCI-emu: échec d'écriture de l'image flash « %s » — FS de la session perdu", g_flash_path);
}

bool loci_emu_active(void) { return g_boot_done != 0; }
const char *loci_emu_backend_name(void) { return "emul"; }
int loci_emu_reset_take(void) { return 0; }
int loci_emu_idle_poll(int cycles) { (void)cycles; return 0; }   /* in co-sim, the MENU button is simulated by the host */

/* ── USB HID mouse (co-sim) ─────────────────────────────────────────
 * Bridge to the real firmware: see emul_hid.c on the ~/loci/emul side. Without it,
 * SDL reports went into the internal model's xram, which the 6502 no longer
 * reads in co-sim (io_bus.c routes the whole MIA to the firmware). */
bool loci_emu_mou_report(uint8_t buttons, int8_t dx, int8_t dy,
                         int8_t wheel, int8_t pan)
{
    if (!g_boot_done) return false;
    return emul_loci_mou_report(&g_emul, buttons, dx, dy, wheel, pan) != 0;
}

bool loci_emu_mou_armed(void)
{
    if (!g_boot_done) return false;
    return emul_loci_mou_armed(&g_emul, NULL) != 0;
}

bool loci_emu_kbd_report(uint8_t modifier, const uint8_t keycodes[6])
{
    if (!g_boot_done) return false;
    return emul_loci_kbd_report(&g_emul, modifier, keycodes) != 0;
}

bool loci_emu_kbd_armed(void)
{
    if (!g_boot_done) return false;
    return emul_loci_kbd_armed(&g_emul, NULL) != 0;
}

static bool g_button_warm;   /* last MENU press = warm freeze (IRQ trap, no reset) */
bool loci_emu_button_was_warm(void) { return g_button_warm; }

bool loci_emu_menu_button(void)
{
    if (!g_boot_started) return false;
    ensure_booted();               /* if the user presses MENU before boot finishes, wait */

    int nromdis_before = 0;
    emul_ext_lines(&g_emul, NULL, NULL, &nromdis_before);
    int armed = emul_loci_menu_button(&g_emul);
    int nromdis = 0;
    emul_ext_lines(&g_emul, NULL, NULL, &nromdis);
    /* WARM freeze (ROM already served): ext.c has installed the IRQ trap and pulsed nIRQ. The
     * pulse stays latched: io_bus/main deliver it to the 6502 as an EDGE on the next
     * drain (loci_emu_irq_take). Above all NO reset: the 6502 must spin at
     * $03BA until the firmware releases the trap towards the menu's restore.s. */
    g_button_warm = nromdis_before &&
                    (emul_loci_irq_peek(&g_emul) > 0 || emul_loci_irq_trap_armed(&g_emul));
    if (g_button_warm) {
        log_info("LOCI-emu: bouton MENU à chaud → trap IRQ posé, nIRQ pulsé (%d) — "
                 "le 6502 gèle dans le menu via $03BA, pas de reset",
                 emul_loci_irq_peek(&g_emul));
        return false;
    }
    if (armed) {
        uint8_t lo = 0, hi = 0;
        emul_loci_serve_read(&g_emul, 0xFFFC, &lo); emul_loci_serve_read(&g_emul, 0xFFFD, &hi);
        log_info("LOCI-emu: bouton MENU → service ROM ARMÉ (nROMDIS=%d), vecteur reset servi = $%04X "
                 "— le 6502 va redémarrer dans le menu", nromdis, (unsigned)(lo | (hi << 8)));
    } else {
        log_warning("LOCI-emu: bouton MENU pressé mais service non armé (nROMDIS=%d)", nromdis);
    }
    return armed != 0;
}

bool loci_emu_diag_button(void)
{
    if (!g_boot_started) return false;
    ensure_booted();
    int armed = emul_loci_diag_button(&g_emul);
    int nromdis = 0;
    emul_ext_lines(&g_emul, NULL, NULL, &nromdis);
    if (armed) {
        uint8_t lo = 0, hi = 0;
        emul_loci_serve_read(&g_emul, 0xFFFC, &lo); emul_loci_serve_read(&g_emul, 0xFFFD, &hi);
        log_info("LOCI-emu: bouton MENU (appui long) → ROM de diagnostic servie (nROMDIS=%d), "
                 "vecteur reset = $%04X", nromdis, (unsigned)(lo | (hi << 8)));
    } else {
        log_warning("LOCI-emu: appui long sans effet — le firmware n'embarque pas de ROM de "
                    "diagnostic (EMBEDDED_TEST108K_ROM) ou le service n'est pas armé (nROMDIS=%d)",
                    nromdis);
    }
    return armed != 0;
}

bool loci_emu_rom_read(uint16_t address, uint8_t *out)
{
    if (!g_boot_done) return false;   /* during the background boot: Oric transparent */
    return emul_loci_serve_read(&g_emul, address, out) != 0;
}

bool loci_emu_romdis(void)
{
    if (!g_boot_done) return false;
    int nromdis = 0;
    emul_ext_lines(&g_emul, NULL, NULL, &nromdis);
    return nromdis != 0;
}

void loci_emu_ext_lines(int *nirq, int *nreset, int *nromdis)
{
    if (!g_boot_done) { if (nirq) *nirq = 0; if (nreset) *nreset = 0; if (nromdis) *nromdis = 0; return; }
    emul_ext_lines(&g_emul, nirq, nreset, nromdis);
}

/* ── Co-simulated MIA $03xx API (step 2) ──
 * Trace: LOCI_API_TRACE=<file> (or "-") — each call (op, A, X, pushed bytes
 * in ASCII) and its result (AX, SREG, errno) at the first released poll. */
static FILE *g_api_trace; static int g_api_trace_init;
static char g_api_push[300]; static int g_api_pushn;
static uint8_t g_api_a, g_api_x; static int g_api_pending;
static void api_trace_init(void)
{
    g_api_trace_init = 1;
    const char *path = getenv("LOCI_API_TRACE");
    if (path && *path) {
        g_api_trace = (path[0] == '-' && !path[1]) ? stderr : fopen(path, "w");
        if (g_api_trace) setvbuf(g_api_trace, NULL, _IOLBF, 0);
    }
}
static void api_trace_write(uint16_t address, uint8_t value)
{
    if (!g_api_trace_init) api_trace_init();
    if (!g_api_trace) return;
    switch (address & 0xFF) {
    case 0xAC: if (g_api_pushn < (int)sizeof g_api_push - 1) g_api_push[g_api_pushn++] = (char)value; break;
    case 0xB4: g_api_a = value; break;
    case 0xB6: g_api_x = value; break;
    case 0xAF: {
        fprintf(g_api_trace, "OP %02X A=%02X X=%02X push[%d]=\"", value, g_api_a, g_api_x, g_api_pushn);
        for (int i = g_api_pushn - 1; i >= 0; i--) {   /* last pushed = 1st character */
            unsigned char c = (unsigned char)g_api_push[i];
            if (c >= 0x20 && c < 0x7F) fputc(c, g_api_trace); else fprintf(g_api_trace, "\\x%02X", c);
        }
        fprintf(g_api_trace, "\"\n");
        g_api_pushn = 0; g_api_pending = (value != 0);
        break; }
    default: break;
    }
}
static void api_trace_result(void)
{
    if (!g_api_trace || !g_api_pending) return;
    uint8_t *io = g_emul.cpu0.sram + (0x20040000u - 0x20000000u);
    if (io[0xB2] == 0xFE) return;             /* still blocked */
    g_api_pending = 0;
    fprintf(g_api_trace, "   -> AX=%04X SREG=%04X errno=%u\n", io[0xB4] | (io[0xB6] << 8),
            io[0xB8] | (io[0xB9] << 8), io[0xAD] | (io[0xAE] << 8));
}

/* A 6502 access to the MIA page during the background boot (1st launch of an
 * ELF, without snapshot): WAIT for the boot to finish rather than serving a
 * floating bus. On hardware, no program loaded from tape calls the API
 * before LOCI (≈ 1 s) is ready; here `--tape X.tap -f` gets there within a few
 * million cycles, and an `open("N:…")` failed on the first launch only
 * (test-bench artifact, not a firmware issue). Without `--loci-emu`, we never get here. */
static void api_wait_boot(void)
{
    if (!g_boot_done && g_boot_started) ensure_booted();
}
bool loci_emu_wait_boot(void)
{
    api_wait_boot();
    return g_boot_done != 0;
}

void loci_emu_api_write(uint16_t address, uint8_t value)
{
    api_wait_boot();
    if (!g_boot_done) return;                 /* boot not finished: LOCI transparent */
    api_trace_write(address, value);
    emul_loci_api_write(&g_emul, address, value);
    api_trace_result();
}

uint8_t loci_emu_api_read(uint16_t address)
{
    api_wait_boot();
    if (!g_boot_done) return 0xFF;            /* floating bus until booted */
    uint8_t v = emul_loci_api_read(&g_emul, address);
    api_trace_result();
    return v;
}

/* ── Co-simulated Microdisc $031x (firmware's oric/dsk.c) ──
 * Diagnostic trace: LOCI_DSK_TRACE=<file> (or "-" = stderr), one access per
 * line (dir, register, value) — identical repeated polls are counted, not
 * repeated. Same spirit as LOCI_ACIA_TRACE. */
static FILE *g_dsk_trace; static int g_dsk_trace_init;
static void dsk_trace(char dir, uint16_t address, uint8_t value)
{
    static const char *reg[16] = { "CMD/STAT", "TRACK", "SECT", "DATA", "CTRL/IRQ", "?5", "?6", "?7",
                                   "DRQ", "?9", "?A", "?B", "?C", "?D", "?E", "?F" };
    static char last_dir; static uint16_t last_addr; static uint8_t last_val; static unsigned repeat;
    if (!g_dsk_trace_init) {
        const char *path = getenv("LOCI_DSK_TRACE");
        g_dsk_trace_init = 1;
        if (path && *path) {
            g_dsk_trace = (path[0] == '-' && !path[1]) ? stderr : fopen(path, "w");
            if (g_dsk_trace) { setvbuf(g_dsk_trace, NULL, _IOLBF, 0);
                               fprintf(g_dsk_trace, "# trace Microdisc co-sim ($0310-$0318) — dir reg val [xN]\n"); }
        }
    }
    if (!g_dsk_trace) return;
    if (dir == last_dir && address == last_addr && value == last_val) { repeat++; return; }
    if (repeat) fprintf(g_dsk_trace, "   (x%u)\n", repeat + 1);
    repeat = 0; last_dir = dir; last_addr = address; last_val = value;
    uint8_t st = 0; uint32_t pos = 0, start = 0, len = 0;
    emul_loci_dsk_debug(&g_emul, &st, &pos, &start, &len);
    fprintf(g_dsk_trace, "%c $%04X %-8s %02X   [state=%u pos=%u start=%u len=%u]\n", dir, address,
            reg[address & 0xF], value, st, pos, start, len);
}

/* ── Co-simulated tape $031x (firmware's oric/tap.c) ── */
void loci_emu_tap_write(uint16_t address, uint8_t value)
{
    if (!g_boot_done) return;
    emul_loci_tap_write(&g_emul, address, value);
}

uint8_t loci_emu_tap_read(uint16_t address)
{
    if (!g_boot_done) return 0xFF;
    return emul_loci_tap_read(&g_emul, address);
}

void loci_emu_tap_motor(uint8_t via_orb)
{
    /* The ROM rewrites ORB on every keyboard scan column: only replay
     * tap_act() (guest-call = expensive) on a CHANGE of PB6, otherwise the
     * co-sim collapses (×50) as soon as BASIC waits for a key. */
    static int last = -1;
    int motor = (via_orb >> 6) & 1;
    if (!g_boot_done || motor == last) return;
    last = motor;
    emul_loci_tap_motor(&g_emul, via_orb);
}

void loci_emu_dsk_tick(void)
{
    if (!g_boot_done) return;
    emul_loci_dsk_tick(&g_emul);
}

void loci_emu_dsk_write(uint16_t address, uint8_t value)
{
    if (!g_boot_done) return;
    dsk_trace('W', address, value);
    emul_loci_dsk_write(&g_emul, address, value);
}

uint8_t loci_emu_dsk_read(uint16_t address)
{
    if (!g_boot_done) return 0xFF;
    uint8_t v = emul_loci_dsk_read(&g_emul, address);
    dsk_trace('R', address, v);
    return v;
}

/* nIRQ pulses captured by the emulator since the last call (the firmware pulses the
 * line: ext_put true→false; a level poll would miss them). EDGE model:
 * io_bus.c drains these pulses right after each MIA transaction (synchronous) and
 * main.c once per frame after loci_emu_tick() (asynchronous) → one EDGE IRQ
 * (cpu_irq_pulse) per pulse, with no level holding and hence no storm. */
int loci_emu_irq_take(void)
{
    if (!g_boot_done) return 0;
    return emul_loci_irq_take(&g_emul);
}

/* BOUNDED free-run (deterministic, main thread): advances the firmware by `steps`
 * RP2040 steps WITHOUT driving the bus (Phi2 idle = HIGH, 1u<<25).
 *
 * ⚠️ DO NOT call between two MIA transactions of an operation in progress. Advancing the
 * firmware with Phi2 HIGH desynchronizes the bus service state machine during
 * a multi-step operation (e.g. opening a file) → the operation never completes,
 * the 6502 stays stuck on `BVC *` → FROZEN MENU (regression observed when
 * main.c called it once per frame). The firmware must advance ONLY IN SYNC with
 * the transactions. Kept for a possible offline use (no disk access
 * in flight); currently NOT called. */
void loci_emu_tick(long steps)
{
    if (!g_boot_done || steps <= 0) return;
    emul_bus_set(1u << 25, 1u << 25);         /* Phi2 idle = HIGH */
    emul_step(&g_emul, steps);
}

/* ── ACIA $0380-$0383 served by the firmware (CDC Phase 2) ── */
void loci_emu_set_cdc_device(const char *path)
{
    if (path) snprintf(g_cdc_dev, sizeof(g_cdc_dev), "%s", path);
}

bool loci_emu_acia_active(void) { return g_boot_done && g_cdc_fd >= 0; }

bool loci_emu_acia_served(uint16_t address)
{
    if (!g_boot_done) return false;
    uint16_t base = emul_loci_acia_base(&g_emul);
    return base && address >= base && address <= base + 3;
}

/* ── Co-simulated ACIA dialogue trace (diagnostic) ───────────────────
 * Enabled by the environment variable LOCI_ACIA_TRACE=<file> (or "-" for
 * stderr). Logs EVERY 6502 access to the $0380-$0383 registers served by the
 * firmware: direction, register, value, ASCII, and the relative timestamp in ms. It is
 * the tool to pinpoint the problem when the AT dialogue goes wrong in co-sim while
 * it works directly (--serial com:): compare what the 6502 reads here with
 * what the dongle actually sent. Zero cost when the variable is absent. */
static FILE  *g_acia_trace;
static int    g_acia_trace_init;
static double g_acia_t0;

static double acia_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1.0e6;
}

static void acia_trace(char dir, uint16_t address, uint8_t value)
{
    static const char *reg[4] = { "DATA", "STAT", "CMD ", "CTRL" };
    if (!g_acia_trace_init) {
        const char *path = getenv("LOCI_ACIA_TRACE");
        g_acia_trace_init = 1;
        if (path && *path) {
            g_acia_trace = (path[0] == '-' && !path[1]) ? stderr : fopen(path, "w");
            if (g_acia_trace) {
                g_acia_t0 = acia_now_ms();
                setvbuf(g_acia_trace, NULL, _IOLBF, 0);   /* line by line: readable even if the emulator is killed */
                fprintf(g_acia_trace, "# trace ACIA co-sim ($0380-$0383) — ms dir reg val ascii\n");
            }
        }
    }
    if (!g_acia_trace) return;
    fprintf(g_acia_trace, "%9.1f %c %s %02X %c\n", acia_now_ms() - g_acia_t0, dir,
            reg[address & 3], value,
            (value >= 0x20 && value < 0x7F) ? (char)value : '.');
}

/* NB: any nIRQ pulses (ACIA RX/TX/cmd) are drained by the caller via
 * loci_emu_irq_take() (io_bus.c after the access + main.c once per frame) — as
 * for the MIA window. loci_emu.c does not touch the host 6502. */
void loci_emu_acia_write(uint16_t address, uint8_t value)
{
    if (!loci_emu_acia_served(address)) return;
    acia_trace('W', address, value);
    emul_loci_acia_write(&g_emul, address, value);
}

uint8_t loci_emu_acia_read(uint16_t address)
{
    if (!loci_emu_acia_served(address)) return 0xFF;
    uint8_t v = emul_loci_acia_read(&g_emul, address);
    acia_trace('R', address, v);
    return v;
}

uint8_t loci_emu_acia_peek(uint16_t address)
{
    if (!loci_emu_acia_served(address)) return 0xFF;
    return emul_loci_acia_peek(&g_emul, address);
}

/* Pumps the CDC↔registers exchange (asynchronous RX from the dongle) — to be called once per
 * frame when loci_emu_acia_active(). */
void loci_emu_acia_tick(void)
{
    if (!loci_emu_acia_active()) return;
    emul_loci_acia_task(&g_emul);
}

bool loci_emu_rom_write(uint16_t address, uint8_t value) { (void)address; (void)value; return false; }

/* Whole I/O page by bus cycles: specific to the neo backend (loci-fw). */
bool    loci_emu_io_page(void) { return false; }
bool    loci_emu_io_read(uint16_t address, uint8_t *out) { (void)address; (void)out; return false; }
void    loci_emu_io_write(uint16_t address, uint8_t value) { (void)address; (void)value; }
