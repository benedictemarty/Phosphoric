/* SPDX-License-Identifier: EUPL-1.2 */
/**
 * @file config.c
 * @brief Configuration system - .ini file parser
 * @author bmarty <bmarty@mailo.com>
 * @date 2026-02-22
 * @version 0.9.0-alpha
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <ctype.h>

#define CONFIG_MAX_KEY   64
#define CONFIG_MAX_VALUE 256
#define CONFIG_MAX_ITEMS 64

typedef struct {
    char key[CONFIG_MAX_KEY];
    char value[CONFIG_MAX_VALUE];
} config_item_t;

static config_item_t config_items[CONFIG_MAX_ITEMS];
static int config_count = 0;

static char* trim(char* str) {
    while (isspace((unsigned char)*str)) str++;
    char* end = str + strlen(str) - 1;
    while (end > str && isspace((unsigned char)*end)) *end-- = '\0';
    return str;
}

void config_init(void) {
    config_count = 0;
    memset(config_items, 0, sizeof(config_items));
}

bool config_load(const char* filename) {
    FILE* fp = fopen(filename, "r");
    if (!fp) return false;

    char line[512];
    while (fgets(line, sizeof(line), fp)) {
        char* trimmed = trim(line);
        if (trimmed[0] == '#' || trimmed[0] == ';' || trimmed[0] == '\0') continue;
        if (trimmed[0] == '[') continue; /* Skip section headers */

        char* eq = strchr(trimmed, '=');
        if (!eq) continue;

        *eq = '\0';
        char* key = trim(trimmed);
        char* value = trim(eq + 1);

        if (config_count < CONFIG_MAX_ITEMS) {
            strncpy(config_items[config_count].key, key, CONFIG_MAX_KEY - 1);
            strncpy(config_items[config_count].value, value, CONFIG_MAX_VALUE - 1);
            config_count++;
        }
    }

    fclose(fp);
    return true;
}

bool config_save(const char* filename) {
    FILE* fp = fopen(filename, "w");
    if (!fp) return false;

    fprintf(fp, "# Phosphoric Configuration\n\n");
    for (int i = 0; i < config_count; i++) {
        fprintf(fp, "%s = %s\n", config_items[i].key, config_items[i].value);
    }

    fclose(fp);
    return true;
}

const char* config_get(const char* key) {
    for (int i = 0; i < config_count; i++) {
        if (strcmp(config_items[i].key, key) == 0) return config_items[i].value;
    }
    return NULL;
}

int config_get_int(const char* key, int default_val) {
    const char* val = config_get(key);
    if (val) return atoi(val);
    return default_val;
}

bool config_get_bool(const char* key, bool default_val) {
    const char* val = config_get(key);
    if (!val) return default_val;
    return strcmp(val, "true") == 0 || strcmp(val, "1") == 0 || strcmp(val, "yes") == 0;
}

void config_set(const char* key, const char* value) {
    for (int i = 0; i < config_count; i++) {
        if (strcmp(config_items[i].key, key) == 0) {
            strncpy(config_items[i].value, value, CONFIG_MAX_VALUE - 1);
            return;
        }
    }
    if (config_count < CONFIG_MAX_ITEMS) {
        strncpy(config_items[config_count].key, key, CONFIG_MAX_KEY - 1);
        strncpy(config_items[config_count].value, value, CONFIG_MAX_VALUE - 1);
        config_count++;
    }
}
