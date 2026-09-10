/* SPDX-License-Identifier: EUPL-1.2 */
/**
 * @file test_cpu_cycles.c
 * @brief Oracle de conformité cycle par cycle du 6502 (V2-S1)
 * @author bmarty <bmarty@mailo.com>
 *
 * Rejoue les vecteurs **SingleStepTests/65x02** (10 000 cas par opcode, chacun
 * donnant l'état initial, l'état final ET la liste des accès bus attendus,
 * cycle par cycle) contre le cœur de Phosphoric, et publie un score.
 *
 * C'est l'instrument de mesure de la V2 (docs/specs/V2_CYCLE_ACCURACY.md) : il
 * transforme « je crois que le timing est juste » en un chiffre. Quatre
 * propriétés sont mesurées séparément, de la plus faible à la plus forte :
 *
 *   1. état final      — PC/S/A/X/Y/P et RAM finale conformes
 *   2. total de cycles — le nombre de cycles de l'instruction est exact
 *   3. sous-séquence   — nos accès bus sont, DANS L'ORDRE, un sous-ensemble des
 *                        cycles attendus : aucun accès parasite, aucune inversion
 *                        (c'est la propriété du niveau **N2** actuel)
 *   4. séquence exacte — nos accès bus sont EXACTEMENT les cycles attendus, un
 *                        par cycle (c'est la propriété du niveau **N3** visé)
 *
 * Le score 4 est **bas aujourd'hui** : le cœur n'émet pas les accès factices et
 * bourre les cycles internes en fin d'instruction (docs/ACCURACY.md). Ce n'est
 * pas un échec du test, c'est la ligne de base que la V2 doit faire monter —
 * d'où l'assertion « jamais en dessous du socle » plutôt que « égal à 100 % ».
 *
 * Les vecteurs ne sont pas versionnés (~1 Go) : `tools/fetch_vectors.sh`.
 * Sans eux la suite se met en SKIP et ne teste que son propre parseur.
 *
 * Variables d'environnement :
 *   CYCLE_VECTORS_DIR  répertoire des .json        (défaut third_party/vectors/65x02)
 *   CYCLE_MAX_CASES    cas par opcode, 0 = tous    (défaut 200)
 *   CYCLE_OPCODES      liste « a9,b1,9d » à tester (défaut : tous les fichiers présents)
 *   CYCLE_VERBOSE      1 = détail par opcode + premiers écarts
 */

#include "cpu/cpu6502.h"
#include "memory/memory.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <dirent.h>

/* ─── Socle de non-régression : la séquence bus exacte ne doit jamais
 * redescendre sous ce taux (en pour dix-mille).
 *
 * Référence V2-S1, exécution exhaustive (244 opcodes hors JAM × 10 000 cas =
 * 2 440 000 cas) : **44,26 %**. Le taux est stable à ±0,1 % dès 100 cas par
 * opcode, donc ce socle vaut aussi pour les exécutions échantillonnées.
 * Les trois autres propriétés sont à 100,00 % — voir docs/ACCURACY.md.
 * V2-E1 (cœur micro-séquencé + accès factices) doit faire monter ce chiffre
 * vers 10000 ; relever le socle à chaque palier franchi. */
#define BUS_EXACT_FLOOR_BP  4400

/* ─── Cadre de test minimal (convention du projet) ─── */
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
 *  Parseur JSON minimal, taillé pour le schéma des vecteurs
 *
 *  [ { "name": str,
 *      "initial": { "pc":n, "s":n, "a":n, "x":n, "y":n, "p":n, "ram":[[a,v],…] },
 *      "final":   { … idem … },
 *      "cycles":  [ [addr, data, "read"|"write"], … ] }, … ]
 *
 *  Volontairement tolérant sur les espaces et l'ordre des clés, strict sur la
 *  forme : tout écart renvoie une erreur plutôt qu'un résultat silencieux.
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
    const char* p;      /* curseur */
    const char* end;
    const char* err;    /* non-NULL dès la première erreur */
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

/* Copie une chaîne JSON dans out (tronquée), sans gérer les échappements :
 * le schéma n'en contient pas. */
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

/* Lit une clé "xxx": et renvoie true si elle vaut `key`. Ne consomme rien en
 * cas de non-correspondance : l'appelant compare dans l'ordre qu'il veut. */
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
 *  Machine d'essai : 64 Ko plats
 *
 *  memory_t est la carte ORIC (ROM en haut, I/O à $0300-$03FF). Pour l'oracle
 *  il faut 64 Ko de RAM nue : `rom_enabled = false` rend $C000-$FFFF
 *  lisible/inscriptible (tableau rom[]), et des callbacks d'I/O triviaux
 *  rendent $0300-$03FF inscriptible au lieu d'être avalé par le bus I/O.
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

/* Accès bus observés pour le cas en cours (remplis par le callback CPU). */
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

/* ─── Compteurs globaux ─── */
typedef struct {
    long cases;
    long state_ok;
    long ram_ok;
    long cycles_ok;
    long subseq_ok;
    long exact_ok;
} score_t;

static score_t g;                     /* total, hors opcodes JAM */
static score_t gj;                    /* opcodes JAM, comptés à part */
static int  g_files = 0;              /* fichiers d'opcodes rejoués */
static int  g_opcodes_clean = 0;      /* opcodes 100 % sur état+RAM+cycles */
static int  g_jam_files = 0;          /* fichiers JAM rencontrés */
static bool g_have_vectors = false;
static char g_worst[256] = "";        /* pire opcode sur l'état final */

/* Les 12 opcodes JAM/KIL bloquent le bus sur un vrai NMOS 6502 (boucle sans
 * fin). L'oracle modélise ce blocage par 11 cycles de lectures répétées ;
 * Phosphoric pose `halted` et renvoie 0 cycle. C'est une divergence de
 * MODÉLISATION, pas de timing : elle ne relève pas de l'échelle N1→N4 et
 * resterait vraie après la V2. Ces opcodes sont donc comptés séparément. */
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

/* Rejoue un cas. Renseigne les 5 booléens de conformité. */
static void run_case(cpu6502_t* cpu, memory_t* mem, const vcase_t* tc,
                     bool* state_ok, bool* ram_ok, bool* cycles_ok,
                     bool* subseq_ok, bool* exact_ok) {
    /* Remise à zéro des seules adresses salies par le cas précédent. */
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

    static memory_t mem;          /* ~80 Ko : statique, pas sur la pile */
    static cpu6502_t cpu;
    memory_init(&mem);
    mem.rom_enabled = false;      /* $C000-$FFFF = RAM plate (tableau rom[]) */
    mem.io_read = flat_io_read;   /* $0300-$03FF = RAM plate, pas le bus I/O */
    mem.io_write = flat_io_write;
    mem.io_userdata = &mem;
    cpu_init(&cpu, &mem);
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

static long pct_bp(long ok, long total) {   /* pour dix-mille */
    return total ? (ok * 10000) / total : 0;
}

static void print_rate(const char* label, long ok, long total) {
    long bp = pct_bp(ok, total);
    printf("    %-22s %8ld / %-8ld  %3ld.%02ld %%\n",
           label, ok, total, bp / 100, bp % 100);
}

/* ─── Tests ─── */

TEST(test_parser_on_embedded_sample) {
    /* Un cas réel de a9.json (LDA #$cc), pour que le parseur soit testé même
     * quand les vecteurs ne sont pas téléchargés. */
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
    /* Le même cas rejoué sur le cœur : vérifie la machine 64 Ko plats et le
     * callback bus, sans dépendre des vecteurs téléchargés. */
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
    cpu_set_bus_callback(&cpu, bus_cb, NULL);
    dirty_n = 0;

    bool st, ram, cy, sub, ex;
    run_case(&cpu, &mem, &tc, &st, &ram, &cy, &sub, &ex);
    ASSERT_TRUE(st);      /* LDA immédiat : état final conforme */
    ASSERT_TRUE(ram);
    ASSERT_TRUE(cy);      /* 2 cycles */
    ASSERT_TRUE(sub);
    ASSERT_TRUE(ex);      /* 2 accès bus, aucun cycle interne : exact dès N2 */

    /* Une écriture en $0300-$03FF doit atterrir en RAM plate (et non être
     * avalée par le bus I/O de l'ORIC) : garde-fou du modèle d'essai. */
    flat_write(&mem, 0x0310, 0x5A);
    ASSERT_EQ(flat_read(&mem, 0x0310), 0x5A);
    /* Et $C000-$FFFF doit être inscriptible (rom_enabled = false). */
    flat_write(&mem, 0xFFFC, 0x42);
    ASSERT_EQ(flat_read(&mem, 0xFFFC), 0x42);
}

TEST(test_jam_divergence_is_documented) {
    /* Les JAM : état final conforme (le PC a bien avancé d'un octet), mais
     * notre cœur s'arrête au lieu de boucler → 0 cycle contre 11 attendus.
     * On verrouille ce comportement pour qu'il reste explicite et ne dérive pas
     * vers un faux « tout est vert ». */
    if (gj.cases == 0) { printf("    (aucun vecteur JAM présent)\n"); return; }
    ASSERT_EQ(gj.state_ok, gj.cases);
    ASSERT_EQ(gj.cycles_ok, 0);
    printf("    %d opcodes JAM, %ld cas : état conforme, cycles hors modèle\n",
           g_jam_files, gj.cases);
}

TEST(test_final_state_conformance) {
    /* Propriété 1 : l'état final (registres + RAM) doit être exact — c'est
     * indépendant du niveau de précision temporelle. */
    ASSERT_EQ(g.state_ok, g.cases);
    ASSERT_EQ(g.ram_ok, g.cases);
}

TEST(test_cycle_count_conformance) {
    /* Propriété 2 : le total de cycles par instruction (niveau N1 déjà revendiqué). */
    ASSERT_EQ(g.cycles_ok, g.cases);
}

TEST(test_bus_subsequence_conformance) {
    /* Propriété 3 : c'est exactement ce que « ordonné au cycle bus » (N2)
     * veut dire — aucun accès parasite, aucune inversion d'ordre. */
    ASSERT_EQ(g.subseq_ok, g.cases);
}

TEST(test_bus_exact_baseline) {
    /* Propriété 4 : la séquence exacte, cycle par cycle (N3). Attendue
     * incomplète aujourd'hui ; on verrouille le socle contre les régressions. */
    long bp = pct_bp(g.exact_ok, g.cases);
    printf("    séquence bus exacte : %ld.%02ld %% (socle %d.%02d %%)\n",
           bp / 100, bp % 100, BUS_EXACT_FLOOR_BP / 100, BUS_EXACT_FLOOR_BP % 100);
    ASSERT_TRUE(bp >= BUS_EXACT_FLOOR_BP);
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

    DIR* d = opendir(dir);
    if (!d) {
        printf("\n  SKIP : vecteurs absents (%s)\n", dir);
        printf("  → tools/fetch_vectors.sh 65x02   (~1 Go, une seule fois)\n");
    } else {
        g_have_vectors = true;
        printf("\n  Vecteurs : %s (max %ld cas/opcode)\n", dir,
               max_cases ? max_cases : 10000);
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
    }

    if (g_have_vectors && g.cases > 0) {
        printf("\n  ── Score de conformité (%d opcodes, %ld cas) ──\n", g_files, g.cases);
        print_rate("etat final (regs)", g.state_ok, g.cases);
        print_rate("etat final (RAM)", g.ram_ok, g.cases);
        print_rate("total de cycles", g.cycles_ok, g.cases);
        print_rate("sous-sequence bus (N2)", g.subseq_ok, g.cases);
        print_rate("sequence bus exacte (N3)", g.exact_ok, g.cases);
        printf("    opcodes 100 %% (etat+RAM+cycles) : %d / %d\n",
               g_opcodes_clean, g_files - g_jam_files);
        if (*g_worst) printf("    opcodes avec ecart : %s\n", g_worst);

        if (gj.cases) {
            printf("    opcodes JAM (hors échelle, comptés à part) : %d fichiers, %ld cas\n",
                   g_jam_files, gj.cases);
        }

        RUN(test_jam_divergence_is_documented);
        RUN(test_final_state_conformance);
        RUN(test_cycle_count_conformance);
        RUN(test_bus_subsequence_conformance);
        RUN(test_bus_exact_baseline);
    }

    printf("\n---\nTests passed: %d\nTests failed: %d\n", tests_passed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
