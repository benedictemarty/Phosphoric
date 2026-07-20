/**
 * @file autotype.h
 * @brief Scan-driven pacing primitives for the --type-keys auto-typer.
 *
 * Historically --type-keys paced keystroke transitions purely on the CPU
 * cycle counter (a fixed number of emulated frames per press/release). That
 * is fragile: a program that scans the keyboard matrix *slower* than the
 * injector advances drops keys, because a key can be pressed AND released
 * inside the program's scan blind-window (the ROM/game never observes the
 * edge).
 *
 * The reliable fix is to gate every keystroke state-transition on the real
 * keyboard scanner: the ORIC keyboard is read through VIA Port B (PB3),
 * sweeping the 8 columns 7->0 each pass. We count a scan *pass* every time
 * the selected column climbs back up (the descending sweep restarts). The
 * auto-typer then refuses to change the matrix until the scanner has
 * completed at least AUTOTYPE_MIN_PASS passes since the previous transition,
 * guaranteeing the program actually observed the current matrix state.
 *
 * This is *additive* to the existing cycle schedule (never faster than
 * before), with a cycle watchdog so a program that never scans the keyboard
 * cannot hang the injector.
 */
#ifndef AUTOTYPE_H
#define AUTOTYPE_H

#include <stdbool.h>
#include <stdint.h>

/** Minimum scan passes the keyboard scanner must complete between two
 *  auto-type state transitions, so the program is guaranteed to have read
 *  the current matrix state at least once (2 = one clean read plus margin). */
#define AUTOTYPE_MIN_PASS 2

/** Cycle watchdog margin past the (already-elapsed) cycle schedule after
 *  which the pass requirement is waived — prevents a stall when the target
 *  program never scans the keyboard at all. 30 PAL frames. */
#define AUTOTYPE_WATCHDOG_CYCLES (30 * 19968)

/**
 * @brief Detect a completed keyboard scan pass from consecutive column reads.
 *
 * The ORIC ROM scanner sweeps columns in descending order (7,6,...,0). A new
 * pass therefore begins whenever the freshly selected column is *higher* than
 * the previous one (the sweep wrapped around). This is robust even when the
 * scanner stops early on a detected key: the next sweep still restarts at a
 * higher column.
 *
 * @param prev_col Previously observed column (0-7), or 0xFF if none yet.
 * @param col      Currently selected column (0-7).
 * @return true if this read starts a new scan pass.
 */
static inline bool autotype_is_new_pass(uint8_t prev_col, uint8_t col) {
    return prev_col != 0xFF && col > prev_col;
}

/**
 * @brief Decide whether the auto-typer may perform its next state transition.
 *
 * @param loci_hid    true when injecting via the LOCI HID path (no ORIC
 *                    matrix scanner involved) — then pacing is purely
 *                    cycle-based and the pass gate is bypassed.
 * @param cycles      Current CPU cycle counter.
 * @param next_cycle  Earliest cycle the next transition may occur (existing
 *                    cycle schedule; unchanged semantics).
 * @param scan_passes Monotonic scan-pass counter.
 * @param last_pass   scan_passes value recorded at the previous transition.
 * @return true if the transition may fire now.
 */
static inline bool autotype_should_fire(bool loci_hid,
                                        int64_t cycles, int64_t next_cycle,
                                        uint32_t scan_passes,
                                        uint32_t last_pass) {
    if (cycles < next_cycle)
        return false;
    if (loci_hid)
        return true;
    if (scan_passes >= last_pass + AUTOTYPE_MIN_PASS)
        return true;
    /* Watchdog: program isn't scanning the keyboard — don't hang forever. */
    return cycles >= next_cycle + AUTOTYPE_WATCHDOG_CYCLES;
}

#endif /* AUTOTYPE_H */
