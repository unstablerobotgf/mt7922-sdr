#!/usr/bin/env bash
# Back to WiFi: unloading mt7921e replays the undo log, then the stock driver and Bluetooth return.
#   tools/sdr-down.sh      (run as root, with no program holding /dev/mt7921_sdr open)
set -uo pipefail

wifi_if() {
	for n in /sys/class/net/*; do
		[ "$(basename "$(readlink -f "$n/device/driver" 2>/dev/null)")" = mt7921e ] &&
			{ basename "$n"; return 0; }
	done
	return 1
}

rmmod mt7921e mt7921_common || { echo "rmmod failed (is /dev/mt7921_sdr still open?)"; exit 1; }
dmesg | grep -E 'sdr: (undo|stream stopped)' | tail -2 | sed -E 's/^.*sdr: //'
modprobe mt7921e
for _ in $(seq 60); do wifi_if >/dev/null && break; sleep 0.5; done
if ifc=$(wifi_if); then
	echo "WiFi back: $ifc"
	command -v nmcli >/dev/null && nmcli device set "$ifc" managed yes
else
	echo "WiFi interface did not come back; see dmesg"
fi
modprobe btusb
rfkill unblock bluetooth
