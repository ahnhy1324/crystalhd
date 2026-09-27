/********************************************************************
 * Copyright(c) 2006-2009 Broadcom Corporation.
 *
 *  Name: libcrystalhd_int_if.cpp
 *
 *  Description: Driver Internal Interfaces
 *
 *  AU
 *
 *  HISTORY:
 *
 ********************************************************************
 *
 * This file is part of libcrystalhd.
 *
 * This library is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published
 * by the Free Software Foundation, either version 2.1 of the License.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 * You should have received a copy of the GNU Lesser General Public License
 * along with this library.  If not, see <http://www.gnu.org/licenses/>.
 *
 *******************************************************************/

#include "7411d.h"
#include "bc_defines.h"
#include "bc_decoder_regs.h"
#include "libcrystalhd_priv.h"
#include "libcrystalhd_int_if.h"
#include "libcrystalhd_fwcmds.h"

#include <emmintrin.h>

#define SV_MAX_LINE_SZ 128
#define PCI_GLOBAL_CONTROL MISC2_GLOBAL_CTRL
#define PCI_INT_STS_REG MISC2_INTERNAL_STATUS

// FLEA
#define BCHP_MISC2_GLOBAL_CTRL 0x00502100 /* Global Control Register */
#define BCHP_CLK_TEMP_MON_CTRL 0x00070040 /* Temperature monitor control. */
#define BCHP_CLK_TEMP_MON_STATUS 0x00070044 /* Temperature monitor status. */


//===================================Externs ===========================================
DRVIFLIB_INT_API BC_STATUS
DtsGetHwType(
    HANDLE  hDevice,
    uint32_t     *DeviceID,
    uint32_t     *VendorID,
    uint32_t     *HWRev
    )
{
	BC_HW_TYPE *pHWInfo;
	BC_IOCTL_DATA *pIocData = NULL;
	DTS_LIB_CONTEXT		*Ctx = NULL;
	BC_STATUS	sts = BC_STS_SUCCESS;

	DTS_GET_CTX(hDevice,Ctx);

	if(!(pIocData = DtsAllocIoctlData(Ctx)))
		return BC_STS_INSUFF_RES;

	pHWInfo = 	&pIocData->u.hwType;

	pHWInfo->PciDevId = 0xffff;
	pHWInfo->PciVenId =  0xffff;
	pHWInfo->HwRev = 0xff;

	if( (sts=DtsDrvCmd(Ctx,BCM_IOC_GET_HWTYPE,0,pIocData,FALSE)) != BC_STS_SUCCESS){
		DtsRelIoctlData(Ctx,pIocData);
		DebugLog_Trace(LDIL_DBG,"DtsGetHwType: Ioctl failed: %d\n",sts);
		return sts;
	}

	*DeviceID	= 	pHWInfo->PciDevId;
	*VendorID	=	pHWInfo->PciVenId;
	*HWRev		=	pHWInfo->HwRev;

	// Set these early
	Ctx->DevId 	= 	pHWInfo->PciDevId;
	Ctx->hwRevId = 	pHWInfo->HwRev;
	Ctx->VendorId = pHWInfo->PciVenId;

	DtsRelIoctlData(Ctx,pIocData);

	return BC_STS_SUCCESS;
}

DRVIFLIB_INT_API VOID
DtsHwReset(
    HANDLE hDevice
    )
{

	return;
}

DRVIFLIB_INT_API BC_STATUS
DtsSoftReset(
    HANDLE hDevice
    )
{

	uint32_t Val = 0;
	DTS_LIB_CONTEXT		*Ctx = NULL;

	DTS_GET_CTX(hDevice,Ctx);

	if(Ctx->DevId == BC_PCI_DEVID_LINK || Ctx->DevId == BC_PCI_DEVID_DOZER)
	{
		DtsDevRegisterWr( hDevice, DecHt_HostSwReset, 0x00000001);	// Assert c011 soft reset
		bc_sleep_ms(50);
		DtsDevRegisterWr( hDevice, DecHt_HostSwReset, 0x00000000  );	// Release c011 soft reset

		/* Disable Stuffing.. */
		DtsFPGARegisterRead(hDevice,PCI_GLOBAL_CONTROL,&Val);
		Val |= BC_BIT(8);
		DtsFPGARegisterWr(hDevice,PCI_GLOBAL_CONTROL,Val);

		//DtsSetCoreClock(hDevice, 0);
	}
	else if(Ctx->DevId == BC_PCI_DEVID_FLEA)
	{
		// For Link, this is used to bring up 7412 and running.
		// Since the 70012 was running in low power mode, but the 7412 was not.
		// In flea there is no need for this. In general most of the chip will be in idle mode,
		// and should not be needed to reset in order to start up.
		// In Flea, we can never do full chip reset, because that will reset PCIe as well and
		// probably cause a BSOD. Individual blocks will have to be reset, either by asserting true resets
		// (but not from the host) or by re-initializing -like the ARM for example.
	}

	return BC_STS_SUCCESS;
}

static BC_STATUS
DtsProgramLinkColorSpace(HANDLE hDevice, BC_OUTPUT_FORMAT ModeSelect)
{
	uint32_t					Val = 0;
	BC_STATUS sts;

	if (ModeSelect != OUTPUT_MODE420 &&
	    ModeSelect != OUTPUT_MODE422_YUY2 &&
	    ModeSelect != OUTPUT_MODE422_UYVY)
		return BC_STS_INV_ARG;

	DebugLog_Trace(LDIL_DBG,"Setting Color Mode to %u\n", ModeSelect);
	/*
	 * EN_WRITE_ALL BIT -Bit 20
	 * This bit dictates that weather the data will be xferred in
	 * 1 -  UYVY Mode.
	 * 0 - YUY2 Mode.
 	 */
	sts = DtsFPGARegisterRead(hDevice,PCI_GLOBAL_CONTROL,&Val);
	if (sts != BC_STS_SUCCESS)
		return sts;

	if (ModeSelect == OUTPUT_MODE420) {
        Val &= 0xffeeffff;
	} else {
		Val |= BC_BIT(16);
		if(ModeSelect == OUTPUT_MODE422_UYVY) {
			Val |= BC_BIT(20);
		} else {
			Val &= ~BC_BIT(20);
		}
	}

	return DtsFPGARegisterWr(hDevice,PCI_GLOBAL_CONTROL,Val);
}

static BC_STATUS
DtsProgramFleaColorSpace(HANDLE hDevice, BC_OUTPUT_FORMAT ModeSelect)
{
	uint32_t			Val = 0;
	BC_STATUS		sts;

	// Flea HW only support UYVY/YUY2
	if( ModeSelect != OUTPUT_MODE422_UYVY && ModeSelect != OUTPUT_MODE422_YUY2 )
		return BC_STS_INV_ARG;

	sts = DtsDevRegisterRead(hDevice, BCHP_MISC2_GLOBAL_CTRL, &Val);
	if (sts != BC_STS_SUCCESS)
		return sts;

	Val &= 0x0000007c;
	if( ModeSelect == OUTPUT_MODE422_YUY2 )
	{
		Val |= BC_BIT(1);  // bit_1  0-> UYVY, 1-> YUY2
	}

	return DtsDevRegisterWr(hDevice, BCHP_MISC2_GLOBAL_CTRL, Val);
}

DRVIFLIB_INT_API BC_STATUS
DtsSetOutputColorSpace(HANDLE hDevice, BC_OUTPUT_FORMAT mode)
{
	DTS_LIB_CONTEXT *Ctx;
	BC_STATUS sts;

	DTS_GET_CTX(hDevice, Ctx);
	/* Keep the previous capture layout visible until programming succeeds.
	 * thLock is recursive: register helpers also use it for the ioctl pool.
	 * Callers must still select the format before registering capture buffers.
	 */
	DtsLock(Ctx);
	if (Ctx->DevId == BC_PCI_DEVID_LINK)
		sts = DtsProgramLinkColorSpace(hDevice, mode);
	else if (Ctx->DevId == BC_PCI_DEVID_FLEA)
		sts = DtsProgramFleaColorSpace(hDevice, mode);
	else
		sts = BC_STS_NOT_IMPL;
	if (sts == BC_STS_SUCCESS)
		Ctx->b422Mode = mode;
	DtsUnLock(Ctx);
	return sts;
}

/* Retain the existing internal entry points for reapplying a cached mode. */
DRVIFLIB_INT_API BC_STATUS
DtsSetLinkIn422Mode(HANDLE hDevice)
{
	DTS_LIB_CONTEXT *Ctx;
	DTS_GET_CTX(hDevice, Ctx);
	DtsLock(Ctx);
	BC_STATUS sts = DtsProgramLinkColorSpace(hDevice, Ctx->b422Mode);
	DtsUnLock(Ctx);
	return sts;
}

DRVIFLIB_INT_API BC_STATUS
DtsSetFleaIn422Mode(HANDLE hDevice)
{
	DTS_LIB_CONTEXT *Ctx;
	DTS_GET_CTX(hDevice, Ctx);
	DtsLock(Ctx);
	BC_STATUS sts = DtsProgramFleaColorSpace(hDevice, Ctx->b422Mode);
	DtsUnLock(Ctx);
	return sts;
}

DRVIFLIB_INT_API BC_STATUS
DtsGetConfig(
    HANDLE hDevice,
	BC_DTS_CFG *cfg
    )
{
	DTS_LIB_CONTEXT		*Ctx;

	DTS_GET_CTX(hDevice,Ctx);

	if(!cfg){
		return BC_STS_INV_ARG;
	}
	*cfg = Ctx->CfgFlags;

	return BC_STS_SUCCESS;
}

DRVIFLIB_INT_API BC_STATUS
DtsSetConfig(
    HANDLE hDevice,
	BC_DTS_CFG *cfg
    )
{
	DTS_LIB_CONTEXT		*Ctx;

	DTS_GET_CTX(hDevice,Ctx);

	if(!cfg){
		return BC_STS_INV_ARG;
	}

	Ctx->CfgFlags = *cfg;

	return BC_STS_SUCCESS;
}

BC_STATUS
DtsSetCoreClock(
    HANDLE hDevice,
	uint32_t freq
    )
{
//	uint32_t Val=0,clkRate=0, cnt;
	DTS_LIB_CONTEXT		*Ctx;
//	uint32_t DevID,VendorID,Revision;

	uint32_t reg;
	uint32_t n, i;
	uint32_t vco_mg;
	uint32_t refresh_reg;

	DTS_GET_CTX(hDevice,Ctx);
	if(Ctx->DevId != BC_PCI_DEVID_LINK)
	{
		//DebugLog_Trace(LDIL_DBG,"DtsSetCoreClock is not supported in this device\n");
		return BC_STS_ERROR;
	}

#if 0
	if(BC_STS_SUCCESS != DtsGetHwType(hDevice,&DevID,&VendorID,&Revision))	{
		DebugLog_Trace(LDIL_DBG,"Get Hardware Type Failed\n");
		return BC_STS_INV_ARG;
	}

	if(DevID == BC_PCI_DEVID_LINK) {
		// Don't set the core clock
		return BC_STS_SUCCESS;
	}

	if(freq){
		DebugLog_Trace(LDIL_DBG,"DtsSetCoreClock: Custom pll settings not implemented yet.\n");
		return BC_STS_NOT_IMPL;
	}
	if(Ctx->CfgFlags & BC_DEC_VCLK_74MHZ){
		clkRate = 0x000230f0;
	}else if(Ctx->CfgFlags & BC_DEC_VCLK_77MHZ){
		clkRate = 0x000230f2;
	}else{
		return BC_STS_INV_ARG;
	}
#endif
	if(freq == 0)
		return BC_STS_SUCCESS;

	n = freq/5;

	//if (n == Ctx->prev_n)
	//	return BC_STS_CLK_NOCHG;

	if ((n * 27) < 560)
		vco_mg = 0;
	else if ((n * 27) < 900)
		vco_mg = 1;
	else if ((n * 27) < 1030)
		vco_mg = 2;
	else
		vco_mg = 3;

	DtsDevRegisterRead(hDevice,DecHt_PllACtl, &reg);

	reg &= 0xFFFFCFC0;
	reg |= n;
	reg |= vco_mg << 12;

	refresh_reg = (7 * freq / 16);
	DtsDevRegisterWr(hDevice,SDRAM_REF_PARAM,((1 << 12) | refresh_reg));

	DtsDevRegisterWr(hDevice, DecHt_PllACtl, reg);
	DebugLog_Trace(LDIL_DBG,"Clock set to %d\n", freq);
	i = 0;

	while (i < 10) {
		DtsDevRegisterRead(hDevice,DecHt_PllACtl, &reg);

		if (reg & 0x00020000) {
			//Ctx->prev_n = n;
			return BC_STS_SUCCESS;
		}
		else {
			bc_sleep_ms(10);
		}
		i++;
	}

	return BC_STS_ERROR;
}

DRVIFLIB_INT_API BC_STATUS
DtsSetTSMode(
	HANDLE hDevice,
	uint32_t	resv1
	)
{
	uint32_t RegVal = 0;
	DTS_LIB_CONTEXT		*Ctx;
	BOOL TsMode = TRUE;

	DTS_GET_CTX(hDevice,Ctx);

	if(Ctx->DevId != BC_PCI_DEVID_LINK && Ctx->DevId != BC_PCI_DEVID_DOZER)
	{
		DebugLog_Trace(LDIL_DBG,"DtsSetTSMode is not supported in this device\n");
		return BC_STS_ERROR;
	}

	if(Ctx->FixFlags & DTS_LOAD_FILE_PLAY_FW)
		TsMode = FALSE;

	if(TsMode){
		DtsFPGARegisterRead(hDevice,PCI_GLOBAL_CONTROL,&RegVal);
		RegVal &= 0xFFFFFFFE;		//Reset Bit 0
		DtsFPGARegisterWr(hDevice,PCI_GLOBAL_CONTROL,RegVal);
	}else{
		// Set the FPGA up in non TS mode
		DtsFPGARegisterRead(hDevice,PCI_GLOBAL_CONTROL,&RegVal);
		RegVal |= 0x01;		//Set Bit 0
		DtsFPGARegisterWr(hDevice,PCI_GLOBAL_CONTROL,RegVal);
	}

	return BC_STS_SUCCESS;
}

DRVIFLIB_INT_API BC_STATUS
DtsSetProgressive(
	HANDLE hDevice,
	uint32_t resv1
	)
{
	uint32_t			RegVal;
	DTS_LIB_CONTEXT		*Ctx = NULL;

	DTS_GET_CTX(hDevice,Ctx);
	if(Ctx->DevId != BC_PCI_DEVID_LINK && Ctx->DevId != BC_PCI_DEVID_DOZER)
	{
		return BC_STS_SUCCESS;
	}

	// Set the FPGA up in Progressive mode - i.e. 1 vsync/frame
	DtsFPGARegisterRead(hDevice,PCI_GLOBAL_CONTROL,&RegVal);
	RegVal |= 0x10;		//Set Bit 4
	DtsFPGARegisterWr(hDevice,PCI_GLOBAL_CONTROL,RegVal);

	return BC_STS_SUCCESS;
}

BC_STATUS
DtsRstVidClkDLL(
				HANDLE hDevice)
{
	uint32_t RegVal,Cnt=100;

	DtsFPGARegisterRead(hDevice,PCI_GLOBAL_CONTROL,&RegVal);
	RegVal |= 0x08;		//Set Bit 3 the Reset Bit
	DtsFPGARegisterWr(hDevice,PCI_GLOBAL_CONTROL,RegVal);

	//
	// Wait for the bit to go low [Unlock]
	//
	while(Cnt)
	{
		DtsFPGARegisterRead(hDevice,PCI_INT_STS_REG,&RegVal);
		if (!(RegVal & 0x04))
		{
			break;

		}else{
			bc_sleep_ms(100);
			Cnt--;
		}
	}
	bc_sleep_ms(100);
	DtsFPGARegisterRead(hDevice,PCI_GLOBAL_CONTROL,&RegVal);
	RegVal &= 0xfffffff7;	//reset bit 3
	DtsFPGARegisterWr(hDevice,PCI_GLOBAL_CONTROL,RegVal);
	RegVal=0;
	Cnt =100;
	while(Cnt)
	{
		DtsFPGARegisterRead(hDevice,PCI_INT_STS_REG,&RegVal);
		if(RegVal & 0x04)
		{
			//
			// This means that the video clock is locked.
			//
			return BC_STS_SUCCESS;
		}else{
			bc_sleep_ms(100);
			Cnt--;
		}
	}

	DebugLog_Trace(LDIL_DBG,"DtsSetVideoClock: DLL did not lock.\n");
	return BC_STS_ERROR;
}

DRVIFLIB_INT_API BC_STATUS
DtsSetVideoClock(
    HANDLE hDevice,
	uint32_t freq
    )
{
	uint32_t Val=0;
	uint32_t clkRate = 0;
	DTS_LIB_CONTEXT *Ctx;
	uint32_t DevID,VendorID,Revision;

	DTS_GET_CTX(hDevice,Ctx);

	if(freq){
		DebugLog_Trace(LDIL_DBG,"DtsSetVideoClock: Custom pll settings not implemented yet.\n");
		return BC_STS_NOT_IMPL;
	}

	if(BC_STS_SUCCESS != DtsGetHwType(hDevice,&DevID,&VendorID,&Revision)) {
		DebugLog_Trace(LDIL_DBG,"Get Hardware Type Failed\n");
		return BC_STS_INV_ARG;
	}
	if(DevID == BC_PCI_DEVID_LINK || DevID == BC_PCI_DEVID_FLEA) {
		// Don't set the video clock
		return BC_STS_SUCCESS;
	}

	if(Ctx->CfgFlags & BC_DEC_VCLK_74MHZ){
		// Program PLL-E to 75 MHZ (n = 44, m = 10, vco_rng = 1)
		clkRate = 0x000012AC;
	}else if(Ctx->CfgFlags & BC_DEC_VCLK_77MHZ){
		// Program PLL-E to 77 MHZ (n = ??, m = ??, vco_rng = ??)
		clkRate = 0x000012B0;
	}else{
		return BC_STS_INV_ARG;
	}


	DtsDevRegisterWr( hDevice, DecHt_PllDCtl, 0x00010000);	// Bypass PLL-D

	bc_sleep_ms(50);

	DtsDevRegisterRead( hDevice, DecHt_PllDCtl, &Val);

	if(Val != 0x00030000){
		DebugLog_Trace(LDIL_DBG,"DtsSetVideoClock: Failed to change PLL_D_CTL\n");
		//FIX_ME
		//return BC_STS_NO_ACCESS;
	}

	DtsDevRegisterWr( hDevice, DecHt_PllECtl, clkRate);

	bc_sleep_ms(50);

	DtsDevRegisterRead( hDevice, DecHt_PllECtl, &Val);

	if(Val != (clkRate | 0x00020000) ){
		DebugLog_Trace(LDIL_DBG,"DtsSetVideoClock: Failed to change PLL_E_CTL\n");
		//FIX_ME
		//return BC_STS_NO_ACCESS;
	}

	//if(BC_STS_SUCCESS !=  DtsRstVidClkDLL(hDevice))
	//{
	//	DebugLog_Trace(LDIL_DBG,"DtsSetVideoClock: Vid Clk DLL Failed to Lock\n");
	//	return BC_STS_ERROR;
	//}

	return BC_STS_SUCCESS;
}
DRVIFLIB_INT_API BOOL
DtsIsVideoClockSet(HANDLE hDevice)
{
	uint32_t RegVal=0;
	DTS_LIB_CONTEXT *Ctx = NULL;
	uint32_t DevID,VendorID,Revision;

	DTS_GET_CTX(hDevice,Ctx);

	if(BC_STS_SUCCESS != DtsGetHwType(hDevice,&DevID,&VendorID,&Revision)) {
		DebugLog_Trace(LDIL_DBG,"Get Hardware Type Failed\n");
		return FALSE;
	}
	if(DevID == BC_PCI_DEVID_LINK || DevID == BC_PCI_DEVID_FLEA) {
		// Don't set the video clock
		return FALSE;
	}

	if((Ctx->RegCfg.DbgOptions & BC_BIT(1)) && (Ctx->OpMode == DTS_PLAYBACK_MODE))
		return FALSE;

	DtsFPGARegisterRead(hDevice,PCI_GLOBAL_CONTROL,&RegVal);

	if(RegVal & BC_BIT(0))
		return TRUE;

	DtsFPGARegisterRead(hDevice,PCI_INT_STS_REG,&RegVal);

	return (RegVal & BC_BIT(2));

}

DRVIFLIB_INT_API BC_STATUS
DtsGetPciConfigSpace(
    HANDLE 	hDevice,
    uint8_t          *info
    )
{
	BC_IOCTL_DATA *pIocData = NULL;
	BC_PCI_CFG *pciInfo;
	DTS_LIB_CONTEXT		*Ctx = NULL;
	BC_STATUS	sts = BC_STS_SUCCESS;


	DTS_GET_CTX(hDevice,Ctx);

	if(!info)
	{
		DebugLog_Trace(LDIL_DBG,"DtsGetPciConfigSpace: Invlid Arguments\n");
		return BC_STS_ERROR;
	}

	if(!(pIocData = DtsAllocIoctlData(Ctx)))
		return BC_STS_INSUFF_RES;

	pciInfo = (BC_PCI_CFG *)&pIocData->u.pciCfg;

	pciInfo->Size = PCI_CFG_SIZE;
	pciInfo->Offset = 0;
	memset(info,0,	PCI_CFG_SIZE);

	if( (sts=DtsDrvCmd(Ctx,BCM_IOC_RD_PCI_CFG,0,pIocData,FALSE)) != BC_STS_SUCCESS){
		DtsRelIoctlData(Ctx,pIocData);
		DebugLog_Trace(LDIL_DBG,"DtsGetPciConfigSpace: Ioctl failed: %d\n",sts);
		return sts;
	}

	memcpy(info,pciInfo->pci_cfg_space,PCI_CFG_SIZE);

	DtsRelIoctlData(Ctx,pIocData);

	return BC_STS_SUCCESS;
}

DRVIFLIB_INT_API BC_STATUS
DtsDevRegisterRead(
    HANDLE		hDevice,
    uint32_t			offset,
    uint32_t			*Value
    )
{
	BC_IOCTL_DATA *pIocData = NULL;
	BC_CMD_REG_ACC	*reg_acc_read;
	DTS_LIB_CONTEXT		*Ctx = NULL;
	BC_STATUS	sts = BC_STS_SUCCESS;


	DTS_GET_CTX(hDevice,Ctx);

	if(!(pIocData = DtsAllocIoctlData(Ctx)))
		return BC_STS_INSUFF_RES;


	reg_acc_read = (BC_CMD_REG_ACC *) &pIocData->u.regAcc;

	//
	// Prepare the command here.
	//
	reg_acc_read->Offset	= offset;
	reg_acc_read->Value		= 0;

	if( (sts=DtsDrvCmd(Ctx,BCM_IOC_REG_RD,0,pIocData,FALSE)) != BC_STS_SUCCESS){
		DtsRelIoctlData(Ctx,pIocData);
		DebugLog_Trace(LDIL_DBG,"DtsDevRegisterRead: Ioctl failed: %d\n",sts);
		return sts;
	}

	*Value = reg_acc_read->Value;

	DtsRelIoctlData(Ctx,pIocData);

	return BC_STS_SUCCESS;
}


DRVIFLIB_INT_API BC_STATUS
DtsDevRegisterWr(
    HANDLE	hDevice,
    uint32_t		offset,
    uint32_t		Value
    )
{
	BC_CMD_REG_ACC	*reg_acc_wr;
	BC_IOCTL_DATA *pIocData = NULL;
	DTS_LIB_CONTEXT		*Ctx = NULL;
	BC_STATUS	sts = BC_STS_SUCCESS;


	DTS_GET_CTX(hDevice, Ctx);

	if (!(pIocData = DtsAllocIoctlData(Ctx)))
		return BC_STS_INSUFF_RES;

	reg_acc_wr = (BC_CMD_REG_ACC *) &pIocData->u.regAcc;

	// Prepare the command here.
	reg_acc_wr->Offset		= offset;
	reg_acc_wr->Value		= Value;

	sts = DtsDrvCmd(Ctx, BCM_IOC_REG_WR, 0, pIocData, FALSE);

	if (sts != BC_STS_SUCCESS)
		DebugLog_Trace(LDIL_DBG,"DtsDevRegisterWr: Ioctl failed: %d\n", sts);

	DtsRelIoctlData(Ctx,pIocData);

	return sts;
}

DRVIFLIB_INT_API BC_STATUS
DtsFPGARegisterRead(
    HANDLE		hDevice,
    uint32_t			offset,
    uint32_t			*Value
    )
{
	BC_IOCTL_DATA *pIocData = NULL;
	BC_CMD_REG_ACC	*reg_acc_read;
	DTS_LIB_CONTEXT		*Ctx = NULL;
	BC_STATUS	sts = BC_STS_SUCCESS;


	DTS_GET_CTX(hDevice,Ctx);

	if(!(pIocData = DtsAllocIoctlData(Ctx)))
		return BC_STS_INSUFF_RES;


	reg_acc_read = (BC_CMD_REG_ACC *) &pIocData->u.regAcc;

	//
	// Prepare the command here.
	//
	reg_acc_read->Offset	= offset;
	reg_acc_read->Value		= 0;

	if( (sts=DtsDrvCmd(Ctx,BCM_IOC_FPGA_RD,0,pIocData,FALSE)) != BC_STS_SUCCESS){
		DtsRelIoctlData(Ctx,pIocData);
		DebugLog_Trace(LDIL_DBG,"DtsFPGARegisterRead: Ioctl failed: %d\n",sts);
		return sts;
	}

	*Value = reg_acc_read->Value;

	DtsRelIoctlData(Ctx,pIocData);

	return BC_STS_SUCCESS;
}

DRVIFLIB_INT_API BC_STATUS
DtsFPGARegisterWr(
    HANDLE	hDevice,
    uint32_t		offset,
    uint32_t		Value
    )
{
	BC_CMD_REG_ACC	*reg_acc_wr;
	BC_IOCTL_DATA *pIocData = NULL;
	DTS_LIB_CONTEXT		*Ctx = NULL;
	BC_STATUS	sts = BC_STS_SUCCESS;


	DTS_GET_CTX(hDevice,Ctx);

	if(!(pIocData = DtsAllocIoctlData(Ctx)))
		return BC_STS_INSUFF_RES;

	reg_acc_wr = (BC_CMD_REG_ACC *) &pIocData->u.regAcc;

	//
	// Prepare the command here.
	//
	reg_acc_wr->Offset		= offset;
	reg_acc_wr->Value		= Value;

	if( (sts=DtsDrvCmd(Ctx,BCM_IOC_FPGA_WR,0,pIocData,FALSE)) != BC_STS_SUCCESS){
		DtsRelIoctlData(Ctx,pIocData);
		DebugLog_Trace(LDIL_DBG,"DtsFPGARegisterWr: Ioctl failed: %d\n",sts);
		return sts;
	}

	DtsRelIoctlData(Ctx,pIocData);

	return BC_STS_SUCCESS;
}

DRVIFLIB_INT_API BC_STATUS
DtsDevMemRd(
    HANDLE	hDevice,
    uint32_t		*Buffer,
    uint32_t		BuffSz,
    uint32_t		Offset
    )
{
	uint8_t					*pXferBuff;
	uint32_t					size_in_dword;
	BC_IOCTL_DATA		*pIoctlData;
	BC_CMD_DEV_MEM		*pMemAccessRd;
	uint32_t					BytesReturned,AllocSz;

	if(!hDevice)
	{
		DebugLog_Trace(LDIL_DBG,"DtsDevMemRd: Invalid Handle\n");
		return BC_STS_INV_ARG;
	}

	if(!Buffer)
	{
		DebugLog_Trace(LDIL_DBG,"DtsDevMemRd: Null Buffer\n");
		return BC_STS_INV_ARG;
	}

	if(BuffSz % 4)
	{
		DebugLog_Trace(LDIL_DBG,"DtsDevMemRd: Buff Size is not a multiple of DWORD\n");
		return BC_STS_ERROR;
	}


	AllocSz = sizeof(BC_IOCTL_DATA) + (BuffSz);

	pIoctlData = (BC_IOCTL_DATA *) malloc(AllocSz);

	if(!pIoctlData)
	{
		DebugLog_Trace(LDIL_DBG,"DtsDevMemRd: Memory Allocation Failed\n");
		return BC_STS_ERROR;
	}

	pXferBuff = ( ((PUCHAR)pIoctlData) + sizeof(BC_IOCTL_DATA));

	pMemAccessRd =  &pIoctlData->u.devMem;
	size_in_dword = BuffSz / 4;
	pIoctlData->RetSts = BC_STS_ERROR;
	pIoctlData->IoctlDataSz = sizeof(BC_IOCTL_DATA);
	pMemAccessRd->StartOff = Offset;
	memset(pXferBuff,'a',BuffSz);
	/* The size is passed in Bytes*/
	pMemAccessRd->NumDwords = size_in_dword;
	if(!DtsDrvIoctl(hDevice,
					BCM_IOC_MEM_RD,
					pIoctlData,
					AllocSz,
					pIoctlData,
					AllocSz,
					(LPDWORD)&BytesReturned,
					0))
	{
		DebugLog_Trace(LDIL_DBG,"DtsDevMemRd: DeviceIoControl Failed\n");
		free(pIoctlData);
		return BC_STS_ERROR;
	}

	if(BC_STS_ERROR == pIoctlData->RetSts)
	{
		BC_STATUS status = pIoctlData->RetSts;

		DebugLog_Trace(LDIL_DBG,"DtsDevMemRd: IOCTL Cmd Failed By Driver\n");
		free(pIoctlData);
		return status;
	}

	memcpy(Buffer,pXferBuff,BuffSz);

	free(pIoctlData);

	return BC_STS_SUCCESS;
}



DRVIFLIB_INT_API BC_STATUS
DtsDevMemWr(
    HANDLE	hDevice,
    uint32_t		*Buffer,
    uint32_t		BuffSz,
    uint32_t		Offset
    )
{
	uint8_t					*pXferBuff;
	uint32_t					size_in_dword;
	BC_IOCTL_DATA		*pIoctlData;
	BC_CMD_DEV_MEM		*pMemAccessRd;
	uint32_t					BytesReturned,AllocSz;

	//pIoctlData = (BC_IOCTL_DATA *)malloc(sizeof(BC_IOCTL_DATA));


	if(!hDevice)
	{
		DebugLog_Trace(LDIL_DBG,"DtsDevMemWr: Invalid Handle\n");
		return BC_STS_INV_ARG;
	}

	if(!Buffer)
	{
		DebugLog_Trace(LDIL_DBG,"DtsDevMemWr: Null Buffer\n");
		return BC_STS_INV_ARG;
	}

	if(BuffSz % 4)
	{
		DebugLog_Trace(LDIL_DBG,"DtsDevMemWr: Buff Size is not a multiple of DWORD\n");
		return BC_STS_ERROR;
	}


	AllocSz = sizeof(BC_IOCTL_DATA) + (BuffSz);

	pIoctlData = (BC_IOCTL_DATA *) malloc(AllocSz);

	if(!pIoctlData)
	{
		DebugLog_Trace(LDIL_DBG,"DtsDevMemWr: Memory Allocation Failed\n");
		return BC_STS_ERROR;
	}

	pXferBuff = ( ((PUCHAR)pIoctlData) + sizeof(BC_IOCTL_DATA));

	pMemAccessRd =  &pIoctlData->u.devMem;
	size_in_dword = BuffSz / 4;
	pIoctlData->RetSts = BC_STS_ERROR;
	pIoctlData->IoctlDataSz = sizeof(BC_IOCTL_DATA);
	pMemAccessRd->StartOff = Offset;
	memcpy(pXferBuff,Buffer,BuffSz);
	/* The size is passed in Bytes*/
	pMemAccessRd->NumDwords = size_in_dword;
	if(!DtsDrvIoctl(hDevice,
					BCM_IOC_MEM_WR,
					pIoctlData,
					AllocSz,
					pIoctlData,
					AllocSz,
					(LPDWORD)&BytesReturned,
					FALSE))
	{
		DebugLog_Trace(LDIL_DBG,"DtsDevMemWr: DeviceIoControl Failed\n");
		free(pIoctlData);
		return BC_STS_ERROR;
	}

	if(BC_STS_ERROR == pIoctlData->RetSts)
	{
		BC_STATUS status = pIoctlData->RetSts;

		DebugLog_Trace(LDIL_DBG,"DtsDevMemWr: IOCTL Cmd Failed By Driver\n");
		free(pIoctlData);
		return status;
	}

	free(pIoctlData);

	return BC_STS_SUCCESS;

}

DRVIFLIB_INT_API BC_STATUS
DtsTxDmaText( HANDLE  hDevice ,
				 uint8_t *pUserData,
				 uint32_t ulSizeInBytes,
				 uint32_t *dramOff,
				 uint8_t Encrypted)
{
	BC_STATUS status = BC_STS_SUCCESS;
	uint32_t		ulDmaSz;
	uint8_t		*pDmaBuff;

	DTS_LIB_CONTEXT		*Ctx = NULL;
	BC_IOCTL_DATA		*pIocData = NULL;

	DTS_GET_CTX(hDevice,Ctx);

	if( (!pUserData) || (!ulSizeInBytes) || !dramOff)
	{
		return BC_STS_INV_ARG;
	}

	pDmaBuff = pUserData;
	ulDmaSz = ulSizeInBytes;

	if(!(pIocData = DtsAllocIoctlData(Ctx)))
		return BC_STS_INSUFF_RES;

	pIocData->RetSts = BC_STS_ERROR;
	pIocData->IoctlDataSz = sizeof(BC_IOCTL_DATA);
	pIocData->u.ProcInput.DramOffset =0;
	pIocData->u.ProcInput.pDmaBuff = pDmaBuff;
	pIocData->u.ProcInput.BuffSz = ulDmaSz;
	pIocData->u.ProcInput.Mapped = FALSE;
	pIocData->u.ProcInput.Encrypted = Encrypted;
	if(Ctx->VidParams.VideoAlgo == BC_VID_ALGO_VC1MP)
		pIocData->u.ProcInput.Encrypted|=0x2;


	status = DtsDrvCmd(Ctx,BCM_IOC_PROC_INPUT,1,pIocData,FALSE);

	*dramOff = pIocData->u.ProcInput.DramOffset;

	if( BC_STS_SUCCESS != status && BC_STS_IO_USER_ABORT != status)
	{
		DebugLog_Trace(LDIL_DBG,"DtsTxDmaText: DeviceIoControl Failed with Sts %d\n", status);
	}

	DtsRelIoctlData(Ctx,pIocData);

	DumpInputSampleToFile(pUserData,ulSizeInBytes);

	return status;
}

DRVIFLIB_INT_API BC_STATUS
DtsCancelProcOutput(
    HANDLE  hDevice,
	PVOID	Context)
{
	DTS_LIB_CONTEXT		*Ctx = NULL;

	DTS_GET_CTX(hDevice,Ctx);

	return DtsCancelFetchOutInt(Ctx);
}


//------------------------------------------------------------------------
// Name: DtsChkYUVSizes
// Description: Check Src/Dst buffer sizes with configured resolution.
//
// Vin:  Strtucture received from HW
// Vout: Structure got from App (Where data need to be copied)
//
//------------------------------------------------------------------------
DRVIFLIB_INT_API BC_STATUS DtsChkYUVSizes(
	DTS_LIB_CONTEXT *Ctx,
	BC_DTS_PROC_OUT *Vout,
	BC_DTS_PROC_OUT *Vin)
{
	if (!Ctx || !Vout || !Vout->Ybuff || !Vin || !Vin->Ybuff){
		return BC_STS_INV_ARG;
	}
	if((!Ctx->b422Mode) && (!Vout->UVbuff || !Vin->UVbuff)){
		return BC_STS_INV_ARG;
	}

	/* Pass on size info irrespective of the status (for debug) */
	Vout->YBuffDoneSz = Vin->YBuffDoneSz;
	Vout->UVBuffDoneSz = Vin->UVBuffDoneSz;

	/* Does Driver qualifies this condition before setting _PIB_VALID flag??..*/
	if( !( Vin->YBuffDoneSz) || ((!Ctx->b422Mode) && (!Vin->UVBuffDoneSz)) ){
		DebugLog_Trace(LDIL_DBG,"DtsChkYUVSizes: Incomplete Transfer\n");
		return BC_STS_IO_XFR_ERROR;
	}
/*
	Let the upper layer take care of this
	if(!(Vin->PoutFlags & BC_POUT_FLAGS_PIB_VALID)){
		DebugLog_Trace(LDIL_DBG,"DtsChkYUVSizes: PIB not Valid\n");
		return BC_STS_IO_XFR_ERROR;
	}*/

	return BC_STS_SUCCESS;
}

/***/

DRVIFLIB_INT_API BC_STATUS
DtsGetDrvStat(
    HANDLE		hDevice,
	BC_DTS_STATS *pDrvStat
    )
{
	BC_IOCTL_DATA *pIocData = NULL;
	BC_DTS_STATS *pIntDrvStat;
	DTS_LIB_CONTEXT		*Ctx = NULL;
	BC_STATUS	sts = BC_STS_SUCCESS;
//	float		fTemperature = 0;

	DTS_GET_CTX(hDevice,Ctx);

	if(!pDrvStat)
	{
		DebugLog_Trace(LDIL_DBG,"DtsGetDrvStat: Invlid Arguments\n");
		return BC_STS_ERROR;
	}

	if(!(pIocData = DtsAllocIoctlData(Ctx)))
		return BC_STS_INSUFF_RES;

	/* Forward the public API's TX-only request, not buffer-size data or its
	 * local hardware/software-size selection (bit31). Keep legacy bit29
	 * behavior unchanged: older drivers lose their stats-only flag when
	 * selecting the VC1 FIFO, so that fix requires a coordinated change.
	 */
	pIocData->u.drvStat.DrvcpbEmptySize =
		pDrvStat->DrvcpbEmptySize & (1U << 30);

	if(Ctx->SingleThreadedAppMode)
		pIocData->u.drvStat.DrvNextMDataPLD = pDrvStat->DrvNextMDataPLD;

	if( (sts=DtsDrvCmd(Ctx,BCM_IOC_GET_DRV_STAT,0,pIocData,FALSE)) != BC_STS_SUCCESS){
		DtsRelIoctlData(Ctx,pIocData);
		DebugLog_Trace(LDIL_DBG,"DtsGetDriveStats: Ioctl failed: %d\n",sts);
		return sts;
	}

	/* DIL counters */
	pIntDrvStat = DtsGetgStats ( );
	//memcpy_s(pDrvStat, 128, pIntDrvStat, 128);
	memcpy(pDrvStat, pIntDrvStat, 128);

	/* Driver counters */
	pIntDrvStat = (BC_DTS_STATS *)&pIocData->u.drvStat;
	pDrvStat->drvRLL = pIntDrvStat->drvRLL;
	pDrvStat->drvFLL = pIntDrvStat->drvFLL;
	pDrvStat->intCount = pIntDrvStat->intCount;
	pDrvStat->pauseCount = pIntDrvStat->pauseCount;
	pDrvStat->DrvIgnIntrCnt = pIntDrvStat->DrvIgnIntrCnt;
	pDrvStat->DrvTotalFrmDropped = pIntDrvStat->DrvTotalFrmDropped;
	pDrvStat->DrvTotalHWErrs = pIntDrvStat->DrvTotalHWErrs;
	pDrvStat->DrvTotalPIBFlushCnt = pIntDrvStat->DrvTotalPIBFlushCnt;
	pDrvStat->DrvTotalFrmCaptured = pIntDrvStat->DrvTotalFrmCaptured;
	pDrvStat->DrvPIBMisses = pIntDrvStat->DrvPIBMisses;
	pDrvStat->DrvPauseTime = pIntDrvStat->DrvPauseTime;
	pDrvStat->DrvRepeatedFrms = pIntDrvStat->DrvRepeatedFrms;
	pDrvStat->TxFifoBsyCnt = pIntDrvStat->TxFifoBsyCnt;
	pDrvStat->pwr_state_change = pIntDrvStat->pwr_state_change;
	pDrvStat->DrvNextMDataPLD = pIntDrvStat->DrvNextMDataPLD;
	pDrvStat->DrvcpbEmptySize = pIntDrvStat->DrvcpbEmptySize;
	pDrvStat->eosDetected = pIntDrvStat->eosDetected;
	pDrvStat->picNumFlags = pIntDrvStat->picNumFlags;


//

	DtsRelIoctlData(Ctx,pIocData);

	return BC_STS_SUCCESS;
}

DRVIFLIB_INT_API BC_STATUS
DtsSetTemperatureMeasure(
    HANDLE			hDevice,
	BOOL			bTurnOn
    )
{
	DTS_LIB_CONTEXT		*Ctx = NULL;
	BC_STATUS	sts = BC_STS_SUCCESS;
	uint32_t	Val = 0;

	DTS_GET_CTX(hDevice,Ctx);
	if( Ctx->DevId != BC_PCI_DEVID_FLEA )
	{
		DebugLog_Trace(LDIL_DBG,"DtsSetTemperatureMeasure Only support for Flea.\n");
		return BC_STS_SUCCESS;
	}

	if( bTurnOn )
	{
		Val = 0x3; //
		sts = DtsDevRegisterWr(hDevice,BCHP_CLK_TEMP_MON_CTRL,Val);
		bc_sleep_ms(10);

		Val = 0x203; //
		sts = DtsDevRegisterWr(hDevice,BCHP_CLK_TEMP_MON_CTRL,Val);
		bc_sleep_ms(10);
	}
	else
	{
		Val = 0x103; //
		sts = DtsDevRegisterWr(hDevice,BCHP_CLK_TEMP_MON_CTRL,Val);
		bc_sleep_ms(10);
	}
	return sts;
}

DRVIFLIB_INT_API BC_STATUS
DtsGetCoreTemperature(
    HANDLE			hDevice,
	float			*pTemperature
    )
{
	DTS_LIB_CONTEXT		*Ctx = NULL;
	BC_STATUS	sts = BC_STS_ERROR;
	uint32_t	Val = 0;
	*pTemperature = 0;

	DTS_GET_CTX(hDevice,Ctx);
	if( Ctx->DevId != BC_PCI_DEVID_FLEA )
	{
		DebugLog_Trace(LDIL_DBG,"DtsSetTemperatureMeasure Only support for Flea.\n");
		return BC_STS_SUCCESS;
	}

	sts = DtsDevRegisterRead(hDevice,BCHP_CLK_TEMP_MON_STATUS,&Val);
	if( sts != BC_STS_SUCCESS )
		return sts;
	Val = Val & 0x0000ffff;

	*pTemperature = 267.2 - 0.7 * (float)Val;

	return sts;
}


DRVIFLIB_INT_API BC_STATUS
DtsRstDrvStat(
    HANDLE		hDevice
    )
{
	BC_IOCTL_DATA *pIocData = NULL;
	DTS_LIB_CONTEXT		*Ctx = NULL;
	BC_STATUS	sts = BC_STS_SUCCESS;

	DTS_GET_CTX(hDevice,Ctx);

	/* CHECK WHETHER NULL pIocData CAN BE PASSED */
	if(!(pIocData = DtsAllocIoctlData(Ctx)))
		return BC_STS_INSUFF_RES;

	/* Driver related counters */
	if( (sts=DtsDrvCmd(Ctx,BCM_IOC_RST_DRV_STAT,0,pIocData,FALSE)) != BC_STS_SUCCESS){
		DtsRelIoctlData(Ctx,pIocData);
		DebugLog_Trace(LDIL_DBG,"DtsRstDrvStats: Ioctl failed: %d\n",sts);
		return sts;
	}

	/* DIL related counters */
	DtsRstStats( );

	DtsRelIoctlData(Ctx,pIocData);

	return BC_STS_SUCCESS;
}

/**/
/* Get firmware files */
DRVIFLIB_INT_API BC_STATUS
DtsGetFWFiles(
	HANDLE hDevice,
	char *StreamFName,
	char *VDecOuter,
	char *VDecInner
	)
{
	DTS_LIB_CONTEXT		*Ctx = NULL;
	BC_STATUS	sts = BC_STS_SUCCESS;

	DTS_GET_CTX(hDevice,Ctx);
	if (!StreamFName || !VDecOuter || !VDecInner)
		return BC_STS_INV_ARG;

	sts = DtsGetFirmwareFiles(Ctx);
	if(sts == BC_STS_SUCCESS){
		if (snprintf(StreamFName, MAX_PATH, "%s", Ctx->StreamFile) >= MAX_PATH ||
			snprintf(VDecOuter, MAX_PATH, "%s", Ctx->VidOuter) >= MAX_PATH ||
			snprintf(VDecInner, MAX_PATH, "%s", Ctx->VidInner) >= MAX_PATH)
			return BC_STS_ERROR;
	}else{
		return sts;
	}

	return sts;
}
/**/

DRVIFLIB_INT_API BC_STATUS
DtsDownloadFWBin(HANDLE	hDevice, uint8_t *binBuff, uint32_t buffsize, uint32_t dramOffset)
{
	BC_STATUS rstatus = BC_STS_SUCCESS;

	/* Write bootloader vector table section */
	rstatus = DtsDevMemWr(hDevice,(uint32_t *)binBuff,buffsize,dramOffset);
	if (BC_STS_SUCCESS != rstatus)
	{
		DebugLog_Trace(LDIL_DBG,"DtsDownloadFWBin: Fw Download Failed\n");
	}

	return rstatus;
}

/* Check the last byte actually touched, not unused padding after the final
 * row. Division avoids overflow even for malformed 32-bit dimensions/stride;
 * SIZE_MAX also prevents unrepresentable pointer arithmetic on i386. */
static bool DtsRawCopyFits(uint64_t available, uint64_t rowBytes,
						uint64_t pitch, uint32_t rows)
{
	if (available > SIZE_MAX)
		available = SIZE_MAX;
	return rows != 0 && rowBytes != 0 && pitch >= rowBytes &&
		pitch <= SIZE_MAX && available >= rowBytes &&
		(uint64_t)(rows - 1) <= (available - rowBytes) / pitch;
}

BC_STATUS
DtsCopyRawDataToOutBuff(DTS_LIB_CONTEXT *Ctx,
						BC_DTS_PROC_OUT *Vout,
						BC_DTS_PROC_OUT *Vin)
{
	BC_STATUS status = DtsChkYUVSizes(Ctx, Vout, Vin);
	if (status != BC_STS_SUCCESS)
		return status;

	/* DtsChkYUVSizes preserves transfer metadata but does not validate a
	 * copy layout. YbuffSz/YBuffDoneSz are DWORD counts; StrideSz is extra
	 * destination pixels. The source pitch always comes from hardware, even
	 * when SIZE is absent (zero pitch used to repeat the first source row). */
	const bool field = !Ctx->VidParams.Progressive;
	if (Vin->PicInfo.width == 0 || Vin->PicInfo.height == 0 ||
		(Vin->PicInfo.width & 1) || Ctx->HWOutPicWidth < Vin->PicInfo.width ||
		(field && (Vin->PicInfo.height & 1)))
		return BC_STS_IO_XFR_ERROR;

	const uint32_t width = (Vout->PoutFlags & BC_POUT_FLAGS_SIZE)
		? Vout->PicInfo.width : Vin->PicInfo.width;
	const uint32_t height = (Vout->PoutFlags & BC_POUT_FLAGS_SIZE)
		? Vout->PicInfo.height : Vin->PicInfo.height;
	if (width == 0 || height == 0 || (width & 1) ||
		width > Vin->PicInfo.width || height > Vin->PicInfo.height ||
		(field && (height & 1)))
		return BC_STS_INV_ARG;

	/* Both SIZE and no-SIZE paths copy one field, not a full frame, when
	 * the current hardware picture is interlaced. A caller weaves fields by
	 * offsetting Ybuff and supplying padding for the other field's row. */
	const uint32_t rows = field ? height / 2 : height;
	const uint64_t rowBytes = (uint64_t)width * 2;
	const uint64_t sourcePitch = (uint64_t)Ctx->HWOutPicWidth * 2;
	const uint64_t padding = (Vout->PoutFlags & BC_POUT_FLAGS_STRIDE)
		? Vout->StrideSz : 0;
	const uint64_t destinationPitch = ((uint64_t)width + padding) * 2;
	if (!DtsRawCopyFits((uint64_t)Vin->YBuffDoneSz * 4, rowBytes, sourcePitch, rows) ||
		!DtsRawCopyFits((uint64_t)Vout->YbuffSz * 4, rowBytes, destinationPitch, rows))
		return BC_STS_IO_XFR_ERROR;

	for (uint32_t y = 0; y < rows; ++y)
		memcpy(Vout->Ybuff + (size_t)y * (size_t)destinationPitch,
			Vin->Ybuff + (size_t)y * (size_t)sourcePitch, (size_t)rowBytes);
	return BC_STS_SUCCESS;
}
/* Non-MODE planar copies share NV12 source geometry. YV12 stores V then U
 * in the single UV buffer; its U plane follows all V rows, including the
 * final V row's padding. Source transfer metadata remains in DWORD units.
 */
struct DtsPlanarCopyLayout {
	uint32_t width, rows, uvRows;
	uint64_t sourcePitch, yPitch, uvPitch, uOffset;
};

static BC_STATUS DtsPlanarCopyCheck(DTS_LIB_CONTEXT *Ctx,
	BC_DTS_PROC_OUT *Vout, BC_DTS_PROC_OUT *Vin, bool yv12,
	DtsPlanarCopyLayout *layout)
{
	BC_STATUS status = DtsChkYUVSizes(Ctx, Vout, Vin);
	if (status != BC_STS_SUCCESS)
		return status;
	if (Ctx->b422Mode != OUTPUT_MODE420_NV12)
		return BC_STS_INV_ARG;

	const bool field = !Ctx->VidParams.Progressive;
	if (!Vin->PicInfo.width || !Vin->PicInfo.height ||
		(Vin->PicInfo.width & 1) || Ctx->HWOutPicWidth < Vin->PicInfo.width ||
		(field && (Vin->PicInfo.height & 1)))
		return BC_STS_IO_XFR_ERROR;
	const uint32_t width = (Vout->PoutFlags & BC_POUT_FLAGS_SIZE)
		? Vout->PicInfo.width : Vin->PicInfo.width;
	const uint32_t height = (Vout->PoutFlags & BC_POUT_FLAGS_SIZE)
		? Vout->PicInfo.height : Vin->PicInfo.height;
	if (!width || !height || (width & 1) ||
		width > Vin->PicInfo.width || height > Vin->PicInfo.height ||
		(field && (height & 1)))
		return BC_STS_INV_ARG;

	layout->width = width;
	layout->rows = field ? height / 2 : height;
	layout->uvRows = (uint32_t)(((uint64_t)layout->rows + 1) / 2);
	layout->sourcePitch = Ctx->HWOutPicWidth;
	const uint64_t paddingY = (Vout->PoutFlags & BC_POUT_FLAGS_STRIDE)
		? Vout->StrideSz : 0;
	/* Retain the legacy default: YV12's chroma padding is half Y padding;
	 * an explicit UV stride overrides it independently of the Y layout. */
	const uint64_t paddingUV = (Vout->PoutFlags & BC_POUT_FLAGS_STRIDE_UV)
		? Vout->StrideSzUV : (yv12 ? paddingY / 2 : paddingY);
	const uint64_t uvBytes = yv12 ? width / 2 : width;
	layout->yPitch = (uint64_t)width + paddingY;
	layout->uvPitch = uvBytes + paddingUV;
	layout->uOffset = 0;
	uint64_t uvAvailable = (uint64_t)Vout->UVbuffSz * 4;
	if (uvAvailable > SIZE_MAX)
		uvAvailable = SIZE_MAX;

	/* Validate all source and destination planes before writing any pixels.
	 * Final U/NV12 padding is unused, but the V-to-U plane gap is required. */
	if (!DtsRawCopyFits((uint64_t)Vin->YBuffDoneSz * 4,
			width, layout->sourcePitch, layout->rows) ||
		!DtsRawCopyFits((uint64_t)Vin->UVBuffDoneSz * 4,
			width, layout->sourcePitch, layout->uvRows) ||
		!DtsRawCopyFits((uint64_t)Vout->YbuffSz * 4,
			width, layout->yPitch, layout->rows) ||
		!DtsRawCopyFits(uvAvailable, uvBytes, layout->uvPitch, layout->uvRows))
		return BC_STS_IO_XFR_ERROR;
	if (yv12) {
		/* The successful first-plane check bounds this product by the
		 * available bytes plus one row's padding, including on i386. */
		layout->uOffset = layout->uvPitch * layout->uvRows;
		if (layout->uOffset > uvAvailable ||
			!DtsRawCopyFits(uvAvailable - layout->uOffset,
				uvBytes, layout->uvPitch, layout->uvRows))
			return BC_STS_IO_XFR_ERROR;
	}
	return BC_STS_SUCCESS;
}

static BC_STATUS DtsCopyPlanar(DTS_LIB_CONTEXT *Ctx, BC_DTS_PROC_OUT *Vout,
	BC_DTS_PROC_OUT *Vin, bool yv12)
{
	DtsPlanarCopyLayout layout;
	BC_STATUS status = DtsPlanarCopyCheck(Ctx, Vout, Vin, yv12, &layout);
	if (status != BC_STS_SUCCESS)
		return status;
	for (uint32_t y = 0; y < layout.rows; ++y)
		memcpy(Vout->Ybuff + (size_t)y * (size_t)layout.yPitch,
			Vin->Ybuff + (size_t)y * (size_t)layout.sourcePitch, layout.width);
	for (uint32_t y = 0; y < layout.uvRows; ++y) {
		const uint8_t *src = Vin->UVbuff + (size_t)y * (size_t)layout.sourcePitch;
		uint8_t *dst = Vout->UVbuff + (size_t)y * (size_t)layout.uvPitch;
		if (yv12) {
			uint8_t *dstU = dst + (size_t)layout.uOffset;
			for (uint32_t x = 0; x < layout.width; x += 2) {
				dst[x / 2] = src[x + 1];
				dstU[x / 2] = src[x];
			}
		} else {
			memcpy(dst, src, layout.width);
		}
	}
	return BC_STS_SUCCESS;
}

BC_STATUS DtsCopyNV12ToYV12(DTS_LIB_CONTEXT *Ctx, BC_DTS_PROC_OUT *Vout,
	BC_DTS_PROC_OUT *Vin)
{
	return DtsCopyPlanar(Ctx, Vout, Vin, true);
}

BC_STATUS DtsCopyNV12(DTS_LIB_CONTEXT *Ctx, BC_DTS_PROC_OUT *Vout,
	BC_DTS_PROC_OUT *Vin)
{
	return DtsCopyPlanar(Ctx, Vout, Vin, false);
}

/* MODE conversions use byte padding, unlike the raw packed-copy API.
 * Keep vector loads within the visible row and use ordinary stores so the
 * caller can consume the completed picture without a streaming-store fence.
 */
static void DtsSwapPackedRow(uint8_t *dst, const uint8_t *src, size_t bytes)
{
	size_t x = 0;
#if defined(__SSE2__)
	for (; bytes - x >= 16; x += 16) {
		const __m128i value = _mm_loadu_si128((const __m128i *)(src + x));
		_mm_storeu_si128((__m128i *)(dst + x),
			_mm_or_si128(_mm_srli_epi16(value, 8), _mm_slli_epi16(value, 8)));
	}
#endif
	for (; x < bytes; x += 2) {
		dst[x] = src[x + 1];
		dst[x + 1] = src[x];
	}
}

static void DtsPackedToNV12Row(uint8_t *dstY, uint8_t *dstUV,
							const uint8_t *src, uint32_t width, bool uyvy)
{
	uint32_t x = 0;
#if defined(__SSE2__)
	const __m128i mask = _mm_set1_epi16(0x00ff);
	for (; width - x >= 16; x += 16) {
		const __m128i a = _mm_loadu_si128((const __m128i *)(src + (size_t)x * 2));
		const __m128i b = _mm_loadu_si128((const __m128i *)(src + (size_t)x * 2 + 16));
		const __m128i low = _mm_packus_epi16(_mm_and_si128(a, mask), _mm_and_si128(b, mask));
		const __m128i high = _mm_packus_epi16(_mm_srli_epi16(a, 8), _mm_srli_epi16(b, 8));
		_mm_storeu_si128((__m128i *)(dstY + x), uyvy ? high : low);
		if (dstUV)
			_mm_storeu_si128((__m128i *)(dstUV + x), uyvy ? low : high);
	}
#endif
	const unsigned yOffset = uyvy ? 1 : 0;
	for (; x < width; ++x) {
		dstY[x] = src[(size_t)x * 2 + yOffset];
		if (dstUV)
			dstUV[x] = src[(size_t)x * 2 + (yOffset ^ 1)];
	}
}

static void DtsNV12ToPackedRow(uint8_t *dst, const uint8_t *srcY,
							const uint8_t *srcUV, const uint8_t *nextUV,
							uint32_t width, bool uyvy)
{
	uint32_t x = 0;
#if defined(__SSE2__)
	for (; width - x >= 16; x += 16) {
		const __m128i y = _mm_loadu_si128((const __m128i *)(srcY + x));
		__m128i uv = _mm_loadu_si128((const __m128i *)(srcUV + x));
		if (nextUV != srcUV)
			uv = _mm_avg_epu8(uv, _mm_loadu_si128((const __m128i *)(nextUV + x)));
		_mm_storeu_si128((__m128i *)(dst + (size_t)x * 2),
			uyvy ? _mm_unpacklo_epi8(uv, y) : _mm_unpacklo_epi8(y, uv));
		_mm_storeu_si128((__m128i *)(dst + (size_t)x * 2 + 16),
			uyvy ? _mm_unpackhi_epi8(uv, y) : _mm_unpackhi_epi8(y, uv));
	}
#endif
	const unsigned yOffset = uyvy ? 1 : 0;
	for (; x < width; ++x) {
		dst[(size_t)x * 2 + yOffset] = srcY[x];
		/* Match SSE2's rounded average, including scalar tail pixels. */
		dst[(size_t)x * 2 + (yOffset ^ 1)] =
			(uint8_t)(((unsigned)srcUV[x] + nextUV[x] + 1) / 2);
	}
}

BC_STATUS DtsCopyFormat(DTS_LIB_CONTEXT *Ctx, BC_DTS_PROC_OUT *Vout,
						BC_DTS_PROC_OUT *Vin)
{
	if (!Ctx || !Vout || !Vin || !Vout->Ybuff || !Vin->Ybuff)
		return BC_STS_INV_ARG;

	/* Preserve the legacy hardware-transfer metadata in DWORD units. */
	Vout->YBuffDoneSz = Vin->YBuffDoneSz;
	Vout->UVBuffDoneSz = Vin->UVBuffDoneSz;
	const BC_OUTPUT_FORMAT source = Ctx->b422Mode;
	const unsigned target = Vout->b422Mode;
	if ((source != OUTPUT_MODE420_NV12 && source != OUTPUT_MODE422_YUY2 &&
		 source != OUTPUT_MODE422_UYVY) ||
		(target != OUTPUT_MODE420_NV12 && target != OUTPUT_MODE422_YUY2 &&
		 target != OUTPUT_MODE422_UYVY))
		return BC_STS_INV_ARG;

	const bool sourcePlanar = source == OUTPUT_MODE420_NV12;
	const bool targetPlanar = target == OUTPUT_MODE420_NV12;
	if ((sourcePlanar && !Vin->UVbuff) || (targetPlanar && !Vout->UVbuff))
		return BC_STS_INV_ARG;

	const bool field = !Ctx->VidParams.Progressive;
	if (!Vin->PicInfo.width || !Vin->PicInfo.height ||
		(Vin->PicInfo.width & 1) || Ctx->HWOutPicWidth < Vin->PicInfo.width ||
		(field && (Vin->PicInfo.height & 1)))
		return BC_STS_IO_XFR_ERROR;

	const uint32_t width = (Vout->PoutFlags & BC_POUT_FLAGS_SIZE)
		? Vout->PicInfo.width : Vin->PicInfo.width;
	const uint32_t height = (Vout->PoutFlags & BC_POUT_FLAGS_SIZE)
		? Vout->PicInfo.height : Vin->PicInfo.height;
	if (!width || !height || (width & 1) ||
		width > Vin->PicInfo.width || height > Vin->PicInfo.height ||
		(field && (height & 1)))
		return BC_STS_INV_ARG;

	const uint32_t rows = field ? height / 2 : height;
	const uint32_t uvRows = (uint32_t)(((uint64_t)rows + 1) / 2);
	const uint64_t sourceBytes = (uint64_t)width * (sourcePlanar ? 1 : 2);
	const uint64_t sourcePitch = (uint64_t)Ctx->HWOutPicWidth * (sourcePlanar ? 1 : 2);
	const uint64_t targetBytes = (uint64_t)width * (targetPlanar ? 1 : 2);
	const uint64_t paddingY = (Vout->PoutFlags & BC_POUT_FLAGS_STRIDE)
		? Vout->StrideSz : 0;
	const uint64_t paddingUV = (Vout->PoutFlags & BC_POUT_FLAGS_STRIDE_UV)
		? Vout->StrideSzUV : paddingY;
	const uint64_t targetPitch = targetBytes + paddingY;
	const uint64_t targetUVPitch = (uint64_t)width + paddingUV;

	/* Validate every touched plane before writing even the first pixel.
	 * The final row need not contain unused trailing padding. */
	if (!DtsRawCopyFits((uint64_t)Vin->YBuffDoneSz * 4,
			sourceBytes, sourcePitch, rows) ||
		!DtsRawCopyFits((uint64_t)Vout->YbuffSz * 4,
			targetBytes, targetPitch, rows) ||
		(sourcePlanar && !DtsRawCopyFits((uint64_t)Vin->UVBuffDoneSz * 4,
			width, Ctx->HWOutPicWidth, uvRows)) ||
		(targetPlanar && !DtsRawCopyFits((uint64_t)Vout->UVbuffSz * 4,
			width, targetUVPitch, uvRows)))
		return BC_STS_IO_XFR_ERROR;

	for (uint32_t y = 0; y < rows; ++y) {
		const uint8_t *srcY = Vin->Ybuff + (size_t)y * (size_t)sourcePitch;
		uint8_t *dstY = Vout->Ybuff + (size_t)y * (size_t)targetPitch;
		if (!sourcePlanar && !targetPlanar) {
			if ((unsigned)source == target)
				memcpy(dstY, srcY, (size_t)targetBytes);
			else
				DtsSwapPackedRow(dstY, srcY, (size_t)targetBytes);
		} else if (!sourcePlanar) {
			/* Retain the legacy even-row chroma decimation. */
			uint8_t *dstUV = (y & 1) ? NULL :
				Vout->UVbuff + (size_t)(y / 2) * (size_t)targetUVPitch;
			DtsPackedToNV12Row(dstY, dstUV, srcY, width,
				source == OUTPUT_MODE422_UYVY);
		} else if (!targetPlanar) {
			const uint8_t *srcUV = Vin->UVbuff + (size_t)(y / 2) * Ctx->HWOutPicWidth;
			const uint8_t *nextUV = srcUV;
			/* Interpolate odd lines within this output/crop, replicating
			 * the final chroma row instead of reading beyond it. */
			if ((y & 1) && y / 2 + 1 < uvRows)
				nextUV += Ctx->HWOutPicWidth;
			DtsNV12ToPackedRow(dstY, srcY, srcUV, nextUV, width,
				target == OUTPUT_MODE422_UYVY);
		} else {
			memcpy(dstY, srcY, width);
			if (!(y & 1))
				memcpy(Vout->UVbuff + (size_t)(y / 2) * (size_t)targetUVPitch,
					Vin->UVbuff + (size_t)(y / 2) * Ctx->HWOutPicWidth, width);
		}
	}
	return BC_STS_SUCCESS;
}



DRVIFLIB_INT_API BC_STATUS
DtsPushFwBinToLink(
	HANDLE hDevice,
    uint32_t *Buffer,
    uint32_t BuffSz)
{
	uint8_t			*pXferBuff;
	BC_IOCTL_DATA	*pIoctlData;
	BC_CMD_DEV_MEM	*pMemAccess;
	uint32_t				BytesReturned, AllocSz;

	if (!hDevice) {
		DebugLog_Trace(LDIL_DBG,"DtsPushFwBinToLink: Invalid Handle\n");
		return BC_STS_INV_ARG;
	}

	if (!Buffer) {
		DebugLog_Trace(LDIL_DBG,"DtsPushFwBinToLink: Null Buffer\n");
		return BC_STS_INV_ARG;
	}

	if (BuffSz % 4) {
		DebugLog_Trace(LDIL_DBG,"DtsPushFwBinToLink: Buff Size is not a multiple of DWORD\n");
		return BC_STS_ERROR;
	}

	AllocSz = sizeof(BC_IOCTL_DATA) + (BuffSz);
	pIoctlData = (BC_IOCTL_DATA *) malloc(AllocSz);
	if(!pIoctlData) {
		DebugLog_Trace(LDIL_DBG,"DtsPushFwBinToLink: Memory Allocation Failed\n");
		return BC_STS_ERROR;
	}
	memset(pIoctlData, 0, AllocSz);
	pXferBuff = ((PUCHAR)pIoctlData) + sizeof(BC_IOCTL_DATA);
	pMemAccess = &pIoctlData->u.devMem;
	pIoctlData->RetSts = BC_STS_ERROR;
	pIoctlData->IoctlDataSz = sizeof(BC_IOCTL_DATA);
	pMemAccess->StartOff = 0;
	pMemAccess->NumDwords = BuffSz/4;
	memcpy(pXferBuff, Buffer, BuffSz);

	if (!DtsDrvIoctl(hDevice, BCM_IOC_FW_DOWNLOAD, pIoctlData, AllocSz, pIoctlData, AllocSz, (LPDWORD)&BytesReturned, 0)) {
		DebugLog_Trace(LDIL_DBG,"DtsPushFwBinToLink: DeviceIoControl Failed\n");
		return BC_STS_ERROR;
	}

	if (BC_STS_ERROR == pIoctlData->RetSts) {
		DebugLog_Trace(LDIL_DBG,"DtsPushFwBinToLink: IOCTL Cmd Failed By Driver\n");
		return pIoctlData->RetSts;
	}

	if(pIoctlData) {
		free(pIoctlData);
	}

	return BC_STS_SUCCESS;
}

/*====================== Debug Routines ========================================*/
void DumpDataToFile(FILE *fp, char *header, uint32_t off, uint8_t *buff, uint32_t dwcount)
{
	uint32_t i, k=1;

#ifndef  _LIB_EN_FWDUMP_
	// Skip FW Download dumping..
	return ;
#endif

	if(!fp)
		return;

	if(header){
		fprintf(fp,"%s\n",header);
	}

	for(i = 0; i < dwcount; i++){
		if (k == 1)
			fprintf(fp, "0x%08X : ", off);

		fprintf(fp," 0x%08X ", *((uint32_t *)buff));

		buff	+= sizeof(uint32_t);
		off		+= sizeof(uint32_t);
		k++;
		if ((i == dwcount - 1) || (k > 4)){
			fprintf(fp,"\n");
			k = 1;
		}
	}
	//fprintf(fp,"\n");

	fflush(fp);
}

void DumpInputSampleToFile(uint8_t *buff, uint32_t buffsize)
{
#ifndef  _LIB_EN_INDUMP_
	return ;
#endif

	static FILE	*pOutputFile=NULL;

	if(!buff || !buffsize){
		if(pOutputFile){
			fclose(pOutputFile);
			pOutputFile = NULL;
		}
		return;
	}

	if(!pOutputFile){
		if(!(pOutputFile = fopen("hdfile_dump.ts","wb")) )
			return;
	}

	fwrite(buff, sizeof(uint8_t), buffsize, pOutputFile);

	fflush(pOutputFile);
}
