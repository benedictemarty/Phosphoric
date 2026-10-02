#!/bin/sh
# SPDX-License-Identifier: EUPL-1.2
#
# ci_apt_install.sh — installe des paquets apt en CI sans rester bloqué.
#
# Usage : tools/ci_apt_install.sh PAQUET...
#
# Le dépôt apt des runners GitHub est parfois très lent : l'étape « Install
# MinGW-w64 » prend 21 à 45 s d'habitude, mais a déjà mis 10, 19 et 47 min
# (et fini par réussir). Chaque tentative est donc bornée (update 4 min,
# install 10 min, délais réseau d'apt à 30 s) et relancée jusqu'à 3 fois.
#
# Author: bmarty <bmarty@mailo.com>
set -u
# Délais en secondes, réglables pour le test (tests/integration/test_ci_apt_install.sh).
T_UPDATE=${CI_APT_T_UPDATE:-240}
T_INSTALL=${CI_APT_T_INSTALL:-600}
PAUSE=${CI_APT_PAUSE:-15}
APT="sudo apt-get -q -o Acquire::Retries=3 -o Acquire::http::Timeout=30 -o Acquire::https::Timeout=30"
for i in 1 2 3; do
    if timeout "$T_UPDATE" $APT update && timeout "$T_INSTALL" $APT install -y "$@"; then
        exit 0
    fi
    echo "ci_apt_install : tentative $i échouée ou trop longue, nouvel essai" >&2
    sleep "$PAUSE"
done
echo "ci_apt_install : échec après 3 tentatives ($*)" >&2
exit 1
