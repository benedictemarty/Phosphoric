/* SPDX-License-Identifier: EUPL-1.2 */
/**
 * @file fuzz_replay.c
 * @brief Rejoue un corpus sur une cible de fuzzing, sans libFuzzer
 * @author bmarty <bmarty@mailo.com>
 *
 * Usage : fuzz_<cible> FICHIER|DOSSIER...   (rejoue chaque fichier)
 *         fuzz_<cible> --seed CHEMIN        (écrit la graine de la cible)
 * Sert à `make test-fuzz-replay` (gcc, sanitizers facultatifs) : les graines et
 * les entrées ayant déjà fait tomber un analyseur (tests/fuzz/regressions/)
 * repassent à chaque exécution de la suite.
 */
#define _POSIX_C_SOURCE 200809L
#include "fuzz_common.h"
#include <dirent.h>
#include <string.h>
#include <sys/stat.h>

static int replay_file(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "fuzz_replay: %s illisible\n", path); return 1; }
    uint8_t* buf = NULL;
    size_t len = 0, cap = 0;
    for (;;) {
        if (len == cap) {
            cap = cap ? cap * 2 : 65536;
            uint8_t* nb = realloc(buf, cap);
            if (!nb) { free(buf); fclose(f); return 1; }
            buf = nb;
        }
        size_t n = fread(buf + len, 1, cap - len, f);
        len += n;
        if (n == 0) break;
    }
    fclose(f);
    LLVMFuzzerTestOneInput(buf, len);
    free(buf);
    return 0;
}

int main(int argc, char** argv) {
    if (argc == 3 && strcmp(argv[1], "--seed") == 0) {
        int rc = fuzz_make_seed(argv[2]);
        if (rc < 0) fprintf(stderr, "fuzz_replay: pas de graine pour cette cible\n");
        return rc == 0 ? 0 : 1;
    }
    int files = 0, errors = 0;
    for (int i = 1; i < argc; i++) {
        struct stat st;
        if (stat(argv[i], &st) != 0) continue;           /* dossier absent : rien à rejouer */
        if (S_ISDIR(st.st_mode)) {
            DIR* d = opendir(argv[i]);
            if (!d) continue;
            struct dirent* e;
            while ((e = readdir(d)) != NULL) {
                if (e->d_name[0] == '.') continue;
                char p[4096];
                snprintf(p, sizeof p, "%s/%s", argv[i], e->d_name);
                errors += replay_file(p);
                files++;
            }
            closedir(d);
        } else {
            errors += replay_file(argv[i]);
            files++;
        }
    }
    printf("  %d entrée(s) rejouée(s)\n", files);
    return errors ? 1 : 0;
}
