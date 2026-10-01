/***************************************************************************
 * Copyright (c) 2005-2009, Broadcom Corporation.
 *
 *  Name: crystalhd_lnx . c
 *
 *  Description:
 *		BCM70012 Linux driver
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

#ifndef _CRYSTALHD_LNX_H_
#define _CRYSTALHD_LNX_H_

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/string.h>
#include <linux/mm.h>
#include <linux/tty.h>
#include <linux/slab.h>
#include <linux/delay.h>
#include <linux/fb.h>
#include <linux/pci.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/rwsem.h>
#include <linux/mutex.h>
#include <linux/pagemap.h>
#include <linux/vmalloc.h>

#include <linux/io.h>
#include <asm/irq.h>
#include <asm/pgtable.h>
#include <linux/uaccess.h>

#include "crystalhd_compat.h"
#include "crystalhd_ioctl_limits.h"
#include "crystalhd_cmds.h"
#include "crystalhd_l0s.h"
#include "crystalhd_v4l2.h"

#define CRYSTAL_HD_NAME "Broadcom Crystal HD Decoder Driver"

/* OS specific PCI information structure and adapter information. */
struct crystalhd_adp {
	/* Hardware board/PCI specifics */
	char			name[32];
	struct pci_dev		*pdev;
	struct crystalhd_l0s_state l0s;

	unsigned long		pci_mem_start;
	uint32_t			pci_mem_len;
	void				*mem_addr;

	unsigned long		pci_i2o_start;
	uint32_t			pci_i2o_len;
	void				*i2o_addr;

	unsigned int		drv_data;
	unsigned int		dmabits;	/* 32 | 64 */
	unsigned int		registered;
	unsigned int		present;
	unsigned int		msi;
	bool			irq_registered;
	u64			generation;
	/* PM readiness under user_lock; probe initializes before publication. */
	bool			hw_accessible;
	struct crystalhd_v4l2	*v4l2;

	spinlock_t		lock;
	struct rw_semaphore	user_lock;
	struct mutex		tx_lock;

	/* API Related */
	int			chd_dec_major;
	unsigned int		cfg_users;

	crystalhd_ioctl_data	*idata_free_head;	/* ioctl data pool */
	struct crystalhd_elem	*elem_pool_head;	/* Queue element pool */

	struct crystalhd_cmd	cmds;

	struct crystalhd_dio_req	*ua_map_free_head;
	struct dma_pool		*fill_byte_pool;
};

struct crystalhd_device_access {
	struct crystalhd_adp *adp;
	bool exclusive;
};

/* Enter one sleepable, non-nested operation using a frontend's probe generation.
 * The caller supplies an inactive handle and holds no device/user/TX locks.
 * Success borrows adp under device -> user locks until same-task exit; do not
 * copy the handle, retain adp afterward or wait for workers while holding it.
 * This gates PM readiness, not firmware/DMA/session validity. Removal can
 * publish cancellation during the operation, so core cancellation checks apply.
 */
int crystalhd_device_enter(u64 generation, bool exclusive,
			   struct crystalhd_device_access *access);
void crystalhd_device_exit(struct crystalhd_device_access *access);

struct crystalhd_adp *chd_get_adp(void);
struct device *chddev(void);

#endif
