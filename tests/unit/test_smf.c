/**
 * @file test_smf.c
 * @brief Standard MIDI File parser unit tests
 * @author bmarty <bmarty@mailo.com>
 * @date 2026-06-23
 * @version 1.0.0
 *
 * Builds tiny SMF byte streams in memory and asserts the parser produces the
 * right time-sorted wire-event list: header decode, variable-length delta
 * times, running status, the tempo map (µs/tick integration), multi-track
 * merge (format 1), SysEx pass-through, and rejection of malformed input.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "io/smf.h"

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name) static void name(void)
#define RUN(name) do { \
    printf("  %-50s", #name); \
    name(); \
    tests_passed++; \
    printf("PASS\n"); \
} while(0)

#define ASSERT_EQ(a, b) do { \
    if ((a) != (b)) { \
        printf("FAIL\n    %s:%d: expected %ld, got %ld\n", __FILE__, __LINE__, (long)(b), (long)(a)); \
        tests_failed++; return; \
    } \
} while(0)

#define ASSERT_TRUE(x) do { \
    if (!(x)) { \
        printf("FAIL\n    %s:%d: expected true\n", __FILE__, __LINE__); \
        tests_failed++; return; \
    } \
} while(0)

/* ── A tiny SMF byte builder ───────────────────────────────────────────── */
static uint8_t buf[512];
static size_t  blen;

static void b_reset(void) { blen = 0; }
static void b_byte(uint8_t v) { buf[blen++] = v; }
static void b_u16(uint16_t v) { b_byte((uint8_t)(v >> 8)); b_byte((uint8_t)v); }
static void b_u32(uint32_t v) {
    b_byte((uint8_t)(v >> 24)); b_byte((uint8_t)(v >> 16));
    b_byte((uint8_t)(v >> 8));  b_byte((uint8_t)v);
}
static void b_str(const char* s) { while (*s) b_byte((uint8_t)*s++); }

/* Header for `ntracks` tracks at `division` ticks/quarter. */
static void b_header(uint16_t format, uint16_t ntracks, uint16_t division) {
    b_str("MThd"); b_u32(6); b_u16(format); b_u16(ntracks); b_u16(division);
}
/* Begin a track; returns the index where the (later patched) length goes. */
static size_t b_track_begin(void) {
    b_str("MTrk"); size_t at = blen; b_u32(0); return at;
}
static void b_track_end(size_t lenpos) {
    uint32_t tlen = (uint32_t)(blen - (lenpos + 4));
    buf[lenpos]   = (uint8_t)(tlen >> 24); buf[lenpos+1] = (uint8_t)(tlen >> 16);
    buf[lenpos+2] = (uint8_t)(tlen >> 8);  buf[lenpos+3] = (uint8_t)tlen;
}

/* ═══════════════════════════════════════════════════════════════════════ */

TEST(test_header_and_basic_events) {
    b_reset();
    b_header(0, 1, 480);
    size_t L = b_track_begin();
    b_byte(0x00); b_byte(0xFF); b_byte(0x51); b_byte(0x03);  /* tempo 500000 */
    b_byte(0x07); b_byte(0xA1); b_byte(0x20);
    b_byte(0x00); b_byte(0x90); b_byte(0x3C); b_byte(0x7F);  /* note on */
    b_byte(0x00); b_byte(0x3C); b_byte(0x40);                /* running status */
    b_byte(0x81); b_byte(0x70);                              /* delta 240 ticks */
    b_byte(0x80); b_byte(0x3C); b_byte(0x00);                /* note off */
    b_byte(0x00); b_byte(0xFF); b_byte(0x2F); b_byte(0x00);  /* end of track */
    b_track_end(L);

    smf_t s;
    ASSERT_TRUE(smf_parse(buf, blen, &s));
    ASSERT_EQ(s.format, 0);
    ASSERT_EQ(s.ntracks, 1);
    ASSERT_EQ((long)s.count, 3);

    /* Event 0: note on at t=0 */
    ASSERT_EQ(s.events[0].t_us, 0u);
    ASSERT_EQ(s.events[0].len, 3);
    ASSERT_EQ(smf_event_bytes(&s, 0)[0], 0x90);
    ASSERT_EQ(smf_event_bytes(&s, 0)[1], 0x3C);
    ASSERT_EQ(smf_event_bytes(&s, 0)[2], 0x7F);

    /* Event 1: running-status note on, reconstructed with status 0x90, t=0 */
    ASSERT_EQ(s.events[1].t_us, 0u);
    ASSERT_EQ(smf_event_bytes(&s, 1)[0], 0x90);
    ASSERT_EQ(smf_event_bytes(&s, 1)[1], 0x3C);
    ASSERT_EQ(smf_event_bytes(&s, 1)[2], 0x40);

    /* Event 2: note off at tick 240 → 240 * (500000/480) = 250000 µs */
    ASSERT_EQ(s.events[2].t_us, 250000u);
    ASSERT_EQ(smf_event_bytes(&s, 2)[0], 0x80);
    smf_free(&s);
}

TEST(test_tempo_changes_affect_timing) {
    b_reset();
    b_header(0, 1, 480);
    size_t L = b_track_begin();
    /* 1000000 µs/qn (60 BPM) → 1000000/480 µs per tick */
    b_byte(0x00); b_byte(0xFF); b_byte(0x51); b_byte(0x03);
    b_byte(0x0F); b_byte(0x42); b_byte(0x40);                /* 0x0F4240 = 1000000 */
    b_byte(0x00); b_byte(0x90); b_byte(0x40); b_byte(0x7F);  /* note on at t=0 */
    b_byte(0x83); b_byte(0x60);                              /* delta 480 ticks */
    b_byte(0x80); b_byte(0x40); b_byte(0x00);                /* note off */
    b_byte(0x00); b_byte(0xFF); b_byte(0x2F); b_byte(0x00);
    b_track_end(L);

    smf_t s;
    ASSERT_TRUE(smf_parse(buf, blen, &s));
    ASSERT_EQ((long)s.count, 2);
    ASSERT_EQ(s.events[0].t_us, 0u);
    /* 480 ticks * 1000000/480 = 1000000 µs (one second at 60 BPM/quarter) */
    ASSERT_EQ(s.events[1].t_us, 1000000u);
    smf_free(&s);
}

TEST(test_format1_track_merge) {
    b_reset();
    b_header(1, 2, 480);
    /* Track 0: tempo only */
    size_t L0 = b_track_begin();
    b_byte(0x00); b_byte(0xFF); b_byte(0x51); b_byte(0x03);
    b_byte(0x07); b_byte(0xA1); b_byte(0x20);                /* 500000 */
    b_byte(0x00); b_byte(0xFF); b_byte(0x2F); b_byte(0x00);
    b_track_end(L0);
    /* Track 1: a note on at tick 0, note off at tick 480 */
    size_t L1 = b_track_begin();
    b_byte(0x00); b_byte(0x90); b_byte(0x3E); b_byte(0x64);
    b_byte(0x83); b_byte(0x60);                              /* delta 480 */
    b_byte(0x80); b_byte(0x3E); b_byte(0x00);
    b_byte(0x00); b_byte(0xFF); b_byte(0x2F); b_byte(0x00);
    b_track_end(L1);

    smf_t s;
    ASSERT_TRUE(smf_parse(buf, blen, &s));
    ASSERT_EQ(s.format, 1);
    ASSERT_EQ((long)s.count, 2);
    ASSERT_EQ(smf_event_bytes(&s, 0)[0], 0x90);
    ASSERT_EQ(s.events[0].t_us, 0u);
    /* 480 ticks * 500000/480 = 500000 µs */
    ASSERT_EQ(s.events[1].t_us, 500000u);
    smf_free(&s);
}

TEST(test_sysex_passthrough) {
    b_reset();
    b_header(0, 1, 480);
    size_t L = b_track_begin();
    /* SysEx: F0 <len=3> 7E 7F 09 (a GM-on-ish blob; F7 terminator counted) */
    b_byte(0x00); b_byte(0xF0); b_byte(0x03); b_byte(0x7E); b_byte(0x7F); b_byte(0xF7);
    b_byte(0x00); b_byte(0xFF); b_byte(0x2F); b_byte(0x00);
    b_track_end(L);

    smf_t s;
    ASSERT_TRUE(smf_parse(buf, blen, &s));
    ASSERT_EQ((long)s.count, 1);
    ASSERT_EQ(s.events[0].len, 4);   /* F0 + 3 data bytes */
    ASSERT_EQ(smf_event_bytes(&s, 0)[0], 0xF0);
    ASSERT_EQ(smf_event_bytes(&s, 0)[3], 0xF7);
    smf_free(&s);
}

TEST(test_reject_garbage) {
    smf_t s;
    uint8_t junk[16] = { 'N','O','P','E', 0,0,0,0,0,0,0,0,0,0,0,0 };
    ASSERT_TRUE(smf_parse(junk, sizeof(junk), &s) == false);
    ASSERT_TRUE(smf_parse((const uint8_t*)"MThd", 4, &s) == false);  /* truncated */
}

TEST(test_load_from_file_roundtrip) {
    b_reset();
    b_header(0, 1, 480);
    size_t L = b_track_begin();
    b_byte(0x00); b_byte(0x90); b_byte(0x24); b_byte(0x7F);
    b_byte(0x00); b_byte(0xFF); b_byte(0x2F); b_byte(0x00);
    b_track_end(L);

    const char* path = "test_smf_tmp.mid";
    FILE* f = fopen(path, "wb");
    ASSERT_TRUE(f != NULL);
    fwrite(buf, 1, blen, f);
    fclose(f);

    smf_t s;
    ASSERT_TRUE(smf_load(path, &s));
    ASSERT_EQ((long)s.count, 1);
    ASSERT_EQ(smf_event_bytes(&s, 0)[1], 0x24);
    smf_free(&s);
    remove(path);
}

int main(void) {
    printf("\n=== Standard MIDI File (SMF) parser Tests ===\n\n");

    RUN(test_header_and_basic_events);
    RUN(test_tempo_changes_affect_timing);
    RUN(test_format1_track_merge);
    RUN(test_sysex_passthrough);
    RUN(test_reject_garbage);
    RUN(test_load_from_file_roundtrip);

    printf("\n=== Results: %d passed, %d failed ===\n\n", tests_passed, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
