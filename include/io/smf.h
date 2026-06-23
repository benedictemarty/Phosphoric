/**
 * @file smf.h
 * @brief Standard MIDI File (SMF / .mid) parser → timed wire-byte event list
 * @author bmarty <bmarty@mailo.com>
 * @date 2026-06-23
 *
 * Parses a Standard MIDI File (format 0 or 1, PPQN or SMPTE division) into a
 * single, time-sorted list of MIDI *wire* events with absolute timestamps in
 * microseconds. Tracks are merged onto one timeline and the tempo map (Set
 * Tempo meta events, from any track) is integrated so the microsecond stamps
 * are musically correct. Running status and SysEx are handled; meta events are
 * consumed for timing (tempo) and otherwise dropped — they never go on the wire.
 *
 * The output is exactly what a MIDI IN jack would carry: it lets the emulator
 * replay a .mid file *into* the Oric (Mageco card) at the right tempo, as if a
 * sequencer were playing the keyboard. The real-time pacing lives in the serial
 * backend (smf: transport); this module is pure, deterministic parsing.
 */

#ifndef SMF_H
#define SMF_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* One deliverable MIDI message (channel voice, channel mode, or SysEx). The
 * bytes point into the owning smf_t's pool and stay valid until smf_free(). */
typedef struct {
    uint32_t t_us;       /**< absolute time from start, microseconds */
    uint16_t len;        /**< number of wire bytes */
    uint32_t off;        /**< offset of the bytes in smf_t::pool */
} smf_event_t;

typedef struct {
    smf_event_t* events; /**< time-sorted deliverable events */
    size_t       count;  /**< number of events */
    uint8_t*     pool;   /**< concatenated wire bytes for all events */
    size_t       pool_len;
    uint16_t     format; /**< SMF format (0, 1, 2) */
    uint16_t     ntracks;/**< track count from the header */
    uint32_t     total_us;/**< timestamp just past the last event */
} smf_t;

/**
 * @brief Parse a Standard MIDI File from a memory buffer.
 * @param data  File bytes
 * @param size  File length
 * @param out   Receives the parsed song (zeroed on failure)
 * @return true on success; false if the data is not a valid SMF
 */
bool smf_parse(const uint8_t* data, size_t size, smf_t* out);

/**
 * @brief Parse a Standard MIDI File from disk.
 * @return true on success; false if the file cannot be read or is invalid
 */
bool smf_load(const char* path, smf_t* out);

/** @brief Pointer to event @p i's wire bytes (into the pool). */
static inline const uint8_t* smf_event_bytes(const smf_t* s, size_t i)
{
    return s->pool + s->events[i].off;
}

/** @brief Free all memory owned by @p s (safe on a zeroed/failed smf_t). */
void smf_free(smf_t* s);

#endif /* SMF_H */
