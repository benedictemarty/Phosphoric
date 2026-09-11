/* SPDX-License-Identifier: EUPL-1.2 */
/**
 * @file ay3891x.c
 * @brief AY-3-8910 PSG emulation - 3 tone channels, noise, envelopes
 * @author bmarty <bmarty@mailo.com>
 * @date 2026-02-22
 * @version 0.6.0-alpha
 *
 * Sound generation supports two paths:
 *  - Immediate (ay_write_data): the historical model, used by tests and any
 *    caller that does not timestamp writes. ay_generate() renders a buffer
 *    from the current register state.
 *  - Timestamped (ay_write_data_timed): the live emulator stamps each
 *    sound-register write with its CPU cycle and queues it. ay_generate()
 *    replays the queued writes onto the `play` state at the exact intra-buffer
 *    sample position, so audio-rate volume hammering (4-bit "digidrums") is
 *    reproduced instead of collapsing to one value per ~46 ms buffer.
 */

#include "audio/audio.h"
#include <string.h>

/* Logarithmic volume table from Oricutron (matches real AY-3-8912 DAC curve) */
static const int32_t voltab[16] = {
    0, 513/4, 828/4, 1239/4, 1923/4, 3238/4, 4926/4, 9110/4,
    10344/4, 17876/4, 24682/4, 30442/4, 38844/4, 47270/4, 56402/4, 65535/4
};

void ay_init(ay3891x_t* ay, uint32_t clock_rate) {
    memset(ay, 0, sizeof(ay3891x_t));
    ay->clock_rate = clock_rate;
    ay->noise_shift = 1;
    ay->play.noise_shift = 1;
    /* Port A (reg 14) and Port B (reg 15) default to 0xFF (no input active).
     * On ORIC, PSG Port A is connected to the keyboard matrix (active low),
     * so 0xFF = no keys pressed. Without this, the ROM sees ghost keypresses. */
    ay->registers[14] = 0xFF;
    ay->registers[15] = 0xFF;
    ay->play.sregs[14] = 0xFF;
    ay->play.sregs[15] = 0xFF;
}

void ay_reset(ay3891x_t* ay) {
    uint32_t clk = ay->clock_rate;
    memset(ay, 0, sizeof(ay3891x_t));
    ay->clock_rate = clk;
    ay->noise_shift = 1;
    ay->play.noise_shift = 1;
    ay->registers[14] = 0xFF;
    ay->registers[15] = 0xFF;
    ay->play.sregs[14] = 0xFF;
    ay->play.sregs[15] = 0xFF;
}

void ay_write_address(ay3891x_t* ay, uint8_t addr) {
    ay->selected_reg = addr & 0x0F;
}

/* Recompute the derived sound parameters for a single register write.
 * Shared by the authoritative state (regs = ay->registers) and the playback
 * shadow (regs = ay->play.sregs) so both stay byte-exact. */
static void apply_sound_regs(const uint8_t* regs, uint16_t tone_period[3],
                             uint16_t* noise_period, uint16_t* env_period,
                             uint8_t* env_shape, uint8_t* env_step,
                             uint32_t* env_counter, bool* env_holding,
                             uint8_t reg) {
    switch (reg) {
    case 0: case 1: tone_period[0] = ((regs[1] & 0x0F) << 8) | regs[0]; break;
    case 2: case 3: tone_period[1] = ((regs[3] & 0x0F) << 8) | regs[2]; break;
    case 4: case 5: tone_period[2] = ((regs[5] & 0x0F) << 8) | regs[4]; break;
    case 6:  *noise_period = regs[6] & 0x1F; break;
    case 11: case 12: *env_period = (regs[12] << 8) | regs[11]; break;
    case 13:
        *env_shape = regs[13] & 0x0F;
        *env_step = 0;
        *env_counter = 0;
        *env_holding = false;
        break;
    default: break; /* reg 7 (mixer), 8-10 (volume), 14-15 (ports): no derived state */
    }
}

static void apply_sound_main(ay3891x_t* ay, uint8_t reg) {
    apply_sound_regs(ay->registers, ay->tone_period, &ay->noise_period,
                     &ay->env_period, &ay->env_shape, &ay->env_step,
                     &ay->env_counter, &ay->env_holding, reg);
}

static void apply_sound_play(ay_play_t* st, uint8_t reg) {
    apply_sound_regs(st->sregs, st->tone_period, &st->noise_period,
                     &st->env_period, &st->env_shape, &st->env_step,
                     &st->env_counter, &st->env_holding, reg);
}

void ay_write_data(ay3891x_t* ay, uint8_t data) {
    if (ay->selected_reg >= AY_NUM_REGISTERS) return;
    ay->registers[ay->selected_reg] = data;
    apply_sound_main(ay, ay->selected_reg);
}

/* Seed the playback state from the current authoritative state. Done once when
 * the live emulator first issues a timestamped write, so playback starts from
 * the sound already programmed. */
static void seed_play(ay3891x_t* ay) {
    memcpy(ay->play.sregs, ay->registers, AY_NUM_REGISTERS);
    for (int i = 0; i < 3; i++) {
        ay->play.tone_period[i] = ay->tone_period[i];
        ay->play.tone_counter[i] = ay->tone_counter[i];
        ay->play.tone_output[i] = ay->tone_output[i];
    }
    ay->play.noise_period = ay->noise_period;
    ay->play.noise_counter = ay->noise_counter;
    ay->play.noise_shift = ay->noise_shift ? ay->noise_shift : 1;
    ay->play.noise_output = ay->noise_output;
    ay->play.env_period = ay->env_period;
    ay->play.env_counter = ay->env_counter;
    ay->play.env_shape = ay->env_shape;
    ay->play.env_step = ay->env_step;
    ay->play.env_volume = ay->env_volume;
    ay->play.env_holding = ay->env_holding;
}

void ay_write_data_timed(ay3891x_t* ay, uint8_t data, uint64_t cpu_cycle) {
    uint8_t reg = ay->selected_reg;
    if (reg >= AY_NUM_REGISTERS) return;

    /* Authoritative state updates immediately (reads, keyboard, headless,
     * savestates all observe current values regardless of audio timing). */
    ay->registers[reg] = data;
    apply_sound_main(ay, reg);

    /* I/O ports (14, 15) have no sound effect — nothing to queue. */
    if (reg >= 14) return;

    if (!ay->play_seeded) {
        seed_play(ay);
        ay->play_seeded = true;
    }
    ay->timed_mode = true;

    /* SPSC enqueue: the CPU thread only advances head, the audio thread only
     * advances tail. On overflow we drop the newest write (bounded memory). */
    uint32_t head = atomic_load_explicit(&ay->evq_head, memory_order_relaxed);
    uint32_t tail = atomic_load_explicit(&ay->evq_tail, memory_order_acquire);
    uint32_t next = (head + 1) & (AY_EVENT_QUEUE_SIZE - 1);
    if (next == tail) return; /* full */
    ay->evq[head].cycle = cpu_cycle;
    ay->evq[head].reg = reg;
    ay->evq[head].val = data;
    atomic_store_explicit(&ay->evq_head, next, memory_order_release);
    atomic_store_explicit(&ay->cur_cycle, cpu_cycle, memory_order_release);
}

void ay_sound_resync(ay3891x_t* ay) {
    /* After a savestate load the authoritative fields are restored directly;
     * mirror them into the playback shadow and clear the (now meaningless)
     * pending event queue so timing restarts cleanly. */
    seed_play(ay);
    atomic_store_explicit(&ay->evq_tail,
        atomic_load_explicit(&ay->evq_head, memory_order_relaxed),
        memory_order_release);
    ay->last_gen_cycle = 0;
    ay->play_seeded = true;
}

uint8_t ay_read_data(ay3891x_t* ay) {
    if (ay->selected_reg >= AY_NUM_REGISTERS) return 0xFF;

    /* Port A (reg 14): when Port A is in input mode (reg7 bit 6 = 1),
     * return external input from callback (keyboard matrix on ORIC).
     * When bit 6 = 0, Port A is in output mode (PSG drives the bus),
     * so return the register value instead of the keyboard state.
     * This matches Oricutron: programs must set reg7 bit 6 = 1 ($7F)
     * to enable keyboard scanning after playing sound. */
    if (ay->selected_reg == 14 && (ay->registers[7] & 0x40)) {
        if (ay->porta_input) return ay->porta_input(ay->userdata);
        return 0xFF;
    }

    return ay->registers[ay->selected_reg];
}

static uint8_t envelope_volume(uint8_t shape, uint8_t step) {
    /* 16 shapes, 32-step cycle (2 x 16) */
    uint8_t s = step & 0x1F;
    bool first_half = s < 16;
    uint8_t pos = s & 0x0F;

    bool attack = (shape & 0x04) != 0;
    bool alternate = (shape & 0x02) != 0;
    bool hold = (shape & 0x01) != 0;

    if (!(shape & 0x08)) {
        /* Shapes 0-7: single decay then off */
        if (s >= 16) return 0;
        return attack ? pos : (15 - pos);
    }

    if (hold) {
        if (s >= 16) {
            if (alternate) return attack ? 0 : 15;
            return attack ? 15 : 0;
        }
        return attack ? pos : (15 - pos);
    }

    if (alternate) {
        if (first_half) return attack ? pos : (15 - pos);
        return attack ? (15 - pos) : pos;
    }

    return attack ? pos : (15 - pos);
}

/* ════════════════════════════════════════════════════════════════════
 *  Cœur du PSG cadencé au matériel (V2-E5)
 *
 *  L'AY-3-8910 divise son horloge d'entrée par 16 pour les générateurs, mais la
 *  sortie carrée bascule DEUX fois par période : le pas interne naturel est donc
 *  **clock/8** (125 kHz sur l'ORIC). À ce rythme :
 *
 *    ton       : bascule tous les TP pas      → f = clock / (16·TP)
 *    bruit     : LFSR avancé tous les 2·NP    → f = clock / (16·NP)
 *    enveloppe : un pas tous les EP           → f = clock / (8·EP),
 *                soit un cycle de 32 pas en clock/(256·EP), la formule de la
 *                datasheet.
 *
 *  Avant la V2, ces compteurs étaient cadencés par ACCUMULATEURS au taux
 *  d'échantillonnage (44,1 kHz) : les fréquences moyennes des tons tombaient
 *  juste, mais l'enveloppe était **deux fois trop lente** (clock/16 par pas) et
 *  toute transition plus rapide que 44,1 kHz repliait au lieu de se moyenner.
 *  Ici, la machine tourne à son vrai rythme et la sortie est **intégrée** sur les
 *  pas couverts par chaque échantillon (filtre boîte) : plus de repliement, et
 *  les périodes très courtes donnent une amplitude faible au lieu d'un bruit
 *  parasite — ce que fait aussi le haut-parleur d'un vrai ORIC.
 * ══════════════════════════════════════════════════════════════════ */

/* Un pas d'horloge interne (clock/8). */
static void ay_tick(ay_play_t* st) {
    for (int ch = 0; ch < 3; ch++) {
        uint32_t period = st->tone_period[ch];
        if (period == 0) period = 1;      /* période 0 se comporte comme 1 */
        if (++st->tone_counter[ch] >= period) {
            st->tone_counter[ch] = 0;
            st->tone_output[ch] ^= 1;
        }
    }

    {
        /* Le LFSR n'a pas le ÷2 de la bascule carrée : il avance tous les 2·NP
         * pas d'horloge interne, soit clock/(16·NP). */
        uint32_t np = st->noise_period ? st->noise_period : 1;
        if (++st->noise_counter >= np * 2u) {
            st->noise_counter = 0;
            /* LFSR 17 bits, polynôme x^17 + x^14 + 1 */
            uint32_t bit = ((st->noise_shift >> 0) ^ (st->noise_shift >> 3)) & 1;
            st->noise_shift = (st->noise_shift >> 1) | (bit << 16);
            st->noise_output = st->noise_shift & 1;
        }
    }

    if (st->env_period && !st->env_holding) {
        if (++st->env_counter >= (uint32_t)st->env_period) {
            st->env_counter = 0;
            st->env_step++;
            if (st->env_step >= 32) {
                if (!(st->env_shape & 0x08)) {
                    st->env_holding = true;
                    st->env_step = 31;
                } else if (st->env_shape & 0x01) {
                    st->env_holding = true;
                    st->env_step &= 0x1F;
                } else {
                    st->env_step &= 0x1F;
                }
            }
        }
    }
    st->env_volume = envelope_volume(st->env_shape, st->env_step);
}

/* Mixage instantané des trois canaux. */
static int32_t ay_mix(const ay_play_t* st) {
    uint8_t mixer = st->sregs[7];
    int32_t output = 0;
    for (int ch = 0; ch < 3; ch++) {
        bool tone_dis = (mixer >> ch) & 1;
        bool noise_dis = (mixer >> (ch + 3)) & 1;
        bool out = (st->tone_output[ch] | tone_dis) & (st->noise_output | noise_dis);
        if (out) {
            uint8_t vol_reg = st->sregs[8 + ch];
            uint8_t vol_idx = (vol_reg & 0x10) ? st->env_volume : (vol_reg & 0x0F);
            output += voltab[vol_idx];
        }
    }
    return output / 3;
}

/* Produit un échantillon de sortie : avance la machine des pas d'horloge
 * couverts par la durée de l'échantillon, en INTÉGRANT la sortie sur ces pas.
 * `steps_q16` = pas d'horloge interne par échantillon, en virgule fixe Q16. */
static int16_t ay_step_sample(ay_play_t* st, uint32_t steps_q16) {
    st->step_acc += steps_q16;
    uint32_t nsteps = st->step_acc >> 16;
    st->step_acc &= 0xFFFFu;

    if (nsteps == 0) return (int16_t)ay_mix(st);   /* sur-échantillonnage */

    int64_t sum = 0;
    for (uint32_t k = 0; k < nsteps; k++) {
        ay_tick(st);
        sum += ay_mix(st);
    }
    return (int16_t)(sum / (int64_t)nsteps);
}

/* Mirror the authoritative state into a transient playback state (immediate
 * path), and copy the evolving runtime back afterwards so accumulators persist
 * across calls exactly as the historical in-place model did. */
static void mirror_main_to_play(ay3891x_t* ay) {
    memcpy(ay->play.sregs, ay->registers, AY_NUM_REGISTERS);
    for (int i = 0; i < 3; i++) {
        ay->play.tone_period[i] = ay->tone_period[i];
        ay->play.tone_counter[i] = ay->tone_counter[i];
        ay->play.tone_output[i] = ay->tone_output[i];
    }
    ay->play.noise_period = ay->noise_period;
    ay->play.noise_counter = ay->noise_counter;
    ay->play.noise_shift = ay->noise_shift ? ay->noise_shift : 1;
    ay->play.noise_output = ay->noise_output;
    ay->play.env_period = ay->env_period;
    ay->play.env_counter = ay->env_counter;
    ay->play.env_shape = ay->env_shape;
    ay->play.env_step = ay->env_step;
    ay->play.env_volume = ay->env_volume;
    ay->play.env_holding = ay->env_holding;
}

static void copy_play_runtime_to_main(ay3891x_t* ay) {
    for (int i = 0; i < 3; i++) {
        ay->tone_counter[i] = ay->play.tone_counter[i];
        ay->tone_output[i] = ay->play.tone_output[i];
    }
    ay->noise_counter = ay->play.noise_counter;
    ay->noise_shift = ay->play.noise_shift;
    ay->noise_output = ay->play.noise_output;
    ay->env_counter = ay->play.env_counter;
    ay->env_step = ay->play.env_step;
    ay->env_volume = ay->play.env_volume;
    ay->env_holding = ay->play.env_holding;
}

void ay_generate(ay3891x_t* ay, int16_t* buffer, int num_samples) {
    /* Pas d'horloge interne (clock/8) par échantillon de sortie, en Q16.
     * Sur l'ORIC : 125 000 / 44 100 ≈ 2,834 pas par échantillon. */
    uint32_t steps_q16 = (uint32_t)(((uint64_t)(ay->clock_rate / 8) << 16)
                                    / (uint64_t)AUDIO_SAMPLE_RATE);
    ay_play_t* st = &ay->play;

    if (!ay->timed_mode) {
        /* Immediate path: render from current state, byte-exact with history. */
        mirror_main_to_play(ay);
        for (int i = 0; i < num_samples; i++) {
            int16_t s = ay_step_sample(st, steps_q16);
            buffer[i * 2] = s;
            buffer[i * 2 + 1] = s;
        }
        copy_play_runtime_to_main(ay);
        return;
    }

    /* Timestamped path: snapshot the pending events and map each to a sample
     * offset proportional to its CPU-cycle position within the span this buffer
     * covers (previous buffer end → most recent write). */
    uint32_t tail = atomic_load_explicit(&ay->evq_tail, memory_order_relaxed);
    uint32_t head = atomic_load_explicit(&ay->evq_head, memory_order_acquire);
    uint64_t span_end = atomic_load_explicit(&ay->cur_cycle, memory_order_acquire);
    uint64_t span_start = ay->last_gen_cycle;
    if (span_start == 0 && head != tail) span_start = ay->evq[tail].cycle;
    if (span_start > span_end) span_start = span_end;
    uint64_t span = (span_end > span_start) ? (span_end - span_start) : 0;

    uint32_t ev = tail;
    for (int i = 0; i < num_samples; i++) {
        while (ev != head) {
            uint64_t c = ay->evq[ev].cycle;
            int off;
            if (span == 0) {
                off = 0;
            } else {
                uint64_t rel = (c > span_start) ? (c - span_start) : 0;
                uint64_t o = rel * (uint64_t)num_samples / span;
                off = (o >= (uint64_t)num_samples) ? (num_samples - 1) : (int)o;
            }
            if (off > i) break;
            st->sregs[ay->evq[ev].reg] = ay->evq[ev].val;
            apply_sound_play(st, ay->evq[ev].reg);
            ev = (ev + 1) & (AY_EVENT_QUEUE_SIZE - 1);
        }
        int16_t s = ay_step_sample(st, steps_q16);
        buffer[i * 2] = s;
        buffer[i * 2 + 1] = s;
    }

    /* All snapshot events have been applied (offsets clamp to the last sample). */
    atomic_store_explicit(&ay->evq_tail, head, memory_order_release);
    ay->last_gen_cycle = span_end;
}
