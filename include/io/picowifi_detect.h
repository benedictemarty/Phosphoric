/* SPDX-License-Identifier: EUPL-1.2 */
/**
 * @file picowifi_detect.h
 * @brief Detection of a real picowifi modem (PicoWiFiModemUSB) plugged into the host
 * @author bmarty <bmarty@mailo.com>
 *
 * The PicoWiFiModemUSB firmware announces itself over USB with the product
 * « PicoWifiModemUSB » (VID 0xCafe, TinyUSB's, shared by other builds: only
 * the product name identifies the modem). The lookup only reads /sys: the
 * port is never opened (opening it at 1200 baud reboots the Pico into
 * programming mode).
 */
#ifndef PICOWIFI_DETECT_H
#define PICOWIFI_DETECT_H

#include <stdbool.h>
#include <stddef.h>

#define PICOWIFI_USB_PRODUCT "PicoWifiModemUSB"

/* First serial port (alphabetical order of /sys/class/tty) whose USB product
 * is PICOWIFI_USB_PRODUCT: « /dev/ttyACM0 » in @p out. @p sysfs_root: NULL
 * for $PHOSPHORIC_SYSFS_ROOT, else « /sys » (tests pass a fake tree). false
 * if none (or a host without /sys: Windows, macOS, web build). */
bool picowifi_detect(const char* sysfs_root, char* out, size_t outsz);

/* --serial option linking the ACIA to port @p dev (115200 8N1). */
void picowifi_serial_spec(const char* dev, char* out, size_t outsz);

#endif /* PICOWIFI_DETECT_H */
