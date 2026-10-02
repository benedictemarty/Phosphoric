#!/bin/sh
# SPDX-License-Identifier: EUPL-1.2
#
# ci_apt_install.sh — installs apt packages in CI without getting stuck.
#
# Usage: tools/ci_apt_install.sh PACKAGE...
#
# The apt mirror of the GitHub runners is sometimes very slow: the "Install
# MinGW-w64" step usually takes 21 to 45 s, but has already taken 10, 19 and
# 47 min (and eventually succeeded). Each attempt is therefore bounded (update
# 4 min, install 10 min, apt network timeouts 30 s) and retried up to 3 times.
#
# Author: bmarty <bmarty@mailo.com>
set -u
# Timeouts in seconds, adjustable for the test (tests/integration/test_ci_apt_install.sh).
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
