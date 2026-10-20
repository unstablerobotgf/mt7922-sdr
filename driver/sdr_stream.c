// SPDX-License-Identifier: BSD-3-Clause-Clear
/* Copyright (c) 2026 unstablerobotgf */
/* MT7922 SDR mode: /dev/mt7921_sdr and the stream command. */

#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/vmalloc.h>
#include "sdr_internal.h"
#include "sdr_fw.h"

static struct mt7921_sdr *sdr_svc;	/* for mt7921e_sdr_shutdown(); one device at a time */

static int sdr_stream_start(struct mt7921_sdr *s, u32 rate, u64 hz)
{
	struct mt7921_sdr_rx *r = &mt7921_sdr_rx_state;
	struct mt792x_dev *dev = s->dev;
	u32 mb = SDR_MAILBOX;
	int ret;

	if (s->streaming)
		return -EBUSY;
	ret = sdr_rf_rate(s, rate);
	if (!ret && hz != s->hz)
		ret = sdr_rf_tune(s, hz);
	if (ret)
		return ret;
	r->next_seq = 0;
	r->packets = r->bad_len = r->bad_nonce = r->seq_gaps = r->dev_gaps = 0;
	r->ring_head = r->ring_tail = 0;
	r->ring_drops = 0;
	mt76_wr(dev, SDR_STOPREG, 0);
	mt76_wr(dev, mb + SDR_MB_LIMIT, 0);	/* until STOP */
	/* Poll guard: past it the program can't prove ICAP didn't lap the DMA; ~half a ring. */
	mt76_wr(dev, mb + SDR_MB_GUARD, rate == 40 ? 750 : rate == 80 ? 375 : rate == 160 ? 190 : 95);
	s->done0 = mt76_rr(dev, mb + SDR_MB_DONE);
	smp_store_release(&s->streaming, true);
	mt76_wr(dev, mb + SDR_MB_CMD, SDR_CMD_STREAM);
	dev_info(dev->mt76.dev, "sdr: stream started, %u MSps at %llu Hz\n", rate, hz);
	return 0;
}

static int sdr_stream_stop(struct mt7921_sdr *s)
{
	struct mt7921_sdr_rx *r = &mt7921_sdr_rx_state;
	struct mt792x_dev *dev = s->dev;
	unsigned long timeout = jiffies + msecs_to_jiffies(3000);
	u32 mb = SDR_MAILBOX, res[8];
	int i;

	if (!s->streaming)
		return 0;
	/* The MCU polls STOPREG between packets; SYSRAM is unreadable until it releases the grant. */
	mt76_wr(dev, SDR_STOPREG, SDR_STOPMAGIC);
	while (mt76_rr(dev, mb + SDR_MB_CANARY) != SDR_CANARY ||
	       mt76_rr(dev, mb + SDR_MB_DONE) != s->done0 + 1) {
		if (time_after(jiffies, timeout)) {
			dev_err(dev->mt76.dev, "sdr: stream did not stop: grant may still be held\n");
			return -EIO;
		}
		msleep(5);
	}
	mt76_wr(dev, SDR_STOPREG, 0);
	smp_store_release(&s->streaming, false);
	wake_up_interruptible(&r->ring_wq);
	for (i = 0; i < 8; i++)
		res[i] = mt76_rr(dev, mb + SDR_MB_RESULT + 4 * i);
	dev_info(dev->mt76.dev,
		 "sdr: stream stopped (state %u): %u packets, %u device gaps, max backlog %u, max poll %u us, DMA err %#x; host %u packets, %u seq gaps, %llu B dropped\n",
		 res[4], res[0], res[1], res[2], res[3], res[5], r->packets, r->seq_gaps,
		 r->ring_drops);
	return 0;
}

static int sdr_fopen(struct inode *inode, struct file *f)
{
	struct mt7921_sdr *s = container_of(f->private_data, struct mt7921_sdr, misc);
	int ret = 0;

	mutex_lock(&s->lock);
	if (!s->up)
		ret = -ENODEV;
	else if (s->open)
		ret = -EBUSY;		/* one reader: the ring has a single tail */
	else
		s->open = true;
	mutex_unlock(&s->lock);
	return ret;
}

static int sdr_frelease(struct inode *inode, struct file *f)
{
	struct mt7921_sdr *s = container_of(f->private_data, struct mt7921_sdr, misc);

	mutex_lock(&s->lock);
	sdr_stream_stop(s);
	s->open = false;
	mutex_unlock(&s->lock);
	return 0;
}

/* Blocks until samples arrive; returns 0 once a stopped stream is drained. */
static ssize_t sdr_fread(struct file *f, char __user *buf, size_t count, loff_t *ppos)
{
	struct mt7921_sdr *s = container_of(f->private_data, struct mt7921_sdr, misc);
	struct mt7921_sdr_rx *r = &mt7921_sdr_rx_state;
	u32 tail = r->ring_tail, avail, off, first, n;
	int ret;

	ret = wait_event_interruptible(r->ring_wq, smp_load_acquire(&r->ring_head) != tail ||
				       !smp_load_acquire(&s->streaming));
	if (ret)
		return ret;
	mutex_lock(&s->lock);		/* shutdown frees the ring under the same lock */
	avail = smp_load_acquire(&r->ring_head) - tail;
	n = min_t(u32, avail, count & ~3u);
	if (!s->up) {
		ret = -ENODEV;
	} else if (n) {
		off = tail & (r->ring_size - 1);
		first = min(n, r->ring_size - off);
		if (copy_to_user(buf, r->ring + off, first) ||
		    copy_to_user(buf + first, r->ring, n - first))
			ret = -EFAULT;
		else
			smp_store_release(&r->ring_tail, tail + n);
	}
	mutex_unlock(&s->lock);
	return ret ? ret : n;
}

static long sdr_fioctl(struct file *f, unsigned int cmd, unsigned long arg)
{
	struct mt7921_sdr *s = container_of(f->private_data, struct mt7921_sdr, misc);
	struct mt7921_sdr_rx *r = &mt7921_sdr_rx_state;
	void __user *uarg = (void __user *)arg;
	struct mt7921_sdr_start st;
	struct mt7921_sdr_gain g;
	struct mt7921_sdr_info info = {};
	bool was;
	long ret = 0;
	u64 hz;

	mutex_lock(&s->lock);
	switch (cmd) {
	case MT7921_SDR_START:
		if (copy_from_user(&st, uarg, sizeof(st)))
			ret = -EFAULT;
		else
			ret = sdr_stream_start(s, st.rate_msps, (u64)st.freq_mhz * 1000000);
		break;
	case MT7921_SDR_STOP:
		ret = sdr_stream_stop(s);
		break;
	case MT7921_SDR_TUNE:
		/* A-die only, through the host's bridge: safe while the MCU streams */
		if (copy_from_user(&hz, uarg, sizeof(hz)))
			ret = -EFAULT;
		else
			ret = sdr_rf_tune(s, hz);
		break;
	case MT7921_SDR_GAIN:
		/* PHY words need MCU ops, and the MCU is busy streaming: stop around them */
		if (copy_from_user(&g, uarg, sizeof(g))) {
			ret = -EFAULT;
			break;
		}
		was = s->streaming;
		if (was)
			ret = sdr_stream_stop(s);
		if (!ret)
			ret = sdr_rf_gain(s, &g);
		if (was && !ret)
			ret = sdr_stream_start(s, s->rate, s->hz);
		break;
	case MT7921_SDR_INFO:
		info.packets = r->packets;
		info.bytes = (u64)r->packets * SDR_EVT_PAYLOAD;
		info.drops = r->ring_drops;
		info.seq_gaps = r->seq_gaps;
		info.dev_gaps = r->dev_gaps;
		info.bad_len = r->bad_len;
		info.streaming = s->streaming;
		info.rate_msps = s->rate;
		info.freq_mhz = (u32)DIV_ROUND_CLOSEST_ULL(s->hz, 1000000);
		if (copy_to_user(uarg, &info, sizeof(info)))
			ret = -EFAULT;
		break;
	default:
		ret = -ENOTTY;
	}
	mutex_unlock(&s->lock);
	return ret;
}

static const struct file_operations sdr_fops = {
	.owner = THIS_MODULE,
	.open = sdr_fopen,
	.release = sdr_frelease,
	.read = sdr_fread,
	.unlocked_ioctl = sdr_fioctl,
	.llseek = noop_llseek,
};

int sdr_service_setup(struct mt7921_sdr *s)
{
	struct mt7921_sdr_rx *r = &mt7921_sdr_rx_state;
	u8 *ring;
	int ret;

	init_waitqueue_head(&r->ring_wq);
	ring = vzalloc(SDR_RING_SIZE);
	if (!ring)
		return -ENOMEM;
	r->ring_size = SDR_RING_SIZE;
	smp_wmb();
	WRITE_ONCE(r->ring, ring);

	s->misc.minor = MISC_DYNAMIC_MINOR;
	s->misc.name = "mt7921_sdr";
	s->misc.fops = &sdr_fops;
	s->misc.mode = 0666;
	ret = misc_register(&s->misc);
	if (ret) {
		WRITE_ONCE(r->ring, NULL);
		vfree(ring);
		return ret;
	}
	sdr_svc = s;
	s->up = true;
	dev_info(s->dev->mt76.dev, "sdr: up: /dev/mt7921_sdr\n");
	return 0;
}

/* From mt7921_pci_remove(), before the host DMA rings go away. */
void mt7921e_sdr_shutdown(struct mt792x_dev *dev)
{
	struct mt7921_sdr_rx *r = &mt7921_sdr_rx_state;
	struct mt7921_sdr *s = sdr_svc;
	u8 *ring;

	if (!s || !s->up)
		return;
	misc_deregister(&s->misc);
	mutex_lock(&s->lock);
	sdr_stream_stop(s);
	s->up = false;
	ring = r->ring;
	WRITE_ONCE(r->ring, NULL);
	msleep(50);		/* let a NAPI pass still copying a late event finish */
	vfree(ring);
	sdr_undo_restore(s);
	mutex_unlock(&s->lock);
	wake_up_interruptible(&r->ring_wq);
	sdr_svc = NULL;
}
