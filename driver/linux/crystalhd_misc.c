/***************************************************************************
 *	   Copyright (c) 2005-2009, Broadcom Corporation.
 *
 *  Name: crystalhd_misc . c
 *
 *  Description:
 *		BCM70012 Linux driver misc routines.
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

#include <linux/device.h>
#include <linux/sched/signal.h>

#include "crystalhd_lnx.h"
#include "crystalhd_misc.h"

/* Some HW specific code defines */
extern uint32_t link_GetRptDropParam(struct crystalhd_hw *hw, uint32_t picHeight, uint32_t picWidth, void *);
extern uint32_t flea_GetRptDropParam(struct crystalhd_hw *hw, void *);

static struct crystalhd_dio_req *crystalhd_alloc_dio(struct crystalhd_adp *adp)
{
	unsigned long flags = 0;
	struct crystalhd_dio_req *temp = NULL;

	if (!adp) {
		printk(KERN_ERR "%s: Invalid arg\n", __func__);
		return temp;
	}

	spin_lock_irqsave(&adp->lock, flags);
	temp = adp->ua_map_free_head;
	if (temp)
		adp->ua_map_free_head = adp->ua_map_free_head->next;
	spin_unlock_irqrestore(&adp->lock, flags);

	return temp;
}

static void crystalhd_free_dio(struct crystalhd_adp *adp, struct crystalhd_dio_req *dio)
{
	unsigned long flags = 0;

	if (!adp || !dio)
		return;
	spin_lock_irqsave(&adp->lock, flags);
	dio->sig = crystalhd_dio_inv;
	dio->page_cnt = 0;
	dio->sg_cnt = 0;
	dio->sg_nents = 0;
	dio->cpu_owned = false;
	dio->fb_size = 0;
	memset(&dio->uinfo, 0, sizeof(dio->uinfo));
	dio->next = adp->ua_map_free_head;
	adp->ua_map_free_head = dio;
	spin_unlock_irqrestore(&adp->lock, flags);
}

static struct crystalhd_elem *crystalhd_alloc_elem(struct crystalhd_adp *adp)
{
	unsigned long flags = 0;
	struct crystalhd_elem *temp = NULL;

	if (!adp)
	{
		printk(KERN_ERR "%s: Invalid args\n", __func__);
		return temp;
	}
	spin_lock_irqsave(&adp->lock, flags);
	temp = adp->elem_pool_head;
	if (temp) {
		adp->elem_pool_head = adp->elem_pool_head->flink;
		memset(temp, 0, sizeof(*temp));
	}

	spin_unlock_irqrestore(&adp->lock, flags);

	return temp;
}
static void crystalhd_free_elem(struct crystalhd_adp *adp, struct crystalhd_elem *elem)
{
	unsigned long flags = 0;

	if (!adp || !elem)
		return;
	spin_lock_irqsave(&adp->lock, flags);
	elem->flink = adp->elem_pool_head;
	adp->elem_pool_head = elem;
	spin_unlock_irqrestore(&adp->lock, flags);
}

static inline void crystalhd_set_sg(struct scatterlist *sg, struct page *page,
				  unsigned int len, unsigned int offset)
{
	sg_set_page(sg, page, len, offset);
#ifdef CONFIG_X86_64
	sg->dma_length = len;
#endif
}

static inline void crystalhd_init_sg(struct scatterlist *sg, unsigned int entries)
{
	sg_init_table(sg, entries);
}

/*========================== Extern ========================================*/
/**
 * crystalhd_pci_cfg_rd - PCIe config read
 * @adp: Adapter instance
 * @off: PCI config space offset.
 * @len: Size -- Byte, Word & dword.
 * @val: Value read
 *
 * Return:
 *	Status.
 *
 * Get value from PCIe config space.
 */
BC_STATUS crystalhd_pci_cfg_rd(struct crystalhd_adp *adp, uint32_t off,
			     uint32_t len, uint32_t *val)
{
	BC_STATUS sts = BC_STS_SUCCESS;
	int rc = 0;

	if (!adp || !val) {
		printk(KERN_ERR "%s: Invalid arg\n", __func__);
		return BC_STS_INV_ARG;
	}
	if (off >= PCI_CFG_SPACE_SIZE || len > PCI_CFG_SPACE_SIZE - off)
		return BC_STS_INV_ARG;

	switch (len) {
	case 1:
		rc = pci_read_config_byte(adp->pdev, off, (u8 *)val);
		break;
	case 2:
		rc = pci_read_config_word(adp->pdev, off, (u16 *)val);
		break;
	case 4:
		rc = pci_read_config_dword(adp->pdev, off, (u32 *)val);
		break;
	default:
		rc = -EINVAL;
		sts = BC_STS_INV_ARG;
		dev_err(&adp->pdev->dev, "Invalid len:%d\n", len);
	};

	if (rc && (sts == BC_STS_SUCCESS))
		sts = BC_STS_ERROR;

	return sts;
}

/**
 * crystalhd_pci_cfg_wr - PCIe config write
 * @adp: Adapter instance
 * @off: PCI config space offset.
 * @len: Size -- Byte, Word & dword.
 * @val: Value to be written
 *
 * Return:
 *	Status.
 *
 * Set value to Link's PCIe config space.
 */
BC_STATUS crystalhd_pci_cfg_wr(struct crystalhd_adp *adp, uint32_t off,
			     uint32_t len, uint32_t val)
{
	BC_STATUS sts = BC_STS_SUCCESS;
	int rc = 0;

	if (!adp) {
		printk(KERN_ERR "%s: Invalid arg\n", __func__);
		return BC_STS_INV_ARG;
	}
	if (off >= PCI_CFG_SPACE_SIZE || len > PCI_CFG_SPACE_SIZE - off)
		return BC_STS_INV_ARG;

	switch (len) {
	case 1:
		rc = pci_write_config_byte(adp->pdev, off, (u8)val);
		break;
	case 2:
		rc = pci_write_config_word(adp->pdev, off, (u16)val);
		break;
	case 4:
		rc = pci_write_config_dword(adp->pdev, off, val);
		break;
	default:
		rc = -EINVAL;
		sts = BC_STS_INV_ARG;
		dev_err(&adp->pdev->dev, "Invalid len:%d\n", len);
	};

	if (rc && (sts == BC_STS_SUCCESS))
		sts = BC_STS_ERROR;

	return sts;
}

/**
 * bc_kern_dma_alloc - Allocate memory for Dma rings
 * @adp: Adapter instance
 * @sz: Size of the memory to allocate.
 * @phy_addr: Physical address of the memory allocated.
 *	   Typedef to system's dma_addr_t (u64)
 *
 * Return:
 *  Pointer to allocated memory..
 *
 * Wrapper to Linux kernel interface.
 *
 */
void *bc_kern_dma_alloc(struct crystalhd_adp *adp, uint32_t sz,
			dma_addr_t *phy_addr)
{
	void *temp = NULL;

	if (!adp || !sz || !phy_addr) {
		printk(KERN_ERR "%s: Invalid arg\n", __func__);
		return temp;
	}

	temp = dma_alloc_coherent(&adp->pdev->dev, sz, phy_addr,GFP_KERNEL);
	if (temp)
		memset(temp, 0, sz);

	return temp;
}

/**
 * bc_kern_dma_free - Release Dma ring memory.
 * @adp: Adapter instance
 * @sz: Size of the memory to allocate.
 * @ka: Kernel virtual address returned during _dio_alloc()
 * @phy_addr: Physical address of the memory allocated.
 *	   Typedef to system's dma_addr_t (u64)
 *
 * Return:
 *     none.
 */
void bc_kern_dma_free(struct crystalhd_adp *adp, uint32_t sz, void *ka,
		      dma_addr_t phy_addr)
{
	if (!adp || !ka || !sz || !phy_addr) {
		printk(KERN_ERR "%s: Invalid arg\n", __func__);
		return;
	}

	dma_free_coherent(&adp->pdev->dev, sz, ka, phy_addr);
}

/**
 * crystalhd_create_dioq - Create Generic DIO queue
 * @adp: Adapter instance
 * @dioq_hnd: Handle to the dio queue created
 * @cb	: Optional - Call back To free the element.
 * @cbctx: Context to pass to callback.
 *
 * Return:
 *  status
 *
 * Initialize Generic DIO queue to hold any data. Callback
 * will be used to free elements while deleting the queue.
 */
BC_STATUS crystalhd_create_dioq(struct crystalhd_adp *adp,
			      struct crystalhd_dioq **dioq_hnd,
			      crystalhd_data_free_cb cb, void *cbctx)
{
	struct crystalhd_dioq *dioq = NULL;

	if (!adp || !dioq_hnd) {
		printk(KERN_ERR "%s: Invalid arg\n", __func__);
		return BC_STS_INV_ARG;
	}

	dioq = kzalloc(sizeof(*dioq), GFP_KERNEL);
	if (!dioq)
		return BC_STS_INSUFF_RES;

	spin_lock_init(&dioq->lock);
	dioq->sig = BC_LINK_DIOQ_SIG;
	dioq->head = (struct crystalhd_elem *)&dioq->head;
	dioq->tail = (struct crystalhd_elem *)&dioq->head;
	crystalhd_create_event(&dioq->event);
	dioq->adp = adp;
	dioq->data_rel_cb = cb;
	dioq->cb_context = cbctx;
	*dioq_hnd = dioq;

	return BC_STS_SUCCESS;
}

/**
 * crystalhd_delete_dioq - Delete Generic DIO queue
 * @adp: Adapter instance
 * @dioq: DIOQ instance..
 *
 * Return:
 *  None.
 *
 * Release Generic DIO queue. This function will remove
 * all the entries from the Queue and will release data
 * by calling the call back provided during creation.
 *
 */
void crystalhd_delete_dioq(struct crystalhd_adp *adp, struct crystalhd_dioq *dioq)
{
	void *temp;

	if (!dioq || (dioq->sig != BC_LINK_DIOQ_SIG))
		return;

	do {
		temp = crystalhd_dioq_fetch(dioq);
		if (temp && dioq->data_rel_cb)
			dioq->data_rel_cb(dioq->cb_context, temp);
	} while (temp);
	dioq->sig = 0;
	kfree(dioq);
}

/**
 * crystalhd_dioq_add - Add new DIO request element.
 * @ioq: DIO queue instance
 * @data: DIO request to be added.
 * @wake: True - Wake up suspended process.
 * @tag: Special tag to assign - For search and get.
 *
 * Return:
 *  Status.
 *
 * Insert new element to Q tail.
 */
BC_STATUS crystalhd_dioq_add(struct crystalhd_dioq *ioq, void *data,
			   bool wake, uint32_t tag)
{
	unsigned long flags = 0;
	struct crystalhd_elem *tmp;

	if (!ioq || (ioq->sig != BC_LINK_DIOQ_SIG) || !data) {
		dev_err(chddev(), "%s: Invalid arg\n", __func__);
		return BC_STS_INV_ARG;
	}

	tmp = crystalhd_alloc_elem(ioq->adp);
	if (!tmp) {
		dev_err(chddev(), "%s: No free elements.\n", __func__);
		return BC_STS_INSUFF_RES;
	}

	tmp->data = data;
	tmp->tag = tag;
	spin_lock_irqsave(&ioq->lock, flags);
	tmp->flink = (struct crystalhd_elem *)&ioq->head;
	tmp->blink = ioq->tail;
	tmp->flink->blink = tmp;
	tmp->blink->flink = tmp;
	ioq->count++;
	spin_unlock_irqrestore(&ioq->lock, flags);

	if (wake)
		crystalhd_set_event(&ioq->event);

	return BC_STS_SUCCESS;
}

/**
 * crystalhd_dioq_fetch - Fetch element from head.
 * @ioq: DIO queue instance
 *
 * Return:
 *	data element from the head..
 *
 * Remove an element from Queue.
 */
void *crystalhd_dioq_fetch(struct crystalhd_dioq *ioq)
{
	unsigned long flags = 0;
	struct crystalhd_elem *tmp;
	struct crystalhd_elem *ret = NULL;
	void *data = NULL;

	if (!ioq || (ioq->sig != BC_LINK_DIOQ_SIG)) {
		dev_err(chddev(), "%s: Invalid arg\n", __func__);
		if(!ioq)
			dev_err(chddev(), "ioq not initialized\n");
		else
			dev_err(chddev(), "ioq invalid signature\n");
		return data;
	}

	spin_lock_irqsave(&ioq->lock, flags);
	tmp = ioq->head;
	if (tmp != (struct crystalhd_elem *)&ioq->head) {
		ret = tmp;
		tmp->flink->blink = tmp->blink;
		tmp->blink->flink = tmp->flink;
		ioq->count--;
	}
	spin_unlock_irqrestore(&ioq->lock, flags);
	if (ret) {
		data = ret->data;
		crystalhd_free_elem(ioq->adp, ret);
	}

	return data;
}
/**
 * crystalhd_dioq_find_and_fetch - Search the tag and Fetch element
 * @ioq: DIO queue instance
 * @tag: Tag to search for.
 *
 * Return:
 *	element from the head..
 *
 * Search TAG and remove the element.
 */
void *crystalhd_dioq_find_and_fetch(struct crystalhd_dioq *ioq, uint32_t tag)
{
	unsigned long flags = 0;
	struct crystalhd_elem *tmp;
	struct crystalhd_elem *ret = NULL;
	void *data = NULL;

	if (!ioq || (ioq->sig != BC_LINK_DIOQ_SIG)) {
		dev_err(chddev(), "%s: Invalid arg\n", __func__);
		return data;
	}

	spin_lock_irqsave(&ioq->lock, flags);
	tmp = ioq->head;
	while (tmp != (struct crystalhd_elem *)&ioq->head) {
		if (tmp->tag == tag) {
			ret = tmp;
			tmp->flink->blink = tmp->blink;
			tmp->blink->flink = tmp->flink;
			ioq->count--;
			break;
		}
		tmp = tmp->flink;
	}
	spin_unlock_irqrestore(&ioq->lock, flags);

	if (ret) {
		data = ret->data;
		crystalhd_free_elem(ioq->adp, ret);
	}

	return data;
}

/**
 * crystalhd_dioq_fetch_wait - Fetch element from Head.
 * @hw: Hardware context containing the ready queue.
 * @to_secs: Wait timeout in seconds..
 * @sig_pend: Set when a signal interrupts the wait.
 *
 * Return:
 *	element from the head..
 *
 * Return element from head if Q is not empty. Wait for new element
 * if Q is empty for Timeout seconds.
 */
void *crystalhd_dioq_fetch_wait(struct crystalhd_hw *hw, uint32_t to_secs, uint32_t *sig_pend)
{
	struct device *dev = chddev();
	unsigned long flags = 0;
	int rc = 0;

	struct crystalhd_rx_dma_pkt *r_pkt = NULL;
	struct crystalhd_dioq *ioq = hw->rx_rdyq;
	uint32_t picYcomp = 0;

	unsigned long fetchTimeout = jiffies + msecs_to_jiffies(to_secs * 1000);

	if (!ioq || (ioq->sig != BC_LINK_DIOQ_SIG) || !to_secs || !sig_pend) {
		dev_err(dev, "%s: Invalid arg\n", __func__);
		return r_pkt;
	}

	spin_lock_irqsave(&ioq->lock, flags);
	while (!time_after_eq(jiffies, fetchTimeout)) {
		if(ioq->count == 0) {
			spin_unlock_irqrestore(&ioq->lock, flags);
			crystalhd_wait_on_event(&ioq->event, (ioq->count > 0),
					250, rc, false);
		}
		else
			spin_unlock_irqrestore(&ioq->lock, flags);
		if (rc == 0) {
			/* Found a packet. Check if it is a repeated picture or not */
			/* Drop the picture if it is a repeated picture */
			/* Lock against checks from get status calls */
			if(down_interruptible(&hw->fetch_sem))
				goto sem_error;
			r_pkt = crystalhd_dioq_fetch(ioq);
			/* Another fetcher or flush may have consumed the packet
			 * after the wakeup and before we acquired fetch_sem.
			 */
			if (!r_pkt) {
				up(&hw->fetch_sem);
				spin_lock_irqsave(&ioq->lock, flags);
				continue;
			}
			/* If format change packet, then return with out checking anything */
			if (r_pkt->flags & (COMP_FLAG_PIB_VALID | COMP_FLAG_FMT_CHANGE))
				goto sem_rel_return;
			if (hw->adp->pdev->device == BC_PCI_DEVID_LINK) {
				picYcomp = link_GetRptDropParam(hw, hw->PICHeight, hw->PICWidth, (void *)r_pkt);
			}
			else {
				/* For Flea, we don't have the width and height handy since they */
				/* come in the PIB in the picture, so this function will also */
				/* populate the width and height */
				picYcomp = flea_GetRptDropParam(hw, (void *)r_pkt);
				/* For flea it is the above function that indicated format change */
				if(r_pkt->flags & (COMP_FLAG_PIB_VALID | COMP_FLAG_FMT_CHANGE))
					goto sem_rel_return;
			}
			if(!picYcomp || (picYcomp == hw->LastPicNo) ||
				(picYcomp == hw->LastTwoPicNo)) {
				/*Discard picture */
				if(picYcomp != 0) {
					hw->LastTwoPicNo = hw->LastPicNo;
					hw->LastPicNo = picYcomp;
				}
				crystalhd_dioq_add(hw->rx_freeq, r_pkt, false, r_pkt->pkt_tag);
				r_pkt = NULL;
				up(&hw->fetch_sem);
			} else {
				if(hw->adp->pdev->device == BC_PCI_DEVID_LINK) {
					if((picYcomp - hw->LastPicNo) > 1) {
						dev_info(dev, "MISSING %u PICTURES\n", (picYcomp - hw->LastPicNo));
					}
				}
				hw->LastTwoPicNo = hw->LastPicNo;
				hw->LastPicNo = picYcomp;
				goto sem_rel_return;
			}
		} else if (rc == -EINTR) {
			*sig_pend = 1;
			return r_pkt;
		}
		spin_lock_irqsave(&ioq->lock, flags);
	}
	dev_info(dev, "FETCH TIMEOUT\n");
	spin_unlock_irqrestore(&ioq->lock, flags);
	return r_pkt;
sem_error:
	return NULL;
sem_rel_return:
	up(&hw->fetch_sem);
	return r_pkt;
}

/**
 * crystalhd_map_dio - Map user address for DMA
 * @adp:	Adapter instance
 * @ubuff:	User buffer to map.
 * @ubuff_sz:	User buffer size.
 * @uv_offset:	UV buffer offset.
 * @en_422mode: TRUE:422 FALSE:420 Capture mode.
 * @dir_tx:	TRUE for Tx (To device from host)
 * @dio_hnd:	Handle to mapped DIO request.
 *
 * Return:
 *	Status.
 *
 * This routine maps user address and lock pages for DMA.
 *
 */
BC_STATUS crystalhd_map_dio(struct crystalhd_adp *adp, void *ubuff,
	uint32_t ubuff_sz, uint32_t uv_offset,
	bool en_422mode, bool dir_tx,
	struct crystalhd_dio_req **dio_hnd)
{
	struct device *dev;
	struct crystalhd_dio_req *dio;
	unsigned long uaddr = (unsigned long)ubuff;
	unsigned long nr_pages;
	unsigned int gup_flags = FOLL_LONGTERM;
	uint32_t count, offset, len;
	long pinned;
	int i;

	if (dio_hnd)
		*dio_hnd = NULL;
	if (!adp || !ubuff || !ubuff_sz || !dio_hnd ||
	    ubuff_sz > ULONG_MAX - uaddr || (uaddr & 3) ||
	    (!dir_tx && ((ubuff_sz | uv_offset) & 3)) ||
	    uv_offset >= ubuff_sz)
		return BC_STS_INV_ARG;

	dev = &adp->pdev->dev;
	/* Subtract before rounding to avoid wrapping near ULONG_MAX. */
	nr_pages = ((uaddr + ubuff_sz - 1) >> PAGE_SHIFT) -
		   (uaddr >> PAGE_SHIFT) + 1;
	dio = crystalhd_alloc_dio(adp);
	if (!dio)
		return BC_STS_INSUFF_RES;
	if (nr_pages > dio->max_pages) {
		crystalhd_free_dio(adp, dio);
		return BC_STS_INSUFF_RES;
	}

	/* The capture metadata parser also updates words in the Y buffer. */
	dio->direction = dir_tx ? DMA_TO_DEVICE : DMA_BIDIRECTIONAL;
	/* Input is owned until its synchronous ioctl completes, including an
	 * unbounded wait for firmware FIFO space before the timed DMA transfer.
	 * Capture registration survives ioctls, discard flush and suspend until
	 * fetch/full flush/close/remove. Neither path has MMU notifiers, so both
	 * require long-term pins. The MM rejects unsupported mappings (e.g. DAX).
	 */
	if (!dir_tx)
		gup_flags |= FOLL_WRITE;
	pinned = pin_user_pages_fast(uaddr, nr_pages, gup_flags, dio->pages);
	dio->page_cnt = pinned > 0 ? pinned : 0;
	dio->sig = crystalhd_dio_locked;
	if (pinned != nr_pages) {
		dev_dbg(dev, "pin pages failed: %lu-%ld\n", nr_pages, pinned);
		goto fail;
	}

	/* The device reads the final 1..3 input bytes from a coherent word.
	 * Exclude those bytes from the streaming SG list before mapping it;
	 * removing a DMA segment afterwards is wrong when the mapper merges it.
	 */
	dio->fb_size = dir_tx ? ubuff_sz & 3 : 0;
	count = ubuff_sz - dio->fb_size;
	if (dio->fb_size) {
		memset(dio->fb_va, 0, 4);
		if (copy_from_user(dio->fb_va, (void __user *)(uaddr + count),
				   dio->fb_size))
			goto fail;
	}
	if (count) {
		offset = offset_in_page(uaddr);
		dio->sg_nents = DIV_ROUND_UP(offset + count, PAGE_SIZE);
		crystalhd_init_sg(dio->sg, dio->sg_nents);
		for (i = 0; i < dio->sg_nents; i++) {
			len = min_t(uint32_t, count, PAGE_SIZE - offset);
			crystalhd_set_sg(&dio->sg[i], dio->pages[i], len, offset);
			count -= len;
			offset = 0;
		}
		dio->sg_cnt = dma_map_sg(dev, dio->sg, dio->sg_nents,
					 dio->direction);
		if (!dio->sg_cnt)
			goto fail;
		dio->sig = crystalhd_dio_sg_mapped;
	}

	/* Plane offsets refer to mapped DMA segments, which may combine pages. */
	if (uv_offset) {
		offset = uv_offset;
		for (i = 0; i < dio->sg_cnt; i++) {
			if (offset < sg_dma_len(&dio->sg[i]))
				break;
			offset -= sg_dma_len(&dio->sg[i]);
		}
		if (i == dio->sg_cnt)
			goto fail;
		dio->uinfo.uv_sg_ix = i;
		dio->uinfo.uv_sg_off = offset;
	}
	dio->uinfo.xfr_len = ubuff_sz;
	dio->uinfo.xfr_buff = ubuff;
	dio->uinfo.uv_offset = uv_offset;
	dio->uinfo.b422mode = en_422mode;
	dio->uinfo.dir_tx = dir_tx;
	*dio_hnd = dio;
	return BC_STS_SUCCESS;

fail:
	crystalhd_unmap_dio(adp, dio);
	return BC_STS_ERROR;
}

void crystalhd_dio_to_cpu(struct crystalhd_adp *adp,
			  struct crystalhd_dio_req *dio)
{
	if (dio->sig == crystalhd_dio_sg_mapped && !dio->cpu_owned) {
		dma_sync_sg_for_cpu(&adp->pdev->dev, dio->sg, dio->sg_nents,
				    dio->direction);
		dio->cpu_owned = true;
	}
}

void crystalhd_dio_to_device(struct crystalhd_adp *adp,
			     struct crystalhd_dio_req *dio)
{
	if (dio->sig == crystalhd_dio_sg_mapped && dio->cpu_owned) {
		dma_sync_sg_for_device(&adp->pdev->dev, dio->sg, dio->sg_nents,
				       dio->direction);
		dio->cpu_owned = false;
	}
}
/**
 * crystalhd_unmap_dio - Release mapped resources
 * @adp: Adapter instance
 * @dio: DIO request instance
 *
 * Return:
 *	Status.
 *
 * This routine is to unmap the user buffer pages.
 */
BC_STATUS crystalhd_unmap_dio(struct crystalhd_adp *adp, struct crystalhd_dio_req *dio)
{
	bool mapped;

	if (!adp || !dio) {
		printk(KERN_ERR "%s: Invalid arg\n", __func__);
		return BC_STS_INV_ARG;
	}

	/* DMA must be stopped before entry. Unmap while the pages are pinned:
	 * SWIOTLB may copy capture data back during dma_unmap_sg().
	 */
	mapped = dio->sig == crystalhd_dio_sg_mapped;
	if (mapped) {
		/* Preserve CPU metadata edits across SWIOTLB's final copy-back. */
		crystalhd_dio_to_device(adp, dio);
		dma_unmap_sg(&adp->pdev->dev, dio->sg, dio->sg_nents,
			     dio->direction);
	}
	if (dio->page_cnt > 0)
		unpin_user_pages_dirty_lock(dio->pages, dio->page_cnt,
			mapped && dio->direction != DMA_TO_DEVICE);

	crystalhd_free_dio(adp, dio);

	return BC_STS_SUCCESS;
}

/**
 * crystalhd_create_dio_pool - Allocate mem pool for DIO management.
 * @adp: Adapter instance
 * @max_pages: Max pages for size calculation.
 *
 * Return:
 *	system error.
 *
 * This routine creates a memory pool to hold dio context for
 * for HW Direct IO operation.
 */
int crystalhd_create_dio_pool(struct crystalhd_adp *adp, uint32_t max_pages)
{
	struct device *dev;
	uint32_t asz = 0, i = 0;
	uint8_t	*temp;
	struct crystalhd_dio_req *dio;

	if (!adp || !max_pages) {
		printk(KERN_ERR "%s: Invalid arg\n", __func__);
		return -EINVAL;
	}

	dev = &adp->pdev->dev;

	/* Get dma memory for fill byte handling..*/
	adp->fill_byte_pool = dma_pool_create("crystalhd_fbyte", &adp->pdev->dev, 8, 8, 0);
	if (!adp->fill_byte_pool) {
		dev_err(dev, "failed to create fill byte pool\n");
		return -ENOMEM;
	}

	/* Get the max size from user based on 420/422 modes */
	asz =  (sizeof(*dio->pages) * max_pages) +
	       (sizeof(*dio->sg) * max_pages) + sizeof(*dio);

	dev_dbg(dev, "Initializing Dio pool %d %d %x %p\n",
		BC_LINK_SG_POOL_SZ, max_pages, asz, adp->fill_byte_pool);

	for (i = 0; i < BC_LINK_SG_POOL_SZ; i++) {
		temp = kzalloc(asz, GFP_KERNEL);
		if ((temp) == NULL) {
			dev_err(dev, "Failed to alloc %d mem\n", asz);
			crystalhd_destroy_dio_pool(adp);
			return -ENOMEM;
		}

		dio = (struct crystalhd_dio_req *)temp;
		temp += sizeof(*dio);
		dio->pages = (struct page **)temp;
		temp += (sizeof(*dio->pages) * max_pages);
		dio->sg = (struct scatterlist *)temp;
		dio->max_pages = max_pages;
		dio->fb_va = dma_pool_alloc(adp->fill_byte_pool, GFP_KERNEL,
					    &dio->fb_pa);
		if (!dio->fb_va) {
			dev_err(dev, "fill byte alloc failed.\n");
			kfree(dio);
			crystalhd_destroy_dio_pool(adp);
			return -ENOMEM;
		}

		crystalhd_free_dio(adp, dio);
	}

	return 0;
}

/**
 * crystalhd_destroy_dio_pool - Release DIO mem pool.
 * @adp: Adapter instance
 *
 * Return:
 *	none.
 *
 * This routine releases dio memory pool during close.
 */
void crystalhd_destroy_dio_pool(struct crystalhd_adp *adp)
{
	struct crystalhd_dio_req *dio;
	int count = 0;

	if (!adp) {
		printk(KERN_ERR "%s: Invalid arg\n", __func__);
		return;
	}

	do {
		dio = crystalhd_alloc_dio(adp);
		if (dio) {
			if (dio->fb_va)
				dma_pool_free(adp->fill_byte_pool,
					      dio->fb_va, dio->fb_pa);
			count++;
			kfree(dio);
		}
	} while (dio);

	if (adp->fill_byte_pool) {
		dma_pool_destroy(adp->fill_byte_pool);
		adp->fill_byte_pool = NULL;
	}

	dev_dbg(&adp->pdev->dev, "Released dio pool %d\n", count);
}

/**
 * crystalhd_create_elem_pool - List element pool creation.
 * @adp: Adapter instance
 * @pool_size: Number of elements in the pool.
 *
 * Return:
 *	0 - success, <0 error
 *
 * Create general purpose list element pool to hold pending,
 * and active requests.
 */
int crystalhd_create_elem_pool(struct crystalhd_adp *adp,
		uint32_t pool_size)
{
	uint32_t i;
	struct crystalhd_elem *temp;

	if (!adp || !pool_size)
		return -EINVAL;

	for (i = 0; i < pool_size; i++) {
		temp = kzalloc(sizeof(*temp), GFP_KERNEL);
		if (!temp) {
			dev_err(&adp->pdev->dev, "kzalloc failed\n");
			return -ENOMEM;
		}
		crystalhd_free_elem(adp, temp);
	}
	dev_dbg(&adp->pdev->dev, "allocated %d elem\n", pool_size);
	return 0;
}

/**
 * crystalhd_delete_elem_pool - List element pool deletion.
 * @adp: Adapter instance
 *
 * Return:
 *	none
 *
 * Delete general purpose list element pool.
 */
void crystalhd_delete_elem_pool(struct crystalhd_adp *adp)
{
	struct crystalhd_elem *temp;
	int dbg_cnt = 0;

	if (!adp)
		return;

	do {
		temp = crystalhd_alloc_elem(adp);
		if (temp) {
			kfree(temp);
			dbg_cnt++;
		}
	} while (temp);

	dev_dbg(&adp->pdev->dev, "released %d elem\n", dbg_cnt);
}

/*================ Debug support routines.. ================================*/
void crystalhd_show_buffer(uint32_t off, uint8_t *buff, uint32_t dwcount)
{
	struct device *dev = chddev();
	uint32_t i, k = 1;

	for (i = 0; i < dwcount; i++) {
		if (k == 1)
			dev_dbg(dev, "0x%08X : ", off);

		dev_dbg(dev, " 0x%08X ", *((uint32_t *)buff));

		buff += sizeof(uint32_t);
		off  += sizeof(uint32_t);
		k++;
		if ((i == dwcount - 1) || (k > 4)) {
			dev_dbg(dev, "\n");
			k = 1;
		}
	}
}
