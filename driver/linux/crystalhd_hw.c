/***************************************************************************
 * Copyright (c) 2005-2009, Broadcom Corporation.
 *
 *  Name: crystalhd_hw . c
 *
 *  Description:
 *		BCM70012/BCM70015 Linux driver hardware layer.
 *
 *  HISTORY:
 *
 **********************************************************************
 * This file is part of the crystalhd device driver.
 *
 * This driver is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, version 2 of the License.
 *
 * This driver is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this driver.  If not, see <http://www.gnu.org/licenses/>.
 **********************************************************************/

#include <linux/pci.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <asm/tsc.h>
#include <asm/msr.h>
#include "crystalhd_lnx.h"
#include "crystalhd_linkfuncs.h"
#include "crystalhd_fleafuncs.h"

#define OFFSETOF(_s_, _m_) ((size_t)(unsigned long)&(((_s_ *)0)->_m_))

BC_STATUS crystalhd_hw_fw_cmd_enter(struct crystalhd_hw *hw)
{
	unsigned long flags;
	BC_STATUS sts = BC_STS_SUCCESS;

	if (!hw)
		return BC_STS_INV_ARG;
	if (mutex_lock_interruptible(&hw->fwcmd_trans_mutex))
		return BC_STS_IO_USER_ABORT;

	/* Reject quarantined or externally posted mailbox work before command-layer
	 * preprocessing can mutate capture/flush state. The low-level begin check is
	 * retained as a defensive backstop for callers outside this transaction.
	 */
	spin_lock_irqsave(&hw->lock, flags);
	if (hw->fwcmd_poisoned || hw->fwcmd_pending)
		sts = BC_STS_BUSY;
	spin_unlock_irqrestore(&hw->lock, flags);
	if (sts != BC_STS_SUCCESS)
		mutex_unlock(&hw->fwcmd_trans_mutex);
	return sts;
}

BC_STATUS crystalhd_hw_fw_cmd_recovery_enter(struct crystalhd_hw *hw)
{
	unsigned long flags;
	BC_STATUS sts = BC_STS_SUCCESS;

	if (!hw)
		return BC_STS_INV_ARG;
	if (mutex_lock_interruptible(&hw->fwcmd_trans_mutex))
		return BC_STS_IO_USER_ABORT;
	/* Recovery may clear quarantine, but must not reset over active work from
	 * a defensive/direct low-level caller that bypassed transaction locking.
	 */
	spin_lock_irqsave(&hw->lock, flags);
	if (hw->fwcmd_pending)
		sts = BC_STS_BUSY;
	spin_unlock_irqrestore(&hw->lock, flags);
	if (sts != BC_STS_SUCCESS)
		mutex_unlock(&hw->fwcmd_trans_mutex);
	return sts;
}

void crystalhd_hw_fw_cmd_leave(struct crystalhd_hw *hw)
{
	if (hw)
		mutex_unlock(&hw->fwcmd_trans_mutex);
}

BC_STATUS crystalhd_hw_fw_cmd_begin(struct crystalhd_hw *hw)
{
	unsigned long flags;
	BC_STATUS sts = BC_STS_SUCCESS;

	if (!hw)
		return BC_STS_INV_ARG;
	if (mutex_lock_interruptible(&hw->fwcmd_mutex))
		return BC_STS_IO_USER_ABORT;

	spin_lock_irqsave(&hw->lock, flags);
	if (hw->fwcmd_poisoned || hw->fwcmd_pending) {
		sts = BC_STS_BUSY;
	} else {
		hw->fwcmd_evt_sts = 0;
		hw->fwcmd_pending = true;
		hw->FwCmdCnt++;
	}
	spin_unlock_irqrestore(&hw->lock, flags);

	if (sts != BC_STS_SUCCESS)
		mutex_unlock(&hw->fwcmd_mutex);
	return sts;
}

BC_STATUS crystalhd_hw_fw_cmd_wait(struct crystalhd_hw *hw)
{
	unsigned long flags;
	BC_STATUS sts;
	int rc = 0;

	if (!hw)
		return BC_STS_INV_ARG;

	crystalhd_wait_on_event(&hw->fwcmd_event,
				READ_ONCE(hw->fwcmd_evt_sts), 20000, rc, true);

	spin_lock_irqsave(&hw->lock, flags);
	if (hw->fwcmd_evt_sts) {
		sts = BC_STS_SUCCESS;
	} else {
		/* The response interrupt can still arrive after this caller leaves.
		 * Quarantine the mailbox and keep the command counted until that late
		 * interrupt is consumed or the hardware is reset. Flea power management
		 * must not sleep the firmware while its response is outstanding.
		 */
		if (hw->fwcmd_pending) {
			hw->fwcmd_pending = false;
			hw->fwcmd_poisoned = true;
		}
		if (rc == -EBUSY)
			sts = BC_STS_TIMEOUT;
		else if (rc == -EINTR)
			sts = BC_STS_IO_USER_ABORT;
		else
			sts = BC_STS_IO_ERROR;
	}
	spin_unlock_irqrestore(&hw->lock, flags);

	return sts;
}

void crystalhd_hw_fw_cmd_end(struct crystalhd_hw *hw)
{
	if (hw)
		mutex_unlock(&hw->fwcmd_mutex);
}

void crystalhd_hw_fw_cmd_complete(struct crystalhd_hw *hw)
{
	unsigned long flags;
	bool wake = false;

	if (!hw)
		return;

	spin_lock_irqsave(&hw->lock, flags);
	if (hw->fwcmd_pending) {
		hw->fwcmd_pending = false;
		hw->fwcmd_evt_sts = 1;
		if (hw->FwCmdCnt)
			hw->FwCmdCnt--;
		wake = true;
	} else if (hw->fwcmd_poisoned) {
		/* Retire the timed-out command's power-management count, but keep the
		 * mailbox poisoned. The firmware may have committed a state-changing
		 * command even though the caller timed out, and the response/post-process
		 * path is no longer available to reconcile host state. Only a verified
		 * firmware/device reset may admit another command.
		 */
		if (hw->FwCmdCnt) {
			hw->FwCmdCnt--;
			wake = true;
		}
	}
	spin_unlock_irqrestore(&hw->lock, flags);

	if (wake)
		crystalhd_set_event(&hw->fwcmd_event);
}

void crystalhd_hw_fw_cmd_reset_locked(struct crystalhd_hw *hw)
{
	unsigned long flags;

	if (!hw)
		return;
	mutex_lock(&hw->fwcmd_mutex);
	spin_lock_irqsave(&hw->lock, flags);
	hw->fwcmd_pending = false;
	hw->fwcmd_poisoned = false;
	hw->fwcmd_evt_sts = 0;
	hw->FwCmdCnt = 0;
	spin_unlock_irqrestore(&hw->lock, flags);
	mutex_unlock(&hw->fwcmd_mutex);
}

void crystalhd_hw_fw_cmd_reset(struct crystalhd_hw *hw)
{
	if (!hw)
		return;
	mutex_lock(&hw->fwcmd_trans_mutex);
	crystalhd_hw_fw_cmd_reset_locked(hw);
	mutex_unlock(&hw->fwcmd_trans_mutex);
}

BC_STATUS crystalhd_hw_open(struct crystalhd_hw *hw, struct crystalhd_adp *adp)
{
	struct device *dev;
	if (!hw || !adp) {
		printk(KERN_ERR "%s: Invalid Arguments\n", __func__);
		return BC_STS_INV_ARG;
	}

	if (hw->dev_started)
		return BC_STS_SUCCESS;

	dev = &adp->pdev->dev;
	hw->PauseThreshold = BC_RX_LIST_CNT - 2;
	hw->DefaultPauseThreshold = BC_RX_LIST_CNT - 2;
	hw->ResumeThreshold = 3;

	/* Setup HW specific functions appropriately */
	if (adp->pdev->device == BC_PCI_DEVID_FLEA) {
		dev_dbg(dev, "crystalhd_hw_open: setting up functions, device = Flea\n");
		hw->pfnStartDevice = crystalhd_flea_start_device;
		hw->pfnStopDevice = crystalhd_flea_stop_device;
		hw->pfnFindAndClearIntr = crystalhd_flea_hw_interrupt_handle;
		hw->pfnReadDevRegister = crystalhd_flea_reg_rd;					/* Done */
		hw->pfnWriteDevRegister = crystalhd_flea_reg_wr;				/* Done */
		hw->pfnReadFPGARegister = crystalhd_flea_reg_rd;				/* Done */
		hw->pfnWriteFPGARegister = crystalhd_flea_reg_wr;				/* Done */
		hw->pfnCheckInputFIFO = crystalhd_flea_check_input_full;
		hw->pfnDevDRAMRead = crystalhd_flea_mem_rd;						/* Done */
		hw->pfnDevDRAMWrite = crystalhd_flea_mem_wr;					/* Done */
		hw->pfnDoFirmwareCmd = crystalhd_flea_do_fw_cmd;
		hw->pfnFWDwnld = crystalhd_flea_download_fw;
		hw->pfnHWGetDoneSize = crystalhd_flea_get_dnsz;
		hw->pfnIssuePause = crystalhd_flea_hw_pause;
		hw->pfnPeekNextDeodedFr = crystalhd_flea_peek_next_decoded_frame;
		hw->pfnPostRxSideBuff = crystalhd_flea_hw_post_cap_buff;
		hw->pfnStartTxDMA = crystalhd_flea_start_tx_dma_engine;
		hw->pfnStopTxDMA = crystalhd_flea_stop_tx_dma_engine;
		hw->pfnStopRXDMAEngines = crystalhd_flea_stop_rx_dma_engine;
		hw->pfnNotifyFLLChange = crystalhd_flea_notify_fll_change;
		hw->pfnNotifyHardware = crystalhd_flea_notify_event;
	} else {
		dev_dbg(dev, "crystalhd_hw_open: setting up functions, device = Link\n");
		hw->pfnStartDevice = crystalhd_link_start_device;
		hw->pfnStopDevice = crystalhd_link_stop_device;
		hw->pfnFindAndClearIntr = crystalhd_link_hw_interrupt_handle;
		hw->pfnReadDevRegister = link_dec_reg_rd;
		hw->pfnWriteDevRegister = link_dec_reg_wr;
		hw->pfnReadFPGARegister = crystalhd_link_reg_rd;
		hw->pfnWriteFPGARegister = crystalhd_link_reg_wr;
		hw->pfnCheckInputFIFO = crystalhd_link_check_input_full;
		hw->pfnDevDRAMRead = crystalhd_link_mem_rd;
		hw->pfnDevDRAMWrite = crystalhd_link_mem_wr;
		hw->pfnDoFirmwareCmd = crystalhd_link_do_fw_cmd;
		hw->pfnFWDwnld = crystalhd_link_download_fw;
		hw->pfnHWGetDoneSize = crystalhd_link_get_dnsz;
		hw->pfnIssuePause = crystalhd_link_hw_pause;
		hw->pfnPeekNextDeodedFr = crystalhd_link_peek_next_decoded_frame;
		hw->pfnPostRxSideBuff = crystalhd_link_hw_post_cap_buff;
		hw->pfnStartTxDMA = crystalhd_link_start_tx_dma_engine;
		hw->pfnStopTxDMA = crystalhd_link_stop_tx_dma_engine;
		hw->pfnStopRXDMAEngines = crystalhd_link_stop_rx_dma_engine;
		hw->pfnNotifyFLLChange = crystalhd_link_notify_fll_change;
		hw->pfnNotifyHardware = crystalhd_link_notify_event;
	}

	hw->adp = adp;
	spin_lock_init(&hw->lock);
	spin_lock_init(&hw->rx_lock);
	sema_init(&hw->fetch_sem, 1);
	mutex_init(&hw->fwcmd_trans_mutex);
	mutex_init(&hw->fwcmd_mutex);
	crystalhd_create_event(&hw->fwcmd_event);
	crystalhd_hw_fw_cmd_reset(hw);

	/* Seed for error checking and debugging. Random numbers */
	hw->tx_ioq_tag_seed = 0x70023070;
	hw->rx_pkt_tag_seed = 0x70029070;

	hw->stop_pending = 0;
	if (!hw->pfnStartDevice(hw))
		return BC_STS_ERROR;
	/* A fresh hardware context resets the engines before recovering from
	 * a prior session's fail-stop of PCI bus mastering.
	 */
	pci_set_master(adp->pdev);
	hw->dev_started = true;

	dev_dbg(dev, "Opening HW. hw:0x%lx, hw->adp:0x%lx\n",
		(uintptr_t)hw, (uintptr_t)(hw->adp));

	return BC_STS_SUCCESS;
}

BC_STATUS crystalhd_hw_close(struct crystalhd_hw *hw)
{
	if (!hw) {
		printk(KERN_ERR "%s: Invalid Arguments\n", __func__);
		return BC_STS_SUCCESS;
	}

	if (!hw->dev_started)
		return BC_STS_SUCCESS;

	/* The caller is retiring this context, even if non-owning files remain. */
	crystalhd_hw_suspend(hw);

	hw->dev_started = false;

	return BC_STS_SUCCESS;
}

struct crystalhd_rx_dma_pkt *crystalhd_hw_alloc_rx_pkt(struct crystalhd_hw *hw)
{
	unsigned long flags = 0;
	struct crystalhd_rx_dma_pkt *temp = NULL;

	if (!hw)
		return NULL;

	spin_lock_irqsave(&hw->lock, flags);
	temp = hw->rx_pkt_pool_head;
	if (temp) {
		hw->rx_pkt_pool_head = hw->rx_pkt_pool_head->next;
		temp->buffer = NULL;
		temp->cookie = NULL;
		temp->capture_epoch = 0;
		temp->pkt_tag = 0;
		temp->flags = 0;
		temp->y_done_sz = 0;
		temp->uv_done_sz = 0;
		memset(&temp->pib, 0, sizeof(temp->pib));
		memset(&temp->metadata, 0, sizeof(temp->metadata));
		temp->uv_phy_addr = 0;
		temp->next = NULL;
	}
	spin_unlock_irqrestore(&hw->lock, flags);

	return temp;
}

void crystalhd_hw_free_rx_pkt(struct crystalhd_hw *hw,
				   struct crystalhd_rx_dma_pkt *pkt)
{
	unsigned long flags = 0;

	if (!hw || !pkt)
		return;

	spin_lock_irqsave(&hw->lock, flags);
	pkt->buffer = NULL;
	pkt->cookie = NULL;
	pkt->capture_epoch = 0;
	pkt->pkt_tag = 0;
	pkt->flags = 0;
	pkt->y_done_sz = 0;
	pkt->uv_done_sz = 0;
	memset(&pkt->pib, 0, sizeof(pkt->pib));
	memset(&pkt->metadata, 0, sizeof(pkt->metadata));
	pkt->uv_phy_addr = 0;
	pkt->next = hw->rx_pkt_pool_head;
	hw->rx_pkt_pool_head = pkt;
	spin_unlock_irqrestore(&hw->lock, flags);
}

void crystalhd_hw_retain_rx_pkt(struct crystalhd_hw *hw,
				struct crystalhd_rx_dma_pkt *pkt)
{
	unsigned long flags = 0;

	if (!hw || !pkt)
		return;

	spin_lock_irqsave(&hw->lock, flags);
	pkt->next = hw->rx_fallback_head;
	hw->rx_fallback_head = pkt;
	spin_unlock_irqrestore(&hw->lock, flags);
}

static struct crystalhd_rx_dma_pkt *
crystalhd_hw_fetch_retained_rx_pkt(struct crystalhd_hw *hw)
{
	struct crystalhd_rx_dma_pkt *pkt;
	unsigned long flags = 0;

	if (!hw)
		return NULL;

	spin_lock_irqsave(&hw->lock, flags);
	pkt = hw->rx_fallback_head;
	if (pkt) {
		hw->rx_fallback_head = pkt->next;
		pkt->next = NULL;
	}
	spin_unlock_irqrestore(&hw->lock, flags);

	return pkt;
}

struct crystalhd_rx_dma_pkt *
crystalhd_hw_fetch_free_rx_pkt(struct crystalhd_hw *hw)
{
	struct crystalhd_rx_dma_pkt *pkt;

	if (!hw)
		return NULL;
	pkt = crystalhd_dioq_fetch(hw->rx_freeq);
	return pkt ? pkt : crystalhd_hw_fetch_retained_rx_pkt(hw);
}

uint32_t crystalhd_hw_count_free_rx_pkts(struct crystalhd_hw *hw)
{
	struct crystalhd_rx_dma_pkt *pkt;
	unsigned long flags = 0;
	uint32_t count;

	if (!hw)
		return 0;
	count = hw->rx_freeq ? crystalhd_dioq_count(hw->rx_freeq) : 0;
	spin_lock_irqsave(&hw->lock, flags);
	for (pkt = hw->rx_fallback_head; pkt; pkt = pkt->next)
		count++;
	spin_unlock_irqrestore(&hw->lock, flags);

	return count;
}

/*
 * Call back from TX - IOQ deletion.
 *
 * This routine will release the TX DMA rings allocated
 * druing setup_dma rings interface.
 *
 * Memory is allocated per DMA ring basis. This is just
 * a place holder to be able to create the dio queues.
 */
void crystalhd_tx_desc_rel_call_back(void *context, void *data)
{
}

/*
 * Rx Packet release callback..
 *
 * Release All user mapped capture buffers and Our DMA packets
 * back to our free pool. The actual cleanup of the DMA
 * ring descriptors happen during dma ring release.
 */
void crystalhd_rx_pkt_rel_call_back(void *context, void *data)
{
	struct crystalhd_hw *hw = (struct crystalhd_hw *)context;
	struct crystalhd_rx_dma_pkt *pkt = (struct crystalhd_rx_dma_pkt *)data;
	struct crystalhd_rx_buffer *buffer;

	if (!pkt || !hw) {
		printk(KERN_ERR "%s: Invalid arg - %p %p\n", __func__, hw, pkt);
		return;
	}

	buffer = pkt->buffer;
	crystalhd_hw_free_rx_pkt(hw, pkt);
	if (buffer)
		crystalhd_rx_buffer_release(hw->adp, buffer);
}

#define crystalhd_hw_delete_ioq(adp, q)		\
	if (q) {				\
		crystalhd_delete_dioq(adp, q);	\
		q = NULL;			\
	}

void crystalhd_hw_delete_ioqs(struct crystalhd_hw *hw)
{
	if (!hw)
		return;

	crystalhd_hw_delete_ioq(hw->adp, hw->tx_actq);
	crystalhd_hw_delete_ioq(hw->adp, hw->tx_freeq);
	crystalhd_hw_delete_ioq(hw->adp, hw->rx_actq);
	crystalhd_hw_delete_ioq(hw->adp, hw->rx_freeq);
	crystalhd_hw_delete_ioq(hw->adp, hw->rx_rdyq);
}

static unsigned int crystalhd_hw_detach_rx_owners(
				struct crystalhd_hw *hw,
				struct crystalhd_rx_buffer **retired)
{
	struct crystalhd_dioq *queues[] = {
		hw->rx_actq, hw->rx_rdyq, hw->rx_freeq,
	};
	struct crystalhd_rx_dma_pkt *packet;
	unsigned int retired_count = 0;
	unsigned int queue_index;

	for (queue_index = 0; queue_index < ARRAY_SIZE(queues); queue_index++) {
		if (!queues[queue_index])
			continue;
		while ((packet = crystalhd_dioq_fetch(queues[queue_index])) != NULL) {
			if (packet->buffer && retired_count < BC_RX_LIST_CNT)
				retired[retired_count++] = packet->buffer;
			crystalhd_hw_free_rx_pkt(hw, packet);
		}
	}
	while ((packet = crystalhd_hw_fetch_retained_rx_pkt(hw)) != NULL) {
		if (packet->buffer && retired_count < BC_RX_LIST_CNT)
			retired[retired_count++] = packet->buffer;
		crystalhd_hw_free_rx_pkt(hw, packet);
	}

	return retired_count;
}

void crystalhd_hw_retire_rx_quiesced(struct crystalhd_hw *hw)
{
	struct crystalhd_rx_buffer *retired[BC_RX_LIST_CNT];
	unsigned int retired_count, retired_index;

	if (!hw || !hw->adp)
		return;

	/* DMA and IRQs are already quiesced, and the caller excludes new work.
	 * The fixed packet inventory bounds detachment without waiting for DMA.
	 */
	down(&hw->fetch_sem);
	hw->rx_cancel_epoch++;
	retired_count = crystalhd_hw_detach_rx_owners(hw, retired);
	up(&hw->fetch_sem);

	/* A callback may return backing storage to its frontend. No packet may
	 * retain that frontend's identity, and no capture lock may be held here.
	 */
	for (retired_index = 0; retired_index < retired_count; retired_index++)
		crystalhd_rx_buffer_release(hw->adp, retired[retired_index]);
}

#define crystalhd_hw_create_ioq(sts, hw, q, cb)			\
do {								\
	sts = crystalhd_create_dioq(hw->adp, &q, cb, hw);	\
	if (sts != BC_STS_SUCCESS)				\
		goto hw_create_ioq_err;				\
} while (0)

/*
 * Create IOQs..
 *
 * TX - Active & Free
 * RX - Active, Ready and Free.
 */
BC_STATUS crystalhd_hw_create_ioqs(struct crystalhd_hw *hw)
{
	BC_STATUS   sts = BC_STS_SUCCESS;

	if (!hw) {
		printk(KERN_ERR "%s: Invalid Arg!!\n", __func__);
		return BC_STS_INV_ARG;
	}

	crystalhd_hw_create_ioq(sts, hw, hw->tx_freeq,
			      crystalhd_tx_desc_rel_call_back);
	crystalhd_hw_create_ioq(sts, hw, hw->tx_actq,
			      crystalhd_tx_desc_rel_call_back);

	crystalhd_hw_create_ioq(sts, hw, hw->rx_freeq,
			      crystalhd_rx_pkt_rel_call_back);
	crystalhd_hw_create_ioq(sts, hw, hw->rx_rdyq,
			      crystalhd_rx_pkt_rel_call_back);
	crystalhd_hw_create_ioq(sts, hw, hw->rx_actq,
			      crystalhd_rx_pkt_rel_call_back);

	return sts;

hw_create_ioq_err:
	crystalhd_hw_delete_ioqs(hw);

	return sts;
}

BC_STATUS crystalhd_hw_setup_dma_rings(struct crystalhd_hw *hw)
{
	struct device *dev;
	unsigned int i;
	void *mem;
	size_t mem_len;
	dma_addr_t phy_addr;
	BC_STATUS sts = BC_STS_SUCCESS;
	struct crystalhd_rx_dma_pkt *rpkt;

	if (!hw || !hw->adp) {
		printk(KERN_ERR "%s: Invalid Arguments\n", __func__);
		return BC_STS_INV_ARG;
	}

	dev = &hw->adp->pdev->dev;

	sts = crystalhd_hw_create_ioqs(hw);
	if (sts != BC_STS_SUCCESS) {
		dev_err(dev, "Failed to create IOQs..\n");
		return sts;
	}

	mem_len = BC_LINK_MAX_SGLS * sizeof(struct dma_descriptor);

	for (i = 0; i < BC_TX_LIST_CNT; i++) {
		mem = bc_kern_dma_alloc(hw->adp, mem_len, &phy_addr);
		if (mem) {
			memset(mem, 0, mem_len);
		} else {
			dev_err(dev, "Insufficient Memory For TX\n");
			crystalhd_hw_free_dma_rings(hw);
			return BC_STS_INSUFF_RES;
		}
		/* rx_pkt_pool -- static memory allocation  */
		hw->tx_pkt_pool[i].desc_mem.pdma_desc_start = mem;
		hw->tx_pkt_pool[i].desc_mem.phy_addr = phy_addr;
		hw->tx_pkt_pool[i].desc_mem.sz = BC_LINK_MAX_SGLS *
						 sizeof(struct dma_descriptor);
		hw->tx_pkt_pool[i].list_tag = 0;

		/* Add TX dma requests to Free Queue..*/
		sts = crystalhd_dioq_add(hw->tx_freeq,
				       &hw->tx_pkt_pool[i], false, 0);
		if (sts != BC_STS_SUCCESS) {
			crystalhd_hw_free_dma_rings(hw);
			return sts;
		}
	}

	for (i = 0; i < BC_RX_LIST_CNT; i++) {
		rpkt = kzalloc(sizeof(*rpkt), GFP_KERNEL);
		if (!rpkt) {
			dev_err(dev, "Insufficient Memory For RX\n");
			crystalhd_hw_free_dma_rings(hw);
			return BC_STS_INSUFF_RES;
		}

		mem = bc_kern_dma_alloc(hw->adp, mem_len, &phy_addr);
		if (mem) {
			memset(mem, 0, mem_len);
		} else {
			dev_err(dev, "Insufficient Memory For RX\n");
			kfree(rpkt);
			crystalhd_hw_free_dma_rings(hw);
			return BC_STS_INSUFF_RES;
		}
		rpkt->desc_mem.pdma_desc_start = mem;
		rpkt->desc_mem.phy_addr = phy_addr;
		rpkt->desc_mem.sz  = BC_LINK_MAX_SGLS * sizeof(struct dma_descriptor);
		rpkt->pkt_tag = hw->rx_pkt_tag_seed + i;
		crystalhd_hw_free_rx_pkt(hw, rpkt);
	}

	return BC_STS_SUCCESS;
}

BC_STATUS crystalhd_hw_free_dma_rings(struct crystalhd_hw *hw)
{
	struct crystalhd_rx_buffer *retired[BC_RX_LIST_CNT];
	unsigned int retired_count, retired_index;
	unsigned int i;
	struct crystalhd_rx_dma_pkt *rpkt = NULL;

	if (!hw || !hw->adp) {
		printk(KERN_ERR "%s: Invalid Arguments\n", __func__);
		return BC_STS_INV_ARG;
	}

	/* No frontend release runs until every RX packet has been detached. */
	retired_count = crystalhd_hw_detach_rx_owners(hw, retired);
	crystalhd_hw_delete_ioqs(hw);
	for (retired_index = 0; retired_index < retired_count; retired_index++)
		crystalhd_rx_buffer_release(hw->adp, retired[retired_index]);

	for (i = 0; i < BC_TX_LIST_CNT; i++) {
		if (hw->tx_pkt_pool[i].desc_mem.pdma_desc_start) {
			bc_kern_dma_free(hw->adp,
				hw->tx_pkt_pool[i].desc_mem.sz,
				hw->tx_pkt_pool[i].desc_mem.pdma_desc_start,
				hw->tx_pkt_pool[i].desc_mem.phy_addr);

			hw->tx_pkt_pool[i].desc_mem.pdma_desc_start = NULL;
		}
	}

	dev_dbg(&hw->adp->pdev->dev, "Releasing RX Pkt pool\n");
	for (i = 0; i < BC_RX_LIST_CNT; i++) {
		rpkt = crystalhd_hw_alloc_rx_pkt(hw);
		if (!rpkt)
			break;
		bc_kern_dma_free(hw->adp, rpkt->desc_mem.sz,
				 rpkt->desc_mem.pdma_desc_start,
				 rpkt->desc_mem.phy_addr);
		kfree(rpkt);
	}

	return BC_STS_SUCCESS;
}

static BC_STATUS crystalhd_hw_tx_req_retire(struct crystalhd_hw *hw,
					    struct tx_dma_pkt *tx_req,
					    hw_comp_callback *call_back,
					    void **cb_context)
{
	if (!hw || !tx_req || !call_back || !cb_context)
		return BC_STS_INV_ARG;

	*call_back = tx_req->call_back;
	*cb_context = tx_req->cb_context;
	if (!*call_back || !*cb_context)
		dev_dbg(&hw->adp->pdev->dev, "Missing Tx Callback - %X\n",
		tx_req->list_tag);

	/* Retire common DMA ownership before a frontend completion can release its
	 * backing buffer or its completion context.
	 */
	tx_req->buffer = NULL;
	tx_req->cb_context = NULL;
	tx_req->call_back = NULL;
	tx_req->list_tag = 0;
	return crystalhd_dioq_add(hw->tx_freeq, tx_req, false, 0);
}

BC_STATUS crystalhd_hw_tx_req_complete(struct crystalhd_hw *hw,
					      uint32_t list_id, BC_STATUS cs)
{
	struct tx_dma_pkt *tx_req;
	hw_comp_callback call_back = NULL;
	void *cb_context = NULL;
	BC_STATUS sts;

	if (!hw || !list_id) {
		printk(KERN_ERR "%s: Invalid Arg!!\n", __func__);
		return BC_STS_INV_ARG;
	}

	tx_req = (struct tx_dma_pkt *)crystalhd_dioq_find_and_fetch(hw->tx_actq, list_id);
	if (!tx_req) {
		if (cs != BC_STS_IO_USER_ABORT)
			dev_err(&hw->adp->pdev->dev, "Find/Fetch: no req!\n");
		return BC_STS_NO_DATA;
	}

	sts = crystalhd_hw_tx_req_retire(hw, tx_req, &call_back, &cb_context);

	if (call_back && cb_context)
		call_back(cb_context, cs);

	return sts;
}

BC_STATUS crystalhd_hw_fill_desc(const struct crystalhd_dma_desc_source *source,
				 struct dma_descriptor *desc,
				 dma_addr_t desc_paddr_base,
				 uint32_t sg_cnt, uint32_t sg_st_ix,
				 uint32_t sg_st_off, uint32_t xfr_sz,
				 struct device *dev, uint32_t destDRAMaddr)
{
	struct scatterlist *sg;
	uint32_t count = 0, ix = 0, len = 0, last_desc_ix = 0;
	uint32_t available, sg_xfr_sz;
	dma_addr_t desc_phy_addr = desc_paddr_base;
	addr_64 addr_temp;
	uint32_t curDRAMaddr = destDRAMaddr;

	if (!source || !desc || !desc_paddr_base || !xfr_sz ||
	    (!sg_cnt && !source->fill_size) ||
	    xfr_sz < source->fill_size ||
	    (!!sg_cnt != !!(xfr_sz - source->fill_size)) ||
	    sg_st_ix > source->dma_nents ||
	    sg_cnt > source->dma_nents - sg_st_ix ||
	    (source->fill_size &&
	     (!source->dir_tx || source->fill_size > 3 ||
	      !source->fill_addr || (source->fill_addr & 3)))) {
		dev_err(dev, "%s: Invalid Args\n", __func__);
		return BC_STS_INV_ARG;
	}
	sg_xfr_sz = xfr_sz - source->fill_size;

	sg = source->sgl;
	for (ix = 0; ix < sg_st_ix && sg; ix++)
		sg = sg_next(sg);
	if (sg_cnt && !sg)
		return BC_STS_INV_ARG;

	for (ix = 0; ix < sg_cnt; ix++) {
		if (!sg)
			return BC_STS_INV_ARG;
		available = cpu_to_le32(sg_dma_len(sg));
		len = available;
		if ((sg_dma_address(sg) & 3) || (len & 3)) {
			dev_err(dev, "unsupported len in sg %d %d %d\n",
				len, ix + sg_st_ix, sg_cnt);
			return BC_STS_NOT_IMPL;
		}
		/* Setup DMA desc with Phy addr & Length at current index. */
		addr_temp.full_addr = cpu_to_le64(sg_dma_address(sg));
		if (!ix) {
			if (sg_st_off >= len || (sg_st_off & 3))
				return BC_STS_INV_ARG;
			addr_temp.full_addr += sg_st_off;
			len -= sg_st_off;
		}
		memset(&desc[ix], 0, sizeof(desc[ix]));
		desc[ix].buff_addr_low  = addr_temp.low_part;
		desc[ix].buff_addr_high = addr_temp.high_part;
		desc[ix].dma_dir        = source->dir_tx;

		/* Chain DMA descriptor.  */
		addr_temp.full_addr = desc_phy_addr + sizeof(struct dma_descriptor);
		desc[ix].next_desc_addr_low = addr_temp.low_part;
		desc[ix].next_desc_addr_high = addr_temp.high_part;

		if (count >= sg_xfr_sz)
			return BC_STS_ERROR;
		if (len > sg_xfr_sz - count)
			len = sg_xfr_sz - count;
		if (len > CRYSTALHD_DMA_DESC_MAX_XFER_BYTES)
			return BC_STS_NOT_IMPL;

		/* Debug.. */
		if (!len || len > available) {
			dev_err(dev, "inv-len(%x) Ix(%d) count:%x xfr_sz:%x "
			"sg_cnt:%d\n", len, ix, count, xfr_sz, sg_cnt);
			return BC_STS_ERROR;
		}
		/* Length expects Multiple of 4 */
		desc[ix].xfer_size = (len / 4);

		count += len;
		/* If TX fill in the destination DRAM address if needed */
		if (source->dir_tx) {
			desc[ix].sdram_buff_addr = curDRAMaddr;
			curDRAMaddr = destDRAMaddr + count;
		}
		else
			desc[ix].sdram_buff_addr = 0;

		desc_phy_addr += sizeof(struct dma_descriptor);
		sg = sg_next(sg);
	}
	if (count != sg_xfr_sz)
		return BC_STS_ERROR;

	last_desc_ix = ix ? ix - 1 : 0;

	if (source->fill_size) {
		memset(&desc[ix], 0, sizeof(desc[ix]));
		addr_temp.full_addr     = source->fill_addr;
		desc[ix].buff_addr_low  = addr_temp.low_part;
		desc[ix].buff_addr_high = addr_temp.high_part;
		desc[ix].dma_dir        = source->dir_tx;
		desc[ix].xfer_size	= 1;
		desc[ix].fill_bytes	= 4 - source->fill_size;
		count += source->fill_size;
		/* If TX fill in the destination DRAM address if needed */
		if (source->dir_tx) {
			desc[ix].sdram_buff_addr = curDRAMaddr;
		}
		else
			desc[ix].sdram_buff_addr = 0;
		last_desc_ix = ix;
	}

	/* setup last descriptor..*/
	desc[last_desc_ix].last_rec_indicator  = 1;
	desc[last_desc_ix].next_desc_addr_low  = 0;
	desc[last_desc_ix].next_desc_addr_high = 0;
	desc[last_desc_ix].intr_enable = 1;

	if (count != xfr_sz) {
		dev_err(dev, "interal error sz curr:%x exp:%x\n",
		count, xfr_sz);
		return BC_STS_ERROR;
	}

	return BC_STS_SUCCESS;
}

BC_STATUS crystalhd_xlat_dma_to_desc(
				const struct crystalhd_dma_desc_source *source,
				uint32_t capacity, uint32_t uv_offset,
				uint32_t uv_sg_ix, uint32_t uv_sg_off,
				struct dma_desc_mem *pdesc_mem,
				uint32_t *uv_desc_index,
				struct device *dev, uint32_t destDRAMaddr)
{
	struct dma_descriptor *desc = NULL;
	struct scatterlist *sg;
	dma_addr_t desc_paddr_base = 0;
	uint32_t sg_cnt = 0, sg_st_ix = 0, sg_st_off = 0;
	uint32_t xfr_sz = 0;
	uint32_t desc_count, mapped_sg_count = 0;
	uint32_t sg_len;
	uint32_t i;
	uint64_t available_bytes, required_sg_bytes, uv_backing_offset = 0;
	bool uv_location_seen = false;
	BC_STATUS sts = BC_STS_SUCCESS;

	if (!source || !pdesc_mem || !uv_desc_index) {
		dev_err(dev, "%s: Invalid Args\n", __func__);
		return BC_STS_INV_ARG;
	}

	if (!pdesc_mem->sz || !pdesc_mem->pdma_desc_start || !capacity ||
	    (!source->sgl && source->dma_nents) ||
	    (!source->dma_nents && !source->fill_size) ||
	    source->fill_size > capacity ||
	    (source->fill_size &&
	     (!source->dir_tx || source->fill_size > 3 ||
	      source->fill_size != (capacity & 3) || !source->fill_addr ||
	      (source->fill_addr & 3))) ||
	    (!source->fill_size && (capacity & 3)) ||
	    (uv_offset && (source->dir_tx || uv_offset >= capacity ||
			   uv_sg_ix >= source->dma_nents ||
			   ((uv_offset | uv_sg_off) & 3))) ||
	    (!uv_offset && (uv_sg_ix || uv_sg_off))) {
		dev_err(dev, "%s: Invalid Args\n", __func__);
		return BC_STS_INV_ARG;
	}
	required_sg_bytes = capacity - source->fill_size;
	available_bytes = 0;
	sg = source->sgl;
	for (i = 0; i < source->dma_nents; i++) {
		if (!sg)
			return BC_STS_INV_ARG;
		sg_len = cpu_to_le32(sg_dma_len(sg));
		if (!sg_len || (sg_len & 3) || (sg_dma_address(sg) & 3))
			return BC_STS_NOT_IMPL;
		if (uv_offset && i == uv_sg_ix) {
			if (uv_sg_off >= sg_len)
				return BC_STS_INV_ARG;
			uv_backing_offset = available_bytes + uv_sg_off;
			uv_location_seen = true;
		}
		available_bytes += sg_len;
		if (!mapped_sg_count && available_bytes >= required_sg_bytes)
			mapped_sg_count = i + 1;
		sg = sg_next(sg);
	}
	if (available_bytes < required_sg_bytes ||
	    (required_sg_bytes && !mapped_sg_count) ||
	    (uv_offset && (!uv_location_seen ||
			   uv_backing_offset != uv_offset)))
		return BC_STS_INV_ARG;

	desc = pdesc_mem->pdma_desc_start;
	desc_paddr_base = pdesc_mem->phy_addr;
	*uv_desc_index = 0;
	/* A split inside a mapped segment needs a second descriptor; a TX
	 * partial word likewise consumes one coherent fill-byte descriptor.
	 */
	if (uv_offset)
		desc_count = uv_sg_ix + !!uv_sg_off +
			     mapped_sg_count - uv_sg_ix;
	else
		desc_count = mapped_sg_count + !!source->fill_size;
	if (desc_count > pdesc_mem->sz / sizeof(*desc))
		return BC_STS_INSUFF_RES;

	if (source->dir_tx || !uv_offset) {
		sg_cnt = mapped_sg_count;
		xfr_sz = capacity;
	} else {
		sg_cnt = uv_sg_ix + !!uv_sg_off;
		xfr_sz = uv_offset;
	}

	sts = crystalhd_hw_fill_desc(source, desc, desc_paddr_base, sg_cnt,
				     sg_st_ix, sg_st_off, xfr_sz, dev,
				     destDRAMaddr);

	if (sts != BC_STS_SUCCESS || !uv_offset)
		return sts;

	/* Prepare for UV mapping.. */
	*uv_desc_index = sg_cnt;
	desc = &pdesc_mem->pdma_desc_start[sg_cnt];
	desc_paddr_base = pdesc_mem->phy_addr +
	(sg_cnt * sizeof(struct dma_descriptor));

	/* Done with desc addr.. now update sg stuff.*/
	sg_cnt    = mapped_sg_count - uv_sg_ix;
	xfr_sz    = capacity - uv_offset;
	sg_st_ix  = uv_sg_ix;
	sg_st_off = uv_sg_off;

	return crystalhd_hw_fill_desc(source, desc, desc_paddr_base, sg_cnt,
				      sg_st_ix, sg_st_off, xfr_sz, dev,
				      destDRAMaddr);
}

static BC_STATUS crystalhd_tx_buffer_preflight(
					 const struct crystalhd_tx_buffer *buffer,
					 uint32_t max_descriptors)
{
	struct scatterlist *sg;
	uint64_t available = 0;
	uint32_t required, mapped_nents = 0;
	uint32_t i, len;

	if (!buffer || !buffer->bytes || !buffer->cookie ||
	    buffer->tail_size > 3 ||
	    buffer->tail_size != (buffer->bytes & 3) ||
	    (buffer->tail_size && (!buffer->tail_addr ||
				    (buffer->tail_addr & 3))))
		return BC_STS_INV_ARG;

	required = buffer->bytes - buffer->tail_size;
	if (!!required != !!buffer->dma_nents ||
	    (required && !buffer->sgl))
		return BC_STS_INV_ARG;

	sg = buffer->sgl;
	for (i = 0; i < buffer->dma_nents; i++) {
		if (!sg)
			return BC_STS_INV_ARG;
		len = sg_dma_len(sg);
		if (!len || (len & 3) || (sg_dma_address(sg) & 3))
			return BC_STS_NOT_IMPL;
		/* A larger backing segment is valid when bytes ends inside it. */
		if (available < required &&
		    len > CRYSTALHD_DMA_DESC_MAX_XFER_BYTES &&
		    required - available > CRYSTALHD_DMA_DESC_MAX_XFER_BYTES)
			return BC_STS_NOT_IMPL;
		available += len;
		if (!mapped_nents && available >= required)
			mapped_nents = i + 1;
		sg = sg_next(sg);
	}
	if (available < required || (required && !mapped_nents))
		return BC_STS_INV_ARG;
	if (mapped_nents + !!buffer->tail_size > max_descriptors)
		return BC_STS_INSUFF_RES;

	return BC_STS_SUCCESS;
}

BC_STATUS crystalhd_xlat_tx_buffer_to_dma_desc(
					 const struct crystalhd_tx_buffer *buffer,
					 struct dma_desc_mem *pdesc_mem,
					 uint32_t *uv_desc_index,
					 struct device *dev,
					 uint32_t destDRAMaddr)
{
	struct crystalhd_dma_desc_source source;
	BC_STATUS sts;

	if (!pdesc_mem || !uv_desc_index)
		return BC_STS_INV_ARG;
	sts = crystalhd_tx_buffer_preflight(buffer,
			pdesc_mem->sz / sizeof(struct dma_descriptor));
	if (sts != BC_STS_SUCCESS)
		return sts;

	source.sgl = buffer->sgl;
	source.dma_nents = buffer->dma_nents;
	source.dir_tx = true;
	source.fill_addr = buffer->tail_addr;
	source.fill_size = buffer->tail_size;

	return crystalhd_xlat_dma_to_desc(&source, buffer->bytes, 0, 0, 0,
					  pdesc_mem, uv_desc_index, dev,
					  destDRAMaddr);
}

BC_STATUS crystalhd_xlat_rx_buffer_to_dma_desc(
					struct crystalhd_rx_buffer *buffer,
					struct dma_desc_mem *pdesc_mem,
					uint32_t *uv_desc_index,
					struct device *dev)
{
	struct crystalhd_dma_desc_source source;

	if (!buffer)
		return BC_STS_INV_ARG;

	source.sgl = buffer->sgl;
	source.dma_nents = buffer->dma_nents;
	source.dir_tx = false;
	source.fill_addr = 0;
	source.fill_size = 0;

	return crystalhd_xlat_dma_to_desc(&source, buffer->capacity,
					  buffer->uv_offset,
					  buffer->uv_sg_ix,
					  buffer->uv_sg_off,
					  pdesc_mem, uv_desc_index, dev, 0);
}

struct crystalhd_rx_dma_pkt *crystalhd_rx_pkt_detach(struct crystalhd_hw *hw,
							     uint32_t list_index,
							     BC_STATUS comp_sts)
{
	struct crystalhd_rx_dma_pkt *rx_pkt = NULL;

	if (!hw || list_index >= DMA_ENGINE_CNT) {
		printk(KERN_ERR "%s: Invalid Arguments\n", __func__);
		return NULL;
	}

	rx_pkt = crystalhd_dioq_find_and_fetch(hw->rx_actq,
		hw->rx_pkt_tag_seed + list_index);
	if (!rx_pkt) {
		dev_err(&hw->adp->pdev->dev, "Act-Q: PostIx:%x L0Sts:%x "
			"L1Sts:%x current L:%x tag:%x comp:%x\n",
			hw->rx_list_post_index, hw->rx_list_sts[0],
			hw->rx_list_sts[1], list_index,
			hw->rx_pkt_tag_seed + list_index, comp_sts);
	}

	return rx_pkt;
}

BC_STATUS crystalhd_rx_pkt_complete(struct crystalhd_hw *hw,
					    struct crystalhd_rx_dma_pkt *rx_pkt,
					    uint32_t list_index,
					    BC_STATUS comp_sts)
{
	uint32_t y_dw_dnsz, uv_dw_dnsz;
	BC_STATUS sts = BC_STS_SUCCESS;
	uint64_t currTick;

	uint32_t totalTick_Hi;
	uint32_t TickSpentInPD_Hi;
	uint64_t temp_64;
	int32_t totalTick_Hi_f;
	int32_t TickSpentInPD_Hi_f;

	if (!hw || !rx_pkt || !rx_pkt->buffer || !rx_pkt->cookie ||
	    list_index >= DMA_ENGINE_CNT) {
		printk(KERN_ERR "%s: Invalid Arguments\n", __func__);
		return BC_STS_INV_ARG;
	}

	/* Recycled free-queue packets can bypass allocation before another DMA. */
	memset(&rx_pkt->metadata, 0, sizeof(rx_pkt->metadata));

	if (comp_sts == BC_STS_SUCCESS)
	{
		hw->DrvTotalFrmCaptured++;

		hw->pfnHWGetDoneSize(hw, list_index, &y_dw_dnsz, &uv_dw_dnsz);
		rx_pkt->y_done_sz = y_dw_dnsz;
		rx_pkt->uv_done_sz = 0;
		rx_pkt->flags = COMP_FLAG_DATA_VALID;
		if (rx_pkt->uv_phy_addr)
			rx_pkt->uv_done_sz = uv_dw_dnsz;
		sts = crystalhd_dioq_add(hw->rx_rdyq, rx_pkt, true,
					 hw->rx_pkt_tag_seed + list_index);
		if (sts != BC_STS_SUCCESS) {
			/* Keep the buffer reachable for process-context cancellation. */
			crystalhd_hw_retain_rx_pkt(hw, rx_pkt);
			return sts;
		}

		if( hw->adp->pdev->device == BC_PCI_DEVID_FLEA)
		{
			/*printk("pre-PD state %x RLL %x Ptsh %x ratio %d currentPS %d\n", */
			/*	hw->FleaPowerState, crystalhd_dioq_count(hw->rx_rdyq) , hw->PauseThreshold, hw->PDRatio, hw->FleaPowerState); */
			if(hw->FleaPowerState == FLEA_PS_ACTIVE)
			{
				if(crystalhd_dioq_count(hw->rx_rdyq) >= hw->PauseThreshold)
				{
					hw->pfnIssuePause(hw, true);
					hw->hw_pause_issued = true;
				}
				/* NAREN check if the PD ratio is less than 50. If so, try to reduce the PauseThreshold to improve the ratio */
				/* never go lower than 6 pictures */
				/* Only do this when we have some data to determine PDRatio */
				/* For now assume that if we have captured 100 pictures then we should have enough data for the analysis to start */
				if((hw->PDRatio < 50) && (hw->PauseThreshold > 6) && (hw->DrvTotalFrmCaptured > 100))
				{
					/*printk("Current PDRatio:%u, PauseThreshold:%u, DrvTotalFrmCaptured:%u  decress PauseThreshold\n", */
					/*	hw->PDRatio, hw->PauseThreshold, hw->DrvTotalFrmCaptured); */
					hw->PauseThreshold--;
				}
				else {
					currTick = rdtsc_ordered();

					temp_64 = (hw->TickSpentInPD)>>24;
					TickSpentInPD_Hi = (uint32_t)(temp_64);
					TickSpentInPD_Hi_f = (int32_t)TickSpentInPD_Hi;

					temp_64 = (currTick - hw->TickCntDecodePU)>>24;
					totalTick_Hi = (uint32_t)(temp_64);
					totalTick_Hi_f = (int32_t)totalTick_Hi;

					if( totalTick_Hi_f <= 0 )
					{
						temp_64 = (hw->TickSpentInPD);
						TickSpentInPD_Hi = (uint32_t)(temp_64);
						TickSpentInPD_Hi_f = (int32_t)TickSpentInPD_Hi;

						temp_64 = (currTick - hw->TickCntDecodePU);
						totalTick_Hi = (uint32_t)(temp_64);
						totalTick_Hi_f = (int32_t)totalTick_Hi;
					}

					if( totalTick_Hi_f <= 0 )
					{
						printk("totalTick_Hi_f <= 0, set hw->PDRatio = 60\n");
						hw->PDRatio = 60;
					}
					else
						hw->PDRatio = (TickSpentInPD_Hi_f * 100) / totalTick_Hi_f;

					/*printk("Current PDRatio:%u, PauseThreshold:%u, DrvTotalFrmCaptured:%u  don't decress PauseThreshold\n", */
					/*	hw->PDRatio, hw->PauseThreshold, hw->DrvTotalFrmCaptured); */

					/*hw->PDRatio = ((uint32_t)(hw->TickSpentInPD))/((uint32_t)(currTick - hw->TickCntDecodePU)/100); */
				}
			}
		}
		else if( hw->hw_pause_issued == false )
		{
#if 0
			if(crystalhd_dioq_count(hw->rx_rdyq) > hw->PauseThreshold)/*HW_PAUSE_THRESHOLD */
			{
				dev_info(&hw->adp->pdev->dev, "HW PAUSE\n");
				hw->pfnIssuePause(hw, true);
				hw->hw_pause_issued = true;
			}
#endif
		}

		return sts;
	}
	/* Check if we can post this capture buffer again. */
	return crystalhd_hw_repost_cap_buffer(hw, rx_pkt);
}

BC_STATUS crystalhd_rx_pkt_done(struct crystalhd_hw *hw,
					uint32_t list_index,
					BC_STATUS comp_sts)
{
	struct crystalhd_rx_dma_pkt *rx_pkt;

	rx_pkt = crystalhd_rx_pkt_detach(hw, list_index, comp_sts);
	if (!rx_pkt)
		return BC_STS_INV_ARG;

	return crystalhd_rx_pkt_complete(hw, rx_pkt, list_index, comp_sts);
}

BC_STATUS crystalhd_hw_post_tx(struct crystalhd_hw *hw,
			     const struct crystalhd_tx_buffer *buffer,
			     hw_comp_callback call_back, void *cb_context,
			     uint32_t *list_id, uint8_t data_flags)
{
	struct device *dev;
	struct tx_dma_pkt *tx_dma_packet = NULL;
	addr_64 desc_addr;
	BC_STATUS sts, add_sts;
	uint32_t dummy_index = 0;
	unsigned long flags;
	uint8_t list_posted;
	uint8_t local_flags = data_flags;
	bool rc;
	uint32_t destDRAMaddr = 0;

	if (!hw || !buffer || !call_back || !cb_context || !list_id) {
		printk(KERN_ERR "%s: Invalid Arguments\n", __func__);
		return BC_STS_INV_ARG;
	}
	sts = crystalhd_tx_buffer_preflight(buffer, BC_LINK_MAX_SGLS);
	if (sts != BC_STS_SUCCESS)
		return sts;

	dev = &hw->adp->pdev->dev;

	/*
	 * Since we hit code in busy condition very frequently,
	 * we will check the code in status first before
	 * checking the availability of free elem.
	 *
	 * This will avoid the Q fetch/add in normal condition.
	 */
	if (READ_ONCE(hw->dma_fault))
		return BC_STS_IO_ERROR;

	rc = hw->pfnCheckInputFIFO(hw, buffer->bytes,
					   &dummy_index, false, &local_flags);

	if (rc) {
		hw->stats.cin_busy++;
		return BC_STS_BUSY;
	}

	if(local_flags & BC_BIT(7))
		destDRAMaddr = hw->TxFwInputBuffInfo.DramBuffAdd;

	/* Get a list from TxFreeQ */
	tx_dma_packet = (struct tx_dma_pkt *)crystalhd_dioq_fetch(hw->tx_freeq);
	if (!tx_dma_packet) {
		dev_err(dev, "No empty elements..\n");
		return BC_STS_INSUFF_RES;
	}

	sts = crystalhd_xlat_tx_buffer_to_dma_desc(buffer,
						   &tx_dma_packet->desc_mem,
						   &dummy_index, dev,
						   destDRAMaddr);
	if (sts != BC_STS_SUCCESS) {
		add_sts = crystalhd_dioq_add(hw->tx_freeq, tx_dma_packet,
					   false, 0);
		if (add_sts != BC_STS_SUCCESS)
			dev_err(dev, "double fault..\n");

		return sts;
	}

	desc_addr.full_addr = tx_dma_packet->desc_mem.phy_addr;

	tx_dma_packet->call_back = call_back;
	tx_dma_packet->cb_context = cb_context;
	tx_dma_packet->buffer = buffer;

	spin_lock_irqsave(&hw->lock, flags);

	list_posted = hw->tx_list_post_index;

	*list_id = tx_dma_packet->list_tag = hw->tx_ioq_tag_seed +
					     hw->tx_list_post_index;

	/* Keep the request reachable before enabling the DMA engine. */
	sts = crystalhd_dioq_add(hw->tx_actq, tx_dma_packet, false,
				 tx_dma_packet->list_tag);
	if (sts != BC_STS_SUCCESS) {
		tx_dma_packet->buffer = NULL;
		tx_dma_packet->cb_context = NULL;
		tx_dma_packet->call_back = NULL;
		tx_dma_packet->list_tag = 0;
		*list_id = 0;
		spin_unlock_irqrestore(&hw->lock, flags);
		crystalhd_dioq_add(hw->tx_freeq, tx_dma_packet, false, 0);
		return sts;
	}

	if( hw->tx_list_post_index % DMA_ENGINE_CNT) {
		hw->TxList1Sts |= TxListWaitingForIntr;
	}
	else {
		hw->TxList0Sts |= TxListWaitingForIntr;
	}

	hw->tx_list_post_index = (hw->tx_list_post_index + 1) % DMA_ENGINE_CNT;

	/*
	 * Interrupt will come as soon as you write
	 * the valid bit. So be ready for that. All
	 * the initialization should happen before that.
	 */

	/* Save the transfer length */
	hw->TxFwInputBuffInfo.HostXferSzInBytes = buffer->bytes;

	hw->pfnStartTxDMA(hw, list_posted, desc_addr);

	spin_unlock_irqrestore(&hw->lock, flags);

	return BC_STS_SUCCESS;
}

/* Stop the shared TX engine and return every list owner exactly once. */
BC_STATUS crystalhd_hw_cancel_all_tx(struct crystalhd_hw *hw)
{
	struct tx_dma_pkt *tx_req;
	hw_comp_callback call_back[DMA_ENGINE_CNT] = { NULL };
	void *cb_context[DMA_ENGINE_CNT] = { NULL };
	BC_STATUS sts = BC_STS_SUCCESS;
	BC_STATUS retire_sts;
	unsigned int count;
	unsigned int i;

	if (!hw) {
		printk(KERN_ERR "%s: Invalid Arguments\n", __func__);
		return BC_STS_INV_ARG;
	}

	/* Backend stop callbacks sleep. The caller excludes process submissions;
	 * drain the ISR before detaching either fixed-list owner.
	 */
	disable_irq(hw->adp->pdev->irq);
	if (hw->pfnStopTxDMA(hw) != BC_STS_SUCCESS)
		crystalhd_hw_dma_fatal_stop(hw);

	for (count = 0; count < DMA_ENGINE_CNT; count++) {
		tx_req = (struct tx_dma_pkt *)crystalhd_dioq_fetch(hw->tx_actq);
		if (!tx_req)
			break;
		retire_sts = crystalhd_hw_tx_req_retire(hw, tx_req,
							 &call_back[count],
							 &cb_context[count]);
		if (sts == BC_STS_SUCCESS && retire_sts != BC_STS_SUCCESS)
			sts = retire_sts;
	}

	for (i = 0; i < count; i++) {
		if (call_back[i] && cb_context[i])
			call_back[i](cb_context[i], BC_STS_IO_USER_ABORT);
	}
	enable_irq(hw->adp->pdev->irq);
	/* Drain a completion latched while the engine was stopping before the
	 * caller releases TX serialization and a fixed list tag can be reused.
	 */
	synchronize_irq(hw->adp->pdev->irq);

	return hw->dma_fault ? BC_STS_IO_ERROR : sts;
}

void crystalhd_hw_dma_fatal_stop(struct crystalhd_hw *hw)
{
	/* Do not release DMA pages while a timed-out engine can issue more
	 * transactions. Recovery requires closing and resetting the session.
	 */
	WRITE_ONCE(hw->dma_fault, true);
	pci_clear_master(hw->adp->pdev);
	if (!pci_wait_for_pending_transaction(hw->adp->pdev))
		dev_err(&hw->adp->pdev->dev, "PCI transactions did not drain after DMA stop\n");
	dev_err(&hw->adp->pdev->dev, "DMA disabled after stop failure; reopen the device\n");
}

BC_STATUS crystalhd_hw_add_cap_buffer(struct crystalhd_hw *hw,
				    struct crystalhd_rx_buffer *buffer,
				    bool en_post)
{
	struct crystalhd_rx_dma_pkt *rpkt;
	uint32_t tag, uv_desc_ix = 0;
	BC_STATUS sts;

	if (!hw || !buffer || !buffer->sgl || !buffer->dma_nents ||
	    !buffer->capacity || !buffer->ops || !buffer->cookie ||
	    (buffer->output_format == MODE420 && !buffer->uv_offset) ||
	    (buffer->output_format != MODE420 &&
	     buffer->output_format != MODE422_YUY2 &&
	     buffer->output_format != MODE422_UYVY) ||
	    (buffer->output_format != MODE420 && buffer->uv_offset) ||
	    !buffer->ops->sync_for_cpu || !buffer->ops->sync_for_device ||
	    !buffer->ops->read || !buffer->ops->write ||
	    !buffer->ops->release) {
		printk(KERN_ERR "%s: Invalid Arguments\n", __func__);
		return BC_STS_INV_ARG;
	}

	rpkt = crystalhd_hw_alloc_rx_pkt(hw);
	if (!rpkt) {
		dev_err(&hw->adp->pdev->dev, "Insufficient resources\n");
		return BC_STS_INSUFF_RES;
	}

	rpkt->buffer = buffer;
	rpkt->cookie = buffer->cookie;
	rpkt->capture_epoch = hw->rx_cancel_epoch;
	tag = rpkt->pkt_tag;

	sts = crystalhd_xlat_rx_buffer_to_dma_desc(buffer, &rpkt->desc_mem,
						   &uv_desc_ix,
						   &hw->adp->pdev->dev);
	if (sts != BC_STS_SUCCESS)
		goto release_packet;

	rpkt->uv_phy_addr = 0;

	/* Store the address of UV in the rx packet for post*/
	if (uv_desc_ix)
		rpkt->uv_phy_addr = rpkt->desc_mem.phy_addr +
				    (sizeof(struct dma_descriptor) * uv_desc_ix);

	if (en_post && !hw->hw_pause_issued) {
		sts = hw->pfnPostRxSideBuff(hw, rpkt);
	}
	else {
		sts = crystalhd_dioq_add(hw->rx_freeq, rpkt, false, tag);
		hw->pfnNotifyFLLChange(hw, false);
	}

	if (sts == BC_STS_SUCCESS || sts == BC_STS_BUSY)
		return sts;
release_packet:
	/* On errors ownership of the buffer remains with the caller. */
	crystalhd_hw_free_rx_pkt(hw, rpkt);
	return sts;
}

static BC_STATUS crystalhd_hw_complete_rx_locked(struct crystalhd_hw *hw,
						 struct crystalhd_rx_completion *result,
						 struct crystalhd_rx_dma_pkt *rpkt,
						 uint64_t expected_epoch,
						 BC_STATUS empty_status)
{
	bool resume_allowed;

	/* A destructive stop may have completed after the ready packet was
	 * detached. Do not let that stale completion wake capture afterward.
	 */
	resume_allowed = rpkt ?
			 rpkt->capture_epoch == hw->rx_cancel_epoch :
			 expected_epoch == hw->rx_cancel_epoch;

	if (resume_allowed && hw->adp->pdev->device == BC_PCI_DEVID_FLEA)
	{
		/*printk("pre-PU state %x RLL %x Rtsh %x, currentPS %d,\n", */
		/*	hw->FleaPowerState, crystalhd_dioq_count(hw->rx_rdyq) , hw->ResumeThreshold, hw->FleaPowerState); */
		if( (hw->FleaPowerState == FLEA_PS_LP_PENDING) ||
			(hw->FleaPowerState == FLEA_PS_LP_COMPLETE))
		{
			if (crystalhd_dioq_count(hw->rx_rdyq)  <= hw->ResumeThreshold) {
				hw->pfnIssuePause(hw, false);	/*Need this Notification For Flea*/
				hw->hw_pause_issued = false;
			}
		}
	}
	else if (resume_allowed && hw->hw_pause_issued)
	{
#if 0
		if(crystalhd_dioq_count(hw->rx_rdyq) < hw->PauseThreshold ) /*HW_RESUME_THRESHOLD */
		{
			dev_info(&hw->adp->pdev->dev, "HW RESUME with rdy list %u \n",crystalhd_dioq_count(hw->rx_rdyq));
			hw->pfnIssuePause(hw, false);
			hw->hw_pause_issued = false;
		}
#endif
	}

	if (!rpkt)
		return empty_status;

	if (rpkt->flags & COMP_FLAG_PIB_VALID)
	{
		result->pib.ppb.picture_number = rpkt->pib.picture_number;
		result->pib.ppb.width = rpkt->pib.width;
		result->pib.ppb.height = rpkt->pib.height;
		result->pib.ppb.chroma_format = rpkt->pib.chroma_format;
		result->pib.ppb.pulldown = rpkt->pib.pulldown;
		result->pib.ppb.flags = rpkt->pib.flags;
		result->pib.ptsStcOffset = rpkt->pib.sess_num;
		result->pib.ppb.aspect_ratio = rpkt->pib.aspect_ratio;
		result->pib.ppb.colour_primaries =
			rpkt->pib.colour_primaries;
		result->pib.ppb.picture_meta_payload =
			rpkt->pib.picture_meta_payload;
		result->pib.resolution = rpkt->pib.frame_rate;
	}

	if (rpkt->metadata.valid && !(rpkt->flags & COMP_FLAG_FMT_CHANGE))
		result->metadata = rpkt->metadata;

	result->buffer = rpkt->buffer;
	result->cookie = rpkt->cookie;
	result->capture_epoch = rpkt->capture_epoch;
	result->flags = rpkt->flags;
	result->y_done_sz = rpkt->y_done_sz;
	result->uv_done_sz = rpkt->uv_done_sz;

	crystalhd_hw_free_rx_pkt(hw, rpkt);

	return BC_STS_SUCCESS;
}

BC_STATUS crystalhd_hw_get_cap_buffer(struct crystalhd_hw *hw,
				      struct crystalhd_rx_completion *result,
				      uint64_t expected_epoch)
{
	struct crystalhd_rx_dma_pkt *rpkt;
	uint32_t sig_pending = 0;
	BC_STATUS sts;

	if (!result)
		return BC_STS_INV_ARG;
	memset(result, 0, sizeof(*result));
	if (!hw)
		return BC_STS_INV_ARG;

	rpkt = crystalhd_dioq_fetch_wait(hw, BC_PROC_OUTPUT_TIMEOUT / 1000,
					 &sig_pending);
	/* Preserve the legacy wait and detached-packet ownership semantics. */
	down(&hw->fetch_sem);
	sts = crystalhd_hw_complete_rx_locked(hw, result, rpkt, expected_epoch,
					      sig_pending ? BC_STS_IO_USER_ABORT :
							    BC_STS_TIMEOUT);
	up(&hw->fetch_sem);
	return sts;
}

/* No wait for picture arrival; semaphore acquisition and hardware callbacks
 * may sleep. Caller retains device/session lifetime through buffer retirement.
 */
BC_STATUS crystalhd_hw_try_get_cap_buffer(struct crystalhd_hw *hw,
					  struct crystalhd_rx_completion *result,
					  uint64_t expected_epoch)
{
	struct crystalhd_rx_buffer *retired = NULL;
	struct crystalhd_rx_dma_pkt *rpkt = NULL;
	BC_STATUS sts;

	if (!result)
		return BC_STS_INV_ARG;
	memset(result, 0, sizeof(*result));
	if (!hw || !hw->adp || !hw->adp->pdev || !hw->rx_rdyq)
		return BC_STS_INV_ARG;
	if (down_interruptible(&hw->fetch_sem))
		return BC_STS_IO_USER_ABORT;
	if (READ_ONCE(hw->dma_fault)) {
		sts = BC_STS_IO_ERROR;
		goto out;
	}
	if (!READ_ONCE(hw->adp->present) ||
	    expected_epoch != hw->rx_cancel_epoch) {
		sts = BC_STS_IO_USER_ABORT;
		goto out;
	}
	rpkt = crystalhd_dioq_try_fetch_locked(hw);
	if (READ_ONCE(hw->dma_fault)) {
		sts = BC_STS_IO_ERROR;
		goto out;
	}
	if (!READ_ONCE(hw->adp->present) ||
	    expected_epoch != hw->rx_cancel_epoch ||
	    (rpkt && rpkt->capture_epoch != expected_epoch)) {
		sts = BC_STS_IO_USER_ABORT;
		goto out;
	}
	sts = crystalhd_hw_complete_rx_locked(hw, result, rpkt, expected_epoch,
					      BC_STS_NO_DATA);
	rpkt = NULL;
out:
	if (rpkt) {
		retired = rpkt->buffer;
		crystalhd_hw_free_rx_pkt(hw, rpkt);
	}
	up(&hw->fetch_sem);
	if (retired)
		crystalhd_rx_buffer_release(hw->adp, retired);
	return sts;
}

BC_STATUS crystalhd_hw_repost_cap_buffer(struct crystalhd_hw *hw,
					 struct crystalhd_rx_dma_pkt *pkt)
{
	BC_STATUS sts = hw->pfnPostRxSideBuff(hw, pkt);

	/* Internal retries already own this registration. On a hard error
	 * retain it for process-context flush/close: dirty unpinning can sleep.
	 * SUCCESS and BUSY have already transferred ownership to a DMA queue.
	 */
	if (sts != BC_STS_SUCCESS && sts != BC_STS_BUSY &&
	    crystalhd_dioq_add(hw->rx_freeq, pkt, false,
				pkt->pkt_tag) != BC_STS_SUCCESS)
		crystalhd_hw_retain_rx_pkt(hw, pkt);
	return sts;
}

BC_STATUS crystalhd_hw_start_capture(struct crystalhd_hw *hw)
{
	struct crystalhd_rx_dma_pkt *rx_pkt;
	BC_STATUS sts;
	uint32_t i;

	if (!hw) {
		printk(KERN_ERR "%s: Invalid Arguments\n", __func__);
		return BC_STS_INV_ARG;
	}
	if (hw->dma_fault)
		return BC_STS_IO_ERROR;

	/* This is start of capture.. Post to both the lists.. */
	for (i = 0; i < DMA_ENGINE_CNT; i++) {
		rx_pkt = crystalhd_hw_fetch_free_rx_pkt(hw);
		if (!rx_pkt)
			return BC_STS_NO_DATA;
		sts = crystalhd_hw_repost_cap_buffer(hw, rx_pkt);
		if (sts == BC_STS_BUSY)
			break; /* the hardware wrapper queued it for a later retry */
		if (sts != BC_STS_SUCCESS)
			return sts;

	}

	return BC_STS_SUCCESS;
}

/* The caller holds fetch_sem across any state change and restart. */
BC_STATUS crystalhd_hw_stop_capture_locked(struct crystalhd_hw *hw, bool unmap)
{
	struct crystalhd_rx_buffer *retired[BC_RX_LIST_CNT];
	unsigned int retired_count;
	unsigned int queue_index;
	struct crystalhd_rx_dma_pkt *packet;

	if (!hw) {
		printk(KERN_ERR "%s: Invalid Arguments\n", __func__);
		return BC_STS_INV_ARG;
	}
	/* A monitor handle never creates capture queues or posts DMA. */
	if (!hw->rx_actq && !hw->rx_rdyq && !hw->rx_freeq)
		return BC_STS_SUCCESS;
	/* A fetch may own a mapped packet outside every queue. Invalidate it
	 * before a destructive RX stop/cancel attempt so it cannot be requeued.
	 */
	if (unmap)
		hw->rx_cancel_epoch++;

	/* No completion or repost may race with queue draining and unpinning. */
	disable_irq(hw->adp->pdev->irq);
	hw->pfnStopRXDMAEngines(hw);

	/* A failed stop keeps registrations reachable until session teardown. */
	if (READ_ONCE(hw->dma_fault))
		goto out;
	if (!unmap) {
		while ((packet = crystalhd_dioq_fetch(hw->rx_actq)) != NULL) {
			if (crystalhd_dioq_add(hw->rx_freeq, packet, false,
						packet->pkt_tag) != BC_STS_SUCCESS)
				crystalhd_hw_retain_rx_pkt(hw, packet);
		}
		while ((packet = crystalhd_dioq_fetch(hw->rx_rdyq)) != NULL) {
			if (crystalhd_dioq_add(hw->rx_freeq, packet, false,
						packet->pkt_tag) != BC_STS_SUCCESS)
				crystalhd_hw_retain_rx_pkt(hw, packet);
		}
		goto out;
	}

	retired_count = crystalhd_hw_detach_rx_owners(hw, retired);
	for (queue_index = 0; queue_index < retired_count; queue_index++)
		crystalhd_rx_buffer_release(hw->adp, retired[queue_index]);

out:
	enable_irq(hw->adp->pdev->irq);
	return hw->dma_fault ? BC_STS_IO_ERROR : BC_STS_SUCCESS;
}

BC_STATUS crystalhd_hw_stop_capture(struct crystalhd_hw *hw, bool unmap)
{
	BC_STATUS sts;

	if (!hw)
		return BC_STS_INV_ARG;
	down(&hw->fetch_sem);
	sts = crystalhd_hw_stop_capture_locked(hw, unmap);
	up(&hw->fetch_sem);
	return sts;
}

BC_STATUS crystalhd_hw_suspend(struct crystalhd_hw *hw)
{
	BC_STATUS sts = BC_STS_SUCCESS;

	if (!hw) {
		printk(KERN_ERR "%s: Invalid Arguments\n", __func__);
		return BC_STS_INV_ARG;
	}

	/* Exclude command pre/post processing as well as the mailbox wait before
	 * resetting the device. This also protects non-PCI-PM stop callers.
	 */
	mutex_lock(&hw->fwcmd_trans_mutex);
	if (!hw->pfnStopDevice(hw)) {
		dev_info(&hw->adp->pdev->dev, "Failed to Stop Device!!\n");
		sts = BC_STS_ERROR;
	}
	mutex_unlock(&hw->fwcmd_trans_mutex);

	return sts;
}

BC_STATUS crystalhd_hw_resume(struct crystalhd_hw *hw)
{
	if (!hw) {
		printk(KERN_ERR "%s: Invalid Arguments\n", __func__);
		return BC_STS_INV_ARG;
	}
	if (READ_ONCE(hw->dma_fault))
		return BC_STS_IO_ERROR;

	mutex_lock(&hw->fwcmd_trans_mutex);
	// Reset list state
	hw->rx_list_sts[0] = sts_free;
	hw->rx_list_sts[1] = sts_free;
	hw->TxList0Sts = ListStsFree;
	hw->TxList1Sts = ListStsFree;
	hw->rx_list_post_index = 0;
	hw->tx_list_post_index = 0;

	if (!hw->pfnStartDevice(hw)) {
		dev_info(&hw->adp->pdev->dev, "Failed to Start Device!!\n");
		mutex_unlock(&hw->fwcmd_trans_mutex);
		return BC_STS_ERROR;
	}
	crystalhd_hw_fw_cmd_reset_locked(hw);
	mutex_unlock(&hw->fwcmd_trans_mutex);

	return BC_STS_SUCCESS;
}

void crystalhd_hw_stats(struct crystalhd_hw *hw, struct crystalhd_hw_stats *stats)
{
	if (!hw) {
		printk(KERN_ERR "%s: Invalid Arguments\n", __func__);
		return;
	}

	/* if called w/NULL stats, its a req to zero out the stats */
	if (!stats) {
		hw->DrvTotalFrmCaptured = 0;
		memset(&hw->stats, 0, sizeof(hw->stats));
		return;
	}

	hw->stats.freeq_count = crystalhd_hw_count_free_rx_pkts(hw);
	hw->stats.rdyq_count  = crystalhd_dioq_count(hw->rx_rdyq);
	memcpy(stats, &hw->stats, sizeof(*stats));
}
