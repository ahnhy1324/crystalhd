// SPDX-License-Identifier: LGPL-2.1-or-later

#include <assert.h>
#include <limits.h>

#include "crystalhd_ioctl_limits.h"

_Static_assert(UINT_MAX == 0xffffffffU,
	       "CrystalHD ioctl limits require 32-bit unsigned int");

int main(void)
{
	unsigned int bytes = 0;

	assert(!crystalhd_ioctl_transfer_size(0, &bytes));
	assert(crystalhd_ioctl_transfer_size(1, &bytes));
	assert(bytes == 4);
	assert(crystalhd_ioctl_transfer_size(
		CRYSTALHD_MAX_IOCTL_TRANSFER / 4, &bytes));
	assert(bytes == CRYSTALHD_MAX_IOCTL_TRANSFER);
	assert(!crystalhd_ioctl_transfer_size(
		CRYSTALHD_MAX_IOCTL_TRANSFER / 4 + 1, &bytes));
	assert(!crystalhd_ioctl_transfer_size(UINT_MAX, &bytes));

	assert(crystalhd_valid_pci_cfg(1, 0));
	assert(crystalhd_valid_pci_cfg(2, 2));
	assert(crystalhd_valid_pci_cfg(4, 252));
	assert(crystalhd_valid_pci_cfg(256, 0));
	assert(!crystalhd_valid_pci_cfg(0, 0));
	assert(!crystalhd_valid_pci_cfg(3, 0));
	assert(!crystalhd_valid_pci_cfg(8, 2));
	assert(!crystalhd_valid_pci_cfg(4, 253));
	assert(!crystalhd_valid_pci_cfg(UINT_MAX, UINT_MAX));

	assert(crystalhd_valid_dram_range(0, 0));
	assert(crystalhd_valid_dram_range(
		CRYSTALHD_DEVICE_DRAM_SIZE - 4, 1));
	assert(crystalhd_valid_dram_range(
		0, CRYSTALHD_DEVICE_DRAM_SIZE / 4));
	assert(!crystalhd_valid_dram_range(1, 1));
	assert(!crystalhd_valid_dram_range(
		CRYSTALHD_DEVICE_DRAM_SIZE, 1));
	assert(!crystalhd_valid_dram_range(0, UINT_MAX));

	return 0;
}
