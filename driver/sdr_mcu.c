// SPDX-License-Identifier: BSD-3-Clause-Clear
/* Copyright (c) 2026 unstablerobotgf */
/* Copyright (C) 2021 MediaTek Inc. (boot sequence after mt7921e pci_mcu.c) */
/* MT7922 SDR mode: boot the SDR program from the ROM, mailbox commands, radio bring-up. */

#include <linux/random.h>
#include "sdr_internal.h"
#include "mcu.h"
#include "sdr_fw.h"

static struct mt7921_sdr sdr_instance;

/* Issue one mailbox command and wait for the program to finish it. */
bool sdr_cmd(struct mt7921_sdr *s, u32 cmd, unsigned int ms)
{
	struct mt792x_dev *dev = s->dev;
	u32 mb = SDR_MAILBOX, done = mt76_rr(dev, mb + SDR_MB_DONE);
	unsigned int i;

	mt76_wr(dev, mb + SDR_MB_CMD, cmd);
	for (i = 0; i < ms / 10; i++) {
		/* Under the ICAP grant a SYSRAM read returns a stale bus value; the canary,
		 * read last, vouches for the two reads before it. */
		u32 n = mt76_rr(dev, mb + SDR_MB_DONE), state = mt76_rr(dev, mb + SDR_MB_STATE);

		if (mt76_rr(dev, mb + SDR_MB_CANARY) != SDR_CANARY) {
			msleep(10);
			continue;
		}
		if (n == done + 1)
			return true;
		if (state != 1)
			break;
		msleep(10);
	}
	dev_err(dev->mt76.dev, "sdr: command %u not done (state %#x, phase %#x, canary %s)\n",
		cmd, mt76_rr(dev, mb + SDR_MB_STATE), mt76_rr(dev, mb + SDR_MB_PHASE),
		mt76_rr(dev, mb + SDR_MB_CANARY) == SDR_CANARY ? "ok" : "lost");
	return false;
}

/* One read-modify-write by the MCU (0x83/0x88 space is not host-reachable); clear == set == 0 only reads. */
u32 sdr_mcu_op(struct mt7921_sdr *s, u32 addr, u32 clear, u32 set)
{
	u32 op = SDR_MAILBOX + SDR_MB_OPS;

	if (s->err)
		return 0;
	mt76_wr(s->dev, op, addr);
	mt76_wr(s->dev, op + 4, clear);
	mt76_wr(s->dev, op + 8, set);
	mt76_wr(s->dev, op + 20, 0);
	if (!sdr_cmd(s, SDR_CMD_OPS, 500)) {
		s->err = -EIO;
		return 0;
	}
	return mt76_rr(s->dev, op + 12);
}

/* A list of {addr, clear, set} in one command, every "before" noted for the undo log. */
int sdr_mcu_ops(struct mt7921_sdr *s, const u32 (*ops)[3], int n)
{
	u32 op = SDR_MAILBOX + SDR_MB_OPS;
	int i;

	if (WARN_ON(n > SDR_MAX_OPS) || s->err)
		return -EINVAL;
	for (i = 0; i < n; i++) {
		mt76_wr(s->dev, op + 20 * i, ops[i][0]);
		mt76_wr(s->dev, op + 20 * i + 4, ops[i][1]);
		mt76_wr(s->dev, op + 20 * i + 8, ops[i][2]);
	}
	mt76_wr(s->dev, op + 20 * i, 0);
	if (!sdr_cmd(s, SDR_CMD_OPS, 1000)) {
		s->err = -EIO;
		return s->err;
	}
	for (i = 0; i < n; i++) {
		u32 before = mt76_rr(s->dev, op + 20 * i + 16);

		sdr_undo_note(s, SDR_UNDO_MCU, 0, ops[i][0], before);
		dev_dbg(s->dev->mt76.dev, "sdr: op %08x &~%08x |%08x: %08x -> %08x\n",
			ops[i][0], ops[i][1], ops[i][2], before, mt76_rr(s->dev, op + 20 * i + 12));
	}
	return 0;
}

/* ICAP and digital front-end setup (BirriaSDR's capture configuration), ICAP left stopped.
 * The grant (0x88009004) is not here: the stream command holds it for the whole stream. */
static const u32 sdr_capture_ops[][3] = {
	{ 0x80021090, ~0u, 0xf1 },		/* ICAP CR0 */
	{ 0x80021098, ~0u, 0 },			/* CR2 ring start */
	{ 0x8002109c, ~0u, 0xfffc },		/* CR3 ring end: 256 KiB */
	{ 0x800210b8, 0xf0000, 0x9c000000 },	/* CR10, plain 32-bit words */
	{ 0x83080004, 0, 0x3840 },		/* chain 0 PHY clock gates */
	{ 0x83080008, 0, 0x3840 },
	{ 0x83090004, 0, 0x3840 },		/* chain 1 */
	{ 0x83090008, 0, 0x3840 },
	{ 0x830ad440, 0xff, 0x80000013 },	/* source: dense ADC samples */
	{ 0x830ad448, 0x0fffffff, 0x008c0400 },	/* tap 4 = chain 0, divider 3 = 40 MSps */
	{ 0x830a1000, ~0u, 0x04090100 },
	{ 0x830a1004, 0xff, 0x10 },
	{ 0x830a1008, ~0u, 0 },
	{ 0x830a3008, 3 << 21, 0 },
	{ 0x820e3000, 0x200, 0x100 },		/* receive-only MAC clocks */
	{ 0x80021090, ~0u, 0x4f1 },		/* stopped, cursor reset */
};

/* The ROM pointers the vendor calls around a grant change must still point into ROM. */
static bool sdr_rom_hooks_ok(struct mt7921_sdr *s)
{
	static const u32 ptrs[] = { 0x00821b4c, 0x00821b50, 0x00821b7c };
	u32 v[ARRAY_SIZE(ptrs)];
	int i;

	for (i = 0; i < ARRAY_SIZE(ptrs); i++) {
		v[i] = sdr_mcu_op(s, ptrs[i], 0, 0);
		if (s->err || v[i] < 0x800000 || v[i] >= 0x840000) {
			dev_err(s->dev->mt76.dev, "sdr: ROM hook %08x -> %08x is not in ROM\n",
				ptrs[i], v[i]);
			return false;
		}
	}
	return true;
}

static int sdr_bringup(struct mt7921_sdr *s)
{
	struct device *d = s->dev->mt76.dev;
	u32 die = sdr_rf_rd(s, 5, 0x02c), xtal = sdr_rf_rd(s, 1, 0xe1c);
	int ret;

	if (s->err || die != SDR_DIE_ID || (xtal & 0x3ff) != SDR_XTAL_40M) {
		dev_err(d, "sdr: die %08x crystal trim %08x: not the verified 40 MHz MT7922, no writes\n",
			die, xtal);
		return -ENODEV;
	}
	if (!sdr_rom_hooks_ok(s))
		return -ENODEV;
	mt76_wr(s->dev, SDR_MAILBOX + SDR_MB_HOOKS, 1);

	ret = sdr_mcu_ops(s, sdr_capture_ops, ARRAY_SIZE(sdr_capture_ops));
	if (!ret)
		ret = sdr_rf_stock_band(s);
	if (!ret)
		ret = sdr_rf_tune(s, 2437000000ULL);
	if (!ret)
		ret = sdr_rf_rate(s, 40);
	if (!ret)
		ret = sdr_service_setup(s);
	if (ret) {
		dev_err(d, "sdr: bring-up failed (%d), restoring\n", ret);
		sdr_undo_restore(s);
	}
	return ret;
}

int mt7921e_sdr_boot(struct mt792x_dev *dev)
{
	struct {
		__le32 option;
		__le32 addr;
	} start = {
		.option = cpu_to_le32(FW_START_OVERRIDE),
		.addr = cpu_to_le32(SDR_ENTRY),
	};
	struct mt7921_sdr *s = &sdr_instance;
	struct mt76_dev *mdev = &dev->mt76;
	u32 mb = SDR_MAILBOX, nonce;
	int i, ret;

	if (s->up)
		return -EBUSY;
	memset(s, 0, sizeof(*s));
	s->dev = dev;
	mutex_init(&s->lock);

	mt76_connac_mcu_restart(mdev);
	if (!mt76_poll_msec(dev, MT_CONN_ON_MISC, MT_TOP_MISC_FW_STATE,
			    MT_TOP_MISC2_FW_PWR_ON, 1000))
		dev_warn(mdev->dev, "sdr: ROM not ready for download\n");

	/* A retained vendor ROM patch would still hook the ROM functions we call. */
	ret = mt76_connac_mcu_patch_sem_ctrl(mdev, true);
	if (ret != PATCH_NOT_DL_SEM_SUCCESS) {
		dev_err(mdev->dev, "sdr: ROM patch present or semaphore busy (%d)\n", ret);
		return -EBUSY;
	}
	ret = mt76_connac_mcu_patch_sem_ctrl(mdev, false);
	if (ret != PATCH_REL_SEM_SUCCESS) {
		dev_err(mdev->dev, "sdr: patch semaphore release = %d\n", ret);
		return -EBUSY;
	}

	/* Clear the mailbox, verifying each word: SYSRAM must be host-reachable before boot. */
	get_random_bytes(&nonce, sizeof(nonce));
	for (i = 0; i < 64; i++) {
		u32 want = i * 4 == SDR_MB_NONCE ? nonce :
			   i * 4 == SDR_MB_POWER_ON ? SDR_UNSET :
			   i * 4 == SDR_MB_CANARY ? SDR_CANARY : 0;

		mt76_wr(dev, mb + 4 * i, want);
		if (mt76_rr(dev, mb + 4 * i) != want) {
			dev_err(mdev->dev, "sdr: mailbox +%#x readback failed\n", 4 * i);
			return -EIO;
		}
	}

	ret = mt76_connac_mcu_init_download(mdev, SDR_ENTRY, sizeof(sdr_fw_image),
					    DL_MODE_NEED_RSP);
	if (!ret)
		ret = mt76_mcu_send_firmware(mdev, MCU_CMD(FW_SCATTER), sdr_fw_image,
					     sizeof(sdr_fw_image));
	if (ret) {
		dev_err(mdev->dev, "sdr: download = %d\n", ret);
		return ret;
	}
	msleep(10);
	dev_info(mdev->dev, "sdr: PDA %08x %08x, %zu bytes to %08x\n",
		 mt76_rr(dev, SDR_PDA_STATUS), mt76_rr(dev, SDR_PDA_ERROR),
		 sizeof(sdr_fw_image), SDR_ENTRY);

	mt7921_sdr_rx_state.nonce = nonce;
	/* The ROM jumps without an ACK, so don't wait for one. */
	mt76_mcu_send_msg(mdev, MCU_CMD(FW_START_REQ), &start, sizeof(start), false);
	for (i = 0; i < 300 && mt76_rr(dev, mb + SDR_MB_MAGIC) != SDR_BOOT_MAGIC; i++)
		msleep(10);

	if (mt76_rr(dev, mb + SDR_MB_MAGIC) != SDR_BOOT_MAGIC ||
	    mt76_rr(dev, mb + SDR_MB_RESPONSE) != (nonce ^ SDR_RESPONSE_XOR) ||
	    mt76_rr(dev, mb + SDR_MB_STATE) != 1) {
		dev_err(mdev->dev, "sdr: program did not start: magic %08x response %08x state %#x\n",
			mt76_rr(dev, mb + SDR_MB_MAGIC), mt76_rr(dev, mb + SDR_MB_RESPONSE),
			mt76_rr(dev, mb + SDR_MB_STATE));
		return -ENODEV;
	}
	dev_info(mdev->dev, "sdr: program running, PHY power-on returned %#x\n",
		 mt76_rr(dev, mb + SDR_MB_POWER_ON));

	return sdr_bringup(s);
}
