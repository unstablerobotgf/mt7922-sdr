/* SPDX-License-Identifier: BSD-3-Clause-Clear */
/* Copyright (c) 2026 unstablerobotgf */
/* MT7922 SDR mode: state and helpers shared by sdr_mcu.c, sdr_rf.c and sdr_stream.c. */
#ifndef __MT7921_SDR_INTERNAL_H
#define __MT7921_SDR_INTERNAL_H

#include <linux/miscdevice.h>
#include <linux/mutex.h>
#include "mt7921.h"
#include "mt7921_sdr_ioctl.h"

/* Host-reachable addresses; mt76_rr/mt76_wr remap them into BAR0. */
#define SDR_PDA_STATUS		0x54000808	/* 0x25 after a complete download */
#define SDR_PDA_ERROR		0x54000814
#define SDR_RF_BRIDGE		0x7c004000	/* A-die SPI bridge; port 5 is TOP */

#define SDR_CANARY		0x5a17c0de	/* host-only mailbox word, see sdr_cmd() */
#define SDR_UNSET		0xa5a5a5a5
#define SDR_MAX_OPS		20
#define SDR_CMD_OPS		1
#define SDR_CMD_STREAM		2

/* Identity gate: the A-die revision and 40 MHz crystal trim the stock tables were taken on. */
#define SDR_DIE_ID		0x79228a00	/* TOP 0x02c */
#define SDR_XTAL_40M		0xd4		/* A-die port 1 0xe1c[9:0]; 0x113 = 52 MHz */

#define SDR_RING_SIZE		SZ_64M
#define SDR_UNDO_MAX		1024		/* ~460 stock overlay registers plus tuning and gain */
#define SDR_WIDE_EXTRAS		11

enum sdr_undo_kind { SDR_UNDO_RF, SDR_UNDO_MCU };	/* A-die bridge, or a program op */

struct sdr_undo {
	u8 kind, port;
	u32 addr, val;
};

struct mt7921_sdr {
	struct mt792x_dev *dev;
	int err;			/* first failure; every later register access is skipped */
	bool quiet;			/* no readback or log line per A-die write */

	struct sdr_undo undo[SDR_UNDO_MAX];	/* first "before" of every register written */
	int nundo;
	bool undo_full;

	u64 hz;
	u32 rate;
	bool wide;			/* 160 MHz digital extras applied */
	u32 wide_saved[SDR_WIDE_EXTRAS];
	bool gain_saved;
	u32 gain_base[2][4];		/* per chain: PHY +0xc4, +0x108, +0x150, +0xd0 */

	struct miscdevice misc;
	struct mutex lock;		/* serialises ioctls, open and shutdown */
	bool up, open, streaming;
	u32 done0;			/* MB_DONE when the stream command was issued */
};

/* sdr_mcu.c */
bool sdr_cmd(struct mt7921_sdr *s, u32 cmd, unsigned int ms);
u32 sdr_mcu_op(struct mt7921_sdr *s, u32 addr, u32 clear, u32 set);
int sdr_mcu_ops(struct mt7921_sdr *s, const u32 (*ops)[3], int n);

/* sdr_rf.c */
u32 sdr_rf_rd(struct mt7921_sdr *s, u8 port, u16 reg);
void sdr_rf_wr(struct mt7921_sdr *s, u8 port, u16 reg, u32 val);
void sdr_undo_note(struct mt7921_sdr *s, u8 kind, u8 port, u32 addr, u32 val);
void sdr_undo_restore(struct mt7921_sdr *s);
int sdr_rf_stock_band(struct mt7921_sdr *s);
int sdr_rf_tune(struct mt7921_sdr *s, u64 hz);
int sdr_rf_rate(struct mt7921_sdr *s, u32 rate);
int sdr_rf_gain(struct mt7921_sdr *s, const struct mt7921_sdr_gain *g);

/* sdr_stream.c */
int sdr_service_setup(struct mt7921_sdr *s);

#endif
