/* SPDX-License-Identifier: EUPL-1.2 */
/**
 * @file emu_clock.c
 * @brief Horloge maître : un appel = un cycle de TOUTE la machine (V2-E2)
 * @author bmarty <bmarty@mailo.com>
 * @date 2026-09-11
 *
 * Avant ce module, le temps de la machine était réparti entre trois endroits :
 * le CPU avançait son horloge, un rappel par cycle (`cpu_cycle_tick` dans
 * main.c) faisait avancer les périphériques φ2, et **la boucle principale
 * calculait elle-même la position du balayage** (`frame_cycles / 64`) pour
 * émettre les scanlines. Conséquence : seule la boucle principale savait
 * cadencer la machine ; le débogueur, les tests et les outils ne pouvaient pas
 * faire avancer « un cycle de machine ».
 *
 * `emu_cycle()` est ce point unique, avec un **ordre intra-cycle figé** :
 *
 *   1. **le CPU** exécute son unique cycle : un accès bus (lecture, écriture,
 *      ou accès factice du NMOS) avec le cœur micro-séquencé. Sur le matériel,
 *      c'est la première chose qui se passe après le front montant de l'horloge
 *      1 MHz : le compteur horizontal de l'ULA s'incrémente et le 6502 fait son
 *      accès DRAM (c'est son CAS qui écrit).
 *   2. **les périphériques φ2** (VIA, FDC, ACIA, DTL, Mageco, cassette) sont
 *      avancés d'exactement un cycle par le rappel d'horloge du CPU, juste après
 *      l'accès bus. Avec le cœur par défaut ce rappel reçoit toujours
 *      `cycles = 1` : les périphériques sont donc déjà au cycle, sans paquet
 *      (vérifié par `test-clock`).
 *   3. **l'ULA** fetche ENSUITE l'octet écran de ce même count (moitié basse du
 *      cycle — l'ULA prend plus de la moitié, d'où l'horloge 1 MHz asymétrique),
 *      puis le balayage avance et les scanlines dues / le tick ULA-NG sont émis.
 *      Une écriture du CPU au cycle *c* est donc vue par la cellule *c*.
 *
 * Cet ordre est celui MESURÉ à l'oscilloscope par Mike Brown (Unofficial ULA
 * Guide 1.02, « Control and Sequencing ») : 6502 d'abord, puis « the ULA access
 * cycle begins ». Jusqu'en 2.0.1 Phosphoric faisait l'inverse (ULA puis CPU,
 * écriture visible à c+1) : un split raster tombait une cellule trop à droite.
 * Sur l'ORIC, l'ULA et le CPU ne se disputent pas la RAM (accès en phases
 * opposées) : il n'y a donc pas de vol de cycle à modéliser, contrairement à un
 * ZX Spectrum.
 *
 * Le cœur historique (`--cpu-legacy`) ne sait pas s'arrêter entre deux cycles :
 * `emu_cycle()` y exécute alors une instruction entière puis rattrape le
 * balayage du même nombre de cycles. Le résultat est identique à l'ancienne
 * boucle, à l'ordonnancement interne près.
 */

#include "emulator.h"
#include "cpu/microseq.h"
#include "video/video.h"
#include "io/ula_ng.h"

/* ─── ULA au cycle (V2-E4) ───
 * Émet le travail vidéo du cycle courant : début de ligne, fetch d'une cellule,
 * fin de ligne. Elle a lieu APRÈS l'accès du CPU du même cycle (ordre mesuré sur
 * le matériel), donc une écriture du CPU pendant ce cycle est vue par la cellule
 * fetchée ce cycle — et une écriture en milieu de ligne n'affecte que les
 * cellules pas encore fetchées.
 *
 * `dot` est le cycle dans la ligne (0-63). La colonne fetchée est
 * `dot - ula_fetch_offset` : 40 cellules visibles, le reste de la ligne étant
 * bordure et blanking. */
/* Le fetch par cycle exige un cœur capable de s'arrêter entre deux cycles : avec
 * `--cpu-legacy`, l'instruction est indivisible, donc on retombe sur le rendu par
 * ligne. Sans cette garde, l'écran resterait noir dans ce mode. */
static bool ula_cycle_in_use(const emulator_t* emu) {
    return emu->ula_per_cycle && cpu_microseq_enabled(&emu->cpu);
}

static void ula_cycle(emulator_t* emu, int line, int dot) {
    if (line >= 224) return;                    /* blanking vertical */
    const uint8_t* mem = emu->memory.ram;

    if (dot == 0) video_line_begin(&emu->video, mem, line);

    int col = dot - emu->ula_fetch_offset;
    if (col >= 0 && col <= 40)
        video_render_cell(&emu->video, mem, line, col);

    if (dot == PAL_CYCLES_PER_LINE - 1) {
        video_line_end(&emu->video, mem, line);
        emu->raster_rendered = line + 1;        /* cette ligne est complète */
    }
}

/* Fait avancer le balayage de `cycles` cycles : émet les scanlines visibles dues
 * (zone active 0-223) et les ticks raster ULA-NG (trame complète 0-311). */
static void clock_advance_raster(emulator_t* emu, int cycles) {
    emu->raster_cycle += cycles;
    emu->frame_cycles = emu->raster_cycle;   /* exposé aux points d'arrêt raster */

    /* Sortie rapide : 63 cycles sur 64 ne franchissent aucune fin de ligne et
     * n'ont donc rien à émettre. Ce test remplace une division par cycle —
     * l'horloge étant appelée un million de fois par seconde émulée, ça compte. */
    if (emu->raster_cycle < emu->raster_next_line) return;

    do {
        /* Rendu scanline : la ligne entière échantillonne la mémoire à l'instant
         * où le faisceau l'achève. En mode ULA au cycle, le rendu a déjà été
         * fait cellule par cellule par ula_cycle() — rien à faire ici. */
        if (!ula_cycle_in_use(emu) && emu->raster_rendered < 224) {
            video_render_scanline(&emu->video, emu->memory.ram, emu->raster_rendered);
            emu->raster_rendered++;
        }
        /* Raster ULA-NG sur la trame PAL complète, découplé de la zone visible :
         * assère la ligne d'IRQ quand la ligne programmée est franchie. */
        if (emu->raster_ng_line < ULA_NG_FRAME_LINES) {
            ula_ng_scanline(&emu->ula_ng, emu->raster_ng_line);
            if (ula_ng_irq(&emu->ula_ng)) cpu_irq_set(&emu->cpu, IRQF_ULANG);
            emu->raster_ng_line++;
        }
        emu->raster_next_line += PAL_CYCLES_PER_LINE;
    } while (emu->raster_cycle >= emu->raster_next_line);
}

bool emu_cycle(emulator_t* emu) {
    if (cpu_microseq_enabled(&emu->cpu)) {
        /* Ordre MESURÉ sur le matériel (Mike Brown, Unofficial ULA Guide 1.02) :
         * au front montant de l'horloge 1 MHz, le compteur horizontal s'incrémente
         * et le 6502 fait son accès (c'est son CAS qui écrit) ; l'ULA fetche
         * ENSUITE l'octet de ce même count, pendant la moitié basse du cycle.
         * Une écriture du CPU au cycle c est donc vue par la cellule c. */
        bool last = cpu_cycle(&emu->cpu);    /* le CPU, puis ses périphériques φ2 */
        if (ula_cycle_in_use(emu))
            ula_cycle(emu, emu->raster_cycle / PAL_CYCLES_PER_LINE,
                      emu->raster_cycle % PAL_CYCLES_PER_LINE);
        clock_advance_raster(emu, 1);
        return last;
    }
    /* Cœur historique : indivisible. Une instruction, puis le balayage. */
    int n = cpu_step(&emu->cpu);
    clock_advance_raster(emu, n);
    return true;
}

int emu_step(emulator_t* emu) {
    uint64_t before = emu->cpu.cycles;
    while (!emu_cycle(emu)) {
        if (emu->cpu.halted) break;
    }
    return (int)(emu->cpu.cycles - before);
}

void emu_clock_resume(emulator_t* emu) {
    if (!emu->clock_resume_pending) return;
    emu->clock_resume_pending = false;

    /* État sauvé après une fin de trame (sortie sur `-c`, `--save-state`) : la
     * trame était terminée, la suivante commence à zéro. */
    if (emu->raster_cycle >= CYCLES_PER_FRAME || emu->raster_cycle < 0) {
        emu->raster_cycle = 0;
        emu->raster_rendered = 0;
        emu->raster_ng_line = 0;
        emu->raster_next_line = PAL_CYCLES_PER_LINE;
        emu->frame_cycles = 0;
        return;
    }

    int line = emu->raster_cycle / PAL_CYCLES_PER_LINE;
    int dot  = emu->raster_cycle % PAL_CYCLES_PER_LINE;
    emu->raster_next_line = (line + 1) * PAL_CYCLES_PER_LINE;
    emu->frame_cycles = emu->raster_cycle;

    /* Lignes déjà balayées avant la sauvegarde : rendues d'un bloc depuis la RAM
     * restaurée (même approximation que la fin de trame après un halt). */
    int done = line < 224 ? line : 224;
    for (int y = 0; y < done; y++)
        video_render_scanline(&emu->video, emu->memory.ram, y);
    emu->raster_rendered = done;

    /* Ligne en cours en mode ULA au cycle : rejoue les cellules déjà fetchées
     * pour reconstituer l'état série de ligne (encre, papier, attributs). */
    if (ula_cycle_in_use(emu) && line < 224) {
        video_line_begin(&emu->video, emu->memory.ram, line);
        int col_end = dot - emu->ula_fetch_offset;
        for (int col = 0; col < col_end && col <= 40; col++)
            video_render_cell(&emu->video, emu->memory.ram, line, col);
    }
}

void emu_clock_frame_begin(emulator_t* emu) {
    if (emu->clock_resume_pending) {
        emu_clock_resume(emu);
        return;
    }
    emu->raster_cycle = 0;
    emu->raster_rendered = 0;
    emu->raster_ng_line = 0;
    emu->raster_next_line = PAL_CYCLES_PER_LINE;
    emu->frame_cycles = 0;
}

void emu_clock_frame_end(emulator_t* emu) {
    /* Termine la trame même si le CPU s'est arrêté en plein écran (halt, point
     * d'arrêt) : l'image affichée doit être complète. Les lignes restantes sont
     * alors rendues d'un bloc — elles n'ont pas été balayées, il n'y a pas de
     * position intermédiaire à respecter. */
    while (emu->raster_rendered < 224) {
        video_render_scanline(&emu->video, emu->memory.ram, emu->raster_rendered);
        emu->raster_rendered++;
    }
}

void emu_raster_pos(const emulator_t* emu, int* line, int* dot) {
    if (line) *line = emu->raster_cycle / PAL_CYCLES_PER_LINE;
    if (dot)  *dot  = emu->raster_cycle % PAL_CYCLES_PER_LINE;
}
