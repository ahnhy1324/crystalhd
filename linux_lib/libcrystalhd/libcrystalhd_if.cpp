/********************************************************************
 * Copyright(c) 2006-2009 Broadcom Corporation.
 *
 *  Name: libcrystalhd_if.cpp
 *
 *  Description: Driver Interface API.
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

#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <stdlib.h>
#include <pthread.h>
#include <sys/ioctl.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include "7411d.h"
#include "libcrystalhd_version.h"
#include "bc_decoder_regs.h"
#include "libcrystalhd_if.h"
#include "libcrystalhd_priv.h"
#include "libcrystalhd_int_if.h"
#include "libcrystalhd_fwcmds.h"
#include "libcrystalhd_fwload_if.h"

static BC_STATUS DtsStopDecoderInternal(HANDLE hDevice, bool retryUnsubmitted,
	bool *controlBusy = NULL);

/* Serialize the command/cleanup interval without holding thLock while
 * allocating, waiting for firmware, or retiring TX/output owners.
 */
class DtsStopAttempt {
	DTS_LIB_CONTEXT *context;
public:
	explicit DtsStopAttempt(DTS_LIB_CONTEXT *ctx) : context(ctx) {}
	~DtsStopAttempt() {
		DtsLock(context);
		context->stopInProgress = false;
		DtsUnLock(context);
	}
};

class DtsLifecycleCall {
	DTS_LIB_CONTEXT *context;
	BC_STATUS status;
	bool active;
public:
	explicit DtsLifecycleCall(DTS_LIB_CONTEXT *ctx,
		DTS_CONTROL_OWNER owner = DTS_CONTROL_LIFECYCLE) : context(ctx),
		status(DtsAcquireControl(ctx, owner)),
		active(status == BC_STS_SUCCESS) {}
	~DtsLifecycleCall() { Release(); }
	BC_STATUS Status() const { return status; }
	void Consume() { active = false; }
	void Release() {
		if (active) {
			DtsReleaseControl(context);
			active = false;
		}
	}
};

class DtsInputCall {
	DTS_LIB_CONTEXT *context;
	BC_STATUS status;
	bool active;
public:
	explicit DtsInputCall(DTS_LIB_CONTEXT *ctx) : context(ctx),
		status(DtsBeginInputCall(ctx)), active(status == BC_STS_SUCCESS) {}
	~DtsInputCall() {
		if (active)
			DtsFinishInputCall(context);
	}
	BC_STATUS Status() const { return status; }
};

/* Consume a channel that is already in recovery quarantine.  No legacy
 * firmware STOP/CLOSE command is safe after a partial reset; only retire
 * host waiters and the driver/interface allocation. */
static BC_STATUS DtsRetireFailedRecoveryDevice(DTS_LIB_CONTEXT *Ctx,
	DtsLifecycleCall &control)
{
	const BC_STATUS output_sts = DtsCancelFetchOutInt(Ctx);
	if (output_sts != BC_STS_SUCCESS) {
		DtsLock(Ctx);
		Ctx->stopInProgress = false;
		DtsUnLock(Ctx);
		return BC_STS_BUSY;
	}
	DtsLock(Ctx);
	const bool output_retained =
		Ctx->outputPhase == DTS_OUTPUT_ACTIVE ||
		Ctx->outputPhase == DTS_OUTPUT_DELIVERING ||
		Ctx->outputPhase == DTS_OUTPUT_RETURNED ||
		(Ctx->ProcOutPending && Ctx->outputPhase != DTS_OUTPUT_RETIRE_ONLY);
	DtsUnLock(Ctx);
	if (output_retained) {
		DtsLock(Ctx);
		Ctx->stopInProgress = false;
		DtsUnLock(Ctx);
		return BC_STS_BUSY;
	}

	uint32_t globMode = DtsGetOPMode();
	if (Ctx->OpMode == DTS_PLAYBACK_MODE)
		globMode &= (~0x1);
	else if (Ctx->OpMode == DTS_DIAG_MODE)
		globMode &= (~0x2);
	else if (Ctx->OpMode == DTS_MONITOR_MODE)
		globMode &= (~0x4);
	else if (Ctx->OpMode == DTS_HWINIT_MODE)
		globMode &= (~0x8);
	else
		globMode = 0;
	DtsSetOPMode(globMode);
	if (Ctx->OpMode == DTS_PLAYBACK_MODE &&
	    Ctx->State != BC_DEC_STATE_CLOSE && Ctx->ProcessID != 0)
		DumpInputSampleToFile(NULL, 0);
	control.Consume();
	const BC_STATUS release_sts = DtsReleaseInterface(Ctx);
	/* BUSY is reserved for a live, non-consumed same-process handle. */
	return release_sts == BC_STS_BUSY ? BC_STS_ERROR : release_sts;
}

BC_STATUS DtsRecoveryFlushInputRing(DTS_LIB_CONTEXT *Ctx)
{
	return txBufFlush(&Ctx->circBuf);
}

BC_STATUS DtsRecoveryRestoreInputState(DTS_LIB_CONTEXT *Ctx)
{
	BC_STATUS status = DtsClrPendMdataList(Ctx);
	if (status != BC_STS_SUCCESS)
		return status;
	DtsLock(Ctx);
	Ctx->LastPicNum = -1;
	Ctx->LastSessNum = -1;
	Ctx->EOSCnt = 0;
	Ctx->DrvStatusEOSCnt = 0;
	Ctx->bEOS = false;
	Ctx->PESConvParams.m_lStartCodeDataSize = 0;
	Ctx->PESConvParams.m_bAddSpsPps = true;
	Ctx->recoveryInjectSpsPps =
		Ctx->PESConvParams.m_pSpsPpsBuf != NULL &&
		Ctx->PESConvParams.m_iSpsPpsLen != 0 &&
		(Ctx->VidParams.MediaSubType == BC_MSUBTYPE_AVC1 ||
		 Ctx->VidParams.MediaSubType == BC_MSUBTYPE_H264);
	DtsUnLock(Ctx);
	return BC_STS_SUCCESS;
}

static bool DtsBeginTxQuiesce(DTS_LIB_CONTEXT *Ctx)
{
	DtsLock(Ctx);
	Ctx->State = BC_DEC_STATE_FLUSH;
	Ctx->txQuiescing = true;
	Ctx->bEOSCheck = false;
	Ctx->eosTxStart = Ctx->txBytesEnqueued;
	Ctx->eosTxFence = Ctx->txBytesEnqueued;
	Ctx->eosDrainGeneration++;
	Ctx->eosTxComplete = false;
	const bool pending = Ctx->txPending;
	DtsUnLock(Ctx);
	return pending;
}

static BC_STATUS DtsWaitForTx(DTS_LIB_CONTEXT *Ctx)
{
	/* The driver can wait for CPB space before its timed DMA starts.
	 * Destructive flush callers first issue the firmware cancellation.
	 * Never hold thLock here: DMA completion returns pooled ioctl data
	 * through that mutex. A timeout does not relinquish the TX buffer.
	 */
	for (unsigned int polls = 0; ; ++polls) {
		DtsLock(Ctx);
		const bool pending = Ctx->txPending;
		DtsUnLock(Ctx);
		if (!pending)
			return BC_STS_SUCCESS;
		if (polls == 500)
			return BC_STS_TIMEOUT;
		bc_sleep_ms(10);
	}
}

/* Call only after TX admission is closed and any popped DMA has retired. */
static void DtsResetTxTracking(DTS_LIB_CONTEXT *Ctx, bool clearFault)
{
	DtsLock(Ctx);
	Ctx->txBytesEnqueued = 0;
	Ctx->txBytesRetired = 0;
	Ctx->eosTxStart = 0;
	Ctx->eosTxFence = 0;
	Ctx->eosDrainGeneration++;
	Ctx->outputProgress = 0;
	Ctx->bEOSCheck = false;
	Ctx->eosTxComplete = false;
	if (clearFault)
		Ctx->txDmaFault = false;
	DtsUnLock(Ctx);
}

/* Keep the entire public call active, including every early return after a
 * failed FETCH, post-repost status/EOS checks, and internal DropFrames loops. */
class DtsOutputCall {
	DTS_LIB_CONTEXT *context;
	BC_STATUS status;
	bool active;
	BC_DTS_PROC_OUT *failed_borrow;
public:
	explicit DtsOutputCall(DTS_LIB_CONTEXT *ctx, bool release = false,
		BC_DTS_PROC_OUT *borrow = NULL) :
		context(ctx), status(release ? DtsBeginOutputRelease(ctx) : DtsBeginOutputCall(ctx)),
		active(status == BC_STS_SUCCESS), failed_borrow(borrow) {}
	~DtsOutputCall() {
		if (active) {
			if (failed_borrow)
				failed_borrow->Ybuff = failed_borrow->UVbuff = NULL;
			DtsFinishOutputCall(context);
		}
	}
	BC_STATUS Status() const { return status; }
	BC_STATUS Publish(BC_DTS_PROC_OUT *output, BC_STATUS result) {
		result = DtsPublishOutput(context, output, result);
		active = false;
		return result;
	}
	BC_STATUS PublishCopy(BC_STATUS result) {
		result = DtsPublishCopiedOutput(context, result);
		active = false;
		return result;
	}
private:
	DtsOutputCall(const DtsOutputCall &);
	DtsOutputCall &operator=(const DtsOutputCall &);
};

#if (!__STDC_WANT_SECURE_LIB__)
inline bool memcpy_s(void *dest, size_t sizeInBytes, void *src, size_t count)
{
  bool status = false;
  if (count > sizeInBytes) {
    DebugLog_Trace(LDIL_DBG,"memcpy_s: buffer overflow\n");
  } else {
    memcpy(dest, src, count);
    status = true;
  }
  return(status);
}

inline bool memmove_s(void *dest, size_t sizeInBytes, void *src, size_t count)
{
  bool status = false;
  if (count > sizeInBytes) {
    DebugLog_Trace(LDIL_DBG,"memmove_s: buffer overflow\n");
  } else {
    memmove(dest, src, count);
    status = true;
  }
  return(status);
}
#endif

// Full TS packet of EOS
static __attribute__((aligned(4))) uint8_t eos_mpeg[184] =
{
    0x00, 0x00, 0x01, 0xb7,
    0x00, 0x00, 0x01, 0xb7,
    0x00, 0x00, 0x01, 0xb7,
    0x00, 0x00, 0x01, 0xb7,
    0x00, 0x00, 0x01, 0xb7,
    0x00, 0x00, 0x01, 0xb7,
    0x00, 0x00, 0x01, 0xb7,
    0x00, 0x00, 0x01, 0xb7,
    0x00, 0x00, 0x01, 0xb7,
    0x00, 0x00, 0x01, 0xb7,
    0x00, 0x00, 0x01, 0xb7,
    0x00, 0x00, 0x01, 0xb7,
    0x00, 0x00, 0x01, 0xb7,
    0x00, 0x00, 0x01, 0xb7,
    0x00, 0x00, 0x01, 0xb7,
    0x00, 0x00, 0x01, 0xb7,
    0x00, 0x00, 0x01, 0xb7,
    0x00, 0x00, 0x01, 0xb7,
    0x00, 0x00, 0x01, 0xb7,
    0x00, 0x00, 0x01, 0xb7,
    0x00, 0x00, 0x01, 0xb7,
    0x00, 0x00, 0x01, 0xb7,
    0x00, 0x00, 0x01, 0xb7,
    0x00, 0x00, 0x01, 0xb7,
    0x00, 0x00, 0x01, 0xb7,
    0x00, 0x00, 0x01, 0xb7,
    0x00, 0x00, 0x01, 0xb7,
    0x00, 0x00, 0x01, 0xb7,
    0x00, 0x00, 0x01, 0xb7,
    0x00, 0x00, 0x01, 0xb7,
    0x00, 0x00, 0x01, 0xb7,
    0x00, 0x00, 0x01, 0xb7,
    0x00, 0x00, 0x01, 0xb7,
    0x00, 0x00, 0x01, 0xb7,
    0x00, 0x00, 0x01, 0xb7,
    0x00, 0x00, 0x01, 0xb7,
    0x00, 0x00, 0x01, 0xb7,
    0x00, 0x00, 0x01, 0xb7,
    0x00, 0x00, 0x01, 0xb7,
    0x00, 0x00, 0x01, 0xb7,
    0x00, 0x00, 0x01, 0xb7,
    0x00, 0x00, 0x01, 0xb7,
    0x00, 0x00, 0x01, 0xb7,
    0x00, 0x00, 0x01, 0xb7,
    0x00, 0x00, 0x01, 0xb7,
    0x00, 0x00, 0x01, 0xb7
};
static __attribute__((aligned(4))) uint8_t eos_avc_vc1[184] =
{
    0x00, 0x00, 0x01, 0x0a,
    0x00, 0x00, 0x01, 0x0a,
    0x00, 0x00, 0x01, 0x0a,
    0x00, 0x00, 0x01, 0x0a,
    0x00, 0x00, 0x01, 0x0a,
    0x00, 0x00, 0x01, 0x0a,
    0x00, 0x00, 0x01, 0x0a,
    0x00, 0x00, 0x01, 0x0a,
    0x00, 0x00, 0x01, 0x0a,
    0x00, 0x00, 0x01, 0x0a,
    0x00, 0x00, 0x01, 0x0a,
    0x00, 0x00, 0x01, 0x0a,
    0x00, 0x00, 0x01, 0x0a,
    0x00, 0x00, 0x01, 0x0a,
    0x00, 0x00, 0x01, 0x0a,
    0x00, 0x00, 0x01, 0x0a,
    0x00, 0x00, 0x01, 0x0a,
    0x00, 0x00, 0x01, 0x0a,
    0x00, 0x00, 0x01, 0x0a,
    0x00, 0x00, 0x01, 0x0a,
    0x00, 0x00, 0x01, 0x0a,
    0x00, 0x00, 0x01, 0x0a,
    0x00, 0x00, 0x01, 0x0a,
    0x00, 0x00, 0x01, 0x0a,
    0x00, 0x00, 0x01, 0x0a,
    0x00, 0x00, 0x01, 0x0a,
    0x00, 0x00, 0x01, 0x0a,
    0x00, 0x00, 0x01, 0x0a,
    0x00, 0x00, 0x01, 0x0a,
    0x00, 0x00, 0x01, 0x0a,
    0x00, 0x00, 0x01, 0x0a,
    0x00, 0x00, 0x01, 0x0a,
    0x00, 0x00, 0x01, 0x0a,
    0x00, 0x00, 0x01, 0x0a,
    0x00, 0x00, 0x01, 0x0a,
    0x00, 0x00, 0x01, 0x0a,
    0x00, 0x00, 0x01, 0x0a,
    0x00, 0x00, 0x01, 0x0a,
    0x00, 0x00, 0x01, 0x0a,
    0x00, 0x00, 0x01, 0x0a,
    0x00, 0x00, 0x01, 0x0a,
    0x00, 0x00, 0x01, 0x0a,
    0x00, 0x00, 0x01, 0x0a,
    0x00, 0x00, 0x01, 0x0a,
    0x00, 0x00, 0x01, 0x0a,
    0x00, 0x00, 0x01, 0x0a
};
static __attribute__((aligned(4))) uint8_t eos_vc1_spmp_link[32] =
{
    0x5a, 0x5a, 0x5a, 0x5a,
    0x00, 0x00, 0x00, 0x20,
    0x00, 0x00, 0x00, 0x05,
    0x5a, 0x5a, 0x5a, 0x5a,
    0x0d, 0x00, 0x00, 0x01,
    0x0a, 0xe0, 0x55, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00
};


static __attribute__((aligned(4))) uint8_t eos_divx[184] =
{
    0x00, 0x00, 0x01, 0xb1,
    0x00, 0x00, 0x01, 0xb1,
    0x00, 0x00, 0x01, 0xb1,
    0x00, 0x00, 0x01, 0xb1,
    0x00, 0x00, 0x01, 0xb1,
    0x00, 0x00, 0x01, 0xb1,
    0x00, 0x00, 0x01, 0xb1,
    0x00, 0x00, 0x01, 0xb1,
    0x00, 0x00, 0x01, 0xb1,
    0x00, 0x00, 0x01, 0xb1,
    0x00, 0x00, 0x01, 0xb1,
    0x00, 0x00, 0x01, 0xb1,
    0x00, 0x00, 0x01, 0xb1,
    0x00, 0x00, 0x01, 0xb1,
    0x00, 0x00, 0x01, 0xb1,
    0x00, 0x00, 0x01, 0xb1,
    0x00, 0x00, 0x01, 0xb1,
    0x00, 0x00, 0x01, 0xb1,
    0x00, 0x00, 0x01, 0xb1,
    0x00, 0x00, 0x01, 0xb1,
    0x00, 0x00, 0x01, 0xb1,
    0x00, 0x00, 0x01, 0xb1,
    0x00, 0x00, 0x01, 0xb1,
    0x00, 0x00, 0x01, 0xb1,
    0x00, 0x00, 0x01, 0xb1,
    0x00, 0x00, 0x01, 0xb1,
    0x00, 0x00, 0x01, 0xb1,
    0x00, 0x00, 0x01, 0xb1,
    0x00, 0x00, 0x01, 0xb1,
    0x00, 0x00, 0x01, 0xb1,
    0x00, 0x00, 0x01, 0xb1,
    0x00, 0x00, 0x01, 0xb1,
    0x00, 0x00, 0x01, 0xb1,
    0x00, 0x00, 0x01, 0xb1,
    0x00, 0x00, 0x01, 0xb1,
    0x00, 0x00, 0x01, 0xb1,
    0x00, 0x00, 0x01, 0xb1,
    0x00, 0x00, 0x01, 0xb1,
    0x00, 0x00, 0x01, 0xb1,
    0x00, 0x00, 0x01, 0xb1,
    0x00, 0x00, 0x01, 0xb1,
    0x00, 0x00, 0x01, 0xb1,
    0x00, 0x00, 0x01, 0xb1,
    0x00, 0x00, 0x01, 0xb1,
    0x00, 0x00, 0x01, 0xb1,
    0x00, 0x00, 0x01, 0xb1
};

static __attribute__((aligned(4))) uint8_t btp_video_done_es_private[] =
{
    /* 0x81, 0x01, 0x14, 0x80,*/
    0x42, 0x52, 0x43, 0x4D, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    /*, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00,*/
};

static __attribute__((aligned(4))) uint8_t btp_video_done_es[] =
{
                                        /*0x81, 0x01, 0x14, 0x80, 0x42, 0x52, 0x43, 0x4D, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF,*/ 0x00, 0x00, 0x00,
    0x00, 0x0C, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xAA, 0xBB, 0xCC, 0xDD, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0xBC, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF
};

#if 0
static __attribute__((aligned(4))) uint8_t btp_video_plunge_es[] =
{
                                        /*0x81, 0x01, 0x14, 0x80, 0x42, 0x52, 0x43, 0x4D, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF,*/ 0x00, 0x00, 0x00,
    0x00, 0x0A, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xAA, 0xBB, 0xCC, 0xDD, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0xBC, 0xAA, 0xBB, 0xCC, 0xDD, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF
};
#endif

static __attribute__((aligned(4))) uint8_t ExtData[] =
{ 0x00, 0x00};


BC_STATUS DtsSetupHardware(HANDLE hDevice, BOOL IgnClkChk)
{
	BC_STATUS sts = BC_STS_SUCCESS;
	DTS_LIB_CONTEXT *Ctx;

	DTS_GET_CTX(hDevice,Ctx);
	DtsLifecycleCall control(Ctx);
	if (control.Status() != BC_STS_SUCCESS)
		return control.Status();
	DtsLock(Ctx);
	const bool input_busy = Ctx->inputCalls &&
		!pthread_equal(Ctx->inputThread, pthread_self());
	DtsUnLock(Ctx);
	if (input_busy)
		return BC_STS_BUSY;

	if( !IgnClkChk){
		if(Ctx->DevId == BC_PCI_DEVID_LINK || Ctx->DevId == BC_PCI_DEVID_FLEA){
			if(DtsGetHwInitSts() != BC_DIL_HWINIT_NOT_YET){
				DebugLog_Trace(LDIL_DBG," HW init already?\n");
				return BC_STS_SUCCESS;
			}
		}
	}

	DtsSetHwInitSts(BC_DIL_HWINIT_IN_PROGRESS);

    if (Ctx->DevId == BC_PCI_DEVID_LINK)
        sts = DtsPushAuthFwToLink(hDevice,NULL);
	else if (Ctx->DevId == BC_PCI_DEVID_FLEA)
		sts = DtsPushFwToFlea(hDevice,NULL);
	else {
		DebugLog_Trace(LDIL_DBG,"HW Type not found\n");
		return BC_STS_ERROR;
	}

	if(sts != BC_STS_SUCCESS){
		return sts;
	}

	/* Initialize Firmware interface */
	sts = DtsFWInitialize(hDevice,0);

	if (sts == BC_STS_SUCCESS)
		DtsSetHwInitSts(BC_DIL_HWINIT_DONE);
	else
		DtsSetHwInitSts(BC_DIL_HWINIT_NOT_YET);

	return sts;
}

static BC_STATUS DtsReleaseChannel(HANDLE  hDevice, uint32_t ChannelID, bool Stop)
{
	BC_STATUS	sts = BC_STS_SUCCESS;
	if(Stop){
		sts = DtsFWStopVideo(hDevice, ChannelID, true);
		if(sts != BC_STS_SUCCESS){
			DebugLog_Trace(LDIL_DBG,"DtsReleaseChannel: StopVideoFailed Ignoring error\n");
		}
	}
	sts = DtsFWCloseChannel(hDevice, ChannelID);
	if(sts != BC_STS_SUCCESS){
		DebugLog_Trace(LDIL_DBG,"DtsReleaseChannel: DtsFWCloseChannel Failed\n");
	}

	return sts;
}

static BC_STATUS DtsRecoverableDecOpen(HANDLE  hDevice,uint32_t StreamType)
{
	BC_STATUS	sts = BC_STS_SUCCESS;
	uint32_t			retry = 3;

	DTS_LIB_CONTEXT		*Ctx = NULL;

	DTS_GET_CTX(hDevice,Ctx);

	if (!DtsChkPID(Ctx->ProcessID))
		return BC_STS_ERROR;

	DtsLock(Ctx);
	const bool recoveryOpen = Ctx->controlOwner == DTS_CONTROL_RECOVERY &&
		Ctx->controlDepth && pthread_equal(Ctx->controlThread, pthread_self());
	DtsUnLock(Ctx);

	/* Recovery has already performed the one permitted full hardware reset.
	 * Do not enter Link's ordinary open repair loop here: a rejected replacement
	 * channel must remain the exact terminal recovery error, not trigger a blind
	 * STOP/CLOSE, another setup, or get overwritten by cleanup success. */
	if (Ctx->DevId == BC_PCI_DEVID_FLEA || recoveryOpen)
	{
		sts = DtsFWOpenChannel(hDevice, StreamType, 0);
		if(sts == BC_STS_SUCCESS)
		{
			/*For Multiapplication support this will change.*/
			if(Ctx->OpenRsp.channelId != 0)
				sts = BC_STS_FW_CMD_ERR;
		}
		return sts;
	}

	while(retry--){
		sts = DtsFWOpenChannel(hDevice, StreamType, 0);
		if(sts == BC_STS_SUCCESS){
			if(Ctx->OpenRsp.channelId == 0){
				break;
			}else{
				DebugLog_Trace(LDIL_DBG,"DtsFWOpenChannel: ChannelID leakage..\n");
				/* First Release the Current Channel.*/
				DtsReleaseChannel(hDevice,Ctx->OpenRsp.channelId, FALSE);
				/* Fall through to release the previous Channel */
				sts = BC_STS_FW_CMD_ERR;
			}
		}

		if((sts == BC_STS_TIMEOUT) || (retry == 1) ){
			/* Do Full initialization */
			sts = DtsSetupHardware(hDevice,TRUE);
			if(sts != BC_STS_SUCCESS)
				break;
			/* Setup The Clock Again */
			sts = DtsSetVideoClock(hDevice,0);
			if(sts != BC_STS_SUCCESS )
				break;

		}else{
			sts = DtsReleaseChannel(hDevice,0,TRUE);
			if(sts != BC_STS_SUCCESS)
				break;
		}
	}
	return sts;
}

//===================================Externs ============================================
DRVIFLIB_API BC_STATUS
DtsDeviceOpen(
    HANDLE	*hDevice,
	uint32_t		mode
    )
{
	int 		drvHandle=1;
	BC_STATUS	Sts=BC_STS_SUCCESS;
	HANDLE		localDevice = NULL;
	uint32_t globMode = 0;
	uint8_t	nTry=1;
	uint32_t		VendorID, DeviceID, RevID, FixFlags, drvMode;
	uint32_t		drvVer, dilVer;
	uint32_t		fwVer, decVer, hwVer;
	pid_t	processID;
	int shmid=0;

	DebugLog_Trace(LDIL_DBG,"Running DIL (%d.%d.%d) Version\n",
		DIL_MAJOR_VERSION,DIL_MINOR_VERSION,DIL_REVISION );

	if (!hDevice)
		return BC_STS_INV_ARG;
	*hDevice = NULL;

	processID = getpid();

	FixFlags = mode;
	mode &= 0xFF;

	Sts = DtsCreateShMem(&shmid);
	if(BC_STS_SUCCESS !=Sts)
		return Sts;

	if (mode != DTS_MONITOR_MODE && DtsIsDecOpened(processID))
	{
		DebugLog_Trace(LDIL_DBG, "DtsDeviceOpen: Decoder is already opened\n");
		DtsDelDilShMem();
		return BC_STS_DEC_EXIST_OPEN;
	}

	DebugLog_Trace(LDIL_DBG,"DtsDeviceOpen: Opening HW in mode %x\n", mode);

	/* For External API case, we support only Plyaback mode. */
	if( !(BC_DTS_DEF_CFG & BC_EN_DIAG_MODE) && (mode != DTS_PLAYBACK_MODE) ){
		DebugLog_Trace(LDIL_ERR,"DtsDeviceOpen: mode %d not supported\n",mode);
		DtsDelDilShMem();
		return BC_STS_INV_ARG;
	}

	if(!glob_mode_valid) {
		globMode = DtsGetOPMode();
		if(globMode&4) {
			globMode&=4;
		}
		DebugLog_Trace(LDIL_DBG,"DtsDeviceOpen: New globmode is %d \n",globMode);
	}
	else{
		globMode = DtsGetOPMode();
	}

	if (mode == DTS_HWINIT_MODE)
		DtsSetHwInitSts(BC_DIL_HWINIT_IN_PROGRESS);

	drvHandle = open(CRYSTALHD_API_DEV_NAME, O_RDWR);
	if(drvHandle < 0)
	{
		DebugLog_Trace(LDIL_ERR,"DtsDeviceOpen: Create File Failed\n");
		DtsDelDilShMem();
		return BC_STS_ERROR;
	}


	/* Initialize Internal Driver interfaces.. */
	if( (Sts = DtsInitInterface(drvHandle,&localDevice, mode)) != BC_STS_SUCCESS){
		DebugLog_Trace(LDIL_ERR,"DtsDeviceOpen: Interface Init Failed:%x\n",Sts);
		goto fail;
	}
	if( (Sts = DtsGetHwType(localDevice,&DeviceID,&VendorID,&RevID))!=BC_STS_SUCCESS){
		DebugLog_Trace(LDIL_DBG,"Get Hardware Type Failed\n");
		goto fail;
	}

	// set Ctx->DevId early, other depend on it
	DtsGetContext(localDevice)->DevId = DeviceID;
	DtsSetgDevID(DeviceID);

	/*
	 * Old layout link cards have issues w/a core clock of 200, so we use
	 * 180 for all link cards, as we have no way to tell old layout from
	 * new layout cards.
	 */
	DtsSetCoreClock(localDevice, 180);

	/*
	 * We have to specify the mode to the driver.
	 * So the driver can cleanup only in case of
	 * playback/Diag mode application close.
	 */
	if ((Sts = DtsGetVersion(localDevice, &drvVer, &dilVer)) != BC_STS_SUCCESS) {
		DebugLog_Trace(LDIL_DBG,"Get drv ver failed\n");
		goto fail;
	}
	/* If driver minor version is more than 13, enable DTS_SKIP_TX_CHK_CPB feature */
	if (FixFlags & DTS_SKIP_TX_CHK_CPB) {
		if (((drvVer >> 16) & 0xFF) > 13)
			FixFlags |= DTS_SKIP_TX_CHK_CPB;
	}

	if (FixFlags & DTS_ADAPTIVE_OUTPUT_PER) {
		if(DtsGetContext(localDevice)->DevId == BC_PCI_DEVID_FLEA)
			Sts = DtsGetFWVersion(localDevice, &fwVer, &decVer, &hwVer, (char*)FWBINFILE_70015, 0);
		else
			Sts = DtsGetFWVersion(localDevice, &fwVer, &decVer, &hwVer, (char*)FWBINFILE_70012, 0);
        if(Sts == BC_STS_SUCCESS) {
			if (fwVer >= ((14 << 16) | (8 << 8) | (1)))		// 2.14.8.1 (ignore 2)
				FixFlags |= DTS_ADAPTIVE_OUTPUT_PER;
			else
				FixFlags &= (~DTS_ADAPTIVE_OUTPUT_PER);
        }
	}

	/* only enable dropping of repeated pictures for Adobe mode and MFT mode */
	/* since we see corrupted video is some cases for non Adobe usage */
	/* NAREN - This is a major hack since we have not root caused the corrupted video */
	if(((FixFlags & DTS_SINGLE_THREADED_MODE) || (FixFlags & DTS_MFT_MODE)) && !(FixFlags & DTS_DIAG_TEST_MODE))
		FixFlags |= DTS_PLAYBACK_DROP_RPT_MODE;

	/* If driver minor version is newer (for major == 2), send the fullMode */
	if (((drvVer >> 24) & 0xFF) == 2) {
		if (((drvVer >> 16) & 0xFF) > 10) drvMode = FixFlags;
		else drvMode = mode;
	}
	else
		drvMode = FixFlags;

	if( (Sts = DtsNotifyOperatingMode(localDevice,drvMode)) != BC_STS_SUCCESS){
		DebugLog_Trace(LDIL_DBG,"Notify Operating Mode Failed\n");
		goto fail;
	}

	/* Setup Hardware Specific Configuration */
	DtsSetupConfig(DtsGetContext(localDevice), DeviceID, RevID, FixFlags);

	/* Enable single threaded mode in the context */
	if (FixFlags & DTS_SINGLE_THREADED_MODE) {
		DebugLog_Trace(LDIL_DBG,"Enable single threaded mode\n");
		DtsGetContext(localDevice)->SingleThreadedAppMode = 1;
	}

	if(mode == DTS_PLAYBACK_MODE){
		globMode |= 0x1;
	} else if(mode == DTS_DIAG_MODE){
		globMode |= 0x2;
	} else if(mode == DTS_MONITOR_MODE) {
		globMode |= 0x4;
	} else if(mode == DTS_HWINIT_MODE) {
		globMode |= 0x8;
	} else {
		globMode = 0;
	}

	DtsSetOPMode(globMode);

	if (DeviceID == BC_PCI_DEVID_LINK || DeviceID == BC_PCI_DEVID_FLEA)
		nTry = HARDWARE_INIT_RETRY_LINK_CNT;
	else
		nTry = HARDWARE_INIT_RETRY_CNT;

	if((mode == DTS_PLAYBACK_MODE)||(mode == DTS_HWINIT_MODE))
	{
		while(nTry--)
		{
			Sts = 	DtsSetupHardware(localDevice, FALSE);
			if(Sts == BC_STS_SUCCESS)
			{
				break;
			}
			else
			{
				DebugLog_Trace(LDIL_DBG,"DtsSetupHardware: Failed from Open\n");
				bc_sleep_ms(100);
			}
		}
		if(Sts != BC_STS_SUCCESS )
			goto fail;
	}

	if(mode == DTS_HWINIT_MODE){
		DtsSetHwInitSts(0);
	}

	// Clear all stats before we start play back
	if(mode == DTS_PLAYBACK_MODE) {
		DtsRstDrvStat(localDevice);
	}

	DtsGetContext(localDevice)->ProcessID = processID;
	*hDevice = localDevice;

	//DtsDevRegisterWr( hDevice,UartSelectA, 3);
	return Sts;

fail:
	if (localDevice)
		DtsReleaseInterface(DtsGetContext(localDevice));
	else {
		close(drvHandle);
		DtsDelDilShMem();
	}
	return Sts;
}

DRVIFLIB_API BC_STATUS
DtsDeviceClose(
    HANDLE hDevice
    )
{
	DTS_LIB_CONTEXT		*Ctx;
	uint32_t globMode = 0;
	BC_STATUS sts = BC_STS_SUCCESS;

	if(hDevice == NULL)
		return BC_STS_SUCCESS;

	DTS_GET_CTX(hDevice,Ctx);

	if (!DtsChkPID(Ctx->ProcessID))
		return BC_STS_ERROR;
	/* If the sole TX/recovery worker has already retired, a pending PM epoch
	 * can never make progress. Convert it atomically to terminal quarantine
	 * before TEARDOWN admission so close cannot return BUSY forever. */
	DtsLock(Ctx);
	if (Ctx->recoveryPending && Ctx->htxThread == 0) {
		Ctx->recoveryPending = false;
		if (Ctx->recoveryStatus == BC_STS_SUCCESS)
			Ctx->recoveryStatus = BC_STS_PWR_MGMT;
	}
	DtsUnLock(Ctx);
	DtsLifecycleCall control(Ctx, DTS_CONTROL_TEARDOWN);
	if (control.Status() != BC_STS_SUCCESS)
		return control.Status();
	DtsLock(Ctx);
	const bool recovery_failed = Ctx->recoveryStatus != BC_STS_SUCCESS;
	const bool input_retained = Ctx->inputCalls != 0;
	const bool control_owned = Ctx->stopInProgress;
	DtsUnLock(Ctx);
	if (control_owned)
		return BC_STS_BUSY;
	if (input_retained)
		return BC_STS_BUSY;
	if (recovery_failed)
		return DtsRetireFailedRecoveryDevice(Ctx, control);

	if(Ctx->State != BC_DEC_STATE_CLOSE){
		sts = DtsCloseDecoder(hDevice);
		DtsLock(Ctx);
		const bool recovery_pending = Ctx->recoveryPending;
		const bool recovery_worker_gone = Ctx->htxThread == 0;
		if (recovery_pending && recovery_worker_gone) {
			Ctx->recoveryPending = false;
			if (Ctx->recoveryStatus == BC_STS_SUCCESS)
				Ctx->recoveryStatus = BC_STS_PWR_MGMT;
		}
		if (Ctx->stopInProgress) {
			DtsUnLock(Ctx);
			return BC_STS_BUSY;
		}
		if (recovery_pending) {
			DtsUnLock(Ctx);
			if (recovery_worker_gone)
				return DtsRetireFailedRecoveryDevice(Ctx, control);
			return BC_STS_BUSY;
		}
		const bool join_needed = Ctx->txQuiescing || DtsStopIsUnresolved(Ctx);
		if (join_needed)
			Ctx->stopInProgress = true;
		DtsUnLock(Ctx);
		if (join_needed) {
			/* DeviceClose has always joined TX before consuming the handle.
			 * Do not return a live worker to existing callers that unload
			 * the library on close. Join before retrying firmware teardown;
			 * this can still wait on an unresponsive driver, as before.
			 * Claim control before joining so a concurrent START cannot
			 * acquire a worker which this close would then shut down.
			 */
			DtsJoinTxThread(Ctx);
			DtsLock(Ctx);
			Ctx->stopInProgress = false;
			const bool late_recovery = Ctx->recoveryPending;
			const bool worker_gone = Ctx->htxThread == 0;
			if (late_recovery && worker_gone) {
				Ctx->recoveryPending = false;
				if (Ctx->recoveryStatus == BC_STS_SUCCESS)
					Ctx->recoveryStatus = BC_STS_PWR_MGMT;
			}
			DtsUnLock(Ctx);
			/* A PM response from the joined worker cannot be recovered now. Do
			 * not issue another CLOSE or reject a RETIRE_ONLY output forever. */
			if (late_recovery) {
				if (!worker_gone)
					return BC_STS_BUSY;
				return DtsRetireFailedRecoveryDevice(Ctx, control);
			}
			DtsCloseDecoder(hDevice);
		}
	}

	/* A concurrent STOP/START/CLOSE is also a live library owner. Once
	 * teardown claims this interval, no new lifecycle call may enter.
	 */
	DtsLock(Ctx);
	const bool pre_cancel_recovery = Ctx->recoveryPending;
	const bool pre_cancel_worker_gone = Ctx->htxThread == 0;
	if (pre_cancel_recovery && pre_cancel_worker_gone) {
		Ctx->recoveryPending = false;
		if (Ctx->recoveryStatus == BC_STS_SUCCESS)
			Ctx->recoveryStatus = BC_STS_PWR_MGMT;
	}
	if (pre_cancel_recovery && !pre_cancel_worker_gone) {
		DtsUnLock(Ctx);
		return BC_STS_BUSY;
	}
	if (pre_cancel_recovery) {
		DtsUnLock(Ctx);
		return DtsRetireFailedRecoveryDevice(Ctx, control);
	}
	if (Ctx->stopInProgress) {
		DtsUnLock(Ctx);
		return BC_STS_BUSY;
	}
	Ctx->stopInProgress = true;
	DtsUnLock(Ctx);
	const BC_STATUS output_sts = DtsCancelFetchOutInt(Ctx);
	if (output_sts != BC_STS_SUCCESS) {
		DtsLock(Ctx);
		Ctx->stopInProgress = false;
		DtsUnLock(Ctx);
		return BC_STS_BUSY;
	}
	DtsLock(Ctx);
	const bool cancel_recovery = Ctx->recoveryPending;
	const bool cancel_worker_gone = Ctx->htxThread == 0;
	if (cancel_recovery && cancel_worker_gone) {
		Ctx->recoveryPending = false;
		if (Ctx->recoveryStatus == BC_STS_SUCCESS)
			Ctx->recoveryStatus = BC_STS_PWR_MGMT;
	}
	if (cancel_recovery && !cancel_worker_gone)
		Ctx->stopInProgress = false;
	DtsUnLock(Ctx);
	if (cancel_recovery) {
		if (!cancel_worker_gone)
			return BC_STS_BUSY;
		return DtsRetireFailedRecoveryDevice(Ctx, control);
	}
	DtsLock(Ctx);
	const bool output_retained = Ctx->ProcOutPending || Ctx->outputPhase != DTS_OUTPUT_IDLE;
	if (output_retained)
		Ctx->stopInProgress = false;
	DtsUnLock(Ctx);
	if (output_retained)
		return BC_STS_BUSY;
	/* Unmask the mode */
	globMode = DtsGetOPMode( );

	// Make sure we are in playback mode before freeing up playback resources
	if(Ctx->OpMode == DTS_PLAYBACK_MODE)
	{
		DtsFlushRxCapture(hDevice,false); // Make sure that all buffers and DMA engines are freed up
	}
	DtsLock(Ctx);
	const bool late_recovery = Ctx->recoveryPending;
	const bool worker_gone = Ctx->htxThread == 0;
	if (late_recovery && worker_gone) {
		/* A status command can publish the PM transition while DeviceClose is
		 * joining the sole recovery worker.  With no worker left, BUSY would be
		 * permanent: quarantine this partial channel and finish terminal host
		 * retirement without replaying firmware lifecycle commands. */
		Ctx->recoveryPending = false;
		if (Ctx->recoveryStatus == BC_STS_SUCCESS)
			Ctx->recoveryStatus = BC_STS_PWR_MGMT;
	}
	if (late_recovery && !worker_gone)
		Ctx->stopInProgress = false;
	DtsUnLock(Ctx);
	if (late_recovery && !worker_gone)
		return BC_STS_BUSY;
	if (late_recovery)
		return DtsRetireFailedRecoveryDevice(Ctx, control);

	if(Ctx->OpMode == DTS_PLAYBACK_MODE){
		globMode &= (~0x1);
	} else if(Ctx->OpMode == DTS_DIAG_MODE){
		globMode &= (~0x2);
	} else if(Ctx->OpMode == DTS_MONITOR_MODE) {
		globMode &= (~0x4);
	} else if(Ctx->OpMode == DTS_HWINIT_MODE) {
		globMode &= (~0x8);
	} else {
		globMode = 0;
	}
	DtsSetOPMode(globMode);
	if (Ctx->OpMode == DTS_PLAYBACK_MODE &&
	    Ctx->State != BC_DEC_STATE_CLOSE && Ctx->ProcessID != 0)
		DumpInputSampleToFile(NULL, 0);
	control.Consume();
	const BC_STATUS release_sts = DtsReleaseInterface(Ctx);
	const BC_STATUS result = sts == BC_STS_SUCCESS ? release_sts : sts;
	return result == BC_STS_BUSY ? BC_STS_ERROR : result;

}

DRVIFLIB_API BC_STATUS
DtsDupOutputPollFd(
    HANDLE hDevice,
    int   *pollFd
    )
{
	DTS_LIB_CONTEXT *Ctx;
	BC_IOCTL_DATA query;
	int fd;

	if (!pollFd)
		return BC_STS_INV_ARG;
	*pollFd = -1;

	Ctx = DtsGetContext(hDevice);
	if (!Ctx || Ctx->DevHandle < 0)
		return BC_STS_INV_ARG;

	memset(&query, 0, sizeof(query));
	query.IoctlDataSz = sizeof(query);
	if (ioctl(Ctx->DevHandle, BCM_IOC_QUERY_CAPS, &query) < 0) {
		if (errno == ENOTTY)
			return BC_STS_NOT_IMPL;
		return BC_STS_IO_ERROR;
	}
	if (query.RetSts == BC_STS_NOT_IMPL)
		return BC_STS_NOT_IMPL;
	if (query.RetSts != BC_STS_SUCCESS)
		return query.RetSts;
	if (query.u.DriverCaps.abi_version < BC_DRIVER_CAPS_ABI_VERSION ||
	    !(query.u.DriverCaps.flags & BC_DRIVER_CAP_RX_POLL))
		return BC_STS_NOT_IMPL;

	do {
		fd = fcntl(Ctx->DevHandle, F_DUPFD_CLOEXEC, 0);
	} while (fd < 0 && errno == EINTR);
	if (fd < 0)
		return BC_STS_IO_ERROR;

	*pollFd = fd;
	return BC_STS_SUCCESS;
}

DRVIFLIB_API BC_STATUS
DtsGetVersion(
    HANDLE  hDevice,
    uint32_t     *DrVer,
    uint32_t     *DilVer
	)
{
	BC_VERSION_INFO *pVerInfo;
	BC_IOCTL_DATA *pIocData = NULL;
	DTS_LIB_CONTEXT		*Ctx = NULL;
	BC_STATUS	sts = BC_STS_SUCCESS;


	DTS_GET_CTX(hDevice,Ctx);

	if(!(pIocData = DtsAllocIoctlData(Ctx)))
		return BC_STS_INSUFF_RES;

	pVerInfo = 	&pIocData->u.VerInfo;

	if( (sts=DtsDrvCmd(Ctx,BCM_IOC_GET_VERSION,0,pIocData,FALSE)) != BC_STS_SUCCESS){
		DtsRelIoctlData(Ctx,pIocData);
		DebugLog_Trace(LDIL_DBG,"DtsGetVersion: Ioctl failed: %d\n",sts);
		return sts;
	}

	*DrVer  = (pVerInfo->DriverMajor << 24) | (pVerInfo->DriverMinor<<16) | pVerInfo->DriverRevision;
	*DilVer = (DIL_MAJOR_VERSION <<24)| (DIL_MINOR_VERSION<<16) | DIL_REVISION;

	DtsRelIoctlData(Ctx,pIocData);

	return BC_STS_SUCCESS;
}

#define MAX_BIN_FILE_SZ 	0x300000

DRVIFLIB_API BC_STATUS
DtsGetFWVersionFromFile(
    HANDLE    hDevice,
    uint32_t  *StreamVer,
	uint32_t  *DecVer,
	char      *fname
	)
{
	static const char marker[] = "Media_PC_FW_Rev";
	BC_STATUS sts = BC_STS_ERROR;
	uint8_t *buf = NULL;
	long fileSize;
	size_t sizeRead;
	FILE *fhnd = NULL;
	char fwdir[MAX_PATH+1];
	char fwfile[MAX_PATH+1];

	DTS_LIB_CONTEXT		*Ctx = NULL;

	DTS_GET_CTX(hDevice,Ctx);

	sts = DtsGetDILPath(hDevice, fwdir, sizeof(fwdir));
	if(sts != BC_STS_SUCCESS){
		return sts;
	}
	sts = BC_STS_ERROR;

	const char *fwname = fname ? fname :
		(Ctx->DevId == BC_PCI_DEVID_FLEA ? FWBINFILE_70015 : FWBINFILE_70012);
	if (snprintf(fwfile, sizeof(fwfile), "%s%s", fwdir, fwname) >=
		(int)sizeof(fwfile))
		return BC_STS_INV_ARG;

	if(!StreamVer){
		DebugLog_Trace(LDIL_DBG,"\nDtsGetFWVersionFromFile: Null Pointer argument");
		return BC_STS_INSUFF_RES;
	}

	fhnd = fopen((const char *)fwfile, "rb");

	if(fhnd == NULL ){
		DebugLog_Trace(LDIL_DBG,"DtsGetFWVersionFromFile:Failed to Open file Err\n");
		return BC_STS_INSUFF_RES;
	}

	if (fseek(fhnd, 0, SEEK_END) || (fileSize = ftell(fhnd)) < 0) {
		DebugLog_Trace(LDIL_DBG,"DtsGetFWVersionFromFile:Failed to query file size\n");
		goto done;
	}
	if (fileSize < (long)(4U + sizeof(marker) - 1U) ||
	    fileSize > (long)MAX_BIN_FILE_SZ) {
		DebugLog_Trace(LDIL_DBG,"DtsGetFWVersionFromFile:Invalid file size\n");
		goto done;
	}
	if (fseek(fhnd, 0, SEEK_SET)) {
		DebugLog_Trace(LDIL_DBG,"DtsGetFWVersionFromFile:Failed to rewind file\n");
		goto done;
	}

	buf=(uint8_t *)malloc((size_t)fileSize);
	if(buf==NULL) {
		DebugLog_Trace(LDIL_DBG,"DtsGetFWVersionFromFile:Failed to allocate memory\n");
		sts = BC_STS_INSUFF_RES;
		goto done;
	}
	/* Read the FW bin file */
	sizeRead = fread(buf, sizeof(uint8_t), (size_t)fileSize, fhnd);
	if(sizeRead != (size_t)fileSize) {
		DebugLog_Trace(LDIL_DBG,"DtsGetFWVersionFromFile:Failed to read bin file\n");
		goto done;
	}

	for (size_t offset = 4U;
	     offset + sizeof(marker) - 1U <= sizeRead; ++offset) {
		if (buf[offset] == (uint8_t)marker[0] &&
		    !memcmp(buf + offset, marker, sizeof(marker) - 1U)) {
			const uint32_t version = ((uint32_t)buf[offset - 4U] << 16) |
						 ((uint32_t)buf[offset - 3U] << 8) |
						 (uint32_t)buf[offset - 2U];
			if (!version)
				break;
			*StreamVer = version;
			if (DecVer)
				*DecVer = 0;
			sts = BC_STS_SUCCESS;
			break;
		}
	}

done:
	free(buf);
	fclose(fhnd);
	return sts;
}

DRVIFLIB_API BC_STATUS
DtsGetFWVersion(
    HANDLE  hDevice,
    uint32_t     *StreamVer,
	uint32_t		*DecVer,
    uint32_t     *HwVer,
	char	*fname,
	uint32_t	     flag
	)
{
	BC_STATUS sts;
	if(flag) //get runtime version by issuing a FWcmd
	{
		DTS_LIB_CONTEXT *Ctx = NULL;
		DTS_GET_CTX(hDevice, Ctx);
		DtsLifecycleCall control(Ctx);
		if (control.Status() != BC_STS_SUCCESS)
			return control.Status();
		sts = DtsFWVersion(hDevice,StreamVer,DecVer,HwVer);
			if(sts == BC_STS_SUCCESS)
			{
				DebugLog_Trace(LDIL_DBG,"FW Version: Stream: %x, Dec: %x, HW:%x\n",*StreamVer,*DecVer,*HwVer);
			}
			else
			{
				DebugLog_Trace(LDIL_DBG,"DtsGetFWVersion: failed to get version fromFW at runtime: %d\n",sts);
				return sts;
			}

	}
	else //read the stream arc version from binary
	{
		sts = DtsGetFWVersionFromFile(hDevice,StreamVer,DecVer,fname);
		if(sts == BC_STS_SUCCESS)
		{
			if (HwVer != NULL)
				*HwVer = 0;
			DebugLog_Trace(LDIL_DBG,"FW Version: Stream: %x",*StreamVer);
			if(DecVer !=NULL)
				DebugLog_Trace(LDIL_DBG," Dec: %x\n",*DecVer);
		}
		else
		{
			DebugLog_Trace(LDIL_DBG,"DtsGetFWVersion: failed to get version from FW bin file: %d\n",sts);
			return sts;
		}

	}

	return BC_STS_SUCCESS;

}


DRVIFLIB_API BC_STATUS
DtsOpenDecoder(
    HANDLE    hDevice,
    uint32_t  StreamType)
{
	BC_STATUS sts = BC_STS_SUCCESS;

	DTS_LIB_CONTEXT	*Ctx = NULL;

	DTS_GET_CTX(hDevice,Ctx);
	DtsLifecycleCall control(Ctx);
	if (control.Status() != BC_STS_SUCCESS)
		return control.Status();

	if(Ctx->State != BC_DEC_STATE_CLOSE)
	{
		if((sts = DtsCloseDecoder(hDevice)) != BC_STS_SUCCESS)
		{
			DebugLog_Trace(LDIL_DBG, "DtsOpenDecoder: DtsCloseDecoder Failed (sts:%d)\n", sts);
			return sts;
		}
	}

	Ctx->LastPicNum = -1;
	Ctx->LastSessNum = -1;
	Ctx->EOSCnt = 0;
	Ctx->DrvStatusEOSCnt = 0;
	Ctx->bEOSCheck = false;
	Ctx->bEOS = false;
	DtsResetTxTracking(Ctx, true);
	Ctx->CapState = 0;
	Ctx->hw_paused = false;
	Ctx->fw_cmd_issued = false;

	sts = DtsSetVideoClock(hDevice,0);
	if (sts != BC_STS_SUCCESS)
	{
		DebugLog_Trace(LDIL_DBG,"Failed to Set Video Clock:%x\n",sts);
		return sts;
	}

	if(Ctx->DevId == BC_PCI_DEVID_LINK)
	{
		// FIX_ME to support other stream types.
		sts = DtsSetTSMode(hDevice,0);
		if(sts != BC_STS_SUCCESS )
		{
			return sts;
		}
	}

	if (Ctx->VidParams.MediaSubType != BC_MSUBTYPE_INVALID)
		StreamType = Ctx->VidParams.StreamType;
	else if (Ctx->DevId == BC_PCI_DEVID_FLEA)
		StreamType = BC_STREAM_TYPE_PES;

	sts = DtsRecoverableDecOpen(hDevice,StreamType);
	if(sts != BC_STS_SUCCESS )
	{
		DebugLog_Trace(LDIL_DBG,"Dts Recoverable Open Failed:%x\n",sts);
		return sts;
	}

	sts = DtsFWSetVideoInput(hDevice);
	if(sts != BC_STS_SUCCESS )
	{
		DebugLog_Trace(LDIL_DBG,"DtsFWSetVideoInput Failed:%x\n",sts);
		return sts;
	}

	Ctx->State = BC_DEC_STATE_STOP;

	DtsSetDecStat(true, Ctx->ProcessID);

	return BC_STS_SUCCESS;
}

DRVIFLIB_API BC_STATUS
DtsStartDecoder(
    HANDLE  hDevice
    )
{
	BC_STATUS	sts = BC_STS_SUCCESS;

	DTS_LIB_CONTEXT		*Ctx = NULL;
	DTS_GET_CTX(hDevice,Ctx);

	if (!DtsChkPID(Ctx->ProcessID))
		return BC_STS_ERROR;
	DtsLifecycleCall control(Ctx);
	if (control.Status() != BC_STS_SUCCESS)
		return control.Status();
	DtsLock(Ctx);
	const bool output_blocked = Ctx->txThreadExit || Ctx->txQuiescing || DtsStopIsUnresolved(Ctx) ||
		Ctx->ProcOutPending || Ctx->CancelWaiting || Ctx->outputPhase != DTS_OUTPUT_IDLE;
	DtsUnLock(Ctx);
	if (output_blocked)
		return BC_STS_BUSY;

	if (Ctx->State == BC_DEC_STATE_CLOSE) {
		DebugLog_Trace(LDIL_DBG,"DtsStartDecoder: Decoder is not opened\n");
		return BC_STS_DEC_NOT_OPEN;
	}

	if( Ctx->State == BC_DEC_STATE_START)
	{
		if ((sts = DtsStopDecoder(hDevice)) != BC_STS_SUCCESS)
		{
			DebugLog_Trace(LDIL_DBG, "DtsStartDecoder: DtsStopDecoder FAILED (sts:%d)\n", sts);
			return sts;
		}
	}
	DtsLock(Ctx);
	/* A DeviceClose join is terminal even if a retained output lease lets
	 * this handle survive temporarily; START does not recreate its worker.
	 */
	if (Ctx->txThreadExit || Ctx->txQuiescing || DtsStopIsUnresolved(Ctx) ||
	    Ctx->ProcOutPending || Ctx->CancelWaiting || Ctx->outputPhase != DTS_OUTPUT_IDLE) {
		DtsUnLock(Ctx);
		return BC_STS_BUSY;
	}
	if (Ctx->State == BC_DEC_STATE_CLOSE) {
		DtsUnLock(Ctx);
		return BC_STS_DEC_NOT_OPEN;
	}
	if (Ctx->State == BC_DEC_STATE_START) {
		DtsUnLock(Ctx);
		return BC_STS_BUSY;
	}
	Ctx->stopInProgress = true;
	DtsUnLock(Ctx);
	DtsStopAttempt attempt(Ctx);

	if(Ctx->VidParams.Progressive){
		sts = DtsSetProgressive(hDevice,0);
		if(sts != BC_STS_SUCCESS ) {
			DebugLog_Trace(LDIL_DBG,"DtsSetProgressive: Failed [%x]\n",sts);
			return sts;
		}
	}

	sts = DtsFWActivateDecoder(hDevice);
	if(sts != BC_STS_SUCCESS ) {
		DebugLog_Trace(LDIL_DBG,"DtsFWActivateDecoder: Failed [%x]\n",sts);
		return sts;
	}

	sts = DtsFWStartVideo(hDevice,
							Ctx->VidParams.VideoAlgo,
							Ctx->VidParams.FGTEnable,
							Ctx->VidParams.MetaDataEnable,
							Ctx->VidParams.Progressive,
							Ctx->VidParams.OptFlags);
	if(sts != BC_STS_SUCCESS) {
		DebugLog_Trace(LDIL_DBG,"DtsFWStartVideo: Failed [%x]\n",sts);
		return sts;
	}

	DtsLock(Ctx);
	Ctx->State = BC_DEC_STATE_START;
	Ctx->stopOutcome = DTS_STOP_NONE;
	Ctx->stopStatus = BC_STS_SUCCESS;
	Ctx->stopHostCleanupDone = false;
	DtsUnLock(Ctx);

	return sts;
}

DRVIFLIB_API BC_STATUS
DtsCloseDecoder(
    HANDLE  hDevice
    )
{
	BC_STATUS	sts = BC_STS_SUCCESS;
	DTS_LIB_CONTEXT		*Ctx = NULL;
	DTS_GET_CTX(hDevice,Ctx);

	if (!DtsChkPID(Ctx->ProcessID))
		return BC_STS_ERROR;
	DtsLifecycleCall control(Ctx);
	if (control.Status() != BC_STS_SUCCESS)
		return control.Status();
	DtsLock(Ctx);
	const bool control_owned = Ctx->stopInProgress;
	DtsUnLock(Ctx);
	if (control_owned)
		return BC_STS_BUSY;

	if(Ctx->State == BC_DEC_STATE_CLOSE)
	{
		return BC_STS_SUCCESS;
	}

	DtsLock(Ctx);
	const bool stop_needed = Ctx->State != BC_DEC_STATE_STOP ||
		Ctx->txQuiescing || DtsStopIsUnresolved(Ctx);
	DtsUnLock(Ctx);
	if (stop_needed)
	{
		/* Close is retirement, not an implicit retry of failed STOP. */
		bool control_busy = false;
		sts = DtsStopDecoderInternal(hDevice, false, &control_busy);
		if (control_busy)
			return sts;
	}
	DtsLock(Ctx);
	if (Ctx->State == BC_DEC_STATE_CLOSE && !Ctx->stopInProgress) {
		DtsUnLock(Ctx);
		return sts;
	}
	const bool cleanup_pending = Ctx->stopInProgress || Ctx->txPending ||
		Ctx->ProcOutPending || Ctx->CancelWaiting || Ctx->outputPhase != DTS_OUTPUT_IDLE ||
		(Ctx->stopOutcome != DTS_STOP_NONE && !Ctx->stopHostCleanupDone) ||
		(Ctx->State != BC_DEC_STATE_STOP &&
		 !(Ctx->State == BC_DEC_STATE_FLUSH && Ctx->stopHostCleanupDone));
	if (!cleanup_pending)
		Ctx->stopInProgress = true;
	DtsUnLock(Ctx);
	if (cleanup_pending)
		return sts == BC_STS_SUCCESS ? BC_STS_BUSY : sts;
	DtsStopAttempt attempt(Ctx);

	BC_STATUS close_sts = DtsFWCloseChannel(hDevice,Ctx->OpenRsp.channelId);
	/* The firmware channel is still owned when close was not confirmed.
	 * Keep the public and shared state open so a later caller can retry.
	 */
	if (close_sts != BC_STS_SUCCESS)
		return sts == BC_STS_SUCCESS ? close_sts : sts;

	DtsSetDecStat(false, Ctx->ProcessID);

	Ctx->State = BC_DEC_STATE_CLOSE;
	DtsLock(Ctx);
	Ctx->stopOutcome = DTS_STOP_NONE;
	Ctx->stopStatus = BC_STS_SUCCESS;
	Ctx->stopHostCleanupDone = false;
	Ctx->txQuiescing = false;
	DtsUnLock(Ctx);

	Ctx->LastPicNum = -1;
	Ctx->LastSessNum = -1;

	Ctx->EOSCnt = 0;
	Ctx->DrvStatusEOSCnt = 0;
	Ctx->bEOSCheck = false;
	Ctx->bEOS = false;
	DtsResetTxTracking(Ctx, true);

//	Ctx->InSampleCount = 0;

	/* Clear all pending lists.. */
	DtsClrPendMdataList(Ctx);

	/* Close the Input dump File */
	DumpInputSampleToFile(NULL,0);

	return sts;
}

static BC_STATUS DtsSetVideoParamsInternal(
	DTS_LIB_CONTEXT *Ctx,
	uint32_t		videoAlg,
	BOOL	FGTEnable,
	BOOL	MetaDataEnable,
	BOOL	Progressive,
    uint32_t     OptFlags)
{
	Ctx->VidParams.VideoAlgo = videoAlg;
	Ctx->VidParams.FGTEnable = FGTEnable;
	Ctx->VidParams.MetaDataEnable = MetaDataEnable;
	Ctx->VidParams.Progressive = Progressive;
	//Ctx->VidParams.Reserved = rsrv;
	//Ctx->VidParams.FrameRate = FrameRate;
	Ctx->VidParams.OptFlags = OptFlags;
	// SingleThreadedAppMode is bit 7 of OptFlags
	if(OptFlags & 0x80) {
		Ctx->SingleThreadedAppMode = 1;
	// Hard code BD mode as well as max frame rate mode for Link
		Ctx->VidParams.OptFlags |= 0xD1;
	}
	else
		Ctx->SingleThreadedAppMode = 0;
	if (Ctx->DevId == BC_PCI_DEVID_FLEA || Ctx->VidParams.VideoAlgo == BC_VID_ALGO_VC1MP)
		Ctx->VidParams.StreamType = BC_STREAM_TYPE_PES;
	else
		Ctx->VidParams.StreamType = BC_STREAM_TYPE_ES;
	return BC_STS_SUCCESS;
}

DRVIFLIB_API BC_STATUS
DtsSetVideoParams(
    HANDLE  hDevice,
	uint32_t		videoAlg,
	BOOL	FGTEnable,
	BOOL	MetaDataEnable,
	BOOL	Progressive,
    uint32_t     OptFlags
	)
{
	DTS_LIB_CONTEXT *Ctx = NULL;
	DTS_GET_CTX(hDevice,Ctx);
	DtsLifecycleCall control(Ctx);
	if (control.Status() != BC_STS_SUCCESS)
		return control.Status();
	DtsLock(Ctx);
	const bool input_busy = Ctx->inputCalls &&
		!pthread_equal(Ctx->inputThread, pthread_self());
	DtsUnLock(Ctx);
	if (input_busy)
		return BC_STS_BUSY;
	return DtsSetVideoParamsInternal(Ctx, videoAlg, FGTEnable,
		MetaDataEnable, Progressive, OptFlags);
}

DRVIFLIB_API BC_STATUS
DtsSetInputFormat(
    HANDLE hDevice,
    BC_INPUT_FORMAT *pInputFormat
    )
{
	DTS_LIB_CONTEXT *Ctx = NULL;
	uint32_t videoAlgo = BC_VID_ALGO_H264;
	uint32_t	ScaledWidth = 0;

	DTS_GET_CTX(hDevice,Ctx);
	if (!pInputFormat || (pInputFormat->metaDataSz && !pInputFormat->pMetaData) ||
		(pInputFormat->mSubtype == BC_MSUBTYPE_WMV3 && pInputFormat->metaDataSz < 4))
		return BC_STS_INV_ARG;
	DtsLifecycleCall control(Ctx);
	if (control.Status() != BC_STS_SUCCESS)
		return control.Status();
	DtsLock(Ctx);
	const bool input_busy = Ctx->inputCalls &&
		!pthread_equal(Ctx->inputThread, pthread_self());
	DtsUnLock(Ctx);
	if (input_busy)
		return BC_STS_BUSY;

	/* Prepare only format/converter state on a private handle. Do not copy the
	 * live context's mutex, rings, pending input or device ownership. Failure
	 * must preserve the previous configuration, including aliased metadata.
	 */
	DTS_LIB_CONTEXT candidate = {};
	BC_INPUT_FORMAT input = *pInputFormat;
	candidate.Sig = LIB_CTX_SIG;
	candidate.DevId = Ctx->DevId;
	candidate.VidParams = Ctx->VidParams;
	candidate.VidParams.pMetaData = NULL;
	candidate.VidParams.MetaDataSz = input.metaDataSz;
	candidate.VidParams.MediaSubType = input.mSubtype;
	candidate.VidParams.WidthInPixels = input.width;
	candidate.VidParams.HeightInPixels = input.height;
	candidate.VidParams.StartCodeSz = input.startCodeSz ? input.startCodeSz : BRCM_START_CODE_SIZE;
	candidate.EnableScaling = Ctx->EnableScaling;
	candidate.bEnable720pDropHalf = Ctx->bEnable720pDropHalf;
	if (input.metaDataSz) {
		candidate.VidParams.pMetaData = (uint8_t *)malloc(input.metaDataSz);
		if (!candidate.VidParams.pMetaData)
			return BC_STS_INSUFF_RES;
		memcpy(candidate.VidParams.pMetaData, input.pMetaData, input.metaDataSz);
	}

	if(input.mSubtype == BC_MSUBTYPE_H264 || input.mSubtype == BC_MSUBTYPE_AVC1)
	{
		videoAlgo = BC_VID_ALGO_H264;
	}
	else if (input.mSubtype == BC_MSUBTYPE_DIVX)
	{
		videoAlgo = BC_VID_ALGO_DIVX;
	}
	else if(input.mSubtype == BC_MSUBTYPE_MPEG2VIDEO)
	{
		videoAlgo = BC_VID_ALGO_MPEG2;
	}
	else if(input.mSubtype == BC_MSUBTYPE_WVC1 || input.mSubtype == BC_MSUBTYPE_WMVA || input.mSubtype == BC_MSUBTYPE_VC1)
	{
		videoAlgo = BC_VID_ALGO_VC1;
	}
	else if (input.mSubtype == BC_MSUBTYPE_WMV3)
	{
		videoAlgo = BC_VID_ALGO_VC1MP;	// Main Profile
	}

	BC_STATUS status = DtsSetVideoParamsInternal(&candidate, videoAlgo, input.FGTEnable,
		input.MetaDataEnable, input.Progressive, input.OptFlags);
	if (status == BC_STS_SUCCESS)
		status = DtsSetPESConverter(&candidate);
	if (status == BC_STS_SUCCESS)
		status = DtsCheckProfile(&candidate);
	if (status != BC_STS_SUCCESS) {
		DtsReleasePESConverter(&candidate);
		free(candidate.VidParams.pMetaData);
		return status;
	}

	if(candidate.DevId == BC_PCI_DEVID_FLEA)
	{
		if(candidate.SingleThreadedAppMode) {
			input.bEnableScaling = true;
			input.ScalingParams.sWidth = 1280;
		}
		if(input.bEnableScaling) {
			if((input.ScalingParams.sWidth > 1920)||
			   (input.ScalingParams.sWidth < 128))
				ScaledWidth = 1280;
			else
				ScaledWidth = input.ScalingParams.sWidth;

			candidate.EnableScaling = (ScaledWidth << 20) | (ScaledWidth << 8) |
					     input.bEnableScaling;
		} else {
			candidate.EnableScaling = 0;
		}

		candidate.bEnable720pDropHalf = 0;
	}

	/* Publish the complete converter/cache tuple in the same critical section
	 * used by DtsGetRecoveryStatus. The getter must never pair a new subtype or
	 * replay latch with freed/old header storage. Detach first, then release old
	 * allocations after unlocking so malloc internals are never under thLock. */
	DtsLock(Ctx);
	uint8_t *oldMetadata = Ctx->VidParams.pMetaData;
	uint8_t *oldHeaders = Ctx->PESConvParams.m_pSpsPpsBuf;
	uint8_t *oldPending = Ctx->PESConvParams.pStartcodePendBuff;
	Ctx->VidParams = candidate.VidParams;
	Ctx->PESConvParams = candidate.PESConvParams;
	Ctx->recoveryInjectSpsPps = false;
	Ctx->SingleThreadedAppMode = candidate.SingleThreadedAppMode;
	Ctx->EnableScaling = candidate.EnableScaling;
	Ctx->bEnable720pDropHalf = candidate.bEnable720pDropHalf;
	DtsUnLock(Ctx);
	free(oldHeaders);
	free(oldPending);
	free(oldMetadata);
	/* Preserve the historical single-thread scaling result, only on success. */
	pInputFormat->bEnableScaling = input.bEnableScaling;
	pInputFormat->ScalingParams.sWidth = input.ScalingParams.sWidth;
	return BC_STS_SUCCESS;
}

DRVIFLIB_API BC_STATUS
DtsGetVideoParams(
    HANDLE  hDevice,
	uint32_t		*videoAlg,
	BOOL	*FGTEnable,
	BOOL	*MetaDataEnable,
	BOOL	*Progressive,
    uint32_t     rsrv
	)
{
	DTS_LIB_CONTEXT		*Ctx = NULL;

	DTS_GET_CTX(hDevice,Ctx);

	if(!videoAlg || !FGTEnable || !MetaDataEnable || !Progressive)
		return BC_STS_INV_ARG;

	*videoAlg = Ctx->VidParams.VideoAlgo;
	*FGTEnable = Ctx->VidParams.FGTEnable;
	*MetaDataEnable = Ctx->VidParams.MetaDataEnable;
	*Progressive = Ctx->VidParams.Progressive;

	 return BC_STS_SUCCESS;

}

DRVIFLIB_API BC_STATUS
DtsFormatChange(
    HANDLE  hDevice,
	uint32_t videoAlg,
	BOOL	FGTEnable,
	BOOL	MetaDataEnable,
	BOOL	Progressive,
    uint32_t rsrv
	)
{
	return BC_STS_NOT_IMPL;
}

static BC_STATUS DtsStopDecoderInternal(HANDLE hDevice, bool retryUnsubmitted,
	bool *controlBusy)
{
	BC_STATUS	sts = BC_STS_SUCCESS;

	DTS_LIB_CONTEXT		*Ctx = NULL;
	DTS_GET_CTX(hDevice,Ctx);

	if (!DtsChkPID(Ctx->ProcessID))
		return BC_STS_ERROR;
	DtsLifecycleCall control(Ctx);
	if (control.Status() != BC_STS_SUCCESS)
		return control.Status();

	DtsLock(Ctx);
	if (Ctx->stopInProgress) {
		if (controlBusy)
			*controlBusy = true;
		DtsUnLock(Ctx);
		return BC_STS_BUSY;
	}
	if ((Ctx->State == BC_DEC_STATE_CLOSE || Ctx->State == BC_DEC_STATE_STOP) &&
	    Ctx->stopOutcome == DTS_STOP_NONE && !Ctx->txQuiescing)
	{
		DtsUnLock(Ctx);
		return BC_STS_SUCCESS;
	}
	if (Ctx->stopOutcome == DTS_STOP_CONFIRMED && Ctx->stopHostCleanupDone) {
		DtsUnLock(Ctx);
		return BC_STS_SUCCESS;
	}
	const bool retry = retryUnsubmitted && Ctx->stopOutcome == DTS_STOP_NOT_SUBMITTED;
	if (Ctx->stopHostCleanupDone && !retry) {
		sts = Ctx->stopStatus;
		DtsUnLock(Ctx);
		return sts;
	}
	if (Ctx->stopOutcome == DTS_STOP_NONE) {
		Ctx->stopStatus = BC_STS_SUCCESS;
	}
	if (!retry)
		sts = Ctx->stopStatus;
	Ctx->stopInProgress = true;
	Ctx->stopHostCleanupDone = false;
	/* Close admission before checking for pending output. Keep the mutex
	 * available while cancellation waits for the admitted owner to retire.
	 * FLUSH is also accepted by the firmware stop command below.
	 */
	DtsUnLock(Ctx);
	DtsStopAttempt attempt(Ctx);
	const bool pending_tx = DtsBeginTxQuiesce(Ctx);
	BC_STATUS cleanup_sts = DtsWaitForInputCalls(Ctx);
	if (cleanup_sts != BC_STS_SUCCESS)
		return sts == BC_STS_SUCCESS ? cleanup_sts : sts;

	// On LINK if the decoder is paused due to the RLL being full, un pause it before flush
	if(Ctx->DevId == BC_PCI_DEVID_LINK && Ctx->hw_paused) {
		const BC_STATUS resume_sts = DtsFWPauseVideo(hDevice,eC011_PAUSE_MODE_OFF);
		if (sts == BC_STS_SUCCESS)
			sts = resume_sts;
		Ctx->hw_paused = false;
	}
	/* A nonzero firmware flush also cancels the driver's CPB-space wait.
	 * Stop must not overtake an already-popped or active DMA transfer.
	 */
	if (pending_tx) {
		const BC_STATUS cancel_sts = DtsFWDecFlushChannel(hDevice, 2);
		if (sts == BC_STS_SUCCESS)
			sts = cancel_sts;
	}
	cleanup_sts = DtsWaitForTx(Ctx);
	if (cleanup_sts != BC_STS_SUCCESS)
		return sts == BC_STS_SUCCESS ? cleanup_sts : sts;
	/* Stop drops pending input as well as pictures. Otherwise a later
	 * Start would admit bytes that were queued in the old session.
	 */
	txBufFlush(&Ctx->circBuf);
	cleanup_sts = DtsCancelFetchOutInt(Ctx);
	if (cleanup_sts != BC_STS_SUCCESS)
		return sts == BC_STS_SUCCESS ? cleanup_sts : sts;
	if (sts == BC_STS_SUCCESS)
		sts = cleanup_sts;

	DtsLock(Ctx);
	const bool submit_stop = Ctx->stopOutcome == DTS_STOP_NONE ||
		Ctx->stopOutcome == DTS_STOP_PENDING || retry;
	if (submit_stop)
		Ctx->stopOutcome = DTS_STOP_PENDING;
	DtsUnLock(Ctx);
	if (submit_stop) {
		cleanup_sts = DtsFWStopVideo(hDevice,Ctx->OpenRsp.channelId, FALSE);
		if (sts == BC_STS_SUCCESS)
			sts = cleanup_sts;
	}

	cleanup_sts = DtsFlushRxCapture(hDevice, false);
	if (cleanup_sts != BC_STS_SUCCESS)
		return sts == BC_STS_SUCCESS ? cleanup_sts : sts;
	if (sts == BC_STS_SUCCESS)
		sts = cleanup_sts;

	DtsResetTxTracking(Ctx, sts == BC_STS_SUCCESS);
	DtsLock(Ctx);
	Ctx->stopHostCleanupDone = true;
	const bool confirmed = Ctx->stopOutcome == DTS_STOP_CONFIRMED;
	Ctx->State = confirmed ? BC_DEC_STATE_STOP : BC_DEC_STATE_FLUSH;
	Ctx->txQuiescing = !confirmed;
	DtsUnLock(Ctx);

	return sts;
}

DRVIFLIB_API BC_STATUS DtsStopDecoder(HANDLE hDevice)
{
	return DtsStopDecoderInternal(hDevice, true);
}

DRVIFLIB_API BC_STATUS
	DtsPauseDecoder(
    HANDLE  hDevice
    )
{
	BC_STATUS	sts = BC_STS_SUCCESS;
	DTS_LIB_CONTEXT		*Ctx = NULL;
	DTS_GET_CTX(hDevice,Ctx);

	if (!DtsChkPID(Ctx->ProcessID))
		return BC_STS_ERROR;
	DtsLifecycleCall control(Ctx);
	if (control.Status() != BC_STS_SUCCESS)
		return control.Status();
	DtsLock(Ctx);
	if (Ctx->txQuiescing || DtsStopIsUnresolved(Ctx)) {
		DtsUnLock(Ctx);
		return BC_STS_BUSY;
	}

	if (Ctx->State == BC_DEC_STATE_CLOSE)
	{
		DebugLog_Trace(LDIL_DBG, "DtsPauseDecoder: Decoder is not opened\n");
		DtsUnLock(Ctx);
		return BC_STS_DEC_NOT_OPEN;
	}
	if (Ctx->State == BC_DEC_STATE_STOP)
	{
		DebugLog_Trace(LDIL_DBG, "DtsPauseDecoder: Decoder is not started\n");
		DtsUnLock(Ctx);
		return BC_STS_DEC_NOT_STARTED;
	}

	if( Ctx->State == BC_DEC_STATE_PAUSE || Ctx->DevId == BC_PCI_DEVID_FLEA)
	{
		Ctx->State = BC_DEC_STATE_PAUSE;
		DtsUnLock(Ctx);
		return BC_STS_SUCCESS;
	}
	Ctx->stopInProgress = true;
	DtsUnLock(Ctx);
	DtsStopAttempt attempt(Ctx);

	sts = DtsFWPauseVideo(hDevice,eC011_PAUSE_MODE_ON);
	if(sts != BC_STS_SUCCESS )
	{
		DebugLog_Trace(LDIL_DBG,"DtsPauseDecoder: Failed\n");
		return sts;
	}
	Ctx->State = BC_DEC_STATE_PAUSE;


	return sts;
}

DRVIFLIB_API BC_STATUS
	DtsResumeDecoder(
    HANDLE  hDevice
    )
{
	BC_STATUS	sts = BC_STS_SUCCESS;

	DTS_LIB_CONTEXT		*Ctx = NULL;
	DTS_GET_CTX(hDevice,Ctx);

	if (!DtsChkPID(Ctx->ProcessID))
		return BC_STS_ERROR;
	DtsLifecycleCall control(Ctx);
	if (control.Status() != BC_STS_SUCCESS)
		return control.Status();
	DtsLock(Ctx);
	if (Ctx->txQuiescing || DtsStopIsUnresolved(Ctx)) {
		DtsUnLock(Ctx);
		return BC_STS_BUSY;
	}

	if (Ctx->State == BC_DEC_STATE_CLOSE)
	{
		DebugLog_Trace(LDIL_DBG, "DtsResumeDecoder: Decoder is not opened\n");
		DtsUnLock(Ctx);
		return BC_STS_DEC_NOT_OPEN;
	}

	if (Ctx->State == BC_DEC_STATE_STOP)
	{
		DebugLog_Trace(LDIL_DBG, "DtsResumeDecoder: Decoder is not started\n");
		DtsUnLock(Ctx);
		return BC_STS_DEC_NOT_STARTED;
	}

	if(Ctx->State == BC_DEC_STATE_START || Ctx->DevId == BC_PCI_DEVID_FLEA)
	{
		Ctx->State = BC_DEC_STATE_START;
		DtsUnLock(Ctx);
		return BC_STS_SUCCESS;
	}
	Ctx->stopInProgress = true;
	DtsUnLock(Ctx);
	DtsStopAttempt attempt(Ctx);

	sts = DtsFWPauseVideo(hDevice,eC011_PAUSE_MODE_OFF);
	if(sts != BC_STS_SUCCESS )
	{
		return sts;
	}

	Ctx->State = BC_DEC_STATE_START;

	return sts;
}


DRVIFLIB_API BC_STATUS
DtsSetVideoPID(
    HANDLE hDevice,
	uint32_t pid
    )
{
	return BC_STS_NOT_IMPL;
}

static void DtsSetCaptureThresholds(DTS_LIB_CONTEXT *Ctx,
				    BC_IOCTL_DATA *pIocData)
{
	uint32_t pool_count = Ctx->MpoolCnt ? Ctx->MpoolCnt :
		BC_MAX_SW_VOUT_BUFFS;
	uint32_t pause_limit = pool_count > 2 ? pool_count - 2 : 1;
	uint32_t pause = Ctx->DevId == BC_PCI_DEVID_FLEA ?
		pause_limit : PAUSE_DECODER_THRESHOLD;
	uint32_t resume = Ctx->DevId == BC_PCI_DEVID_FLEA ?
		FLEA_RT_PU_THRESHOLD : RESUME_DECODER_THRESHOLD;

	if (pause > pause_limit)
		pause = pause_limit;
	if (resume >= pause)
		resume = pause > 1 ? pause / 2 : 1;

	pIocData->u.RxCap.StartDeliveryThsh = RX_START_DELIVERY_THRESHOLD;
	pIocData->u.RxCap.PauseThsh = pause;
	pIocData->u.RxCap.ResumeThsh = resume;
}

static BC_STATUS DtsCaptureControlStatus(DTS_LIB_CONTEXT *Ctx, BC_STATUS sts)
{
	if (sts != BC_STS_BUSY)
		return sts;
	DtsLock(Ctx);
	/* Preserve the legacy state result when an ordinary Stop/Close already
	 * crossed its FLUSH barrier. Recovery and teardown contention must remain
	 * BUSY so no public capture call can hide an active reset/retirement. */
	const bool stopped = Ctx->controlOwner == DTS_CONTROL_LIFECYCLE &&
		DtsRecoveryAdmissionStatus(Ctx) == BC_STS_SUCCESS &&
		Ctx->State != BC_DEC_STATE_START;
	DtsUnLock(Ctx);
	return stopped ? BC_STS_DEC_NOT_STARTED : sts;
}


DRVIFLIB_API BC_STATUS
DtsStartCaptureImmidiate(HANDLE		hDevice,
						 uint32_t   Reserved)
{
	DTS_LIB_CONTEXT		*Ctx = NULL;
	BC_IOCTL_DATA		*pIocData = NULL;
	BC_STATUS			sts = BC_STS_SUCCESS;
	//unused uint32_t			Sz=0;

	DTS_GET_CTX(hDevice,Ctx);

	if (Ctx->ProcessID != getpid() || !DtsChkPID(Ctx->ProcessID))
		return BC_STS_ERROR;
	DtsLifecycleCall control(Ctx);
	if (control.Status() != BC_STS_SUCCESS)
		return DtsCaptureControlStatus(Ctx, control.Status());

	DtsLock(Ctx);
	if(Ctx->State != BC_DEC_STATE_START)
	{
		DebugLog_Trace(LDIL_DBG, "DtsStartCaptureImmidiate: Decoder is not started\n");
		sts = BC_STS_DEC_NOT_STARTED;
		goto done;
	}
	if (DtsStopIsUnresolved(Ctx)) {
		sts = BC_STS_BUSY;
		goto done;
	}

	if(Ctx->CfgFlags & BC_ADDBUFF_MOVE){
		sts = DtsMapYUVBuffs(Ctx);
		if(sts != BC_STS_SUCCESS){
			DebugLog_Trace(LDIL_DBG,"DtsMapYUVBuffs failed Sts:%d\n",sts);
			goto done;
		}
	}

	if(!(pIocData = DtsAllocIoctlData(Ctx))) {
		sts = BC_STS_INSUFF_RES;
		goto done;
	}

//	DebugLog_Trace(LDIL_DBG,"DbgOptions=%x\n", Ctx->RegCfg.DbgOptions);

	pIocData->u.RxCap.Rsrd = ST_CAP_IMMIDIATE;
	DtsSetCaptureThresholds(Ctx, pIocData);

	if( (sts=DtsDrvCmd(Ctx,BCM_IOC_START_RX_CAP,0,pIocData,FALSE)) != BC_STS_SUCCESS){
		DebugLog_Trace(LDIL_DBG,"DtsStartCapture: Failed: %d\n",sts);
	}

	DtsRelIoctlData(Ctx,pIocData);

done:
	DtsUnLock(Ctx);
	return sts;
}

DRVIFLIB_API BC_STATUS
DtsStartCapture(HANDLE  hDevice)
{
	DTS_LIB_CONTEXT		*Ctx = NULL;
	BC_IOCTL_DATA *pIocData = NULL;
	BC_STATUS	sts = BC_STS_SUCCESS;
	//unused uint32_t	Sz=0;

	DTS_GET_CTX(hDevice,Ctx);

	if (Ctx->ProcessID != getpid() || !DtsChkPID(Ctx->ProcessID))
		return BC_STS_ERROR;
	DtsLifecycleCall control(Ctx);
	if (control.Status() != BC_STS_SUCCESS)
		return DtsCaptureControlStatus(Ctx, control.Status());

	DtsLock(Ctx);
	if (Ctx->State != BC_DEC_STATE_START)
	{
		DebugLog_Trace(LDIL_DBG, "DtsStartCapture: Decoder is not started\n");
		sts = BC_STS_DEC_NOT_STARTED;
		goto done;
	}
	if (DtsStopIsUnresolved(Ctx)) {
		sts = BC_STS_BUSY;
		goto done;
	}

	if(Ctx->CfgFlags & BC_ADDBUFF_MOVE){
		sts = DtsMapYUVBuffs(Ctx);
		if(sts != BC_STS_SUCCESS){
			DebugLog_Trace(LDIL_DBG,"DtsMapYUVBuffs failed Sts:%d\n",sts);
			goto done;
		}
	}

	if(!(pIocData = DtsAllocIoctlData(Ctx))) {
		sts = BC_STS_INSUFF_RES;
		goto done;
	}

//	DebugLog_Trace(LDIL_DBG,"DbgOptions=%x\n", Ctx->RegCfg.DbgOptions);

	pIocData->u.RxCap.Rsrd = NO_PARAM;
	DtsSetCaptureThresholds(Ctx, pIocData);

	if( (sts=DtsDrvCmd(Ctx,BCM_IOC_START_RX_CAP,0,pIocData,FALSE)) != BC_STS_SUCCESS){
		DebugLog_Trace(LDIL_DBG,"DtsStartCapture: Failed: %d\n",sts);
	}

	DtsRelIoctlData(Ctx,pIocData);

done:
	DtsUnLock(Ctx);
	return sts;
}

DRVIFLIB_API BC_STATUS
DtsFlushRxCapture(
				HANDLE  hDevice,
				BOOL	bDiscardOnly)
{
	DTS_LIB_CONTEXT		*Ctx = NULL;
	BC_STATUS			Sts;
	BC_IOCTL_DATA *pIocData = NULL;

	DTS_GET_CTX(hDevice,Ctx);

	/* A mutex held by another parent thread is not recoverable after fork.
	 * Preserve the historical clean no-op while rejecting inherited driver
	 * ownership, and make that decision before touching the mutex. */
	if ((Ctx->ProcessID && Ctx->ProcessID != getpid()) ||
	    !DtsChkPID(Ctx->ProcessID))
		return Ctx->bMapOutBufDirty ? BC_STS_ERROR : BC_STS_SUCCESS;
	DtsLifecycleCall control(Ctx);
	if (control.Status() != BC_STS_SUCCESS)
		return control.Status();

	DtsLock(Ctx);
	if (!bDiscardOnly && (Ctx->outputPhase != DTS_OUTPUT_IDLE ||
	    Ctx->ProcOutPending || Ctx->CancelWaiting)) {
		Sts = DtsUnmapYUVBuffs(Ctx);
		DtsUnLock(Ctx);
		return Sts;
	}
	if (!Ctx->bMapOutBufDirty)
	{
		DtsUnLock(Ctx);
		return BC_STS_SUCCESS;
	}

	if (Ctx->State == BC_DEC_STATE_CLOSE)
	{
		DtsUnLock(Ctx);
		return BC_STS_DEC_NOT_OPEN;
	}
	if (!bDiscardOnly) {
		Sts = DtsUnmapYUVBuffs(Ctx);
		DtsUnLock(Ctx);
		return Sts;
	}


	if(!(pIocData = DtsAllocIoctlData(Ctx))) {
		DtsUnLock(Ctx);
		return BC_STS_INSUFF_RES;
	}

	pIocData->u.FlushRxCap.bDiscardOnly = bDiscardOnly;
	Sts = DtsDrvCmd(Ctx,BCM_IOC_FLUSH_RX_CAP, 0, pIocData, TRUE);
	DtsUnLock(Ctx);

	return Sts;
}


DRVIFLIB_INT_API BC_STATUS
DtsCancelTxRequest(
	HANDLE	hDevice,
	uint32_t Operation)
{
	DTS_LIB_CONTEXT *Ctx = NULL;
	DTS_GET_CTX(hDevice,Ctx);
	return DtsWaitForTx(Ctx);
}


DRVIFLIB_API BC_STATUS
DtsProcOutput(
    HANDLE  hDevice,
	uint32_t milliSecWait,
	BC_DTS_PROC_OUT *pOut)
{
	BC_STATUS	stRel,sts = BC_STS_SUCCESS;
	BC_DTS_PROC_OUT OutBuffs;
	uint32_t	width=0, savFlags=0;

	DTS_LIB_CONTEXT		*Ctx = NULL;

	DTS_GET_CTX(hDevice,Ctx);

	if (!DtsChkPID(Ctx->ProcessID))
		return BC_STS_ERROR;

	if(!pOut){
		DebugLog_Trace(LDIL_DBG,"DtsProcOutput: Invalid Arg!!\n");
		return BC_STS_INV_ARG;
	}
	DtsOutputCall output_call(Ctx);
	if (output_call.Status() != BC_STS_SUCCESS)
		return output_call.Status();

	if(!(Ctx->FixFlags & DTS_LOAD_FILE_PLAY_FW)){
		if(!(Ctx->RegCfg.DbgOptions & BC_BIT(6))){
			DebugLog_Trace(LDIL_DBG,"DtsProcOutput: Use NoCopy Interface for PIB encryption scheme\n");
			return BC_STS_ERR_USAGE;
		}
	}

	savFlags = pOut->PoutFlags;

	for (;;)
	{
		memset(&OutBuffs,0,sizeof(OutBuffs));

		sts = DtsFetchOutInCall(Ctx,&OutBuffs,milliSecWait);

		if(sts != BC_STS_SUCCESS)
		{
			if(sts == BC_STS_TIMEOUT)
			{
				/* BCM70015 sends a firmware timing marker on explicit drain.
				 * A timeout can occur before queued input or DMA completes;
				 * it must not manufacture EOS. Retain the BCM70012 fallback.
				 */
				if (Ctx->DevId == BC_PCI_DEVID_LINK &&
					Ctx->bEOSCheck == true && Ctx->bEOS == false)
				{
					if(milliSecWait)
						Ctx->EOSCnt = BC_EOS_PIC_COUNT;
					else
						Ctx->EOSCnt ++;

					if(Ctx->EOSCnt >= BC_EOS_PIC_COUNT)
					{
						/* Mark this picture as end of stream..*/
						pOut->PicInfo.flags |= VDEC_FLAG_LAST_PICTURE;
						Ctx->bEOS = TRUE;
						DebugLog_Trace(LDIL_DBG,"HIT EOS with counter\n");
					}
				}

				sts = BC_STS_NO_DATA;
			}
			// Have to make sure EOS is returned correctly for FLEA
			// In case of Flea the EOS picture has no data and hence the status will be STS_NO_DATA
			if(OutBuffs.PicInfo.flags & VDEC_FLAG_EOS)
				pOut->PicInfo.flags |= (VDEC_FLAG_EOS|VDEC_FLAG_LAST_PICTURE);
			/* Fetch handles its own admission on failure. A BUSY
			 * or stopped caller never owned the shared output buffer.
			 */
			return sts;
		}
		sts = DtsCommitFetchedOutput(Ctx, &OutBuffs);
		if (sts != BC_STS_SUCCESS)
			return sts;
		sts = DtsPrepareFetchedOutput(Ctx, &OutBuffs);
		if (sts != BC_STS_SUCCESS) {
			stRel = DtsRelRxBuff(Ctx,&Ctx->pOutData->u.RxBuffs,FALSE);
			return output_call.PublishCopy(stRel == BC_STS_SUCCESS ? sts : stRel);
		}

		pOut->discCnt = 0;
		Ctx->bEOS = FALSE;
		pOut->PoutFlags |= OutBuffs.PoutFlags;
		if (!(pOut->PoutFlags & BC_POUT_FLAGS_MODE))
			pOut->b422Mode = OutBuffs.b422Mode;
		/* Copying the discontinuity count */
		if(OutBuffs.discCnt)
			pOut->discCnt = OutBuffs.discCnt;

		if(pOut->PoutFlags & BC_POUT_FLAGS_FMT_CHANGE){
			if(pOut->PoutFlags & BC_POUT_FLAGS_PIB_VALID){
				pOut->PicInfo = OutBuffs.PicInfo;
			}

			/* Update Counters */
			DtsUpdateOutStats(Ctx,pOut);
			DtsUpdateVidParams(Ctx, pOut);

			DtsRelRxBuff(Ctx,&Ctx->pOutData->u.RxBuffs,true);

			return output_call.PublishCopy(BC_STS_FMT_CHANGE);
		}

		if (Ctx->DevId == BC_PCI_DEVID_FLEA)
		{
			if (Ctx->bEOSCheck == true && Ctx->bEOS == true && (OutBuffs.PicInfo.flags & VDEC_FLAG_EOS))
			{
				Ctx->bEOS = true;
				pOut->PicInfo.flags |= (VDEC_FLAG_LAST_PICTURE|VDEC_FLAG_EOS);
				DebugLog_Trace(LDIL_DBG,"HIT EOS with PIB tag\n");
			}
		}
		else
		{
			if (DtsCheckRptPic(Ctx, &OutBuffs) == true)
			{
				stRel = DtsRelRxBuff(Ctx,&Ctx->pOutData->u.RxBuffs,FALSE);
				if (stRel != BC_STS_SUCCESS)
					return stRel;
				return BC_STS_NO_DATA;
			}
		}

		if(pOut->DropFrames)
		{
			/* Return this dropped buffer before fetching another frame. */
			stRel = DtsRelRxBuff(Ctx,&Ctx->pOutData->u.RxBuffs,FALSE);

			if(stRel != BC_STS_SUCCESS)
			{
				DebugLog_Trace(LDIL_DBG,
					"DtsProcOutput: Failed to repost output buffer.. %x\n",
					stRel);
				return stRel;
			}
			pOut->DropFrames--;

			/* Get back the original flags */
			pOut->PoutFlags = savFlags;

			// Added support for dropping of single pictures by Adobe FP
			if(Ctx->SingleThreadedAppMode && (pOut->DropFrames == 0))
			{
				pOut->PicInfo.timeStamp = 0;
				pOut->PicInfo.picture_number = OutBuffs.PicInfo.picture_number;
				if(OutBuffs.PicInfo.flags & VDEC_FLAG_EOS)
					pOut->PicInfo.flags |= (VDEC_FLAG_EOS|VDEC_FLAG_LAST_PICTURE);
				return output_call.PublishCopy(BC_STS_SUCCESS);
			}
			sts = DtsContinueOutputCall(Ctx);
			if (sts != BC_STS_SUCCESS)
				return sts;
			continue;
		}
		break;
	}


	if(pOut->AppCallBack && pOut->hnd && (OutBuffs.PoutFlags & BC_POUT_FLAGS_PIB_VALID))
	{
		/* Merge in and out flags */
		OutBuffs.PoutFlags |= pOut->PoutFlags;
		width = Ctx->HWOutPicWidth;
		pOut->AppCallBack(pOut->hnd,
					width,
					OutBuffs.PicInfo.height,
					0,
					&OutBuffs);
	}

	if (pOut->PoutFlags & BC_POUT_FLAGS_MODE) {
		sts = DtsCopyFormat(Ctx,pOut,&OutBuffs);
	} else {
		if(Ctx->b422Mode) {
			sts = DtsCopyRawDataToOutBuff(Ctx,pOut,&OutBuffs);
		}else{
			if(pOut->PoutFlags & BC_POUT_FLAGS_YV12){
				sts = DtsCopyNV12ToYV12(Ctx,pOut,&OutBuffs);
			}else {
				sts = DtsCopyNV12(Ctx,pOut,&OutBuffs);
			}
		}
	}

	if(pOut->PoutFlags & BC_POUT_FLAGS_PIB_VALID){
		pOut->PicInfo = OutBuffs.PicInfo;
	}

	/* Update Counters */
	DtsUpdateOutStats(Ctx,pOut);

	/* We need to release the buffers even if we fail to copy..*/
	stRel = DtsRelRxBuff(Ctx,&Ctx->pOutData->u.RxBuffs,FALSE);

	if(sts != BC_STS_SUCCESS){
		DebugLog_Trace(LDIL_DBG,"DtsProcOutput: Failed to copy out buffs.. %x\n", sts);
		return output_call.PublishCopy(sts);
	}

	return output_call.PublishCopy(stRel);
}

#ifdef _ENABLE_CODE_INSTRUMENTATION_
static void dts_swap_buffer(uint32_t *dst, uint32_t *src, uint32_t cnt)
{
	uint32_t i=0;

	for (i=0; i < cnt; i++){
		dst[i] = BC_SWAP32(src[i]);
	}

}
#endif

DRVIFLIB_API BC_STATUS
DtsProcOutputNoCopy(
    HANDLE  hDevice,
	uint32_t		milliSecWait,
	BC_DTS_PROC_OUT *pOut
)
{

	BC_STATUS	sts = BC_STS_SUCCESS;

	DTS_LIB_CONTEXT		*Ctx = NULL;

	DTS_GET_CTX(hDevice,Ctx);

	if (!DtsChkPID(Ctx->ProcessID))
		return BC_STS_ERROR;

	if(!pOut){
		return BC_STS_INV_ARG;
	}
	DtsOutputCall output_call(Ctx, false, pOut);
	if (output_call.Status() != BC_STS_SUCCESS)
		return output_call.Status();
	for (;;) {
		BC_DTS_PROC_OUT fetched = *pOut;
		fetched.Ybuff = fetched.UVbuff = NULL;
		fetched.bPibEnc = Ctx->DevId == BC_PCI_DEVID_LINK;
		fetched.b422Mode = Ctx->b422Mode;

		if( (sts = DtsFetchOutInCall(Ctx,&fetched,milliSecWait)) != BC_STS_SUCCESS){
			DebugLog_Trace(LDIL_DBG,"DtsProcOutput: No Active Channels\n");
			/* Embedded EOS is a control output: FETCH owns no buffer after
			 * DtsSetupProcOutInfo changes the status to NO_DATA, but its PIB is
			 * still part of the legacy NoCopy result.  Publish that parsed
			 * metadata while keeping the borrowed pointers cleared. */
			*pOut = fetched;
				/* In case of a peek..*/
			if((sts == BC_STS_TIMEOUT) && !(milliSecWait) ){
				sts = BC_STS_NO_DATA;
			}
			/* No output ownership on failure: do not update statistics or
			 * satisfy DropFrames by releasing another caller's buffer.
			 */
			return sts;
		}
		/*
		 * If the PIB is not encrypted then we can direclty go
		 * to the PIB and get the information weather the Picture
		 * is encrypted or not.
		 */

		if(fetched.PoutFlags & BC_POUT_FLAGS_FMT_CHANGE){
			sts = output_call.Publish(&fetched, BC_STS_FMT_CHANGE);
			if (sts == BC_STS_FMT_CHANGE)
				*pOut = fetched;
			else
				pOut->Ybuff = pOut->UVbuff = NULL;
			return sts;
		}

		if(fetched.DropFrames){
			sts = DtsCommitFetchedOutput(Ctx, &fetched);
			if (sts != BC_STS_SUCCESS)
				return sts;
			sts = DtsPrepareFetchedOutput(Ctx, &fetched);
			if (sts != BC_STS_SUCCESS) {
				const BC_STATUS released =
					DtsRelRxBuff(Ctx,&Ctx->pOutData->u.RxBuffs,FALSE);
				sts = output_call.Publish(&fetched,
					released == BC_STS_SUCCESS ? sts : released);
				pOut->Ybuff = pOut->UVbuff = NULL;
				return sts;
			}
			DtsUpdateOutStats(Ctx,&fetched);
			/* We need to release the buffers even if we fail to copy..*/
			sts = DtsRelRxBuff(Ctx,&Ctx->pOutData->u.RxBuffs,FALSE);
			fetched.Ybuff = fetched.UVbuff = NULL;

			if(sts != BC_STS_SUCCESS)
			{
				DebugLog_Trace(LDIL_DBG,"DtsProcOutput: Failed to release buffs.. %x\n", sts);
				sts = output_call.Publish(&fetched, sts);
				pOut->Ybuff = pOut->UVbuff = NULL;
				return sts;
			}
			pOut->DropFrames--;
			sts = DtsContinueOutputCall(Ctx);
			if (sts != BC_STS_SUCCESS)
				return sts;
		}
		else {
			sts = output_call.Publish(&fetched, BC_STS_SUCCESS);
			if (sts == BC_STS_SUCCESS)
				*pOut = fetched;
			else
				pOut->Ybuff = pOut->UVbuff = NULL;
			return sts;
		}

	}
}

DRVIFLIB_API BC_STATUS
DtsReleaseOutputBuffs(
    HANDLE  hDevice,
	PVOID   Reserved,
	BOOL	fChange)
{
	DTS_LIB_CONTEXT		*Ctx = NULL;

	DTS_GET_CTX(hDevice,Ctx);

	if(fChange)
		return BC_STS_SUCCESS;
	DtsOutputCall release_call(Ctx, true);
	if (release_call.Status() != BC_STS_SUCCESS)
		return release_call.Status();

	return DtsRelRxBuff(Ctx, &Ctx->pOutData->u.RxBuffs, FALSE);
}

DRVIFLIB_INT_API BC_STATUS
DtsSendData( HANDLE  hDevice ,
				 uint8_t *pUserData,
				 uint32_t ulSizeInBytes,
				 uint64_t timeStamp,
				 BOOL encrypted
			    )
{
	DTS_LIB_CONTEXT		*Ctx = NULL;

	DTS_GET_CTX(hDevice,Ctx);

	for (;;) {
		/* Serialize the final enqueue with Stop/Flush admission closure.
		 * A packet prepared before FLUSH must not refill the reset ring.
		 * Lock order is thLock -> ring flushLock -> ring pushpopLock;
		 * no wait or DMA ioctl runs while thLock is held.
		 */
		DtsLock(Ctx);
		const BC_STATUS recoverySts = DtsRecoveryAdmissionStatus(Ctx);
		if (recoverySts != BC_STS_SUCCESS) {
			DtsUnLock(Ctx);
			return recoverySts;
		}
		if (Ctx->txQuiescing || DtsStopIsUnresolved(Ctx) ||
		    (Ctx->State != BC_DEC_STATE_START && Ctx->State != BC_DEC_STATE_PAUSE)) {
			DtsUnLock(Ctx);
			return BC_STS_IO_USER_ABORT;
		}
		/* No amount of consumer progress can make this enqueue fit. */
		if (ulSizeInBytes > Ctx->circBuf.totalSize) {
			DtsUnLock(Ctx);
			return BC_STS_INSUFF_RES;
		}
		if (ulSizeInBytes <= DtsTxFreeSize(hDevice)) {
			const BC_STATUS sts = txBufPush(&Ctx->circBuf, pUserData, ulSizeInBytes);
			if (sts == BC_STS_SUCCESS)
				Ctx->txBytesEnqueued += ulSizeInBytes;
			DtsUnLock(Ctx);
			return sts;
		}
		DtsUnLock(Ctx);
		usleep(5 * 1000);
	}
}

DRVIFLIB_API uint32_t
DtsTxFreeSize( HANDLE hDevice )
{
	DTS_LIB_CONTEXT                *Ctx = NULL;

	DTS_GET_CTX(hDevice,Ctx);

	// Match txBufPop's lock order: reset uses flushLock, while byte
	// accounting in push/pop uses pushpopLock.
	pthread_mutex_lock(&Ctx->circBuf.flushLock);
	pthread_mutex_lock(&Ctx->circBuf.pushpopLock);
	uint32_t freeSize = Ctx->circBuf.freeSize;
	pthread_mutex_unlock(&Ctx->circBuf.pushpopLock);
	pthread_mutex_unlock(&Ctx->circBuf.flushLock);
	return freeSize;
}

DRVIFLIB_API BC_STATUS
DtsSendSPESPkt(HANDLE  hDevice ,
			   uint64_t timeStamp,
			   BOOL encrypted
			   )
{
	BC_STATUS	sts = BC_STS_SUCCESS;
	DTS_LIB_CONTEXT		*Ctx = NULL;

	DTS_INPUT_MDATA		*im = NULL;
	//unused uint8_t *temp=NULL;
	uint32_t ulSize = 0;
	uint8_t* pSPESPkt = NULL;
	uint8_t	i = 0;

	DTS_GET_CTX(hDevice,Ctx);
	DtsInputCall input_call(Ctx);
	if (input_call.Status() != BC_STS_SUCCESS)
		return input_call.Status();

	if (Ctx->State == BC_DEC_STATE_CLOSE)
	{
		return BC_STS_DEC_NOT_OPEN;
	}

	if (Ctx->State == BC_DEC_STATE_STOP || Ctx->State == BC_DEC_STATE_FLUSH)
	{
		return BC_STS_DEC_NOT_STARTED;
	}

	while (i <20)
	{
		sts = DtsPrepareMdata(Ctx, timeStamp, &im, &pSPESPkt, &ulSize);
		if (sts == BC_STS_SUCCESS)
			break;
		i++;
		bc_sleep_ms(2);
	}
	if (sts == BC_STS_SUCCESS)
	{
		if(Ctx->VidParams.VideoAlgo == BC_VID_ALGO_VC1MP)
		{
			if(posix_memalign((void**)&pSPESPkt, 8, 32+sizeof(BC_PES_HDR_FORMAT))){
				DebugLog_Trace(LDIL_DBG, "DtsProcInput: Failed to alloc mem for  ASFHdr for SPES:%x\n", sts);
				return BC_STS_INSUFF_RES;
			}

			sts =DtsPrepareMdataASFHdr(Ctx, im, pSPESPkt);

			if(sts != BC_STS_SUCCESS){
				free(pSPESPkt);
				DebugLog_Trace(LDIL_DBG, "DtsProcInput: Failed to Prepare ASFHdr for SPES:%x\n", sts);
				return sts;
			}
			ulSize = 32+sizeof(BC_PES_HDR_FORMAT);
		}
		sts = DtsSendData(hDevice, pSPESPkt, ulSize, 0, encrypted);
		if(Ctx->VidParams.VideoAlgo == BC_VID_ALGO_VC1MP)
			free(pSPESPkt);

		if(sts != BC_STS_SUCCESS){
			DebugLog_Trace(LDIL_DBG, "DtsProcInput: Failed to send Spes hdr:%x\n", sts);
			DtsFreeMdata(Ctx,im,TRUE);
			return sts;
		}

		sts = DtsInsertMdata(Ctx,im);

		if(sts != BC_STS_SUCCESS){
			DebugLog_Trace(LDIL_DBG, "DtsProcInput: DtsInsertMdata failed\n");
		}
	}
	return sts;
}

DRVIFLIB_API BC_STATUS
DtsAlignSendData( HANDLE  hDevice ,
				 uint8_t *pUserData,
				 uint32_t ulSizeInBytes,
				 uint64_t timeStamp,
				 BOOL encrypted
			    )
{
	BC_STATUS	sts = BC_STS_SUCCESS;
	DTS_LIB_CONTEXT		*Ctx = NULL;
	uint8_t	oddBytes = 0;

	uint8_t* alignBuf;

	DTS_GET_CTX(hDevice,Ctx);
	DtsInputCall input_call(Ctx);
	if (input_call.Status() != BC_STS_SUCCESS)
		return input_call.Status();

	alignBuf = Ctx->alignBuf;

	uint32_t ulRestBytes = ulSizeInBytes;
	uint32_t ulDeliverBytes = 0;
	uint32_t ulPktLength;
	uint32_t ulPktHeaderSz;
	uint32_t ulUsedDataBytes = 0;

	uint8_t* pDeliverBuf = NULL;
	uint8_t* pRestDataPt = pUserData;
	uint8_t bAddPTS = 1;
	uint8_t bPrivData = Ctx->PESConvParams.m_bPESPrivData;
	uint8_t bExtData = Ctx->PESConvParams.m_bPESExtField;
	uint8_t bStuffing = Ctx->PESConvParams.m_bStuffing;

	uint8_t* pPrivData = Ctx->PESConvParams.m_pPESPrivData;
	uint8_t* pExtData = Ctx->PESConvParams.m_pPESExtField;
	uint32_t nExtDataLen = Ctx->PESConvParams.m_nPESExtLen;
	uint32_t	nStuffingBytes = Ctx->PESConvParams.m_nStuffingBytes;


	uint8_t j = 0;
	uint8_t k = 0;

	//SoftRave (VC-1 S/M and Divx) EOS Timing Marker
	if ((timeStamp || Ctx->PESConvParams.m_bSoftRave))
	{
		bAddPTS = 1;
	}
	else
	{
		bAddPTS = 0;
	}
	while (ulRestBytes)
	{
		DtsLock(Ctx);
		const BC_STATUS recoverySts = DtsRecoveryAdmissionStatus(Ctx);
		if (recoverySts != BC_STS_SUCCESS) {
			DtsUnLock(Ctx);
			return recoverySts;
		}
		const bool inputAdmitted = !Ctx->txQuiescing && !DtsStopIsUnresolved(Ctx) &&
			(Ctx->State == BC_DEC_STATE_START || Ctx->State == BC_DEC_STATE_PAUSE);
		const bool paused = Ctx->State == BC_DEC_STATE_PAUSE;
		DtsUnLock(Ctx);
		if (!inputAdmitted)
			break;
		sts = BC_STS_SUCCESS;

		pDeliverBuf = pRestDataPt;

		if (paused)
		{
			/* A five-second uninterruptible pause held an admitted input call
			 * until the recovery drain timeout boundary. Poll admission so a PM
			 * epoch retires the call promptly without holding thLock. */
			usleep(10 * 1000);
			continue;
		}

		/* Copy the non-DWORD aligned buffer first */
		oddBytes = (uint8_t)((uintptr_t)pDeliverBuf % 4);

		if (Ctx->VidParams.StreamType == BC_STREAM_TYPE_ES)
		{
			// SPES Mode
			ulDeliverBytes = ulRestBytes > ALIGN_BUF_SIZE ? ALIGN_BUF_SIZE : ulRestBytes;
			if (timeStamp)
			{
				sts = DtsSendSPESPkt(hDevice, timeStamp, encrypted);
				if (sts != BC_STS_SUCCESS)
					return sts;
			}
			timeStamp = 0;

			if(oddBytes)
			{
				if (ulRestBytes > ALIGN_BUF_SIZE)
					ulDeliverBytes = ALIGN_BUF_SIZE - oddBytes;
				else
					ulDeliverBytes = ulRestBytes;

				memcpy(alignBuf, pDeliverBuf, ulDeliverBytes);
				pDeliverBuf = alignBuf;
			}
			ulUsedDataBytes = ulDeliverBytes;
		}
		else if (Ctx->VidParams.StreamType == BC_STREAM_TYPE_PES)
		{
			if (Ctx->DevId == BC_PCI_DEVID_LINK && timeStamp)
			{
				sts = DtsSendSPESPkt(hDevice, timeStamp, encrypted);
				if (sts != BC_STS_SUCCESS)
					return sts;
				timeStamp = 0;
				bAddPTS =0;
			}

			if (bAddPTS)
				ulPktHeaderSz = 5;
			else
				ulPktHeaderSz = 0;
			if (bPrivData || bExtData)
			     ulPktHeaderSz += 1;

			if (bPrivData)
				ulPktHeaderSz += 16;

			if (bExtData)
				ulPktHeaderSz += nExtDataLen + 1;

			if (bStuffing)
				ulPktHeaderSz += nStuffingBytes;
			ulPktLength = ulRestBytes + 3 + ulPktHeaderSz;

			if (ulPktLength > MAX_RE_PES_BOUND)
			{
				ulPktLength = MAX_RE_PES_BOUND;
			}
			ulUsedDataBytes = ulPktLength - ulPktHeaderSz - 3;
			ulDeliverBytes = ulPktLength + 6;


			memcpy(alignBuf,(uint8_t *)b_pes_header, 9);
			*((uint16_t *)(alignBuf + 4)) = WORD_SWAP((uint16_t)ulPktLength);
			*(alignBuf + 8) = (uint8_t)ulPktHeaderSz;

			j = 9;
			if (bAddPTS)
			{
				*(alignBuf + 7) = 0x80;
				PTS2MakerBit5Bytes(alignBuf + j, timeStamp);
				j += 5;
			}

			if (bPrivData || bExtData)
			{
				*(alignBuf + 7) |= 0x01;
			    *(alignBuf + j) = 0x00;
				if (bPrivData)
					*(alignBuf + j) |= 0x80;
				if (bExtData)
					*(alignBuf + j) |= 0x01;
				j++;
			}

			if (bPrivData)
			{
				memcpy(alignBuf + j, pPrivData, 16);
				j += 16;
			}
			if (bExtData)
			{
				*(alignBuf + j) = 0x80 | nExtDataLen;
				j++;
				memcpy(alignBuf + j, pExtData, nExtDataLen);
				j += nExtDataLen;
			}

			if (bStuffing)
			{
				for (k = 0; k < nStuffingBytes; k ++, j++)
				{
					*(alignBuf + j) = 0xFF;
				}
			}
			memcpy(alignBuf + j, pDeliverBuf, ulUsedDataBytes);
			pDeliverBuf = alignBuf;

		}

		if (ulDeliverBytes)
		{
			sts = DtsSendData(hDevice,pDeliverBuf ,ulDeliverBytes, 0, encrypted);

			if(sts == BC_STS_BUSY )
			{
				DtsLock(Ctx);
				const bool recoveryBlocked = DtsRecoveryBlocksCaller(Ctx);
				const bool stopped = Ctx->State == BC_DEC_STATE_STOP;
				DtsUnLock(Ctx);
				if (recoveryBlocked || stopped)
				{
					break;
				}
				sleep(2);
			}
			else if(sts == BC_STS_SUCCESS)
			{
				ulRestBytes -= ulUsedDataBytes;
				pRestDataPt += ulUsedDataBytes;
				bAddPTS = 0;
				timeStamp = 0;
				bPrivData = 0;
				bExtData = 0;
				pPrivData = NULL;
				pExtData = NULL;
				nExtDataLen = 0;
				bStuffing = 0;
				nStuffingBytes = 0;
			}
			else if(sts == BC_STS_IO_USER_ABORT)
			{
				/* Unsent/canceled bytes were not accepted by the ring. */
				break;
			} else
				break; // On any other error condition
		}
	}

	/* A concurrent stop/flush can end the loop before all bytes are queued,
	 * including before its first iteration. Do not report that as accepted.
	 */
	if (sts == BC_STS_SUCCESS && ulRestBytes != 0) {
		DtsLock(Ctx);
		const BC_STATUS recoverySts = DtsRecoveryAdmissionStatus(Ctx);
		sts = recoverySts != BC_STS_SUCCESS ? recoverySts : BC_STS_IO_USER_ABORT;
		DtsUnLock(Ctx);
	}
	return sts;
}

DRVIFLIB_API BC_STATUS
DtsProcInput( HANDLE  hDevice ,
				 uint8_t *pUserData,
				 uint32_t ulSizeInBytes,
				 uint64_t timeStamp,
				 BOOL  encrypted
			    )
{
	BC_STATUS	sts = BC_STS_SUCCESS;
	DTS_LIB_CONTEXT		*Ctx = NULL;

	uint32_t Offset = 0;
	bool clearRecoveryConfigAfterInput = false;
	const uint8_t *const cacheInput = pUserData;
	const uint32_t cacheInputSize = ulSizeInBytes;

	DTS_GET_CTX(hDevice,Ctx);

	if (!DtsChkPID(Ctx->ProcessID))
		return BC_STS_ERROR;
	DtsInputCall input_call(Ctx);
	if (input_call.Status() != BC_STS_SUCCESS)
		return input_call.Status();

	DtsLock(Ctx);
	const BC_STATUS recoverySts = DtsRecoveryAdmissionStatus(Ctx);
	if (recoverySts != BC_STS_SUCCESS || Ctx->txQuiescing || DtsStopIsUnresolved(Ctx)) {
		DtsUnLock(Ctx);
		return recoverySts != BC_STS_SUCCESS ? recoverySts : BC_STS_BUSY;
	}
	if (Ctx->State == BC_DEC_STATE_FLUSH)
		Ctx->State = BC_DEC_STATE_START;
	DtsUnLock(Ctx);

	if (Ctx->State != BC_DEC_STATE_START)
	{
		if (!DtsIsDecOpened(0))
		{
			/* Lifecycle calls wait for TX/output owners, which need thLock
			 * to retire. The input/control owner serializes this sequence.
			 */
			sts = DtsOpenDecoder(hDevice, Ctx->VidParams.StreamType);
			if (sts == BC_STS_SUCCESS)
			{
				sts = DtsStartDecoder(hDevice);
				if (sts == BC_STS_SUCCESS)
					sts = DtsStartCapture(hDevice);
			}
			if (sts != BC_STS_SUCCESS)
				return sts;
		}
	}

	DtsLock(Ctx);
	Ctx->bEOSCheck = false;
	Ctx->bEOS = false;
	Ctx->eosTxStart = Ctx->txBytesEnqueued;
	Ctx->eosTxFence = Ctx->txBytesEnqueued;
	Ctx->eosDrainGeneration++;
	Ctx->eosTxComplete = false;
	DtsUnLock(Ctx);

	// According to ASF spec special timestamps can be 0x1FFFFFFFF or 0x1FFFFFFFE
	// NAREN - FIXME - should we add support for these pre-roll timestamps
	if (Ctx->DevId == BC_PCI_DEVID_FLEA && timeStamp != 0xFFFFFFFFFFFFFFFFULL)
	{
		timeStamp /= 10000;
	}

	if((Ctx->VidParams.MediaSubType == BC_MSUBTYPE_WVC1) || (Ctx->VidParams.MediaSubType == BC_MSUBTYPE_WMV3) || (Ctx->VidParams.MediaSubType == BC_MSUBTYPE_WMVA))
		DtsCheckKeyFrame(hDevice, pUserData);

	DtsLock(Ctx);
	const bool recoveryInjectSpsPps = Ctx->recoveryInjectSpsPps;
	DtsUnLock(Ctx);
	if (Ctx->PESConvParams.m_bAddSpsPps)
	{

		if  (Ctx->PESConvParams.m_pSpsPpsBuf != NULL && Ctx->PESConvParams.m_iSpsPpsLen != 0)
		{
			//Check if SequenceHeader is already delivered within input data.
			//Unnecessary SpsPps input will cause no timestamp output.
			bool inputHasConfig = false;
			if (!Ctx->SingleThreadedAppMode || recoveryInjectSpsPps)
				inputHasConfig = (recoveryInjectSpsPps ||
					Ctx->VidParams.MediaSubType == BC_MSUBTYPE_H264) ?
					DtsCheckRecoverySpsPps(hDevice, pUserData, ulSizeInBytes) :
					DtsCheckSpsPps(hDevice, pUserData, ulSizeInBytes);
			if ((!Ctx->SingleThreadedAppMode || recoveryInjectSpsPps) &&
				!inputHasConfig)
			{
				// Used to flag softRAVE special handling of Sequence level information as well as preroll samples
				if (Ctx->PESConvParams.m_bSoftRave)
					sts = DtsAlignSendData(hDevice, Ctx->PESConvParams.m_pSpsPpsBuf, Ctx->PESConvParams.m_iSpsPpsLen, 0xFFFFFFFFFFFFFFFFULL, 0);
				else
					sts = DtsAlignSendData(hDevice, Ctx->PESConvParams.m_pSpsPpsBuf, Ctx->PESConvParams.m_iSpsPpsLen, 0, 0);
				if (sts != BC_STS_SUCCESS)
					return sts;
				if (recoveryInjectSpsPps) {
					DtsLock(Ctx);
					Ctx->recoveryInjectSpsPps = false;
					DtsUnLock(Ctx);
				}
			}
			else if (recoveryInjectSpsPps && inputHasConfig)
				clearRecoveryConfigAfterInput = true;
		}
		/* If in-band configuration suppresses recovery replay, retain both
		 * latches until that AU is actually accepted so a failed enqueue can
		 * be retried without stranding recoveryInjectSpsPps. */
		if (!clearRecoveryConfigAfterInput)
			Ctx->PESConvParams.m_bAddSpsPps = false;
	}
	//Use 0xFFFFFFFD for SoftRave context instead of zero
	// For pre-roll samples for softRAVE context we send the timestamp as 0 to the softRAVE to handle.
	// The FW is responsible to add the timestamp back.
	//Both 0x1FFFFFFFF and 0x1FFFFFFFE when right shifted by XPT become 0xFFFFFFFF which will be treated as special PTS by softrave to replace the PTS entry with Start code entry
	if (Ctx->PESConvParams.m_bSoftRave && (((timeStamp&0x1FFFFFFFFULL) == 0x1FFFFFFFFULL) || ((timeStamp&0x1FFFFFFFFULL) == 0x1FFFFFFFEULL)))
	{
		//timeStamp = 0x0;
		timeStamp = 0xFFFFFFFDULL;
	}

	if ((sts = DtsAddStartCode(hDevice, &pUserData, &ulSizeInBytes, &timeStamp)) != BC_STS_SUCCESS)
	{
		if (sts == BC_STS_IO_XFR_ERROR)
			return BC_STS_SUCCESS;
		return BC_STS_ERROR;
	}

	/* Parsing may run concurrently with the status worker. Re-check the
	 * recovery lease at the last common point before any packet path can
	 * enqueue bytes, so a late S3 transition is reported as retryable BUSY.
	 */
	DtsLock(Ctx);
	const BC_STATUS finalRecoverySts = DtsRecoveryAdmissionStatus(Ctx);
	DtsUnLock(Ctx);
	if (finalRecoverySts != BC_STS_SUCCESS)
		return finalRecoverySts;

	if (Ctx->VidParams.StreamType == BC_STREAM_TYPE_PES || timeStamp == 0)
	{
		sts = DtsAlignSendData(hDevice, pUserData, ulSizeInBytes, timeStamp, encrypted);
	}
	else
	{
		if(!Ctx->SingleThreadedAppMode && (DtsFindStartCode(hDevice, pUserData, ulSizeInBytes, &Offset) != BC_STS_SUCCESS))
		{
			timeStamp = 0;
			Offset = 0;
		}
		if(Offset == 0)
		{
			sts = DtsAlignSendData(hDevice, pUserData, ulSizeInBytes, timeStamp, encrypted);
		}
		else
		{
			if ((sts=DtsAlignSendData(hDevice, pUserData, Offset, 0, encrypted)) != BC_STS_SUCCESS)
				return sts;

			if(ulSizeInBytes > Offset)
				sts = DtsAlignSendData(hDevice, pUserData+Offset, ulSizeInBytes-Offset, timeStamp, encrypted);
			else
				sts = BC_STS_SUCCESS;
		}
	}
	/* Commit H.264 recovery state only after this AU was accepted.  Until then
	 * the previous complete cache must survive a transport failure or a late
	 * suspend that flushes bytes queued earlier in this call. */
	if (sts == BC_STS_SUCCESS && Ctx->VidParams.MediaSubType == BC_MSUBTYPE_H264)
		(void)DtsCacheSpsPps(hDevice, cacheInput, cacheInputSize);
	if (sts == BC_STS_SUCCESS && clearRecoveryConfigAfterInput) {
		DtsLock(Ctx);
		Ctx->recoveryInjectSpsPps = false;
		Ctx->PESConvParams.m_bAddSpsPps = false;
		DtsUnLock(Ctx);
	}
	return sts;
}

DRVIFLIB_API BC_STATUS
DtsGetColorPrimaries( HANDLE  hDevice ,
				 uint32_t *colorPrimaries
				 )
{
	return BC_STS_NOT_IMPL;
}

DRVIFLIB_API BC_STATUS
DtsSendEOS( HANDLE  hDevice, uint32_t Op
			  )
{
	BC_STATUS			sts = BC_STS_SUCCESS;
	DTS_LIB_CONTEXT		*Ctx = NULL;

	DTS_GET_CTX(hDevice,Ctx);
	DtsInputCall input_call(Ctx);
	if (input_call.Status() != BC_STS_SUCCESS)
		return input_call.Status();

	uint8_t	*pEOS;
	uint32_t nEOSLen;
	uint32_t	nTag;
	uint64_t drainGeneration = 0;
	bool wakeTxWorker = false;
	const bool softRave = Ctx->PESConvParams.m_bSoftRave;
	const bool divxTailBoundary = softRave && Ctx->DevId == BC_PCI_DEVID_FLEA &&
		Ctx->VidParams.MediaSubType == BC_MSUBTYPE_DIVX;

	if (Ctx->State == BC_DEC_STATE_CLOSE)
	{
		return BC_STS_DEC_NOT_OPEN;
	}

	if (Ctx->State == BC_DEC_STATE_STOP || Ctx->State == BC_DEC_STATE_FLUSH)
	{
		return BC_STS_DEC_NOT_STARTED;
	}

	/* A new drain attempt is armed only after every EOS fragment is queued.
	 * Mode 5 retains its existing behavior of not changing EOS checking.
	 */
	if (Op == 0 || Op == 5)
	{
		DtsLock(Ctx);
		if (Op == 0)
			Ctx->bEOSCheck = false;
		Ctx->eosTxStart = Ctx->txBytesEnqueued;
		Ctx->eosTxFence = Ctx->txBytesEnqueued;
		Ctx->eosDrainGeneration++;
		drainGeneration = Ctx->eosDrainGeneration;
		Ctx->eosTxComplete = false;
		DtsUnLock(Ctx);
	}

	/* The first MPEG-4 sequence end needs a zero PTS to commit the final VOP.
	 * The timing marker and repeated sequence ends remain control-only PES.
	 */
	if (!divxTailBoundary)
		Ctx->PESConvParams.m_bSoftRave = false;

	Ctx->PESConvParams.m_bPESExtField = false;
	Ctx->PESConvParams.m_bPESPrivData = false;

	Ctx->PESConvParams.m_pPESExtField = NULL;
	Ctx->PESConvParams.m_pPESPrivData = NULL;

	//SoftRave (VC-1 S/M and Divx) EOS Timing Marker

	if ((Ctx->VidParams.MediaSubType == BC_MSUBTYPE_WVC1) ||
		(Ctx->VidParams.MediaSubType == BC_MSUBTYPE_WMV3) ||
		(Ctx->VidParams.MediaSubType == BC_MSUBTYPE_WMVA) ||
		(Ctx->VidParams.MediaSubType == BC_MSUBTYPE_VC1)
		)
	{
		Ctx->PESConvParams.m_bPESExtField = true;
		Ctx->PESConvParams.m_nPESExtLen = 2;
		Ctx->PESConvParams.m_pPESExtField = ExtData;
	}

	if (Ctx->VidParams.MediaSubType == BC_MSUBTYPE_MPEG1VIDEO ||
		Ctx->VidParams.MediaSubType == BC_MSUBTYPE_MPEG2VIDEO)
	{
		pEOS = eos_mpeg;
		nEOSLen = 4;
	}
	else if (Ctx->VidParams.MediaSubType == BC_MSUBTYPE_DIVX || Ctx->VidParams.MediaSubType == BC_MSUBTYPE_DIVX311)
	{
		pEOS = eos_divx;
		nEOSLen = 8;
	}
	else if (Ctx->DevId == BC_PCI_DEVID_LINK && Ctx->VidParams.MediaSubType == BC_MSUBTYPE_WMV3)
	{
		pEOS = eos_vc1_spmp_link;
		nEOSLen = 32;
	}
	else
	{
		pEOS = eos_avc_vc1;
		nEOSLen = 8;
	}

	sts = DtsAlignSendData(hDevice, pEOS, nEOSLen, 0, 0);
	if (sts != BC_STS_SUCCESS)
		goto eos_cleanup;
	Ctx->PESConvParams.m_bSoftRave = false;

	/* Only send timing marker if this is FLEA */
	/* LINK Support LAST_PICTURE and does not need timing marker */
	if (Op == 0)
	{
		if (Ctx->DevId == BC_PCI_DEVID_FLEA)
		{
			Ctx->PESConvParams.m_bPESPrivData = true;
			Ctx->PESConvParams.m_pPESPrivData = btp_video_done_es_private;
			if (Ctx->PESConvParams.m_bPESExtField == true)
			{
				Ctx->PESConvParams.m_bStuffing = false;
				Ctx->PESConvParams.m_nStuffingBytes = 0;
			}
			else
			{
				Ctx->PESConvParams.m_bStuffing = true;
				Ctx->PESConvParams.m_nStuffingBytes = 3;
			}

			nTag = 0xffff0000 | (0xffff & 0x0001);
			btp_video_done_es[0]   = 0x00;
			btp_video_done_es[13]   = (nTag >> 24) & 0xff;
			btp_video_done_es[14]   = (nTag >> 16) & 0xff;
			btp_video_done_es[15]   = (nTag >> 8)  & 0xff;
			btp_video_done_es[16]   = nTag & 0xff;

			sts = DtsAlignSendData(hDevice, btp_video_done_es, sizeof(btp_video_done_es), 0, 0);
			if (sts != BC_STS_SUCCESS)
				goto eos_cleanup;
			Ctx->PESConvParams.m_bPESPrivData = false;
			Ctx->PESConvParams.m_pPESPrivData = NULL;
			Ctx->PESConvParams.m_bStuffing = false;
			Ctx->PESConvParams.m_nStuffingBytes = 0;

			sts = DtsAlignSendData(hDevice, pEOS, nEOSLen, 0, 0);
			if (sts != BC_STS_SUCCESS)
				goto eos_cleanup;
			sts = DtsAlignSendData(hDevice, pEOS, nEOSLen, 0, 0);
			if (sts != BC_STS_SUCCESS)
				goto eos_cleanup;
		}
	}
	if (Op == 0 || Op == 5)
	{
		DtsLock(Ctx);
		if (Ctx->eosDrainGeneration != drainGeneration)
		{
			DtsUnLock(Ctx);
			sts = BC_STS_IO_USER_ABORT;
			goto eos_cleanup;
		}
		Ctx->eosTxFence = Ctx->txBytesEnqueued;
		if (Op == 0) {
			Ctx->bEOSCheck = true;
			wakeTxWorker = true;
		}
		Ctx->eosTxComplete = Ctx->bEOSCheck && !Ctx->txDmaFault &&
			Ctx->eosTxFence > Ctx->eosTxStart &&
			Ctx->txBytesRetired >= Ctx->eosTxFence;
		DtsUnLock(Ctx);
		/* The worker may have consumed the final drain bytes before EOS mode
		 * was published. Wake its latched idle wait so the 5 ms EOS cadence
		 * takes effect immediately instead of inheriting the normal idle wait. */
		if (wakeTxWorker)
			txBufWake(&Ctx->circBuf);
	}

eos_cleanup:
	/* Do not carry partial timing-marker metadata into later input. */
	Ctx->PESConvParams.m_bSoftRave = softRave;
	Ctx->PESConvParams.m_bPESExtField = false;
	Ctx->PESConvParams.m_pPESExtField = NULL;
	Ctx->PESConvParams.m_bPESPrivData = false;
	Ctx->PESConvParams.m_pPESPrivData = NULL;
	Ctx->PESConvParams.m_bStuffing = false;
	Ctx->PESConvParams.m_nStuffingBytes = 0;
	//SoftRave (VC-1 S/M and Divx) EOS Timing Marker

	return sts;
}

DRVIFLIB_API BC_STATUS
DtsFlushInput( HANDLE  hDevice ,
				 uint32_t Op )
{
	BC_STATUS			sts = BC_STS_SUCCESS;
	DTS_LIB_CONTEXT		*Ctx = NULL;

	DTS_GET_CTX(hDevice,Ctx);
	DebugLog_Trace(LDIL_DBG, "Flush called with opcode %u\n", Op);

	if (!DtsChkPID(Ctx->ProcessID))
		return BC_STS_ERROR;
	if (Op == 0 || Op == 5) {
		DtsLock(Ctx);
		const BC_STATUS admission = DtsRecoveryAdmissionStatus(Ctx);
		const bool closed = Ctx->State == BC_DEC_STATE_CLOSE;
		DtsUnLock(Ctx);
		if (admission != BC_STS_SUCCESS)
			return admission;
		if (closed)
			return BC_STS_DEC_NOT_OPEN;
		return DtsSendEOS(hDevice, Op);
	}
	DtsLifecycleCall control(Ctx);
	if (control.Status() != BC_STS_SUCCESS)
		return control.Status();

	if (Ctx->State == BC_DEC_STATE_CLOSE)
	{
		return BC_STS_DEC_NOT_OPEN;
	}
	DtsLock(Ctx);
	const bool stop_pending = DtsStopIsUnresolved(Ctx);
	DtsUnLock(Ctx);
	if (stop_pending)
		return BC_STS_BUSY;

	if(Op != 0 && Op != 5)
	{
		const bool pending_tx = DtsBeginTxQuiesce(Ctx);
		const BC_STATUS input_sts = DtsWaitForInputCalls(Ctx);
		if (input_sts != BC_STS_SUCCESS)
			return input_sts;
		/* An input admitted before quiesce may still pass through its legacy
		 * one-shot metadata cleanup while we wait. Rearm only after that owner
		 * has retired, under the same lock used by input/recovery publication.
		 * Op 3 returns early below, so it must share this placement. */
		DtsLock(Ctx);
		Ctx->PESConvParams.m_bAddSpsPps = true;
		DtsUnLock(Ctx);
		if (Op != 3 && pending_tx) {
			if(Ctx->DevId == BC_PCI_DEVID_LINK && Ctx->hw_paused) {
				sts = DtsFWPauseVideo(hDevice,eC011_PAUSE_MODE_OFF);
				Ctx->hw_paused = false;
			}
			/* This first flush cancels a driver waiting for CPB space.
			 * Bytes already popped may still enter DMA after it. Wait for
			 * that owner, then establish the final firmware flush below.
			 */
			const BC_STATUS cancel_sts = DtsFWDecFlushChannel(hDevice, Op == 4 ? 2 : Op);
			if (sts == BC_STS_SUCCESS)
				sts = cancel_sts;
		}
		const BC_STATUS wait_sts = DtsCancelTxRequest(hDevice, Op);
		if (wait_sts != BC_STS_SUCCESS)
			return sts == BC_STS_SUCCESS ? wait_sts : sts;
		txBufFlush(&Ctx->circBuf);
		DtsResetTxTracking(Ctx, false);
		if (Op == 3) {
			DtsLock(Ctx);
			Ctx->txQuiescing = false;
			DtsUnLock(Ctx);
			return sts;
		}
		DtsClrPendMdataList(Ctx);
	}

	// On LINK if the decoder is paused due to the RLL being full, un pause it before flush
	if(Ctx->DevId == BC_PCI_DEVID_LINK && Ctx->hw_paused) {
		sts = DtsFWPauseVideo(hDevice,eC011_PAUSE_MODE_OFF);
		Ctx->hw_paused = false;
	}

	BC_STATUS cleanup_sts = BC_STS_SUCCESS;
	if(Op == 4)
		cleanup_sts = DtsFWDecFlushChannel(hDevice,2);
	else if (Op != 0 && Op != 5)
		cleanup_sts = DtsFWDecFlushChannel(hDevice,Op);
	if (sts == BC_STS_SUCCESS)
		sts = cleanup_sts;

	if(Op != 0 && Op != 5)
	{
		if (Ctx->State != BC_DEC_STATE_CLOSE)
		{
			/* Stop waits for pending output to retire. The output thread
			 * needs thLock in DtsDecPend, so do not hold it across that
			 * wait. Stop/Close lock their shared structures internally.
			 */
			cleanup_sts = DtsStopDecoder(hDevice);
			if (sts == BC_STS_SUCCESS)
				sts = cleanup_sts;
			cleanup_sts = DtsCloseDecoder(hDevice);
			if (sts == BC_STS_SUCCESS)
				sts = cleanup_sts;
		}
	}

	Ctx->LastPicNum = -1;
	Ctx->LastSessNum = -1;
	Ctx->EOSCnt = 0;
	Ctx->DrvStatusEOSCnt = 0;
	Ctx->bEOS = FALSE;
//	Ctx->InSampleCount = 0;

	Ctx->PESConvParams.m_lStartCodeDataSize = 0;

	/* Cleanup must still run after an error, but its later success must not
	 * tell the caller that the failed firmware operation completed.
	 */
	return sts;
}

DRVIFLIB_API BC_STATUS
DtsSetRateChange(HANDLE  hDevice ,
				 uint32_t rate,
				 uint8_t direction )
{
	BC_STATUS			sts = BC_STS_SUCCESS;
	DTS_LIB_CONTEXT		*Ctx = NULL;

	DTS_GET_CTX(hDevice,Ctx);
	DtsLifecycleCall control(Ctx);
	if (control.Status() != BC_STS_SUCCESS)
		return control.Status();

	//For Rate Change
	uint32_t mode = 0;
	uint32_t HostTrickModeEnable = 0;

	if (Ctx->State == BC_DEC_STATE_CLOSE)
	{
		return BC_STS_DEC_NOT_OPEN;
	}

	if (Ctx->State == BC_DEC_STATE_STOP || Ctx->State == BC_DEC_STATE_FLUSH)
	{
		return BC_STS_DEC_NOT_STARTED;
	}

	if (!DtsChkPID(Ctx->ProcessID))
		return BC_STS_ERROR;
	if (!rate)
		return BC_STS_INV_ARG;

	//Change Rate Value for Version 1.1
	//Rate: Specifies the new rate x 10000
	//Rate is the inverse of speed. For example, if the playback speed is 2x, the rate is 1/2, so the Rate member is set to 5000.

	float fRate = float(1) / ((float)rate / (float)10000);

	//Mode Decision
	if(fRate < 1)
	{
		//Slow
		LONG Rate = LONG(float(1) / fRate);

		mode = eC011_SKIP_PIC_IPB_DECODE;
		HostTrickModeEnable = 1;

		sts = DtsFWSetHostTrickMode(hDevice,HostTrickModeEnable);
		if(sts != BC_STS_SUCCESS)
		{
			DebugLog_Trace(LDIL_DBG,"DtsSetRateChange:DtsFWSetHostTrickMode Failed\n");
			return sts;
		}

		sts = DtsFWSetSkipPictureMode(hDevice,mode);
		if(sts != BC_STS_SUCCESS)
		{
			DebugLog_Trace(LDIL_DBG,"DtsSetRateChange: DtsFWSetSkipPictureMode Failed\n");
			return sts;
		}

		sts = DtsFWSetSlowMotionRate(hDevice, Rate);

		if(sts != BC_STS_SUCCESS)
		{
			DebugLog_Trace(LDIL_DBG,"DtsSetRateChange: Set Slow Forward Failed\n");

			return sts;
		}
	}
	else
	{
		//Fast
		LONG Rate = (LONG)fRate;

		//For I-Frame Only Trick Mode
		//Direction: 0: Forward, 1: Backward
		if(Ctx->DevId == BC_PCI_DEVID_FLEA)
		{
			//Flea
			if((Rate <= 2) && (direction == 0))
			{
				DebugLog_Trace(LDIL_DBG,"DtsSetRateChange: Set Normal Speed\n");
				mode = eC011_SKIP_PIC_IPB_DECODE;
				HostTrickModeEnable = 0;
			}
			else if((Rate <= 3) && (direction == 0))
			{
				DebugLog_Trace(LDIL_DBG,"DtsSetRateChange: Set Fast Speed for IP-Frame Only\n");
				mode = eC011_SKIP_PIC_IP_DECODE;
				HostTrickModeEnable = 1;
			}
			else
			{
				//I-Frame Only for Fast Forward and All Speed Backward
				DebugLog_Trace(LDIL_DBG,"DtsSetRateChange: Set Very Fast Speed for I-Frame Only\n");
				mode = eC011_SKIP_PIC_I_DECODE;
				HostTrickModeEnable = 1;
			}
		}
		else
		{
			//Link
			if((Rate == 1) && (direction == 0))
			{
				DebugLog_Trace(LDIL_DBG,"DtsSetRateChange: Set Normal Speed\n");
				mode = eC011_SKIP_PIC_IPB_DECODE;
				HostTrickModeEnable = 0;
				if(fRate > 1 && fRate < 2)
				{
					//Nav is giving I instead of IBP for 1.4x or 1.6x
					DebugLog_Trace(LDIL_DBG,"DtsSetRateChange: Set 1.x I only\n");
					mode = eC011_SKIP_PIC_I_DECODE;
					HostTrickModeEnable = 1;
				}
			}
			else
			{
				//I-Frame Only for Fast Forward and All Speed Backward
				DebugLog_Trace(LDIL_DBG,"DtsSetRateChange: Set Very Fast Speed for I-Frame Only\n");
				mode = eC011_SKIP_PIC_I_DECODE;
				HostTrickModeEnable = 1;
			}
		}

		sts = DtsFWSetHostTrickMode(hDevice,HostTrickModeEnable);
		if(sts != BC_STS_SUCCESS)
		{
			DebugLog_Trace(LDIL_DBG,"DtsSetRateChange:DtsFWSetHostTrickMode Failed\n");
			return sts;
		}

		sts = DtsFWSetSkipPictureMode(hDevice,mode);
		if(sts != BC_STS_SUCCESS)
		{
			DebugLog_Trace(LDIL_DBG,"DtsSetRateChange: DtsFWSetSkipPictureMode Failed\n");
			return sts;
		}

		if(Ctx->DevId != BC_PCI_DEVID_FLEA)
		{
			sts = DtsFWSetFFRate(hDevice, Rate);
			if(sts != BC_STS_SUCCESS)
			{
				DebugLog_Trace(LDIL_DBG,"DtsSetRateChange: Set Fast Forward Failed\n");
				return sts;
			}
		}
	}

	return BC_STS_SUCCESS;
}

//Set FF Rate for Catching Up
DRVIFLIB_API BC_STATUS
DtsSetFFRate(HANDLE  hDevice ,
				 uint32_t rate)
{
	BC_STATUS			sts = BC_STS_SUCCESS;
	DTS_LIB_CONTEXT		*Ctx = NULL;

	DTS_GET_CTX(hDevice,Ctx);
	DtsLifecycleCall control(Ctx);
	if (control.Status() != BC_STS_SUCCESS)
		return control.Status();

	//For Rate Change
	uint32_t mode = 0;
	uint32_t HostTrickModeEnable = 0;

	if (Ctx->State == BC_DEC_STATE_CLOSE)
	{
		return BC_STS_DEC_NOT_OPEN;
	}

	if (Ctx->State == BC_DEC_STATE_STOP || Ctx->State == BC_DEC_STATE_FLUSH)
	{
		return BC_STS_DEC_NOT_STARTED;
	}

	if (!DtsChkPID(Ctx->ProcessID))
		return BC_STS_ERROR;
	if (!rate)
		return BC_STS_INV_ARG;

	//Change Rate Value for Version 1.1
	//Rate: Specifies the new rate x 10000
	//Rate is the inverse of speed. For example, if the playback speed is 2x, the rate is 1/2, so the Rate member is set to 5000.

	float fRate = float(1) / ((float)rate / (float)10000);

	//Mode Decision
	if(fRate < 1)
	{
		//Error
		//Only for FF

		DebugLog_Trace(LDIL_DBG,"DtsSetFFRate: NOT Support Slow Motion\n");

		return BC_STS_INV_ARG;
	}
	else
	{
		//Fast
		LONG Rate = (LONG)fRate;

		if(Ctx->DevId == BC_PCI_DEVID_FLEA)
		{
			if(Rate > FLEA_MAX_TRICK_MODE_SPEED)
			{
				Rate = FLEA_MAX_TRICK_MODE_SPEED;
			}
		}

		//IPB Mode for Catching Up
		mode = eC011_SKIP_PIC_IPB_DECODE;

		if(Rate == 1)
		{
			//Normal Mode
			DebugLog_Trace(LDIL_DBG,"DtsSetFFRate: Set Normal Speed\n");

			HostTrickModeEnable = 0;
		}
		else
		{
			//Fast Forward
			DebugLog_Trace(LDIL_DBG,"DtsSetFFRate: Set Fast Forward\n");

			HostTrickModeEnable = 1;
		}

		sts = DtsFWSetHostTrickMode(hDevice, HostTrickModeEnable);
		if(sts != BC_STS_SUCCESS)
		{
			DebugLog_Trace(LDIL_DBG,"DtsSetFFRate:DtsFWSetHostTrickMode Failed\n");
			return sts;
		}

		sts = DtsFWSetSkipPictureMode(hDevice, mode);
		if(sts != BC_STS_SUCCESS)
		{
			DebugLog_Trace(LDIL_DBG,"DtsSetFFRate: DtsFWSetSkipPictureMode Failed\n");
			return sts;
		}

		sts = DtsFWSetFFRate(hDevice, Rate);
		if(sts != BC_STS_SUCCESS)
		{
			DebugLog_Trace(LDIL_DBG,"DtsSetFFRate: Set Fast Forward Failed\n");
			return sts;
		}
	}

	return BC_STS_SUCCESS;
}

DRVIFLIB_API BC_STATUS
DtsSetSkipPictureMode( HANDLE  hDevice ,
						uint32_t SkipMode )
{
	BC_STATUS			sts = BC_STS_SUCCESS;
	DTS_LIB_CONTEXT		*Ctx = NULL;

	DTS_GET_CTX(hDevice,Ctx);
	DtsLifecycleCall control(Ctx);
	if (control.Status() != BC_STS_SUCCESS)
		return control.Status();

	sts = DtsFWSetSkipPictureMode(hDevice,SkipMode);

	if(sts != BC_STS_SUCCESS)
	{
		DebugLog_Trace(LDIL_DBG,"DtsSetSkipPictureMode: Set Picture Mode Failed, %d\n",SkipMode);
		return sts;
	}

	return BC_STS_SUCCESS;
}

DRVIFLIB_API BC_STATUS
	DtsSetIFrameTrickMode(
	HANDLE  hDevice
	)
{
	BC_STATUS			sts = BC_STS_SUCCESS;
	DTS_LIB_CONTEXT		*Ctx = NULL;

	DTS_GET_CTX(hDevice,Ctx);
	DtsLifecycleCall control(Ctx);
	if (control.Status() != BC_STS_SUCCESS)
		return control.Status();

	sts = DtsFWSetHostTrickMode(hDevice,1);
	if(sts != BC_STS_SUCCESS)
	{
		DebugLog_Trace(LDIL_DBG,"DtsSetIFrameTrickMode: DtsFWSetHostTrickMode Failed\n");
		return sts;
	}

	sts = DtsFWSetSkipPictureMode(hDevice,eC011_SKIP_PIC_I_DECODE);
	if(sts != BC_STS_SUCCESS)
	{
		DebugLog_Trace(LDIL_DBG,"DtsSetIFrameTrickMode: DtsFWSetSkipPictureMode Failed\n");
		return sts;
	}
	return BC_STS_SUCCESS;
}


DRVIFLIB_API BC_STATUS
	DtsStepDecoder(
    HANDLE  hDevice
    )
{
	BC_STATUS	sts = BC_STS_SUCCESS;

	DTS_LIB_CONTEXT		*Ctx = NULL;
	DTS_GET_CTX(hDevice,Ctx);
	DtsLifecycleCall control(Ctx);
	if (control.Status() != BC_STS_SUCCESS)
		return control.Status();

	if( Ctx->State != BC_DEC_STATE_PAUSE ) {
		DebugLog_Trace(LDIL_DBG,"DtsStepDecoder: Failed because Decoder is Not Paused\n");
		return BC_STS_ERR_USAGE;
	}

	sts = DtsFWFrameAdvance(hDevice);
	if(sts != BC_STS_SUCCESS )
	{
		DebugLog_Trace(LDIL_DBG,"DtsStepDecoder: Failed \n");
		return sts;
	}

	return sts;
}

#if 0
DRVIFLIB_API BC_STATUS
	DtsIs422Supported(
    HANDLE  hDevice,
	uint8_t*		bSupported)
{
	BC_STATUS	sts = BC_STS_SUCCESS;

	DTS_LIB_CONTEXT		*Ctx = NULL;
	DTS_GET_CTX(hDevice,Ctx);

	if(Ctx->DevId == BC_PCI_DEVID_LINK || Ctx->DevId == BC_PCI_DEVID_FLEA)
	{
		*bSupported = 1;
	}
	else
	{
		*bSupported = 0;
	}
	return sts;
}

DRVIFLIB_API BC_STATUS
DtsSet422Mode(HANDLE hDevice, uint8_t	Mode422)
{
	BC_STATUS	sts = BC_STS_SUCCESS;
	DTS_LIB_CONTEXT		*Ctx = NULL;

	DTS_GET_CTX(hDevice,Ctx);

	if(Ctx->DevId != BC_PCI_DEVID_LINK)
	{
		return sts;
	}

	Ctx->b422Mode = (BC_OUTPUT_FORMAT)Mode422;

	sts = DtsSetLinkIn422Mode(hDevice);

	return sts;
}

#endif

DRVIFLIB_API BC_STATUS
DtsIsEndOfStream(
    HANDLE  hDevice,
    uint8_t*	bEOS
)
{
	BC_STATUS	sts = BC_STS_SUCCESS;
	DTS_LIB_CONTEXT		*Ctx = NULL;
	DTS_GET_CTX(hDevice,Ctx);
	if (!bEOS)
		return BC_STS_INV_ARG;

	DtsLock(Ctx);
	*bEOS = FALSE;
	sts = DtsRecoveryAdmissionStatus(Ctx);
	if (sts == BC_STS_SUCCESS)
		*bEOS = Ctx->bEOS;
	DtsUnLock(Ctx);

	return sts;
}

DRVIFLIB_API BC_STATUS
DtsSetColorSpace(
	HANDLE  hDevice,
	BC_OUTPUT_FORMAT	Mode422
)
{
	return DtsSetOutputColorSpace(hDevice, Mode422);
}

DRVIFLIB_API BC_STATUS
DtsGetDILPath(
    HANDLE  hDevice,
	char		*DilPath,
	uint32_t		size
    )
{
	DTS_LIB_CONTEXT		*Ctx = NULL;
	uint32_t *ptemp=NULL;
	DTS_GET_CTX(hDevice,Ctx);

	if(!DilPath || !size){
		return BC_STS_INV_ARG;
	}

	/* if the first 4 bytes are zero, then the dil path in the context
	is not yet set. Hence go ahead and look at registry and update the
	context. and then just copy the dil path from context.
	*/
	ptemp = (uint32_t*)&Ctx->DilPath[0];
	if(!(*ptemp))
		DtsGetFirmwareFiles(Ctx);

	if (snprintf(DilPath, size, "%s", Ctx->DilPath) >= (int)size)
		return BC_STS_INV_ARG;


	return BC_STS_SUCCESS;
}

DRVIFLIB_API BC_STATUS
DtsDropPictures( HANDLE  hDevice ,
                uint32_t Pictures )
{
	BC_STATUS			sts = BC_STS_SUCCESS;
	DTS_LIB_CONTEXT		*Ctx = NULL;

	DTS_GET_CTX(hDevice,Ctx);
	DtsLifecycleCall control(Ctx);
	if (control.Status() != BC_STS_SUCCESS)
		return control.Status();
	if (Ctx->State == BC_DEC_STATE_CLOSE)
	{
		return BC_STS_DEC_NOT_OPEN;
	}

	if (Ctx->State == BC_DEC_STATE_STOP || Ctx->State == BC_DEC_STATE_FLUSH)
	{
		return BC_STS_DEC_NOT_STARTED;
	}

	sts = DtsFWDrop(hDevice,Pictures);

	if(sts != BC_STS_SUCCESS)
	{
		DebugLog_Trace(LDIL_DBG,"DtsDropPictures: Set Picture Mode Failed, %d\n",Pictures);
		return sts;
	}

	return BC_STS_SUCCESS;
}

DRVIFLIB_API BC_STATUS
DtsGetDriverStatus( HANDLE  hDevice,
	                BC_DTS_STATUS *pStatus)
{
	BC_DTS_STATS temp;
	BC_STATUS ret;
	BOOL realHWCPBSize = false; // Always report DIL buffer size unless explicitly asked to report HW size
	BOOL readTXinfoOnly = false; // Report only TX information
	uint64_t statusGeneration;
	bool statusObserveAllowed;

	uint64_t NextTimeStamp = 0;

	DTS_LIB_CONTEXT			*Ctx = NULL;
	DTS_GET_CTX(hDevice,Ctx);
	if (!pStatus)
		return BC_STS_INV_ARG;

	memset(&temp, 0, sizeof(temp));
	temp.DrvNextMDataPLD = Ctx->HWOutPicWidth | (0x1U << 31);

	// If bit 31 of the input cpbEmptySize is set, then report the real HW size
	// Else report the buffered size
	// If Bit 30 of the input cpbEmptySize is set, then only report TX information
	// and not probe for anything else
	// Use Bit 29 to indicate to the driver to read the VC1 fifo status
	if(pStatus->cpbEmptySize >> 31)
		realHWCPBSize = true;
	if((pStatus->cpbEmptySize >> 30) & 0x1) {
		readTXinfoOnly = true;
		temp.DrvcpbEmptySize |= (1U << 30);
	}

	if(Ctx->VidParams.VideoAlgo == BC_VID_ALGO_VC1MP)
		temp.DrvcpbEmptySize |= (1U << 29);

	/* The status ioctl can straddle the first PM observation.  Keep the
	 * returned counters useful to the caller, but never publish its stale EOS
	 * suffix into a newly-reset recovery epoch. */
	DtsLock(Ctx);
	statusGeneration = Ctx->recoveryGeneration;
	statusObserveAllowed = !Ctx->recoveryPending &&
		Ctx->controlOwner != DTS_CONTROL_RECOVERY;
	DtsUnLock(Ctx);
	ret = DtsGetDrvStat(hDevice, &temp);

	if (ret != BC_STS_SUCCESS)
	{
		return ret;
	}

	pStatus->FreeListCount      = temp.drvFLL;
	pStatus->ReadyListCount     = temp.drvRLL;
	pStatus->FramesCaptured     = temp.opFrameCaptured;
	pStatus->FramesDropped      = temp.opFrameDropped;
	pStatus->FramesRepeated     = temp.reptdFrames;
	pStatus->PIBMissCount       = temp.pibMisses;
	pStatus->InputCount         = temp.ipSampleCnt;
	pStatus->InputBusyCount     = temp.TxFifoBsyCnt;
	pStatus->InputTotalSize     = temp.ipTotalSize;
	pStatus->cpbEmptySize		= temp.DrvcpbEmptySize;
	pStatus->picNumFlags		= temp.picNumFlags;
	pStatus->PowerStateChange	= temp.pwr_state_change;
	DtsLock(Ctx);
	const bool observeCurrent = statusObserveAllowed &&
		statusGeneration == Ctx->recoveryGeneration &&
		!Ctx->recoveryPending && Ctx->controlOwner != DTS_CONTROL_RECOVERY;
	if (observeCurrent)
		DtsObservePowerState(Ctx, pStatus->PowerStateChange);
	bool statusCurrent = observeCurrent &&
		statusGeneration == Ctx->recoveryGeneration &&
		!Ctx->recoveryPending && Ctx->controlOwner != DTS_CONTROL_RECOVERY;
	if (statusCurrent) {
		if(temp.eosDetected)
		{
			Ctx->bEOS = true;
		}

		if (Ctx->bEOSCheck == true && Ctx->bEOS == true)
		{
			if (pStatus->ReadyListCount == 0)
			{
				Ctx->DrvStatusEOSCnt ++;

				if(Ctx->DrvStatusEOSCnt >= BC_EOS_PIC_COUNT)
				{
					/* Mark this picture as end of stream..*/
					Ctx->bEOS = TRUE;
				}
			}
			else
				Ctx->DrvStatusEOSCnt = 0;
		}
	}
	DtsUnLock(Ctx);
	if (!statusCurrent) {
		/* Queue/timestamp fields belong to the pre-reset driver sample. */
		pStatus->ReadyListCount = 0;
		pStatus->NextTimeStamp = 0;
		pStatus->picNumFlags = 0;
	}

	if(!realHWCPBSize)
		pStatus->cpbEmptySize = Ctx->circBuf.freeSize;

	if(!readTXinfoOnly && statusCurrent)
	{
		DtsLock(Ctx);
		const bool timestampCurrent = statusGeneration == Ctx->recoveryGeneration &&
			!Ctx->recoveryPending && Ctx->controlOwner != DTS_CONTROL_RECOVERY;
		if (!timestampCurrent) {
			/* Recovery can begin after the first suffix check.  Suppress the
			 * stale ready/timestamp tuple in the same locked revalidation that
			 * protects metadata lookup, and skip Link flow-control below. */
			pStatus->ReadyListCount = 0;
			pStatus->NextTimeStamp = 0;
			pStatus->picNumFlags = 0;
			statusCurrent = false;
		}
		/* return the timestamp of the next picture to be returned by ProcOutput */
		if(timestampCurrent && (pStatus->ReadyListCount > 0) &&
		   Ctx->SingleThreadedAppMode) {
			if(Ctx->DevId == BC_PCI_DEVID_FLEA)
			{
				if(temp.DrvNextMDataPLD == 0xFFFFFFFF){
					//For Pre-Load
					pStatus->NextTimeStamp = 0xFFFFFFFFFFFFFFFFLL;
				}else{
					//Normal PTS
					//Change PTS becuase of Shift PTS Issue in FW and 32-bit (ms) and 64-bit (100 ns) Scaling
					pStatus->NextTimeStamp = (temp.DrvNextMDataPLD * 2 * 10000);
				}
			}else{
				DtsFetchTimeStampMdata(Ctx, ((temp.DrvNextMDataPLD & 0xFF) << 8) | ((temp.DrvNextMDataPLD & 0xFF00) >> 8), &NextTimeStamp);
				pStatus->NextTimeStamp = NextTimeStamp;
			}
		}
		DtsUnLock(Ctx);
	}
	// For LINK Pause HW if the RLL is too full. Prevent overflows
	// Hard coded values for now
	if(statusCurrent && Ctx->DevId == BC_PCI_DEVID_LINK &&
	   Ctx->SingleThreadedAppMode) {
		DtsLifecycleCall control(Ctx);
		if (control.Status() != BC_STS_SUCCESS)
			return ret;
		DtsLock(Ctx);
		const bool flowControlCurrent =
			statusGeneration == Ctx->recoveryGeneration &&
			!Ctx->recoveryPending && Ctx->recoveryStatus == BC_STS_SUCCESS;
		DtsUnLock(Ctx);
		if (!flowControlCurrent) {
			pStatus->ReadyListCount = 0;
			pStatus->NextTimeStamp = 0;
			pStatus->picNumFlags = 0;
			return ret;
		}
		if(pStatus->ReadyListCount > 10 && !Ctx->hw_paused && !Ctx->fw_cmd_issued) {
			if (DtsFWPauseVideo(hDevice,eC011_PAUSE_MODE_ON) == BC_STS_SUCCESS)
				Ctx->hw_paused = true;
		}
		else if (pStatus->ReadyListCount  < 6 && Ctx->hw_paused && !Ctx->fw_cmd_issued) {
			if (DtsFWPauseVideo(hDevice,eC011_PAUSE_MODE_OFF) == BC_STS_SUCCESS)
				Ctx->hw_paused = false;
		}
	}

	return ret;
}

DRVIFLIB_API BC_STATUS
DtsGetRecoveryStatus(HANDLE hDevice, uint64_t *pGeneration,
	BC_STATUS *pRecoveryStatus, uint8_t *pInProgress,
	uint32_t *pReplayBytes)
{
	DTS_LIB_CONTEXT *Ctx = NULL;
	DTS_GET_CTX(hDevice, Ctx);
	if (!pGeneration || !pRecoveryStatus || !pInProgress || !pReplayBytes)
		return BC_STS_INV_ARG;

	DtsLock(Ctx);
	if (Ctx->recoveryPending && Ctx->htxThread == 0) {
		/* A joined/failed TX worker cannot settle a pending epoch. Publish a
		 * terminal outcome so the caller can proceed to the consuming
		 * DeviceClose path instead of waiting forever. */
		Ctx->recoveryPending = false;
		if (Ctx->recoveryStatus == BC_STS_SUCCESS)
			Ctx->recoveryStatus = BC_STS_PWR_MGMT;
	}
	*pGeneration = Ctx->recoveryGeneration;
	*pRecoveryStatus = Ctx->recoveryStatus;
	*pInProgress = Ctx->recoveryPending ||
		Ctx->controlOwner == DTS_CONTROL_RECOVERY;
	*pReplayBytes = Ctx->VidParams.MediaSubType == BC_MSUBTYPE_H264 ?
		Ctx->PESConvParams.m_iSpsPpsLen : 0;
	DtsUnLock(Ctx);
	return BC_STS_SUCCESS;
}

DRVIFLIB_API BC_STATUS DtsGetCapabilities (HANDLE  hDevice, PBC_HW_CAPS	pCapsBuffer)
{
	DTS_LIB_CONTEXT *Ctx;
	BC_STATUS sts = BC_STS_SUCCESS;
	uint32_t pciids = 0;
	int shmid = 0;

//	DebugLog_Trace(LDIL_DBG,"DtsGetCapabilities: Called\n");

	if(hDevice != NULL) {
		DTS_GET_CTX(hDevice,Ctx); // Called after the HW has been opened
		pciids = Ctx->DevId;
	}
	else {
		// called before HW has been opened
		// First make sure no one else has the HW open already
		if(BC_STS_SUCCESS == DtsCreateShMem(&shmid)) {
			pciids = DtsGetgDevID();
			DtsDelDilShMem();
			if(pciids == BC_PCI_DEVID_INVALID) {
				sts = DtsGetHWFeatures(&pciids);
				pciids >>= 16;
				if(sts != BC_STS_SUCCESS)
					return sts;
			}
		}
		else
			return BC_STS_INSUFF_RES;
	}

	if (pciids == BC_PCI_DEVID_INVALID)
	{
		return BC_STS_ERROR;
	}

	// Should check with driver/FW if current video is supported or not, and output supported format
	if(pciids == BC_PCI_DEVID_LINK)
	{
		pCapsBuffer->flags = PES_CONV_SUPPORT;
		pCapsBuffer->ColorCaps.Count = 3;

		pCapsBuffer->ColorCaps.OutFmt[0] = OUTPUT_MODE420;
		pCapsBuffer->ColorCaps.OutFmt[1] = OUTPUT_MODE422_YUY2;
		pCapsBuffer->ColorCaps.OutFmt[2] = OUTPUT_MODE422_UYVY;
		pCapsBuffer->Reserved1 = NULL;

		//Decoder Capability
		pCapsBuffer->DecCaps = BC_DEC_FLAGS_H264 | BC_DEC_FLAGS_MPEG2 | BC_DEC_FLAGS_VC1;
	}
	if(pciids == BC_PCI_DEVID_FLEA)
	{
		pCapsBuffer->flags = PES_CONV_SUPPORT;
		pCapsBuffer->ColorCaps.Count = 1;
		pCapsBuffer->ColorCaps.OutFmt[0] = OUTPUT_MODE422_YUY2;
		pCapsBuffer->ColorCaps.OutFmt[1] = OUTPUT_MODE_INVALID;
		pCapsBuffer->ColorCaps.OutFmt[2] = OUTPUT_MODE_INVALID;
		pCapsBuffer->Reserved1 = NULL;

		//Decoder Capability
		pCapsBuffer->DecCaps = BC_DEC_FLAGS_H264 | BC_DEC_FLAGS_MPEG2 | BC_DEC_FLAGS_VC1 | BC_DEC_FLAGS_M4P2;
	}

	return BC_STS_SUCCESS;
}

DRVIFLIB_API BC_STATUS DtsSetScaleParams(HANDLE hDevice, PBC_SCALING_PARAMS pScaleParams)
{
	DTS_LIB_CONTEXT *Ctx = NULL;
	DTS_GET_CTX(hDevice, Ctx);
	if (!pScaleParams)
		return BC_STS_INV_ARG;
	DtsLifecycleCall control(Ctx);
	if (control.Status() != BC_STS_SUCCESS)
		return control.Status();
	uint32_t ScaledWidth = 0;

	if (Ctx->DevId == BC_PCI_DEVID_FLEA) {
		if ((pScaleParams->sWidth > 1920) || (pScaleParams->sWidth < 128))
			ScaledWidth = 1280;
		else
			ScaledWidth = pScaleParams->sWidth;

		Ctx->EnableScaling = (ScaledWidth << 20) | (ScaledWidth << 8) | 1;

	} else {
		DebugLog_Trace(LDIL_ERR,"DtsSetScaleParams: not supported\n");
		return BC_STS_INV_ARG;
	}

	return DtsCheckProfile(hDevice);
}

DRVIFLIB_API BC_STATUS DtsCrystalHDVersion(HANDLE  hDevice, PBC_INFO_CRYSTAL bCrystalInfo)
{
	DTS_LIB_CONTEXT			*Ctx = NULL;
	DTS_GET_CTX(hDevice,Ctx);

	if(Ctx->DevId == BC_PCI_DEVID_LINK)
		bCrystalInfo->device = 0;
	else if(Ctx->DevId == BC_PCI_DEVID_FLEA)
		bCrystalInfo->device = 1;

	bCrystalInfo->dilVersion.dilRelease = DIL_MAJOR_VERSION;
	bCrystalInfo->dilVersion.dilMajor = DIL_MINOR_VERSION;
	bCrystalInfo->dilVersion.dilMinor = DIL_REVISION;

	bCrystalInfo->drvVersion.drvRelease = DRIVER_MAJOR_VERSION;
	bCrystalInfo->drvVersion.drvMajor = DRIVER_MINOR_VERSION;
	bCrystalInfo->drvVersion.drvMinor = DRIVER_REVISION;

	bCrystalInfo->fwVersion.fwRelease = FW_MAJOR_VERSION;
	bCrystalInfo->fwVersion.fwMajor = FW_MINOR_VERSION;
	bCrystalInfo->fwVersion.fwMinor = FW_REVISION;

	return BC_STS_SUCCESS;
}
