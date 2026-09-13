// SPDX-License-Identifier: LGPL-2.1-or-later
#ifndef _CRYSTALHD_IOCTL_LIMITS_H_
#define _CRYSTALHD_IOCTL_LIMITS_H_

#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stdbool.h>
#endif

#define CRYSTALHD_MAX_IOCTL_TRANSFER (4U * 1024U * 1024U)
#define CRYSTALHD_PCI_CONFIG_SIZE 256U
#define CRYSTALHD_DEVICE_DRAM_SIZE (64U * 1024U * 1024U)
#define CRYSTALHD_FLEA_COLOR_REGISTER 0x00502100U

/* BCM70015 supports packed 422 here: bit 0 must remain clear; bit 1
 * selects YUY2 instead of UYVY. All other controls belong to the driver.
 */
static inline unsigned int crystalhd_flea_color_control(unsigned int previous,
						unsigned int requested)
{
	return (previous & ~3U) | (requested & 2U);
}

static inline bool crystalhd_ioctl_transfer_size(unsigned int dwords,
						 unsigned int *bytes)
{
	if (!dwords || dwords > CRYSTALHD_MAX_IOCTL_TRANSFER / 4U)
		return false;

	*bytes = dwords * 4U;
	return true;
}

static inline bool crystalhd_valid_pci_cfg(unsigned int size,
					   unsigned int offset)
{
	if (!size || size > CRYSTALHD_PCI_CONFIG_SIZE ||
	    offset > CRYSTALHD_PCI_CONFIG_SIZE - size)
		return false;

	if (size <= 4U)
		return (size == 1U || size == 2U || size == 4U) &&
		       !(offset & (size - 1U));

	return !(size & 3U) && !(offset & 3U);
}

static inline bool crystalhd_valid_dram_range(unsigned int start,
					      unsigned int dwords)
{
	return !(start & 3U) &&
	       dwords <= CRYSTALHD_DEVICE_DRAM_SIZE / 4U &&
	       start <= CRYSTALHD_DEVICE_DRAM_SIZE - dwords * 4U;
}

#endif /* _CRYSTALHD_IOCTL_LIMITS_H_ */
