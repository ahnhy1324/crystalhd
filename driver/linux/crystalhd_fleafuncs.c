/***************************************************************************
 * Copyright (c) 2005-2009, Broadcom Corporation.
 *
 *  Name: crystalhd_fleafuncs.c
 *
 *  Description:
 *		BCM70015 Linux driver HW layer.
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
#include <linux/sched/signal.h>
#include <asm/tsc.h>
#include <asm/msr.h>
#include "70015/magnum/basemodules/chp/70015/rdb/a0/bchp_clk.h"
#include "70015/magnum/basemodules/chp/70015/rdb/a0/bchp_ddr23_ctl_regs_0.h"
#include "70015/magnum/basemodules/chp/70015/rdb/a0/bchp_ddr23_phy_control_regs.h"
#include "70015/magnum/basemodules/chp/70015/rdb/a0/bchp_ddr23_phy_byte_lane_0.h"
#include "70015/magnum/basemodules/chp/70015/rdb/a0/bchp_ddr23_phy_byte_lane_1.h"
#include "70015/magnum/basemodules/chp/70015/rdb/a0/bchp_pri_arb_control_regs.h"
#include "70015/magnum/basemodules/chp/70015/rdb/a0/bchp_misc3.h"
#include "crystalhd_hw.h"
#include "crystalhd_fleafuncs.h"
#include "crystalhd_lnx.h"
#include "FleaDefs.h"
#include "crystalhd_flea_ddr.h"
#include "crystalhd_fw_research.h"
#include "crystalhd_rx_receipt.h"
#include "../../include/crystalhd_fw_research.h"

#define OFFSETOF(_s_, _m_) ((size_t)(unsigned long)&(((_s_ *)0)->_m_))

bool crystalhd_flea_core_reset(struct crystalhd_hw *hw);
void crystalhd_flea_disable_interrupts(struct crystalhd_hw *hw);
void crystalhd_flea_enable_interrupts(struct crystalhd_hw *hw);
void crystalhd_flea_clear_interrupts(struct crystalhd_hw *hw);
bool crystalhd_flea_detect_ddr3(struct crystalhd_hw *hw);
bool crystalhd_flea_init_dram(struct crystalhd_hw *hw);
bool crystalhd_flea_detect_fw_alive(struct crystalhd_hw *hw);
void crystalhd_flea_handle_PicQSts_intr(struct crystalhd_hw *hw);
void crystalhd_flea_update_tx_buff_info(struct crystalhd_hw *hw);
void crystalhd_flea_init_temperature_measure (struct crystalhd_hw *hw, bool bTurnOn);
void crystalhd_flea_update_temperature(struct crystalhd_hw *hw);
bool crystalhd_flea_wake_up_hw(struct crystalhd_hw *hw);
BC_STATUS crystalhd_flea_fw_cmd_post_proc(struct crystalhd_hw *hw, BC_FW_CMD *fw_cmd);
void crystalhd_flea_clear_rx_errs_intrs(struct crystalhd_hw *hw);
uint32_t flea_GetRptDropParam(struct crystalhd_hw *hw, void* pRxDMAReq);
static bool flea_get_picture_info(struct crystalhd_hw *hw,
	struct crystalhd_rx_dma_pkt *rx_pkt, uint32_t *pic_number,
	uint64_t *metadata, bool prepare_output);
BC_STATUS crystalhd_flea_hw_fire_rxdma(struct crystalhd_hw *hw,
#ifdef CRYSTALHD_ENABLE_FW_RESEARCH
	struct crystalhd_rx_dma_pkt *rx_pkt,
	struct crystalhd_rx_receipt_key *receipt_key
#else
	struct crystalhd_rx_dma_pkt *rx_pkt
#endif
	);

bool crystalhd_flea_core_reset(struct crystalhd_hw *hw)
{
	unsigned int pollCnt=0,regVal=0;

	if (READ_ONCE(hw->dma_fault) ||
	    READ_ONCE(hw->FleaPowerState) == FLEA_PS_FAULT)
		return false;
	dev_dbg(&hw->adp->pdev->dev,"[crystalhd_flea_core_reset]: Starting core reset\n");

	hw->pfnWriteDevRegister(hw->adp, BCHP_MISC3_RESET_CTRL,
			      BCHP_MISC3_RESET_CTRL_CORE_RESET_MASK);

	pollCnt=0;
	while (1)
	{
		pollCnt++;
		regVal=0;

		msleep_interruptible(1);

		regVal = hw->pfnReadDevRegister(hw->adp, BCHP_MISC3_RESET_CTRL);

		if(!(regVal & BCHP_MISC3_RESET_CTRL_CORE_RESET_MASK))
		{
			/*
			-- Bit is 0, Reset is completed. Which means that
			-- wait for sometime and then allow other accesses.
			*/
			msleep_interruptible(1);
			break;
		}

		if(pollCnt > MAX_VALID_POLL_CNT)
		{
			printk("!!FATAL ERROR!! Core Reset Failure\n");
			return false;
		}
	}

	msleep_interruptible(5);

	return true;
}

void crystalhd_flea_disable_interrupts(struct crystalhd_hw *hw)
{
	union FLEA_INTR_BITS_COMMON IntrMaskReg;
	/*
	-- Mask everything except the reserved bits.
	*/
	IntrMaskReg.WholeReg =0xffffffff;
	IntrMaskReg.Reserved1=0;
	IntrMaskReg.Reserved2=0;
	IntrMaskReg.Reserved3=0;
	IntrMaskReg.Reserved4=0;

	hw->pfnWriteDevRegister(hw->adp, BCHP_INTR_INTR_MSK_SET_REG, IntrMaskReg.WholeReg);

	return;
}

void crystalhd_flea_enable_interrupts(struct crystalhd_hw *hw)
{
	union FLEA_INTR_BITS_COMMON IntrMaskReg;
	/*
	-- Clear The Mask for everything except the reserved bits.
	*/
	IntrMaskReg.WholeReg =0xffffffff;
	IntrMaskReg.Reserved1=0;
	IntrMaskReg.Reserved2=0;
	IntrMaskReg.Reserved3=0;
	IntrMaskReg.Reserved4=0;

	hw->pfnWriteDevRegister(hw->adp, BCHP_INTR_INTR_MSK_CLR_REG, IntrMaskReg.WholeReg);

	return;
}

void crystalhd_flea_clear_interrupts(struct crystalhd_hw *hw)
{
	union FLEA_INTR_BITS_COMMON	IntrStsValue;

	IntrStsValue.WholeReg = hw->pfnReadDevRegister(hw->adp, BCHP_INTR_INTR_STATUS);

	if(IntrStsValue.WholeReg)
	{
		hw->pfnWriteDevRegister(hw->adp, BCHP_INTR_INTR_CLR_REG, IntrStsValue.WholeReg);
		hw->pfnWriteDevRegister(hw->adp, BCHP_INTR_EOI_CTRL, 1);
	}

	return;
}

bool crystalhd_flea_detect_ddr3(struct crystalhd_hw *hw)
{
	uint32_t regVal = 0;

	/*Set the Multiplexer to select the GPIO-6*/
	regVal = hw->pfnReadDevRegister(hw->adp, BCHP_SUN_TOP_CTRL_PIN_MUX_CTRL_0);

	/*Make sure that the bits-24:27 are reset*/
	if(regVal & 0x0f000000)
	{
		regVal = regVal & 0xf0ffffff; /*Clear bit 24-27 for selecting GPIO_06*/
		hw->pfnWriteDevRegister(hw->adp, BCHP_SUN_TOP_CTRL_PIN_MUX_CTRL_0, regVal);
	}

	regVal=0;
	/*Set the Direction of GPIO-6*/
	regVal = hw->pfnReadDevRegister(hw->adp, BCHP_GIO_IODIR_LO);

	if(!(regVal & BC_BIT(6)))
	{
		/*Set the Bit number 6 to make the GPIO6 as input*/
		regVal |= BC_BIT(6);
		hw->pfnWriteDevRegister(hw->adp, BCHP_GIO_IODIR_LO, regVal);
	}

	regVal=0;
	regVal = hw->pfnReadDevRegister(hw->adp,	BCHP_GIO_DATA_LO);

	/*If this bit is clear then have DDR-3 else we have DDR-2*/
	if(!(regVal & BC_BIT(6)))
	{
		dev_dbg(&hw->adp->pdev->dev,"DDR-3 Detected\n");
		return true;
	}
	dev_dbg(&hw->adp->pdev->dev,"DDR-2 Detected\n");
	return false;
}

bool crystalhd_flea_init_dram(struct crystalhd_hw *hw)
{
	int32_t ddr2_speed_grade[2];
	uint32_t sd_0_col_size, sd_0_bank_size, sd_0_row_size;
	uint32_t ddr3_mode[2];
	uint32_t regVal;
	bool bDDR3Detected=false; /*Should be filled in using the detection logic. Default to DDR2 */

	/* On all designs we are using DDR2 or DDR3 x16 and running at a max of 400Mhz */
	/* Only one bank of DDR supported. The other is a dummy */

	ddr2_speed_grade[0] = DDR2_400MHZ;
	ddr2_speed_grade[1] = DDR2_400MHZ;
	sd_0_col_size = COL_BITS_10;
	sd_0_bank_size = BANK_SIZE_8;
	sd_0_row_size = ROW_SIZE_8K; /* DDR2 */
	/*	sd_0_row_size = ROW_SIZE_16K; // DDR3 */
	ddr3_mode[0] = 0;
	ddr3_mode[1] = 0;

	bDDR3Detected = crystalhd_flea_detect_ddr3(hw);
	if(bDDR3Detected)
	{
		ddr3_mode[0] = 1;
		sd_0_row_size = ROW_SIZE_16K; /* DDR3 */
	}

	/* Step 1. PLL Init */
	if (!crystalhd_flea_ddr_pll_config(hw, ddr2_speed_grade, 1, 0))
		return false;

	/* Step 2. DDR CTRL Init */
	crystalhd_flea_ddr_ctrl_init(hw, 0, ddr3_mode[0], ddr2_speed_grade[0], sd_0_col_size, sd_0_bank_size, sd_0_row_size, 0);

	/* Step 3 RTS Init - Real time scheduling memory arbiter */
	crystalhd_flea_ddr_arb_rts_init(hw);

	/* NAREN turn off ODT. The REF1 and SV1 and most customer designs allow this. */
	/* IF SOMEONE COMPLAINS ABOUT MEMORY OR DATA CORRUPTION LOOK HERE FIRST */

	/*hw->pfnWriteDevRegister(hw->adp, BCHP_DDR23_CTL_REGS_0_LOAD_EMODE_CMD, 0x02, false); */

	/*regVal = hw->pfnReadDevRegister(hw->adp, BCHP_DDR23_CTL_REGS_0_PARAMS3);
	regVal &= ~(BCHP_DDR23_CTL_REGS_0_PARAMS3_wr_odt_en_MASK);
	hw->pfnWriteDevRegister(hw->adp, BCHP_DDR23_CTL_REGS_0_PARAMS3, regVal);*/

	/*regVal = hw->pfnReadDevRegister(hw->adp, BCHP_DDR23_PHY_BYTE_LANE_1_DRIVE_PAD_CTL);
	regVal |= BCHP_DDR23_PHY_BYTE_LANE_1_DRIVE_PAD_CTL_seltxdrv_ci_MASK;
	hw->pfnWriteDevRegister(hw->adp, BCHP_DDR23_PHY_BYTE_LANE_1_DRIVE_PAD_CTL, regVal);

	regVal = hw->pfnReadDevRegister(hw->adp, BCHP_DDR23_PHY_BYTE_LANE_0_DRIVE_PAD_CTL);
	regVal |= BCHP_DDR23_PHY_BYTE_LANE_0_DRIVE_PAD_CTL_seltxdrv_ci_MASK;
	hw->pfnWriteDevRegister(hw->adp, BCHP_DDR23_PHY_BYTE_LANE_0_DRIVE_PAD_CTL, regVal);*/

	regVal = hw->pfnReadDevRegister(hw->adp, BCHP_DDR23_PHY_BYTE_LANE_1_CLOCK_PAD_DISABLE);
	regVal |= BCHP_DDR23_PHY_BYTE_LANE_1_CLOCK_PAD_DISABLE_clk_pad_dis_MASK;
	hw->pfnWriteDevRegister(hw->adp, BCHP_DDR23_PHY_BYTE_LANE_1_CLOCK_PAD_DISABLE, regVal);

	regVal = hw->pfnReadDevRegister(hw->adp, BCHP_DDR23_PHY_BYTE_LANE_0_READ_CONTROL);
	regVal &= ~(BCHP_DDR23_PHY_BYTE_LANE_0_READ_CONTROL_dq_odt_enable_MASK);
	hw->pfnWriteDevRegister(hw->adp, BCHP_DDR23_PHY_BYTE_LANE_0_READ_CONTROL, regVal);

	regVal = hw->pfnReadDevRegister(hw->adp, BCHP_DDR23_PHY_BYTE_LANE_1_READ_CONTROL);
	regVal &= ~(BCHP_DDR23_PHY_BYTE_LANE_1_READ_CONTROL_dq_odt_enable_MASK);
	hw->pfnWriteDevRegister(hw->adp, BCHP_DDR23_PHY_BYTE_LANE_1_READ_CONTROL, regVal);

	return true;
}

uint32_t crystalhd_flea_reg_rd(struct crystalhd_adp *adp, uint32_t reg_off)
{
	uint32_t baseAddr = reg_off >> 16;
	uint32_t direct_off = reg_off & 0x0000FFFF;
	uint32_t val;
	unsigned long flags;
	void	*regAddr;

	if (!adp) {
		printk(KERN_ERR "%s: Invalid args\n", __func__);
		return 0;
	}

	if(baseAddr == 0 || baseAddr == FLEA_GISB_DIRECT_BASE) /* Direct Mapped Region */
	{
		if (adp->pci_i2o_len < sizeof(uint32_t) ||
		    direct_off > adp->pci_i2o_len - sizeof(uint32_t)) {
			dev_err(&adp->pdev->dev, "%s: reg_off out of range: 0x%08x\n",
					__func__, reg_off);
			return 0;
		}
		regAddr = adp->i2o_addr + direct_off;
		spin_lock_irqsave(&adp->gisb_lock, flags);
		val = readl(regAddr);
		spin_unlock_irqrestore(&adp->gisb_lock, flags);
		return val;
	}
	else /* non directly mapped region */
	{
		if(adp->pci_i2o_len < FLEA_GISB_INDIRECT_DATA + sizeof(uint32_t)) {
			printk("Un-expected mapped region size\n");
			return 0;
		}
		regAddr = adp->i2o_addr + FLEA_GISB_INDIRECT_ADDRESS;
		spin_lock_irqsave(&adp->gisb_lock, flags);
		writel(reg_off | 0x10000000, regAddr);
		regAddr = adp->i2o_addr + FLEA_GISB_INDIRECT_DATA;
		val = readl(regAddr);
		spin_unlock_irqrestore(&adp->gisb_lock, flags);
		return val;
	}
}

void crystalhd_flea_reg_wr(struct crystalhd_adp *adp, uint32_t reg_off, uint32_t val)
{
	uint32_t baseAddr = reg_off >> 16;
	uint32_t direct_off = reg_off & 0x0000FFFF;
	unsigned long flags;
	void	*regAddr;

	if (!adp) {
		printk(KERN_ERR "%s: Invalid args\n", __func__);
		return;
	}

	if(baseAddr == 0 || baseAddr == FLEA_GISB_DIRECT_BASE) /* Direct Mapped Region */
	{
		if (adp->pci_i2o_len < sizeof(uint32_t) ||
		    direct_off > adp->pci_i2o_len - sizeof(uint32_t)) {
			dev_err(&adp->pdev->dev, "%s: reg_off out of range: 0x%08x\n",
					__func__, reg_off);
					return ;
		}
		regAddr = adp->i2o_addr + direct_off;
		spin_lock_irqsave(&adp->gisb_lock, flags);
		writel(val, regAddr);
		spin_unlock_irqrestore(&adp->gisb_lock, flags);
	}
	else /* non directly mapped region */
	{
		if(adp->pci_i2o_len < FLEA_GISB_INDIRECT_DATA + sizeof(uint32_t)) {
			printk("Un-expected mapped region size\n");
			return;
		}
		regAddr = adp->i2o_addr + FLEA_GISB_INDIRECT_ADDRESS;
		spin_lock_irqsave(&adp->gisb_lock, flags);
		writel(reg_off | 0x10000000, regAddr);
		regAddr = adp->i2o_addr + FLEA_GISB_INDIRECT_DATA;
		writel(val, regAddr);
		spin_unlock_irqrestore(&adp->gisb_lock, flags);
	}
}

static uint32_t crystalhd_flea_dram_burst(uint32_t addr, uint32_t remaining)
{
	uint32_t mask = BCHP_MISC2_DIRECT_WINDOW_CONTROL_DIRECT_WINDOW_BASE_ADDR_MASK;
	uint32_t window_words = ((~mask + 1U) - (addr & ~mask)) / 4U;

	/* Bound IRQ-disabled PIO work and never cross the selected window. */
	if (remaining > 64U)
		remaining = 64U;
	if (remaining > window_words)
		remaining = window_words;
	return remaining;
}

/**
* crystalhd_flea_mem_rd - Read data from DRAM area.
* @hw: Hardware context.
* @start_off: Start offset.
* @dw_cnt: Count in dwords.
* @rd_buff: Buffer to copy the data from dram.
*
* Return:
*	Status.
*
* Dram read routine.
*/
BC_STATUS crystalhd_flea_mem_rd(struct crystalhd_hw *hw, uint32_t start_off,
								uint32_t dw_cnt, uint32_t *rd_buff)
{
	uint32_t ix = 0, addr = start_off;
	uint32_t mask = BCHP_MISC2_DIRECT_WINDOW_CONTROL_DIRECT_WINDOW_BASE_ADDR_MASK;
	unsigned long flags;

	if (!hw || !hw->adp || !rd_buff) {
		printk(KERN_ERR "%s: Invalid arg\n", __func__);
		return BC_STS_INV_ARG;
	}

	if (READ_ONCE(hw->dma_fault) ||
	    READ_ONCE(hw->FleaPowerState) == FLEA_PS_FAULT)
		return BC_STS_IO_ERROR;
	if( hw->FleaPowerState == FLEA_PS_LP_COMPLETE ) {
		/*printk(KERN_ERR "%s: Flea power down, cann't read memory.\n", __func__); */
		return BC_STS_BUSY;
	}

	if (!crystalhd_valid_dram_range(start_off, dw_cnt)) {
		printk(KERN_ERR "Access beyond DRAM limit at Addr 0x%x and size 0x%x words\n", start_off, dw_cnt);
		return BC_STS_ERROR;
	}

	while (ix < dw_cnt) {
		uint32_t count = crystalhd_flea_dram_burst(addr, dw_cnt - ix);
		uint32_t n;

		/* The register helper's GISB lock alone cannot pin this window
		 * against interrupt-side DRAM transfers after selecting it.
		 */
		spin_lock_irqsave(&hw->adp->dram_lock, flags);
		if (READ_ONCE(hw->dma_fault)) {
			spin_unlock_irqrestore(&hw->adp->dram_lock, flags);
			return BC_STS_IO_ERROR;
		}
		hw->pfnWriteDevRegister(hw->adp, BCHP_MISC2_DIRECT_WINDOW_CONTROL,
			(addr & mask) |
			BCHP_MISC2_DIRECT_WINDOW_CONTROL_DIRECT_WINDOW_ENABLE_MASK);
		for (n = 0; n < count; n++, addr += 4)
			rd_buff[ix + n] = readl(hw->adp->mem_addr + (addr & ~mask));
		spin_unlock_irqrestore(&hw->adp->dram_lock, flags);
		ix += count;
	}
	return BC_STS_SUCCESS;
}

/**
* crystalhd_flea_mem_wr - Write data to DRAM area.
* @hw: Hardware context.
* @start_off: Start offset.
* @dw_cnt: Count in dwords.
* @wr_buff: Data Buffer to be written.
*
* Return:
*	Status.
*
* Dram write routine.
*/
BC_STATUS crystalhd_flea_mem_wr(struct crystalhd_hw *hw, uint32_t start_off,
								uint32_t dw_cnt, const uint32_t *wr_buff)
{
	uint32_t ix = 0, addr = start_off;
	uint32_t mask = BCHP_MISC2_DIRECT_WINDOW_CONTROL_DIRECT_WINDOW_BASE_ADDR_MASK;
	unsigned long flags;

	if (!hw || !hw->adp || !wr_buff) {
		printk(KERN_ERR "%s: Invalid arg\n", __func__);
		return BC_STS_INV_ARG;
	}

	if (READ_ONCE(hw->dma_fault) ||
	    READ_ONCE(hw->FleaPowerState) == FLEA_PS_FAULT)
		return BC_STS_IO_ERROR;
	if( hw->FleaPowerState == FLEA_PS_LP_COMPLETE ) {
		/*printk(KERN_ERR "%s: Flea power down, cann't write memory.\n", __func__); */
		return BC_STS_BUSY;
	}

	if (!crystalhd_valid_dram_range(start_off, dw_cnt)) {
		printk("Access beyond DRAM limit at Addr 0x%x and size 0x%x words\n", start_off, dw_cnt);
		return BC_STS_ERROR;
	}

	while (ix < dw_cnt) {
		uint32_t count = crystalhd_flea_dram_burst(addr, dw_cnt - ix);
		uint32_t offset = addr & ~mask, n;

		spin_lock_irqsave(&hw->adp->dram_lock, flags);
		if (READ_ONCE(hw->dma_fault)) {
			spin_unlock_irqrestore(&hw->adp->dram_lock, flags);
			return BC_STS_IO_ERROR;
		}
		hw->pfnWriteDevRegister(hw->adp, BCHP_MISC2_DIRECT_WINDOW_CONTROL,
			(addr & mask) |
			BCHP_MISC2_DIRECT_WINDOW_CONTROL_DIRECT_WINDOW_ENABLE_MASK);
		for (n = 0; n < count; n++, addr += 4)
			writel(wr_buff[ix + n], hw->adp->mem_addr + (addr & ~mask));
		/* Flush posted writes while their window is still selected. */
		readl(hw->adp->mem_addr + offset);
		spin_unlock_irqrestore(&hw->adp->dram_lock, flags);
		ix += count;
	}

	return BC_STS_SUCCESS;
}


/* Runtime power register helpers. The caller owns hw->lock and the
 * transition reservation. These bounded checks do not prove physical leases.
 */
#define FLEA_PM_CORE_CLOCKS (BCHP_CLK_PM_CTRL_DIS_ARM_CLK_MASK | \
	BCHP_CLK_PM_CTRL_DIS_AVD_CLK_MASK | \
	BCHP_CLK_PM_CTRL_DIS_AVD_108_CLK_MASK | \
	BCHP_CLK_PM_CTRL_DIS_AVD_216_CLK_MASK)
#define FLEA_PM_BRIDGE_CLOCKS (BCHP_CLK_PM_CTRL_DIS_BLINK_108_CLK_MASK | \
	BCHP_CLK_PM_CTRL_DIS_BLINK_216_CLK_MASK)
#define FLEA_PM_DDR_CLOCKS (BCHP_CLK_PM_CTRL_DIS_DDR_108_CLK_MASK | \
	BCHP_CLK_PM_CTRL_DIS_DDR_216_CLK_MASK)
#define FLEA_PM_MEMC_IDLE (BCHP_DDR23_CTL_REGS_0_CTL_STATUS_idle_MASK | \
	BCHP_DDR23_CTL_REGS_0_CTL_STATUS_rd_fifo1_empty_MASK | \
	BCHP_DDR23_CTL_REGS_0_CTL_STATUS_rd_fifo2_empty_MASK | \
	BCHP_DDR23_CTL_REGS_0_CTL_STATUS_rd_fifo3_empty_MASK)
#define FLEA_PM_MEMC_KNOWN (FLEA_PM_MEMC_IDLE | \
	BCHP_DDR23_CTL_REGS_0_CTL_STATUS_clke_MASK)

struct flea_power_register {
	uint32_t address;
	uint32_t owned;
	uint32_t reserved;
};

static const struct flea_power_register flea_power_registers[FLEA_PR_COUNT] = {
	[FLEA_PR_CLOCK] = { BCHP_CLK_PM_CTRL,
		FLEA_PM_CORE_CLOCKS | FLEA_PM_DDR_CLOCKS,
		BCHP_CLK_PM_CTRL_reserved_for_eco0_MASK |
		BCHP_CLK_PM_CTRL_reserved1_MASK |
		BCHP_CLK_PM_CTRL_reserved2_MASK |
		BCHP_CLK_PM_CTRL_reserved3_MASK },
	[FLEA_PR_PHY_CLOCK] = { BCHP_DDR23_PHY_CONTROL_REGS_CLK_PM_CTRL,
		BCHP_DDR23_PHY_CONTROL_REGS_CLK_PM_CTRL_DIS_DDR_CLK_MASK,
		BCHP_DDR23_PHY_CONTROL_REGS_CLK_PM_CTRL_reserved0_MASK },
	[FLEA_PR_PLL] = { BCHP_DDR23_PHY_CONTROL_REGS_PLL_CONFIG,
		BCHP_DDR23_PHY_CONTROL_REGS_PLL_CONFIG_PWRDN_MASK,
		BCHP_DDR23_PHY_CONTROL_REGS_PLL_CONFIG_reserved0_MASK |
		BCHP_DDR23_PHY_CONTROL_REGS_PLL_CONFIG_reserved1_MASK |
		BCHP_DDR23_PHY_CONTROL_REGS_PLL_CONFIG_reserved2_MASK },
	[FLEA_PR_PHY_REGULATOR] = { BCHP_DDR23_PHY_CONTROL_REGS_CLOCK_REG_CONTROL,
		BCHP_DDR23_PHY_CONTROL_REGS_CLOCK_REG_CONTROL_pwrdn_MASK,
		BCHP_DDR23_PHY_CONTROL_REGS_CLOCK_REG_CONTROL_reserved0_MASK },
	[FLEA_PR_LANE0_REGULATOR] = { BCHP_DDR23_PHY_BYTE_LANE_0_CLOCK_REG_CONTROL,
		BCHP_DDR23_PHY_BYTE_LANE_0_CLOCK_REG_CONTROL_pwrdn_MASK,
		BCHP_DDR23_PHY_BYTE_LANE_0_CLOCK_REG_CONTROL_reserved0_MASK },
	[FLEA_PR_LANE1_REGULATOR] = { BCHP_DDR23_PHY_BYTE_LANE_1_CLOCK_REG_CONTROL,
		BCHP_DDR23_PHY_BYTE_LANE_1_CLOCK_REG_CONTROL_pwrdn_MASK,
		BCHP_DDR23_PHY_BYTE_LANE_1_CLOCK_REG_CONTROL_reserved0_MASK },
	[FLEA_PR_PHY_PAD] = { BCHP_DDR23_PHY_CONTROL_REGS_IDLE_PAD_CONTROL,
		BCHP_DDR23_PHY_CONTROL_REGS_IDLE_PAD_CONTROL_idle_MASK |
		BCHP_DDR23_PHY_CONTROL_REGS_IDLE_PAD_CONTROL_ctl_iddq_MASK |
		BCHP_DDR23_PHY_CONTROL_REGS_IDLE_PAD_CONTROL_rxenb_MASK |
		BCHP_DDR23_PHY_CONTROL_REGS_IDLE_PAD_CONTROL_cke_reb_MASK,
		BCHP_DDR23_PHY_CONTROL_REGS_IDLE_PAD_CONTROL_reserved0_MASK |
		BCHP_DDR23_PHY_CONTROL_REGS_IDLE_PAD_CONTROL_reserved1_MASK |
		BCHP_DDR23_PHY_CONTROL_REGS_IDLE_PAD_CONTROL_reserved2_MASK },
	[FLEA_PR_LANE0_PAD] = { BCHP_DDR23_PHY_BYTE_LANE_0_IDLE_PAD_CONTROL,
		BCHP_DDR23_PHY_BYTE_LANE_0_IDLE_PAD_CONTROL_idle_MASK |
		BCHP_DDR23_PHY_BYTE_LANE_0_IDLE_PAD_CONTROL_dm_iddq_MASK |
		BCHP_DDR23_PHY_BYTE_LANE_0_IDLE_PAD_CONTROL_dq_iddq_MASK |
		BCHP_DDR23_PHY_BYTE_LANE_0_IDLE_PAD_CONTROL_read_enb_iddq_MASK |
		BCHP_DDR23_PHY_BYTE_LANE_0_IDLE_PAD_CONTROL_dqs_iddq_MASK |
		BCHP_DDR23_PHY_BYTE_LANE_0_IDLE_PAD_CONTROL_clk_iddq_MASK,
		BCHP_DDR23_PHY_BYTE_LANE_0_IDLE_PAD_CONTROL_reserved0_MASK },
	[FLEA_PR_LANE1_PAD] = { BCHP_DDR23_PHY_BYTE_LANE_1_IDLE_PAD_CONTROL,
		BCHP_DDR23_PHY_BYTE_LANE_1_IDLE_PAD_CONTROL_idle_MASK |
		BCHP_DDR23_PHY_BYTE_LANE_1_IDLE_PAD_CONTROL_dm_iddq_MASK |
		BCHP_DDR23_PHY_BYTE_LANE_1_IDLE_PAD_CONTROL_dq_iddq_MASK |
		BCHP_DDR23_PHY_BYTE_LANE_1_IDLE_PAD_CONTROL_read_enb_iddq_MASK |
		BCHP_DDR23_PHY_BYTE_LANE_1_IDLE_PAD_CONTROL_dqs_iddq_MASK |
		BCHP_DDR23_PHY_BYTE_LANE_1_IDLE_PAD_CONTROL_clk_iddq_MASK,
		BCHP_DDR23_PHY_BYTE_LANE_1_IDLE_PAD_CONTROL_reserved0_MASK },
	[FLEA_PR_PARAMS] = { BCHP_DDR23_CTL_REGS_0_PARAMS2,
		BCHP_DDR23_CTL_REGS_0_PARAMS2_clke_MASK,
		BCHP_DDR23_CTL_REGS_0_PARAMS2_reserved0_MASK },
	[FLEA_PR_REFRESH] = { BCHP_PRI_ARB_CONTROL_REGS_REFRESH_CTL_0,
		BCHP_PRI_ARB_CONTROL_REGS_REFRESH_CTL_0_enable_MASK,
		BCHP_PRI_ARB_CONTROL_REGS_REFRESH_CTL_0_reserved0_MASK },
	[FLEA_PR_ARBITER] = { BCHP_PRI_ARB_CONTROL_REGS_MASTER_CTL,
		BCHP_PRI_ARB_CONTROL_REGS_MASTER_CTL_arb_disable_MASK,
		BCHP_PRI_ARB_CONTROL_REGS_MASTER_CTL_reserved0_MASK },
};

static bool flea_pm_read(struct crystalhd_hw *hw, enum FLEA_POWER_REG index,
			 uint32_t *value)
{
	const struct flea_power_register *reg = &flea_power_registers[index];

	*value = hw->pfnReadDevRegister(hw->adp, reg->address);
	return *value != ~0U && !(*value & reg->reserved);
}

/* Verify only bits owned by this transition; preserve other live fields. */
static bool flea_pm_update(struct crystalhd_hw *hw, enum FLEA_POWER_REG index,
			   uint32_t mask, uint32_t value)
{
	const struct flea_power_register *reg = &flea_power_registers[index];
	uint32_t live;

	if (!flea_pm_read(hw, index, &live))
		return false;
	mask &= reg->owned;
	hw->pfnWriteDevRegister(hw->adp, reg->address,
			       (live & ~mask) | (value & mask));
	return flea_pm_read(hw, index, &live) &&
	       (live & mask) == (value & mask);
}

static bool flea_pm_restore(struct crystalhd_hw *hw, enum FLEA_POWER_REG index,
			    uint32_t mask)
{
	return flea_pm_update(hw, index, mask, hw->FleaPowerSaved.value[index]);
}

static bool flea_pm_capture(struct crystalhd_hw *hw)
{
	unsigned int i;
	uint32_t value, owned;

	hw->FleaPowerSaved.valid = false;
	/* BLINK must remain available for register access and fault masking.
	 * Reject an inaccessible clock baseline before touching DDR/PHY words.
	 */
	for (i = 0; i < FLEA_PR_COUNT; i++) {
		if (!flea_pm_read(hw, i, &value))
			return false;
		owned = value & flea_power_registers[i].owned;
		/* Do not retain a baseline that itself requests clock/power gating. */
		if ((i == FLEA_PR_CLOCK && (value & FLEA_PM_BRIDGE_CLOCKS)) ||
		    (i <= FLEA_PR_LANE1_REGULATOR && owned) ||
		    ((i == FLEA_PR_PARAMS || i == FLEA_PR_ARBITER) && owned) ||
		    (i == FLEA_PR_REFRESH && !owned) ||
		    (i == FLEA_PR_PHY_PAD &&
		     (owned & BCHP_DDR23_PHY_CONTROL_REGS_IDLE_PAD_CONTROL_idle_MASK)) ||
		    (i == FLEA_PR_LANE0_PAD &&
		     (owned & BCHP_DDR23_PHY_BYTE_LANE_0_IDLE_PAD_CONTROL_idle_MASK)) ||
		    (i == FLEA_PR_LANE1_PAD &&
		     (owned & BCHP_DDR23_PHY_BYTE_LANE_1_IDLE_PAD_CONTROL_idle_MASK)))
			return false;
		hw->FleaPowerSaved.value[i] = owned;
	}
	hw->FleaPowerSaved.valid = true;
	return true;
}

/* Software polling budgets only, not a new hardware settling guarantee. */
static bool flea_pm_wait_cke(struct crystalhd_hw *hw, uint32_t expected)
{
	unsigned int i;
	uint32_t value;

	for (i = 0; i < 100; i++) {
		value = hw->pfnReadDevRegister(hw->adp,
					     BCHP_DDR23_CTL_REGS_0_CTL_STATUS);
		if (value & ~FLEA_PM_MEMC_KNOWN)
			return false;
		if ((value & BCHP_DDR23_CTL_REGS_0_CTL_STATUS_clke_MASK) == expected)
			return true;
	}
	return false;
}

static bool flea_pm_wait_pll(struct crystalhd_hw *hw)
{
	unsigned int i;
	uint32_t value;

	for (i = 0; i < 100; i++) {
		value = hw->pfnReadDevRegister(hw->adp,
					BCHP_DDR23_PHY_CONTROL_REGS_PLL_STATUS);
		if (value & BCHP_DDR23_PHY_CONTROL_REGS_PLL_STATUS_reserved0_MASK)
			return false;
		if (value & BCHP_DDR23_PHY_CONTROL_REGS_PLL_STATUS_LOCK_MASK)
			return true;
	}
	return false;
}

static BC_STATUS flea_pm_fault(struct crystalhd_hw *hw, const char *stage)
{
	if (hw->FleaPowerState != FLEA_PS_FAULT)
		dev_err(&hw->adp->pdev->dev, "runtime power fault: %s\n", stage);
	hw->FleaPowerState = FLEA_PS_FAULT;
	crystalhd_hw_publish_fault(hw);
	return BC_STS_IO_ERROR;
}

static void flea_pm_restore_watchdogs(struct crystalhd_hw *hw)
{
#ifdef _POWER_HANDLE_AVD_WATCHDOG_
	hw->pfnWriteDevRegister(hw->adp, BCHP_DECODE_CPUREGS_0_REG_WATCHDOG_TMR,
			       hw->OLWatchDogTimer);
	hw->pfnWriteDevRegister(hw->adp, BCHP_DECODE_CPUREGS2_0_REG_WATCHDOG_TMR,
			       hw->ILWatchDogTimer);
#else
	(void)hw;
#endif
}

/* Runtime power transitions. */
static
BC_STATUS crystalhd_flea_runtime_power_up(struct crystalhd_hw *hw)
{
	uint64_t currTick, temp_64;
	uint32_t totalTick_Hi, TickSpentInPD_Hi;
	long totalTick_Hi_f, TickSpentInPD_Hi_f;

	if (!hw->FleaPowerSaved.valid)
		return flea_pm_fault(hw, "snapshot read");
	if (!flea_pm_restore(hw, FLEA_PR_CLOCK, FLEA_PM_DDR_CLOCKS))
		return flea_pm_fault(hw, "restore read");
	udelay(25);
	if (!flea_pm_restore(hw, FLEA_PR_PHY_REGULATOR, ~0U) ||
	    !flea_pm_restore(hw, FLEA_PR_PLL, ~0U) ||
	    !flea_pm_restore(hw, FLEA_PR_PHY_CLOCK, ~0U))
		return flea_pm_fault(hw, "restore read");
	udelay(25);
	if (!flea_pm_wait_pll(hw))
		return flea_pm_fault(hw, "PLL lock");
	if (!flea_pm_restore(hw, FLEA_PR_PHY_PAD, ~0U) ||
	    !flea_pm_restore(hw, FLEA_PR_LANE0_REGULATOR, ~0U) ||
	    !flea_pm_restore(hw, FLEA_PR_LANE1_REGULATOR, ~0U) ||
	    !flea_pm_restore(hw, FLEA_PR_LANE0_PAD, ~0U) ||
	    !flea_pm_restore(hw, FLEA_PR_LANE1_PAD, ~0U) ||
	    !flea_pm_restore(hw, FLEA_PR_REFRESH, ~0U) ||
	    !flea_pm_restore(hw, FLEA_PR_PARAMS, ~0U))
		return flea_pm_fault(hw, "restore read");
	udelay(5);
	hw->pfnWriteDevRegister(hw->adp, BCHP_DDR23_CTL_REGS_0_REFRESH_CMD, 0x60);
	if (!flea_pm_wait_cke(hw, hw->FleaPowerSaved.cke))
		return flea_pm_fault(hw, "CKE wake");
	if (!flea_pm_restore(hw, FLEA_PR_CLOCK, FLEA_PM_CORE_CLOCKS) ||
	    !flea_pm_restore(hw, FLEA_PR_ARBITER, ~0U))
		return flea_pm_fault(hw, "restore read");
	flea_pm_restore_watchdogs(hw);
	hw->FleaPowerSaved.valid = false;

	currTick = rdtsc_ordered();
	hw->TickSpentInPD += (currTick - hw->TickStartInPD);

	temp_64 = (hw->TickSpentInPD)>>24;
	TickSpentInPD_Hi = (uint32_t)(temp_64);
	TickSpentInPD_Hi_f = (long)TickSpentInPD_Hi;

	temp_64 = (currTick - hw->TickCntDecodePU)>>24;
	totalTick_Hi = (uint32_t)(temp_64);
	totalTick_Hi_f = (long)totalTick_Hi;

	if( totalTick_Hi_f <= 0 )
	{
		temp_64 = (hw->TickSpentInPD);
		TickSpentInPD_Hi = (uint32_t)(temp_64);
		TickSpentInPD_Hi_f = (long)TickSpentInPD_Hi;

		temp_64 = (currTick - hw->TickCntDecodePU);
		totalTick_Hi = (uint32_t)(temp_64);
		totalTick_Hi_f = (long)totalTick_Hi;
	}

	if( totalTick_Hi_f <= 0 )
	{
		printk("totalTick_Hi_f <= 0, set hw->PDRatio = 60\n");
		hw->PDRatio = 60;
	}
	else
		hw->PDRatio = (TickSpentInPD_Hi_f * 100) / totalTick_Hi_f;

	/*printk("Ticks currently spent in PD: 0x%llx Total: 0x%llx Ratio %d,\n", */
	/*	hw->TickSpentInPD, (currTick - hw->TickCntDecodePU), hw->PDRatio); */

	/* NAREN check if the PD ratio is greater than 75. If so, try to increase the PauseThreshold to improve the ratio */
	/* never go higher than the default threshold */
	if((hw->PDRatio > 75) && (hw->PauseThreshold < hw->DefaultPauseThreshold))
	{
		/*printk("Current PDRatio:%u, PauseThreshold:%u, DefaultPauseThreshold:%u, incress PauseThreshold.\n", */
		/*		hw->PDRatio, hw->PauseThreshold, hw->DefaultPauseThreshold); */
		hw->PauseThreshold++;
	}
	else
	{
		/*printk("Current PDRatio:%u, PauseThreshold:%u, DefaultPauseThreshold:%u, don't incress PauseThreshold.\n", */
		/*		hw->PDRatio, hw->PauseThreshold, hw->DefaultPauseThreshold); */
	}

	return BC_STS_SUCCESS;
}

static
enum FLEA_POWER_DOWN_RESULT crystalhd_flea_runtime_power_dn(struct crystalhd_hw *hw)
{
	uint32_t value = 0;
	unsigned int i;

	if (!flea_pm_capture(hw))
		return FLEA_PD_REFUSED;
#ifdef _POWER_HANDLE_AVD_WATCHDOG_
	hw->OLWatchDogTimer = hw->pfnReadDevRegister(hw->adp,
					BCHP_DECODE_CPUREGS_0_REG_WATCHDOG_TMR);
	hw->ILWatchDogTimer = hw->pfnReadDevRegister(hw->adp,
					BCHP_DECODE_CPUREGS2_0_REG_WATCHDOG_TMR);
	hw->pfnWriteDevRegister(hw->adp, BCHP_DECODE_CPUREGS_0_REG_WATCHDOG_TMR,
			       ~0U);
	hw->pfnWriteDevRegister(hw->adp, BCHP_DECODE_CPUREGS2_0_REG_WATCHDOG_TMR,
			       ~0U);
#endif
	if (!flea_pm_update(hw, FLEA_PR_ARBITER, ~0U, ~0U))
		goto restore_fault;
	/* Idle and read FIFO bits remain refusal guards, not physical leases. */
	for (i = 0; i <= 400; i++) {
		value = hw->pfnReadDevRegister(hw->adp,
					     BCHP_DDR23_CTL_REGS_0_CTL_STATUS);
		if (value & ~FLEA_PM_MEMC_KNOWN)
			goto refuse;
		if ((value & FLEA_PM_MEMC_IDLE) == FLEA_PM_MEMC_IDLE)
			break;
		udelay(10);
	}
	if ((value & FLEA_PM_MEMC_IDLE) != FLEA_PM_MEMC_IDLE ||
	    !(value & BCHP_DDR23_CTL_REGS_0_CTL_STATUS_clke_MASK))
		goto refuse;
	hw->FleaPowerSaved.cke = value & BCHP_DDR23_CTL_REGS_0_CTL_STATUS_clke_MASK;

	if (!flea_pm_update(hw, FLEA_PR_PARAMS, ~0U, ~0U))
		goto recover;
	hw->pfnWriteDevRegister(hw->adp, BCHP_DDR23_CTL_REGS_0_REFRESH_CMD, 0x60);
	if (!flea_pm_update(hw, FLEA_PR_REFRESH, ~0U, 0) ||
	    !flea_pm_wait_cke(hw, 0))
		goto recover;

	if (!flea_pm_update(hw, FLEA_PR_CLOCK, FLEA_PM_CORE_CLOCKS, ~0U) ||
	    !flea_pm_update(hw, FLEA_PR_LANE0_PAD, ~0U, ~0U) ||
	    !flea_pm_update(hw, FLEA_PR_LANE1_PAD, ~0U, ~0U) ||
	    !flea_pm_update(hw, FLEA_PR_LANE0_REGULATOR, ~0U, ~0U) ||
	    !flea_pm_update(hw, FLEA_PR_LANE1_REGULATOR, ~0U, ~0U) ||
	    !flea_pm_update(hw, FLEA_PR_PHY_PAD, ~0U, ~0U) ||
	    !flea_pm_update(hw, FLEA_PR_PHY_CLOCK, ~0U, ~0U) ||
	    !flea_pm_update(hw, FLEA_PR_PLL, ~0U, ~0U) ||
	    !flea_pm_update(hw, FLEA_PR_PHY_REGULATOR, ~0U, ~0U) ||
	    !flea_pm_update(hw, FLEA_PR_CLOCK, FLEA_PM_DDR_CLOCKS, ~0U))
		goto restore_fault;
	hw->TickStartInPD = rdtsc_ordered();
	hw->DrvPauseCnt++;
	return FLEA_PD_ENTERED;

recover:
	/* Only a checked recovery may release the arbiter after a request. */
	if (!flea_pm_restore(hw, FLEA_PR_REFRESH, ~0U) ||
	    !flea_pm_restore(hw, FLEA_PR_PARAMS, ~0U))
		goto recovery_fault;
	udelay(5);
	hw->pfnWriteDevRegister(hw->adp, BCHP_DDR23_CTL_REGS_0_REFRESH_CMD, 0x60);
	if (!flea_pm_wait_cke(hw, hw->FleaPowerSaved.cke))
		goto recovery_fault;
refuse:
	if (!flea_pm_restore(hw, FLEA_PR_ARBITER, ~0U))
		goto restore_fault;
	flea_pm_restore_watchdogs(hw);
	hw->FleaPowerSaved.valid = false;
	return FLEA_PD_REFUSED;

recovery_fault:
	flea_pm_fault(hw, "self-refresh recovery");
	return FLEA_PD_FAULT;
restore_fault:
	flea_pm_fault(hw, "restore read");
	return FLEA_PD_FAULT;
}

bool crystalhd_flea_detect_fw_alive(struct crystalhd_hw *hw)
{
	uint32_t pollCnt		= 0;
	uint32_t hbCnt			= 0;
	uint32_t heartBeatReg1	= 0;
	uint32_t heartBeatReg2	= 0;
	bool	 bRetVal		= false;

	heartBeatReg1 = hw->pfnReadDevRegister(hw->adp, HEART_BEAT_REGISTER);
	while(1)
	{
		heartBeatReg2 = hw->pfnReadDevRegister(hw->adp, HEART_BEAT_REGISTER);
		if(heartBeatReg1 != heartBeatReg2) {
			hbCnt++;
			heartBeatReg1 = heartBeatReg2;
		}

		if(hbCnt >= HEART_BEAT_POLL_CNT) {
			bRetVal = true;
			break;
		}

		pollCnt++;
		if(pollCnt >= FLEA_MAX_POLL_CNT) {
			bRetVal = false;
			break;
		}

		msleep_interruptible(1);
	}

	return bRetVal;
}

void crystalhd_flea_handle_PicQSts_intr(struct crystalhd_hw *hw)
{
	uint32_t	newChBitmap=0;

	newChBitmap = hw->pfnReadDevRegister(hw->adp, RX_DMA_PIC_QSTS_MBOX);

	hw->PicQSts = newChBitmap;

	/* -- For link we were enabling the capture on format change
	   -- For Flea, we will get a PicQSts interrupt where we will
	   -- enable the capture. */

	if(hw->RxCaptureState != 1)
	{
		hw->RxCaptureState = 1;
	}
}

void crystalhd_flea_update_tx_buff_info(struct crystalhd_hw *hw)
{
	TX_INPUT_BUFFER_INFO	TxBuffInfo = {0};
	uint32_t ReadSzInDWords=0;
	BC_STATUS sts;

	ReadSzInDWords = (sizeof(TxBuffInfo) - sizeof(TxBuffInfo.Reserved))/4;
	sts = hw->pfnDevDRAMRead(hw, hw->TxBuffInfoAddr, ReadSzInDWords,
			       (uint32_t *)&TxBuffInfo);
	if (sts != BC_STS_SUCCESS)
		goto invalidate;

	if(TxBuffInfo.DramBuffAdd % 4)
	{
		printk("Tx Err:: DWORD UNAligned Tx Addr. Not Updating\n");
		goto invalidate;
	}

	hw->TxFwInputBuffInfo.DramBuffAdd			= TxBuffInfo.DramBuffAdd;
	hw->TxFwInputBuffInfo.DramBuffSzInBytes		= TxBuffInfo.DramBuffSzInBytes;
	hw->TxFwInputBuffInfo.Flags					= TxBuffInfo.Flags;
	hw->TxFwInputBuffInfo.HostXferSzInBytes		= TxBuffInfo.HostXferSzInBytes;
	hw->TxFwInputBuffInfo.SeqNum				= TxBuffInfo.SeqNum;

	return;

invalidate:
	/* A failed notification must not leave previously advertised space usable. */
	hw->TxFwInputBuffInfo.DramBuffAdd = 0;
	hw->TxFwInputBuffInfo.DramBuffSzInBytes = 0;
}

static BC_STATUS crystalhd_flea_set_next_power_state(struct crystalhd_hw *hw,
					      enum FLEA_STATE_CH_EVENT PowerEvt);

static BC_STATUS crystalhd_flea_publish_fll(struct crystalhd_hw *hw,
					  bool bCleanupContext)
{
	unsigned long flags = 0;
	uint32_t freeListLen = 0;
	BC_STATUS sts = BC_STS_SUCCESS;
	bool reserved = false, recheck = false;

	if (READ_ONCE(hw->dma_fault) ||
	    READ_ONCE(hw->FleaPowerState) == FLEA_PS_FAULT)
		return BC_STS_IO_ERROR;
	/* The cleanup heartbeat can sleep. Reserve an active device before
	 * probing it, without holding the spinlock across that wait.
	 */
	if (bCleanupContext && READ_ONCE(hw->FleaEnablePWM)) {
		spin_lock_irqsave(&hw->lock, flags);
		if (!READ_ONCE(hw->adp->present) || READ_ONCE(hw->dma_fault) ||
		    !READ_ONCE(hw->dev_started))
			sts = BC_STS_IO_ERROR;
		else if (hw->FleaPowerState != FLEA_PS_ACTIVE ||
			 hw->FleaPowerTransition)
			sts = BC_STS_BUSY;
		else {
			hw->FleaPowerTransition = true;
			reserved = true;
		}
		spin_unlock_irqrestore(&hw->lock, flags);
		if (sts != BC_STS_SUCCESS)
			return sts;
	}
	/*
	* When we are doing the cleanup we should update DRAM only if the
	* firmware is running. So Detect the heart beat.
	*/
	if (bCleanupContext && !crystalhd_flea_detect_fw_alive(hw)) {
		if (!reserved)
			return BC_STS_SUCCESS;
		/* Keep failed cleanup write-free, including deferred PM work. */
		sts = BC_STS_IO_ERROR;
		spin_lock_irqsave(&hw->lock, flags);
		goto unlock;
	}

	spin_lock_irqsave(&hw->lock, flags);
	if (READ_ONCE(hw->dma_fault) || hw->FleaPowerState == FLEA_PS_FAULT) {
		sts = BC_STS_IO_ERROR;
		goto unlock;
	}
	if (READ_ONCE(hw->FleaEnablePWM)) {
		if (!READ_ONCE(hw->adp->present) || READ_ONCE(hw->dma_fault) ||
		    !READ_ONCE(hw->dev_started)) {
			sts = BC_STS_IO_ERROR;
			goto unlock;
		}
		/* Queue changes must not overwrite a zero-FLL pause or publish
		 * work while another caller is bringing the device back up.
		 */
		if (hw->FleaPowerState != FLEA_PS_ACTIVE ||
		    (hw->FleaPowerTransition && !reserved)) {
			sts = BC_STS_BUSY;
			goto unlock;
		}
	}
	/* Sample under the publication lock: an older notifier must not
	 * overwrite a newer FLL value with its pre-lock queue snapshot.
	 */
	freeListLen = crystalhd_hw_count_free_rx_pkts_locked(hw);
	sts = hw->pfnDevDRAMWrite(hw, hw->FleaFLLUpdateAddr, 1, &freeListLen);
unlock:
	if (reserved) {
		hw->FleaPowerTransition = false;
		if (sts == BC_STS_SUCCESS) {
			recheck = hw->FleaPowerRecheck;
			hw->FleaPowerRecheck = false;
		}
	}
	spin_unlock_irqrestore(&hw->lock, flags);
	if (recheck)
		crystalhd_flea_set_next_power_state(hw, FLEA_EVT_CMD_COMP);

	return sts;
}

/* Keep the void notifier ABI: RX queue ownership has already transferred. */
void crystalhd_flea_notify_fll_change(struct crystalhd_hw *hw, bool bCleanupContext)
{
	crystalhd_flea_publish_fll(hw, bCleanupContext);
}


static
void crystalhd_flea_init_power_state(struct crystalhd_hw *hw)
{
	hw->FleaEnablePWM = false;	/* Runtime power-down remains disabled. */
	hw->FleaPowerState = FLEA_PS_NONE;
	hw->FleaPowerTransition = false;
	hw->FleaPowerRecheck = false;
	hw->FleaTxActive = 0;
	hw->FleaIrqActive = 0;
	hw->FleaRawActive = 0;
	hw->FleaPowerSaved.valid = false;
}

/* Only the selector's transition reservation owner may call this helper. */
static
BC_STATUS crystalhd_flea_set_power_state(struct crystalhd_hw *hw,
					 enum FLEA_POWER_STATES NewState)
{
	BC_STATUS sts = BC_STS_SUCCESS;
	uint32_t tempFLL = 0;
	uint32_t freeListLen;
	unsigned long flags;
	bool replay_pic = false, replay_tx = false;
	enum FLEA_POWER_DOWN_RESULT down;

	spin_lock_irqsave(&hw->lock, flags);
	if (!READ_ONCE(hw->adp->present) || READ_ONCE(hw->dma_fault) ||
	    !READ_ONCE(hw->dev_started)) {
		sts = BC_STS_IO_ERROR;
		goto unlock;
	}

	switch (NewState) {
	case FLEA_PS_ACTIVE:
		if (hw->FleaPowerState == FLEA_PS_ACTIVE)
			break;
		if (hw->FleaPowerState != FLEA_PS_LP_PENDING &&
		    hw->FleaPowerState != FLEA_PS_LP_COMPLETE) {
			sts = BC_STS_ERR_USAGE;
			break;
		}
		if (hw->FleaPowerState == FLEA_PS_LP_COMPLETE) {
			sts = crystalhd_flea_runtime_power_up(hw);
			if (sts != BC_STS_SUCCESS)
				break;
			/* Clocks are on; admission still needs FLL publication. */
			hw->FleaPowerState = FLEA_PS_LP_PENDING;
		}
		/* Restore the current count, not a snapshot from before wake. */
		freeListLen = crystalhd_hw_count_free_rx_pkts_locked(hw);
		if (hw->pfnDevDRAMWrite(hw, hw->FleaFLLUpdateAddr, 1,
				      &freeListLen) != BC_STS_SUCCESS) {
			sts = BC_STS_IO_ERROR;
			break;
		}
		hw->FleaPowerState = FLEA_PS_ACTIVE;
		replay_pic = hw->PwrDwnPiQIntr;
		replay_tx = hw->PwrDwnTxIntr;
		hw->PwrDwnPiQIntr = false;
		hw->PwrDwnTxIntr = false;
		spin_unlock_irqrestore(&hw->lock, flags);

		/* Replay firmware notifications while the selector still owns
		 * the transition; RX doorbell admission happens after release.
		 */
		if (replay_pic)
			crystalhd_flea_handle_PicQSts_intr(hw);
		if (replay_tx)
			crystalhd_flea_update_tx_buff_info(hw);
		return BC_STS_SUCCESS;

	case FLEA_PS_LP_PENDING:
	case FLEA_PS_LP_COMPLETE:
		if (hw->FleaPowerState == FLEA_PS_LP_COMPLETE)
			break;
		if (hw->FleaPowerState != FLEA_PS_ACTIVE &&
		    hw->FleaPowerState != FLEA_PS_LP_PENDING) {
			sts = BC_STS_ERR_USAGE;
			break;
		}
		if (hw->pfnDevDRAMWrite(hw, hw->FleaFLLUpdateAddr, 1,
				      &tempFLL) != BC_STS_SUCCESS) {
			sts = BC_STS_IO_ERROR;
			break;
		}
		hw->FleaPowerState = FLEA_PS_LP_PENDING;
		if (NewState == FLEA_PS_LP_PENDING)
			break;
		/* Recheck owners under their admission lock. A newly busy owner
		 * defers optional power-down, but the pause request was accepted.
		 * These host checks do not prove physical source quiescence.
		 */
		if (hw->FleaTxActive || hw->FleaIrqActive || hw->FleaRawActive ||
		    hw->fwcmd_trans_active || hw->fwcmd_active || hw->FwCmdCnt ||
		    hw->fwcmd_poisoned ||
		    hw->TxList0Sts != ListStsFree || hw->TxList1Sts != ListStsFree ||
		    hw->rx_list_sts[0] != sts_free || hw->rx_list_sts[1] != sts_free ||
		    hw->EmptyCnt || hw->SingleThreadAppFIFOEmpty)
			break;
		/* A refusal preserves zero-FLL; an unconfirmed recovery is terminal. */
		down = crystalhd_flea_runtime_power_dn(hw);
		if (down == FLEA_PD_ENTERED)
			hw->FleaPowerState = FLEA_PS_LP_COMPLETE;
		else if (down == FLEA_PD_FAULT)
			sts = BC_STS_IO_ERROR;
		break;
	default:
		sts = BC_STS_ERR_USAGE;
		break;
	}
unlock:
	spin_unlock_irqrestore(&hw->lock, flags);
	return sts;
}

/* Choose and reserve one directional request atomically, including no-ops. */
static
BC_STATUS crystalhd_flea_set_next_power_state(struct crystalhd_hw *hw,
					      enum FLEA_STATE_CH_EVENT PowerEvt)
{
	enum FLEA_POWER_STATES next;
	struct crystalhd_rx_dma_pkt *rx_pkt;
	BC_STATUS sts = BC_STS_SUCCESS;
	BC_STATUS request_sts = BC_STS_SUCCESS;
	unsigned long flags;
	bool repost, recheck, followup = false;

	if (READ_ONCE(hw->FleaPowerState) == FLEA_PS_FAULT ||
	    READ_ONCE(hw->dma_fault))
		return BC_STS_IO_ERROR;
	if (!READ_ONCE(hw->FleaEnablePWM)) {
		/* Preserve the supported, runtime-PWM-disabled path. */
		hw->FleaPowerState = FLEA_PS_ACTIVE;
		return BC_STS_SUCCESS;
	}

retry:
	repost = false;
	recheck = false;
	sts = BC_STS_SUCCESS;
	spin_lock_irqsave(&hw->lock, flags);
	if (hw->FleaPowerState == FLEA_PS_FAULT || READ_ONCE(hw->dma_fault)) {
		sts = BC_STS_IO_ERROR;
		goto unlock;
	}
	if (hw->FleaPowerTransition) {
		/* The last completion may retire between the setter's final
		 * owner check and this selector's reservation release.
		 */
		if (PowerEvt == FLEA_EVT_CMD_COMP &&
		    (hw->FleaPowerState == FLEA_PS_LP_PENDING ||
		     hw->hw_pause_issued))
			hw->FleaPowerRecheck = true;
		sts = BC_STS_BUSY;
		goto unlock;
	}
	if (PowerEvt == FLEA_EVT_STOP_DEVICE) {
		hw->FleaPowerState = FLEA_PS_STOPPED;
		goto unlock;
	}
	if (PowerEvt == FLEA_EVT_START_DEVICE) {
		hw->FleaPowerState = FLEA_PS_ACTIVE;
		goto unlock;
	}
	if (!READ_ONCE(hw->adp->present) || READ_ONCE(hw->dma_fault) ||
	    !READ_ONCE(hw->dev_started)) {
		sts = BC_STS_IO_ERROR;
		goto unlock;
	}
	if (hw->FleaPowerState != FLEA_PS_ACTIVE &&
	    hw->FleaPowerState != FLEA_PS_LP_PENDING &&
	    hw->FleaPowerState != FLEA_PS_LP_COMPLETE) {
		sts = BC_STS_ERR_USAGE;
		goto unlock;
	}

	next = hw->FleaPowerState;
	switch (PowerEvt) {
	case FLEA_EVT_RESUME:
	case FLEA_EVT_RX_RESUME:
	case FLEA_EVT_FW_CMD_POST:
		next = FLEA_PS_ACTIVE;
		break;
	case FLEA_EVT_PAUSE:
	case FLEA_EVT_RX_PAUSE:
	case FLEA_EVT_CMD_COMP:
		if (hw->FleaPowerState == FLEA_PS_LP_COMPLETE ||
		    (PowerEvt == FLEA_EVT_CMD_COMP &&
		     hw->FleaPowerState != FLEA_PS_LP_PENDING &&
		     !hw->hw_pause_issued))
			break;
		/* Do not revoke a temporary wake before its command or TX
		 * producer reaches the doorbell. That owner retries on release.
		 */
		if (PowerEvt == FLEA_EVT_CMD_COMP &&
		    hw->FleaPowerState == FLEA_PS_ACTIVE &&
		    (hw->fwcmd_trans_active || hw->fwcmd_active || hw->FwCmdCnt ||
		     hw->FleaTxActive))
			break;
		/* A firmware command may have woken an RX-paused device.
		 * Recheck current intent here, not a caller's stale observation.
		 */
		next = FLEA_PS_LP_PENDING;
		if (hw->TxList0Sts == ListStsFree && hw->TxList1Sts == ListStsFree &&
		    !hw->EmptyCnt && !hw->SingleThreadAppFIFOEmpty &&
		    hw->rx_list_sts[0] == sts_free && hw->rx_list_sts[1] == sts_free &&
		    !hw->FleaTxActive && !hw->FleaIrqActive && !hw->FleaRawActive &&
		    !hw->FwCmdCnt && !hw->fwcmd_active && !hw->fwcmd_trans_active &&
		    !hw->fwcmd_poisoned)
			next = FLEA_PS_LP_COMPLETE;
		break;
	default:
		/* FLL_CHANGE no longer carries an ambiguous pause/resume request. */
		break;
	}
	if (next == hw->FleaPowerState) {
		if (PowerEvt == FLEA_EVT_RX_PAUSE || PowerEvt == FLEA_EVT_RX_RESUME)
			hw->hw_pause_issued = PowerEvt == FLEA_EVT_RX_PAUSE;
		goto unlock;
	}
	hw->FleaPowerTransition = true;
	spin_unlock_irqrestore(&hw->lock, flags);

	sts = crystalhd_flea_set_power_state(hw, next);

	spin_lock_irqsave(&hw->lock, flags);
	if (sts == BC_STS_SUCCESS &&
	    (PowerEvt == FLEA_EVT_RX_PAUSE || PowerEvt == FLEA_EVT_RX_RESUME))
		hw->hw_pause_issued = PowerEvt == FLEA_EVT_RX_PAUSE;
	hw->FleaPowerTransition = false;
	/* An owner may also have retired while the selected PENDING state
	 * was not yet published, when its completion had nothing to retry.
	 */
	if (sts == BC_STS_SUCCESS) {
		recheck = (hw->FleaPowerState == FLEA_PS_LP_PENDING ||
			   (hw->FleaPowerState == FLEA_PS_ACTIVE && hw->hw_pause_issued)) &&
			(hw->FleaPowerRecheck || next == FLEA_PS_LP_PENDING);
		hw->FleaPowerRecheck = false;
	}
	repost = sts == BC_STS_SUCCESS && next == FLEA_PS_ACTIVE;
unlock:
	spin_unlock_irqrestore(&hw->lock, flags);
	if (!followup)
		request_sts = sts;
	/* Reposting takes rx_lock before hw->lock. A competing transition
	 * after release is checked again at the actual RX doorbell; BUSY
	 * retains the packet on the free queue for a later wake.
	 */
	if (repost && READ_ONCE(hw->PicQSts)) {
		rx_pkt = crystalhd_hw_fetch_free_rx_pkt(hw);
		if (rx_pkt) {
			BC_STATUS rx_sts = crystalhd_hw_repost_cap_buffer(hw, rx_pkt);

			if (rx_sts != BC_STS_SUCCESS && rx_sts != BC_STS_BUSY)
				dev_err(chddev(), "failed to repost RX buffer\n");
		}
	}
	if (recheck) {
		/* Completion replay is advisory; it cannot revoke an accepted
		 * pause/resume request. A still-busy owner yields a no-op.
		 */
		followup = true;
		PowerEvt = FLEA_EVT_CMD_COMP;
		goto retry;
	}
	return request_sts;
}

/* External raw access is serialized with optional runtime power-down. */
BC_STATUS crystalhd_flea_raw_enter(struct crystalhd_hw *hw, bool write, bool *owned)
{
	unsigned long flags;
	BC_STATUS sts = BC_STS_SUCCESS;

	*owned = false;
	spin_lock_irqsave(&hw->lock, flags);
	if (READ_ONCE(hw->dma_fault) || hw->FleaPowerState == FLEA_PS_FAULT)
		sts = BC_STS_IO_ERROR;
	else if (hw->FleaEnablePWM) {
		if (!READ_ONCE(hw->adp->present) || !READ_ONCE(hw->dev_started))
			sts = BC_STS_IO_ERROR;
		else if (write || hw->FleaPowerTransition ||
			 hw->FleaPowerState != FLEA_PS_ACTIVE || hw->FleaRawActive == ~0U)
			sts = BC_STS_BUSY;
		else {
			hw->FleaRawActive++;
			*owned = true;
		}
	}
	spin_unlock_irqrestore(&hw->lock, flags);
	return sts;
}

void crystalhd_flea_raw_leave(struct crystalhd_hw *hw, bool owned)
{
	unsigned long flags;
	bool recheck;

	if (!owned)
		return;
	spin_lock_irqsave(&hw->lock, flags);
	hw->FleaRawActive--;
	recheck = !READ_ONCE(hw->dma_fault) &&
		  (hw->FleaPowerState == FLEA_PS_LP_PENDING || hw->hw_pause_issued);
	spin_unlock_irqrestore(&hw->lock, flags);
	if (recheck)
		crystalhd_flea_set_next_power_state(hw, FLEA_EVT_CMD_COMP);
}

/* Called with the outer transaction mutex held, after its last state update. */
void crystalhd_flea_fw_transaction_done(struct crystalhd_hw *hw)
{
	if (hw->adp->pdev->device != BC_PCI_DEVID_FLEA ||
	    !READ_ONCE(hw->FleaEnablePWM) || !READ_ONCE(hw->adp->present) ||
	    READ_ONCE(hw->dma_fault) || !READ_ONCE(hw->dev_started))
		return;
	if (READ_ONCE(hw->FleaPowerState) == FLEA_PS_LP_PENDING ||
	    READ_ONCE(hw->hw_pause_issued))
		crystalhd_flea_set_next_power_state(hw, FLEA_EVT_CMD_COMP);
}

/* was FleaSetRxPicFireAddr */
#if 0
static
void crystalhd_flea_set_rx_pic_fire_addr(struct crystalhd_hw *hw, uint32_t BorshContents)
{
	hw->FleaRxPicDelAddr = BorshContents + 1 + HOST_TO_FW_PIC_DEL_INFO_ADDR;
	hw->FleaFLLUpdateAddr = BorshContents + 1 + HOST_TO_FW_FLL_ADDR;

	return;
}
#endif

void crystalhd_flea_init_temperature_measure (struct crystalhd_hw *hw, bool bTurnOn)
{
	hw->TemperatureRegVal=0;

	if(bTurnOn) {
		hw->pfnWriteDevRegister(hw->adp, BCHP_CLK_TEMP_MON_CTRL, 0x3);
		hw->pfnWriteDevRegister(hw->adp, BCHP_CLK_TEMP_MON_CTRL, 0x203);
	} else {
		hw->pfnWriteDevRegister(hw->adp, BCHP_CLK_TEMP_MON_CTRL, 0x103);
	}

	return;
}

/* was HwFleaUpdateTempInfo */
void crystalhd_flea_update_temperature(struct crystalhd_hw *hw)
{
	uint32_t	regVal = 0;

	regVal = hw->pfnReadDevRegister(hw->adp, BCHP_CLK_TEMP_MON_STATUS);
	hw->TemperatureRegVal = regVal;

	return;
}

/**
* crystalhd_flea_download_fw - Write data to DRAM area.
* @hw: Hardware context.
* @pBuffer: Buffer pointer for the FW data.
* @buffSz: data size in bytes.
*
* Return:
*	Status.
*
* Flea firmware download routine.
*/
BC_STATUS crystalhd_flea_download_fw(struct crystalhd_hw *hw,
				     const uint8_t *pBuffer, uint32_t buffSz)
{
	uint32_t pollCnt=0,regVal=0;
	uint32_t borchStachAddr=0;
	const uint32_t *pCmacSig=NULL;
	uint32_t cmacOffset=0,i=0;
	BC_STATUS sts;
	/*uint32_t BuffSz = (BuffSzInDWords * 4); */
	/*uint32_t HBCnt=0; */

	bool bRetVal = true;
	bool bSecure = true; // Default production cards. Can be false only for internal Broadcom dev cards

/*
 *-- Step 1. Enable the SRCUBBING and DRAM SCRAMBLING
 *-- Step 2. Poll for SCRAM_KEY_DONE_INT.
 *-- Step 3. Write the BORCH and STARCH addresses.
 *-- Step 4. Write the firmware to DRAM.
 *-- Step 5. Write the CMAC to SCRUB->CMAC registers.
 *-- Step 6. Write the ARM run bit to 1.
 *-- Step 7. Poll for BOOT verification done interrupt.
 */

	/* Validate the complete payload/length/signature layout before MMIO or
	 * any subtraction based on the untrusted image size.
	 */
	if (!hw || !hw->adp || !hw->adp->pdev ||
	    !hw->pfnReadDevRegister || !hw->pfnWriteDevRegister ||
	    !hw->pfnDevDRAMWrite ||
	    !crystalhd_valid_firmware_image(pBuffer, buffSz,
					       CRYSTALHD_FLEA_MIN_FIRMWARE_SIZE))
		return BC_STS_INV_ARG;

	dev_dbg(&hw->adp->pdev->dev, "[%s]: Sz:%d\n", __func__, buffSz);

/*-- Step 1. Enable the SRCUBBING and DRAM SCRAMBLING. */
/*   Can we set both the bits at the same time?? Security Arch Doc describes the steps */
/*   and the first step is to enable scrubbing and then scrambling. */

	if(bSecure)
	{
		dev_dbg(&hw->adp->pdev->dev,"[crystalhd_flea_download_fw]: step 1. Enable scrubbing\n");

		/* Enable Scrubbing */
		regVal = hw->pfnReadDevRegister(hw->adp, BCHP_SCRUB_CTRL_SCRUB_ENABLE);
		regVal |= SCRUB_ENABLE_BIT;
		hw->pfnWriteDevRegister(hw->adp, BCHP_SCRUB_CTRL_SCRUB_ENABLE, regVal);

		/* Enable Scrambling */
		regVal |= DRAM_SCRAM_ENABLE_BIT;
		hw->pfnWriteDevRegister(hw->adp, BCHP_SCRUB_CTRL_SCRUB_ENABLE, regVal);


		//-- Step 2. Poll for SCRAM_KEY_DONE_INT.
		dev_dbg(&hw->adp->pdev->dev,"[crystalhd_flea_download_fw]: step 2. Poll for SCRAM_KEY_DONE_INT\n");

		pollCnt=0;
		while(pollCnt < FLEA_MAX_POLL_CNT)
		{
			regVal = hw->pfnReadDevRegister(hw->adp, BCHP_WRAP_MISC_INTR2_PCI_STATUS);

			if(regVal & SCRAM_KEY_DONE_INT_BIT)
				break;

			pollCnt++;
			msleep_interruptible(1); /*1 Milli Sec delay*/
		}

		/* -- Will Assert when we do not see SCRAM_KEY_DONE_INTERRUPT */
		if(!(regVal & SCRAM_KEY_DONE_INT_BIT))
		{
			dev_err(&hw->adp->pdev->dev,"[crystalhd_flea_download_fw]: step 2. Did not get scram key done interrupt.\n");
			return BC_STS_ERROR;
		}
	}

	/*Clear the interrupts by writing the register value back*/
	regVal &= 0x00FFFFFF; /*Mask off the reserved bits.[24-31] */
	hw->pfnWriteDevRegister(hw->adp, BCHP_WRAP_MISC_INTR2_PCI_CLEAR, regVal);

/*-- Step 3. Write the BORCH and STARCH addresses. */
	borchStachAddr = GetScrubEndAddr(buffSz);
	if(!bSecure)
		borchStachAddr = (buffSz - 1) & BCHP_SCRUB_CTRL_BORCH_END_ADDRESS_BORCH_END_ADDR_MASK;

	hw->pfnWriteDevRegister(hw->adp, BCHP_SCRUB_CTRL_BORCH_END_ADDRESS, borchStachAddr);
	hw->pfnWriteDevRegister(hw->adp, BCHP_SCRUB_CTRL_STARCH_END_ADDRESS, borchStachAddr);

	/*
	 *	Now the command address is
	 *	relative to firmware file size.
	 */
	/*FWIFSetFleaCmdAddr(pHWExt->pFwExt, */
	/*		borchStachAddr+1+DDRADDR_4_FWCMDS); */

	hw->fwcmdPostAddr = borchStachAddr+1+DDRADDR_4_FWCMDS;
	hw->fwcmdPostMbox = FW_CMD_POST_MBOX;
	hw->fwcmdRespMbox = FW_CMD_RES_MBOX;
	/*FleaSetRxPicFireAddr(pHWExt,borchStachAddr); */
	hw->FleaRxPicDelAddr = borchStachAddr + 1 + HOST_TO_FW_PIC_DEL_INFO_ADDR;
	hw->FleaFLLUpdateAddr = borchStachAddr + 1 + HOST_TO_FW_FLL_ADDR;

	dev_dbg(&hw->adp->pdev->dev,"[crystalhd_flea_download_fw]: step 3. Write the BORCH and STARCH addresses. %x:%x, %x:%x\n",
			BCHP_SCRUB_CTRL_BORCH_END_ADDRESS,
			borchStachAddr,
			BCHP_SCRUB_CTRL_STARCH_END_ADDRESS,
			borchStachAddr );

/*-- Step 4. Write the firmware to DRAM. [Without the Signature, 32-bit access to DRAM] */

	dev_dbg(&hw->adp->pdev->dev,"[crystalhd_flea_download_fw]: step 4. Write the firmware to DRAM. Sz:%d Bytes\n",
			buffSz - FLEA_FW_SIG_LEN_IN_BYTES - LENGTH_FIELD_SIZE);

	if(bSecure)
		sts = hw->pfnDevDRAMWrite(hw, FW_DOWNLOAD_START_ADDR,
			(buffSz - FLEA_FW_SIG_LEN_IN_BYTES -
			 LENGTH_FIELD_SIZE) / 4, (const uint32_t *)pBuffer);
	else
		sts = hw->pfnDevDRAMWrite(hw, FW_DOWNLOAD_START_ADDR,
			buffSz / 4, (const uint32_t *)pBuffer);
	if (sts != BC_STS_SUCCESS) {
		dev_err(&hw->adp->pdev->dev,
			"[%s]: firmware DRAM write failed: %d\n", __func__, sts);
		return sts;
	}

/* -- Step 5. Write the signature to CMAC register. */
/*
-- This is what we need to write to CMAC registers.
==================================================================================
Register							Offset				Boot Image CMAC
														Value
==================================================================================
BCHP_SCRUB_CTRL_BI_CMAC_31_0		0x000f600c			CMAC Bits[31:0]
BCHP_SCRUB_CTRL_BI_CMAC_63_32		0x000f6010			CMAC Bits[63:32]
BCHP_SCRUB_CTRL_BI_CMAC_95_64		0x000f6014			CMAC Bits[95:64]
BCHP_SCRUB_CTRL_BI_CMAC_127_96		0x000f6018			CMAC Bits[127:96]
==================================================================================
*/

	if(bSecure)
	{
		dev_dbg(&hw->adp->pdev->dev,"[crystalhd_flea_download_fw]: step 5. Write the signature to CMAC register.\n");
		cmacOffset = buffSz - FLEA_FW_SIG_LEN_IN_BYTES;
		pCmacSig = (const uint32_t *)&pBuffer[cmacOffset];

		for(i=0;i < FLEA_FW_SIG_LEN_IN_DWORD;i++)
		{
			uint32_t offSet = (BCHP_SCRUB_CTRL_BI_CMAC_127_96 - (i * 4));

			hw->pfnWriteDevRegister(hw->adp, offSet, cpu_to_be32(*pCmacSig));

			pCmacSig++;
		}
	}

/*-- Step 6. Write the ARM run bit to 1. */
/*   We need a write back because we do not want to change other bits */
	dev_dbg(&hw->adp->pdev->dev,"[crystalhd_flea_download_fw]: step 6. Write the ARM run bit to 1.\n");

	regVal = hw->pfnReadDevRegister(hw->adp, BCHP_ARMCR4_BRIDGE_REG_BRIDGE_CTL);
	regVal |= ARM_RUN_REQ_BIT;
	hw->pfnWriteDevRegister(hw->adp, BCHP_ARMCR4_BRIDGE_REG_BRIDGE_CTL, regVal);

	if(bSecure)
	{
		/* -- Step 7. Poll for Boot Verification done/failure interrupt.*/
		dev_dbg(&hw->adp->pdev->dev,"[crystalhd_flea_download_fw]: step 7. Poll for Boot Verification done/failure interrupt.\n");
		pollCnt=0;
		while(1)
		{
			regVal = hw->pfnReadDevRegister(hw->adp, BCHP_WRAP_MISC_INTR2_PCI_STATUS);

			if(regVal & BOOT_VER_FAIL_BIT )
			{
				dev_err(&hw->adp->pdev->dev,"[crystalhd_flea_download_fw]: step 7. Error bit occured. RetVal:%x\n", regVal);

				bRetVal = false;
				break;
			}

			if(regVal & BOOT_VER_DONE_BIT)
			{
				dev_dbg(&hw->adp->pdev->dev,"[crystalhd_flea_download_fw]: step 7. Done  RetVal:%x\n", regVal);

				bRetVal = true; /*This is the only place we return TRUE from*/
				break;
			}

			pollCnt++;
			if( pollCnt >= FLEA_MAX_POLL_CNT )
			{
				dev_err(&hw->adp->pdev->dev,"[crystalhd_flea_download_fw]: step 7. Both done and failure bits are not set.\n");
				bRetVal = false;
				break;
			}

			msleep_interruptible(5); /*5 Milli Sec delay*/
		}

		if( !bRetVal )
		{
			dev_info(&hw->adp->pdev->dev,"[crystalhd_flea_download_fw]: step 7. Firmware image signature failure.\n");
			return BC_STS_ERROR;
		}

		/*Clear the interrupts by writing the register value back*/
		regVal &= 0x00FFFFFF; //Mask off the reserved bits.[24-31]
		hw->pfnWriteDevRegister(hw->adp, BCHP_WRAP_MISC_INTR2_PCI_CLEAR, regVal);

		msleep_interruptible(10); /*10 Milli Sec delay*/
	}
	else
		bRetVal = true;

	/*
	   -- It was seen on Dell390 systems that the firmware command was fired before the
	   -- firmware was actually ready to accept the firmware commands. The driver did
	   -- not recieve a response for the firmware commands and this was causing the DIL to timeout
	   -- ,reclaim the resources and crash. The following code looks for the heartbeat and
	   -- to make sure that we return from this function only when we get the heart beat making sure
	   -- that the firmware is running.
	   */

	bRetVal = crystalhd_flea_detect_fw_alive(hw);
	if( !bRetVal )
	{
		dev_info(&hw->adp->pdev->dev,"[crystalhd_flea_download_fw]: step 8. Detect firmware heart beat failed.\n");
		return BC_STS_ERROR;
	}

	dev_dbg(&hw->adp->pdev->dev, "[%s]: Complete.\n", __func__);
	return BC_STS_SUCCESS;
}

bool crystalhd_flea_start_device(struct crystalhd_hw *hw)
{
	uint32_t	regVal	= 0;

	/*
	-- Issue Core reset to bring in the default values in place
	*/
	if (!crystalhd_flea_core_reset(hw))
		return false;

	/*
	-- If the gisb arbitar register is not set to some other value
	-- and the firmware crashes, we see a NMI since the hardware did
	-- not respond to a register read at all. The PCI-E trace confirms the problem.
	-- Right now we are setting the register values to 0x7e00 and will check later
	-- what should be the correct value to program.
	*/
	hw->pfnWriteDevRegister(hw->adp, BCHP_SUN_GISB_ARB_TIMER, 0xd80);

	/*
	-- Disable all interrupts
	*/
	crystalhd_flea_clear_interrupts(hw);
	crystalhd_flea_disable_interrupts(hw);

	/*
	-- Enable the option for getting the done count in
	-- Rx DMA engine.
	*/
	regVal = hw->pfnReadDevRegister(hw->adp, BCHP_MISC1_DMA_DEBUG_OPTIONS_REG);
	regVal |= 0x10;
	hw->pfnWriteDevRegister(hw->adp, BCHP_MISC1_DMA_DEBUG_OPTIONS_REG, regVal);

	/*
	-- Enable the TX DMA Engine once on startup.
	-- This is a new bit added.
	*/
	hw->pfnWriteDevRegister(hw->adp, BCHP_MISC1_TX_DMA_CTRL, 0x01);

	/*
	-- Enable the RX3 DMA Engine once on startup.
	-- This is a new bit added.
	*/
	hw->pfnWriteDevRegister(hw->adp, BCHP_MISC1_HIF_DMA_CTRL, 0x01);

	/*
	-- Set the Run bit for RX-Y and RX-UV DMA engines.
	*/
	hw->pfnWriteDevRegister(hw->adp, BCHP_MISC1_Y_RX_SW_DESC_LIST_CTRL_STS, 0x01);

	/*
	-- Make sure Early L1 is disabled - NAREN - This will not prevent the device from entering L1 under active mode
	*/
	regVal = hw->pfnReadDevRegister(hw->adp, BCHP_MISC_PERST_CLOCK_CTRL);
	regVal &= ~BCHP_MISC_PERST_CLOCK_CTRL_EARLY_L1_EXIT_MASK;
	hw->pfnWriteDevRegister(hw->adp, BCHP_MISC_PERST_CLOCK_CTRL, regVal);

	if (!crystalhd_flea_init_dram(hw)) {
		/* Undo the startup enables; this is not proof of DMA quiescence. */
		hw->pfnWriteDevRegister(hw->adp, BCHP_MISC1_TX_DMA_CTRL, 0);
		hw->pfnWriteDevRegister(hw->adp, BCHP_MISC1_HIF_DMA_CTRL, 0);
		hw->pfnWriteDevRegister(hw->adp, BCHP_MISC1_Y_RX_SW_DESC_LIST_CTRL_STS, 0);
		return false;
	}

	msleep_interruptible(5);
	/* Enable the Single Shot Transaction on PCI by disabling the */
	/* bit 29 of transaction configuration register */
	regVal = hw->pfnReadDevRegister(hw->adp, BCHP_PCIE_TL_TRANSACTION_CONFIGURATION);
	regVal &= (~(BC_BIT(29)));
	hw->pfnWriteDevRegister(hw->adp, BCHP_PCIE_TL_TRANSACTION_CONFIGURATION, regVal);
	crystalhd_flea_init_temperature_measure(hw,true);

	crystalhd_flea_init_power_state(hw);
	if (crystalhd_flea_set_next_power_state(hw, FLEA_EVT_START_DEVICE) !=
	    BC_STS_SUCCESS)
		return false;

	/*
	-- Enable all interrupts
	*/
	crystalhd_flea_clear_interrupts(hw);
	crystalhd_flea_enable_interrupts(hw);

	/*
	-- This is the only time we set this pointer for Flea.
	-- Since there is no stop the pointer is not reset anytime....
	-- except for fatal errors.
	*/
	hw->rx_list_post_index = 0;
	hw->RxCaptureState = 0;
	msleep_interruptible(1);
	return true;
}

bool crystalhd_flea_stop_device(struct crystalhd_hw *hw)
{
	uint32_t regVal=0, pollCnt=0;

	/*
	-- Issue the core reset so that we
	-- make sure there is nothing running.
	*/
	if (!crystalhd_flea_core_reset(hw))
		return false;

	crystalhd_flea_init_temperature_measure(hw, false);

	/*
	-- If the gisb arbitrater register is not set to some other value
	-- and the firmware crashes, we see a NMI since the hardware did
	-- not respond to a register read at all. The PCI-E trace confirms the problem.
	-- Right now we are setting the register values to 0x7e00 and will check later
	-- what should be the correct value to program.
	*/
	hw->pfnWriteDevRegister(hw->adp, BCHP_SUN_GISB_ARB_TIMER, 0xd80);

	/*
	-- Disable the TX DMA Engine once on shutdown.
	-- This is a new bit added.
	*/
	hw->pfnWriteDevRegister(hw->adp, BCHP_MISC1_TX_DMA_CTRL, 0x0);

	/*
	-- Disable the RX3 DMA Engine once on Stop.
	-- This is a new bit added.
	*/
	hw->pfnWriteDevRegister(hw->adp, BCHP_MISC1_HIF_DMA_CTRL, 0x0);

	/*
	-- Clear the RunStop Bit For RX DMA Control
	*/
	hw->pfnWriteDevRegister(hw->adp, BCHP_MISC1_Y_RX_SW_DESC_LIST_CTRL_STS, 0x0);

	hw->pfnWriteDevRegister(hw->adp, BCHP_PRI_ARB_CONTROL_REGS_REFRESH_CTL_0, 0x0);

	/* * Wait for MEMC to become idle */
	pollCnt=0;
	while (1)
	{
		regVal = hw->pfnReadDevRegister(hw->adp, BCHP_DDR23_CTL_REGS_0_CTL_STATUS);

		if(regVal & BCHP_DDR23_CTL_REGS_0_CTL_STATUS_idle_MASK)
			break;

		pollCnt++;
		if(pollCnt >= 100)
			break;

		msleep_interruptible(1);
	}

	if (!(regVal & BCHP_DDR23_CTL_REGS_0_CTL_STATUS_idle_MASK)) {
		dev_err(&hw->adp->pdev->dev, "MEMC did not become idle during shutdown\n");
		return false;
	}

	/*First Disable the AVD and ARM before disabling the DRAM*/
	regVal = hw->pfnReadDevRegister(hw->adp, BCHP_CLK_PM_CTRL);

	regVal = BCHP_CLK_PM_CTRL_DIS_ARM_CLK_MASK |
			 BCHP_CLK_PM_CTRL_DIS_AVD_CLK_MASK |
			 BCHP_CLK_PM_CTRL_DIS_AVD_108_CLK_MASK |
			 BCHP_CLK_PM_CTRL_DIS_AVD_216_CLK_MASK;

	hw->pfnWriteDevRegister(hw->adp, BCHP_CLK_PM_CTRL, regVal);

	/*
	-- Disable the interrupt after disabling the ARM and AVD.
	-- We should be able to access the registers because we still
	-- have not disabled the clock for blink block. We disable the
	-- blick 108 abd 216 clock at the end of this function.
	*/
	crystalhd_flea_clear_interrupts(hw);
	crystalhd_flea_disable_interrupts(hw);

	/*Now try disabling the DRAM.*/
	regVal = hw->pfnReadDevRegister(hw->adp, BCHP_DDR23_CTL_REGS_0_PARAMS2);

	regVal |= BCHP_DDR23_CTL_REGS_0_PARAMS2_clke_MASK;

	/* * disable CKE */
	hw->pfnWriteDevRegister(hw->adp, BCHP_DDR23_CTL_REGS_0_PARAMS2, regVal);

	/* * issue refresh command */
	hw->pfnWriteDevRegister(hw->adp, BCHP_DDR23_CTL_REGS_0_REFRESH_CMD, 0x60);

	pollCnt=0;
	while(1)
	{
		regVal = hw->pfnReadDevRegister(hw->adp, BCHP_DDR23_CTL_REGS_0_CTL_STATUS);

		if(!(regVal & BCHP_DDR23_CTL_REGS_0_CTL_STATUS_clke_MASK))
			break;

		pollCnt++;
		if(pollCnt >= 100)
			break;

		msleep_interruptible(1);
	}

	/* * Enable DDR clock, DM and READ_ENABLE pads power down and force into the power down */
	hw->pfnWriteDevRegister(hw->adp, BCHP_DDR23_PHY_BYTE_LANE_0_IDLE_PAD_CONTROL,
							BCHP_DDR23_PHY_BYTE_LANE_0_IDLE_PAD_CONTROL_idle_MASK |
							BCHP_DDR23_PHY_BYTE_LANE_0_IDLE_PAD_CONTROL_dm_iddq_MASK |
							BCHP_DDR23_PHY_BYTE_LANE_0_IDLE_PAD_CONTROL_dq_iddq_MASK |
							BCHP_DDR23_PHY_BYTE_LANE_0_IDLE_PAD_CONTROL_read_enb_iddq_MASK |
							BCHP_DDR23_PHY_BYTE_LANE_0_IDLE_PAD_CONTROL_dqs_iddq_MASK |
							BCHP_DDR23_PHY_BYTE_LANE_0_IDLE_PAD_CONTROL_clk_iddq_MASK);

	hw->pfnWriteDevRegister(hw->adp, BCHP_DDR23_PHY_BYTE_LANE_1_IDLE_PAD_CONTROL,
							BCHP_DDR23_PHY_BYTE_LANE_1_IDLE_PAD_CONTROL_idle_MASK |
							BCHP_DDR23_PHY_BYTE_LANE_1_IDLE_PAD_CONTROL_dm_iddq_MASK |
							BCHP_DDR23_PHY_BYTE_LANE_1_IDLE_PAD_CONTROL_dq_iddq_MASK |
							BCHP_DDR23_PHY_BYTE_LANE_1_IDLE_PAD_CONTROL_read_enb_iddq_MASK |
							BCHP_DDR23_PHY_BYTE_LANE_1_IDLE_PAD_CONTROL_dqs_iddq_MASK |
							BCHP_DDR23_PHY_BYTE_LANE_1_IDLE_PAD_CONTROL_clk_iddq_MASK);

	/* * Power down BL LDO cells */
	hw->pfnWriteDevRegister(hw->adp, BCHP_DDR23_PHY_BYTE_LANE_0_CLOCK_REG_CONTROL,
							BCHP_DDR23_PHY_BYTE_LANE_0_CLOCK_REG_CONTROL_pwrdn_MASK);

	hw->pfnWriteDevRegister(hw->adp, BCHP_DDR23_PHY_BYTE_LANE_1_CLOCK_REG_CONTROL,
							BCHP_DDR23_PHY_BYTE_LANE_1_CLOCK_REG_CONTROL_pwrdn_MASK);

	/* * Enable DDR control signal pad power down and force into the power down */
	hw->pfnWriteDevRegister(hw->adp, BCHP_DDR23_PHY_CONTROL_REGS_IDLE_PAD_CONTROL,
							BCHP_DDR23_PHY_CONTROL_REGS_IDLE_PAD_CONTROL_idle_MASK |
							BCHP_DDR23_PHY_CONTROL_REGS_IDLE_PAD_CONTROL_ctl_iddq_MASK);

	/* * Disable ddr phy clock */
	hw->pfnWriteDevRegister(hw->adp, BCHP_DDR23_PHY_CONTROL_REGS_CLK_PM_CTRL,
							BCHP_DDR23_PHY_CONTROL_REGS_CLK_PM_CTRL_DIS_DDR_CLK_MASK);

	/* * Disable PLL output */
	regVal = hw->pfnReadDevRegister(hw->adp, BCHP_DDR23_PHY_CONTROL_REGS_PLL_CONFIG);

	hw->pfnWriteDevRegister(hw->adp, BCHP_DDR23_PHY_CONTROL_REGS_PLL_CONFIG,
							regVal & ~BCHP_DDR23_PHY_CONTROL_REGS_PLL_CONFIG_ENB_CLKOUT_MASK);

	/* * Power down addr_ctl LDO cells */
	hw->pfnWriteDevRegister(hw->adp, BCHP_DDR23_PHY_CONTROL_REGS_CLOCK_REG_CONTROL,
							BCHP_DDR23_PHY_CONTROL_REGS_CLOCK_REG_CONTROL_pwrdn_MASK);

	/* * Power down the PLL */
	regVal = hw->pfnReadDevRegister(hw->adp, BCHP_DDR23_PHY_CONTROL_REGS_PLL_CONFIG);

	hw->pfnWriteDevRegister(hw->adp, BCHP_DDR23_PHY_CONTROL_REGS_PLL_CONFIG,
							regVal | BCHP_DDR23_PHY_CONTROL_REGS_PLL_CONFIG_PWRDN_MASK);

	/* shut down the PLL1 */
	regVal = hw->pfnReadDevRegister(hw->adp, BCHP_CLK_PLL1_CTRL);

	hw->pfnWriteDevRegister(hw->adp, BCHP_CLK_PLL1_CTRL,
							regVal | BCHP_CLK_PLL1_CTRL_POWERDOWN_MASK);

	hw->pfnWriteDevRegister(hw->adp, BCHP_CLK_PLL0_ARM_DIV,	0xff);

	regVal = hw->pfnReadDevRegister(hw->adp, BCHP_CLK_PM_CTRL);

	regVal |= BCHP_CLK_PM_CTRL_DIS_SUN_27_LOW_PWR_MASK |
			  BCHP_CLK_PM_CTRL_DIS_SUN_108_LOW_PWR_MASK |
			  BCHP_CLK_PM_CTRL_DIS_MISC_OTP_9_CLK_MASK |
			  BCHP_CLK_PM_CTRL_DIS_ARM_CLK_MASK |
			  BCHP_CLK_PM_CTRL_DIS_AVD_CLK_MASK |
			  BCHP_CLK_PM_CTRL_DIS_AVD_108_CLK_MASK |
			  BCHP_CLK_PM_CTRL_DIS_BLINK_108_CLK_MASK |
			  BCHP_CLK_PM_CTRL_DIS_MISC_108_CLK_MASK |
			  BCHP_CLK_PM_CTRL_DIS_BLINK_216_CLK_MASK |
			  BCHP_CLK_PM_CTRL_DIS_DDR_108_CLK_MASK |
			  BCHP_CLK_PM_CTRL_DIS_DDR_216_CLK_MASK |
			  BCHP_CLK_PM_CTRL_DIS_AVD_216_CLK_MASK |
			  BCHP_CLK_PM_CTRL_DIS_MISC_216_CLK_MASK |
			  BCHP_CLK_PM_CTRL_DIS_SUN_216_CLK_MASK;

	hw->pfnWriteDevRegister(hw->adp, BCHP_CLK_PM_CTRL, regVal);

	return crystalhd_flea_set_next_power_state(hw, FLEA_EVT_STOP_DEVICE) ==
		BC_STS_SUCCESS;
}

bool
crystalhd_flea_wake_up_hw(struct crystalhd_hw *hw)
{
	if(hw->FleaPowerState != FLEA_PS_ACTIVE)
	{
		if (crystalhd_flea_set_next_power_state(hw, FLEA_EVT_RESUME) !=
		    BC_STS_SUCCESS) {
			hw->WakeUpDecodeDone = false;
			return false;
		}
	}
	if (hw->FleaPowerState != FLEA_PS_ACTIVE) {
		hw->WakeUpDecodeDone = false;
		return false;
	}

	/* Now notify HW of the number of entries in the Free List */
	/* This starts up the channel bitmap delivery */
	if (crystalhd_flea_publish_fll(hw, false) != BC_STS_SUCCESS) {
		hw->WakeUpDecodeDone = false;
		return false;
	}

	hw->WakeUpDecodeDone = true;

	return true;
}

/* Enabled-PWM TX callers own this count until failure or DMA-list admission.
 * Passive FIFO queries never acquire it, including the VC-1 flags variant.
 */
BC_STATUS crystalhd_flea_tx_begin(struct crystalhd_hw *hw)
{
	unsigned long flags;
	BC_STATUS sts = BC_STS_SUCCESS;
	bool wake;

	spin_lock_irqsave(&hw->lock, flags);
	if (!READ_ONCE(hw->adp->present) || READ_ONCE(hw->dma_fault) ||
	    !READ_ONCE(hw->dev_started)) {
		sts = BC_STS_IO_ERROR;
		goto unlock;
	}
	if (hw->FleaPowerTransition) {
		sts = BC_STS_BUSY;
		goto unlock;
	}
	wake = !hw->WakeUpDecodeDone ||
		(hw->FleaPowerState != FLEA_PS_ACTIVE &&
		 hw->SingleThreadAppFIFOEmpty);
	/* Normal TX retries must not undo RX ready-queue backpressure. */
	if (!wake && hw->FleaPowerState != FLEA_PS_ACTIVE) {
		sts = BC_STS_BUSY;
		goto unlock;
	}
	hw->FleaTxActive++;
	spin_unlock_irqrestore(&hw->lock, flags);

	if (wake) {
		sts = crystalhd_flea_set_next_power_state(hw, FLEA_EVT_RESUME);
		if (sts == BC_STS_SUCCESS)
			sts = crystalhd_flea_publish_fll(hw, false);
		if (sts != BC_STS_SUCCESS) {
			crystalhd_flea_tx_end(hw);
			return sts;
		}
		WRITE_ONCE(hw->WakeUpDecodeDone, true);
	}
	return BC_STS_SUCCESS;
unlock:
	spin_unlock_irqrestore(&hw->lock, flags);
	return sts;
}

void crystalhd_flea_tx_end(struct crystalhd_hw *hw)
{
	unsigned long flags;
	bool recheck;

	spin_lock_irqsave(&hw->lock, flags);
	hw->FleaTxActive--;
	recheck = hw->FleaPowerState == FLEA_PS_LP_PENDING ||
		  hw->hw_pause_issued;
	spin_unlock_irqrestore(&hw->lock, flags);
	if (recheck)
		crystalhd_flea_set_next_power_state(hw, FLEA_EVT_CMD_COMP);
}

BC_STATUS crystalhd_flea_request_tx_wrap(struct crystalhd_hw *hw, uint32_t needed_sz)
{
	uint32_t address, available, flags_address, flags;
	const uint32_t flags_offset = offsetof(TX_INPUT_BUFFER_INFO, Flags);
	BC_STATUS sts = BC_STS_NO_DATA;

	if (!hw || !hw->adp || !hw->adp->pdev)
		return BC_STS_INV_ARG;
	lockdep_assert_held(&hw->adp->user_lock);
	lockdep_assert_held(&hw->adp->tx_lock);
	if (hw->adp->pdev->device != BC_PCI_DEVID_FLEA ||
	    !needed_sz || needed_sz > 0xfff0U + 6U)
		return BC_STS_NO_DATA;
	/* The mailbox ISR publishes this cache without hw->lock. Drain it
	 * across the request AND cache invalidation, so a new firmware region
	 * cannot be published and then accidentally erased by this process.
	 * Do not invoke the wake path here: it may repost RX and take hw->lock.
	 */
	/* RX process callbacks also use the shared DRAM access window. */
	down(&hw->fetch_sem);
	disable_irq(hw->adp->pdev->irq);
	dev_dbg(&hw->adp->pdev->dev,
		"Typed TX BUSY: needed=%u contiguous=%u address=%x flags=%x awake=%u power=%u seq=%u info=%x fault=%u present=%u\n",
		needed_sz, hw->TxFwInputBuffInfo.DramBuffSzInBytes,
		hw->TxFwInputBuffInfo.DramBuffAdd, hw->TxFwInputBuffInfo.Flags,
		(unsigned int)hw->WakeUpDecodeDone, (unsigned int)hw->FleaPowerState,
		hw->TxFwInputBuffInfo.SeqNum, hw->TxBuffInfoAddr,
		(unsigned int)READ_ONCE(hw->dma_fault),
		(unsigned int)READ_ONCE(hw->adp->present));
	if (READ_ONCE(hw->dma_fault) || !READ_ONCE(hw->adp->present)) {
		sts = BC_STS_IO_ERROR;
		goto out;
	}
	if (!hw->WakeUpDecodeDone || hw->FleaPowerState != FLEA_PS_ACTIVE ||
	    (hw->TxFwInputBuffInfo.Flags & DFW_FLAGS_TX_ABORT))
		goto out;
	/* A previous typed transfer may still target this firmware region.
	 * TX admission is serialized; the disabled IRQ fences its completion.
	 * Defer WRAP until that DMA ownership has actually retired. */
	if (hw->tx_actq && crystalhd_dioq_count(hw->tx_actq))
		goto out;

	address = hw->TxFwInputBuffInfo.DramBuffAdd;
	available = hw->TxFwInputBuffInfo.DramBuffSzInBytes;
	if (!address || (address & 3) || !available || available >= needed_sz ||
	    !hw->TxBuffInfoAddr || (hw->TxBuffInfoAddr & 3) ||
	    hw->TxBuffInfoAddr > ~0U - flags_offset - sizeof(uint32_t) + 1U)
		goto out;
	flags_address = hw->TxBuffInfoAddr + flags_offset;
	sts = hw->pfnDevDRAMRead(hw, flags_address, 1, &flags);
	if (sts != BC_STS_SUCCESS)
		goto out;
	if (flags & DFW_FLAGS_TX_ABORT) {
		sts = BC_STS_IO_ERROR;
		goto out;
	}
	flags |= DFW_FLAGS_WRAP;
	sts = hw->pfnDevDRAMWrite(hw, flags_address, 1, &flags);
	if (sts != BC_STS_SUCCESS)
		goto out;
	/* Same protocol as the legacy single-thread statistics probe. No DMA
	 * may use this old region while waiting for a fresh mailbox interrupt.
	 */
	hw->TxFwInputBuffInfo.DramBuffSzInBytes = 0;
	hw->TxFwInputBuffInfo.DramBuffAdd = 0;
	dev_dbg(&hw->adp->pdev->dev,
		"Typed TX wrap requested: needed=%u contiguous=%u address=%x\n",
		needed_sz, available, address);
out:
	enable_irq(hw->adp->pdev->irq);
	up(&hw->fetch_sem);
	return sts;
}

bool crystalhd_flea_check_input_full(struct crystalhd_hw *hw, uint32_t needed_sz, uint32_t *empty_sz, bool b_188_byte_pkts, uint8_t *flags)
{
	uint32_t				regVal=0;
	TX_INPUT_BUFFER_INFO	*pTxBuffInfo;
	uint32_t				FlagsAddr=0;

	*empty_sz = 0;
	/* Both enabled-PWM callers hold hw->lock. Actual TX wakes before
	 * entering this callback; a stats query must never recurse into it.
	 */
	if (READ_ONCE(hw->FleaEnablePWM) &&
	    (!READ_ONCE(hw->adp->present) || READ_ONCE(hw->dma_fault) ||
	     !READ_ONCE(hw->dev_started) || hw->FleaPowerTransition ||
	     hw->FleaPowerState != FLEA_PS_ACTIVE)) {
		*flags = 0;
		return true;
	}
/*	*DramAddrOut=0; */


	/* Add condition here to wake up the HW in case some application is trying to do TX before starting RX - like FP */
	/* To prevent deadlocks. We are called here from Synchronized context so we can safely call this directly */

	if(hw->WakeUpDecodeDone != true)
	{
		/* Only wake up the HW if we are either being called from a single threaded app - like FP */
		/* or if we are not checking for the input buffer size as just a test */
		if(*flags == 0) {
			if (READ_ONCE(hw->FleaEnablePWM))
				return true;
			if (!crystalhd_flea_wake_up_hw(hw)) {
				*flags = 0;
				return true; /* Publication failed: do not admit TX. */
			}
		}
		else {
			*empty_sz = 2 * 1024 * 1024; /* FW Buffer size */
			/**DramAddrOut=0; */
			*flags=0;
			return false;
		}
	}

	/* if we have told the app that we have buffer empty then we cannot go to low power */
	if((hw->FleaPowerState != FLEA_PS_ACTIVE) && !hw->SingleThreadAppFIFOEmpty)
	{
		/**TxBuffSzOut=0; */
		/**DramAddrOut=0; */
		*empty_sz = 0;
		*flags=0;
		/*printk("PD can't Tx\n"); */
		return true; /*Indicate FULL*/
	}


	if(hw->TxFwInputBuffInfo.Flags & DFW_FLAGS_TX_ABORT)
	{
		*empty_sz=0;
		/**DramAddrOut=0; */
		*flags |= DFW_FLAGS_TX_ABORT;
		return true;
	}

	if( (hw->TxFwInputBuffInfo.DramBuffSzInBytes < needed_sz)
		||(!hw->TxFwInputBuffInfo.DramBuffAdd))
	{
		*empty_sz=0;
		/**DramAddrOut=0; */
		*flags=0;
		return true; /*Indicate FULL*/
	}

	if(hw->TxFwInputBuffInfo.DramBuffAdd % 4)
	{
		/*
		-- Indicate Full if we get a non-dowrd aligned address.
		-- This will avoid us posting the command to firmware and
		-- The TX will timeout and we will close the application properly.
		-- This avoids a illegal operation as far as the TX is concerned.
		*/
		printk("TxSDRAM-Destination Address Not DWORD Aligned:%x\n",hw->TxFwInputBuffInfo.DramBuffAdd);
		return true;
	}

	/*
	-- We got everything correctly from the firmware and hence we should be
	-- able to do the DMA. Indicate what app wants to hear.
	-- Firmware SAYS: I AM HUNGRY, GIVE ME FOOD. :)
	*/
	*empty_sz=hw->TxFwInputBuffInfo.DramBuffSzInBytes;
	/**dramAddrOut=pHWExt->TxFwInputBuffInfo.DramBuffAdd; */
/*	printk("empty size is %d\n", *empty_sz); */

	/* If we are just checking stats and are not actually going to DMA, don't increment */
	/* But we have to account for single threaded apps */
	if((*flags & 0x08) == 0x08)
	{
		/* This is a synchronized function */
		/* NAREN - In single threaded mode, if we have less than a defined size of buffer */
		/* ask the firmware to wrap around. To prevent deadlocks. */
		if(hw->TxFwInputBuffInfo.DramBuffSzInBytes < TX_WRAP_THRESHOLD)
		{
			pTxBuffInfo = (TX_INPUT_BUFFER_INFO *) (0);
			FlagsAddr = hw->TxBuffInfoAddr + ((uintptr_t) (&pTxBuffInfo->Flags));
			/* Read Modify the Flags to ask the FW to WRAP */
			hw->pfnDevDRAMRead(hw,FlagsAddr,1,&regVal);
			regVal |= DFW_FLAGS_WRAP;
			hw->pfnDevDRAMWrite(hw,FlagsAddr,1,&regVal);

			/* Indicate Busy to the application because we have to get new buffers from FW */
			*empty_sz=0;
			/* *DramAddrOut=0; */
			*flags=0;
			/* Wait for the next interrupt from the HW */
			hw->TxFwInputBuffInfo.DramBuffSzInBytes = 0;
			hw->TxFwInputBuffInfo.DramBuffAdd = 0;
			return true;
		}
		else
			hw->SingleThreadAppFIFOEmpty = true;
	}
	else if (!READ_ONCE(hw->FleaEnablePWM) && (*flags & 0x04) != 0x04)
		hw->EmptyCnt++;		/*OS_INTERLOCK_INCREMENT(&pHWExt->EmptyCnt); */

	/* Different from our Windows implementation */
	/* set bit 7 of the flags field to indicate that we have to use the destination address for TX */
	*flags |= BC_BIT(7);

	return false; /*Indicate Empty*/
}

BC_STATUS crystalhd_flea_fw_cmd_post_proc(struct crystalhd_hw *hw, BC_FW_CMD *fw_cmd)
{
	BC_STATUS sts = BC_STS_SUCCESS;
	unsigned long flags;
	uint32_t channel;
	struct DecRspChannelStartVideo *st_rsp = NULL;
	struct C011_TS_CMD				*pGenRsp = NULL;
	struct DecRspChannelChannelOpen *pRsp  = NULL;

	pGenRsp = (struct C011_TS_CMD *) fw_cmd->rsp;

	switch (fw_cmd->cmd[0]) {
		case eCMD_C011_DEC_CHAN_STREAM_OPEN:
			channel = pGenRsp->ulParams[2];
			if (channel >= FLEA_DECODER_CHANNEL_COUNT) {
				spin_lock_irqsave(&hw->lock, flags);
				hw->fwcmd_poisoned = true;
				spin_unlock_irqrestore(&hw->lock, flags);
				dev_err(&hw->adp->pdev->dev,
					"invalid Flea stream-open channel: %u\n", channel);
				return BC_STS_IO_ERROR;
			}
			hw->channelNum = channel;

			dev_dbg(&hw->adp->pdev->dev, "Snooped Stream Open Cmd For ChNo:%x\n", hw->channelNum);
			break;
		case eCMD_C011_DEC_CHAN_OPEN:
			pRsp = (struct DecRspChannelChannelOpen *)pGenRsp;
			channel = pRsp->ChannelID;
			if (channel >= FLEA_DECODER_CHANNEL_COUNT) {
				spin_lock_irqsave(&hw->lock, flags);
				hw->fwcmd_poisoned = true;
				spin_unlock_irqrestore(&hw->lock, flags);
				dev_err(&hw->adp->pdev->dev,
					"invalid Flea channel-open channel: %u\n", channel);
				return BC_STS_IO_ERROR;
			}
			hw->channelNum = channel;

			/* used in Flea to update the Tx Buffer stats */
			hw->TxBuffInfoAddr = pRsp->transportStreamCaptureAddr;
			hw->TxFwInputBuffInfo.DramBuffAdd=0;
			hw->TxFwInputBuffInfo.DramBuffSzInBytes=0;
			hw->TxFwInputBuffInfo.Flags=0;
			hw->TxFwInputBuffInfo.HostXferSzInBytes=0;
			hw->TxFwInputBuffInfo.SeqNum=0;

			/* NAREN Init power management states here when we start the channel */
			hw->PwrDwnTxIntr = false;
			hw->PwrDwnPiQIntr = false;
			hw->EmptyCnt = 0;
			hw->SingleThreadAppFIFOEmpty = false;

			dev_dbg(&hw->adp->pdev->dev, "Snooped ChOpen Cmd For ChNo:%x TxBuffAddr:%x\n",
					hw->channelNum,
					hw->TxBuffInfoAddr);
			break;
		case eCMD_C011_DEC_CHAN_START_VIDEO:
			st_rsp = (struct DecRspChannelStartVideo *)fw_cmd->rsp;
			hw->pib_del_Q_addr = st_rsp->picInfoDeliveryQ;
			hw->pib_rel_Q_addr = st_rsp->picInfoReleaseQ;

			dev_dbg(&hw->adp->pdev->dev, "Snooping CHAN_START_VIDEO command to get the Addr of Del/Rel Queue\n");
			dev_dbg(&hw->adp->pdev->dev, "DelQAddr:%x RelQAddr:%x\n",
					hw->pib_del_Q_addr, hw->pib_rel_Q_addr);
			break;
		default:
			break;
	}
	return sts;
}

BC_STATUS crystalhd_flea_do_fw_cmd(struct crystalhd_hw *hw, BC_FW_CMD *fw_cmd)
{
	struct device *dev;
	uint32_t cnt = 0, cmd_res_addr;
	uint32_t *cmd_buff, *res_buff;
	BC_STATUS sts;
	unsigned long flags;

	if (!hw || !fw_cmd) {
		printk(KERN_ERR "%s: Invalid Arguments\n", __func__);
		return BC_STS_INV_ARG;
	}

	dev = &hw->adp->pdev->dev;

	dev_dbg(dev, "%s entered\n", __func__);

	cmd_buff = fw_cmd->cmd;
	res_buff = fw_cmd->rsp;

	if (!cmd_buff || !res_buff) {
		dev_err(dev, "Invalid Parameters for F/W Command\n");
		return BC_STS_INV_ARG;
	}

	memset(res_buff, 0, sizeof(fw_cmd->rsp));
	sts = crystalhd_hw_fw_cmd_begin(hw);
	if (sts != BC_STS_SUCCESS)
		return sts;

	if(hw->FleaPowerState != FLEA_PS_ACTIVE)
	{
		crystalhd_flea_set_next_power_state(hw,	FLEA_EVT_FW_CMD_POST);
	}
	if (hw->FleaPowerState != FLEA_PS_ACTIVE) {
		spin_lock_irqsave(&hw->lock, flags);
		crystalhd_hw_fw_cmd_abort_unposted_locked(hw);
		spin_unlock_irqrestore(&hw->lock, flags);
		sts = BC_STS_IO_ERROR;
		goto done;
	}

	spin_lock_irqsave(&hw->lock, flags);

	/*Write the command to the memory*/
	sts = hw->pfnDevDRAMWrite(hw, hw->fwcmdPostAddr, FW_CMD_BUFF_SZ, cmd_buff);

	/*Memory Read for memory arbitrator flush*/
	if (sts == BC_STS_SUCCESS)
		sts = hw->pfnDevDRAMRead(hw, hw->fwcmdPostAddr, 1, &cnt);
	if (sts != BC_STS_SUCCESS) {
		crystalhd_hw_fw_cmd_abort_unposted_locked(hw);
		spin_unlock_irqrestore(&hw->lock, flags);
		sts = BC_STS_IO_ERROR;
		goto done;
	}

	/* Write the command address to mailbox */
	hw->pfnWriteDevRegister(hw->adp, hw->fwcmdPostMbox, hw->fwcmdPostAddr);
	crystalhd_fw_research_command_posted_locked(hw, fw_cmd);

	spin_unlock_irqrestore(&hw->lock, flags);
	crystalhd_fw_research_command_posted(hw, fw_cmd);

	/* The acknowledged mailbox IRQ is the completion fence. Its latched
	 * state also covers a response that arrives before entering the wait. */
	/* FW commands should complete even if we got a signal from the upper layer. */
	sts = crystalhd_hw_fw_cmd_wait(hw);
	if (sts == BC_STS_TIMEOUT) {
		dev_err(dev, "Firmware command T/O\n");
	} else if (sts == BC_STS_IO_USER_ABORT) {
		dev_info(dev, "FwCmd Wait Signal - Can Never Happen\n");
	} else if (sts != BC_STS_SUCCESS) {
		dev_err(dev, "FwCmd IO Error.\n");
	}

	if (sts != BC_STS_SUCCESS) {
		dev_err(dev, "FwCmd Failed.\n");
		goto done;
	}

	spin_lock_irqsave(&hw->lock, flags);

	/*Get the Responce Address*/
	cmd_res_addr = hw->pfnReadDevRegister(hw->adp, hw->fwcmdRespMbox);

	/*Read the Response*/
	sts = hw->pfnDevDRAMRead(hw, cmd_res_addr, FW_CMD_BUFF_SZ, res_buff);
	if (sts != BC_STS_SUCCESS) {
		/* Firmware may have committed the command. Without its reply, do
		 * not publish stale/partial state or admit another command before
		 * verified firmware/device recovery. The ISR already retired it.
		 */
		memset(res_buff, 0, sizeof(fw_cmd->rsp));
		hw->fwcmd_poisoned = true;
		sts = BC_STS_IO_ERROR;
	}

	spin_unlock_irqrestore(&hw->lock, flags);
	if (sts != BC_STS_SUCCESS)
		goto done;

	if (res_buff[2] != 0) {
		if (cmd_buff[0] == eCMD_C011_DEC_CHAN_START_VIDEO)
			dev_err(dev,
				"firmware reply rejected: stage=reply-validation "
				"request=0x%08x sequence=0x%08x channel=%u "
				"response=0x%08x response-sequence=0x%08x "
				"status=0x%08x\n",
				cmd_buff[0], cmd_buff[1], cmd_buff[2],
				res_buff[0], res_buff[1], res_buff[2]);
		else
			dev_err(dev,
				"firmware reply rejected: stage=reply-validation "
				"request=0x%08x sequence=0x%08x "
				"request-word2=0x%08x response=0x%08x "
				"response-sequence=0x%08x status=0x%08x\n",
				cmd_buff[0], cmd_buff[1], cmd_buff[2],
				res_buff[0], res_buff[1], res_buff[2]);
		sts = BC_STS_FW_CMD_ERR;
		goto done;
	}

	sts = crystalhd_flea_fw_cmd_post_proc(hw, fw_cmd);
	if (sts != BC_STS_SUCCESS)
		dev_err(dev, "crystalhd_fw_cmd_post_proc Failed.\n");

done:
	crystalhd_hw_fw_cmd_end(hw);
	/* The last reply consumer may have been the only remaining inhibitor. */
	if (READ_ONCE(hw->FleaEnablePWM) &&
	    READ_ONCE(hw->FleaPowerState) == FLEA_PS_LP_PENDING)
		crystalhd_flea_set_next_power_state(hw, FLEA_EVT_CMD_COMP);
	return sts;

}

void crystalhd_flea_get_dnsz(struct crystalhd_hw *hw, uint32_t list_index, uint32_t *y_dw_dnsz, uint32_t *uv_dw_dnsz)
{
	uint32_t y_dn_sz_reg, uv_dn_sz_reg;

	if (!list_index) {
		y_dn_sz_reg  = BCHP_MISC1_Y_RX_LIST0_CUR_BYTE_CNT;
		uv_dn_sz_reg = BCHP_MISC1_HIF_RX_LIST0_CUR_BYTE_CNT;
	} else {
		y_dn_sz_reg  = BCHP_MISC1_Y_RX_LIST1_CUR_BYTE_CNT;
		uv_dn_sz_reg = BCHP_MISC1_HIF_RX_LIST1_CUR_BYTE_CNT;
	}

	*y_dw_dnsz  = hw->pfnReadFPGARegister(hw->adp, y_dn_sz_reg);
	*uv_dw_dnsz = hw->pfnReadFPGARegister(hw->adp, uv_dn_sz_reg);

	return ;
}

BC_STATUS crystalhd_flea_hw_pause(struct crystalhd_hw *hw, bool state)
{
	/* SUCCESS accepts the request; LP_PENDING is not physical quiescence. */
	return crystalhd_flea_set_next_power_state(hw,
		state ? FLEA_EVT_PAUSE : FLEA_EVT_RESUME);
}

/* RX flow-control intent is distinct from firmware-command wakeups. */
BC_STATUS crystalhd_flea_rx_pause(struct crystalhd_hw *hw, bool state)
{
	return crystalhd_flea_set_next_power_state(hw,
		state ? FLEA_EVT_RX_PAUSE : FLEA_EVT_RX_RESUME);
}

bool crystalhd_flea_peek_next_decoded_frame(struct crystalhd_hw *hw, uint64_t *meta_payload, uint32_t *picNumFlags, uint32_t PicWidth)
{
	unsigned long flags = 0;
	struct crystalhd_dioq *ioq;
	struct crystalhd_elem *tmp;
	struct crystalhd_rx_dma_pkt *rpkt;

	*meta_payload = 0;

	ioq = hw->rx_rdyq;
	spin_lock_irqsave(&ioq->lock, flags);

	if ((ioq->count > 0) && (ioq->head != (struct crystalhd_elem *)&ioq->head)) {
		tmp = ioq->head;
		spin_unlock_irqrestore(&ioq->lock, flags);
		rpkt = (struct crystalhd_rx_dma_pkt *)tmp->data;
		if (rpkt) {
			/* Status may inspect the same ready picture repeatedly. Saving
			 * the first pixels for userspace belongs to the actual dequeue,
			 * otherwise a second peek saves our line-number marker as pixels.
			 */
			flea_get_picture_info(hw, rpkt, picNumFlags, meta_payload, false);
			/*printk("%s: flea_GetPictureInfo Pic#:%d\n", __func__, PicNumber); */
		}
		return true;
	}
	spin_unlock_irqrestore(&ioq->lock, flags);

	return false;

}

void crystalhd_flea_clear_rx_errs_intrs(struct crystalhd_hw *hw)
/*
-- Clears all the errors and interrupt on RX DMA engine.
*/
{
	uint32_t ulRegVal;
	union FLEA_INTR_BITS_COMMON	IntrToClear,IntrSts;

	IntrToClear.WholeReg = 0;
	IntrSts.WholeReg = 0;

	IntrSts.WholeReg = hw->pfnReadDevRegister(hw->adp, BCHP_INTR_INTR_STATUS);
	if(IntrSts.WholeReg)
	{
		ulRegVal = hw->pfnReadDevRegister(hw->adp, BCHP_MISC1_Y_RX_ERROR_STATUS);
		hw->pfnWriteDevRegister(hw->adp, BCHP_MISC1_Y_RX_ERROR_STATUS, ulRegVal);
		ulRegVal = hw->pfnReadDevRegister(hw->adp, BCHP_MISC1_HIF_RX_ERROR_STATUS);
		hw->pfnWriteDevRegister(hw->adp, BCHP_MISC1_HIF_RX_ERROR_STATUS, ulRegVal);

		IntrToClear.L0UVRxDMADone	=	IntrSts.L0UVRxDMADone;
		IntrToClear.L0UVRxDMAErr	=	IntrSts.L0UVRxDMAErr;
		IntrToClear.L0YRxDMADone	=	IntrSts.L0YRxDMADone;
		IntrToClear.L0YRxDMAErr		=	IntrSts.L0YRxDMAErr;
		IntrToClear.L1UVRxDMADone	=	IntrSts.L1UVRxDMADone;
		IntrToClear.L1UVRxDMAErr	=	IntrSts.L1UVRxDMAErr;
		IntrToClear.L1YRxDMADone	=	IntrSts.L1YRxDMADone;
		IntrToClear.L1YRxDMAErr		=	IntrSts.L1YRxDMAErr;

		hw->pfnWriteDevRegister(hw->adp, BCHP_INTR_INTR_CLR_REG, IntrToClear.WholeReg);

		hw->pfnWriteDevRegister(hw->adp, BCHP_INTR_EOI_CTRL, 1);
	}
	return;
}


void crystalhd_flea_stop_rx_dma_engine(struct crystalhd_hw *hw)
{
	union FLEA_INTR_BITS_COMMON	IntrStsValue;
	bool failedL0 = true, failedL1 = true;
	uint32_t pollCnt = 0;
	unsigned long flags;

	hw->RxCaptureState = 2;

	if((hw->rx_list_sts[0] == sts_free) && (hw->rx_list_sts[1] == sts_free)) {
		hw->RxCaptureState = 0;
		hw->RxSeqNum = 0;
		hw->rx_list_post_index = 0;
		return; /* Nothing to be done */
	}

	if(hw->rx_list_sts[0] == sts_free)
		failedL0 = false;
	if(hw->rx_list_sts[1] == sts_free)
		failedL1 = false;

	while(1)
	{
		IntrStsValue.WholeReg = hw->pfnReadDevRegister(hw->adp, BCHP_INTR_INTR_STATUS);

		if(hw->rx_list_sts[0] != sts_free) {
			if ((!(hw->rx_list_sts[0] & rx_waiting_y_intr) ||
			     IntrStsValue.L0YRxDMADone || IntrStsValue.L0YRxDMAErr) &&
			    (!(hw->rx_list_sts[0] & rx_waiting_uv_intr) ||
			     IntrStsValue.L0UVRxDMADone || IntrStsValue.L0UVRxDMAErr))
			{
				failedL0 = false;
			}
		}
		else
			failedL0 = false;

		if(hw->rx_list_sts[1] != sts_free) {
			if ((!(hw->rx_list_sts[1] & rx_waiting_y_intr) ||
			     IntrStsValue.L1YRxDMADone || IntrStsValue.L1YRxDMAErr) &&
			    (!(hw->rx_list_sts[1] & rx_waiting_uv_intr) ||
			     IntrStsValue.L1UVRxDMADone || IntrStsValue.L1UVRxDMAErr))
			{
				failedL1 = false;
			}
		}
		else
			failedL1 = false;

		msleep(10); /* Pending close signal must not collapse DMA drain waits. */

		if(pollCnt >= MAX_VALID_POLL_CNT)
			break;

		if((failedL0 == false) && (failedL1 == false))
			break;

		pollCnt++;
	}

	if(failedL0 || failedL1)
		crystalhd_hw_dma_fatal_stop(hw);

	hw->RxCaptureState = 0;
	hw->RxSeqNum = 0;

	crystalhd_flea_clear_rx_errs_intrs(hw);
	/* Stop runs with IRQ delivery disabled. Its polling consumed the
	 * completions, so the ISR can no longer clear the software busy bits.
	 */
	if (!failedL0 && !failedL1) {
		spin_lock_irqsave(&hw->rx_lock, flags);
		hw->rx_list_sts[0] = sts_free;
		hw->rx_list_sts[1] = sts_free;
		hw->rx_list_post_index = 0;
		spin_unlock_irqrestore(&hw->rx_lock, flags);
	}
}

BC_STATUS crystalhd_flea_hw_fire_rxdma(struct crystalhd_hw *hw,
#ifdef CRYSTALHD_ENABLE_FW_RESEARCH
					   struct crystalhd_rx_dma_pkt *rx_pkt,
					   struct crystalhd_rx_receipt_key *receipt_key
#else
					   struct crystalhd_rx_dma_pkt *rx_pkt
#endif
					   )
{
	struct device *dev;
	addr_64 desc_addr;
	unsigned long rx_flags, hw_flags;
	PIC_DELIVERY_HOST_INFO	PicDeliInfo = {0};
	uint32_t BuffSzInDwords, pkt_tag, channel;
	BC_STATUS sts;

	if (!hw || !rx_pkt || !rx_pkt->buffer) {
		printk(KERN_ERR "%s: Invalid Arguments\n", __func__);
		return BC_STS_INV_ARG;
	}

	dev = &hw->adp->pdev->dev;
	channel = READ_ONCE(hw->channelNum);
	if (channel >= FLEA_DECODER_CHANNEL_COUNT) {
		dev_err(dev, "Flea RX channel out of range: %u\n", channel);
		return BC_STS_IO_ERROR;
	}

	if (hw->rx_list_post_index >= DMA_ENGINE_CNT) {
		dev_err(dev, "List Out Of bounds %x\n", hw->rx_list_post_index);
		return BC_STS_INV_ARG;
	}
	if (READ_ONCE(hw->dma_fault))
		return BC_STS_IO_ERROR;

	if(hw->RxCaptureState != 1) {
		dev_err(dev, "Capture not enabled\n");
		return BC_STS_BUSY;
	}

	spin_lock_irqsave(&hw->rx_lock, rx_flags);
	if (hw->rx_list_post_index >= DMA_ENGINE_CNT) {
		dev_err(dev, "List Out Of bounds %x\n", hw->rx_list_post_index);
		spin_unlock_irqrestore(&hw->rx_lock, rx_flags);
		return BC_STS_INV_ARG;
	}
	if (hw->rx_list_sts[hw->rx_list_post_index]) {
		dev_dbg(dev, "HW list is busy\n");
		spin_unlock_irqrestore(&hw->rx_lock, rx_flags);
		return BC_STS_BUSY;
	}

	if (!TEST_BIT(hw->PicQSts, channel)) {
		/* NO pictures available for this channel */
		dev_dbg(dev, "No Picture Available for DMA\n");
		spin_unlock_irqrestore(&hw->rx_lock, rx_flags);
		return BC_STS_BUSY;
	}

	desc_addr.full_addr = rx_pkt->desc_mem.phy_addr;

	PicDeliInfo.ListIndex = hw->rx_list_post_index;
	PicDeliInfo.RxSeqNumber = hw->RxSeqNum;
	PicDeliInfo.HostDescMemLowAddr_Y = desc_addr.low_part;
	PicDeliInfo.HostDescMemHighAddr_Y = desc_addr.high_part;
	PicDeliInfo.ChannelID = channel;
	if (rx_pkt->uv_phy_addr) {
		/* Program the UV descriptor */
		desc_addr.full_addr = rx_pkt->uv_phy_addr;
		PicDeliInfo.HostDescMemLowAddr_UV = desc_addr.low_part;
		PicDeliInfo.HostDescMemHighAddr_UV = desc_addr.high_part;
	}

	BuffSzInDwords = (sizeof (PicDeliInfo) - sizeof(PicDeliInfo.Reserved))/4;

	/* Keep the shared metadata and list admission serialized until the
	 * doorbell. A failed write must not transfer ownership or arm an IRQ.
	 */
	spin_lock_irqsave(&hw->lock, hw_flags);
	if (READ_ONCE(hw->dma_fault)) {
		sts = BC_STS_IO_ERROR;
		goto unlock;
	}
	if (READ_ONCE(hw->FleaEnablePWM)) {
		if (!READ_ONCE(hw->adp->present) || !READ_ONCE(hw->dev_started)) {
			sts = BC_STS_IO_ERROR;
			goto unlock;
		}
		if (hw->FleaPowerState != FLEA_PS_ACTIVE ||
		    hw->FleaPowerTransition) {
			sts = BC_STS_BUSY;
			goto unlock;
		}
	}
	sts = hw->pfnDevDRAMWrite(hw, hw->FleaRxPicDelAddr, BuffSzInDwords,
				(uint32_t *)&PicDeliInfo);
	if (sts != BC_STS_SUCCESS)
		goto unlock;

	pkt_tag = hw->rx_pkt_tag_seed + hw->rx_list_post_index;
	sts = crystalhd_dioq_add(hw->rx_actq, rx_pkt, false, pkt_tag);
	if (sts != BC_STS_SUCCESS)
		goto unlock;
	rx_pkt->pkt_tag = pkt_tag;
#ifdef CRYSTALHD_ENABLE_FW_RESEARCH
	crystalhd_rx_receipt_reserve(hw, rx_pkt, hw->rx_list_post_index,
				     hw->RxSeqNum, channel, receipt_key);
#endif
	CLEAR_BIT(hw->PicQSts, channel);
	hw->rx_list_sts[hw->rx_list_post_index] |= rx_waiting_y_intr;
	if (rx_pkt->uv_phy_addr)
		hw->rx_list_sts[hw->rx_list_post_index] |= rx_waiting_uv_intr;
	hw->rx_list_post_index = (hw->rx_list_post_index + 1) % DMA_ENGINE_CNT;

	crystalhd_rx_buffer_sync_for_device(hw->adp, rx_pkt->buffer);
	hw->pfnWriteDevRegister(hw->adp, RX_POST_MAILBOX, channel);
	hw->RxSeqNum++;

unlock:
	spin_unlock_irqrestore(&hw->lock, hw_flags);
	spin_unlock_irqrestore(&hw->rx_lock, rx_flags);
	return sts;
}

BC_STATUS crystalhd_flea_hw_post_cap_buff(struct crystalhd_hw *hw, struct crystalhd_rx_dma_pkt *rx_pkt)
{
#ifdef CRYSTALHD_ENABLE_FW_RESEARCH
	struct crystalhd_rx_receipt_key receipt_key = { };
	BC_STATUS sts = crystalhd_flea_hw_fire_rxdma(hw, rx_pkt, &receipt_key);

	if (sts == BC_STS_SUCCESS)
		crystalhd_rx_receipt_fire_return(hw, &receipt_key);
#else
	BC_STATUS sts = crystalhd_flea_hw_fire_rxdma(hw, rx_pkt);
#endif

	/* BUSY transfers ownership to the free queue; other errors leave the
	 * packet with the caller, just as on BCM70012.
	 */
	if (sts == BC_STS_BUSY) {
		BC_STATUS queued = crystalhd_dioq_add(hw->rx_freeq, rx_pkt,
						false, rx_pkt->pkt_tag);
		if (queued != BC_STS_SUCCESS)
			return queued;
	}

	hw->pfnNotifyFLLChange(hw, false);

	return sts;
}

BC_STATUS crystalhd_flea_prepare_tx_dma(struct crystalhd_hw *hw, uint32_t bytes)
{
	TX_INPUT_BUFFER_INFO TxBuffInfo = { 0 };
	uint32_t WrSzInDWords = (sizeof(TxBuffInfo.DramBuffAdd) +
				 sizeof(TxBuffInfo.DramBuffSzInBytes) +
				 sizeof(TxBuffInfo.HostXferSzInBytes)) / 4;

	/* Do not reuse capacity after attempting a firmware-visible write. */
	hw->TxFwInputBuffInfo.DramBuffAdd = 0;
	hw->TxFwInputBuffInfo.DramBuffSzInBytes = 0;
	TxBuffInfo.HostXferSzInBytes = bytes;

	return hw->pfnDevDRAMWrite(hw, hw->TxBuffInfoAddr, WrSzInDWords,
				 (uint32_t *)&TxBuffInfo);
}

void crystalhd_flea_start_tx_dma_engine(struct crystalhd_hw *hw, uint8_t list_id, addr_64 desc_addr)
{
	uint32_t dma_cntrl;
	uint32_t first_desc_u_addr, first_desc_l_addr;

	if (!READ_ONCE(hw->FleaEnablePWM))
		hw->EmptyCnt--;
	hw->SingleThreadAppFIFOEmpty = false;

	if (list_id == 0) {
		first_desc_u_addr = BCHP_MISC1_TX_FIRST_DESC_U_ADDR_LIST0;
		first_desc_l_addr = BCHP_MISC1_TX_FIRST_DESC_L_ADDR_LIST0;
	} else {
		first_desc_u_addr = BCHP_MISC1_TX_FIRST_DESC_U_ADDR_LIST1;
		first_desc_l_addr = BCHP_MISC1_TX_FIRST_DESC_L_ADDR_LIST1;
	}

	dma_cntrl = hw->pfnReadFPGARegister(hw->adp, BCHP_MISC1_TX_SW_DESC_LIST_CTRL_STS);
	if (!(dma_cntrl & BCHP_MISC1_TX_SW_DESC_LIST_CTRL_STS_TX_DMA_RUN_STOP_MASK)) {
		dma_cntrl |= BCHP_MISC1_TX_SW_DESC_LIST_CTRL_STS_TX_DMA_RUN_STOP_MASK;
		hw->pfnWriteFPGARegister(hw->adp, BCHP_MISC1_TX_SW_DESC_LIST_CTRL_STS,
								 dma_cntrl);
	}

	hw->pfnWriteFPGARegister(hw->adp, first_desc_u_addr, desc_addr.high_part);

	hw->pfnWriteFPGARegister(hw->adp, first_desc_l_addr, desc_addr.low_part | 0x01);
	/* Be sure we set the valid bit ^^^^ */

	return;
}

BC_STATUS crystalhd_flea_stop_tx_dma_engine(struct crystalhd_hw *hw)
{
	struct device *dev;
	uint32_t dma_cntrl, cnt = 30;
	uint32_t l1 = 1, l2 = 1;

	dma_cntrl = hw->pfnReadFPGARegister(hw->adp, BCHP_MISC1_TX_SW_DESC_LIST_CTRL_STS);

	dev = &hw->adp->pdev->dev;

	dev_dbg(dev, "Stopping TX DMA Engine..\n");

	if (!(dma_cntrl & BCHP_MISC1_TX_SW_DESC_LIST_CTRL_STS_TX_DMA_RUN_STOP_MASK)) {
		hw->TxList0Sts = ListStsFree;
		hw->TxList1Sts = ListStsFree;
		hw->tx_list_post_index = 0;

		dev_dbg(dev, "Already Stopped\n");
		return BC_STS_SUCCESS;
	}

	crystalhd_flea_disable_interrupts(hw);

	/* Issue stop to HW */
	dma_cntrl &= ~BCHP_MISC1_TX_SW_DESC_LIST_CTRL_STS_TX_DMA_RUN_STOP_MASK;
	hw->pfnWriteFPGARegister(hw->adp, BCHP_MISC1_TX_SW_DESC_LIST_CTRL_STS, dma_cntrl);

	dev_dbg(dev, "Cleared the DMA Start bit\n");

	/* Poll for 3seconds (30 * 100ms) on both the lists..*/
	while ((l1 || l2) && cnt) {

		if (l1) {
			l1 = hw->pfnReadFPGARegister(hw->adp,
										 BCHP_MISC1_TX_FIRST_DESC_L_ADDR_LIST0);
			l1 &= BCHP_MISC1_TX_SW_DESC_LIST_CTRL_STS_TX_DMA_RUN_STOP_MASK;
		}

		if (l2) {
			l2 = hw->pfnReadFPGARegister(hw->adp,
										 BCHP_MISC1_TX_FIRST_DESC_L_ADDR_LIST1);
			l2 &= BCHP_MISC1_TX_SW_DESC_LIST_CTRL_STS_TX_DMA_RUN_STOP_MASK;
		}

		msleep(100); /* Drain DMA even when the closing task has a signal. */

		cnt--;
	}

	if (!cnt) {
		dev_err(dev, "Failed to stop TX DMA.. l1 %d, l2 %d\n", l1, l2);
		crystalhd_flea_enable_interrupts(hw);
		return BC_STS_ERROR;
	}

	hw->TxList0Sts = ListStsFree;
	hw->TxList1Sts = ListStsFree;

	hw->tx_list_post_index = 0;
	dev_dbg(dev, "stopped TX DMA..\n");
	crystalhd_flea_enable_interrupts(hw);

	return BC_STS_SUCCESS;
}

static void crystalhd_flea_update_tx_done_to_fw(struct crystalhd_hw *hw)
{
	struct device *dev;
	uint32_t				regVal		= 0;
	uint32_t				seqNumAddr	= 0;
	uint32_t				seqVal		= 0;
	TX_INPUT_BUFFER_INFO	*pTxBuffInfo;

	dev = &hw->adp->pdev->dev;
	/*
	-- first update the sequence number and then update the
	-- scratch.
	*/
	pTxBuffInfo = (TX_INPUT_BUFFER_INFO *) (0);
	seqNumAddr = hw->TxBuffInfoAddr + ((uintptr_t) (&pTxBuffInfo->SeqNum));

	/*Read the seqnece number */
	hw->pfnDevDRAMRead(hw, seqNumAddr, 1, &regVal);

	seqVal = regVal;
	regVal++;

	/*Increment and Write back to same memory location. */
	hw->pfnDevDRAMWrite(hw,	seqNumAddr, 1, &regVal);

	regVal = hw->pfnReadDevRegister(hw->adp, INDICATE_TX_DONE_REG);
	regVal++;
	hw->pfnWriteDevRegister(hw->adp, INDICATE_TX_DONE_REG, regVal);

	dev_dbg(dev, "TxUpdate[SeqNum DRAM Addr:%x] SeqNum:%x ScratchValue:%x\n",
		seqNumAddr, seqVal, regVal);

	return;
}

bool crystalhd_flea_tx_list0_handler(struct crystalhd_hw *hw, uint32_t err_sts)
{
	uint32_t err_mask, tmp;

	err_mask = MISC1_TX_DMA_ERROR_STATUS_TX_L0_DESC_TX_ABORT_ERRORS_MASK |
	MISC1_TX_DMA_ERROR_STATUS_TX_L0_DMA_DATA_TX_ABORT_ERRORS_MASK |
	MISC1_TX_DMA_ERROR_STATUS_TX_L0_FIFO_FULL_ERRORS_MASK;

	if (!(err_sts & err_mask))
		return false;

	dev_err(&hw->adp->pdev->dev, "Error on Tx-L0 %x\n", err_sts);

	tmp = err_mask;

	if (err_sts & MISC1_TX_DMA_ERROR_STATUS_TX_L0_FIFO_FULL_ERRORS_MASK)
		tmp &= ~MISC1_TX_DMA_ERROR_STATUS_TX_L0_FIFO_FULL_ERRORS_MASK;

	if (tmp) {
		/* reset list index.*/
		hw->tx_list_post_index = 0;
	}

	tmp = err_sts & err_mask;
	hw->pfnWriteDevRegister(hw->adp, BCHP_MISC1_TX_DMA_ERROR_STATUS, tmp);

	return true;
}

bool crystalhd_flea_tx_list1_handler(struct crystalhd_hw *hw, uint32_t err_sts)
{
	uint32_t err_mask, tmp;

	err_mask = MISC1_TX_DMA_ERROR_STATUS_TX_L1_DESC_TX_ABORT_ERRORS_MASK |
	MISC1_TX_DMA_ERROR_STATUS_TX_L1_DMA_DATA_TX_ABORT_ERRORS_MASK |
	MISC1_TX_DMA_ERROR_STATUS_TX_L1_FIFO_FULL_ERRORS_MASK;

	if (!(err_sts & err_mask))
		return false;

	dev_err(&hw->adp->pdev->dev, "Error on Tx-L1 %x\n", err_sts);

	tmp = err_mask;

	if (err_sts & MISC1_TX_DMA_ERROR_STATUS_TX_L1_FIFO_FULL_ERRORS_MASK)
		tmp &= ~MISC1_TX_DMA_ERROR_STATUS_TX_L1_FIFO_FULL_ERRORS_MASK;

	if (tmp) {
		/* reset list index.*/
		hw->tx_list_post_index = 0;
	}

	tmp = err_sts & err_mask;
	hw->pfnWriteDevRegister(hw->adp, BCHP_MISC1_TX_DMA_ERROR_STATUS, tmp);

	return true;
}

void crystalhd_flea_tx_isr(struct crystalhd_hw *hw, union FLEA_INTR_BITS_COMMON int_sts)
{
	uint32_t err_sts;

	if (int_sts.L0TxDMADone) {
		hw->TxList0Sts &= ~TxListWaitingForIntr;
		crystalhd_hw_tx_req_complete(hw, hw->tx_ioq_tag_seed + 0, BC_STS_SUCCESS);
	}

	if (int_sts.L1TxDMADone) {
		hw->TxList1Sts &= ~TxListWaitingForIntr;
		crystalhd_hw_tx_req_complete(hw, hw->tx_ioq_tag_seed + 1, BC_STS_SUCCESS);
	}

	if (!(int_sts.L0TxDMAErr || int_sts.L1TxDMAErr))
		/* No error mask set.. */
		return;

	/* Handle Tx errors. */
	err_sts = hw->pfnReadDevRegister(hw->adp, BCHP_MISC1_TX_DMA_ERROR_STATUS);

	if (crystalhd_flea_tx_list0_handler(hw, err_sts))
		crystalhd_hw_tx_req_complete(hw, hw->tx_ioq_tag_seed + 0, BC_STS_ERROR);

	if (crystalhd_flea_tx_list1_handler(hw, err_sts))
		crystalhd_hw_tx_req_complete(hw, hw->tx_ioq_tag_seed + 1, BC_STS_ERROR);

	hw->stats.tx_errors++;
}

bool crystalhd_flea_rx_list0_handler(struct crystalhd_hw *hw,
									 union FLEA_INTR_BITS_COMMON int_sts,
									 uint32_t y_err_sts,
									 uint32_t uv_err_sts)
{
	uint32_t tmp;
	enum list_sts tmp_lsts;

	if (!(y_err_sts & GET_Y0_ERR_MSK) && !(uv_err_sts & GET_UV0_ERR_MSK))
		return false;

	tmp_lsts = hw->rx_list_sts[0];

	/* Y0 - DMA */
	tmp = y_err_sts & GET_Y0_ERR_MSK;
	if (int_sts.L0YRxDMADone)
		hw->rx_list_sts[0] &= ~rx_waiting_y_intr;

	if (y_err_sts & MISC1_Y_RX_ERROR_STATUS_RX_L0_UNDERRUN_ERROR_MASK) {
		hw->rx_list_sts[0] &= ~rx_waiting_y_intr;
		tmp &= ~MISC1_Y_RX_ERROR_STATUS_RX_L0_UNDERRUN_ERROR_MASK;
	}

	if (y_err_sts & MISC1_Y_RX_ERROR_STATUS_RX_L0_FIFO_FULL_ERRORS_MASK) {
		/* Can never happen for Flea */
		printk("FLEA fifo full - impossible\n");
		hw->rx_list_sts[0] &= ~rx_y_mask;
		hw->rx_list_sts[0] |= rx_y_error;
		tmp &= ~MISC1_Y_RX_ERROR_STATUS_RX_L0_FIFO_FULL_ERRORS_MASK;
	}

	if (tmp) {
		hw->rx_list_sts[0] &= ~rx_y_mask;
		hw->rx_list_sts[0] |= rx_y_error;
		hw->rx_list_post_index = 0;
	}

	/* UV0 - DMA */
	tmp = uv_err_sts & GET_UV0_ERR_MSK;
	if (int_sts.L0UVRxDMADone)
		hw->rx_list_sts[0] &= ~rx_waiting_uv_intr;

	if (uv_err_sts & MISC1_UV_RX_ERROR_STATUS_RX_L0_UNDERRUN_ERROR_MASK) {
		hw->rx_list_sts[0] &= ~rx_waiting_uv_intr;
		tmp &= ~MISC1_UV_RX_ERROR_STATUS_RX_L0_UNDERRUN_ERROR_MASK;
	}

	if (uv_err_sts & MISC1_UV_RX_ERROR_STATUS_RX_L0_FIFO_FULL_ERRORS_MASK) {
		/* Can never happen for Flea */
		printk("FLEA fifo full - impossible\n");
		hw->rx_list_sts[0] &= ~rx_uv_mask;
		hw->rx_list_sts[0] |= rx_uv_error;
		tmp &= ~MISC1_UV_RX_ERROR_STATUS_RX_L0_FIFO_FULL_ERRORS_MASK;
	}

	if (tmp) {
		hw->rx_list_sts[0] &= ~rx_uv_mask;
		hw->rx_list_sts[0] |= rx_uv_error;
		hw->rx_list_post_index = 0;
	}

	if (y_err_sts & GET_Y0_ERR_MSK) {
		tmp = y_err_sts & GET_Y0_ERR_MSK;
		hw->pfnWriteDevRegister(hw->adp, BCHP_MISC1_Y_RX_ERROR_STATUS, tmp);
	}

	if (uv_err_sts & GET_UV0_ERR_MSK) {
		tmp = uv_err_sts & GET_UV0_ERR_MSK;
		hw->pfnWriteDevRegister(hw->adp, BCHP_MISC1_HIF_RX_ERROR_STATUS, tmp);
	}

	return (tmp_lsts != hw->rx_list_sts[0]);
}

bool crystalhd_flea_rx_list1_handler(struct crystalhd_hw *hw,
									 union FLEA_INTR_BITS_COMMON int_sts,
									 uint32_t y_err_sts,
									 uint32_t uv_err_sts)
{
	uint32_t tmp;
	enum list_sts tmp_lsts;

	if (!(y_err_sts & GET_Y1_ERR_MSK) && !(uv_err_sts & GET_UV1_ERR_MSK))
		return false;

	tmp_lsts = hw->rx_list_sts[1];

	/* Y1 - DMA */
	tmp = y_err_sts & GET_Y1_ERR_MSK;
	if (int_sts.L1YRxDMADone)
		hw->rx_list_sts[1] &= ~rx_waiting_y_intr;

	if (y_err_sts & MISC1_Y_RX_ERROR_STATUS_RX_L1_UNDERRUN_ERROR_MASK) {
		hw->rx_list_sts[1] &= ~rx_waiting_y_intr;
		tmp &= ~MISC1_Y_RX_ERROR_STATUS_RX_L1_UNDERRUN_ERROR_MASK;
	}

	if (y_err_sts & MISC1_Y_RX_ERROR_STATUS_RX_L1_FIFO_FULL_ERRORS_MASK) {
		/* Can never happen for Flea */
		printk("FLEA fifo full - impossible\n");
		hw->rx_list_sts[1] &= ~rx_y_mask;
		hw->rx_list_sts[1] |= rx_y_error;
		tmp &= ~MISC1_Y_RX_ERROR_STATUS_RX_L1_FIFO_FULL_ERRORS_MASK;
	}

	if (tmp) {
		hw->rx_list_sts[1] &= ~rx_y_mask;
		hw->rx_list_sts[1] |= rx_y_error;
		hw->rx_list_post_index = 0;
	}

	/* UV1 - DMA */
	tmp = uv_err_sts & GET_UV1_ERR_MSK;
	if (int_sts.L1UVRxDMADone)
		hw->rx_list_sts[1] &= ~rx_waiting_uv_intr;

	if (uv_err_sts & MISC1_UV_RX_ERROR_STATUS_RX_L1_UNDERRUN_ERROR_MASK) {
		hw->rx_list_sts[1] &= ~rx_waiting_uv_intr;
		tmp &= ~MISC1_UV_RX_ERROR_STATUS_RX_L1_UNDERRUN_ERROR_MASK;
	}

	if (uv_err_sts & MISC1_UV_RX_ERROR_STATUS_RX_L1_FIFO_FULL_ERRORS_MASK) {
		/* Can never happen for Flea */
		printk("FLEA fifo full - impossible\n");
		hw->rx_list_sts[1] &= ~rx_uv_mask;
		hw->rx_list_sts[1] |= rx_uv_error;
		tmp &= ~MISC1_UV_RX_ERROR_STATUS_RX_L1_FIFO_FULL_ERRORS_MASK;
	}

	if (tmp) {
		hw->rx_list_sts[1] &= ~rx_uv_mask;
		hw->rx_list_sts[1] |= rx_uv_error;
		hw->rx_list_post_index = 0;
	}

	if (y_err_sts & GET_Y1_ERR_MSK) {
		tmp = y_err_sts & GET_Y1_ERR_MSK;
		hw->pfnWriteDevRegister(hw->adp, BCHP_MISC1_Y_RX_ERROR_STATUS, tmp);
	}

	if (uv_err_sts & GET_UV1_ERR_MSK) {
		tmp = uv_err_sts & GET_UV1_ERR_MSK;
		hw->pfnWriteDevRegister(hw->adp, BCHP_MISC1_HIF_RX_ERROR_STATUS, tmp);
	}

	return (tmp_lsts != hw->rx_list_sts[1]);
}

void crystalhd_flea_rx_isr(struct crystalhd_hw *hw, union FLEA_INTR_BITS_COMMON intr_sts)
{
	struct crystalhd_rx_dma_pkt *completed[DMA_ENGINE_CNT] = { NULL, NULL };
	BC_STATUS completion_status[DMA_ENGINE_CNT] = {
		BC_STS_NO_DATA, BC_STS_NO_DATA
	};
	unsigned long flags;
	uint32_t i, list_avail = 0;
	BC_STATUS comp_sts = BC_STS_NO_DATA;
	uint32_t y_err_sts, uv_err_sts, y_dn_sz = 0, uv_dn_sz = 0;
	bool ret = 0;
	uint64_t receipt_irq_entry_ns;

	if (!hw) {
		printk(KERN_ERR "%s: Invalid Arguments\n", __func__);
		return;
	}
	receipt_irq_entry_ns = crystalhd_rx_receipt_irq_entry(hw);

	if (!(intr_sts.L0YRxDMADone || intr_sts.L1YRxDMADone || intr_sts.L0UVRxDMADone || intr_sts.L1UVRxDMADone ||
		intr_sts.L0YRxDMAErr || intr_sts.L1YRxDMAErr || intr_sts.L0UVRxDMAErr || intr_sts.L1UVRxDMAErr))
		return;

	spin_lock_irqsave(&hw->rx_lock, flags);

	y_err_sts = hw->pfnReadDevRegister(hw->adp, BCHP_MISC1_Y_RX_ERROR_STATUS);
	uv_err_sts = hw->pfnReadDevRegister(hw->adp, BCHP_MISC1_HIF_RX_ERROR_STATUS);

	for (i = 0; i < DMA_ENGINE_CNT; i++) {
		/* Update States..*/
		if (i == 0)
			ret = crystalhd_flea_rx_list0_handler(hw, intr_sts, y_err_sts, uv_err_sts);
		else
			ret = crystalhd_flea_rx_list1_handler(hw, intr_sts, y_err_sts, uv_err_sts);
		if (ret) {
			switch (hw->rx_list_sts[i]) {
				case sts_free:
					comp_sts = BC_STS_SUCCESS;
					list_avail = 1;
					hw->stats.rx_success++;
					break;
				case rx_y_error:
				case rx_uv_error:
				case rx_sts_error:
					/* We got error on both or Y or uv. */
					hw->stats.rx_errors++;
					hw->pfnHWGetDoneSize(hw, i, &y_dn_sz, &uv_dn_sz);
					dev_info(&hw->adp->pdev->dev, "list_index:%x "
					"rx[%d] rxtot[%d] Y:%x UV:%x Int:%x YDnSz:%x "
					"UVDnSz:%x\n", i, hw->stats.rx_errors,
							 hw->stats.rx_errors + hw->stats.rx_success,
							 y_err_sts, uv_err_sts, intr_sts.WholeReg,
							 y_dn_sz, uv_dn_sz);
							 hw->rx_list_sts[i] = sts_free;
							 comp_sts = BC_STS_ERROR;
							 break;
				default:
					/* Wait for completion..*/
					comp_sts = BC_STS_NO_DATA;
					break;
			}
		}
		/* handle completion...*/
		if (comp_sts != BC_STS_NO_DATA) {
			crystalhd_rx_receipt_irq(hw, i, receipt_irq_entry_ns,
						 comp_sts);
			if (comp_sts == BC_STS_SUCCESS) {
				/* Done sizes belong to this list until rx_lock drops. */
				crystalhd_rx_pkt_done(hw, i, comp_sts);
			} else {
				completed[i] = crystalhd_rx_pkt_detach(hw, i,
								 comp_sts);
				completion_status[i] = comp_sts;
			}
			comp_sts = BC_STS_NO_DATA;
		}
	}

	spin_unlock_irqrestore(&hw->rx_lock, flags);

	for (i = 0; i < DMA_ENGINE_CNT; i++) {
		if (completed[i])
			crystalhd_rx_pkt_complete(hw, completed[i], i,
						  completion_status[i]);
	}

	if (list_avail)
		crystalhd_hw_start_capture(hw);
}

bool crystalhd_flea_hw_interrupt_handle(struct crystalhd_adp *adp, struct crystalhd_hw *hw)
{
	union FLEA_INTR_BITS_COMMON	IntrStsValue;
	bool				bIntFound		= false;
	bool				bPostRxBuff		= false;
	bool				bSomeCmdDone	= false;
	struct crystalhd_rx_dma_pkt *rx_pkt;
	unsigned long power_flags;
	bool irq_power_owner = false;

	bool	rc = false;

	if (!adp || !hw->dev_started)
		return rc;
	if (READ_ONCE(hw->dma_fault))
		return crystalhd_hw_ack_fault_interrupt(hw);

	IntrStsValue.WholeReg=0;

	IntrStsValue.WholeReg = hw->pfnReadDevRegister(hw->adp, BCHP_INTR_INTR_STATUS);

	if(!IntrStsValue.WholeReg)
		return rc;	/*Not Our interrupt*/

	/*If any of the bit is set we have a problem*/
	if(IntrStsValue.HaltIntr || IntrStsValue.PcieTgtCaAttn || IntrStsValue.PcieTgtUrAttn)
	{
		printk("Bad HW Error in CrystalHD Driver\n");
		return rc;
	}

	/* Our interrupt */
	hw->stats.num_interrupts++;
	rc = true;

	/* NAREN When In Power Down state, only interrupts possible are TXFIFO and PiQ       */
	/* Save the state of these interrupts to process them when we resume from power down */
	if (READ_ONCE(hw->FleaEnablePWM))
	{
		spin_lock_irqsave(&hw->lock, power_flags);
		if (READ_ONCE(hw->dma_fault) || hw->FleaPowerState == FLEA_PS_FAULT) {
			spin_unlock_irqrestore(&hw->lock, power_flags);
			return crystalhd_hw_ack_fault_interrupt(hw);
		}
		if (hw->FleaPowerState != FLEA_PS_LP_COMPLETE) {
			/* List retirement precedes later firmware notifications. */
			hw->FleaIrqActive++;
			irq_power_owner = true;
			spin_unlock_irqrestore(&hw->lock, power_flags);
			goto awake;
		}
		if(IntrStsValue.ArmMbox1Int)
		{
			hw->PwrDwnPiQIntr = true;
			bIntFound = true;
		}

		if(IntrStsValue.ArmMbox2Int)
		{
			hw->PwrDwnTxIntr = true;
			bIntFound = true;
		}

		/*Write End Of Interrupt for PCIE*/
		if(bIntFound)
		{
			hw->pfnWriteDevRegister(hw->adp, BCHP_INTR_INTR_CLR_REG, IntrStsValue.WholeReg);
			hw->pfnWriteDevRegister(hw->adp, BCHP_INTR_EOI_CTRL, 1);
		}
		spin_unlock_irqrestore(&hw->lock, power_flags);
		return (bIntFound);
	}

awake:
	/*
	-- Arm Mail box Zero interrupt is
	-- BCHP_ARMCR4_BRIDGE_REG_MBOX_ARM1
	*/
	if(IntrStsValue.ArmMbox0Int)
	{
		/*HWFWCmdComplete(pHWExt,IntrBmp); */
		bIntFound = true;
		bSomeCmdDone = true;
	}

	/* Rx interrupts */
	crystalhd_flea_rx_isr(hw, IntrStsValue);

	if( IntrStsValue.L0YRxDMADone || IntrStsValue.L1YRxDMADone || IntrStsValue.L0UVRxDMADone || IntrStsValue.L1UVRxDMADone || IntrStsValue.L0YRxDMAErr || IntrStsValue.L1YRxDMAErr )
	{
		bSomeCmdDone = true;
	}


	/* Tx interrupts*/
	crystalhd_flea_tx_isr(hw, IntrStsValue);

	/*
	-- Indicate the TX Done to Flea Firmware.
	*/
	if(IntrStsValue.L0TxDMADone || IntrStsValue.L1TxDMADone || IntrStsValue.L0TxDMAErr || IntrStsValue.L1TxDMAErr)
	{
		crystalhd_flea_update_tx_done_to_fw(hw);
		bSomeCmdDone = true;
	}
	/*
	-- We are doing this here because we processed the interrupts.
	-- We might want to change the PicQSts bitmap in any of the interrupts.
	-- This should be done before trying to post the next RX buffer.
	-- NOTE: ArmMbox1Int is BCHP_ARMCR4_BRIDGE_REG_MBOX_ARM2
	*/
	if(IntrStsValue.ArmMbox1Int)
	{
		/*pHWExt->FleaBmpIntrCnt++; */
		crystalhd_flea_update_temperature(hw);
		crystalhd_flea_handle_PicQSts_intr(hw);
		bPostRxBuff = true;
		bIntFound = true;
	}

	if(IntrStsValue.ArmMbox2Int)
	{
		crystalhd_flea_update_temperature(hw);
		crystalhd_flea_update_tx_buff_info(hw);
		bIntFound = true;
	}

	/*Write End Of Interrupt for PCIE*/
	if(rc)
	{
		hw->pfnWriteDevRegister(hw->adp, BCHP_INTR_INTR_CLR_REG, IntrStsValue.WholeReg);
		hw->pfnWriteDevRegister(hw->adp, BCHP_INTR_EOI_CTRL, 1);
	}
	/* Publish completion only after acknowledging the device. A late response
	 * retires its power count, but quarantine remains until verified reset.
	 */
	if (IntrStsValue.ArmMbox0Int)
		crystalhd_hw_fw_cmd_complete(hw);

	/* Try to post RX Capture buffer from ISR context */
	if(bPostRxBuff) {
		rx_pkt = crystalhd_hw_fetch_free_rx_pkt(hw);
		if (rx_pkt)
			crystalhd_hw_repost_cap_buffer(hw, rx_pkt);
	}

	if (irq_power_owner) {
		spin_lock_irqsave(&hw->lock, power_flags);
		hw->FleaIrqActive--;
		spin_unlock_irqrestore(&hw->lock, power_flags);
	}
	if ((READ_ONCE(hw->FleaPowerState) == FLEA_PS_LP_PENDING ||
	     (irq_power_owner && READ_ONCE(hw->hw_pause_issued))) &&
	    (bSomeCmdDone || irq_power_owner))
	{
		/*printk("interrupt_handle: current PS:%d, bSomeCmdDone%d\n", hw->FleaPowerState,bSomeCmdDone); */
		crystalhd_flea_set_next_power_state(hw, FLEA_EVT_CMD_COMP);
	}

	/* NAREN place the device in low power mode if we have not started playing video */
	/*if((hw->FleaPowerState == FLEA_PS_ACTIVE) && (hw->WakeUpDecodeDone != true)) */
	/*{ */
	/*	if((hw->ReadyListLen == 0) && (hw->FreeListLen == 0)) */
	/*	{ */
	/*		crystalhd_flea_set_next_power_state(hw, FLEA_EVT_FLL_CHANGE); */
	/*		printk("ISR Idle\n"); */
	/*	} */
	/*} */

	return rc;
}

static BC_STATUS flea_rx_read(struct crystalhd_rx_buffer *buffer,
			      uint32_t limit, uint32_t offset,
			      void *dst, size_t size)
{
	if (offset > limit || size > limit - offset)
		return BC_STS_INV_ARG;

	return crystalhd_rx_buffer_read(buffer, offset, dst, size);
}

static BC_STATUS flea_rx_write(struct crystalhd_rx_buffer *buffer,
			       uint32_t limit, uint32_t offset,
			       const void *src, size_t size)
{
	if (offset > limit || size > limit - offset)
		return BC_STS_INV_ARG;

	return crystalhd_rx_buffer_write(buffer, offset, src, size);
}

/* This function cannot be called from ISR context since buffer I/O may sleep. */
static bool flea_get_picture_info(struct crystalhd_hw *hw,
	struct crystalhd_rx_dma_pkt *rx_pkt, uint32_t *PicNumber,
	uint64_t *PicMetaData, bool prepare_output)
{
	struct device *dev = &hw->adp->pdev->dev;
	struct crystalhd_rx_buffer *buffer;
	uint32_t PicInfoLineNum = 0, offset = 0, size = 0;
	uint32_t y_capacity = 0, y_done_bytes = 0, row_width = 0;
	PBC_PIC_INFO_BLOCK pPicInfoLine;
	uint32_t scratch_word, widthField = 0;
	uint64_t offset64;
	unsigned int pixel_stride;
	BC_STATUS sts = BC_STS_SUCCESS;
	bool rtVal = true;
	const char *reason = "missing-buffer";
	union {
		BC_PIC_INFO_BLOCK pib[2];
		uint8_t bytes[2 * sizeof(BC_PIC_INFO_BLOCK) + 16];
	} pic_info;

	*PicNumber = 0;
	*PicMetaData = 0;

	if (rx_pkt)
		memset(&rx_pkt->metadata, 0, sizeof(rx_pkt->metadata));

	if (!rx_pkt || !rx_pkt->buffer)
		goto getpictureinfo_err;
	buffer = rx_pkt->buffer;
	reason = "output-format";
	switch (buffer->output_format) {
	case MODE420:
		pixel_stride = 1;
		break;
	case MODE422_YUY2:
	case MODE422_UYVY:
		pixel_stride = 2;
		break;
	default:
		goto getpictureinfo_err;
	}

	reason = "y-capacity";
	if (buffer->uv_offset > buffer->capacity)
		goto getpictureinfo_err;
	y_capacity = buffer->uv_offset ? buffer->uv_offset : buffer->capacity;
	reason = "done-size";
	if (rx_pkt->y_done_sz > y_capacity / 4 || rx_pkt->y_done_sz < 2)
		goto getpictureinfo_err;
	y_done_bytes = rx_pkt->y_done_sz * 4;
	crystalhd_rx_buffer_sync_for_cpu(hw->adp, buffer);

	offset = y_done_bytes - PIC_PIB_DATA_OFFSET_FROM_END;
	reason = "pib-line-read";
	sts = flea_rx_read(buffer, y_done_bytes, offset, &PicInfoLineNum,
			   sizeof(PicInfoLineNum));
	if (sts != BC_STS_SUCCESS)
		goto getpictureinfo_err;
	reason = "pib-line-range";
	if (PicInfoLineNum > 1092) {
		dev_err(dev, "Invalid Line Number[%x], DoneSz:0x%x Bytes\n",
			(int)PicInfoLineNum, y_done_bytes);
		goto getpictureinfo_err;
	}

	offset = y_done_bytes - PIC_WIDTH_OFFSET_FROM_END;
	reason = "width-read";
	sts = flea_rx_read(buffer, y_done_bytes, offset, &widthField,
			   sizeof(widthField));
	if (sts != BC_STS_SUCCESS)
		goto getpictureinfo_err;

	row_width = widthField & 0x3FFFFFFF;
	reason = "row-width";
	if (!row_width || row_width > 2048) {
		dev_err(dev, "Invalid width [%d]\n", row_width);
		goto getpictureinfo_err;
	}
	hw->PICWidth = row_width;

	size = pixel_stride * sizeof(BC_PIC_INFO_BLOCK);
	offset64 = (uint64_t)PicInfoLineNum * row_width * pixel_stride + 4;
	/* Flea can report completion at the trailer embedded within a packed
	 * PIB. Require its common fields in completed data, but allow extension
	 * bytes through the registered Y plane. Planar buffers stop before UV.
	 */
	reason = "pib-common-range";
	if (offset64 >= y_done_bytes ||
	    pixel_stride * OFFSETOF(BC_PIC_INFO_BLOCK, other) >
		    y_done_bytes - offset64)
		goto getpictureinfo_err;
	offset = offset64;
	reason = "pib-read";
	sts = flea_rx_read(buffer, y_capacity, offset, pic_info.bytes, size);
	if (sts != BC_STS_SUCCESS)
		goto getpictureinfo_err;

	pPicInfoLine = &pic_info.pib[0];
	*PicMetaData = pPicInfoLine->timeStamp;
	reason = "format-width";
	if ((widthField & PIB_FORMAT_CHANGE_BIT) &&
	    (!pPicInfoLine->width || pPicInfoLine->width > 2048))
		goto getpictureinfo_err;

	if (widthField & PIB_EOS_DETECTED_BIT) {
		dev_dbg(dev, "Got EOS flag: original_pic=%u session=%u pts=%llu flags=%x prepare=%u\n",
			pPicInfoLine->picture_number, pPicInfoLine->sess_num,
			(unsigned long long)pPicInfoLine->timeStamp,
			pPicInfoLine->flags, prepare_output);
		hw->DrvEosDetected = 1;
		if (prepare_output) {
			scratch_word = 0xFFFFFFFF;
			reason = "eos-marker-write";
			sts = flea_rx_write(buffer, y_done_bytes, 0,
					    &scratch_word, sizeof(scratch_word));
			if (sts != BC_STS_SUCCESS)
				goto getpictureinfo_err;
		}
	} else {
		if (hw->DrvEosDetected == 1)
			hw->DrvCancelEosFlag = 1;

		hw->DrvEosDetected = 0;
		reason = "first-pixel-read";
		sts = flea_rx_read(buffer, y_done_bytes, 0, &scratch_word,
				   sizeof(scratch_word));
		if (sts != BC_STS_SUCCESS)
			goto getpictureinfo_err;

		pPicInfoLine->ycom = scratch_word;
		if (prepare_output) {
			reason = "pib-write";
			sts = flea_rx_write(buffer, y_capacity, offset,
					    pic_info.bytes, size);
			if (sts != BC_STS_SUCCESS)
				goto getpictureinfo_err;

			scratch_word = PicInfoLineNum;
			reason = "pib-marker-write";
			sts = flea_rx_write(buffer, y_done_bytes, 0,
					    &scratch_word, sizeof(scratch_word));
			if (sts != BC_STS_SUCCESS)
				goto getpictureinfo_err;
		}
	}

	if (widthField & PIB_FORMAT_CHANGE_BIT) {
		rx_pkt->flags = COMP_FLAG_PIB_VALID | COMP_FLAG_FMT_CHANGE;

		rx_pkt->pib.picture_number			= pPicInfoLine->picture_number;
		rx_pkt->pib.width					= pPicInfoLine->width;
		rx_pkt->pib.height					= pPicInfoLine->height;
		rx_pkt->pib.chroma_format			= pPicInfoLine->chroma_format;
		rx_pkt->pib.pulldown				= pPicInfoLine->pulldown;
		rx_pkt->pib.flags					= pPicInfoLine->flags;
		rx_pkt->pib.sess_num				= pPicInfoLine->sess_num;
		rx_pkt->pib.aspect_ratio			= pPicInfoLine->aspect_ratio;
		rx_pkt->pib.colour_primaries		= pPicInfoLine->colour_primaries;
		rx_pkt->pib.picture_meta_payload	= pPicInfoLine->picture_meta_payload;
		rx_pkt->pib.frame_rate				= pPicInfoLine->frame_rate;
		rx_pkt->pib.custom_aspect_ratio_width_height = pPicInfoLine->custom_aspect_ratio_width_height;
		rx_pkt->pib.n_drop				= pPicInfoLine->n_drop;
		rx_pkt->pib.ycom				= pPicInfoLine->ycom;
		hw->PICHeight = rx_pkt->pib.height;
		hw->PICWidth = rx_pkt->pib.width;
		hw->LastPicNo = 0;
		hw->LastTwoPicNo = 0;
		hw->PDRatio = 0;
		hw->PauseThreshold = hw->DefaultPauseThreshold;
		hw->TickSpentInPD = 0;
		hw->TickCntDecodePU = rdtsc_ordered();

		dev_dbg(dev, "[FMT CH] DoneSz:0x%x, PIB:%x %x %x %x %x %x %x %x %x %x\n",
			y_done_bytes, rx_pkt->pib.picture_number,
			rx_pkt->pib.aspect_ratio, rx_pkt->pib.chroma_format,
			rx_pkt->pib.colour_primaries, rx_pkt->pib.frame_rate,
			rx_pkt->pib.height, rx_pkt->pib.width,
			rx_pkt->pib.n_drop, rx_pkt->pib.pulldown,
			rx_pkt->pib.ycom);
		rtVal = false;
	}

	if (pPicInfoLine->flags & FLEA_DECODE_ERROR_FLAG) {
		dev_dbg(dev, "RX firmware decode error: flags=%x picture=%u pts=%llu row=%u line=%u ybytes=%u\n",
			pPicInfoLine->flags, pPicInfoLine->picture_number,
			(unsigned long long)pPicInfoLine->timeStamp, row_width,
			PicInfoLineNum, y_done_bytes);
		*PicNumber = 0;
	} else {
		offset64 = (uint64_t)PicInfoLineNum * row_width * pixel_stride;
		reason = "picture-number-range";
		if (offset64 > y_done_bytes)
			goto getpictureinfo_err;
		offset = offset64;
		reason = "picture-number-read";
		sts = flea_rx_read(buffer, y_done_bytes, offset, PicNumber,
				   sizeof(*PicNumber));
		if (sts != BC_STS_SUCCESS)
			goto getpictureinfo_err;

		/* A status peek must report the sentinel that dequeue would write. */
		if (!prepare_output && (widthField & PIB_EOS_DETECTED_BIT) &&
		    PicInfoLineNum == 0)
			*PicNumber = 0xFFFFFFFF;
	}

	/* Preserve the bounded PIB for opt-in error consumers without turning a
	 * failed picture into a repeat-filter number. Legacy still receives zero.
	 */
	if (prepare_output && rtVal &&
	    ((*PicNumber && !(pPicInfoLine->flags & FLEA_DECODE_ERROR_FLAG)) ||
	     ((pPicInfoLine->flags & FLEA_DECODE_ERROR_FLAG) &&
	      (rx_pkt->flags & COMP_FLAG_DATA_VALID) &&
	      buffer->ops->report_decode_errors))) {
		rx_pkt->metadata.firmware_timestamp = pPicInfoLine->timeStamp;
		rx_pkt->metadata.picture_number =
			(pPicInfoLine->flags & FLEA_DECODE_ERROR_FLAG) ?
			pPicInfoLine->picture_number : *PicNumber;
		rx_pkt->metadata.picture_flags = pPicInfoLine->flags;
		rx_pkt->metadata.picture_width = pPicInfoLine->width;
		rx_pkt->metadata.picture_height = pPicInfoLine->height;
		rx_pkt->metadata.row_width = row_width;
		rx_pkt->metadata.pib_line = PicInfoLineNum;
#ifdef CRYSTALHD_ENABLE_FW_RESEARCH
		rx_pkt->metadata.session_number = pPicInfoLine->sess_num;
#endif
		rx_pkt->metadata.eos_trailer = !!(widthField & PIB_EOS_DETECTED_BIT);
		if (!rx_pkt->metadata.eos_trailer)
			rx_pkt->metadata.first_pixel_word = pPicInfoLine->ycom;
		rx_pkt->metadata.valid = true;
	}

	return rtVal;

getpictureinfo_err:
	dev_dbg(dev, "RX picture parse rejected: reason=%s status=%u row=%u line=%u ybytes=%u capacity=%u offset=%u size=%u widthfield=%x\n",
		reason, sts, row_width, PicInfoLineNum, y_done_bytes, y_capacity,
		offset, size, widthField);
	(void)reason; /* Diagnostic may be compiled out. */
	*PicNumber = 0;
	*PicMetaData = 0;
	return false;
}

bool flea_GetPictureInfo(struct crystalhd_hw *hw,
	struct crystalhd_rx_dma_pkt *rx_pkt, uint32_t *pic_number,
	uint64_t *metadata)
{
	return flea_get_picture_info(hw, rx_pkt, pic_number, metadata, true);
}

uint32_t flea_GetRptDropParam(struct crystalhd_hw *hw, void* pRxDMAReq)
{
	uint32_t PicNumber = 0,result = 0;
	uint64_t PicMetaData = 0;
	struct crystalhd_rx_dma_pkt *rx_pkt = pRxDMAReq;
	bool parsed;

	parsed = flea_GetPictureInfo(hw, rx_pkt, &PicNumber, &PicMetaData);
	crystalhd_rx_receipt_parse(hw, rx_pkt, parsed);
	if (parsed)
		result = PicNumber;

	return result;
}

bool crystalhd_flea_notify_event(struct crystalhd_hw *hw, enum BRCM_EVENT EventCode)
{
	switch(EventCode)
	{
		case BC_EVENT_START_CAPTURE:
		{
			return crystalhd_flea_wake_up_hw(hw);
		}
		default:
			break;
	}

	return true;
}
