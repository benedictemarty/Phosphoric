/* SPDX-License-Identifier: EUPL-1.2 */
/**
 * @file picowifi_detect.c
 * @brief Detection of a real picowifi modem and of a LOCI-USB (Feather) by
 *        their USB product name (/sys)
 * @author bmarty <bmarty@mailo.com>
 */
#include "io/picowifi_detect.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if !defined(_WIN32) && !defined(__EMSCRIPTEN__)
#include <dirent.h>
#endif

void picowifi_serial_spec(const char* dev, char* out, size_t outsz) {
    snprintf(out, outsz, "com:115200,8,N,1,%s", dev);
}

#if defined(_WIN32) || defined(__EMSCRIPTEN__)

static bool usb_tty_detect(const char* sysfs_root, const char* product, bool prefix,
                           char* out, size_t outsz) {
    (void)sysfs_root; (void)product; (void)prefix; (void)out; (void)outsz;
    return false;
}

#else

/* USB product of port @p tty: <root>/class/tty/<tty>/device is the USB
 * interface, the « product » file is in the parent device. @p prefix: the
 * product starts with @p product (otherwise: it is equal to it). */
static bool tty_product_matches(const char* root, const char* tty, const char* product,
                                bool prefix) {
    char path[512], line[128];
    snprintf(path, sizeof(path), "%.400s/class/tty/%.63s/device/../product", root, tty);
    FILE* f = fopen(path, "r");
    if (!f) return false;
    bool ok = fgets(line, sizeof(line), f) != NULL;
    fclose(f);
    if (!ok) return false;
    line[strcspn(line, "\r\n")] = '\0';
    return prefix ? strncmp(line, product, strlen(product)) == 0
                  : strcmp(line, product) == 0;
}

static int by_name(const void* a, const void* b) {
    return strcmp((const char*)a, (const char*)b);
}

static bool usb_tty_detect(const char* sysfs_root, const char* product, bool prefix,
                           char* out, size_t outsz) {
    const char* env = getenv("PHOSPHORIC_SYSFS_ROOT");    /* tests */
    const char* root = sysfs_root ? sysfs_root : env && *env ? env : "/sys";
    char dir[512];
    snprintf(dir, sizeof(dir), "%.480s/class/tty", root);
    DIR* d = opendir(dir);
    if (!d) return false;
    char names[64][64];
    int n = 0;
    struct dirent* de;
    while ((de = readdir(d)) != NULL && n < 64)
        if (strncmp(de->d_name, "ttyACM", 6) == 0 || strncmp(de->d_name, "ttyUSB", 6) == 0)
            snprintf(names[n++], sizeof(names[0]), "%.63s", de->d_name);
    closedir(d);
    qsort(names, (size_t)n, sizeof(names[0]), by_name);
    for (int i = 0; i < n; i++) {
        if (tty_product_matches(root, names[i], product, prefix)) {
            snprintf(out, outsz, "/dev/%s", names[i]);
            return true;
        }
    }
    return false;
}

#endif

bool picowifi_detect(const char* sysfs_root, char* out, size_t outsz) {
    return usb_tty_detect(sysfs_root, PICOWIFI_USB_PRODUCT, false, out, outsz);
}

bool loci_usb_detect(const char* sysfs_root, char* out, size_t outsz) {
    return usb_tty_detect(sysfs_root, LOCI_USB_PRODUCT_PREFIX, true, out, outsz);
}
