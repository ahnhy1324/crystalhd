/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Disabled-by-default, privileged RX lifecycle observer for Flea research. */
#include <linux/ktime.h>
#include <linux/rcupdate.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>

#include "crystalhd_lnx.h"
#include "crystalhd_rx_receipt.h"
#include "../../include/crystalhd_fw_research.h"

struct crystalhd_rx_receipt_store {
	spinlock_t lock;
	struct crystalhd_fw_research_rx_receipt *records;
	u64 run_id;
	u32 capacity;
	u32 count;
	u32 overflow;
	u32 active[DMA_ENGINE_CNT];
	bool armed;
	bool sealed;
};

static struct crystalhd_rx_receipt_store *
crystalhd_rx_receipt_store(struct crystalhd_hw *hw)
{
	if (!hw || !hw->adp)
		return NULL;
	return rcu_dereference(hw->adp->rx_receipts);
}

static void
crystalhd_rx_receipt_retire(struct crystalhd_rx_receipt_store *store)
{
	if (!store)
		return;
	/* Every lookup-to-last-dereference interval, including sealed READ's
	 * memcpy, is an RCU read-side critical section.  Waiting here also covers
	 * a shared IRQ that entered after the ioctl synchronized its own IRQ.
	 */
	synchronize_rcu();
	kvfree(store->records);
	kfree(store);
}

static struct crystalhd_fw_research_rx_receipt *
crystalhd_rx_receipt_record_locked(struct crystalhd_rx_receipt_store *store,
					   struct crystalhd_rx_dma_pkt *pkt)
{
	u32 index;

	if (!store || !pkt || pkt->receipt_run_id != store->run_id ||
	    !pkt->receipt_index_plus_one)
		return NULL;
	index = pkt->receipt_index_plus_one - 1;
	if (index >= store->count || index >= store->capacity)
		return NULL;
	return &store->records[index];
}

static struct crystalhd_fw_research_rx_receipt *
crystalhd_rx_receipt_key_locked(struct crystalhd_rx_receipt_store *store,
				const struct crystalhd_rx_receipt_key *key)
{
	u32 index;

	if (!store || !key || key->run_id != store->run_id ||
	    !key->index_plus_one)
		return NULL;
	index = key->index_plus_one - 1;
	if (index >= store->count || index >= store->capacity)
		return NULL;
	return &store->records[index];
}

int crystalhd_rx_receipt_arm(struct crystalhd_adp *adp,
	struct crystalhd_fw_research_rx_receipt_arm *request)
{
	struct crystalhd_rx_receipt_store *old, *store;
	u64 run_id;

	if (!adp || !request || !request->capacity ||
	    request->capacity > CRYSTALHD_FW_RESEARCH_RX_RECEIPT_MAX_RECORDS)
		return -EINVAL;
	store = kzalloc(sizeof(*store), GFP_KERNEL);
	if (!store)
		return -ENOMEM;
	store->records = kvcalloc(request->capacity,
				  sizeof(*store->records), GFP_KERNEL);
	if (!store->records) {
		kfree(store);
		return -ENOMEM;
	}
	spin_lock_init(&store->lock);
	/* The exclusive device/user-lock contract serializes pointer writers. */
	old = rcu_dereference_protected(adp->rx_receipts, 1);
	if (old) {
		/* Never discard a live or merely armed run. */
		if (!READ_ONCE(old->sealed)) {
			kvfree(store->records);
			kfree(store);
			return -EBUSY;
		}
	}
	if (adp->rx_receipt_last_run_id == U64_MAX) {
		kvfree(store->records);
		kfree(store);
		return -EOVERFLOW;
	}
	run_id = adp->rx_receipt_last_run_id + 1;
	store->run_id = run_id;
	store->capacity = request->capacity;
	store->armed = true;
	adp->rx_receipt_last_run_id = run_id;
	rcu_assign_pointer(adp->rx_receipts, store);
	crystalhd_rx_receipt_retire(old);
	request->generation = adp->generation;
	request->run_id = run_id;
	return 0;
}

int crystalhd_rx_receipt_cancel_empty_arm(struct crystalhd_adp *adp,
	u64 generation, u64 run_id)
{
	struct crystalhd_rx_receipt_store *store;
	unsigned long flags;
	u32 i;
	int rc = 0;

	if (!adp || !generation || !run_id)
		return -EINVAL;
	/* The exclusive device/user-lock contract serializes pointer writers. */
	store = rcu_dereference_protected(adp->rx_receipts, 1);
	if (!store)
		return -ENOENT;
	spin_lock_irqsave(&store->lock, flags);
	if (adp->generation != generation || store->run_id != run_id)
		rc = -ESTALE;
	else if (!store->armed || store->sealed || store->count ||
		 store->overflow)
		rc = -EBUSY;
	else
		for (i = 0; i < DMA_ENGINE_CNT; i++)
			if (store->active[i]) {
				rc = -EBUSY;
				break;
			}
	if (!rc) {
		WRITE_ONCE(store->armed, false);
		rcu_assign_pointer(adp->rx_receipts, NULL);
	}
	spin_unlock_irqrestore(&store->lock, flags);
	if (!rc)
		crystalhd_rx_receipt_retire(store);
	return rc;
}

int crystalhd_rx_receipt_seal(struct crystalhd_adp *adp,
	struct crystalhd_fw_research_rx_receipt_seal *request)
{
	struct crystalhd_rx_receipt_store *store;
	unsigned long flags;
	bool recover_current;
	int rc = 0;

	if (!adp || !request)
		return -EINVAL;
	recover_current = request->flags ==
		CRYSTALHD_FW_RESEARCH_RX_RECEIPT_SEAL_CURRENT;
	if ((!recover_current && (request->flags || !request->run_id)) ||
	    (recover_current && request->run_id))
		return -EINVAL;
	rcu_read_lock();
	store = rcu_dereference(adp->rx_receipts);
	if (!store)
		goto no_store;
	spin_lock_irqsave(&store->lock, flags);
	if (store->sealed) {
		/* CURRENT is the recovery path for a lost ARM or SEAL reply.  Make
		 * it idempotent so losing this reply cannot strand the run again.
		 */
		if (!recover_current)
			rc = -EALREADY;
	} else if (!store->armed) {
		rc = -EALREADY;
	} else if (!recover_current && request->run_id != store->run_id) {
		rc = -ESTALE;
	} else {
		WRITE_ONCE(store->armed, false);
		store->sealed = true;
	}
	if (!rc) {
		request->sealed = 1;
		request->generation = adp->generation;
		request->run_id = store->run_id;
		request->record_count = store->count;
		request->overflow_count = store->overflow;
	}
	spin_unlock_irqrestore(&store->lock, flags);
	rcu_read_unlock();
	return rc;
no_store:
	rcu_read_unlock();
	return -ENOENT;
}

int crystalhd_rx_receipt_read(struct crystalhd_adp *adp,
	struct crystalhd_fw_research_rx_receipt_read *request)
{
	struct crystalhd_rx_receipt_store *store;
	unsigned long flags;
	u32 available = 0;
	int rc = 0;

	if (!adp || !request)
		return -EINVAL;
	if (!request->run_id)
		return -EINVAL;
	rcu_read_lock();
	store = rcu_dereference(adp->rx_receipts);
	if (!store)
		goto no_store;
	spin_lock_irqsave(&store->lock, flags);
	if (!store->sealed)
		rc = -EBUSY;
	else if (request->run_id != store->run_id)
		rc = -ESTALE;
	else if (request->start > store->count)
		rc = -ERANGE;
	else {
		available = min_t(u32, store->count - request->start,
			CRYSTALHD_FW_RESEARCH_RX_RECEIPT_READ_RECORDS);
		request->generation = adp->generation;
		request->run_id = store->run_id;
		request->returned = available;
		request->record_count = store->count;
		request->overflow_count = store->overflow;
		request->sealed = 1;
	}
	spin_unlock_irqrestore(&store->lock, flags);
	/* Sealed records are immutable; RCU retains this store through the copy. */
	if (!rc)
		memcpy(request->records, &store->records[request->start],
		       available * sizeof(*request->records));
	rcu_read_unlock();
	return rc;
no_store:
	rcu_read_unlock();
	return -ENOENT;
}

void crystalhd_rx_receipt_cleanup(struct crystalhd_adp *adp)
{
	struct crystalhd_rx_receipt_store *store;

	if (!adp)
		return;
	/* Device lifetime exclusion serializes this terminal pointer writer. */
	store = rcu_dereference_protected(adp->rx_receipts, 1);
	rcu_assign_pointer(adp->rx_receipts, NULL);
	crystalhd_rx_receipt_retire(store);
}

void crystalhd_rx_receipt_descriptors(struct crystalhd_rx_dma_pkt *pkt,
	uint32_t uv_descriptor_index)
{
	struct dma_descriptor *descriptor;
	u32 count, limit, start;

	if (!pkt)
		return;
	pkt->receipt_y_descriptor_count = 0;
	pkt->receipt_uv_descriptor_count = 0;
	descriptor = pkt->desc_mem.pdma_desc_start;
	limit = pkt->desc_mem.sz / sizeof(*descriptor);
	if (!descriptor || !limit)
		return;
	start = uv_descriptor_index;
	if (start >= limit)
		return;
	for (count = start; count < limit; count++)
		if (descriptor[count].last_rec_indicator) {
			count++;
			break;
		}
	if (count <= start || count > limit)
		return;
	if (uv_descriptor_index) {
		pkt->receipt_y_descriptor_count = uv_descriptor_index;
		pkt->receipt_uv_descriptor_count = count - uv_descriptor_index;
	} else {
		pkt->receipt_y_descriptor_count = count;
	}
}

void crystalhd_rx_receipt_reserve(struct crystalhd_hw *hw,
	struct crystalhd_rx_dma_pkt *pkt, uint32_t list_index,
	uint32_t rx_sequence, uint32_t channel_id,
	struct crystalhd_rx_receipt_key *key)
{
	struct crystalhd_rx_receipt_store *store;
	struct crystalhd_fw_research_rx_receipt *record;
	unsigned long flags;
	u32 index;

	if (!pkt || list_index >= DMA_ENGINE_CNT)
		return;
	if (key)
		memset(key, 0, sizeof(*key));
	pkt->receipt_run_id = 0;
	pkt->receipt_index_plus_one = 0;
	rcu_read_lock();
	store = crystalhd_rx_receipt_store(hw);
	if (!store)
		goto rcu_unlock;
	spin_lock_irqsave(&store->lock, flags);
	if (!store->armed || store->sealed)
		goto unlock;
	if (store->active[list_index]) {
		index = store->active[list_index] - 1;
		if (index < store->count)
			store->records[index].stage_flags |=
				CRYSTALHD_FW_RESEARCH_RX_STAGE_LIST_REPLACED;
		store->active[list_index] = 0;
		store->overflow++;
	}
	if (store->count >= store->capacity) {
		store->overflow++;
		goto unlock;
	}
	index = store->count++;
	record = &store->records[index];
	record->ordinal = index;
	record->capture_epoch = pkt->capture_epoch;
	record->rx_sequence = rx_sequence;
	record->list_index = list_index;
	record->packet_tag = pkt->pkt_tag;
	record->channel_id = channel_id;
	record->y_descriptor_count = pkt->receipt_y_descriptor_count;
	record->uv_descriptor_count = pkt->receipt_uv_descriptor_count;
	record->stage_flags = CRYSTALHD_FW_RESEARCH_RX_STAGE_RESERVED;
	store->active[list_index] = index + 1;
	pkt->receipt_run_id = store->run_id;
	pkt->receipt_index_plus_one = index + 1;
	if (key) {
		key->run_id = store->run_id;
		key->index_plus_one = index + 1;
	}
unlock:
	spin_unlock_irqrestore(&store->lock, flags);
rcu_unlock:
	rcu_read_unlock();
}

void crystalhd_rx_receipt_fire_return(struct crystalhd_hw *hw,
	const struct crystalhd_rx_receipt_key *key)
{
	struct crystalhd_rx_receipt_store *store;
	struct crystalhd_fw_research_rx_receipt *record;
	unsigned long flags;
	u64 now;

	if (!key)
		return;
	rcu_read_lock();
	store = crystalhd_rx_receipt_store(hw);
	if (!store)
		goto rcu_unlock;
	now = ktime_get_ns();
	spin_lock_irqsave(&store->lock, flags);
	record = crystalhd_rx_receipt_key_locked(store, key);
	if (record) {
		record->fire_return_ns = now;
		record->stage_flags |= CRYSTALHD_FW_RESEARCH_RX_STAGE_FIRE_RETURN;
		if (record->irq_entry_ns && record->irq_entry_ns < now)
			record->stage_flags |=
				CRYSTALHD_FW_RESEARCH_RX_STAGE_CLOCK_INVERTED;
	}
	spin_unlock_irqrestore(&store->lock, flags);
rcu_unlock:
	rcu_read_unlock();
}

uint64_t crystalhd_rx_receipt_irq_entry(struct crystalhd_hw *hw)
{
	struct crystalhd_rx_receipt_store *store;
	u64 now = 0;

	rcu_read_lock();
	store = crystalhd_rx_receipt_store(hw);
	if (store && READ_ONCE(store->armed))
		now = ktime_get_ns();
	rcu_read_unlock();
	return now;
}

void crystalhd_rx_receipt_irq(struct crystalhd_hw *hw, uint32_t list_index,
	uint64_t irq_entry_ns, BC_STATUS completion_status)
{
	struct crystalhd_rx_receipt_store *store;
	struct crystalhd_fw_research_rx_receipt *record;
	unsigned long flags;
	u32 index;

	if (!irq_entry_ns || list_index >= DMA_ENGINE_CNT)
		return;
	rcu_read_lock();
	store = crystalhd_rx_receipt_store(hw);
	if (!store)
		goto rcu_unlock;
	spin_lock_irqsave(&store->lock, flags);
	if (!store->armed || store->sealed || !store->active[list_index])
		goto unlock;
	index = store->active[list_index] - 1;
	store->active[list_index] = 0;
	if (index >= store->count)
		goto unlock;
	record = &store->records[index];
	record->irq_entry_ns = irq_entry_ns;
	record->completion_status = completion_status;
	record->stage_flags |= CRYSTALHD_FW_RESEARCH_RX_STAGE_IRQ_ENTRY;
	if (record->fire_return_ns && irq_entry_ns < record->fire_return_ns)
		record->stage_flags |=
			CRYSTALHD_FW_RESEARCH_RX_STAGE_CLOCK_INVERTED;
unlock:
	spin_unlock_irqrestore(&store->lock, flags);
rcu_unlock:
	rcu_read_unlock();
}

void crystalhd_rx_receipt_complete(struct crystalhd_hw *hw,
	struct crystalhd_rx_dma_pkt *pkt, BC_STATUS completion_status)
{
	struct crystalhd_rx_receipt_store *store;
	struct crystalhd_fw_research_rx_receipt *record;
	unsigned long flags;
	u64 now;

	if (!pkt)
		return;
	rcu_read_lock();
	store = crystalhd_rx_receipt_store(hw);
	if (!store)
		goto rcu_unlock;
	now = ktime_get_ns();
	spin_lock_irqsave(&store->lock, flags);
	record = crystalhd_rx_receipt_record_locked(store, pkt);
	if (record) {
		record->completion_ns = now;
		record->completion_status = completion_status;
		/* The completion path refreshes sizes and flags only after a
		 * successful DMA.  A recycled packet can otherwise retain values
		 * from its previous transfer, so never report them for an error.
		 */
		if (completion_status == BC_STS_SUCCESS) {
			record->y_done_words = pkt->y_done_sz;
			record->uv_done_words = pkt->uv_done_sz;
			record->packet_flags = pkt->flags;
		}
		record->stage_flags |= CRYSTALHD_FW_RESEARCH_RX_STAGE_COMPLETE;
	}
	spin_unlock_irqrestore(&store->lock, flags);
rcu_unlock:
	rcu_read_unlock();
}

void crystalhd_rx_receipt_parse(struct crystalhd_hw *hw,
	struct crystalhd_rx_dma_pkt *pkt, bool parsed)
{
	struct crystalhd_rx_receipt_store *store;
	struct crystalhd_fw_research_rx_receipt *record;
	unsigned long flags;
	u64 now;

	if (!pkt)
		return;
	rcu_read_lock();
	store = crystalhd_rx_receipt_store(hw);
	if (!store)
		goto rcu_unlock;
	now = ktime_get_ns();
	spin_lock_irqsave(&store->lock, flags);
	record = crystalhd_rx_receipt_record_locked(store, pkt);
	if (!record || (record->stage_flags &
		       (CRYSTALHD_FW_RESEARCH_RX_STAGE_PARSED |
			CRYSTALHD_FW_RESEARCH_RX_STAGE_PARSE_FAILED)))
		goto unlock;
	record->parse_ns = now;
	record->packet_flags = pkt->flags;
	if (pkt->metadata.valid) {
		record->firmware_timestamp = pkt->metadata.firmware_timestamp;
		record->picture_number = pkt->metadata.picture_number;
		record->picture_flags = pkt->metadata.picture_flags;
		record->picture_width = pkt->metadata.picture_width;
		record->picture_height = pkt->metadata.picture_height;
		record->row_width = pkt->metadata.row_width;
		record->pib_line = pkt->metadata.pib_line;
		record->session_number = pkt->metadata.session_number;
		record->eos_trailer = pkt->metadata.eos_trailer;
	} else if (pkt->flags & COMP_FLAG_PIB_VALID) {
		record->picture_number = pkt->pib.picture_number;
		record->picture_flags = pkt->pib.flags;
		record->picture_width = pkt->pib.width;
		record->picture_height = pkt->pib.height;
		record->session_number = pkt->pib.sess_num;
	}
	if (parsed || (pkt->flags & COMP_FLAG_PIB_VALID))
		record->stage_flags |= CRYSTALHD_FW_RESEARCH_RX_STAGE_PARSED;
	else
		record->stage_flags |=
			CRYSTALHD_FW_RESEARCH_RX_STAGE_PARSE_FAILED;
unlock:
	spin_unlock_irqrestore(&store->lock, flags);
rcu_unlock:
	rcu_read_unlock();
}

void crystalhd_rx_receipt_disposition(struct crystalhd_hw *hw,
	struct crystalhd_rx_dma_pkt *pkt, uint32_t disposition)
{
	struct crystalhd_rx_receipt_store *store;
	struct crystalhd_fw_research_rx_receipt *record;
	unsigned long flags;
	u64 now;

	if (!pkt)
		return;
	rcu_read_lock();
	store = crystalhd_rx_receipt_store(hw);
	if (!store)
		goto rcu_unlock;
	now = ktime_get_ns();
	spin_lock_irqsave(&store->lock, flags);
	record = crystalhd_rx_receipt_record_locked(store, pkt);
	if (record && !(record->stage_flags &
			CRYSTALHD_FW_RESEARCH_RX_STAGE_DISPOSITION)) {
		record->disposition_ns = now;
		record->disposition = disposition;
		record->stage_flags |=
			CRYSTALHD_FW_RESEARCH_RX_STAGE_DISPOSITION;
	}
	spin_unlock_irqrestore(&store->lock, flags);
rcu_unlock:
	rcu_read_unlock();
}
