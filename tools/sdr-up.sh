#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 unstablerobotgf
# Switch the MT7922 from WiFi to SDR mode: Bluetooth off, mt7921e reloaded with sdr=1.
#   tools/sdr-up.sh [MODULE_DIR]     (default: build/mt76/mt7921; run as root)
# /dev/mt7921_sdr appears once the radio is up. tools/sdr-down.sh undoes it.
set -uo pipefail
here=$(cd "$(dirname "$0")/.." && pwd)
mods=${1:-$here/build/mt76/mt7921}

want=$(uname -r)
for ko in mt7921-common.ko mt7921e.ko; do
	have=$(modinfo -F vermagic "$mods/$ko" 2>/dev/null | cut -d' ' -f1)
	[ "$have" = "$want" ] || { echo "$mods/$ko is for '$have', running $want: rebuild"; exit 1; }
done

# Bluetooth shares the connectivity subsystem (TOP, CONN_INFRA) with WiFi: keep it out of the way.
rfkill block bluetooth; sleep 1
rmmod btusb btmtk 2>/dev/null; sleep 1
if lsmod | grep -q '^btusb '; then echo "btusb still loaded"; exit 1; fi

rmmod mt7921e mt7921_common || exit 1	# not modprobe -r: keep mt76 and mt792x-lib loaded
insmod "$mods/mt7921-common.ko" sdr=1 || exit 1
insmod "$mods/mt7921e.ko" || exit 1
for _ in $(seq 60); do [ -e /dev/mt7921_sdr ] && break; sleep 0.5; done
if [ -e /dev/mt7921_sdr ]; then
	echo "SDR up: /dev/mt7921_sdr"
else
	echo "no /dev/mt7921_sdr; last driver messages:"
	dmesg | grep 'sdr:' | tail -8
	exit 1
fi
