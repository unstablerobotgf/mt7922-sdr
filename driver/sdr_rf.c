// SPDX-License-Identifier: ISC
/* MT7922 SDR mode: A-die and PHY state. Every write is logged in an undo log first. */

#include "sdr_internal.h"
#include "sdr_stock_2g.h"

/* A-die SPI bridge (BirriaSDR): command/data/result at +0x10, or +0x50 for TOP (port 5).
 * Writing the data word launches the transfer. Port 1 = firmware bank 0, port 0 = bank 1. */
static int sdr_bridge(struct mt792x_dev *dev, u8 port, u16 reg, u32 *val, bool write)
{
	u32 cmd = SDR_RF_BRIDGE + (port == 5 ? 0x50 : 0x10);
	u32 busy = port == 5 ? BIT(5) : BIT(1), ctl;

	if (!mt76_poll(dev, SDR_RF_BRIDGE, busy, 0, 1000))
		return -ETIMEDOUT;
	ctl = mt76_rr(dev, cmd);
	mt76_wr(dev, cmd, (ctl & 0xffff0000) | port << 13 | reg | (write ? 0 : 0x1000));
	mt76_wr(dev, cmd + 4, write ? *val : 0);
	if (!mt76_poll(dev, SDR_RF_BRIDGE, busy, 0, 1000))
		return -ETIMEDOUT;
	if (!write)
		*val = mt76_rr(dev, cmd + 8);
	return 0;
}

void sdr_undo_note(struct mt7921_sdr *s, u8 kind, u8 port, u32 addr, u32 val)
{
	int i;

	for (i = 0; i < s->nundo; i++)
		if (s->undo[i].kind == kind && s->undo[i].port == port && s->undo[i].addr == addr)
			return;
	if (s->nundo < SDR_UNDO_MAX)
		s->undo[s->nundo++] = (struct sdr_undo){ kind, port, addr, val };
	else
		s->undo_full = true;	/* callers stop writing: this one can't be restored */
}

/* Newest first, so a register written twice ends at its original value. */
void sdr_undo_restore(struct mt7921_sdr *s)
{
	int i, rf = 0, mcu = 0, fail = 0;
	bool mcu_ok = true;

	for (i = s->nundo - 1; i >= 0; i--) {
		struct sdr_undo *u = &s->undo[i];
		u32 v = u->val;

		switch (u->kind) {
		case SDR_UNDO_RF:
			if (sdr_bridge(s->dev, u->port, u->addr, &v, true))
				fail++;
			else
				rf++;
			break;
		case SDR_UNDO_MCU:
			if (!mcu_ok) {
				fail++;
				break;
			}
			s->err = 0;
			sdr_mcu_op(s, u->addr, ~0u, v);
			if (s->err) {
				mcu_ok = false;	/* one timeout: the program is gone, stop asking */
				fail++;
			} else {
				mcu++;
			}
			break;
		}
	}
	dev_info(s->dev->mt76.dev, "sdr: undo: restored %d rf, %d mcu registers, %d failed\n",
		 rf, mcu, fail);
	s->nundo = 0;
}

u32 sdr_rf_rd(struct mt7921_sdr *s, u8 port, u16 reg)
{
	u32 v = 0;

	if (!s->err && sdr_bridge(s->dev, port, reg, &v, false)) {
		dev_err(s->dev->mt76.dev, "sdr: bridge timeout reading %u/%03x\n", port, reg);
		s->err = -ETIMEDOUT;
	}
	return v;
}

void sdr_rf_wr(struct mt7921_sdr *s, u8 port, u16 reg, u32 val)
{
	u32 before = sdr_rf_rd(s, port, reg);

	if (s->err)
		return;
	sdr_undo_note(s, SDR_UNDO_RF, port, reg, before);
	if (s->undo_full) {
		s->err = -ENOSPC;
		return;
	}
	if (sdr_bridge(s->dev, port, reg, &val, true)) {
		dev_err(s->dev->mt76.dev, "sdr: bridge timeout writing %u/%03x\n", port, reg);
		s->err = -ETIMEDOUT;
		return;
	}
	if (!s->quiet)
		dev_dbg(s->dev->mt76.dev, "sdr: rf %u/%03x %08x -> %08x\n", port, reg, before,
			sdr_rf_rd(s, port, reg));
}

static void sdr_rf_upd(struct mt7921_sdr *s, u8 port, u16 reg, u32 clear, u32 set)
{
	u32 v = sdr_rf_rd(s, port, reg);

	sdr_rf_wr(s, port, reg, (v & ~clear) | set);
}

/* PHY (0x83xxxxxx) read-modify-write through the program, before value noted for undo. */
static void sdr_phy_upd(struct mt7921_sdr *s, u32 addr, u32 clear, u32 set)
{
	u32 before = sdr_mcu_op(s, addr, 0, 0);

	if (s->err)
		return;
	sdr_undo_note(s, SDR_UNDO_MCU, 0, addr, before);
	if (s->undo_full) {
		s->err = -ENOSPC;
		return;
	}
	sdr_mcu_op(s, addr, clear, set);
}

/* The vendor firmware's own 2.4 GHz A-die state at 2437 MHz, read back from a stock driver in
 * monitor mode. BirriaSDR's analog constants are for another die and must not be used here. */
int sdr_rf_stock_band(struct mt7921_sdr *s)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(sdr_stock_2g) && !s->err; i++)
		sdr_rf_wr(s, sdr_stock_2g[i].port, sdr_stock_2g[i].addr, sdr_stock_2g[i].val);
	dev_info(s->dev->mt76.dev, "sdr: stock 2.4 GHz A-die state: %d of %zu registers (err %d)\n",
		 i, ARRAY_SIZE(sdr_stock_2g), s->err);
	return s->err;
}

/* Nearest channel's stock per-channel state, then the exact synthesizer word for hz:
 * word = hz * 2^21 / 60 MHz (reference 60 MHz, VCO at 2x, 20 fractional bits). */
int sdr_rf_tune(struct mt7921_sdr *s, u64 hz)
{
	int ch, i;
	u32 word;

	if (hz < 2390000000ULL || hz > 2500000000ULL)
		return -ERANGE;
	ch = clamp_t(int, DIV_ROUND_CLOSEST_ULL(hz, 1000000) - 2412 + 2, 0, 64) / 5;
	ch = min(ch, 12);
	s->err = 0;
	s->quiet = true;
	for (i = 0; i < ARRAY_SIZE(sdr_stock_ch) && !s->err; i++) {
		u16 a = sdr_stock_ch[i].addr;

		/* 0x258-0x268 are AGC state (snapshots caught mid-packet); 0xdac is a readback */
		if ((a >= 0x258 && a <= 0x268) || a == 0xdac)
			continue;
		if (sdr_stock_ch[i].port == 1 && (a == 0xc04 || a == 0xc08)) {
			/* [31:21] is a VCO band code, ~linear per channel: interpolate (and
			 * extrapolate past channels 1 and 11, the measured ends). */
			s64 pos = div_s64((s64)hz - 2412000000LL, 5000);	/* 1/1000 channel */
			int lo = clamp_t(int, div_s64(pos, 1000), 0, 9);
			u32 v0 = sdr_stock_ch[i].val[lo], v1 = sdr_stock_ch[i].val[lo + 1];
			s64 f0 = v0 >> 21, f1 = v1 >> 21;

			f0 += div_s64((f1 - f0) * (pos - lo * 1000), 1000);
			sdr_rf_wr(s, 1, a, (v0 & 0x1fffff) | (u32)clamp_t(s64, f0, 0, 0x7ff) << 21);
			continue;
		}
		sdr_rf_wr(s, sdr_stock_ch[i].port, a, sdr_stock_ch[i].val[ch]);
	}
	word = (u32)DIV_ROUND_CLOSEST_ULL(hz << 21, 60000000);
	sdr_rf_wr(s, 1, 0x184, (u32)div_u64(hz, 1000));		/* frequency in kHz */
	sdr_rf_wr(s, 1, 0xc6c, (u32)div_u64(hz, 125000) - 2);	/* calibration count */
	sdr_rf_wr(s, 1, 0xc28, 0x40000000 | word);
	sdr_rf_wr(s, 1, 0xc68, 0x80000000 | word);		/* bit 31 LOAD self-clears */
	s->quiet = false;
	if (s->err)
		return -EIO;
	s->hz = hz;
	return 0;
}

/* Vendor's 160 MHz digital front-end words (read back at 5250 MHz / 160 MHz). */
static const u32 sdr_wide_extras[SDR_WIDE_EXTRAS][2] = {
	{ 0x83080054, 0x00260000 }, { 0x83080058, 0x0014008f }, { 0x8308005c, 0x00000006 },
	{ 0x8308102c, 0xffff0000 }, { 0x83081040, 0x3a0a5110 }, { 0x830810c0, 0x0000fcfc },
	{ 0x830810c4, 0x0000fda0 }, { 0x830810c8, 0xd7dabada }, { 0x830810cc, 0x0000e1f4 },
	{ 0x830810d0, 0x00000090 }, { 0x830810d4, 0x00000006 },
};

/* Rate = 320 MHz >> tap divider (3/2/1/0 = 40/80/160/320 MSps). Filter width code w
 * (0..3 = 20/40/80/160 MHz) goes to the analog filter on both ports and the digital class. */
int sdr_rf_rate(struct mt7921_sdr *s, u32 rate)
{
	int div = rate == 40 ? 3 : rate == 80 ? 2 : rate == 160 ? 1 : rate == 320 ? 0 : -1;
	u32 w = div == 3 ? 1 : div == 2 ? 2 : 3;
	bool wide = div <= 1;
	int i;

	if (div < 0)
		return -EINVAL;
	s->err = 0;
	sdr_rf_upd(s, 0, 0x160, 0xc0000000, w << 30);
	sdr_rf_upd(s, 1, 0x160, 0xc0000000, w << 30);
	sdr_phy_upd(s, 0x83081000, 0x0f000000, w << 24);
	sdr_phy_upd(s, 0x83091000, 0x0f000000, w << 24);
	sdr_phy_upd(s, 0x830ad448, 0x000c0000, div << 18);
	if (wide != s->wide) {
		for (i = 0; i < SDR_WIDE_EXTRAS && !s->err; i++) {
			u32 a = sdr_wide_extras[i][0];

			if (wide)
				s->wide_saved[i] = sdr_mcu_op(s, a, 0, 0);
			sdr_phy_upd(s, a, ~0u, wide ? sdr_wide_extras[i][1] : s->wide_saved[i]);
		}
		s->wide = wide;
	}
	if (s->err)
		return -EIO;
	s->rate = rate;
	return 0;
}

/* Gain on both chains' CSD blocks (0x83000000 + chain << 16, the layout w1700k documents for
 * MT7996): +0xc4 forces LNA/LPF (sets A and B), +0x108 [29:24] digital, +0x150 force D,
 * +0xd0 [27:24] coarse-AGC force (kept cleared). Plus BirriaSDR's A-die 0x768 clamp. */
static const u16 sdr_gain_off[4] = { 0xc4, 0x108, 0x150, 0xd0 };
static const u32 sdr_gain_mask[4] = { 0x00f7f703, 0x3f000000, 0x8070f000, 0x0f000000 };

int sdr_rf_gain(struct mt7921_sdr *s, const struct mt7921_sdr_gain *g)
{
	u32 lna = g->lna, lpf = g->lpf, dig = (u32)g->digital & 63, v[4];
	int c, k;

	if (g->manual && (lna > 4 || lpf > 9 || g->digital < -32 || g->digital > 31))
		return -EINVAL;
	s->err = 0;
	/* AGC's own values, saved once before the first force, are what "auto" restores */
	for (c = 0; c < 2 && !s->gain_saved; c++)
		for (k = 0; k < 4; k++)
			s->gain_base[c][k] = sdr_mcu_op(s, 0x83000000 + (c << 16) + sdr_gain_off[k], 0, 0);
	s->gain_saved = !s->err;
	v[0] = 3 | lna << 8 | lpf << 12 | lna << 16 | lpf << 20;
	v[1] = dig << 24;
	v[2] = 0x80000000 | lna << 20;
	v[3] = 0;
	if (g->manual)		/* analog clamp first */
		sdr_rf_upd(s, 1, 0x768, 0xff800000, 0x80000000 | lna << 27 | lpf << 23);
	for (c = 0; c < 2; c++)
		for (k = 1; k < 5; k++) {	/* +0x108, +0x150, +0xd0, then +0xc4 last */
			int i = k % 4;
			u32 set = g->manual ? v[i] : s->gain_base[c][i] & sdr_gain_mask[i];

			if (!g->manual && !s->gain_saved)
				continue;
			sdr_phy_upd(s, 0x83000000 + (c << 16) + sdr_gain_off[i], sdr_gain_mask[i], set);
		}
	if (!g->manual)		/* release the clamp last */
		sdr_rf_upd(s, 1, 0x768, 0xff800000, 0);
	dev_info(s->dev->mt76.dev, "sdr: gain %s lna %u lpf %u digital %d (err %d)\n",
		 g->manual ? "manual" : "auto", lna, lpf, g->digital, s->err);
	return s->err ? -EIO : 0;
}
