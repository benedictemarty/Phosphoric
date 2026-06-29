/**
 * @file audio.h
 * @brief AY-3-8910 PSG and audio output interface
 */

#ifndef AUDIO_H
#define AUDIO_H

#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>

#define AY_NUM_CHANNELS  3
#define AY_NUM_REGISTERS 16  /* 14 sound + 2 I/O ports (Port A=14, Port B=15) */
#define AUDIO_SAMPLE_RATE 44100
#define AUDIO_BUFFER_SIZE 2048

/* Timestamped PSG register write queue (digidrums / sample-accurate audio).
 * Size must be a power of two. 4096 events ≈ 9 audio buffers of backlog at a
 * dense digidrum write rate; on overflow the newest write is dropped. */
#define AY_EVENT_QUEUE_SIZE 4096

/* One CPU-cycle-stamped sound-register write. */
typedef struct {
    uint64_t cycle;   /* CPU cycle at which the write occurred */
    uint8_t  reg;     /* sound register 0..13 */
    uint8_t  val;     /* value written */
} ay_event_t;

/* Self-contained PSG generation state. Used as the live playback state in
 * timestamped mode (driven only by the event queue, on the audio thread) and
 * as a transient mirror of the authoritative ay3891x_t fields otherwise. */
typedef struct {
    uint8_t  sregs[AY_NUM_REGISTERS];  /* register shadow (mixer/volume/period bytes) */
    uint16_t tone_period[3];
    uint32_t tone_counter[3];
    uint8_t  tone_output[3];
    uint16_t noise_period;
    uint32_t noise_counter;
    uint32_t noise_shift;
    uint8_t  noise_output;
    uint16_t env_period;
    uint32_t env_counter;
    uint8_t  env_shape;
    uint8_t  env_step;
    uint8_t  env_volume;
    bool     env_holding;
} ay_play_t;

typedef struct ay3891x_s {
    uint8_t registers[AY_NUM_REGISTERS];
    uint8_t selected_reg;

    /* Tone generators */
    uint16_t tone_period[3];
    uint32_t tone_counter[3];   /* Fractional accumulator for clock rate conversion */
    uint8_t  tone_output[3];

    /* Noise generator */
    uint16_t noise_period;
    uint32_t noise_counter;     /* Fractional accumulator for clock rate conversion */
    uint32_t noise_shift;
    uint8_t  noise_output;

    /* Envelope */
    uint16_t env_period;
    uint32_t env_counter;       /* Fractional accumulator for clock rate conversion */
    uint8_t  env_shape;
    uint8_t  env_step;
    uint8_t  env_volume;
    bool     env_holding;

    /* Clock */
    uint32_t clock_rate;    /* 1 MHz for ORIC */

    /* Port A external input callback (for keyboard on ORIC) */
    uint8_t (*porta_input)(void* userdata);
    void* userdata;

    /* ── Cycle-accurate timestamped register writes (digidrums) ──
     * The fields above stay authoritative and are updated immediately on every
     * write (CPU thread) so reads, the keyboard matrix, headless emulation and
     * savestates always see current state. When the live emulator uses
     * ay_write_data_timed(), sound-register writes are ALSO queued here with
     * their CPU cycle; ay_generate() (audio thread) replays them onto `play`
     * at the exact intra-buffer sample position, so a volume register hammered
     * at audio rate (4-bit PCM "digidrum") is reproduced faithfully instead of
     * collapsing to one value per ~46 ms buffer. */
    ay_play_t play;                  /* audio-thread playback state */
    ay_event_t evq[AY_EVENT_QUEUE_SIZE];
    _Atomic uint32_t evq_head;       /* producer index (CPU thread) */
    _Atomic uint32_t evq_tail;       /* consumer index (audio thread) */
    _Atomic uint64_t cur_cycle;      /* CPU cycle of the most recent timed write */
    uint64_t last_gen_cycle;         /* CPU cycle at the end of the previous buffer */
    bool     timed_mode;             /* set once ay_write_data_timed() is used */
    bool     play_seeded;            /* `play` has been seeded from authoritative state */
} ay3891x_t;

void ay_init(ay3891x_t* ay, uint32_t clock_rate);
void ay_reset(ay3891x_t* ay);
void ay_write_address(ay3891x_t* ay, uint8_t addr);
void ay_write_data(ay3891x_t* ay, uint8_t data);
void ay_write_data_timed(ay3891x_t* ay, uint8_t data, uint64_t cpu_cycle);
void ay_sound_resync(ay3891x_t* ay);  /* resync shadow/queue after a savestate load */
uint8_t ay_read_data(ay3891x_t* ay);
void ay_generate(ay3891x_t* ay, int16_t* buffer, int num_samples);

/* Audio output (SDL2 or headless stub) */
bool audio_init(ay3891x_t* psg);
void audio_cleanup(void);
void audio_pause(bool pause);

/* Cast server audio forwarding */
typedef struct cast_server_s cast_server_t;
void audio_set_cast_server(cast_server_t* server);

#endif
