/* _DEFAULT_SOURCE : cfmakeraw + B115200 (config raw du dongle CDC, glibc). */
#define _DEFAULT_SOURCE
/* loci_emu.c — backend LOCI par émulation du vrai firmware RP2040. Voir loci_emu.h.
 *
 * Étape 1 : OVERLAY ROM. Le firmware réel sert la ROM de boot LOCI ($C000-$FFFF)
 * quand nROMDIS est actif ; memory_read() de Phosphoric délègue à loci_emu_rom_read().
 * Étape 2 : API MIA $03xx co-simulée (loci_emu_api_read/write).
 *
 * MODÈLE D'EXÉCUTION — MONO-THREAD (déterministe) : le firmware boote dans un
 * thread au démarrage (zéro attente), puis ce thread REND LA MAIN. Ensuite tout
 * l'accès à l'émulateur se fait depuis le thread principal de Phosphoric :
 *  - `loci_emu_tick()` : free-run BORNÉ (n pas RP2040) une fois par frame, pour que
 *    le firmware progresse (tâches de fond, nIRQ) sans piloter le bus ;
 *  - `loci_emu_api_*` / `loci_emu_rom_read` : accès bus co-simulés.
 * Un thread émulateur DÉDIÉ (free-run continu) a été essayé mais RETIRÉ : sur cette
 * cible il provoque une lourde contention et déstabilise les lignes partagées
 * (régression ×100 + BASIC cassé) — le débit émulateur (~1/10 réel) ne le justifie
 * pas. Le tick borné suffit et reste déterministe. */
#include "io/loci_emu.h"
#include "utils/logging.h"
#include "emul_lib.h"          /* ~/loci/emul/src (via -I dans le Makefile) */
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
static volatile int g_boot_done;   /* 1 quand le firmware a fini de booter (idle) */
static int       g_boot_started, g_joined;
static char      g_snap_path[512]; /* cache d'état « boot terminé » = <elf>.snap  */
static char      g_usb_image[1024]; /* image FAT servie comme disque USB émulé (option) */
static char      g_cdc_dev[1024];  /* dongle CDC (ex. /dev/ttyACM0 ou PTY) servi comme modem $0380 */
static int       g_cdc_fd = -1;    /* descripteur ouvert du dongle (>=0 = ACIA routée vers le firmware) */

/* Sortie UART0 du firmware -> log Phosphoric (une ligne à la fois). */
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

/* Ouvre le dongle CDC. Si c'est un TTY (ex. /dev/ttyACM0), le passe en RAW B115200
 * (le vrai PicoWifiModemUSB) ; sinon (PTY, socket) l'utilise tel quel. Non-bloquant.
 * Renvoie le fd (>=0) ou -1. */
static int cdc_open(const char *path)
{
    int fd = open(path, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) return -1;
    struct termios tio;
    if (tcgetattr(fd, &tio) == 0) {          /* TTY : configuration raw B115200 */
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
    /* Le snapshot (emul_lib, SNAP_VERSION 3) persiste SRAM+bootrom + les PAGES FLASH
     * écrites par le firmware (littlefs) → FS cohérent au restore (plus de « Corrupted
     * dir pair » au 2e lancement). Boot instantané conservé. */
    int cached = emul_loci_boot_cached(&g_emul, g_snap_path[0] ? g_snap_path : NULL);
    log_info("LOCI-emu: %s", cached ? "état restauré depuis le snapshot (boot instantané)"
                                    : "boot complet effectué (snapshot écrit pour la prochaine fois)");
    /* Monte l'image USB émulée (drive « 1: ») si fournie — après le boot/restore
     * (le snapshot précède le montage). Sans image, aucun disque USB (inchangé). */
    if (g_usb_image[0]) {
        int mounted = emul_loci_usb_mount(&g_emul);
        log_info("LOCI-emu: disque USB émulé « %s » -> %s", g_usb_image,
                 mounted ? "monté (drive 1:)" : "échec du montage");
    }
    /* Modem CDC (dongle réel /dev/ttyACM0 ou PTY) : attache + montage APRÈS le boot.
     * L'ACIA $0380 du firmware parlera à ce descripteur (au lieu du 6551 comportemental). */
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

/* Attend la fin du boot arrière-plan (idempotent). */
static void ensure_booted(void)
{
    if (g_boot_started && !g_joined) { pthread_join(g_boot_thread, NULL); g_joined = 1; }
}

int loci_emu_start(const char *elf_path)
{
    g_emul.uart_tx = uart_cb; g_emul.uart_user = NULL;
    if (emul_init(&g_emul, elf_path, 0x10000100u) != 0) {   /* rapide : charge l'ELF, ne step pas */
        log_error("LOCI-emu: emul_init a échoué (%s)", elf_path);
        return -1;
    }
    snprintf(g_snap_path, sizeof(g_snap_path), "%s.snap", elf_path);
    /* Enregistre l'HLE USB MSC AVANT le boot (les hooks doivent être posés avant
     * le montage ; le snapshot restore ne les écrase pas — hors cpu_snap_t). */
    if (g_usb_image[0] && !emul_loci_set_usb_image(&g_emul, g_usb_image)) {
        log_warning("LOCI-emu: image USB « %s » illisible — disque USB désactivé", g_usb_image);
        g_usb_image[0] = '\0';
    }
    log_info("LOCI-emu: firmware RP2040 (%s) — boot en arrière-plan (LOCI transparent)…", elf_path);

    if (pthread_create(&g_boot_thread, NULL, boot_thread_fn, NULL) == 0) {
        g_boot_started = 1;        /* boot asynchrone : loci_emu_start rend la main tout de suite */
    } else {
        boot_thread_fn(NULL);      /* repli synchrone si le thread échoue */
    }
    return 0;
}

/* Déclare l'image FAT à servir comme disque USB émulé (drive « 1: »). À appeler
 * AVANT loci_emu_start (le montage a lieu juste après le boot). */
void loci_emu_set_usb_image(const char *path)
{
    if (path) snprintf(g_usb_image, sizeof(g_usb_image), "%s", path);
}

void loci_emu_stop(void) { }   /* plus de thread persistant (mono-thread post-boot) */

bool loci_emu_active(void) { return g_boot_done != 0; }

/* ── Souris USB HID (co-sim) ─────────────────────────────────────────
 * Pont vers le firmware réel : voir emul_hid.c côté ~/loci/emul. Sans lui,
 * les rapports SDL partaient dans le xram du modèle interne, que le 6502 ne
 * lit plus en co-sim (io_bus.c route tout le MIA vers le firmware). */
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

bool loci_emu_menu_button(void)
{
    if (!g_boot_started) return false;
    ensure_booted();               /* si l'utilisateur presse MENU avant la fin du boot, on attend */

    int armed = emul_loci_menu_button(&g_emul);
    int nromdis = 0;
    emul_ext_lines(&g_emul, NULL, NULL, &nromdis);
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
    if (!g_boot_done) return false;   /* pendant le boot arrière-plan : Oric transparent */
    return emul_loci_serve_read(&g_emul, address, out) != 0;
}

void loci_emu_ext_lines(int *nirq, int *nreset, int *nromdis)
{
    if (!g_boot_done) { if (nirq) *nirq = 0; if (nreset) *nreset = 0; if (nromdis) *nromdis = 0; return; }
    emul_ext_lines(&g_emul, nirq, nreset, nromdis);
}

/* ── API MIA $03xx co-simulée (étape 2) ── */
void loci_emu_api_write(uint16_t address, uint8_t value)
{
    if (!g_boot_done) return;                 /* boot pas fini : LOCI transparent */
    emul_loci_api_write(&g_emul, address, value);
}

uint8_t loci_emu_api_read(uint16_t address)
{
    if (!g_boot_done) return 0xFF;            /* bus flottant tant que non booté */
    return emul_loci_api_read(&g_emul, address);
}

/* Pulses nIRQ captés par l'émulateur depuis le dernier appel (le firmware pulse la
 * ligne : ext_put true→false ; un poll de niveau les manquerait). Modèle EDGE :
 * io_bus.c draine ces pulses juste après chaque transaction MIA (synchrones) et
 * main.c une fois par frame après loci_emu_tick() (asynchrones) → une IRQ EDGE
 * (cpu_irq_pulse) par pulse, sans maintien de niveau donc sans tempête. */
int loci_emu_irq_take(void)
{
    if (!g_boot_done) return 0;
    return emul_loci_irq_take(&g_emul);
}

/* Free-run BORNÉ (déterministe, thread principal) : avance le firmware de `steps`
 * pas RP2040 SANS piloter le bus (Phi2 au repos = HAUT, 1u<<25).
 *
 * ⚠️ NE PAS appeler entre deux transactions MIA d'une opération en cours. Avancer le
 * firmware avec Phi2 HAUT désynchronise la machine à états de service du bus pendant
 * une opération multi-étapes (ex. ouverture de fichier) → l'opération ne se termine
 * plus, le 6502 reste bloqué sur `BVC *` → MENU FIGÉ (régression constatée quand
 * main.c l'appelait une fois par frame). Le firmware ne doit avancer QU'EN SYNC avec
 * les transactions. Conservée pour un éventuel usage hors-ligne (aucun accès disque
 * en vol) ; actuellement NON appelée. */
void loci_emu_tick(long steps)
{
    if (!g_boot_done || steps <= 0) return;
    emul_bus_set(1u << 25, 1u << 25);         /* Phi2 au repos = HAUT */
    emul_step(&g_emul, steps);
}

/* ── ACIA $0380-$0383 servie par le firmware (Phase 2 CDC) ── */
void loci_emu_set_cdc_device(const char *path)
{
    if (path) snprintf(g_cdc_dev, sizeof(g_cdc_dev), "%s", path);
}

bool loci_emu_acia_active(void) { return g_boot_done && g_cdc_fd >= 0; }

/* ── Trace du dialogue ACIA co-simulé (diagnostic) ───────────────────
 * Activée par la variable d'environnement LOCI_ACIA_TRACE=<fichier> (ou "-" pour
 * stderr). Journalise CHAQUE accès 6502 aux registres $0380-$0383 servis par le
 * firmware : sens, registre, valeur, ASCII, et l'horodatage relatif en ms. C'est
 * l'outil de localisation quand le dialogue AT part de travers en co-sim alors
 * qu'il passe en direct (--serial com:) : on compare ce que le 6502 lit ici avec
 * ce que le dongle a réellement émis. Aucun coût quand la variable est absente. */
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
                setvbuf(g_acia_trace, NULL, _IOLBF, 0);   /* ligne par ligne : lisible même si l'émulateur est tué */
                fprintf(g_acia_trace, "# trace ACIA co-sim ($0380-$0383) — ms dir reg val ascii\n");
            }
        }
    }
    if (!g_acia_trace) return;
    fprintf(g_acia_trace, "%9.1f %c %s %02X %c\n", acia_now_ms() - g_acia_t0, dir,
            reg[address & 3], value,
            (value >= 0x20 && value < 0x7F) ? (char)value : '.');
}

/* NB : les pulses nIRQ éventuels (ACIA RX/TX/cmd) sont drainés par l'appelant via
 * loci_emu_irq_take() (io_bus.c après l'accès + main.c une fois par frame) — comme
 * pour la fenêtre MIA. loci_emu.c ne touche pas au 6502 hôte. */
void loci_emu_acia_write(uint16_t address, uint8_t value)
{
    if (!loci_emu_acia_active()) return;
    acia_trace('W', address, value);
    emul_loci_acia_write(&g_emul, address, value);
}

uint8_t loci_emu_acia_read(uint16_t address)
{
    if (!loci_emu_acia_active()) return 0xFF;
    uint8_t v = emul_loci_acia_read(&g_emul, address);
    acia_trace('R', address, v);
    return v;
}

uint8_t loci_emu_acia_peek(uint16_t address)
{
    if (!loci_emu_acia_active()) return 0xFF;
    return emul_loci_acia_peek(&g_emul, address);
}

/* Pompe l'échange CDC↔registres (RX asynchrone du dongle) — à appeler une fois par
 * frame quand loci_emu_acia_active(). */
void loci_emu_acia_tick(void)
{
    if (!loci_emu_acia_active()) return;
    emul_loci_acia_task(&g_emul);
}
