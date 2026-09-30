/* SPDX-License-Identifier: EUPL-1.2 */
/**
 * @file fuzz_common.h
 * @brief Outils communs aux cibles de fuzzing (tests/fuzz/)
 * @author bmarty <bmarty@mailo.com>
 *
 * Chaque cible définit LLVMFuzzerTestOneInput() : avec clang
 * (-fsanitize=fuzzer, `make fuzz`) libFuzzer fournit main() ; avec gcc,
 * fuzz_replay.c rejoue un corpus (`make test-fuzz-replay`). Chaque cible
 * définit aussi fuzz_make_seed() : une graine valide, ou FUZZ_NO_SEED().
 *
 * Les analyseurs de Phosphoric prennent des chemins : l'entrée est écrite dans
 * un fichier temporaire, réutilisé d'un appel à l'autre.
 */
#ifndef FUZZ_COMMON_H
#define FUZZ_COMMON_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size);

/* Graine (fuzz_replay --seed CHEMIN) : 0 = écrite. Pas de symbole faible :
 * l'éditeur de liens de macOS refuse un symbole faible non défini. */
int fuzz_make_seed(const char* path);
#define FUZZ_NO_SEED() \
    int fuzz_make_seed(const char* path) { (void)path; return -1; }

static char fuzz_tmp_path[64];
static void fuzz_tmp_remove(void) { if (fuzz_tmp_path[0]) unlink(fuzz_tmp_path); }

static inline const char* fuzz_tmpfile(const uint8_t* data, size_t size) {
    char* path = fuzz_tmp_path;
    if (!path[0]) {
        snprintf(path, sizeof fuzz_tmp_path, "/tmp/phosphoric_fuzz_%ld", (long)getpid());
        atexit(fuzz_tmp_remove);
    }
    FILE* f = fopen(path, "wb");
    if (!f) abort();
    if (size && fwrite(data, 1, size, f) != size) abort();
    fclose(f);
    return path;
}

/* Journal muet : une entrée invalide produit des erreurs attendues, et les
 * écrire ralentirait le fuzzing. Pour les cibles qui lient utils/logging.c. */
#define FUZZ_QUIET_LOGS() do {                              \
        static int fuzz_quiet_done;                         \
        if (!fuzz_quiet_done) {                             \
            fuzz_quiet_done = 1;                            \
            log_init(LOG_LEVEL_ERROR);                      \
            FILE* fuzz_null = fopen("/dev/null", "w");      \
            if (fuzz_null) log_set_stream(fuzz_null);       \
        }                                                   \
    } while (0)

#endif /* FUZZ_COMMON_H */
