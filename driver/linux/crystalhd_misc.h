/***************************************************************************
 * Copyright (c) 2005-2009, Broadcom Corporation.
 *
 *  Name: crystalhd_misc . h
 *
 *  Description:
 *		BCM70012 Linux driver general purpose routines.
 *		Includes reg/mem read and write routines.
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

#ifndef _CRYSTALHD_MISC_H_
#define _CRYSTALHD_MISC_H_

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/string.h>
#include <linux/ioctl.h>
#include <linux/dma-mapping.h>
#include <linux/sched.h>
#include <linux/refcount.h>
#include "bc_dts_glob_lnx.h"

/* forward declare */
struct crystalhd_adp;
struct crystalhd_hw;
struct crystalhd_rx_buffer;
struct crystalhd_tx_buffer;

/* Both operations run in process context. Each reference keeps the complete
 * buffer descriptor, ops, cookie, SG mapping and DMA backing alive.
 */
struct crystalhd_tx_buffer_ops {
	void (*get)(const struct crystalhd_tx_buffer *buffer);
	void (*put)(struct crystalhd_adp *adp,
		    const struct crystalhd_tx_buffer *buffer);
};

/* Frontend-neutral, already DMA-mapped input buffer. The frontend retains a
 * reference through submission; the synchronous core takes an extra backing
 * lease before posting. Completion notification does not release that lease.
 * The descriptor, ops, SG and backing survive until the last put and remain
 * immutable while the core holds its submission or retained lease.
 * The final 1..3 bytes live in one coherent, zero-padded word.
 */
struct crystalhd_tx_buffer {
	struct scatterlist	*sgl;
	uint32_t		dma_nents; /* mapped entries, not original entries */
	uint32_t		bytes; /* exact transfer length within the backing */
	dma_addr_t		tail_addr;
	uint32_t		tail_size;
	const struct crystalhd_tx_buffer_ops *ops;
	void			*cookie; /* stable, non-NULL ownership identity */
};

/* The buffer, ops and cookie stay immutable while submitted. Device sync can
 * run from hard IRQ context with a spinlock held and must not sleep. CPU sync,
 * read, write and release run in process context; release follows full detach.
 */
struct crystalhd_rx_buffer_ops {
	/* Native clients can return an explicit failed-frame completion. Legacy
	 * clients leave this clear and retain their historical drop behavior.
	 */
	bool report_decode_errors;
	void (*sync_for_cpu)(struct crystalhd_adp *adp,
			     struct crystalhd_rx_buffer *buffer);
	void (*sync_for_device)(struct crystalhd_adp *adp,
				struct crystalhd_rx_buffer *buffer);
	BC_STATUS (*read)(struct crystalhd_rx_buffer *buffer, uint32_t offset,
			  void *dst, size_t size);
	BC_STATUS (*write)(struct crystalhd_rx_buffer *buffer, uint32_t offset,
			   const void *src, size_t size);
	void (*release)(struct crystalhd_adp *adp,
			struct crystalhd_rx_buffer *buffer);
};

/* Frontend-neutral capture buffer presented to the hardware core. */
struct crystalhd_rx_buffer {
	struct scatterlist			*sgl;
	uint32_t				dma_nents;
	uint32_t				capacity;
	uint32_t				uv_offset;
	uint32_t				uv_sg_ix;
	uint32_t				uv_sg_off;
	BC_OUTPUT_FORMAT				output_format;
	const struct crystalhd_rx_buffer_ops	*ops;
	void					*cookie;
};

/* Global element pool for all Queue management.
 * TX: Active = BC_TX_LIST_CNT, Free = BC_TX_LIST_CNT.
 * RX: Free = BC_RX_LIST_CNT, Active = 2
 * FW-CMD: 4
 */
#define	BC_LINK_ELEM_POOL_SZ	((BC_TX_LIST_CNT * 2) + BC_RX_LIST_CNT + 2 + 4)

/* Driver's IODATA pool count */
#define	CHD_IODATA_POOL_SZ    (BC_IOCTL_DATA_POOL_SIZE * BC_LINK_MAX_OPENS)

/* Scatter Gather memory pool size for Tx and Rx */
#define BC_LINK_SG_POOL_SZ    (BC_TX_LIST_CNT + BC_RX_LIST_CNT)

enum _crystalhd_dio_sig {
	crystalhd_dio_inv = 0,
	crystalhd_dio_locked,
	crystalhd_dio_sg_mapped,
};

struct crystalhd_dio_user_info {
	void			*xfr_buff;
	uint32_t		xfr_len;
	uint32_t		uv_offset;
	bool			dir_tx;

	uint32_t		uv_sg_ix;
	uint32_t		uv_sg_off;
	BC_OUTPUT_FORMAT	b422mode;
};

struct crystalhd_dio_req {
	uint32_t						sig;
	uint32_t						max_pages;
	struct page						**pages;
	struct scatterlist				*sg;
	int								sg_cnt;
	int								sg_nents; /* original count passed to DMA mapping */
	int								page_cnt;
	int								direction;
	bool							cpu_owned;
	struct crystalhd_dio_user_info	uinfo;
	struct crystalhd_tx_buffer		tx_buffer;
	refcount_t				tx_refs;
	struct crystalhd_rx_buffer		rx_buffer;
	void							*fb_va;
	uint32_t						fb_size;
	dma_addr_t						fb_pa;
	struct crystalhd_dio_req		*next;
};

#define BC_LINK_DIOQ_SIG	(0x09223280)

struct crystalhd_elem {
	struct crystalhd_elem		*flink;
	struct crystalhd_elem		*blink;
	void				*data;
	uint32_t			tag;
};

typedef void (*crystalhd_data_free_cb)(void *context, void *data);

struct crystalhd_dioq {
	uint32_t		sig;
	struct crystalhd_adp	*adp;
	struct crystalhd_elem	*head;
	struct crystalhd_elem	*tail;
	uint32_t		count;
	spinlock_t		lock;
	wait_queue_head_t	event;
	crystalhd_data_free_cb	data_rel_cb;
	void			*cb_context;
};

/* Runs in IRQ or synchronous-cancel context after common TX ownership has
 * retired. A completion must not sleep or re-enter TX; defer such work.
 */
typedef void (*hw_comp_callback)(void *context, BC_STATUS sts);

/*========== PCIe Config access routines.================*/
BC_STATUS crystalhd_pci_cfg_rd(struct crystalhd_adp *, uint32_t, uint32_t, uint32_t *);
BC_STATUS crystalhd_pci_cfg_wr(struct crystalhd_adp *, uint32_t, uint32_t, uint32_t);

/*========= Linux Kernel Interface routines. ======================= */
void *bc_kern_dma_alloc(struct crystalhd_adp *, uint32_t, dma_addr_t *);
void bc_kern_dma_free(struct crystalhd_adp *, uint32_t,
		      void *, dma_addr_t);
#define crystalhd_create_event(_ev)	init_waitqueue_head(_ev)
#define crystalhd_set_event(_ev)		wake_up_interruptible(_ev)
#define crystalhd_wait_on_event(ev, condition, timeout, ret, nosig)	\
do {									\
	DECLARE_WAITQUEUE(entry, current);				\
	unsigned long end = jiffies + msecs_to_jiffies(timeout);		\
		ret = 0;						\
	add_wait_queue(ev, &entry);					\
	for (;;) {									\
		set_current_state(TASK_INTERRUPTIBLE);		\
		if (condition) {					\
			break;						\
		}							\
		if (time_after_eq(jiffies, end)) {			\
			ret = -EBUSY;					\
			break;						\
		}							\
		schedule_timeout((HZ / 100 > 1) ? HZ / 100 : 1);	\
		if (!nosig && signal_pending(current)) {		\
			ret = -EINTR;					\
			break;						\
		}							\
	}								\
	set_current_state(TASK_RUNNING);				\
	remove_wait_queue(ev, &entry);					\
} while (0)

#define crystalhd_wait_on_event_until(ev, condition, deadline, ret, nosig) \
do {									\
	DECLARE_WAITQUEUE(entry, current);				\
	unsigned long __chd_end = (deadline);				\
	unsigned long __chd_now;					\
	unsigned long __chd_remaining;				\
	unsigned long __chd_slice;					\
	ret = 0;							\
	add_wait_queue(ev, &entry);					\
	for (;;) {							\
		set_current_state(TASK_INTERRUPTIBLE);			\
		if (condition)						\
			break;						\
		__chd_now = jiffies;					\
		if (time_after_eq(__chd_now, __chd_end)) {		\
			ret = -EBUSY;					\
			break;						\
		}							\
		__chd_remaining = __chd_end - __chd_now;		\
		__chd_slice = (HZ / 100 > 1) ? HZ / 100 : 1;	\
		if (__chd_slice > __chd_remaining)			\
			__chd_slice = __chd_remaining;			\
		schedule_timeout(__chd_slice);				\
		if (!nosig && signal_pending(current)) {		\
			ret = -EINTR;					\
			break;						\
		}							\
	}								\
	set_current_state(TASK_RUNNING);				\
	remove_wait_queue(ev, &entry);					\
} while (0)

/*================ Direct IO mapping routines ==================*/
extern int crystalhd_create_dio_pool(struct crystalhd_adp *, uint32_t);
extern void crystalhd_destroy_dio_pool(struct crystalhd_adp *);
extern BC_STATUS crystalhd_map_dio(struct crystalhd_adp *, void *, uint32_t,
				   uint32_t, BC_OUTPUT_FORMAT, bool,
				   struct crystalhd_dio_req **);

extern BC_STATUS crystalhd_unmap_dio(struct crystalhd_adp *, struct crystalhd_dio_req*);
void crystalhd_tx_buffer_get(const struct crystalhd_tx_buffer *buffer);
void crystalhd_tx_buffer_put(struct crystalhd_adp *adp,
			     const struct crystalhd_tx_buffer *buffer);
void crystalhd_dio_to_cpu(struct crystalhd_adp *, struct crystalhd_dio_req *);
void crystalhd_dio_to_device(struct crystalhd_adp *, struct crystalhd_dio_req *);
void crystalhd_rx_buffer_sync_for_cpu(struct crystalhd_adp *,
				      struct crystalhd_rx_buffer *);
void crystalhd_rx_buffer_sync_for_device(struct crystalhd_adp *,
					 struct crystalhd_rx_buffer *);
BC_STATUS crystalhd_rx_buffer_read(struct crystalhd_rx_buffer *, uint32_t,
				   void *, size_t);
BC_STATUS crystalhd_rx_buffer_write(struct crystalhd_rx_buffer *, uint32_t,
				    const void *, size_t);
void crystalhd_rx_buffer_release(struct crystalhd_adp *,
				 struct crystalhd_rx_buffer *);
struct crystalhd_dio_req *
crystalhd_dio_from_rx_buffer(struct crystalhd_rx_buffer *);

/*================ General Purpose Queues ==================*/
extern BC_STATUS crystalhd_create_dioq(struct crystalhd_adp *, struct crystalhd_dioq **, crystalhd_data_free_cb , void *);
extern void crystalhd_delete_dioq(struct crystalhd_adp *, struct crystalhd_dioq *);
extern BC_STATUS crystalhd_dioq_add(struct crystalhd_dioq *ioq, void *data, bool wake, uint32_t tag);
extern void *crystalhd_dioq_fetch(struct crystalhd_dioq *ioq);
extern void *crystalhd_dioq_find_and_fetch(struct crystalhd_dioq *ioq, uint32_t tag);
extern void *crystalhd_dioq_fetch_wait(struct crystalhd_hw *hw, uint32_t to_secs, uint32_t *sig_pend);
/* Caller holds fetch_sem; bounded scan without waiting for picture arrival. */
void *crystalhd_dioq_try_fetch_locked(struct crystalhd_hw *hw);

#define crystalhd_dioq_count(_ioq)	((_ioq) ? _ioq->count : 0)

extern int crystalhd_create_elem_pool(struct crystalhd_adp *, uint32_t);
extern void crystalhd_delete_elem_pool(struct crystalhd_adp *);

/*================ Debug routines/macros .. ================================*/
extern void crystalhd_show_buffer(uint32_t off, uint8_t *buff, uint32_t dwcount);

#endif
