/**
 * @file test_picowifi.c
 * @brief PicoWiFiModemUSB backend unit tests
 * @author bmarty <bmarty@mailo.com>
 * @date 2026-06-13
 *
 * Drives the picowifi backend through its serial vtable (send AT chars,
 * drain the response from the RX ring) and asserts the v0.1.0 AT command
 * behaviour: WiFi config, result-code formatting, S-registers, speed dial,
 * telnet, info, factory reset, and error handling. No real network is used
 * — only the hermetic command-parsing paths are exercised.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "io/serial_backend.h"

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name) static void name(void)
#define RUN(name) do { \
    printf("  %-52s", #name); \
    name(); \
    tests_passed++; \
    printf("PASS\n"); \
} while(0)

#define ASSERT_TRUE(x) do { \
    if (!(x)) { \
        printf("FAIL\n    %s:%d: expected true\n", __FILE__, __LINE__); \
        tests_failed++; return; \
    } \
} while(0)

#define ASSERT_FALSE(x) do { \
    if ((x)) { \
        printf("FAIL\n    %s:%d: expected false\n", __FILE__, __LINE__); \
        tests_failed++; return; \
    } \
} while(0)

#define ASSERT_CONTAINS(hay, needle) do { \
    if (strstr((hay), (needle)) == NULL) { \
        printf("FAIL\n    %s:%d: \"%s\" not found in:\n      <<<%s>>>\n", \
               __FILE__, __LINE__, (needle), (hay)); \
        tests_failed++; return; \
    } \
} while(0)

#define ASSERT_NOT_CONTAINS(hay, needle) do { \
    if (strstr((hay), (needle)) != NULL) { \
        printf("FAIL\n    %s:%d: \"%s\" unexpectedly found in:\n      <<<%s>>>\n", \
               __FILE__, __LINE__, (needle), (hay)); \
        tests_failed++; return; \
    } \
} while(0)

/* ═══════════════════════════════════════════════════════════════════════
 *  Harness
 * ═══════════════════════════════════════════════════════════════════════ */

static serial_backend_t* pw;

static void pw_setup(const char* ssid, const char* pass) {
    pw = serial_backend_picowifi_create(ssid, pass);
    pw->open(pw);
}

static void pw_teardown(void) {
    /* Mirror serial_backend_destroy() without linking serial_backend.c
     * (which pulls in the ACIA/digitelec deps): close frees the impl. */
    pw->close(pw);
    free(pw);
    pw = NULL;
}

/* Send a command line (CR appended), drain the response into out. */
static void pw_cmd(const char* line, char* out, size_t outsz) {
    for (const char* p = line; *p; p++) pw->send(pw, (uint8_t)*p);
    pw->send(pw, '\r');
    size_t i = 0;
    uint8_t b;
    while (i < outsz - 1 && pw->recv(pw, &b)) out[i++] = (char)b;
    out[i] = '\0';
}

/* ═══════════════════════════════════════════════════════════════════════
 *  Tests
 * ═══════════════════════════════════════════════════════════════════════ */

TEST(test_bare_at_ok) {
    pw_setup(NULL, NULL);
    char r[512];
    pw_cmd("AT", r, sizeof(r));
    ASSERT_CONTAINS(r, "OK");
    pw_teardown();
}

TEST(test_ssid_set_and_query) {
    pw_setup(NULL, NULL);
    char r[512];
    pw_cmd("AT$SSID=MyNetwork", r, sizeof(r));
    ASSERT_CONTAINS(r, "OK");
    pw_cmd("AT$SSID?", r, sizeof(r));
    ASSERT_CONTAINS(r, "MyNetwork");
    pw_teardown();
}

TEST(test_ssid_preset_from_factory) {
    pw_setup("HomeWiFi", "secret");
    char r[512];
    pw_cmd("AT$SSID?", r, sizeof(r));
    ASSERT_CONTAINS(r, "HomeWiFi");
    pw_teardown();
}

TEST(test_password_query_shows_value) {
    /* Sprint 46: firmware shows the actual password (no masking). */
    pw_setup(NULL, NULL);
    char r[512];
    pw_cmd("AT$PASS=hunter2", r, sizeof(r));
    ASSERT_CONTAINS(r, "OK");
    pw_cmd("AT$PASS?", r, sizeof(r));
    ASSERT_CONTAINS(r, "hunter2");
    pw_teardown();
}

TEST(test_mdns_default_picomodem) {
    /* Sprint 46: firmware factory default is "picomodem", not "espmodem". */
    pw_setup(NULL, NULL);
    char r[512];
    pw_cmd("AT$MDNS?", r, sizeof(r));
    ASSERT_CONTAINS(r, "picomodem");
    pw_teardown();
}

TEST(test_baud_default_9600) {
    /* Sprint 46: firmware DEFAULT_SPEED is 9600. */
    pw_setup(NULL, NULL);
    char r[512];
    pw_cmd("AT$SB?", r, sizeof(r));
    ASSERT_CONTAINS(r, "9600");
    pw_teardown();
}

TEST(test_location_default_computer_room) {
    pw_setup(NULL, NULL);
    char r[512];
    pw_cmd("AT$TTL?", r, sizeof(r));
    ASSERT_CONTAINS(r, "Computer Room");
    pw_teardown();
}

TEST(test_predefined_speed_dials) {
    /* Sprint 46: 3 factory speed dials (particles/altair/heatwave). */
    pw_setup(NULL, NULL);
    char r[512];
    pw_cmd("AT&Z0?", r, sizeof(r));
    ASSERT_CONTAINS(r, "particles");
    pw_cmd("AT&Z1?", r, sizeof(r));
    ASSERT_CONTAINS(r, "altair");
    pw_cmd("AT&Z2?", r, sizeof(r));
    ASSERT_CONTAINS(r, "heatwave");
    pw_teardown();
}

TEST(test_compound_command_line) {
    /* Sprint 46: "ATS0=7 NET0" applies BOTH settings, emits ONE OK. */
    pw_setup(NULL, NULL);
    char r[512];
    pw_cmd("ATS0=7 NET0", r, sizeof(r));
    ASSERT_CONTAINS(r, "OK");
    pw_cmd("ATS0?", r, sizeof(r));
    ASSERT_CONTAINS(r, "7");
    pw_cmd("ATNET?", r, sizeof(r));
    ASSERT_CONTAINS(r, "0");
    pw_teardown();
}

TEST(test_compound_unknown_token_errors) {
    pw_setup(NULL, NULL);
    char r[512];
    pw_cmd("ATE0 WXYZ Q0", r, sizeof(r));   /* WXYZ unknown → whole line ERROR */
    ASSERT_CONTAINS(r, "ERROR");
    pw_teardown();
}

TEST(test_numeric_result_codes_atv0) {
    pw_setup(NULL, NULL);
    char r[512];
    pw_cmd("ATV0", r, sizeof(r));   /* switch to numeric */
    pw_cmd("AT", r, sizeof(r));     /* should answer "0" not "OK" */
    ASSERT_CONTAINS(r, "0");
    ASSERT_NOT_CONTAINS(r, "OK");
    pw_teardown();
}

TEST(test_quiet_mode_atq1) {
    pw_setup(NULL, NULL);
    char r[512];
    pw_cmd("ATE0", r, sizeof(r));   /* no echo to keep response clean */
    pw_cmd("ATQ1", r, sizeof(r));   /* enable quiet */
    pw_cmd("AT", r, sizeof(r));     /* no result code at all */
    ASSERT_NOT_CONTAINS(r, "OK");
    ASSERT_NOT_CONTAINS(r, "0");
    pw_teardown();
}

TEST(test_echo_off_ate0) {
    pw_setup(NULL, NULL);
    char r[512];
    pw_cmd("ATE0", r, sizeof(r));
    pw_cmd("AT$MDNS?", r, sizeof(r));
    /* With echo off the command itself must not be echoed back */
    ASSERT_NOT_CONTAINS(r, "MDNS");
    ASSERT_CONTAINS(r, "picomodem");
    pw_teardown();
}

TEST(test_s0_register) {
    pw_setup(NULL, NULL);
    char r[512];
    pw_cmd("ATS0=3", r, sizeof(r));
    ASSERT_CONTAINS(r, "OK");
    pw_cmd("ATS0?", r, sizeof(r));
    ASSERT_CONTAINS(r, "3");
    pw_teardown();
}

TEST(test_s2_escape_char) {
    pw_setup(NULL, NULL);
    char r[512];
    pw_cmd("ATS2?", r, sizeof(r));
    ASSERT_CONTAINS(r, "43");   /* default '+' */
    pw_teardown();
}

TEST(test_telnet_mode) {
    pw_setup(NULL, NULL);
    char r[512];
    pw_cmd("ATNET0", r, sizeof(r));
    ASSERT_CONTAINS(r, "OK");
    pw_cmd("ATNET?", r, sizeof(r));
    ASSERT_CONTAINS(r, "0");
    pw_teardown();
}

TEST(test_terminal_size) {
    pw_setup(NULL, NULL);
    char r[512];
    pw_cmd("AT$TTS=132x43", r, sizeof(r));
    ASSERT_CONTAINS(r, "OK");
    pw_cmd("AT$TTS?", r, sizeof(r));
    ASSERT_CONTAINS(r, "132x43");
    pw_teardown();
}

TEST(test_speed_dial_store_and_query) {
    pw_setup(NULL, NULL);
    char r[512];
    pw_cmd("AT&Z0=bbs.example.com:6502,myBBS", r, sizeof(r));
    ASSERT_CONTAINS(r, "OK");
    pw_cmd("AT&Z0?", r, sizeof(r));
    ASSERT_CONTAINS(r, "bbs.example.com:6502");
    ASSERT_CONTAINS(r, "myBBS");
    pw_teardown();
}

TEST(test_dial_speed_slot_empty) {
    pw_setup("Net", NULL);
    char r[512];
    pw_cmd("ATDS5", r, sizeof(r));   /* empty slot → ERROR (firmware) */
    ASSERT_CONTAINS(r, "ERROR");
    pw_teardown();
}

TEST(test_dial_no_ssid_no_carrier) {
    pw_setup(NULL, NULL);            /* no WiFi configured */
    char r[512];
    pw_cmd("ATDT example.com:23", r, sizeof(r));
    ASSERT_CONTAINS(r, "NO CARRIER");
    pw_teardown();
}

TEST(test_wifi_connect_status) {
    /* Sprint 46: ATC? returns numeric 0/1 (firmware), not text. */
    pw_setup("MyNet", "pw");
    char r[512];
    pw_cmd("ATC1", r, sizeof(r));    /* associate */
    ASSERT_CONTAINS(r, "OK");
    pw_cmd("ATC?", r, sizeof(r));
    ASSERT_CONTAINS(r, "1");
    ASSERT_NOT_CONTAINS(r, "CONNECTED");
    pw_teardown();
}

TEST(test_wifi_connect_without_ssid_errors) {
    pw_setup(NULL, NULL);
    char r[512];
    pw_cmd("ATC1", r, sizeof(r));
    ASSERT_CONTAINS(r, "ERROR");
    pw_teardown();
}

TEST(test_info_command) {
    pw_setup("Net", NULL);
    char r[512];
    pw_cmd("ATI", r, sizeof(r));
    ASSERT_CONTAINS(r, "PicoWiFiModemUSB");
    ASSERT_CONTAINS(r, "0.1.0");
    pw_teardown();
}

TEST(test_unknown_command_error) {
    pw_setup(NULL, NULL);
    char r[512];
    pw_cmd("ATWXYZ", r, sizeof(r));
    ASSERT_CONTAINS(r, "ERROR");
    pw_teardown();
}

TEST(test_missing_at_prefix_error) {
    pw_setup(NULL, NULL);
    char r[512];
    pw_cmd("HELLO", r, sizeof(r));
    ASSERT_CONTAINS(r, "ERROR");
    pw_teardown();
}

TEST(test_factory_reset_clears_ssid) {
    pw_setup("ToErase", "pw");
    char r[512];
    pw_cmd("AT&F", r, sizeof(r));
    ASSERT_CONTAINS(r, "OK");
    pw_cmd("AT$SSID?", r, sizeof(r));
    ASSERT_NOT_CONTAINS(r, "ToErase");
    pw_teardown();
}

TEST(test_save_profile_atw_ok) {
    pw_setup(NULL, NULL);
    char r[512];
    pw_cmd("AT&W", r, sizeof(r));    /* save: no-op but must answer OK */
    ASSERT_CONTAINS(r, "OK");
    pw_teardown();
}

TEST(test_view_profile_atv_amp) {
    pw_setup("ViewNet", NULL);
    char r[512];
    pw_cmd("AT&V", r, sizeof(r));
    ASSERT_CONTAINS(r, "ViewNet");
    ASSERT_CONTAINS(r, "OK");
    pw_teardown();
}

TEST(test_repeat_last_command) {
    pw_setup(NULL, NULL);
    char r[512];
    pw_cmd("AT$MDNS?", r, sizeof(r));
    pw_cmd("A/", r, sizeof(r));      /* repeat → picomodem again */
    ASSERT_CONTAINS(r, "picomodem");
    pw_teardown();
}

TEST(test_atz_is_noop) {
    /* Sprint 46: firmware ATZ does NOT reset (would drop USB) — just OK.
     * So a prior ATV0 (numeric) stays in effect after ATZ. */
    pw_setup(NULL, NULL);
    char r[512];
    pw_cmd("ATV0", r, sizeof(r));    /* numeric result codes */
    pw_cmd("ATZ", r, sizeof(r));     /* no-op */
    pw_cmd("AT", r, sizeof(r));
    ASSERT_CONTAINS(r, "0");         /* still numeric, ATZ did not restore V1 */
    ASSERT_NOT_CONTAINS(r, "OK");
    pw_teardown();
}

TEST(test_baud_set_and_query) {
    pw_setup(NULL, NULL);
    char r[512];
    pw_cmd("AT$SB=9600", r, sizeof(r));
    ASSERT_CONTAINS(r, "OK");
    pw_cmd("AT$SB?", r, sizeof(r));
    ASSERT_CONTAINS(r, "9600");
    pw_teardown();
}

TEST(test_flow_control_amp_k) {
    pw_setup(NULL, NULL);
    char r[512];
    pw_cmd("AT&K1", r, sizeof(r));
    ASSERT_CONTAINS(r, "OK");
    pw_cmd("AT&K?", r, sizeof(r));
    ASSERT_CONTAINS(r, "1");
    pw_teardown();
}

TEST(test_backend_type_tag) {
    pw_setup(NULL, NULL);
    ASSERT_TRUE(pw->type == SERIAL_BACKEND_PICOWIFI);
    ASSERT_FALSE(pw->connected(pw));  /* idle, no data connection */
    pw_teardown();
}

/* ═══════════════════════════════════════════════════════════════════════
 *  Runner
 * ═══════════════════════════════════════════════════════════════════════ */

int main(void) {
    printf("\n");
    printf("═══════════════════════════════════════════════════════\n");
    printf("  PicoWiFiModemUSB Backend Tests\n");
    printf("═══════════════════════════════════════════════════════\n\n");

    RUN(test_bare_at_ok);
    RUN(test_ssid_set_and_query);
    RUN(test_ssid_preset_from_factory);
    RUN(test_password_query_shows_value);
    RUN(test_mdns_default_picomodem);
    RUN(test_baud_default_9600);
    RUN(test_location_default_computer_room);
    RUN(test_predefined_speed_dials);
    RUN(test_compound_command_line);
    RUN(test_compound_unknown_token_errors);
    RUN(test_numeric_result_codes_atv0);
    RUN(test_quiet_mode_atq1);
    RUN(test_echo_off_ate0);
    RUN(test_s0_register);
    RUN(test_s2_escape_char);
    RUN(test_telnet_mode);
    RUN(test_terminal_size);
    RUN(test_speed_dial_store_and_query);
    RUN(test_dial_speed_slot_empty);
    RUN(test_dial_no_ssid_no_carrier);
    RUN(test_wifi_connect_status);
    RUN(test_wifi_connect_without_ssid_errors);
    RUN(test_info_command);
    RUN(test_unknown_command_error);
    RUN(test_missing_at_prefix_error);
    RUN(test_factory_reset_clears_ssid);
    RUN(test_save_profile_atw_ok);
    RUN(test_view_profile_atv_amp);
    RUN(test_repeat_last_command);
    RUN(test_atz_is_noop);
    RUN(test_baud_set_and_query);
    RUN(test_flow_control_amp_k);
    RUN(test_backend_type_tag);

    printf("\n═══════════════════════════════════════════════════════\n");
    printf("  Results: %d passed, %d failed\n", tests_passed, tests_failed);
    printf("═══════════════════════════════════════════════════════\n\n");

    return tests_failed > 0 ? 1 : 0;
}
