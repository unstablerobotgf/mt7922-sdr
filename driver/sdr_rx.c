// SPDX-License-Identifier: BSD-3-Clause-Clear
/* Copyright (c) 2026 unstablerobotgf */
/* MT7922 SDR mode: module parameter and the sample-event consumer (mt7921-common). */

#include <linux/module.h>
#include <linux/unaligned.h>
#include "mt7921.h"

bool mt7921_sdr;
module_param_named(sdr, mt7921_sdr, bool, 0444);
MODULE_PARM_DESC(sdr, "boot the SDR program instead of the vendor firmware (MT7922 PCIe only)");
EXPORT_SYMBOL_GPL(mt7921_sdr);

struct mt7921_sdr_rx mt7921_sdr_rx_state;
EXPORT_SYMBOL_GPL(mt7921_sdr_rx_state);

/* Consume a sample event into the ring; anything else goes back to the normal event path. */
bool mt7921_sdr_rx(struct sk_buff *skb)
{
	struct mt7921_sdr_rx *r = &mt7921_sdr_rx_state;
	struct mt76_connac2_mcu_rxd *rxd;
	u32 seq, len, head, off, first;
	u8 *ring;

	if (skb_linearize(skb) || skb->len < SDR_EVT_HDR)
		return false;
	rxd = (struct mt76_connac2_mcu_rxd *)skb->data;
	if (rxd->eid != SDR_EVT_EID || rxd->ext_eid != SDR_EVT_EXT_EID)
		return false;

	seq = get_unaligned_le32(skb->data + SDR_EVT_SEQ);
	r->packets++;
	if (skb->len != SDR_EVT_HDR + SDR_EVT_PAYLOAD + 4)
		r->bad_len++;
	if (get_unaligned_le32(skb->data + SDR_EVT_NONCE) != r->nonce)
		r->bad_nonce++;
	if (seq != r->next_seq)
		r->seq_gaps++;
	r->next_seq = seq + 1;
	r->dev_gaps = max(r->dev_gaps, get_unaligned_le32(skb->data + SDR_EVT_GAPS));

	len = min_t(u32, skb->len - SDR_EVT_HDR, SDR_EVT_PAYLOAD) & ~3u;
	head = r->ring_head;
	ring = READ_ONCE(r->ring);	/* shutdown clears it, then waits before freeing */
	if (!ring || r->ring_size - (head - smp_load_acquire(&r->ring_tail)) < len) {
		r->ring_drops += len;
	} else {
		off = head & (r->ring_size - 1);
		first = min(len, r->ring_size - off);
		memcpy(ring + off, skb->data + SDR_EVT_HDR, first);
		memcpy(ring, skb->data + SDR_EVT_HDR + first, len - first);
		smp_store_release(&r->ring_head, head + len);
		wake_up_interruptible(&r->ring_wq);
	}
	dev_kfree_skb(skb);
	return true;
}
