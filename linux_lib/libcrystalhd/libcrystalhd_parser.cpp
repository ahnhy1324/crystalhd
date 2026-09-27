/********************************************************************
 * Copyright(c) 2006-2009 Broadcom Corporation.
 *
 *  Name: libcrystalhd_parser.h
 *
 *  Description: Driver Interface library Internal.
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

#include <stdlib.h>
#include "7411d.h"
#include "libcrystalhd_if.h"
#include "libcrystalhd_priv.h"
#include "libcrystalhd_parser.h"

//Check Number of Reference
//NAL Unit
//#include "NALUnit.h"

//Advanced Profile
static const uint8_t b_asf_vc1_frame_scode[4]={0x00, 0x00, 0x01, 0x0D};
//Simple / Main Profile

//SP/MP Code-In Magic Word
static const uint8_t b_asf_vc1_sm_codein_scode[4] = {0x5a, 0x5a, 0x5a, 0x5a};
static const uint8_t b_asf_vc1_sm_codein_data_suffix[1] = {0x0D};
static const uint8_t b_asf_vc1_sm_codein_sl_suffix[1] = {0x0F};
static const uint8_t b_asf_vc1_sm_codein_pts_suffix[1] = {0xBD};

static const uint8_t b_asf_vc1_sm_seqheader_scode[4] = {0x00, 0x00, 0x01, 0x0F};
static const uint8_t b_asf_vc1_sm_picheader_scode[4] = {0x00, 0x00, 0x01, 0x0D};
static const uint8_t b_asf_vc1_sm_codein_header[16] = {0x5a, 0x5a, 0x5a, 0x5a,
	                                                   0x00, 0x00, 0x00, 0x00,
	                                                   0x00, 0x00, 0x00, 0x00,
	                                                   0x5a, 0x5a, 0x5a, 0x5a};


static const uint8_t b_asf_vc1_sm_codein_seqhdr[32] = {0x5a, 0x5a, 0x5a, 0x5a,
	                                                   0x00, 0x00, 0x00, 0x20,
	                                                   0x00, 0x00, 0x00, 0x07,
	                                                   0x5a, 0x5a, 0x5a, 0x5a,
													   0x0f, 0x00, 0x00, 0x00,
	                                                   0x00, 0x00, 0x00, 0x00,
													   0x00, 0x00, 0x00, 0x00,
													   0x00, 0x00, 0x00, 0x00};

static const uint8_t b_asf_vc1_sm_seqhdr[12] = {0x00, 0x00, 0x01, 0x0F,
                                                0x00, 0x00, 0x00, 0x00,
												0x00, 0x00, 0x00, 0x00};

//Swap Data
uint32_t DWORD_SWAP(uint32_t x)
{
	return ((uint32_t)(((x) << 24) | ((x) >> 24) | (((x) & 0xFF00) << 8) | (((x) & 0xFF0000) >> 8)));
}

uint16_t WORD_SWAP(uint16_t x)
{
	return ((uint16_t)(((x) << 8) | ((x) >> 8)));
}

uint64_t ULONGLONG_SWAP(uint64_t x)
{
	return ((uint64_t)(((x) << 56) | ((x) >> 56) | (((x) & 0xFF00LL) << 40) | (((x) & 0xFF000000000000LL) >> 40) | (((x) & 0xFF0000LL) << 24) | (((x) & 0xFF0000000000LL) >> 24) | (((x) & 0xFF000000LL) << 8) | (((x) & 0xFF00000000LL) >> 8)));
}

void PTS2MakerBit5Bytes(uint8_t* pMakerBit, int64_t llPTS)
{
	//4 Bits: '0010'
	//3 Bits: PTS[32:30]
	//1 Bit: '1'
	//15 Bits: PTS[29:15]
	//1 Bit: '1'
	//15 Bits: PTS[14:0]
	//1 Bit : '1'
	//0010 xxx1 xxxx xxxx xxxx xxx1 xxxx xxxx xxxx xxx1

	//Swap
	uint64_t ullSwap = ULONGLONG_SWAP(llPTS);

	uint8_t *pPTS = (uint8_t *)(&ullSwap);

	//Reserved the last 5 bytes, using the last 33 bits of PTS
	//0000 000x xxxx xxxx xxxx xxxx xxxx xxxx xxxx xxxx
	pPTS = pPTS + 3;

	//1st Byte: 0010 xxx1
	*(pMakerBit) = 0x21 | ((*(pPTS) & 0x01) << 3) | ((*(pPTS + 1) & 0xC0) >> 5);

	//2nd Byte: xxxx xxxx
	*(pMakerBit + 1) = ((*(pPTS + 1) & 0x3F) << 2) | ((*(pPTS + 2) & 0xC0) >> 6);

	//3rd Byte: xxxx xxx1
	*(pMakerBit + 2) = 0x01 | ((*(pPTS + 2) & 0x3F) << 2) | ((*(pPTS + 3) & 0x80) >> 6);

	//4th Byte: xxxx xxxx
	*(pMakerBit + 3) = ((*(pPTS + 3) & 0x7F) << 1) | ((*(pPTS + 4) & 0x80) >> 7);

	//5th Byte: xxxx xxx1
	*(pMakerBit + 4) = 0x01 | ((*(pPTS + 4) & 0x7F) << 1);
}

/*
void PESHeaderPTSInsert(uint8_t *pPESHdr, uint64_t llPTS)
{
	//Swap
	uint64_t ullSwap = ULONGLONG_SWAP(llPTS);

	uint8_t *pPTS = (uint8_t *)(&ullSwap);

	//PES Header Bit[8~12] PTS 5Bytes Field
	//0010 xxx1 xxxx xxxx
	//xxxx xxx1 xxxx xxxx
	//xxxx xxx1

}
*/

BC_STATUS DtsReleasePESConverter(HANDLE hDevice)
{
	DTS_LIB_CONTEXT *Ctx = NULL;
	DTS_GET_CTX(hDevice,Ctx);

	if (Ctx->PESConvParams.m_pSpsPpsBuf)
		free(Ctx->PESConvParams.m_pSpsPpsBuf);
	Ctx->PESConvParams.m_pSpsPpsBuf = NULL;
	Ctx->PESConvParams.m_iSpsPpsLen = 0;

	if (Ctx->PESConvParams.pStartcodePendBuff)
		free(Ctx->PESConvParams.pStartcodePendBuff);
	Ctx->PESConvParams.pStartcodePendBuff = NULL;
	Ctx->PESConvParams.lPendBufferSize = 0;

	return BC_STS_SUCCESS;
}

BC_STATUS DtsInitPESConverter(HANDLE hDevice)
{
	DTS_LIB_CONTEXT *Ctx = NULL;
	DTS_GET_CTX(hDevice,Ctx);

	Ctx->PESConvParams.m_bIsFirstByteStreamNALU = true;
	Ctx->PESConvParams.m_pSpsPpsBuf = NULL;
	Ctx->PESConvParams.m_iSpsPpsLen = 0;
	Ctx->PESConvParams.m_lStartCodeDataSize = 0;

	Ctx->PESConvParams.pStartcodePendBuff = NULL;
	Ctx->PESConvParams.lPendBufferSize = 0;

	Ctx->PESConvParams.m_SymbInt.m_nSize = 0;
	Ctx->PESConvParams.m_SymbInt.m_nUsed = 0;
	Ctx->PESConvParams.m_SymbInt.m_pCurrent = NULL;
	Ctx->PESConvParams.m_SymbInt.m_pInputBuffer = NULL;
	Ctx->PESConvParams.m_SymbInt.m_pInputBufferEnd = NULL;
	Ctx->PESConvParams.m_SymbInt.m_ulMask = 0;
	Ctx->PESConvParams.m_SymbInt.m_ulOffset = 0;
	Ctx->PESConvParams.m_SymbInt.m_ulZero = 0;

	Ctx->PESConvParams.m_bAddSpsPps = true;
	Ctx->PESConvParams.m_bIsAdd_SCode_CodeIn = false;
	Ctx->PESConvParams.m_bRangered = false;
	Ctx->PESConvParams.m_bFinterpFlag = false;
	Ctx->PESConvParams.m_bMaxbFrames = false;

	return BC_STS_SUCCESS;

}

BC_STATUS DtsSetVC1SH(HANDLE hDevice)
{
	DTS_LIB_CONTEXT *Ctx = NULL;
	DTS_GET_CTX(hDevice,Ctx);
	const bool advanced = Ctx->VidParams.MediaSubType == BC_MSUBTYPE_WVC1 ||
		Ctx->VidParams.MediaSubType == BC_MSUBTYPE_WMVA;
	if ((!advanced && Ctx->VidParams.MediaSubType != BC_MSUBTYPE_WMV3) ||
		(Ctx->VidParams.MetaDataSz && !Ctx->VidParams.pMetaData) ||
		(!advanced && Ctx->VidParams.MetaDataSz < 4))
		return BC_STS_INV_ARG;

	uint32_t size = Ctx->VidParams.MetaDataSz;
	if (!advanced) {
		if (Ctx->DevId == BC_PCI_DEVID_LINK)
			size = sizeof(b_asf_vc1_sm_codein_seqhdr);
		else if (Ctx->DevId == BC_PCI_DEVID_FLEA)
			size = sizeof(b_asf_vc1_sm_seqhdr);
		else
			return BC_STS_INV_ARG;
	}
	uint8_t *header = NULL;
	if (size && posix_memalign((void **)&header, 8, size))
		return BC_STS_INSUFF_RES;
	if (advanced) {
		if (size)
			memcpy(header, Ctx->VidParams.pMetaData, size);
	} else {
		const bool link = Ctx->DevId == BC_PCI_DEVID_LINK;
		memcpy(header, link ? b_asf_vc1_sm_codein_seqhdr : b_asf_vc1_sm_seqhdr, size);
		const unsigned offset = link ? 17 : 4;
		// Byte stores also cover Link's deliberately unaligned dimensions.
		header[offset] = (uint8_t)(Ctx->VidParams.WidthInPixels >> 8);
		header[offset + 1] = (uint8_t)Ctx->VidParams.WidthInPixels;
		header[offset + 2] = (uint8_t)(Ctx->VidParams.HeightInPixels >> 8);
		header[offset + 3] = (uint8_t)Ctx->VidParams.HeightInPixels;
		memcpy(header + offset + 4, Ctx->VidParams.pMetaData, 4);
	}
	free(Ctx->PESConvParams.m_pSpsPpsBuf);
	Ctx->PESConvParams.m_pSpsPpsBuf = header;
	Ctx->PESConvParams.m_iSpsPpsLen = size;
	return BC_STS_SUCCESS;
}

static unsigned DtsSequenceStartCode(const uint8_t *data, uint32_t size)
{
	if (size >= 4 && data[0] == 0 && data[1] == 0 && data[2] == 0 && data[3] == 1)
		return 4;
	if (size >= 3 && data[0] == 0 && data[1] == 0 && data[2] == 1)
		return 3;
	return 0;
}

// Validate framing and count output before allocation; the second pass copies
// the same bounded spans. No SPS/RBSP semantics are inferred here. Metadata may
// use mixed three/four-byte Annex-B startcodes or two-byte big-endian lengths.
static BC_STATUS DtsConvertSequenceHeaders(const uint8_t *data, uint32_t size,
	bool divx, uint8_t *output, uint32_t capacity, uint32_t *output_size)
{
	const bool annex_b = DtsSequenceStartCode(data, size) != 0;
	uint32_t position = 0, written = 0;
	while (position < size) {
		uint32_t begin, length;
		if (annex_b) {
			const unsigned prefix = DtsSequenceStartCode(data + position, size - position);
			if (!prefix)
				return BC_STS_INV_ARG;
			begin = position + prefix;
			position = begin;
			while (position < size && !DtsSequenceStartCode(data + position, size - position))
				++position;
			length = position - begin;
		} else {
			if (size - position < 2)
				return BC_STS_INV_ARG;
			length = ((uint32_t)data[position] << 8) | data[position + 1];
			position += 2;
			begin = position;
			if (length > size - position)
				return BC_STS_INV_ARG;
			position += length;
		}
		if (!length)
			return BC_STS_INV_ARG;
		const unsigned type = data[begin] & 0x1f;
		if (!divx && type != 7 && type != 8)
			continue;
		if (written > UINT32_MAX - 4 || length > UINT32_MAX - 4 - written)
			return BC_STS_INV_ARG;
		if (output) {
			if (written > capacity || capacity - written < 4 ||
				length > capacity - written - 4)
				return BC_STS_INV_ARG;
			output[written] = output[written + 1] = output[written + 2] = 0;
			output[written + 3] = 1;
			memcpy(output + written + 4, data + begin, length);
		}
		written += 4 + length;
	}
	*output_size = written;
	return BC_STS_SUCCESS;
}

BC_STATUS DtsSetSpsPps(HANDLE hDevice)
{
	DTS_LIB_CONTEXT *Ctx = NULL;
	DTS_GET_CTX(hDevice,Ctx);
	const bool divx = Ctx->VidParams.MediaSubType == BC_MSUBTYPE_DIVX;
	if (Ctx->VidParams.MediaSubType != BC_MSUBTYPE_AVC1 && !divx)
		return BC_STS_SUCCESS;
	if (Ctx->VidParams.MetaDataSz && !Ctx->VidParams.pMetaData)
		return BC_STS_INV_ARG;

	uint32_t size = 0;
	BC_STATUS status = DtsConvertSequenceHeaders(Ctx->VidParams.pMetaData,
		Ctx->VidParams.MetaDataSz, divx, NULL, 0, &size);
	if (status != BC_STS_SUCCESS)
		return status;
	uint8_t *headers = NULL;
	if (size) {
		if (posix_memalign((void **)&headers, 8, size))
			return BC_STS_INSUFF_RES;
		uint32_t copied = 0;
		status = DtsConvertSequenceHeaders(Ctx->VidParams.pMetaData,
			Ctx->VidParams.MetaDataSz, divx, headers, size, &copied);
		if (status != BC_STS_SUCCESS || copied != size) {
			free(headers);
			return status != BC_STS_SUCCESS ? status : BC_STS_INV_ARG;
		}
	}
	free(Ctx->PESConvParams.m_pSpsPpsBuf);
	Ctx->PESConvParams.m_pSpsPpsBuf = headers;
	Ctx->PESConvParams.m_iSpsPpsLen = size;
	return BC_STS_SUCCESS;
}

BC_STATUS DtsSetPESConverter(HANDLE hDevice)
{
	DTS_LIB_CONTEXT *Ctx = NULL;
	DTS_GET_CTX(hDevice,Ctx);
	if ((Ctx->VidParams.MetaDataSz && !Ctx->VidParams.pMetaData) ||
		(Ctx->VidParams.MediaSubType == BC_MSUBTYPE_WMV3 && Ctx->VidParams.MetaDataSz < 4))
		return BC_STS_INV_ARG;

	// Build with no ownership of the previous converter. In particular,
	// DtsInitPESConverter must not overwrite its still-live allocation pointers.
	DTS_LIB_CONTEXT candidate = {};
	candidate.Sig = LIB_CTX_SIG;
	candidate.DevId = Ctx->DevId;
	candidate.VidParams = Ctx->VidParams;
	BC_STATUS status = DtsInitPESConverter(&candidate);
	if (status != BC_STS_SUCCESS)
		return status;
	const BC_MEDIA_SUBTYPE subtype = candidate.VidParams.MediaSubType;
	candidate.PESConvParams.m_bSoftRave = candidate.DevId == BC_PCI_DEVID_FLEA &&
		(subtype == BC_MSUBTYPE_WMV3 || subtype == BC_MSUBTYPE_DIVX ||
		 subtype == BC_MSUBTYPE_DIVX311);

	if (subtype == BC_MSUBTYPE_AVC1 || subtype == BC_MSUBTYPE_H264 || subtype == BC_MSUBTYPE_DIVX) {
		status = DtsSetSpsPps(&candidate);
		if (subtype == BC_MSUBTYPE_AVC1)
			candidate.PESConvParams.m_bIsAdd_SCode_CodeIn = true;
	}
	if (subtype == BC_MSUBTYPE_WVC1 || subtype == BC_MSUBTYPE_WMV3 || subtype == BC_MSUBTYPE_WMVA) {
		candidate.PESConvParams.m_bIsAdd_SCode_CodeIn = true;
		if (subtype == BC_MSUBTYPE_WMV3) {
			uint32_t header;
			memcpy(&header, candidate.VidParams.pMetaData, sizeof(header));
			header = DWORD_SWAP(header);
			candidate.PESConvParams.m_bRangered = (header & 0x00000080) != 0;
			// MAXBFRAMES is a count, not an all-three-bits-set flag.
			candidate.PESConvParams.m_bMaxbFrames = (header & 0x00000070) != 0;
			candidate.PESConvParams.m_bFinterpFlag = (header & 0x00000002) != 0;
		}
		status = DtsSetVC1SH(&candidate);
	}
	if (status != BC_STS_SUCCESS) {
		DtsReleasePESConverter(&candidate);
		return status;
	}
	DtsReleasePESConverter(hDevice);
	Ctx->PESConvParams = candidate.PESConvParams;
	return BC_STS_SUCCESS;
}

BC_STATUS DtsCheckProfile(HANDLE hDevice)
{
/*
	DTS_LIB_CONTEXT *Ctx = NULL;
	DTS_GET_CTX(hDevice,Ctx);

	uint8_t* pSequenceHeader = Ctx->VidParams.pMetaData;
	LONG lSize = Ctx->VidParams.MetaDataSz;

	Ctx->VidParams.NumOfRefFrames = 0;
	Ctx->VidParams.LevelIDC = 0;

	if (Ctx->VidParams.MediaSubType == BC_MSUBTYPE_DIVX && Ctx->DevId != BC_PCI_DEVID_FLEA)
		return BC_STS_ERROR;
	if((Ctx->VidParams.MediaSubType != BC_MSUBTYPE_AVC1) && (Ctx->VidParams.MediaSubType != BC_MSUBTYPE_H264))
		return BC_STS_SUCCESS;
	if (pSequenceHeader == NULL || lSize <= 0)
		return BC_STS_SUCCESS;

	//Get SPS Size
	int iSPSSize = (pSequenceHeader[0] << 8) + pSequenceHeader[1];
	uint8_t *pSPS = &pSequenceHeader[2];

	if (iSPSSize <=0)
		return BC_STS_SUCCESS;

	NALUnit nalu(pSPS, iSPSSize);
	if(nalu.Type() == NALUnit::NAL_Sequence_Params)
	{
		SeqParamSet sq;
		if(sq.Parse(&nalu))
		{
			int cx = sq.EncodedWidth();
			int cy = sq.EncodedHeight();
			bool bInterlace = sq.Interlaced();
			unsigned int iNumRefFrames = sq.NumRefFrames();

			// In Link we allocated 12 HD buffers for VDEC processing. This implies 12 * 1920 * 1088 bytes of memory storage.
			// The actual number of reference pictures allowed will be 2 less than the number of buffers allocated.
			// 2 is the number of pictures for processing overhead.
			// For various resolutions the amount of memory required per buffer is as follows -
			// 288x352 - 172032
			// 144x176 - 65536
			// 1088x1920 - 3194880
			// 576x720	- 688128
			// 320x352	- 196608
			// 160x176	- 65536
			// 720x1280 - 1474560
			// Use these values to determine the number of reference pictures we can support without errors in the HW

			unsigned int iNumRefPicturesSupported;

			// Just handle the two large cases. Assume that no clips exist with greater than 24 reference pictures
			// since no valid Profile/Level exists for that case. However x264 allows crazy things to happen. So change if needed.
			if(cy > 720)
				iNumRefPicturesSupported = ((12 * 3194880) / 3194880) - 2;
			else
				iNumRefPicturesSupported = ((12 * 3194880) / 1474560) - 2;

			if(iNumRefFrames > iNumRefPicturesSupported)
			{
				//Reject
				return BC_STS_ERROR;
			}
			Ctx->VidParams.NumOfRefFrames = iNumRefFrames;
			Ctx->VidParams.LevelIDC = sq.LevelIDC();
		}
	}
*/
	return BC_STS_SUCCESS;
}

BOOL DtsChkAVCSps(HANDLE hDevice, uint8_t *pBuffer, uint32_t ulSize)
{
	NALU_t Nalu;
	int ret = 0;
	uint32_t Pos = 0;

	while (1)
	{
		ret=DtsGetNaluType(hDevice, pBuffer + Pos,ulSize - Pos,&Nalu, false);
		if (ret <= 0)
		{
			return FALSE;
		}
		Pos += ret;

		if (Nalu.NalUnitType == NALU_TYPE_SPS)
			return TRUE;
	}
	return FALSE;
}


/* DtsProcInput asks this before DtsAddStartCode converts AVC1 length-prefixed
 * NALs to Annex B. Checking those bytes with the Annex-B-only parser misses
 * an in-band SPS and sends a second, untimestamped copy of the parameter
 * sets. Detect only; do not consume the converter's pending NAL state.
 */
static BOOL DtsAVC1HasSps(const DTS_LIB_CONTEXT *Ctx,
						const uint8_t *pBuffer, uint32_t ulSize)
{
	const uint32_t lengthSize = Ctx->VidParams.StartCodeSz;
	if (!pBuffer || !ulSize || lengthSize == 0 || lengthSize > 4 ||
		Ctx->PESConvParams.m_lStartCodeDataSize != 0)
		return FALSE;

	/* Match DtsAddH264SCode's format decision: conversion is disabled once
	 * its configured length field reads 1 (the existing Annex-B marker).
	 * Do not mistake a length such as 0x00000107 for a three-byte start code.
	 */
	uint32_t initialSize = 0;
	if (lengthSize <= ulSize)
		for (uint32_t i = 0; i < lengthSize; ++i)
			initialSize = (initialSize << 8) | pBuffer[i];
	const bool annexB = !Ctx->PESConvParams.m_bIsAdd_SCode_CodeIn || initialSize == 1;
	/* Use a bounded Annex-B scan: the legacy DtsChkAVCSps parser can read
	 * past a no-SPS AU. Leading zero bytes remain accepted in this mode.
	 */
	uint32_t first = 0;
	while (first < ulSize && pBuffer[first] == 0)
		++first;
	if (annexB)
	{
		if (ulSize < 4 || first < 2 || first >= ulSize || pBuffer[first] != 1)
			return FALSE;
		for (uint32_t i = 0; i < ulSize - 3; ++i)
			if (pBuffer[i] == 0 && pBuffer[i + 1] == 0 &&
				pBuffer[i + 2] == 1 && (pBuffer[i + 3] & 0x1f) == NALU_TYPE_SPS)
				return TRUE;
		return FALSE;
	}
	uint32_t pos = 0;
	while (pos < ulSize)
	{
		if (lengthSize > ulSize - pos)
			return FALSE;
		uint32_t nalSize = 0;
		for (uint32_t i = 0; i < lengthSize; ++i)
			nalSize = (nalSize << 8) | pBuffer[pos + i];
		pos += lengthSize;
		if (!nalSize || nalSize > ulSize - pos)
			return FALSE;
		if ((pBuffer[pos] & 0x1f) == NALU_TYPE_SPS)
			return TRUE;
		pos += nalSize;
	}
	return FALSE;
}

BOOL DtsCheckSpsPps(HANDLE hDevice, uint8_t *pBuffer, uint32_t ulSize)
{
	DTS_LIB_CONTEXT *Ctx = NULL;
	DTS_GET_CTX(hDevice,Ctx);
	if (Ctx->VidParams.MediaSubType == BC_MSUBTYPE_AVC1)
		return DtsAVC1HasSps(Ctx, pBuffer, ulSize);

	if((Ctx->VidParams.MediaSubType == BC_MSUBTYPE_H264) ||
	   (Ctx->VidParams.MediaSubType == BC_MSUBTYPE_DIVX) ||
	   (Ctx->VidParams.MediaSubType == BC_MSUBTYPE_DIVX311))
		return DtsChkAVCSps(hDevice, pBuffer, ulSize);
	else
		return FALSE;

}

BC_STATUS DtsAddH264SCode(HANDLE hDevice, uint8_t **ppBuffer, uint32_t *pUlDataSize, uint64_t *pTimeStamp)
{
	DTS_LIB_CONTEXT *Ctx = NULL;
	DTS_GET_CTX(hDevice,Ctx);

	uint32_t lPendActualSize = 0;
	uint8_t *pPendCurrentPos = NULL;

	uint8_t *pStart = NULL;
	uint32_t lDataRemained = 0;
	//unused uint64_t timestamp = *pTimeStamp;

	int sts;

	if(Ctx->PESConvParams.lPendBufferSize < (*pUlDataSize*2))
	{
		if (Ctx->PESConvParams.pStartcodePendBuff)
			free(Ctx->PESConvParams.pStartcodePendBuff);

		Ctx->PESConvParams.lPendBufferSize = *pUlDataSize * 2;
		if (Ctx->PESConvParams.lPendBufferSize < 1024)
			Ctx->PESConvParams.lPendBufferSize = 1024;
		// minimum 8 byte aligned since posix_memalign needs to have min of size of (void *) and on 64-bit this is 8 bytes
		sts = posix_memalign((void**)&Ctx->PESConvParams.pStartcodePendBuff, 8, Ctx->PESConvParams.lPendBufferSize);
		if(sts != 0)
		  return BC_STS_INSUFF_RES;
	}
	//Replace Start Code
	lDataRemained = *pUlDataSize;
	pStart = *ppBuffer;

	lPendActualSize = 0;
	pPendCurrentPos = Ctx->PESConvParams.pStartcodePendBuff;

	while(1)
	{
		if(Ctx->PESConvParams.m_lStartCodeDataSize != 0)
		{
			//Remained
			if(Ctx->PESConvParams.m_lStartCodeDataSize >= lDataRemained)
			{
				//Pre-Update Size
				lPendActualSize = lPendActualSize + lDataRemained;

				//Check Copy Size
				if(lPendActualSize > Ctx->PESConvParams.lPendBufferSize)
				{
					//Error
					Ctx->PESConvParams.m_lStartCodeDataSize = 0;
					return BC_STS_ERROR;
				}

				//Copy to Pend Buffer
				memcpy(pPendCurrentPos, pStart, lDataRemained);

				//Update Pending Buffer
				pPendCurrentPos = pPendCurrentPos + lDataRemained;

				//Update Source Buffer
				pStart = pStart + lDataRemained;
				Ctx->PESConvParams.m_lStartCodeDataSize = Ctx->PESConvParams.m_lStartCodeDataSize - lDataRemained;
				lDataRemained = 0;
				//Done
				break;
			}
			else
			{
				//Get Start Code Position

				//Pre-Update Size
				lPendActualSize = lPendActualSize + Ctx->PESConvParams.m_lStartCodeDataSize;

				//Check Copy Size
				if(lPendActualSize > Ctx->PESConvParams.lPendBufferSize)
				{
					//Error
					Ctx->PESConvParams.m_lStartCodeDataSize = 0;
					return BC_STS_ERROR;
				}


				//Copy to Pend Buffer
				memcpy(pPendCurrentPos, pStart, Ctx->PESConvParams.m_lStartCodeDataSize);

				//Update Pending Buffer
				pPendCurrentPos = pPendCurrentPos + Ctx->PESConvParams.m_lStartCodeDataSize;

				//Update Source Buffer
				pStart = pStart + Ctx->PESConvParams.m_lStartCodeDataSize;
				lDataRemained = lDataRemained - Ctx->PESConvParams.m_lStartCodeDataSize;
				Ctx->PESConvParams.m_lStartCodeDataSize = 0;
			}
		}

		//Get Start Code
		uint8_t *StartCode = pStart;
		if(lDataRemained > Ctx->VidParams.StartCodeSz)
		{
			//Get Size
			for(uint32_t i = 0;i < Ctx->VidParams.StartCodeSz;i++)
			{
				Ctx->PESConvParams.m_lStartCodeDataSize <<= 8;
				Ctx->PESConvParams.m_lStartCodeDataSize += StartCode[i];
			}

			if(Ctx->PESConvParams.m_lStartCodeDataSize < 0)
			{
				//Error
				Ctx->PESConvParams.m_lStartCodeDataSize = 0;
				return BC_STS_ERROR;
			}
			else if(Ctx->PESConvParams.m_lStartCodeDataSize == 1)
			{
				//Could be Alreay a Start Code
				Ctx->PESConvParams.m_lStartCodeDataSize = 0;
				Ctx->PESConvParams.m_bIsAdd_SCode_CodeIn = false;
				if (Ctx->PESConvParams.pStartcodePendBuff)
					free(Ctx->PESConvParams.pStartcodePendBuff);
				Ctx->PESConvParams.lPendBufferSize = 0;
				Ctx->PESConvParams.pStartcodePendBuff = NULL;
				return BC_STS_SUCCESS;
			}
			else if(Ctx->PESConvParams.m_lStartCodeDataSize < *pUlDataSize)
			{
				//Succeeded

				//BRCM Start Code
				//Pre-Update Size
				lPendActualSize = lPendActualSize + BRCM_START_CODE_SIZE;

				//Check Copy Size
				if(lPendActualSize >Ctx->PESConvParams.lPendBufferSize)
				{
					//Error
					Ctx->PESConvParams.m_lStartCodeDataSize = 0;
					return BC_STS_ERROR;
				}

				//Add to Pending Buffer
				for(int i=0;i<BRCM_START_CODE_SIZE - 1;i++)
				{
					pPendCurrentPos[i] = 0;
				}

				pPendCurrentPos[BRCM_START_CODE_SIZE - 1] = 1;

				//Update Pending Buffer
				pPendCurrentPos = pPendCurrentPos + BRCM_START_CODE_SIZE;

				//Update Source Buffer
				pStart = pStart + Ctx->VidParams.StartCodeSz;
				lDataRemained = lDataRemained - Ctx->VidParams.StartCodeSz;
			}
		}
		else
		{
			//Error
			return BC_STS_IO_XFR_ERROR;
		}
	}//While
	//Use Pending Buffer Directly

	*ppBuffer = Ctx->PESConvParams.pStartcodePendBuff;
	*pUlDataSize = lPendActualSize;
	return BC_STS_SUCCESS;
}


BC_STATUS DtsAddVC1SCode(HANDLE hDevice, uint8_t **ppBuffer, uint32_t *pUlDataSize, uint64_t *pTimeStamp)
{
	uint32_t iCount = 0;

	int sts = 0;
	DTS_LIB_CONTEXT *Ctx = NULL;

	DTS_GET_CTX(hDevice,Ctx);

	uint64_t timestamp = *pTimeStamp;

	//Check Start Code for AP and SP/MP
	if(((*ppBuffer)[0] == 0x00) && ((*ppBuffer)[1] == 0x00) && ((*ppBuffer)[2] == 0x01) && (((*ppBuffer)[3] == 0x0F) || ((*ppBuffer)[3] == 0x0D) || ((*ppBuffer)[3] == 0xE0)))
	{
		Ctx->PESConvParams.m_bIsAdd_SCode_CodeIn = false;
		if (Ctx->PESConvParams.pStartcodePendBuff)
			free(Ctx->PESConvParams.pStartcodePendBuff);
		Ctx->PESConvParams.lPendBufferSize = 0;
		Ctx->PESConvParams.pStartcodePendBuff = NULL;
		return BC_STS_SUCCESS;
	}

	if(Ctx->PESConvParams.lPendBufferSize < (*pUlDataSize*2))
	{
		if (Ctx->PESConvParams.pStartcodePendBuff)
			free(Ctx->PESConvParams.pStartcodePendBuff);

		Ctx->PESConvParams.lPendBufferSize = *pUlDataSize * 2;
		if (Ctx->PESConvParams.lPendBufferSize < 1024)
			Ctx->PESConvParams.lPendBufferSize = 1024;
		sts = posix_memalign((void**)&Ctx->PESConvParams.pStartcodePendBuff, 8, Ctx->PESConvParams.lPendBufferSize);
		if(sts)
			return BC_STS_INSUFF_RES;
	}

	//unused uint8_t* pSequenceHeader = Ctx->VidParams.pMetaData;
	//unused LONG iSHSize = Ctx->VidParams.MetaDataSz;

	if((Ctx->VidParams.MediaSubType == BC_MSUBTYPE_WVC1) || (Ctx->VidParams.MediaSubType == BC_MSUBTYPE_WMVA))
	{

		//Copy Start Code
		LONG lStartCodeSize = 4;

		memcpy(Ctx->PESConvParams.pStartcodePendBuff + iCount, (uint8_t *)b_asf_vc1_frame_scode, lStartCodeSize);
		iCount += lStartCodeSize;

		//Copy Data
		memcpy(Ctx->PESConvParams.pStartcodePendBuff + iCount, (uint8_t *)*ppBuffer, *pUlDataSize);
		iCount +=*pUlDataSize;
		*pTimeStamp = timestamp;
	}
	else
	{
		//SP / MP

		uint8_t* pData = *ppBuffer;
		uint8_t* pDst = Ctx->PESConvParams.pStartcodePendBuff;

		if (Ctx->DevId == BC_PCI_DEVID_LINK)
		{
			LONG lCIHeaderSize = 17;
			LONG lCIPacketSize = 0;
			LONG lCIZeroPaddingSize = 0;
			LONG lCILastDataLoc = *pUlDataSize - 1;

			if ((lCIHeaderSize + *pUlDataSize) & 0x1F)
				lCIPacketSize = (((lCIHeaderSize + *pUlDataSize) / 32) + 1) * 32;
			else
				lCIPacketSize = (lCIHeaderSize + *pUlDataSize);

			//Code-In Zero Padding
			lCIZeroPaddingSize = lCIPacketSize - (lCIHeaderSize + *pUlDataSize);

			memcpy(pDst, b_asf_vc1_sm_codein_header, 16);
			*((uint32_t *)(pDst + 4)) = DWORD_SWAP((uint32_t)lCIPacketSize);
			*((uint32_t *)(pDst + 8)) = DWORD_SWAP((uint32_t)lCILastDataLoc);

			memcpy(pDst + 16, (uint8_t *)b_asf_vc1_sm_codein_data_suffix, 1);
			memcpy(pDst + 17, pData, *pUlDataSize);
			memset(pDst + 17 + *pUlDataSize, 0, lCIZeroPaddingSize);
			iCount = lCIPacketSize;
		}
		else if (Ctx->DevId == BC_PCI_DEVID_FLEA)
		{
			memcpy(pDst, b_asf_vc1_sm_picheader_scode, 4);
			memcpy(pDst + 4, pData, *pUlDataSize);
			iCount = *pUlDataSize + 4;
		}
	}

	*ppBuffer = Ctx->PESConvParams.pStartcodePendBuff;
	*pUlDataSize  = iCount;
	return BC_STS_SUCCESS;
}

BC_STATUS DtsCheckKeyFrame(HANDLE hDevice, uint8_t *pBuffer)
{
	DTS_LIB_CONTEXT *Ctx = NULL;
	DTS_GET_CTX(hDevice,Ctx);

	//unused BC_STATUS sts = BC_STS_SUCCESS;
	bool bKeyFrame = false;
	int iType = 0;


	if((Ctx->VidParams.MediaSubType == BC_MSUBTYPE_WVC1) || (Ctx->VidParams.MediaSubType == BC_MSUBTYPE_WMVA))
	{
		//Advanced Profile

		//0: P Type
		//1: B Type
		//2: I Type
		//3: BI Type

		//Interlaced Check
		if(!Ctx->VidParams.Progressive)
		{
			//Interlaced
			//Skip FCM with 2 bits

			//Get Type
			//--xx xxxx
			//--?
			if((pBuffer[0] & 0x20) == 0)
			{
				iType = 0;
			}
			//--1?
			else if((pBuffer[0] & 0x10) == 0)
			{
				iType = 1;
			}
			//--11 ?
			else if((pBuffer[0] & 0x08) == 0)
			{
				iType = 2;
			}
			//--11 1?
			else if((pBuffer[0] & 0x04) == 0)
			{
				iType = 3;
			}
		}
		else
		{
			//Progress
			//Get Type
			//xxxx xxxx
			//?
			if((pBuffer[0] & 0x80) == 0)
			{
				iType = 0;
			}
			//1?
			else if((pBuffer[0] & 0x40) == 0)
			{
				iType = 1;
			}
			//11?
			else if((pBuffer[0] & 0x20) == 0)
			{
				iType = 2;
			}
			//111?
			else if((pBuffer[0] & 0x10) == 0)
			{
				iType = 3;
			}
		}

		//Check Key Frame
		if(iType == 2)
		{
			bKeyFrame = true;
		}
	}
	else
	{
		//SP / MP

		//0: P Type
		//1: B Type
		//2: I Type
		//3: BI Type

		int iCurrentBit = 0;
		//Check Finterpflag
		if(Ctx->PESConvParams.m_bFinterpFlag)
		{
			iCurrentBit = iCurrentBit + 1;
		}

		//Drop 2 bits
		iCurrentBit = iCurrentBit + 2;

		//Check Rangered
		if(Ctx->PESConvParams.m_bRangered)
		{
			iCurrentBit = iCurrentBit + 1;
		}

		//Check Bit
		uint8_t bFirstCheck = 0;
		uint8_t bSecondCheck = 0;
		if(iCurrentBit == 2)
		{
			bFirstCheck = 0x20;
			bSecondCheck = 0x10;
		}
		else if(iCurrentBit == 3)
		{
			bFirstCheck = 0x10;
			bSecondCheck = 0x08;
		}
		else if(iCurrentBit == 4)
		{
			bFirstCheck = 0x08;
			bSecondCheck = 0x04;
		}

		if(pBuffer[0] & bFirstCheck)
		{
			iType = 0;
		}
		else if(!Ctx->PESConvParams.m_bMaxbFrames)
		{
			iType = 2;
		}
		else if(pBuffer[0] & bSecondCheck)
		{
			iType = 2;
		}
		else
		{
			iType = 1;
		}

		//Check Key Frame
		if(iType == 2)
		{
			bKeyFrame = true;
		}
	}

	if (bKeyFrame)
	{
		Ctx->PESConvParams.m_bAddSpsPps = true;
	}
	return BC_STS_SUCCESS;
}

BC_STATUS DtsAddStartCode(HANDLE hDevice, uint8_t **ppBuffer, uint32_t *pUlDataSize, uint64_t * pTimeStamp)
{
	DTS_LIB_CONTEXT *Ctx = NULL;
	DTS_GET_CTX(hDevice,Ctx);

	//unused BC_STATUS sts = BC_STS_SUCCESS;


	if (!Ctx->PESConvParams.m_bIsAdd_SCode_CodeIn)
		return BC_STS_SUCCESS;

	if(Ctx->VidParams.MediaSubType == BC_MSUBTYPE_AVC1 || Ctx->VidParams.MediaSubType == BC_MSUBTYPE_DIVX)
		return DtsAddH264SCode(hDevice, ppBuffer, pUlDataSize, pTimeStamp);

	if((Ctx->VidParams.MediaSubType == BC_MSUBTYPE_WVC1) || (Ctx->VidParams.MediaSubType == BC_MSUBTYPE_WMV3) || (Ctx->VidParams.MediaSubType == BC_MSUBTYPE_WMVA) || (Ctx->VidParams.MediaSubType == BC_MSUBTYPE_VC1))
		return DtsAddVC1SCode(hDevice, ppBuffer, pUlDataSize, pTimeStamp);

	return BC_STS_SUCCESS;
}

int DtsFindBSStartCode (unsigned char *Buf, int ZerosInStartcode)
{
	BOOL bStartCode = TRUE;
	int i;

	for (i = 0; i < ZerosInStartcode; i++)
		if(Buf[i] != 0)
			bStartCode = FALSE;

	if(Buf[i] != 1)
		bStartCode = FALSE;
	return bStartCode;
}

int DtsGetNaluType(HANDLE hDevice, uint8_t* pInputBuf, uint32_t ulSize, NALU_t* pNalu, bool bSkipSyncMarker)
{
	DTS_LIB_CONTEXT *Ctx = NULL;
	int b20sInSC, b30sInSC ;
	int bStartCodeFound, Rewind;
	int nLeadingZero8BitsCount=0, TrailingZero8Bits=0;
	//unused bool bSetIDR = true;
	//unused static BOOL fOne = TRUE;
	uint32_t Pos = 0;

	DTS_GET_CTX(hDevice,Ctx);

	if (bSkipSyncMarker)
	{
		pNalu->NalUnitType = (pInputBuf[0]) & 0x1f;
		pNalu->Len = ulSize;
		return 1;
	}
	while(Pos <= ulSize)
	{
		if( (pInputBuf[Pos++]) == 0)
			continue;
		else
			break;
	}
	if(pInputBuf[Pos-1] != 1)
	{
		return -1;
	}
	if(Pos < 3)
	{
		return -1;
	}
	else if(Pos == 3)
	{
		pNalu->StartcodePrefixLen = 3;
		nLeadingZero8BitsCount = 0;
	}
	else
	{
		nLeadingZero8BitsCount = Pos-4;
		pNalu->StartcodePrefixLen = 4;
	}

	//the 1st byte stream NAL unit can has nLeadingZero8BitsCount, but subsequent ones are not
	//allowed to contain it since these zeros(if any) are considered trailing_zero_8bits
	//of the previous byte stream NAL unit.
	if(!Ctx->PESConvParams.m_bIsFirstByteStreamNALU && nLeadingZero8BitsCount>0)
	{
//		DbgLog((LOG_TRACE, 1, TEXT("GetNaluType : ret 3\n")));
		return -1;
	}
	Ctx->PESConvParams.m_bIsFirstByteStreamNALU = false;

	bStartCodeFound = 0;
	b20sInSC = 0;
	b30sInSC = 0;

	while( (!bStartCodeFound) && (Pos < ulSize))
	{
		Pos++;
		if(Pos > ulSize)
		{
//			DbgLog((LOG_TRACE, 1, TEXT("GetNaluType : Pos > size = %d\n"),ulSize));

		}
		b30sInSC = DtsFindBSStartCode( (pInputBuf + Pos- 4), 3);
		if(b30sInSC != 1)
			b20sInSC = DtsFindBSStartCode( (pInputBuf + Pos -3), 2);
		bStartCodeFound = (b20sInSC  || b30sInSC);
	}

	Rewind = 0;
	if(!bStartCodeFound)
	{
//		DbgLog((LOG_TRACE, 1, TEXT("GetNaluType : ret 4 Pos = %d size = %d\n"),Pos,ulSize));

		//even if next start code is not found pprocess this NAL.
#if 0
		return -1;
#endif

	}

	if(bStartCodeFound)
	{
		//Count the trailing_zero_8bits
		//TrailingZero8Bits is present only for start code 00 00 00 01
		if(b30sInSC)
		{
			while(pInputBuf[Pos-5-TrailingZero8Bits]==0)
				TrailingZero8Bits++;
		}
		// Here, we have found another start code (and read length of startcode bytes more than we should
		// have.  Hence, go back in the file

		if(b30sInSC)
			Rewind = -4;
		else if (b20sInSC)
			Rewind = -3;
	}

	// Here the leading zeros(if any), Start code, the complete NALU, trailing zeros(if any)
	// until the  next start code .
	// Total size traversed is Pos, Pos+rewind are the number of bytes excluding the next
	// start code, and (Pos+rewind)-StartcodePrefixLen-LeadingZero8BitsCount-TrailingZero8Bits
	// is the size of the NALU.

	pNalu->Len = (Pos+Rewind)-pNalu->StartcodePrefixLen-nLeadingZero8BitsCount-TrailingZero8Bits;

	pNalu->NalUnitType = (pInputBuf[nLeadingZero8BitsCount+pNalu->StartcodePrefixLen]) & 0x1f;

	return (Pos+Rewind);
}

BC_STATUS DtsParseAVC(HANDLE hDevice, uint8_t* pInputBuf, ULONG ulSize, uint32_t* Offset, bool bIDR, int *pNalType)
{
	NALU_t Nalu;
	int ret = 0;
	uint32_t Pos = 0;
	bool bResult = false;

	*pNalType = -1;

	while (1)
	{
		ret=DtsGetNaluType(hDevice, pInputBuf + Pos,ulSize - Pos,&Nalu, false);
		if (ret <= 0)
		{
			return BC_STS_ERROR;
		}
		Pos += ret;

		switch (Nalu.NalUnitType)
		{
		case NALU_TYPE_SLICE:
		case NALU_TYPE_IDR:
			bResult = true;
			break;
		case NALU_TYPE_SEI:
		case NALU_TYPE_PPS:
		case NALU_TYPE_SPS:
			if(!bIDR)
				bResult = true;
			break;
		case NALU_TYPE_DPA:
		case NALU_TYPE_DPC:
		case NALU_TYPE_AUD:
		case NALU_TYPE_EOSEQ:
		case NALU_TYPE_EOSTREAM:
		case NALU_TYPE_FILL:
		default:
			break;
		}
		if(bResult)
		{
			*Offset = Pos;
			break;
		}
	}
	*pNalType = Nalu.NalUnitType;

	return BC_STS_SUCCESS;
}

BC_STATUS DtsFindIDR(HANDLE hDevice, uint8_t* pInputBuffer, uint32_t ulSizeInBytes, uint32_t* pOffset)
{
	int	nNalType = 0;
	uint32_t ulPos = 0;

	DtsParseAVC(hDevice, pInputBuffer, ulSizeInBytes, &ulPos, true, &nNalType);

	if( (nNalType == NALU_TYPE_SLICE) | (nNalType == NALU_TYPE_IDR))
	{
		*pOffset = ulPos;
		return BC_STS_SUCCESS;
	}

	return BC_STS_ERROR;
}

BC_STATUS DtsFindStartCode(HANDLE hDevice, uint8_t* pInputBuffer, uint32_t ulSizeInBytes, uint32_t* pOffset)
{
	DTS_LIB_CONTEXT *Ctx = NULL;
	uint32_t i=0;
	uint8_t	Suffix1 = 0;
	uint8_t	Suffix2 = 0;
	*pOffset = 0;

	DTS_GET_CTX(hDevice,Ctx);

	if (Ctx->VidParams.MediaSubType == BC_MSUBTYPE_WVC1 || Ctx->VidParams.MediaSubType == BC_MSUBTYPE_WMVA || Ctx->VidParams.MediaSubType == BC_MSUBTYPE_VC1)
	{
		Suffix1 = VC1_FRM_SUFFIX;
		Suffix2 = VC1_SEQ_SUFFIX;
	}
	else if (Ctx->VidParams.MediaSubType == BC_MSUBTYPE_MPEG2VIDEO)
	{
		Suffix1 = MPEG2_FRM_SUFFIX;
		Suffix2 = MPEG2_SEQ_SUFFIX;
	}
	else if (Ctx->VidParams.MediaSubType ==BC_MSUBTYPE_WMV3)
	//For VC-1 SP/MP
	{
		Suffix1 = VC1_SM_FRM_SUFFIX;
	}

	if((Ctx->VidParams.MediaSubType == BC_MSUBTYPE_H264) || (Ctx->VidParams.MediaSubType == BC_MSUBTYPE_AVC1) || (Ctx->VidParams.MediaSubType == BC_MSUBTYPE_DIVX))
	{
		int	nNalType = 0;
		uint32_t ulPos = 0;
		if (DtsParseAVC(hDevice, pInputBuffer,ulSizeInBytes,&ulPos,false, &nNalType) != BC_STS_SUCCESS)
			return BC_STS_ERROR;

		if( (nNalType == NALU_TYPE_SEI) || (nNalType == NALU_TYPE_PPS) || (nNalType == NALU_TYPE_SPS))
		{
			*pOffset = ulPos;
			return BC_STS_SUCCESS;
		}
		else if( (nNalType == NALU_TYPE_SLICE) | (nNalType == NALU_TYPE_IDR))
		{
			*pOffset = 0;
			return BC_STS_SUCCESS;
		}
		return BC_STS_ERROR;

	}
	else if((Ctx->VidParams.MediaSubType == BC_MSUBTYPE_MPEG2VIDEO) || (Ctx->VidParams.MediaSubType == BC_MSUBTYPE_WVC1) ||
		(Ctx->VidParams.MediaSubType == BC_MSUBTYPE_WMVA) || (Ctx->VidParams.MediaSubType ==BC_MSUBTYPE_WMV3) || (Ctx->VidParams.MediaSubType == BC_MSUBTYPE_VC1))

	{
		while(i < ulSizeInBytes)
		{
			if( (*(pInputBuffer +i) == Suffix1) || (*(pInputBuffer +i) == Suffix2))
			{
				if(i >= 3)
				{
					if( (*(pInputBuffer+(i-3)) == 0x00) && (*(pInputBuffer+(i-2)) == 0x00) && (*(pInputBuffer+(i-1)) == 0x01))
					{
						*pOffset = i-3;
						return BC_STS_SUCCESS;
					}
				}
			}
			i++;
		}
		return BC_STS_ERROR;

	}
	return BC_STS_SUCCESS;
}

BOOL DtsFindPTSInfoCode(HANDLE hDevice, uint8_t* pInputBuffer, uint32_t ulSizeInBytes)
{
	DTS_LIB_CONTEXT *Ctx = NULL;
	DTS_GET_CTX(hDevice,Ctx);

	if((Ctx->VidParams.MediaSubType  != BC_MSUBTYPE_WMV3) && (Ctx->VidParams.MediaSubType  != BC_MSUBTYPE_WMVA))
		return FALSE;

	//Move PES Header
	uint32_t ulCurrent = 8;
	uint8_t bPESHeaderLen = pInputBuffer[ulCurrent];

	ulCurrent = ulCurrent + bPESHeaderLen + 1;

	//Check PES Payload for PTS Info
	if((ulSizeInBytes - ulCurrent) == 32)
	{
		DWORD dwFirstCode = *(DWORD *)&pInputBuffer[ulCurrent];
		ulCurrent = ulCurrent + 12;

		DWORD dwSecondCode = *(DWORD *)&pInputBuffer[ulCurrent];
		ulCurrent = ulCurrent + 4;

		if((dwFirstCode == VC1_SM_MAGIC_WORD) && (dwSecondCode == VC1_SM_MAGIC_WORD) && (pInputBuffer[ulCurrent] == VC1_SM_PTS_INFO_START_CODE))
		{
			return TRUE;
		}
	}

	return FALSE;

}

BC_STATUS DtsSymbIntSiBuffer (HANDLE hDevice, uint8_t* pInputBuffer, ULONG ulSize)
{
	DTS_LIB_CONTEXT *Ctx = NULL;
	DTS_GET_CTX(hDevice,Ctx);

	SYMBINT *pSymbint = &(Ctx->PESConvParams.m_SymbInt);
	pSymbint->m_pCurrent = pSymbint->m_pInputBuffer   = (uint8_t*) pInputBuffer;
	pSymbint->m_nSize =  ulSize;
	pSymbint->m_pInputBufferEnd  = pSymbint->m_pInputBuffer + ulSize;
	pSymbint->m_nUsed  = 1;
	pSymbint->m_ulOffset = 0;
	pSymbint->m_ulMask   = 0x80;

	return BC_STS_SUCCESS;
}

BC_STATUS DtsSymbIntSiUe (HANDLE hDevice, ULONG* pCode)
{
	uint32_t	ulSuffix;
	int     nLeadingZeros;
	int     nBit;
	DTS_LIB_CONTEXT *Ctx = NULL;
	DTS_GET_CTX(hDevice,Ctx);

	SYMBINT *pSymbint = &(Ctx->PESConvParams.m_SymbInt);

	nLeadingZeros = -1;
	for (nBit = 0; nBit == 0; nLeadingZeros++)
	{
		nBit = DtsSymbIntNextBit(hDevice);
		if(pSymbint->m_nUsed >= pSymbint->m_nSize)
			return BC_STS_ERROR;
	}

	*pCode = (1 << nLeadingZeros) - 1;

	ulSuffix = 0;
	while (nLeadingZeros-- > 0 )
	{
		ulSuffix = (ulSuffix << 1) | DtsSymbIntNextBit(hDevice);
		if(pSymbint->m_nUsed >= pSymbint->m_nSize)
			return BC_STS_ERROR;
	}
	*pCode += ulSuffix;

	return BC_STS_SUCCESS;
}

inline int DtsSymbIntNextBit ( HANDLE hDevice )
{
	int   nBit;
	DTS_LIB_CONTEXT *Ctx = NULL;
	DTS_GET_CTX(hDevice,Ctx);

	SYMBINT *pSymbint = &(Ctx->PESConvParams.m_SymbInt);

	nBit = (pSymbint->m_pCurrent[0] & pSymbint->m_ulMask) ? 1 : 0;

	if ((pSymbint->m_ulMask >>= 1) == 0)
	{
		pSymbint->m_ulMask = 0x80;

		if ( pSymbint->m_nUsed == pSymbint->m_nSize )
			pSymbint->m_pCurrent = pSymbint->m_pInputBuffer;//reset look again
		else
		{
			if ( ++pSymbint->m_pCurrent == pSymbint->m_pInputBufferEnd )
				pSymbint->m_pCurrent = pSymbint->m_pInputBuffer;
			pSymbint->m_nUsed++;
		}
	}
	pSymbint->m_ulOffset++;
	return nBit;
}

#if 0
void *DtsAlignedMalloc(size_t size, size_t alignment)
{
    void *p1 ,*p2; // basic pointer needed for computation.

    if((p1 =(void *) malloc(size + alignment + sizeof(size_t)))==NULL)
        return NULL;

    size_t addr = (size_t)p1 + alignment + sizeof(size_t);
    p2 = (void *)(addr - (addr%alignment));

    *((size_t *)p2-1) = (size_t)p1;

    return p2;
}

void DtsAlignedFree(void *ptr)
{
    if (ptr)
        free((void *)(*((size_t *) ptr-1)));
}

#endif
