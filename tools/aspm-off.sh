#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 unstablerobotgf
# aspm-off -- disable PCIe ASPM L0s/L1 for the MT7922 and its root port.
#
# Why: full-rate 80 MSps streaming (320 MB/s in 8 KiB DMA bursts roughly
# 25.6 us apart) is only sustained when the PCIe link is not allowed to
# drop into ASPM L1 between bursts; with L1 enabled the exit latency costs
# up to ~12 % of the 80 MSps rate (10 s runs swing 88 % to ~100 % duty
# depending on the link state). 40 MSps tolerates it, and 160 MSps is
# link-rate-limited (gapped) either way. On platforms whose firmware
# declares ASPM unsupported the kernel never manages these bits, so the
# enable bits persist until written; this helper is the per-device write.
#
# What it does: clears only bits 1:0 (the ASPM L0s and L1 enable bits) of
# the 16-bit Link Control register (LnkCtl, the PCIe capability +0x10
# word) on the MT7922 endpoint (PCI 14c3:7922) and its immediate parent
# port, read-modify-write, all other bits preserved. Idempotent. No other
# device is ever touched. Link power-state enable bits only: no flash,
# eFuse, OTP or calibration writes.
#
# Usage:
#   sudo tools/aspm-off.sh off      apply (idempotent; auto-locates the MT7922)
#   tools/aspm-off.sh status        print the current LnkCtl words
#
# The setting holds until a reboot or a link retrain with the bits re-set;
# run `off` again after a reboot (or wire it into your boot process).

SETPCI=${SETPCI:-/usr/bin/setpci}

bdf_of() {		# sysfs dir -> BDF as setpci wants it (strip the 0000: domain prefix)
	basename "$1" | sed 's/^0000://'
}

find_endpoint() {		# resolved sysfs dir of the MT7922 (14c3:7922), or empty
	for d in /sys/bus/pci/devices/0000:*; do
		[ -r "$d/vendor" ] || continue
		if [ "$(cat "$d/vendor" 2>/dev/null)" = "0x14c3" ] && \
		   [ "$(cat "$d/device" 2>/dev/null)" = "0x7922" ]; then
			readlink -f "$d"
			return 0
		fi
	done
	return 1
}

apply_dev() {		# clear LnkCtl bits 1:0 (ASPM L0s/L1 enable) on one BDF; idempotent
	bdf=$1
	[ -n "$bdf" ] || return 0
	cur=$($SETPCI -s "$bdf" CAP_EXP+10.w 2>/dev/null) || {
		echo "aspm-off: cannot read LnkCtl on $bdf" >&2
		return 1
	}
	curnum=$(( 0x$cur ))
	wantnum=$(( curnum & ~0x3 ))
	if [ "$curnum" -eq "$wantnum" ]; then
		echo "$bdf LnkCtl $cur (ASPM L0s/L1 bits 1:0 already clear)"
	else
		$SETPCI -s "$bdf" CAP_EXP+10.w=$(printf '%04x' "$wantnum") >/dev/null || {
			echo "aspm-off: cannot write LnkCtl on $bdf (need root?)" >&2
			return 1
		}
		echo "$bdf LnkCtl $cur -> $(printf '%04x' "$wantnum") (cleared ASPM L0s/L1 enable bits 1:0)"
	fi
}

case "${1:-off}" in
	off)
		[ "$(id -u)" = 0 ] || { echo "aspm-off: needs root (PCIe config-space write)" >&2; exit 1; }
		d=$(find_endpoint) || { echo "aspm-off: MT7922 (14c3:7922) not present" >&2; exit 1; }
		apply_dev "$(bdf_of "$d")"
		apply_dev "$(bdf_of "$(dirname "$d")")"
		;;
	status)
		d=$(find_endpoint) || { echo "aspm-off: MT7922 (14c3:7922) not present" >&2; exit 1; }
		for x in "$d" "$(dirname "$d")"; do
			bdf=$(bdf_of "$x")
			printf '%s LnkCtl %s (bits 1:0 = ASPM L0s/L1 enable)\n' \
				"$bdf" "$($SETPCI -s "$bdf" CAP_EXP+10.w 2>/dev/null)"
		done
		;;
	*)
		echo "usage: $0 {off|status}" >&2
		exit 2
		;;
esac
