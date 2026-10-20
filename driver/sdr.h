/* SPDX-License-Identifier: BSD-3-Clause-Clear */
/* Copyright (c) 2026 unstablerobotgf */
/* MT7922 SDR mode: the interface between mt7921-common and mt7921e. */
#ifndef __MT7921_SDR_H
#define __MT7921_SDR_H

#include <linux/sizes.h>
#include <linux/skbuff.h>
#include <linux/wait.h>

struct mt792x_dev;

extern bool mt7921_sdr;

/* Sample events from the SDR program: connac2 MCU RXD, then 8 KiB of samples, then 4 bytes. */
#define SDR_EVT_EID		0xed
#define SDR_EVT_EXT_EID		0xf5
#define SDR_EVT_HDR		44
#define SDR_EVT_GAPS		8	/* program's overrun count so far */
#define SDR_EVT_SEQ		36
#define SDR_EVT_NONCE		40
#define SDR_EVT_PAYLOAD		SZ_8K

/* Single producer (MCU RX NAPI) and single consumer (read()); head/tail are free-running. */
struct mt7921_sdr_rx {
	u32 nonce, next_seq;
	u32 packets, bad_len, bad_nonce, seq_gaps, dev_gaps;
	u8 *ring;		/* power-of-two size; NULL drops every event */
	u32 ring_size, ring_head, ring_tail;
	u64 ring_drops;		/* bytes lost to a full ring */
	wait_queue_head_t ring_wq;
};

extern struct mt7921_sdr_rx mt7921_sdr_rx_state;

bool mt7921_sdr_rx(struct sk_buff *skb);
int mt7921e_sdr_boot(struct mt792x_dev *dev);
void mt7921e_sdr_shutdown(struct mt792x_dev *dev);

#endif
