/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef _CRYSTALHD_RX_RECEIPT_H_
#define _CRYSTALHD_RX_RECEIPT_H_

struct crystalhd_adp;
struct crystalhd_hw;
struct crystalhd_rx_dma_pkt;
struct crystalhd_fw_research_rx_receipt_arm;
struct crystalhd_fw_research_rx_receipt_seal;
struct crystalhd_fw_research_rx_receipt_read;

/* Stack-carried across the successful fire return so a concurrently recycled
 * packet cannot redirect the return timestamp to its next DMA admission.
 */
struct crystalhd_rx_receipt_key {
	uint64_t run_id;
	uint32_t index_plus_one;
};

#ifdef CRYSTALHD_ENABLE_FW_RESEARCH
/* ARM/SEAL require the adapter user-lock writer and an idle session inventory.
 * READ retains device/user read access, and terminal cleanup runs only after
 * device lifetime exclusion. The store pointer is RCU-published because an IRQ
 * sharing the line can begin after the ioctl's synchronize_irq() boundary.
 */
int crystalhd_rx_receipt_arm(struct crystalhd_adp *adp,
	struct crystalhd_fw_research_rx_receipt_arm *request);
int crystalhd_rx_receipt_cancel_empty_arm(struct crystalhd_adp *adp,
	uint64_t generation, uint64_t run_id);
int crystalhd_rx_receipt_seal(struct crystalhd_adp *adp,
	struct crystalhd_fw_research_rx_receipt_seal *request);
int crystalhd_rx_receipt_read(struct crystalhd_adp *adp,
	struct crystalhd_fw_research_rx_receipt_read *request);
void crystalhd_rx_receipt_cleanup(struct crystalhd_adp *adp);
void crystalhd_rx_receipt_descriptors(struct crystalhd_rx_dma_pkt *pkt,
	uint32_t uv_descriptor_index);
void crystalhd_rx_receipt_reserve(struct crystalhd_hw *hw,
	struct crystalhd_rx_dma_pkt *pkt, uint32_t list_index,
	uint32_t rx_sequence, uint32_t channel_id,
	struct crystalhd_rx_receipt_key *key);
void crystalhd_rx_receipt_fire_return(struct crystalhd_hw *hw,
	const struct crystalhd_rx_receipt_key *key);
uint64_t crystalhd_rx_receipt_irq_entry(struct crystalhd_hw *hw);
void crystalhd_rx_receipt_irq(struct crystalhd_hw *hw, uint32_t list_index,
	uint64_t irq_entry_ns, BC_STATUS completion_status);
void crystalhd_rx_receipt_complete(struct crystalhd_hw *hw,
	struct crystalhd_rx_dma_pkt *pkt, BC_STATUS completion_status);
void crystalhd_rx_receipt_parse(struct crystalhd_hw *hw,
	struct crystalhd_rx_dma_pkt *pkt, bool parsed);
void crystalhd_rx_receipt_disposition(struct crystalhd_hw *hw,
	struct crystalhd_rx_dma_pkt *pkt, uint32_t disposition);
#else
static inline void crystalhd_rx_receipt_cleanup(struct crystalhd_adp *adp)
{
	(void)adp;
}
static inline void crystalhd_rx_receipt_descriptors(
	struct crystalhd_rx_dma_pkt *pkt, uint32_t uv_descriptor_index)
{
	(void)pkt;
	(void)uv_descriptor_index;
}
static inline void crystalhd_rx_receipt_reserve(struct crystalhd_hw *hw,
	struct crystalhd_rx_dma_pkt *pkt, uint32_t list_index,
	uint32_t rx_sequence, uint32_t channel_id,
	struct crystalhd_rx_receipt_key *key)
{
	(void)hw;
	(void)pkt;
	(void)list_index;
	(void)rx_sequence;
	(void)channel_id;
	(void)key;
}
static inline void crystalhd_rx_receipt_fire_return(struct crystalhd_hw *hw,
	const struct crystalhd_rx_receipt_key *key)
{
	(void)hw;
	(void)key;
}
static inline uint64_t crystalhd_rx_receipt_irq_entry(struct crystalhd_hw *hw)
{
	(void)hw;
	return 0;
}
static inline void crystalhd_rx_receipt_irq(struct crystalhd_hw *hw,
	uint32_t list_index, uint64_t irq_entry_ns,
	BC_STATUS completion_status)
{
	(void)hw;
	(void)list_index;
	(void)irq_entry_ns;
	(void)completion_status;
}
static inline void crystalhd_rx_receipt_complete(struct crystalhd_hw *hw,
	struct crystalhd_rx_dma_pkt *pkt, BC_STATUS completion_status)
{
	(void)hw;
	(void)pkt;
	(void)completion_status;
}
static inline void crystalhd_rx_receipt_parse(struct crystalhd_hw *hw,
	struct crystalhd_rx_dma_pkt *pkt, bool parsed)
{
	(void)hw;
	(void)pkt;
	(void)parsed;
}
static inline void crystalhd_rx_receipt_disposition(struct crystalhd_hw *hw,
	struct crystalhd_rx_dma_pkt *pkt, uint32_t disposition)
{
	(void)hw;
	(void)pkt;
	(void)disposition;
}
#endif

#endif
