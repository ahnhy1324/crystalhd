/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef CRYSTALHD_PM_SHADOW_RING_H
#define CRYSTALHD_PM_SHADOW_RING_H

/* Copied numeric records only: the reader never retains an hw pointer. */
#define CHD_PM_SHADOW_RECORDS 64U
#define CHD_PM_SHADOW_RECEIPTS 64U
#define CHD_PM_SHADOW_UNKNOWN (~0U)

enum chd_pm_shadow_stage {
	CHD_PM_INIT,
	CHD_PM_SELECT,
	CHD_PM_SET,
	CHD_PM_FLL,
	CHD_PM_FLL_RESTORE,
	CHD_PM_DOWN_REFUSED,
	CHD_PM_UP_ENTRY,
	CHD_PM_RX_ADMIT,
	CHD_PM_RX_REFUSED,
	CHD_PM_STOP,
	CHD_PM_FLL_RESTORE_BEGIN,
	CHD_PM_TX_ENTER,
	CHD_PM_TX_LEAVE,
	CHD_PM_RAW_ENTER,
	CHD_PM_RAW_LEAVE,
	CHD_PM_IRQ_ENTER,
	CHD_PM_IRQ_LEAVE,
	CHD_PM_FW_ENTER,
	CHD_PM_FW_LEAVE,
	CHD_PM_FW_TRANSACTION_ENTER,
	CHD_PM_FW_TRANSACTION_LEAVE,
	CHD_PM_FW_RETIRE,
	CHD_PM_FW_RESET,
	CHD_PM_RETIRE,
	CHD_PM_FAULT,
	CHD_PM_ARBITER,
	CHD_PM_RETENTION,
	CHD_PM_STAGE_COUNT,
};

enum chd_pm_shadow_violation {
	CHD_PM_DOWN_OWNER = 1U,
	CHD_PM_RX_NOT_ACTIVE = 2U,
	CHD_PM_FLL_EARLY_ACTIVE = 4U,
	CHD_PM_LP_COMPLETE = 8U,
};

struct chd_pm_shadow_record {
	u64 sequence, time_ns, session;
	u32 stage, request, old_state, selected_state, new_state;
	u32 reservation, status, fll, violations, dma_fault;
	u32 tx_owner, irq_owner, raw_owner;
	u32 fw_transaction, fw_active, fw_count, fw_poisoned;
	u32 tx_list0, tx_list1, rx_list0, rx_list1;
	u32 empty_count, single_fifo_empty, rx_pause;
};

struct chd_pm_shadow_ring {
	u64 total, overwritten, down_refused, up_entry, lp_complete, violations;
	u64 receipt_total, receipt_dropped, stage_count[CHD_PM_STAGE_COUNT];
	u32 next, count;
	u32 receipt_count, retire_valid, first_fault_valid;
	struct chd_pm_shadow_record record[CHD_PM_SHADOW_RECORDS];
	/* First bounded transition receipts are never overwritten by IRQ traffic. */
	struct chd_pm_shadow_record receipt[CHD_PM_SHADOW_RECEIPTS];
	struct chd_pm_shadow_record retire, first_fault;
	u32 arbiter_valid;
	struct chd_pm_arbiter_report arbiter;
	u32 retention_valid;
	struct chd_pm_retention_report retention;
};

static inline void chd_pm_shadow_increment(u64 *value)
{
	if (*value != ~(u64)0)
		(*value)++;
}

/* The caller serializes this bounded, allocation-free writer. */
static inline void chd_pm_shadow_store(struct chd_pm_shadow_ring *ring,
				      struct chd_pm_shadow_record *record,
				      u32 critical)
{
	chd_pm_shadow_increment(&ring->total);
	record->sequence = ring->total;
	if (record->stage < CHD_PM_STAGE_COUNT)
		chd_pm_shadow_increment(&ring->stage_count[record->stage]);
	if (record->stage == CHD_PM_DOWN_REFUSED)
		chd_pm_shadow_increment(&ring->down_refused);
	if (record->stage == CHD_PM_UP_ENTRY)
		chd_pm_shadow_increment(&ring->up_entry);
	if (record->violations & CHD_PM_LP_COMPLETE)
		chd_pm_shadow_increment(&ring->lp_complete);
	if (record->violations)
		chd_pm_shadow_increment(&ring->violations);
	if (critical) {
		chd_pm_shadow_increment(&ring->receipt_total);
		if (ring->receipt_count < CHD_PM_SHADOW_RECEIPTS)
			ring->receipt[ring->receipt_count++] = *record;
		else
			chd_pm_shadow_increment(&ring->receipt_dropped);
	}
	if (record->stage == CHD_PM_RETIRE) {
		ring->retire = *record;
		ring->retire_valid = 1;
	}
	if (!ring->first_fault_valid &&
	    (record->dma_fault || record->new_state == FLEA_PS_FAULT || record->fw_poisoned ||
	     record->violations || record->stage == CHD_PM_UP_ENTRY)) {
		ring->first_fault = *record;
		ring->first_fault_valid = 1;
	}
	ring->record[ring->next] = *record;
	ring->next = (ring->next + 1) % CHD_PM_SHADOW_RECORDS;
	if (ring->count < CHD_PM_SHADOW_RECORDS)
		ring->count++;
	else
		chd_pm_shadow_increment(&ring->overwritten);
}

#endif
