// SPDX-License-Identifier: LGPL-2.1-or-later

#include <stddef.h>
#include <stdint.h>

#include "bc_dts_defs.h"
#include "7411d.h"
typedef struct C011_PIB C011_PIB;
#include "bc_dts_glob_lnx.h"

/*
 * The ioctl number includes sizeof(BC_IOCTL_DATA).  Accidental layout drift
 * therefore changes the public command numbers and breaks old applications.
 */
#if UINTPTR_MAX == UINT64_MAX
#define EXPECT_IOCTL_DATA_SIZE 544U
#define EXPECT_IOCTL_UNION_OFFSET 16U
#define EXPECT_IOCTL_NEXT_OFFSET 536U
#define EXPECT_PROC_INPUT_SIZE 24U
#define EXPECT_YUV_BUFFS_SIZE 40U
#define EXPECT_DEC_OUT_SIZE 320U
#define EXPECT_DTS_STATS_SIZE 136U
#define EXPECT_PPB_SIZE 240U
#define EXPECT_PIB_SIZE 272U
#define EXPECT_IOCTL_BASE 0xc2206200U
#elif UINTPTR_MAX == UINT32_MAX
#define EXPECT_IOCTL_DATA_SIZE 536U
#define EXPECT_IOCTL_UNION_OFFSET 12U
#define EXPECT_IOCTL_NEXT_OFFSET 532U
#define EXPECT_PROC_INPUT_SIZE 16U
#define EXPECT_YUV_BUFFS_SIZE 28U
#define EXPECT_DEC_OUT_SIZE 300U
#define EXPECT_DTS_STATS_SIZE 132U
#define EXPECT_PPB_SIZE 232U
#define EXPECT_PIB_SIZE 264U
#define EXPECT_IOCTL_BASE 0xc2186200U
#else
#error Unsupported pointer size
#endif

_Static_assert(sizeof(BC_IOCTL_DATA) == EXPECT_IOCTL_DATA_SIZE,
	       "BC_IOCTL_DATA ABI changed");
_Static_assert(offsetof(BC_IOCTL_DATA, u) == EXPECT_IOCTL_UNION_OFFSET,
	       "BC_IOCTL_DATA union offset changed");
_Static_assert(offsetof(BC_IOCTL_DATA, next) == EXPECT_IOCTL_NEXT_OFFSET,
	       "BC_IOCTL_DATA next offset changed");
_Static_assert(sizeof(BC_PROC_INPUT) == EXPECT_PROC_INPUT_SIZE,
	       "BC_PROC_INPUT ABI changed");
_Static_assert(sizeof(BC_DEC_YUV_BUFFS) == EXPECT_YUV_BUFFS_SIZE,
	       "BC_DEC_YUV_BUFFS ABI changed");
_Static_assert(sizeof(BC_DEC_OUT_BUFF) == EXPECT_DEC_OUT_SIZE,
	       "BC_DEC_OUT_BUFF ABI changed");
_Static_assert(sizeof(BC_DTS_STATS) == EXPECT_DTS_STATS_SIZE,
	       "BC_DTS_STATS ABI changed");
_Static_assert(sizeof(PPB) == EXPECT_PPB_SIZE, "PPB ABI changed");
_Static_assert(sizeof(C011_PIB) == EXPECT_PIB_SIZE,
	       "C011_PIB ABI changed");
_Static_assert(offsetof(PPB, other) == 112U, "PPB common prefix changed");
_Static_assert(offsetof(PPB_H264, user_data) == 112U,
	       "PPB_H264 pointer offset changed");
_Static_assert(offsetof(PPB_MPEG, userData) == 48U,
	       "PPB_MPEG pointer offset changed");
_Static_assert(offsetof(PPB_VC1, userData) == 88U,
	       "PPB_VC1 pointer offset changed");
_Static_assert(offsetof(BC_DTS_STATS, DrvNextMDataPLD) == 80U,
	       "BC_DTS_STATS 64-bit field offset changed");
_Static_assert(sizeof(BC_FW_CMD) == 520U, "BC_FW_CMD ABI changed");
_Static_assert(sizeof(BC_PCI_CFG) == 264U, "BC_PCI_CFG ABI changed");
_Static_assert(_IOC_TYPE(BCM_IOC_GET_VERSION) == BC_IOC_BASE,
	       "CrystalHD ioctl magic changed");
_Static_assert(_IOC_NR(BCM_IOC_GET_VERSION) == DRV_CMD_VERSION,
	       "CrystalHD ioctl command changed");
_Static_assert(_IOC_SIZE(BCM_IOC_GET_VERSION) == sizeof(BC_IOCTL_DATA),
	       "CrystalHD ioctl size encoding changed");
_Static_assert(BCM_IOC_GET_VERSION == EXPECT_IOCTL_BASE + DRV_CMD_VERSION,
	       "CrystalHD GET_VERSION ioctl number changed");
_Static_assert(BCM_IOC_FW_DOWNLOAD == EXPECT_IOCTL_BASE + DRV_CMD_FW_DOWNLOAD,
	       "CrystalHD FW_DOWNLOAD ioctl number changed");
_Static_assert(BCM_IOC_RELEASE == EXPECT_IOCTL_BASE + DRV_CMD_RELEASE,
	       "CrystalHD RELEASE ioctl number changed");

int main(void)
{
	return 0;
}
