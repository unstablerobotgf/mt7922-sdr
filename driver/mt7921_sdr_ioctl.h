/* SPDX-License-Identifier: ISC */
/* /dev/mt7921_sdr: MT7922 IQ streaming. read() returns raw little-endian 32-bit words:
 * I in bits 29:16, Q in bits 13:0, 14-bit two's complement with bits 1:0 always zero. */
#ifndef __MT7921_SDR_IOCTL_H
#define __MT7921_SDR_IOCTL_H

#include <linux/ioctl.h>
#include <linux/types.h>

struct mt7921_sdr_start {
	__u32 rate_msps;	/* 40, 80, 160 or 320; above 80 exceeds PCIe 2.0 x1 and is gapped */
	__u32 freq_mhz;		/* 2390..2500 */
};

struct mt7921_sdr_info {
	__u64 packets, bytes;
	__u64 drops;		/* bytes lost to a full host ring */
	__u32 seq_gaps;		/* events missing between the device and the ring */
	__u32 dev_gaps;		/* device overruns: ICAP lapped the DMA and the stream restarted */
	__u32 bad_len, streaming, rate_msps, freq_mhz;
	__u32 pad;
};

/* manual = 0 hands gain back to the PHY's AGC. Steps are approximate (measured). */
struct mt7921_sdr_gain {
	__u32 manual;
	__u32 lna;		/* 0..4, ~6 dB per step */
	__u32 lpf;		/* 0..9, ~2 dB per step */
	__s32 digital;		/* -32..31, ~1 dB per step */
};

#define MT7921_SDR_START	_IOW('S', 1, struct mt7921_sdr_start)
#define MT7921_SDR_STOP		_IO('S', 2)
#define MT7921_SDR_INFO		_IOR('S', 3, struct mt7921_sdr_info)
/* Stops and restarts a running stream around the change (a few ms gap). */
#define MT7921_SDR_GAIN		_IOW('S', 4, struct mt7921_sdr_gain)
/* Centre frequency in Hz, 2390..2500 MHz; retunes a running stream without stopping it. */
#define MT7921_SDR_TUNE		_IOW('S', 5, __u64)

#endif
