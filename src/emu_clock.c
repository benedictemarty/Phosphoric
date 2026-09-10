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
 *   1. **φ1 — l'ULA** consomme son cycle (avancement du balayage, émission des
 *      scanlines dues, tick raster ULA-NG). Le vrai ULA de l'ORIC accède à la
 *      RAM pendant φ1, donc AVANT l'accès du CPU : une écriture du CPU au cycle
 *      *c* n'est visible par l'ULA qu'au cycle *c+1*.
 *   2. **φ2 — le CPU** exécute son unique cycle : un accès bus (lecture,
 *      écriture, ou accès factice du NMOS) avec le cœur micro-séquencé.
 *   3. **fin de cycle — les périphériques φ2** (VIA, FDC, ACIA, DTL, Mageco,
 *      cassette) sont avancés d'exactement un cycle par le rappel d'horloge du
 *      CPU, juste après l'accès bus. Avec le cœur par défaut ce rappel reçoit
 *      toujours `cycles = 1` : les périphériques sont donc déjà au cycle, sans
 *      paquet (vérifié par `test-clock`).
 *
 * Sur l'ORIC, l'ULA et le CPU ne se disputent pas la RAM (accès en phases
 * opposées) : il n'y a donc pas de vol de cycle à modéliser, contrairement à un
 * ZX Spectrum. L'ordre ci-dessus n'est pas un arbitrage, c'est une convention de
 * visibilité — et elle est testée.
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
        /* Rendu scanline : chaque ligne échantillonne la mémoire à l'instant
         * exact où le faisceau l'émet (l'ULA au fetch octet par cycle est
         * l'objet de V2-E4 ; ici la granularité reste la ligne). */
        if (emu->raster_rendered < 224) {
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
        clock_advance_raster(emu, 1);        /* φ1 : l'ULA d'abord */
        return cpu_cycle(&emu->cpu);         /* φ2 : le CPU, puis ses périphériques */
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

void emu_clock_frame_begin(emulator_t* emu) {
    emu->raster_cycle = 0;
    emu->raster_rendered = 0;
    emu->raster_ng_line = 0;
    emu->raster_next_line = PAL_CYCLES_PER_LINE;
    emu->frame_cycles = 0;
}

void emu_clock_frame_end(emulator_t* emu) {
    /* Termine la trame même si le CPU s'est arrêté en plein écran (halt, point
     * d'arrêt) : l'image affichée doit être complète. */
    while (emu->raster_rendered < 224) {
        video_render_scanline(&emu->video, emu->memory.ram, emu->raster_rendered);
        emu->raster_rendered++;
    }
}

void emu_raster_pos(const emulator_t* emu, int* line, int* dot) {
    if (line) *line = emu->raster_cycle / PAL_CYCLES_PER_LINE;
    if (dot)  *dot  = emu->raster_cycle % PAL_CYCLES_PER_LINE;
}
