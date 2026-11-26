/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Isolated copied-value backend for the existing RX receipt hook sites.
 * No packet mutation, descriptor traversal, device access or allocation. */
#include <linux/ktime.h>
#include "crystalhd_lnx.h"
#include "crystalhd_rx_receipt.h"
#include "crystalhd_tx_notify.h"

#define RX_PTR_LOW(p) ((uint32_t)(uint64_t)(unsigned long)(p))
#define RX_PTR_HIGH(p) ((uint32_t)((uint64_t)(unsigned long)(p) >> 32))

/* RX notify backend begin: exercised verbatim by the device-free harness. */
void crystalhd_rx_receipt_reserve(struct crystalhd_hw *hw,
 struct crystalhd_rx_dma_pkt *pkt, uint32_t list_index,
 uint32_t rx_sequence, uint32_t channel_id,
 struct crystalhd_rx_receipt_key *key)
{
 if (!hw || !pkt || !key)
  return;
 /* Private backend key: sequence/tag plus list, entirely stack-carried.
  * Fire return never rereads a packet that an IRQ may have recycled. */
 key->run_id = ((uint64_t)rx_sequence << 32) | pkt->pkt_tag;
 key->index_plus_one = list_index + 1;
 CHD_RX_NOTE(hw, CHD_RX_RESERVE, RX_PTR_LOW(pkt), RX_PTR_HIGH(pkt),
  pkt->pkt_tag, list_index, rx_sequence, channel_id,
  (uint32_t)pkt->capture_epoch, (uint32_t)(pkt->capture_epoch >> 32));
}
void crystalhd_rx_receipt_fire_return(struct crystalhd_hw *hw,
 const struct crystalhd_rx_receipt_key *key)
{
 if (!hw || !key || !key->index_plus_one)
  return;
 CHD_RX_NOTE(hw, CHD_RX_FIRE_RETURN, (uint32_t)key->run_id,
  (uint32_t)(key->run_id >> 32), key->index_plus_one - 1);
}
uint64_t crystalhd_rx_receipt_irq_entry(struct crystalhd_hw *hw)
{
 return hw ? ktime_get_mono_fast_ns() : 0;
}
void crystalhd_rx_receipt_irq(struct crystalhd_hw *hw, uint32_t list_index,
 uint64_t irq_entry_ns, BC_STATUS completion_status)
{
 if (!hw)
  return;
 CHD_RX_NOTE(hw, CHD_RX_IRQ, list_index, completion_status,
  (uint32_t)irq_entry_ns, (uint32_t)(irq_entry_ns >> 32));
}
void crystalhd_rx_receipt_complete(struct crystalhd_hw *hw,
 struct crystalhd_rx_dma_pkt *pkt, BC_STATUS completion_status)
{
 bool success = completion_status == BC_STS_SUCCESS;
 if (!hw || !pkt)
  return;
 /* Failed completion does not refresh the previous transfer's byte fields. */
 CHD_RX_NOTE(hw, CHD_RX_COMPLETE, RX_PTR_LOW(pkt), RX_PTR_HIGH(pkt),
  pkt->pkt_tag, completion_status, success ? pkt->y_done_sz : 0,
  success ? pkt->uv_done_sz : 0, success ? pkt->flags : 0,
  (uint32_t)pkt->capture_epoch, (uint32_t)(pkt->capture_epoch >> 32));
}
void crystalhd_rx_receipt_parse(struct crystalhd_hw *hw,
 struct crystalhd_rx_dma_pkt *pkt, bool parsed)
{
 uint64_t stamp = 0;
 uint32_t number = 0, flags = 0;
 if (!hw || !pkt)
  return;
 if (pkt->metadata.valid) {
  stamp = pkt->metadata.firmware_timestamp;
  number = pkt->metadata.picture_number;
  flags = pkt->metadata.picture_flags;
 } else if (pkt->flags & COMP_FLAG_PIB_VALID) {
  number = pkt->pib.picture_number;
  flags = pkt->pib.flags;
 }
 if (!pkt->metadata.valid && pkt->format_metadata.valid)
  stamp = pkt->format_metadata.firmware_timestamp;
 /* Only the actual acceptance hook calls this backend: parser peeks do not.
  * parsed=false can be a format notification, not necessarily a bad PIB. */
 CHD_RX_NOTE(hw, CHD_RX_PARSE, RX_PTR_LOW(pkt), RX_PTR_HIGH(pkt),
  pkt->pkt_tag, parsed, pkt->flags, pkt->metadata.valid,
  (uint32_t)stamp, (uint32_t)(stamp >> 32), number, flags,
  pkt->metadata.valid ? pkt->metadata.eos_trailer : 0,
  pkt->format_metadata.valid);
}
void crystalhd_rx_receipt_disposition(struct crystalhd_hw *hw,
 struct crystalhd_rx_dma_pkt *pkt, uint32_t disposition)
{
 if (!hw || !pkt)
  return;
 CHD_RX_NOTE(hw, CHD_RX_DISPOSITION, RX_PTR_LOW(pkt), RX_PTR_HIGH(pkt),
  pkt->pkt_tag, disposition);
}
/* RX notify backend end. */
