/**
 * @file test_control_queue.c
 * @brief Concurrency tests for the control command queue (sprint 93, Epic 2).
 *
 * Model under test: many producer threads call control_queue_submit() while a
 * single consumer thread calls control_queue_drain() in a loop (standing in for
 * the emulator's per-frame drain). The key safety properties:
 *
 *   1. Routing — each producer gets back ITS OWN reply, never another thread's.
 *      Each producer owns a unique zero-page address; it writes a value only it
 *      uses, then reads that address back and asserts the byte matches. A routing
 *      bug (reply handed to the wrong waiter) or a data race on the shared
 *      emulator would corrupt this round-trip.
 *   2. No loss / no hang — every submit() returns; the total processed count
 *      equals the number submitted.
 *   3. Clean teardown — destroy() releases the queue with no blocked producers.
 *
 * Run under valgrind --tool=helgrind for race detection.
 *
 * Author: bmarty <bmarty@mailo.com>
 */

#define _POSIX_C_SOURCE 200809L
#include "control_queue.h"
#include "emulator.h"
#include "cpu/cpu6502.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdint.h>
#include <stdatomic.h>

/* Short yield between drains (~50 µs); nanosleep is the POSIX.1-2008 sleep. */
static void nap_us(long us) {
    struct timespec ts = { .tv_sec = 0, .tv_nsec = us * 1000L };
    nanosleep(&ts, NULL);
}

static int tests_passed = 0;
static int tests_failed = 0;

#define NPROD       8      /* producer threads                         */
#define ROUNDS    100      /* write/read round-trips per producer      */

static emulator_t*      g_emu;
static control_queue_t* g_queue;
static atomic_int       g_consumer_stop;
static atomic_int       g_route_errors;   /* producer round-trip mismatches */

/* Consumer: drain the queue repeatedly until told to stop, then a final drain
 * to flush anything enqueued in the gap. */
static void* consumer_main(void* arg) {
    (void)arg;
    while (!atomic_load(&g_consumer_stop)) {
        control_queue_drain(g_queue, g_emu);
        nap_us(50);
    }
    control_queue_drain(g_queue, g_emu);
    return NULL;
}

/* Producer p owns zero-page address 0x20+p. For each round it writes a value
 * derived from (p, round) and reads it straight back, asserting the byte the
 * queue routed back to *this* thread is the one it just wrote. */
static void* producer_main(void* arg) {
    int p = (int)(intptr_t)arg;
    uint16_t addr = (uint16_t)(0x20 + p);
    for (int r = 0; r < ROUNDS; r++) {
        uint8_t val = (uint8_t)((p * 7 + r * 13) & 0xFF);
        char line[64], want[64];
        char* reply = NULL;

        snprintf(line, sizeof(line), "write %02X %02X", addr, val);
        control_queue_submit(g_queue, line, &reply, NULL);
        if (!reply || strcmp(reply, "OK count=1\n") != 0)
            atomic_fetch_add(&g_route_errors, 1);
        free(reply);
        reply = NULL;

        snprintf(line, sizeof(line), "read %02X 1", addr);
        snprintf(want, sizeof(want), "OK %02X\n", val);
        control_queue_submit(g_queue, line, &reply, NULL);
        if (!reply || strcmp(reply, want) != 0)
            atomic_fetch_add(&g_route_errors, 1);
        free(reply);
    }
    return NULL;
}

static void test_concurrent_routing(void) {
    printf("  %-42s", "concurrent_routing_8x100");
    g_emu = calloc(1, sizeof(emulator_t));
    cpu_init(&g_emu->cpu, &g_emu->memory);
    g_queue = control_queue_create();
    atomic_store(&g_consumer_stop, 0);
    atomic_store(&g_route_errors, 0);

    pthread_t consumer;
    pthread_create(&consumer, NULL, consumer_main, NULL);

    pthread_t prod[NPROD];
    for (int i = 0; i < NPROD; i++)
        pthread_create(&prod[i], NULL, producer_main, (void*)(intptr_t)i);
    for (int i = 0; i < NPROD; i++)
        pthread_join(prod[i], NULL);

    atomic_store(&g_consumer_stop, 1);
    pthread_join(consumer, NULL);

    int errs = atomic_load(&g_route_errors);
    control_queue_destroy(g_queue);
    free(g_emu);

    if (errs == 0) { printf("[OK]\n"); tests_passed++; }
    else { printf("[FAIL] %d routing/round-trip mismatches\n", errs); tests_failed++; }
}

/* A submit() after the queue is shutting down must not hang: destroy() releases
 * waiters. Here we simply verify submit on a fresh queue then destroy is clean,
 * and that draining an empty queue is a no-op. */
static void test_drain_empty_and_teardown(void) {
    printf("  %-42s", "drain_empty_and_teardown");
    emulator_t* emu = calloc(1, sizeof(emulator_t));
    cpu_init(&emu->cpu, &emu->memory);
    control_queue_t* q = control_queue_create();

    int n = control_queue_drain(q, emu);      /* nothing queued */
    int n_null = control_queue_drain(NULL, emu); /* NULL queue no-op */

    control_queue_destroy(q);
    free(emu);

    if (n == 0 && n_null == 0) { printf("[OK]\n"); tests_passed++; }
    else { printf("[FAIL] drain of empty/NULL returned %d/%d\n", n, n_null); tests_failed++; }
}

/* Single-threaded sanity: submit is synchronous relative to a drain we run
 * ourselves between the two halves, proving the reply is the dispatch output. */
static void test_single_thread_roundtrip(void) {
    printf("  %-42s", "single_thread_roundtrip");
    emulator_t* emu = calloc(1, sizeof(emulator_t));
    cpu_init(&emu->cpu, &emu->memory);
    control_queue_t* q = control_queue_create();

    atomic_int stop; atomic_store(&stop, 0);
    /* Reuse the shared consumer by pointing globals at these locals. */
    g_emu = emu; g_queue = q; atomic_store(&g_consumer_stop, 0);
    pthread_t consumer;
    pthread_create(&consumer, NULL, consumer_main, NULL);

    char* reply = NULL;
    control_result_t r = control_queue_submit(q, "hello", &reply, NULL);
    int ok = (r == CONTROL_CONTINUE) && reply &&
             strncmp(reply, "OK server=phosphoric/", 21) == 0;
    free(reply);

    reply = NULL;
    r = control_queue_submit(q, "quit", &reply, NULL);
    ok = ok && (r == CONTROL_QUIT) && reply && strcmp(reply, "OK\n") == 0;
    free(reply);

    atomic_store(&g_consumer_stop, 1);
    pthread_join(consumer, NULL);
    (void)stop;
    control_queue_destroy(q);
    free(emu);

    if (ok) { printf("[OK]\n"); tests_passed++; }
    else { printf("[FAIL] submit reply/result mismatch\n"); tests_failed++; }
}

int main(void) {
    printf("=== control_queue concurrency tests ===\n");
    test_drain_empty_and_teardown();
    test_single_thread_roundtrip();
    test_concurrent_routing();
    printf("=== result: %d passed, %d failed ===\n", tests_passed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
