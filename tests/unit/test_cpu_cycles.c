/* SPDX-License-Identifier: EUPL-1.2 */
/**
 * @file test_cpu_cycles.c
 * @brief Cycle-by-cycle conformance oracle for the 6502 (V2-S1)
 * @author bmarty <bmarty@mailo.com>
 *
 * Replays the **SingleStepTests/65x02** vectors (10,000 cases per opcode, each
 * giving the initial state, the final state AND the list of expected bus
 * accesses, cycle by cycle) against the Phosphoric core, and publishes a score.
 *
 * This is the V2 measuring instrument (docs/specs/V2_CYCLE_ACCURACY.md): it
 * turns "I believe the timing is right" into a number. Four properties are
 * measured separately, from the weakest to the strongest:
 *
 *   1. final state     — PC/S/A/X/Y/P and final RAM conform
 *   2. cycle total     — the instruction's cycle count is exact
 *   3. subsequence     — our bus accesses are, IN ORDER, a subset of the
 *                        expected cycles: no spurious access, no inversion
 *                        (this is the property of the current **N2** level)
 *   4. exact sequence  — our bus accesses are EXACTLY the expected cycles, one
 *                        per cycle (this is the property of the targeted **N3** level)
 *
 * Score 4 is **low today**: the core does not emit the dummy accesses and
 * pads the internal cycles at the end of the instruction (docs/ACCURACY.md).
 * This is not a test failure, it is the baseline that V2 must raise —
 * hence the assertion "never below the floor" rather than "equal to 100 %".
 *
 * The vectors are not versioned (~1 GB): `tools/fetch_vectors.sh`.
 * Without them the suite goes to SKIP and only tests its own parser.
 *
 * Environment variables:
 *   CYCLE_VECTORS_DIR  directory of the .json files (default third_party/vectors/65x02)
 *   CYCLE_MAX_CASES    cases per opcode, 0 = all   (default 200)
 *   CYCLE_OPCODES      list "a9,b1,9d" to test     (default: all files present)
 *   CYCLE_VERBOSE      1 = per-opcode detail + first deviations
 *   CYCLE_ENGINE       "legacy" (default) or "microseq" — which core to judge
 */

#include "cpu/cpu6502.h"
#include "cpu/microseq.h"
#include "memory/memory.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <dirent.h>

/* ─── Non-regression floor: the exact bus sequence must never
 * drop back below this rate (in basis points, per ten thousand).
 *
 * V2-S1 reference, exhaustive run (244 non-JAM opcodes × 10,000 cases =
 * 2,440,000 cases): **44.26 %**. The rate is stable within ±0.1 % from 100 cases
 * per opcode on, so this floor also holds for sampled runs.
 * The other three properties are at 100.00 % — see docs/ACCURACY.md.
 * V2-E1 (micro-sequenced core + dummy accesses) must raise this figure
 * towards 10000; raise the floor at each milestone reached. */
#define BUS_EXACT_FLOOR_BP  4400

/* ─── Minimal test framework (project convention) ─── */
static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name) static void name(void)
#define RUN(name) do { printf("  Running %s...\n", #name); name(); } while (0)
#define ASSERT_TRUE(cond) do { \
    if (cond) { tests_passed++; printf("    PASS: %s\n", #cond); } \
    else { tests_failed++; printf("    FAIL: %s (%s:%d)\n", #cond, __FILE__, __LINE__); } \
} while (0)
#define ASSERT_EQ(a, b) do { \
    long long _a = (long long)(a), _b = (long long)(b); \
    if (_a == _b) { tests_passed++; printf("    PASS: %s == %s\n", #a, #b); } \
    else { tests_failed++; printf("    FAIL: %s == %s (%lld != %lld) (%s:%d)\n", \
           #a, #b, _a, _b, __FILE__, __LINE__); } \
} while (0)

/* ════════════════════════════════════════════════════════════════════
 *  Minimal JSON parser, tailored to the vectors' schema
 *
 *  [ { "name": str,
 *      "initial": { "pc":n, "s":n, "a":n, "x":n, "y":n, "p":n, "ram":[[a,v],…] },
 *      "final":   { … same … },
 *      "cycles":  [ [addr, data, "read"|"write"], … ] }, … ]
 *
 *  Deliberately lenient on whitespace and key order, strict on the
 *  shape: any deviation returns an error rather than a silent result.
 * ══════════════════════════════════════════════════════════════════ */

#define MAX_RAM   24
#define MAX_CYC   12

typedef struct {
    uint16_t pc;
    uint8_t  s, a, x, y, p;
    int      nram;
    uint16_t ram_a[MAX_RAM];
    uint8_t  ram_v[MAX_RAM];
} vstate_t;

typedef struct {
    uint16_t addr;
    uint8_t  val;
    bool     write;
} vbus_t;

typedef struct {
    char     name[40];
    vstate_t initial, final;
    int      ncyc;
    vbus_t   cyc[MAX_CYC];
} vcase_t;

typedef struct {
    const char* p;      /* cursor */
    const char* end;
    const char* err;    /* non-NULL from the first error on */
} jparse_t;

static void jskip(jparse_t* j) {
    while (j->p < j->end && (*j->p == ' ' || *j->p == '\t' ||
                             *j->p == '\n' || *j->p == '\r' || *j->p == ','))
        j->p++;
}

static bool jeat(jparse_t* j, char c) {
    jskip(j);
    if (j->p < j->end && *j->p == c) { j->p++; return true; }
    return false;
}

static void jexpect(jparse_t* j, char c) {
    if (!jeat(j, c) && !j->err) j->err = "caractère attendu absent";
}

static long jnum(jparse_t* j) {
    jskip(j);
    const char* s = j->p;
    long v = 0;
    bool neg = false;
    if (j->p < j->end && *j->p == '-') { neg = true; j->p++; }
    while (j->p < j->end && *j->p >= '0' && *j->p <= '9') { v = v * 10 + (*j->p - '0'); j->p++; }
    if (j->p == s && !j->err) j->err = "nombre attendu";
    return neg ? -v : v;
}

/* Copies a JSON string into out (truncated), without handling escapes:
 * the schema contains none. */
static void jstr(jparse_t* j, char* out, size_t out_sz) {
    jskip(j);
    jexpect(j, '"');
    size_t n = 0;
    while (j->p < j->end && *j->p != '"') {
        if (out && n + 1 < out_sz) out[n++] = *j->p;
        j->p++;
    }
    if (out && out_sz) out[n < out_sz ? n : out_sz - 1] = '\0';
    jexpect(j, '"');
}

/* Reads a key "xxx": and returns true if it equals `key`. Consumes nothing on
 * a mismatch: the caller compares in whatever order it wants. */
static bool jkey_is(jparse_t* j, const char* key) {
    jskip(j);
    const char* save = j->p;
    char buf[24];
    jstr(j, buf, sizeof(buf));
    if (!j->err && strcmp(buf, key) == 0 && jeat(j, ':')) return true;
    j->p = save;
    return false;
}

static void jparse_state(jparse_t* j, vstate_t* st) {
    memset(st, 0, sizeof(*st));
    jexpect(j, '{');
    while (!j->err) {
        jskip(j);
        if (jeat(j, '}')) break;
        if (j->p >= j->end) { j->err = "état non terminé"; break; }
        if      (jkey_is(j, "pc")) st->pc = (uint16_t)jnum(j);
        else if (jkey_is(j, "s"))  st->s  = (uint8_t)jnum(j);
        else if (jkey_is(j, "a"))  st->a  = (uint8_t)jnum(j);
        else if (jkey_is(j, "x"))  st->x  = (uint8_t)jnum(j);
        else if (jkey_is(j, "y"))  st->y  = (uint8_t)jnum(j);
        else if (jkey_is(j, "p"))  st->p  = (uint8_t)jnum(j);
        else if (jkey_is(j, "ram")) {
            jexpect(j, '[');
            while (!j->err) {
                jskip(j);
                if (jeat(j, ']')) break;
                jexpect(j, '[');
                long a = jnum(j);
                long v = jnum(j);
                jexpect(j, ']');
                if (st->nram < MAX_RAM) {
                    st->ram_a[st->nram] = (uint16_t)a;
                    st->ram_v[st->nram] = (uint8_t)v;
                    st->nram++;
                } else if (!j->err) {
                    j->err = "trop d'entrées RAM";
                }
            }
        } else {
            j->err = "clé d'état inconnue";
        }
    }
}

static bool jparse_case(jparse_t* j, vcase_t* tc) {
    jskip(j);
    if (j->p >= j->end) return false;
    if (!jeat(j, '{')) return false;
    memset(tc, 0, sizeof(*tc));
    while (!j->err) {
        jskip(j);
        if (jeat(j, '}')) break;
        if (j->p >= j->end) { j->err = "cas non terminé"; break; }
        if      (jkey_is(j, "name"))    jstr(j, tc->name, sizeof(tc->name));
        else if (jkey_is(j, "initial")) jparse_state(j, &tc->initial);
        else if (jkey_is(j, "final"))   jparse_state(j, &tc->final);
        else if (jkey_is(j, "cycles")) {
            jexpect(j, '[');
            while (!j->err) {
                jskip(j);
                if (jeat(j, ']')) break;
                jexpect(j, '[');
                long a = jnum(j);
                long v = jnum(j);
                char kind[12];
                jstr(j, kind, sizeof(kind));
                jexpect(j, ']');
                if (tc->ncyc < MAX_CYC) {
                    tc->cyc[tc->ncyc].addr  = (uint16_t)a;
                    tc->cyc[tc->ncyc].val   = (uint8_t)v;
                    tc->cyc[tc->ncyc].write = (strcmp(kind, "write") == 0);
                    tc->ncyc++;
                } else if (!j->err) {
                    j->err = "trop de cycles";
                }
            }
        } else {
            j->err = "clé de cas inconnue";
        }
    }
    return !j->err;
}

/* ════════════════════════════════════════════════════════════════════
 *  Test machine: 64 KB flat
 *
 *  memory_t is the ORIC map (ROM at the top, I/O at $0300-$03FF). The oracle
 *  needs 64 KB of bare RAM: `rom_enabled = false` makes $C000-$FFFF
 *  readable/writable (rom[] array), and trivial I/O callbacks make
 *  $0300-$03FF writable instead of being swallowed by the I/O bus.
 * ══════════════════════════════════════════════════════════════════ */

static uint8_t flat_io_read(uint16_t addr, void* ud) {
    memory_t* mem = (memory_t*)ud;
    return mem->ram[addr];
}

static void flat_io_write(uint16_t addr, uint8_t v, void* ud) {
    memory_t* mem = (memory_t*)ud;
    mem->ram[addr] = v;
}

static uint8_t flat_read(memory_t* mem, uint16_t a) {
    return (a < 0xC000) ? mem->ram[a] : mem->rom[a - 0xC000];
}

static void flat_write(memory_t* mem, uint16_t a, uint8_t v) {
    if (a < 0xC000) mem->ram[a] = v;
    else            mem->rom[a - 0xC000] = v;
}

/* Bus accesses observed for the current case (filled by the CPU callback). */
#define MAX_OBS 32
static vbus_t  obs[MAX_OBS];
static int     obs_n;
static uint16_t dirty[MAX_OBS + MAX_RAM * 2];
static int     dirty_n;

static void note_dirty(uint16_t a) {
    if (dirty_n < (int)(sizeof(dirty) / sizeof(dirty[0]))) dirty[dirty_n++] = a;
}

static void bus_cb(void* ctx, uint16_t addr, uint8_t val, bool write) {
    (void)ctx;
    if (obs_n < MAX_OBS) {
        obs[obs_n].addr = addr;
        obs[obs_n].val = val;
        obs[obs_n].write = write;
        obs_n++;
    }
    note_dirty(addr);
}

/* ─── Global counters ─── */
typedef struct {
    long cases;
    long state_ok;
    long ram_ok;
    long cycles_ok;
    long subseq_ok;
    long exact_ok;
} score_t;

static score_t g;                     /* total, excluding JAM opcodes (current engine) */
static score_t gj;                    /* JAM opcodes, counted separately */
static score_t g_legacy, g_ms;        /* scores kept per engine */
static int  g_files = 0;              /* opcode files replayed */
static int  g_opcodes_clean = 0;      /* opcodes 100 % on state+RAM+cycles */
static int  g_jam_files = 0;          /* JAM files encountered */
static bool g_have_vectors = false;
static bool g_microseq = false;        /* core under judgement: micro-sequenced? */
static char g_worst[256] = "";        /* worst opcode on the final state */

/* The 12 JAM/KIL opcodes lock up the bus on a real NMOS 6502 (endless
 * loop). The oracle models this lock-up as 11 cycles of repeated reads;
 * Phosphoric sets `halted` and returns 0 cycles. This is a MODELLING
 * divergence, not a timing one: it does not belong on the N1→N4 scale and
 * would remain true after V2. These opcodes are therefore counted separately. */
static bool opcode_is_jam(const char* op) {
    static const char* jam[] = { "02","12","22","32","42","52",
                                 "62","72","92","b2","d2","f2" };
    for (size_t i = 0; i < sizeof(jam) / sizeof(jam[0]); i++)
        if (strcmp(op, jam[i]) == 0) return true;
    return false;
}

static bool seq_is_subsequence(const vbus_t* sub, int ns, const vbus_t* sup, int np) {
    int i = 0;
    for (int k = 0; k < np && i < ns; k++) {
        if (sub[i].addr == sup[k].addr && sub[i].val == sup[k].val &&
            sub[i].write == sup[k].write)
            i++;
    }
    return i == ns;
}

static bool seq_is_exact(const vbus_t* a, int na, const vbus_t* b, int nb) {
    if (na != nb) return false;
    for (int i = 0; i < na; i++)
        if (a[i].addr != b[i].addr || a[i].val != b[i].val || a[i].write != b[i].write)
            return false;
    return true;
}

/* Replays one case. Fills in the 5 conformance booleans. */
static void run_case(cpu6502_t* cpu, memory_t* mem, const vcase_t* tc,
                     bool* state_ok, bool* ram_ok, bool* cycles_ok,
                     bool* subseq_ok, bool* exact_ok) {
    /* Reset only the addresses dirtied by the previous case. */
    for (int i = 0; i < dirty_n; i++) flat_write(mem, dirty[i], 0);
    dirty_n = 0;
    obs_n = 0;

    for (int i = 0; i < tc->initial.nram; i++) {
        flat_write(mem, tc->initial.ram_a[i], tc->initial.ram_v[i]);
        note_dirty(tc->initial.ram_a[i]);
    }
    for (int i = 0; i < tc->final.nram; i++) note_dirty(tc->final.ram_a[i]);

    cpu->PC = tc->initial.pc;
    cpu->SP = tc->initial.s;
    cpu->A  = tc->initial.a;
    cpu->X  = tc->initial.x;
    cpu->Y  = tc->initial.y;
    cpu->P  = tc->initial.p;
    cpu->cycles = 0;
    cpu->halted = false;
    cpu->irq = 0;
    cpu->irq_pulse = 0;
    cpu->nmi_pending = false;

    int cyc = cpu_step(cpu);

    *state_ok = (cpu->PC == tc->final.pc && cpu->SP == tc->final.s &&
                 cpu->A == tc->final.a && cpu->X == tc->final.x &&
                 cpu->Y == tc->final.y && cpu->P == tc->final.p);

    *ram_ok = true;
    for (int i = 0; i < tc->final.nram; i++)
        if (flat_read(mem, tc->final.ram_a[i]) != tc->final.ram_v[i]) { *ram_ok = false; break; }

    *cycles_ok = (cyc == tc->ncyc);
    *subseq_ok = seq_is_subsequence(obs, obs_n, tc->cyc, tc->ncyc);
    *exact_ok  = seq_is_exact(obs, obs_n, tc->cyc, tc->ncyc);
}

static void run_opcode_file(const char* path, const char* opname,
                            long max_cases, bool verbose) {
    FILE* fp = fopen(path, "rb");
    if (!fp) return;
    if (fseek(fp, 0, SEEK_END) != 0) { fclose(fp); return; }
    long sz = ftell(fp);
    rewind(fp);
    char* buf = (char*)malloc((size_t)sz + 1);
    if (!buf) { fclose(fp); return; }
    size_t rd = fread(buf, 1, (size_t)sz, fp);
    fclose(fp);
    buf[rd] = '\0';

    static memory_t mem;          /* ~80 KB: static, not on the stack */
    static cpu6502_t cpu;
    memory_init(&mem);
    mem.rom_enabled = false;      /* $C000-$FFFF = flat RAM (rom[] array) */
    mem.io_read = flat_io_read;   /* $0300-$03FF = flat RAM, not the I/O bus */
    mem.io_write = flat_io_write;
    mem.io_userdata = &mem;
    cpu_init(&cpu, &mem);
    cpu_set_microseq(&cpu, g_microseq);
    cpu_set_bus_callback(&cpu, bus_cb, NULL);
    dirty_n = 0;

    jparse_t j = { buf, buf + rd, NULL };
    jeat(&j, '[');

    score_t f = {0};
    vcase_t tc;
    int shown = 0;
    while ((max_cases == 0 || f.cases < max_cases) && jparse_case(&j, &tc)) {
        bool st, ram, cy, sub, ex;
        run_case(&cpu, &mem, &tc, &st, &ram, &cy, &sub, &ex);
        f.cases++;
        f.state_ok += st; f.ram_ok += ram; f.cycles_ok += cy;
        f.subseq_ok += sub; f.exact_ok += ex;
        if (verbose && !(st && ram && cy) && shown < 3) {
            printf("      écart %s [%s] : état=%d ram=%d cycles=%d (%d attendus)\n",
                   opname, tc.name, st, ram, cy, tc.ncyc);
            shown++;
        }
        jeat(&j, ',');
    }
    free(buf);

    if (j.err) {
        printf("    parse %s : %s (après %ld cas)\n", opname, j.err, f.cases);
        tests_failed++;
        return;
    }
    if (f.cases == 0) return;

    g_files++;
    if (opcode_is_jam(opname)) {
        gj.cases += f.cases; gj.state_ok += f.state_ok; gj.ram_ok += f.ram_ok;
        gj.cycles_ok += f.cycles_ok; gj.subseq_ok += f.subseq_ok;
        gj.exact_ok += f.exact_ok;
        g_jam_files++;
        if (verbose)
            printf("    %s : JAM, %ld cas (comptés à part)\n", opname, f.cases);
        return;
    }
    g.cases += f.cases; g.state_ok += f.state_ok; g.ram_ok += f.ram_ok;
    g.cycles_ok += f.cycles_ok; g.subseq_ok += f.subseq_ok; g.exact_ok += f.exact_ok;
    if (f.state_ok == f.cases && f.ram_ok == f.cases && f.cycles_ok == f.cases)
        g_opcodes_clean++;
    else
        snprintf(g_worst + strlen(g_worst), sizeof(g_worst) - strlen(g_worst) - 1,
                 "%s ", opname);
    if (verbose || f.subseq_ok != f.cases || f.state_ok != f.cases)
        printf("    %s : %ld cas, état %ld, ram %ld, cycles %ld, sous-séq %ld, exact %ld\n",
               opname, f.cases, f.state_ok, f.ram_ok, f.cycles_ok, f.subseq_ok, f.exact_ok);
}

static long pct_bp(long ok, long total) {   /* per ten thousand */
    return total ? (ok * 10000) / total : 0;
}

static void print_rate(const char* label, long ok, long total) {
    long bp = pct_bp(ok, total);
    printf("    %-22s %8ld / %-8ld  %3ld.%02ld %%\n",
           label, ok, total, bp / 100, bp % 100);
}

/* ─── Tests ─── */

TEST(test_parser_on_embedded_sample) {
    /* A real case from a9.json (LDA #$cc), so the parser is tested even
     * when the vectors are not downloaded. */
    static const char sample[] =
        "[\n{ \"name\": \"a9 cc 21\", \"initial\": { \"pc\": 45930, \"s\": 172, "
        "\"a\": 67, \"x\": 145, \"y\": 150, \"p\": 237, \"ram\": [ [45930, 169], "
        "[45931, 204], [45932, 33]]}, \"final\": { \"pc\": 45932, \"s\": 172, "
        "\"a\": 204, \"x\": 145, \"y\": 150, \"p\": 237, \"ram\": [ [45930, 169], "
        "[45931, 204], [45932, 33]]}, \"cycles\": [ [45930, 169, \"read\"], "
        "[45931, 204, \"read\"]] }\n]";
    jparse_t j = { sample, sample + sizeof(sample) - 1, NULL };
    jeat(&j, '[');
    vcase_t tc;
    bool ok = jparse_case(&j, &tc);
    ASSERT_TRUE(ok && j.err == NULL);
    ASSERT_EQ(tc.initial.pc, 45930);
    ASSERT_EQ(tc.initial.nram, 3);
    ASSERT_EQ(tc.final.a, 204);
    ASSERT_EQ(tc.ncyc, 2);
    ASSERT_EQ(tc.cyc[1].addr, 45931);
    ASSERT_TRUE(tc.cyc[0].write == false);
    ASSERT_TRUE(strcmp(tc.name, "a9 cc 21") == 0);
}

TEST(test_embedded_sample_executes) {
    /* The same case replayed on the core: checks the 64 KB flat machine and the
     * bus callback, without depending on the downloaded vectors. */
    static const char sample[] =
        "{ \"name\": \"a9 cc 21\", \"initial\": { \"pc\": 45930, \"s\": 172, "
        "\"a\": 67, \"x\": 145, \"y\": 150, \"p\": 237, \"ram\": [ [45930, 169], "
        "[45931, 204], [45932, 33]]}, \"final\": { \"pc\": 45932, \"s\": 172, "
        "\"a\": 204, \"x\": 145, \"y\": 150, \"p\": 237, \"ram\": [ [45930, 169], "
        "[45931, 204], [45932, 33]]}, \"cycles\": [ [45930, 169, \"read\"], "
        "[45931, 204, \"read\"]] }";
    jparse_t j = { sample, sample + sizeof(sample) - 1, NULL };
    vcase_t tc;
    bool parsed = jparse_case(&j, &tc);
    ASSERT_TRUE(parsed);

    static memory_t mem;
    static cpu6502_t cpu;
    memory_init(&mem);
    mem.rom_enabled = false;
    mem.io_read = flat_io_read;
    mem.io_write = flat_io_write;
    mem.io_userdata = &mem;
    cpu_init(&cpu, &mem);
    cpu_set_microseq(&cpu, g_microseq);
    cpu_set_bus_callback(&cpu, bus_cb, NULL);
    dirty_n = 0;

    bool st, ram, cy, sub, ex;
    run_case(&cpu, &mem, &tc, &st, &ram, &cy, &sub, &ex);
    ASSERT_TRUE(st);      /* LDA immediate: final state conforms */
    ASSERT_TRUE(ram);
    ASSERT_TRUE(cy);      /* 2 cycles */
    ASSERT_TRUE(sub);
    ASSERT_TRUE(ex);      /* 2 bus accesses, no internal cycle: exact already at N2 */

    /* A write to $0300-$03FF must land in flat RAM (and not be
     * swallowed by the ORIC I/O bus): safeguard of the test model. */
    flat_write(&mem, 0x0310, 0x5A);
    ASSERT_EQ(flat_read(&mem, 0x0310), 0x5A);
    /* And $C000-$FFFF must be writable (rom_enabled = false). */
    flat_write(&mem, 0xFFFC, 0x42);
    ASSERT_EQ(flat_read(&mem, 0xFFFC), 0x42);
}

TEST(test_jam_divergence_is_documented) {
    /* The JAMs: final state conforms (the PC did advance by one byte), but
     * our core halts instead of looping → 0 cycles versus 11 expected.
     * This behaviour is locked in so it stays explicit and does not drift
     * into a false "everything is green". */
    if (gj.cases == 0) { printf("    (aucun vecteur JAM présent)\n"); return; }
    ASSERT_EQ(gj.state_ok, gj.cases);
    ASSERT_EQ(gj.cycles_ok, 0);
    printf("    %d opcodes JAM, %ld cas : état conforme, cycles hors modèle\n",
           g_jam_files, gj.cases);
}

TEST(test_final_state_conformance) {
    /* Property 1: the final state (registers + RAM) must be exact — this is
     * independent of the timing accuracy level. */
    ASSERT_EQ(g.state_ok, g.cases);
    ASSERT_EQ(g.ram_ok, g.cases);
}

TEST(test_cycle_count_conformance) {
    /* Property 2: the cycle total per instruction (level N1 already claimed). */
    ASSERT_EQ(g.cycles_ok, g.cases);
}

TEST(test_bus_subsequence_conformance) {
    /* Property 3: this is exactly what "ordered to the bus cycle" (N2)
     * means — no spurious access, no order inversion. */
    ASSERT_EQ(g.subseq_ok, g.cases);
}

TEST(test_bus_exact_baseline) {
    if (g_microseq) {
        /* The micro-sequenced engine aims for 100 %: that is its whole purpose. */
        long bpm = pct_bp(g.exact_ok, g.cases);
        printf("    séquence bus exacte (micro-séquencé) : %ld.%02ld %%\n",
               bpm / 100, bpm % 100);
        ASSERT_EQ(g.exact_ok, g.cases);
        return;
    }
    /* Property 4: the exact sequence, cycle by cycle (N3). Expected to be
     * incomplete today; the floor is locked in against regressions. */
    long bp = pct_bp(g.exact_ok, g.cases);
    printf("    séquence bus exacte : %ld.%02ld %% (socle %d.%02d %%)\n",
           bp / 100, bp % 100, BUS_EXACT_FLOOR_BP / 100, BUS_EXACT_FLOOR_BP % 100);
    /* The floor was measured over the 244 opcodes: on a SAMPLE (CI only
     * downloads about twenty opcodes, chosen where the legacy engine differs)
     * the rate is not comparable. The floor is only judged on the full set. */
    if (g_files < 240) {
        printf("    (échantillon de %d opcodes : socle non jugé, il vaut pour le jeu complet)\n",
               g_files);
        return;
    }
    ASSERT_TRUE(bp >= BUS_EXACT_FLOOR_BP);
}

TEST(test_microseq_dominates_legacy) {
    /* Migration safeguard: on the same set of cases, the micro-sequenced core
     * is at least as conformant as the legacy one on every property. */
    ASSERT_TRUE(g_ms.state_ok  >= g_legacy.state_ok);
    ASSERT_TRUE(g_ms.cycles_ok >= g_legacy.cycles_ok);
    ASSERT_TRUE(g_ms.subseq_ok >= g_legacy.subseq_ok);
    ASSERT_TRUE(g_ms.exact_ok  >  g_legacy.exact_ok);
    printf("    séquence bus exacte : historique %ld vs micro-séquencé %ld (sur %ld cas)\n",
           g_legacy.exact_ok, g_ms.exact_ok, g_legacy.cases);
}

int main(void) {
    printf("=== Oracle de conformité cycle par cycle (SingleStepTests/65x02) ===\n\n");

    RUN(test_parser_on_embedded_sample);
    RUN(test_embedded_sample_executes);

    const char* dir = getenv("CYCLE_VECTORS_DIR");
    if (!dir || !*dir) dir = "third_party/vectors/65x02";
    const char* maxs = getenv("CYCLE_MAX_CASES");
    long max_cases = maxs ? strtol(maxs, NULL, 10) : 200;
    const char* only = getenv("CYCLE_OPCODES");
    bool verbose = getenv("CYCLE_VERBOSE") && *getenv("CYCLE_VERBOSE") == '1';
    const char* engine = getenv("CYCLE_ENGINE");

    /* BOTH cores are judged in the same run (unless CYCLE_ENGINE
     * designates one): the legacy engine must hold its floor, the
     * micro-sequenced engine must be at 100 %. The comparison is what makes
     * the demonstration — and it forbids improving one by breaking
     * the other. */
    for (int pass = 0; pass < 2; pass++) {
        g_microseq = (pass == 1);
        if (engine && *engine) {
            bool want_ms = (strcmp(engine, "microseq") == 0);
            if (g_microseq != want_ms) continue;
        }

        /* reset the counters for this engine */
        memset(&g, 0, sizeof(g));
        memset(&gj, 0, sizeof(gj));
        g_files = 0; g_jam_files = 0; g_opcodes_clean = 0; g_worst[0] = '\0';

        DIR* d = opendir(dir);
        if (!d) {
            printf("\n  SKIP : vecteurs absents (%s)\n", dir);
            printf("  → tools/fetch_vectors.sh 65x02   (~1 Go, une seule fois)\n");
            break;
        }
        g_have_vectors = true;
        printf("\n  ══ Cœur %s ══  (vecteurs %s, max %ld cas/opcode)\n",
               g_microseq ? "MICRO-SÉQUENCÉ (V2-E1)" : "HISTORIQUE (N2)",
               dir, max_cases ? max_cases : 10000);
        char path[1024];
        struct dirent* e;
        while ((e = readdir(d)) != NULL) {
            size_t n = strlen(e->d_name);
            if (n != 7 || strcmp(e->d_name + 2, ".json") != 0) continue;
            char op[3] = { e->d_name[0], e->d_name[1], '\0' };
            if (only && *only && !strstr(only, op)) continue;
            snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
            run_opcode_file(path, op, max_cases, verbose);
        }
        closedir(d);

        if (g.cases == 0) continue;

        printf("\n  ── Score (%d opcodes, %ld cas) ──\n", g_files, g.cases);
        print_rate("etat final (regs)", g.state_ok, g.cases);
        print_rate("etat final (RAM)", g.ram_ok, g.cases);
        print_rate("total de cycles", g.cycles_ok, g.cases);
        print_rate("sous-sequence bus (N2)", g.subseq_ok, g.cases);
        print_rate("sequence bus exacte (N3)", g.exact_ok, g.cases);
        printf("    opcodes 100 %% (etat+RAM+cycles) : %d / %d\n",
               g_opcodes_clean, g_files - g_jam_files);
        if (*g_worst) printf("    opcodes avec ecart : %s\n", g_worst);
        if (gj.cases)
            printf("    opcodes JAM (hors échelle, comptés à part) : %d fichiers, %ld cas\n",
                   g_jam_files, gj.cases);

        RUN(test_jam_divergence_is_documented);
        RUN(test_final_state_conformance);
        RUN(test_cycle_count_conformance);
        RUN(test_bus_subsequence_conformance);
        RUN(test_bus_exact_baseline);

        if (g_microseq) g_ms = g; else g_legacy = g;
    }

    /* The micro-sequencer must never be WORSE than the legacy engine. */
    if (g_legacy.cases && g_ms.cases) {
        RUN(test_microseq_dominates_legacy);
    }

    printf("\n---\nTests passed: %d\nTests failed: %d\n", tests_passed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
