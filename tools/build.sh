#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 unstablerobotgf
# Build mt7921-common.ko and mt7921e.ko with SDR mode, out of tree, against the running kernel.
#   tools/build.sh MT76_SRC [KDIR]
# MT76_SRC: drivers/net/wireless/mediatek/mt76 from the source of the running kernel version.
# KDIR: kernel build directory (default /lib/modules/$(uname -r)/build).
# NDS32_CROSS: binutils prefix for the firmware (default nds32le-elf-), or NDS32_AS/LD/OBJCOPY.
# KMAKE_ARGS: extra kbuild arguments, e.g. "CONFIG_DEBUG_INFO_BTF_MODULES=" without pahole.
set -euo pipefail
here=$(cd "$(dirname "$0")/.." && pwd)
src=${1:?usage: build.sh MT76_SRC [KDIR]}
kdir=${2:-/lib/modules/$(uname -r)/build}
out=$here/build/mt76

rm -rf "$out"
mkdir -p "$(dirname "$out")"
cp -r "$src" "$out"
patch -d "$out/.." -p5 --quiet < "$here/driver/mt76-sdr.patch" ||
	{ echo "mt76-sdr.patch does not apply to $src"; exit 1; }
cp "$here"/driver/{sdr.h,sdr_internal.h,sdr_rx.c,sdr_mcu.c,sdr_rf.c,sdr_stream.c,sdr_stock_2g.h,mt7921_sdr_ioctl.h} \
	"$out/mt7921/"
python3 "$here/firmware/build_fw.py" "$here/firmware/sdr_fw.S" "$out/mt7921/sdr_fw.h" \
	--cross "${NDS32_CROSS:-nds32le-elf-}" ${NDS32_AS:+--as "$NDS32_AS"} \
	${NDS32_LD:+--ld "$NDS32_LD"} ${NDS32_OBJCOPY:+--objcopy "$NDS32_OBJCOPY"}
make -C "$kdir" M="$out/mt7921" modules KBUILD_MODPOST_WARN=1 ${KMAKE_ARGS:-} -j"$(nproc)"
sync	# a power loss right after a build has left zero-length .ko files
ls -l "$out/mt7921/mt7921-common.ko" "$out/mt7921/mt7921e.ko"
