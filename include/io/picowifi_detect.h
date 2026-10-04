/* SPDX-License-Identifier: EUPL-1.2 */
/**
 * @file picowifi_detect.h
 * @brief Détection d'un modem picowifi réel (PicoWiFiModemUSB) et du pont
 *        LOCI-USB (Feather, --loci-hw) branchés sur l'hôte
 * @author bmarty <bmarty@mailo.com>
 *
 * Le firmware PicoWiFiModemUSB s'annonce en USB avec le produit
 * « PicoWifiModemUSB » (VID 0xCafe, celui de TinyUSB, partagé par d'autres
 * montages : seul le nom de produit identifie le modem). La recherche lit
 * seulement /sys : le port n'est jamais ouvert (une ouverture à 1200 bauds
 * redémarre le Pico en mode programmation).
 */
#ifndef PICOWIFI_DETECT_H
#define PICOWIFI_DETECT_H

#include <stdbool.h>
#include <stddef.h>

#define PICOWIFI_USB_PRODUCT "PicoWifiModemUSB"

/* Premier port série (ordre alphabétique de /sys/class/tty) dont le produit USB
 * est PICOWIFI_USB_PRODUCT : « /dev/ttyACM0 » dans @p out. @p sysfs_root : NULL
 * pour $PHOSPHORIC_SYSFS_ROOT, sinon « /sys » (les tests passent une
 * arborescence factice). false si aucun (ou hôte sans /sys : Windows, macOS,
 * version web). */
bool picowifi_detect(const char* sysfs_root, char* out, size_t outsz);

/* Firmware loci-usb (Feather RP2040 ou carte LOCI-USB, ~/loci/firmware et
 * loci-fw-usb) : produit « LOCI-USB (bus 6502 pour Phosphoric) » ou
 * « LOCI-USB loci-fw (bus 6502) », VID:PID 2E8A:000A (générique Raspberry Pi,
 * insuffisant) — reconnu par le début de son nom de produit. */
#define LOCI_USB_PRODUCT_PREFIX "LOCI-USB"

/* Premier port série dont le produit USB commence par LOCI_USB_PRODUCT_PREFIX
 * (mêmes règles que picowifi_detect : /sys seulement, port jamais ouvert). */
bool loci_usb_detect(const char* sysfs_root, char* out, size_t outsz);

/* Option --serial qui relie l'ACIA au port @p dev (115200 8N1). */
void picowifi_serial_spec(const char* dev, char* out, size_t outsz);

#endif /* PICOWIFI_DETECT_H */
