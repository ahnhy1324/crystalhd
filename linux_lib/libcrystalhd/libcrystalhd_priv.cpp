/********************************************************************
 * Copyright(c) 2006-2009 Broadcom Corporation.
 *
 *  Name: libcrystalhd_priv.cpp
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

#include <link.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <semaphore.h>
#include <pthread.h>
#include <sys/shm.h>
#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>
#if defined(__SSE2__)
#include <emmintrin.h>
#endif
#include "7411d.h"
#include "libcrystalhd_if.h"
#include "libcrystalhd_int_if.h"
#include "libcrystalhd_priv.h"
#include "libcrystalhd_parser.h"

/*============== Global shared area usage ======================*/
/* Global mode settings */
/*
	Bit 0 (LSB)	- Play Back mode
	Bit 1		- Diag mode
	Bit 2		- Monitor mode
	Bit 3		- HwInit mode
	bit 5       - Hwsetup in progress
*/

bc_dil_glob_s *bc_dil_glob_ptr=NULL;
BOOL glob_mode_valid=TRUE;

BC_STATUS DtsCreateShMem(int *shmem_id)
{
	int shmid=-1;
	key_t shmkey=BC_DIL_SHMEM_KEY;
	shmid_ds buf;
	uint32_t mode=0;


	if(shmem_id==NULL) {
		DebugLog_Trace(LDIL_DBG,"Invalid argument ...\n");
		return BC_STS_INSUFF_RES;
	}

	*shmem_id =shmid;
	//First Try to create it.
	if((shmid= shmget(shmkey, 1024, 0644|IPC_CREAT|IPC_EXCL))== -1 ) {
		if(errno==EEXIST) {
			//DebugLog_Trace(LDIL_DBG,"DtsCreateShMem:shmem already exists :%d\n",errno);
			//shmem segment already exists so get the shmid
			if((shmid= shmget(shmkey, 1024, 0644))== -1 ) {
				DebugLog_Trace(LDIL_DBG,"DtsCreateShMem:unable to get shmid :%d\n",errno);
				return BC_STS_INSUFF_RES;
			}

			//we got the shmid, see if any process is alreday attached to it
			if(shmctl(shmid,IPC_STAT,&buf)==-1){
				DebugLog_Trace(LDIL_DBG,"DtsCreateShMem:shmctl failed ...\n");
				return BC_STS_ERROR;
			}


			if(buf.shm_nattch ==0) {
				//No process is currently attached to the shmem seg. go ahead and delete it as its contents are stale.
				if(-1!=shmctl(shmid,IPC_RMID,NULL)){
						DebugLog_Trace(LDIL_DBG,"DtsCreateShMem:deleted shmem segment and creating a new one ...\n");
						//return BC_STS_ERROR;

				}
				//create a new shmem
				if((shmid= shmget(shmkey, 1024, 0644|IPC_CREAT|IPC_EXCL))== -1 ) {
					DebugLog_Trace(LDIL_DBG,"DtsCreateShMem:unable to get shmid :%d\n",errno);
					return BC_STS_INSUFF_RES;
			    }
				//attach to it
				DtsGetDilShMem(shmid);

		   }

			else{ //someone is attached to it

					DtsGetDilShMem(shmid);
					mode = DtsGetOPMode();
					if(! ((mode==1)||(mode==2)||(mode==4))&&(buf.shm_nattch==1) ) {
						glob_mode_valid=FALSE;
						DebugLog_Trace(LDIL_DBG,"DtsCreateShMem:globmode %d is invalid\n",mode);
					}

			    }

		   }else{

			DebugLog_Trace(LDIL_DBG,"shmcreate failed with err %d",errno);
			return BC_STS_ERROR;
		   }
	}else{
		//we created just attach to it
		DtsGetDilShMem(shmid);
	}

	*shmem_id =shmid;

	return BC_STS_SUCCESS;
}

BC_STATUS DtsGetDilShMem(uint32_t shmid)
{
	bc_dil_glob_ptr=(bc_dil_glob_s *)shmat(shmid,(void *)0,0);

	if((long)bc_dil_glob_ptr==-1) {
		DebugLog_Trace(LDIL_DBG,"Unable to open shared memory ...\n");
		return BC_STS_ERROR;
	}

	return BC_STS_SUCCESS;
}

BC_STATUS DtsDelDilShMem()
{
	int shmid =0;
	shmid_ds buf;
	//First dettach the shared mem segment

	if(shmdt(bc_dil_glob_ptr)==-1) {

		DebugLog_Trace(LDIL_DBG,"Unable to detach from Dil shared memory ...\n");
		//return BC_STS_ERROR;
	}

	//delete the shared mem segment if there are no other attachments
	if ((shmid =shmget((key_t)BC_DIL_SHMEM_KEY,0,0))==-1){
			DebugLog_Trace(LDIL_DBG,"DtsDelDilShMem:Unable get shmid ...\n");
			return BC_STS_ERROR;
	}

	if(shmctl(shmid,IPC_STAT,&buf)==-1){
		DebugLog_Trace(LDIL_DBG,"DtsDelDilShMem:shmctl failed ...\n");
		return BC_STS_ERROR;
	}


	if(buf.shm_nattch ==0) {
		//No process is currently attached to the shmem seg. go ahead and delete it
		if(-1!=shmctl(shmid,IPC_RMID,NULL)){
				// DebugLog_Trace(LDIL_DBG,"DtsDelDilShMem:deleted shmem segment ...\n");
				return BC_STS_ERROR;

		} else{
			DebugLog_Trace(LDIL_DBG,"DtsDelDilShMem:unable to delete shmem segment ...\n");
		}

	}
	return BC_STS_SUCCESS;


}

uint32_t DtsGetgDevID(void)
{
	if(bc_dil_glob_ptr == NULL)
		return BC_PCI_DEVID_INVALID;
	else
		return bc_dil_glob_ptr->DevID;
}

void DtsSetgDevID(uint32_t DevID)
{
	bc_dil_glob_ptr->DevID = DevID;
}

uint32_t DtsGetOPMode( void )
{
	return  bc_dil_glob_ptr->gDilOpMode;
}

void DtsSetOPMode( uint32_t value )
{
	bc_dil_glob_ptr->gDilOpMode = value;
}

uint32_t DtsGetHwInitSts( void )
{
	return bc_dil_glob_ptr->gHwInitSts;
}

void DtsSetHwInitSts( uint32_t value )
{
	bc_dil_glob_ptr->gHwInitSts = value;
}

void DtsRstStats( void )
{
	memset(&bc_dil_glob_ptr->stats, 0, sizeof(bc_dil_glob_ptr->stats));
}

BC_DTS_STATS * DtsGetgStats ( void )
{
	return &bc_dil_glob_ptr->stats;
}

bool DtsIsDecOpened(pid_t nNewPID)
{
	if(bc_dil_glob_ptr == NULL)
		return false;

	if (nNewPID == 0)
		return bc_dil_glob_ptr->g_bDecOpened;

	if (nNewPID == bc_dil_glob_ptr->g_nProcID)
		return false;

	return bc_dil_glob_ptr->g_bDecOpened;
}

bool DtsChkPID(pid_t nCurPID)
{
	if (bc_dil_glob_ptr->g_nProcID == 0)
		return true;

	return (nCurPID == bc_dil_glob_ptr->g_nProcID);
}

void DtsSetDecStat(bool bDecOpen, pid_t PID)
{
	if (bDecOpen == true)
		bc_dil_glob_ptr->g_nProcID = PID;
	else
		bc_dil_glob_ptr->g_nProcID = 0;

	bc_dil_glob_ptr->g_bDecOpened = bDecOpen;
}
/*============== Global shared area usage End.. ======================*/

#define TOP_FIELD_FLAG				0x01
#define BOTTOM_FIELD_FLAG			0x02
#define PROGRESSIVE_FRAME_FLAG	0x03

typedef struct _DTS_RX_BUFFER_LAYOUT {
	uint32_t allocationBytes;
	uint32_t registeredBytes;
	uint32_t uvOffset;
	uint32_t outputFormat;
} DTS_RX_BUFFER_LAYOUT;

/* Keep the legacy 1920x1090 fallback allocation until output geometry is
 * known.  Allocation and driver-registration extents are intentionally
 * separate for planar output. */
static BC_STATUS DtsGetRxBufferLayout(const DTS_LIB_CONTEXT *Ctx,
	DTS_RX_BUFFER_LAYOUT *layout)
{
	const uint64_t pixels = (uint64_t)1920 * 1090;
	const uint64_t allocation = pixels * 2;
	uint64_t registered;
	uint64_t uvOffset;

	if (!Ctx || !layout)
		return BC_STS_INV_ARG;

	switch (Ctx->b422Mode) {
	case OUTPUT_MODE420:
		registered = pixels + pixels / 2;
		uvOffset = pixels;
		break;
	case OUTPUT_MODE422_YUY2:
	case OUTPUT_MODE422_UYVY:
		registered = allocation;
		uvOffset = 0;
		break;
	default:
		return BC_STS_INV_ARG;
	}

	if (allocation > (uint64_t)(uint32_t)-1 ||
	    allocation > (uint64_t)(size_t)-1 ||
	    registered > allocation || uvOffset > registered ||
	    (allocation & 3) || (registered & 3) || (uvOffset & 3))
		return BC_STS_INV_ARG;

	layout->allocationBytes = (uint32_t)allocation;
	layout->registeredBytes = (uint32_t)registered;
	layout->uvOffset = (uint32_t)uvOffset;
	layout->outputFormat = Ctx->b422Mode;
	return BC_STS_SUCCESS;
}

static BC_STATUS DtsValidateRxBuffer(const DTS_LIB_CONTEXT *Ctx,
	const void *buff, uint32_t buffSize, DTS_RX_BUFFER_LAYOUT *layout)
{
	BC_STATUS sts;

	if (!buff)
		return BC_STS_INV_ARG;
	sts = DtsGetRxBufferLayout(Ctx, layout);
	if (sts != BC_STS_SUCCESS)
		return sts;
	if (buffSize < layout->registeredBytes)
		return BC_STS_INV_ARG;
	return BC_STS_SUCCESS;
}

static uint32_t DtsConfiguredRxPoolCount()
{
	const char *value = getenv("CRYSTALHD_EXPERIMENTAL_RX_POOL_COUNT");

	if (!value || !strcmp(value, "16"))
		return BC_MAX_SW_VOUT_BUFFS;
	if (!strcmp(value, "8"))
		return 8;

	DebugLog_Trace(LDIL_DBG,
		"Ignoring invalid CRYSTALHD_EXPERIMENTAL_RX_POOL_COUNT\n");
	return BC_MAX_SW_VOUT_BUFFS;
}

static BC_STATUS DtsInitLock(DTS_LIB_CONTEXT *Ctx)
{
	pthread_mutexattr_t thLockattr;
	int ret = pthread_mutexattr_init(&thLockattr);
	if (ret) {
		DebugLog_Trace(LDIL_DBG, "Error initializing attributes\n");
		return BC_STS_INSUFF_RES;
	}
	ret = pthread_mutexattr_settype(&thLockattr, PTHREAD_MUTEX_RECURSIVE);
	if (ret) {
		DebugLog_Trace(LDIL_DBG, "Error setting type of mutex\n");
		pthread_mutexattr_destroy(&thLockattr);
		return BC_STS_INSUFF_RES;
	}
	ret = pthread_mutex_init(&Ctx->thLock, &thLockattr);
	pthread_mutexattr_destroy(&thLockattr);
	if (ret) {
		DebugLog_Trace(LDIL_DBG, "Error initializing mutex\n");
		return BC_STS_INSUFF_RES;
	}
	return BC_STS_SUCCESS;
}
static void DtsDelLock(DTS_LIB_CONTEXT	*Ctx)
{
	pthread_mutex_destroy(&Ctx->thLock);

}
void DtsLock(DTS_LIB_CONTEXT	*Ctx)
{
	pthread_mutex_lock(&Ctx->thLock);
}
void DtsUnLock(DTS_LIB_CONTEXT	*Ctx)
{
	pthread_mutex_unlock(&Ctx->thLock);
}

/* Caller holds thLock. */
static BC_STATUS DtsAcquireControlLocked(DTS_LIB_CONTEXT *Ctx,
	DTS_CONTROL_OWNER owner)
{
	const bool lifecycle = owner == DTS_CONTROL_LIFECYCLE;
	if (owner == DTS_CONTROL_TEARDOWN && Ctx->recoveryPending &&
	    Ctx->controlOwner == DTS_CONTROL_NONE) {
		/* DeviceClose is the terminal escape from a PM epoch that may never
		 * reach RESUME. Claiming teardown closes all new admission; its later
		 * TX join retires the sole recovery worker without replaying firmware.
		 * An already-owned recovery transaction is left alone and returns BUSY
		 * through the ordinary foreign-owner check below. */
		Ctx->recoveryPending = false;
		if (Ctx->recoveryStatus == BC_STS_SUCCESS)
			Ctx->recoveryStatus = BC_STS_PWR_MGMT;
	}
	if (Ctx->controlOwner != DTS_CONTROL_NONE) {
		if (!Ctx->controlDepth ||
		    !pthread_equal(Ctx->controlThread, pthread_self()))
			return BC_STS_BUSY;
		if (owner == DTS_CONTROL_RECOVERY &&
		    Ctx->controlOwner != DTS_CONTROL_RECOVERY)
			return BC_STS_BUSY;
		if (Ctx->recoveryStatus != BC_STS_SUCCESS &&
		    Ctx->controlOwner != DTS_CONTROL_RECOVERY &&
		    !(Ctx->controlOwner == DTS_CONTROL_TEARDOWN &&
		      owner == DTS_CONTROL_TEARDOWN))
			return Ctx->recoveryStatus;
		Ctx->controlDepth++;
		return BC_STS_SUCCESS;
	}

	if (owner == DTS_CONTROL_RECOVERY && DtsStopIsUnresolved(Ctx))
		return BC_STS_BUSY;
	if (lifecycle &&
	    Ctx->recoveryStatus != BC_STS_SUCCESS)
		return Ctx->recoveryStatus;
	if (owner == DTS_CONTROL_TEARDOWN &&
	    Ctx->recoveryStatus == BC_STS_SUCCESS && Ctx->recoveryPending)
		return BC_STS_BUSY;
	if (lifecycle && Ctx->recoveryPending)
		return BC_STS_BUSY;
	if (lifecycle && Ctx->stopInProgress)
		return BC_STS_BUSY;

	Ctx->controlOwner = owner;
	Ctx->controlThread = pthread_self();
	Ctx->controlDepth = 1;
	return BC_STS_SUCCESS;
}

BC_STATUS DtsAcquireControl(DTS_LIB_CONTEXT *Ctx, DTS_CONTROL_OWNER owner)
{
	DtsLock(Ctx);
	const BC_STATUS sts = DtsAcquireControlLocked(Ctx, owner);
	DtsUnLock(Ctx);
	return sts;
}

void DtsReleaseControl(DTS_LIB_CONTEXT *Ctx)
{
	DtsLock(Ctx);
	if (Ctx->controlOwner != DTS_CONTROL_NONE && Ctx->controlDepth &&
	    pthread_equal(Ctx->controlThread, pthread_self())) {
		if (!--Ctx->controlDepth) {
			Ctx->controlOwner = DTS_CONTROL_NONE;
			memset(&Ctx->controlThread, 0, sizeof(Ctx->controlThread));
		}
	}
	DtsUnLock(Ctx);
}

BC_STATUS DtsBeginInputCall(DTS_LIB_CONTEXT *Ctx)
{
	DtsLock(Ctx);
	BC_STATUS sts = DtsRecoveryAdmissionStatus(Ctx);
	const bool nestedInput = Ctx->inputCalls &&
		pthread_equal(Ctx->inputThread, pthread_self());
	const bool foreignControl = Ctx->controlOwner != DTS_CONTROL_NONE &&
		(!Ctx->controlDepth ||
		 !pthread_equal(Ctx->controlThread, pthread_self()));
	/* An outer admitted input can cross a concurrent ordinary lifecycle
	 * lease before its internal Align/SPES call.  Do not turn that nested
	 * suffix into a partial BUSY after earlier bytes were queued.  Recovery
	 * and terminal teardown remain absolute barriers, as do the downstream
	 * txQuiescing/state/stop checks. */
	if (sts == BC_STS_SUCCESS && foreignControl &&
	    (!nestedInput || Ctx->controlOwner != DTS_CONTROL_LIFECYCLE))
		sts = BC_STS_BUSY;
	if (sts == BC_STS_SUCCESS && Ctx->inputCalls && !nestedInput)
		sts = BC_STS_BUSY;
	if (sts == BC_STS_SUCCESS) {
		if (!Ctx->inputCalls)
			Ctx->inputThread = pthread_self();
		Ctx->inputCalls++;
	}
	DtsUnLock(Ctx);
	return sts;
}

void DtsFinishInputCall(DTS_LIB_CONTEXT *Ctx)
{
	DtsLock(Ctx);
	if (Ctx->inputCalls && !--Ctx->inputCalls)
		memset(&Ctx->inputThread, 0, sizeof(Ctx->inputThread));
	DtsUnLock(Ctx);
}

static BC_STATUS DtsAcquireOutput(DTS_LIB_CONTEXT *Ctx)
{
	BC_STATUS sts = BC_STS_SUCCESS;
	/* Admission and the flush/stop barrier use the same lock. A caller
	 * delayed before this point must not start a fetch after cancellation
	 * observed an empty pending count. There is one shared pOutData buffer.
	 */
	DtsLock(Ctx);
	if (DtsRecoveryAdmissionStatus(Ctx) != BC_STS_SUCCESS)
		sts = DtsRecoveryAdmissionStatus(Ctx);
	else if (Ctx->controlOwner != DTS_CONTROL_NONE &&
		 (!Ctx->controlDepth ||
		  !pthread_equal(Ctx->controlThread, pthread_self())))
		sts = BC_STS_BUSY;
	else if (Ctx->State == BC_DEC_STATE_CLOSE)
		sts = BC_STS_DEC_NOT_OPEN;
	else if (Ctx->State != BC_DEC_STATE_START && Ctx->State != BC_DEC_STATE_PAUSE)
		sts = BC_STS_DEC_NOT_STARTED;
	else if (Ctx->CancelWaiting)
		sts = BC_STS_IO_USER_ABORT;
	else if (DtsStopIsUnresolved(Ctx))
		sts = BC_STS_BUSY;
	else if (Ctx->outputPhase != DTS_OUTPUT_ACTIVE)
		sts = BC_STS_BUSY;
	else if (Ctx->ProcOutPending)
		sts = BC_STS_BUSY;
	else
		Ctx->ProcOutPending = 1;
	DtsUnLock(Ctx);
	return sts;
}
static void DtsDecPend(DTS_LIB_CONTEXT	*Ctx)
{
	DtsLock(Ctx);
	if(Ctx->ProcOutPending)
		Ctx->ProcOutPending--;
	if (!Ctx->ProcOutPending && Ctx->outputPhase == DTS_OUTPUT_RETURNED)
		Ctx->outputPhase = DTS_OUTPUT_IDLE;
	DtsUnLock(Ctx);
}

/* The phase protects the complete public call, not just one FETCH packet.
 * Pending can become zero before post-fetch checks or an internal refetch. */
BC_STATUS DtsBeginOutputCall(DTS_LIB_CONTEXT *Ctx)
{
	if (!Ctx)
		return BC_STS_INV_ARG;
	DtsLock(Ctx);
	BC_STATUS status = BC_STS_SUCCESS;
	if (DtsRecoveryAdmissionStatus(Ctx) != BC_STS_SUCCESS)
		status = DtsRecoveryAdmissionStatus(Ctx);
	else if (Ctx->controlOwner != DTS_CONTROL_NONE &&
		 (!Ctx->controlDepth ||
		  !pthread_equal(Ctx->controlThread, pthread_self())))
		status = BC_STS_BUSY;
	else if (Ctx->State == BC_DEC_STATE_CLOSE)
		status = BC_STS_DEC_NOT_OPEN;
	else if (Ctx->State != BC_DEC_STATE_START && Ctx->State != BC_DEC_STATE_PAUSE)
		status = BC_STS_DEC_NOT_STARTED;
	else if (Ctx->CancelWaiting)
		status = BC_STS_IO_USER_ABORT;
	else if (DtsStopIsUnresolved(Ctx))
		status = BC_STS_BUSY;
	else if (Ctx->outputPhase != DTS_OUTPUT_IDLE || Ctx->ProcOutPending)
		status = BC_STS_BUSY;
	else {
		Ctx->outputPhase = DTS_OUTPUT_ACTIVE;
		Ctx->outputRecoveryGeneration = Ctx->recoveryGeneration;
	}
	DtsUnLock(Ctx);
	return status;
}

BC_STATUS DtsBeginOutputRelease(DTS_LIB_CONTEXT *Ctx)
{
	if (!Ctx)
		return BC_STS_INV_ARG;
	DtsLock(Ctx);
	const bool returned = Ctx->outputPhase == DTS_OUTPUT_RETURNED && Ctx->ProcOutPending;
	if (returned)
		Ctx->outputPhase = DTS_OUTPUT_ACTIVE;
	DtsUnLock(Ctx);
	return returned ? BC_STS_SUCCESS : BC_STS_BUSY;
}

/* Invoke only after the call's last context/buffer access. Repost failures
 * retain pending, but the failed public call handed out no continuing borrow. */
void DtsFinishOutputCall(DTS_LIB_CONTEXT *Ctx)
{
	DtsLock(Ctx);
	if (Ctx->outputPhase == DTS_OUTPUT_ACTIVE ||
	    Ctx->outputPhase == DTS_OUTPUT_DELIVERING)
		Ctx->outputPhase = Ctx->ProcOutPending ? DTS_OUTPUT_RETIRE_ONLY : DTS_OUTPUT_IDLE;
	DtsUnLock(Ctx);
}

/* Caller holds thLock. A generation increase alone is insufficient: only the
 * cancellation stamp from that exact PM epoch can invalidate this old call. */
static bool DtsRecoveryCanceledGenerationLocked(const DTS_LIB_CONTEXT *Ctx)
{
	return Ctx->outputRecoveryGeneration < Ctx->recoveryGeneration &&
		Ctx->recoveryCancelGeneration == Ctx->recoveryGeneration;
}

static bool DtsRecoveryCanceledOutputLocked(const DTS_LIB_CONTEXT *Ctx)
{
	return Ctx->outputPhase == DTS_OUTPUT_ACTIVE &&
		DtsRecoveryCanceledGenerationLocked(Ctx);
}

/* Caller holds thLock. */
static BC_STATUS DtsNormalizeRecoveryOutputAbortLocked(DTS_LIB_CONTEXT *Ctx,
	BC_DTS_PROC_OUT *output, BC_STATUS status)
{
	if ((status != BC_STS_IO_USER_ABORT && status != BC_STS_PWR_MGMT) ||
	    !output || output->Ybuff || output->UVbuff)
		return status;

	return DtsRecoveryCanceledGenerationLocked(Ctx) ? BC_STS_BUSY : status;
}

static BC_STATUS DtsNormalizeRecoveryOutputAbort(DTS_LIB_CONTEXT *Ctx,
	BC_DTS_PROC_OUT *output, BC_STATUS status)
{
	DtsLock(Ctx);
	status = DtsNormalizeRecoveryOutputAbortLocked(Ctx, output, status);
	DtsUnLock(Ctx);
	return status;
}

BC_STATUS DtsPrepareFetchedOutput(DTS_LIB_CONTEXT *Ctx,
	BC_DTS_PROC_OUT *output)
{
	const BC_DEC_YUV_BUFFS &buffer = Ctx->pOutData->u.DecOutData.OutPutBuffs;
	if (Ctx->softwareUyvy && !(output->PoutFlags & BC_POUT_FLAGS_FMT_CHANGE) &&
	    !(output->PicInfo.flags & VDEC_FLAG_EOS) &&
	    (!(Ctx->pOutData->u.DecOutData.Flags & COMP_FLAG_DATA_VALID) ||
	     buffer.YuvBuff != output->Ybuff || buffer.UVbuffOffset ||
	     buffer.UVBuffDoneSz))
		return BC_STS_IO_XFR_ERROR;
	return DtsPrepareOutputPacking(Ctx, output, buffer.YuvBuffSz);
}

/* Linearize copy/callback delivery before any caller-visible work.  Recovery
 * may cancel an old ACTIVE fetch, but once DELIVERING is published it waits
 * for the bounded userspace copy/repost suffix instead of retroactively
 * invalidating pixels or a callback that cannot be retracted. */
BC_STATUS DtsCommitFetchedOutput(DTS_LIB_CONTEXT *Ctx, BC_DTS_PROC_OUT *output)
{
	if (!Ctx || !output)
		return BC_STS_INV_ARG;

	DtsLock(Ctx);
	if (Ctx->outputPhase != DTS_OUTPUT_ACTIVE || !Ctx->ProcOutPending) {
		DtsUnLock(Ctx);
		output->Ybuff = output->UVbuff = NULL;
		return BC_STS_ERROR;
	}
	const bool recoveryCanceled = DtsRecoveryCanceledOutputLocked(Ctx);
	const bool canceled = Ctx->CancelWaiting;
	if (!recoveryCanceled && !canceled)
		Ctx->outputPhase = DTS_OUTPUT_DELIVERING;
	DtsUnLock(Ctx);

	if (!recoveryCanceled && !canceled)
		return BC_STS_SUCCESS;

	/* Keep ACTIVE until the one retirement attempt completes.  A failed ACK
	 * leaves ProcOutPending set and the outer finalizer changes it to
	 * RETIRE_ONLY; it must never cause a second ADD_RXBUFFS command. */
	(void)DtsRelRxBuff(Ctx, &Ctx->pOutData->u.RxBuffs, FALSE);
	output->Ybuff = output->UVbuff = NULL;
	return recoveryCanceled ? BC_STS_BUSY : BC_STS_IO_USER_ABORT;
}

/* A dropped frame can continue the same public call only after its driver
 * lease was returned and no recovery/stop barrier arrived during that
 * repost. */
BC_STATUS DtsContinueOutputCall(DTS_LIB_CONTEXT *Ctx)
{
	if (!Ctx)
		return BC_STS_INV_ARG;
	DtsLock(Ctx);
	if (Ctx->outputPhase != DTS_OUTPUT_DELIVERING || Ctx->ProcOutPending) {
		DtsUnLock(Ctx);
		return BC_STS_ERROR;
	}
	BC_STATUS status = DtsRecoveryAdmissionStatus(Ctx);
	if (status == BC_STS_SUCCESS && Ctx->CancelWaiting)
		status = BC_STS_IO_USER_ABORT;
	if (status == BC_STS_SUCCESS && DtsStopIsUnresolved(Ctx))
		status = BC_STS_BUSY;
	if (status == BC_STS_SUCCESS) {
		Ctx->outputPhase = DTS_OUTPUT_ACTIVE;
		Ctx->outputRecoveryGeneration = Ctx->recoveryGeneration;
	}
	DtsUnLock(Ctx);
	return status;
}

BC_STATUS DtsPublishOutput(DTS_LIB_CONTEXT *Ctx, BC_DTS_PROC_OUT *output, BC_STATUS status)
{
	DtsLock(Ctx);
	if (Ctx->outputPhase != DTS_OUTPUT_ACTIVE &&
	    Ctx->outputPhase != DTS_OUTPUT_DELIVERING) {
		output->Ybuff = output->UVbuff = NULL;
		DtsUnLock(Ctx);
		return BC_STS_ERROR;
	}
	const bool needsCommit = Ctx->outputPhase == DTS_OUTPUT_ACTIVE &&
		Ctx->ProcOutPending &&
		(status == BC_STS_SUCCESS || status == BC_STS_FMT_CHANGE);
	const bool activeFailure = Ctx->outputPhase == DTS_OUTPUT_ACTIVE &&
		!needsCommit;
	DtsUnLock(Ctx);
	if (activeFailure) {
		output->Ybuff = output->UVbuff = NULL;
		if (status == BC_STS_SUCCESS)
			status = BC_STS_IO_XFR_ERROR;
		DtsLock(Ctx);
		status = DtsNormalizeRecoveryOutputAbortLocked(Ctx, output, status);
		if (Ctx->outputPhase == DTS_OUTPUT_ACTIVE ||
		    Ctx->outputPhase == DTS_OUTPUT_DELIVERING)
			Ctx->outputPhase = Ctx->ProcOutPending ?
				DTS_OUTPUT_RETIRE_ONLY : DTS_OUTPUT_IDLE;
		DtsUnLock(Ctx);
		return status;
	}
	if (needsCommit) {
		const BC_STATUS commitStatus = DtsCommitFetchedOutput(Ctx, output);
		if (commitStatus != BC_STS_SUCCESS) {
			DtsFinishOutputCall(Ctx);
			return commitStatus;
		}
	}
	if (status == BC_STS_SUCCESS || status == BC_STS_FMT_CHANGE) {
		const BC_STATUS packing = DtsPrepareFetchedOutput(Ctx, output);
		if (packing != BC_STS_SUCCESS) {
			const BC_STATUS released = DtsRelRxBuff(Ctx,
				&Ctx->pOutData->u.RxBuffs, FALSE);
			status = released == BC_STS_SUCCESS ? packing : released;
		} else {
			DtsUpdateOutStats(Ctx, output);
			if (status == BC_STS_FMT_CHANGE) {
				const BC_STATUS released = DtsRelRxBuff(Ctx,
					&Ctx->pOutData->u.RxBuffs, TRUE);
				if (released != BC_STS_SUCCESS)
					status = released;
				output->Ybuff = output->UVbuff = NULL;
			}
		}
	}

	if (status != BC_STS_SUCCESS)
		output->Ybuff = output->UVbuff = NULL;

	DtsLock(Ctx);
	if (Ctx->outputPhase != DTS_OUTPUT_DELIVERING) {
		DtsUnLock(Ctx);
		output->Ybuff = output->UVbuff = NULL;
		return BC_STS_ERROR;
	}
	if (status == BC_STS_SUCCESS && Ctx->ProcOutPending) {
		Ctx->outputPhase = DTS_OUTPUT_RETURNED;
		DtsUnLock(Ctx);
		return BC_STS_SUCCESS;
	}
	if (status == BC_STS_SUCCESS) {
		status = BC_STS_IO_XFR_ERROR;
		output->Ybuff = output->UVbuff = NULL;
	}
	/* Normalize the exact recovery generation before publishing a terminal
	 * phase, under the same lock.  The unlock below is this call's final Ctx
	 * access, so teardown can safely consume it once the phase is visible. */
	status = DtsNormalizeRecoveryOutputAbortLocked(Ctx, output, status);
	Ctx->outputPhase = Ctx->ProcOutPending ?
		DTS_OUTPUT_RETIRE_ONLY : DTS_OUTPUT_IDLE;
	DtsUnLock(Ctx);
	return status;
}

/* Copy-mode has already returned/reposted the driver buffer. Validate and
 * retire the userspace call phase in one lock operation so a PM epoch cannot
 * land between the final generation check and the RAII finalizer. Caller
 * destination pointers are never library-owned and must remain unchanged. */
BC_STATUS DtsPublishCopiedOutput(DTS_LIB_CONTEXT *Ctx, BC_STATUS status)
{
	DtsLock(Ctx);
	if (Ctx->outputPhase != DTS_OUTPUT_DELIVERING) {
		DtsUnLock(Ctx);
		return BC_STS_ERROR;
	}
	Ctx->outputPhase = Ctx->ProcOutPending ?
		DTS_OUTPUT_RETIRE_ONLY : DTS_OUTPUT_IDLE;
	DtsUnLock(Ctx);
	return status;
}

void DtsGetFrameRate(DTS_LIB_CONTEXT *Ctx, BC_DTS_PROC_OUT *pOut)
{
	if (Ctx->VidParams.FrameRate == pOut->PicInfo.frame_rate)
	{
		pOut->PicInfo.frame_rate = 0;
		return;
	}

	// DebugLog_Trace(LDIL_DBG,"DtsGetFrameRate: frame_rate:%d (%d)\n", pOut->PicInfo.frame_rate, Ctx->VidParams.FrameRate);

	Ctx->VidParams.FrameRate = pOut->PicInfo.frame_rate;

	if (pOut->PicInfo.frame_rate == vdecFrameRate23_97)
		pOut->PicInfo.frame_rate = 23970;
	else if (pOut->PicInfo.frame_rate == vdecFrameRate24)
		pOut->PicInfo.frame_rate = 24000;
	else if (pOut->PicInfo.frame_rate == vdecFrameRate25)
		pOut->PicInfo.frame_rate = 25000;
	else if (pOut->PicInfo.frame_rate == vdecFrameRate29_97)
		pOut->PicInfo.frame_rate = 29970;
	else if (pOut->PicInfo.frame_rate == vdecFrameRate30)
		pOut->PicInfo.frame_rate = 30000;
	else if (pOut->PicInfo.frame_rate == vdecFrameRate50)
		pOut->PicInfo.frame_rate = 50000;
	else if (pOut->PicInfo.frame_rate == vdecFrameRate59_94)
		pOut->PicInfo.frame_rate = 59940;
	else if (pOut->PicInfo.frame_rate == vdecFrameRate60)
		pOut->PicInfo.frame_rate = 60000;
	else if (pOut->PicInfo.frame_rate == vdecFrameRate14_985)
		pOut->PicInfo.frame_rate = 14985;
	else if (pOut->PicInfo.frame_rate == vdecFrameRate7_496)
		pOut->PicInfo.frame_rate = 7496;
	else if (pOut->PicInfo.frame_rate == vdecFrameRateUnknown)
		pOut->PicInfo.frame_rate = 23970;
	else
		pOut->PicInfo.frame_rate = 23970;

}


uint32_t DtsGetHWOutputStride(DTS_LIB_CONTEXT *Ctx, C011_PIB *pPIBInfo)
{
	if (Ctx->DevId == BC_PCI_DEVID_FLEA)
	{
		return pPIBInfo->ppb.width;
	}
	else
	{
		return pPIBInfo->resolution;
	}
}

uint32_t DtsGetWidthfromResolution(DTS_LIB_CONTEXT *Ctx, uint32_t Resolution)
{
	uint32_t Width;

	/* For Flea source width is always equal to video width */
	/* For Link translate from format container to actual source width */
	if(Ctx->DevId == BC_PCI_DEVID_FLEA)
		return 0;

	switch (Resolution) {
	case vdecRESOLUTION_CUSTOM:
		Width = 0;
		break;
	case vdecRESOLUTION_1080i:
	case vdecRESOLUTION_1080i25:
    case vdecRESOLUTION_1080i29_97:
    case vdecRESOLUTION_1080p29_97:
    case vdecRESOLUTION_1080p30:
    case vdecRESOLUTION_1080p24:
    case vdecRESOLUTION_1080p25:
	case vdecRESOLUTION_1080p0:
	case vdecRESOLUTION_1080i0:
	case vdecRESOLUTION_1080p23_976:
		Width = 1920;
		break;
	case vdecRESOLUTION_240p29_97:
	case vdecRESOLUTION_240p30:
	case vdecRESOLUTION_288p25:
		Width = 1440;
		break;
	case vdecRESOLUTION_720p:
    case vdecRESOLUTION_720p50:
	case vdecRESOLUTION_720p59_94:
    case vdecRESOLUTION_720p24:
	case vdecRESOLUTION_720p29_97:
	case vdecRESOLUTION_720p0:
	case vdecRESOLUTION_720p23_976:
		Width = 1280;
		break;
	case vdecRESOLUTION_480i:
	case vdecRESOLUTION_NTSC:
	case vdecRESOLUTION_PAL1:
    case vdecRESOLUTION_SD_DVD:
    case vdecRESOLUTION_480p656:
	case vdecRESOLUTION_480p:
    case vdecRESOLUTION_576p:
	case vdecRESOLUTION_480p23_976:
	case vdecRESOLUTION_480p29_97:
	case vdecRESOLUTION_576p25:
	case vdecRESOLUTION_480p0:
	case vdecRESOLUTION_480i0:
	case vdecRESOLUTION_576p0:
		Width = 720;
		break;
	default:
		Width = 0;
		break;
	}
	return Width;
}

static void DtsCopyAppPIB(DTS_LIB_CONTEXT *Ctx, BC_DEC_OUT_BUFF *decOut, BC_DTS_PROC_OUT *pOut)
{
	C011_PIB			*srcPib = &decOut->PibInfo;
	BC_PIC_INFO_BLOCK	*dstPib = &pOut->PicInfo;
	//uint16_t					sNum = 0;
	//BC_STATUS			sts = BC_STS_SUCCESS;

	Ctx->FormatInfo.timeStamp		= dstPib->timeStamp	= 0;
	Ctx->FormatInfo.picture_number	= dstPib->picture_number = srcPib->ppb.picture_number;
	Ctx->FormatInfo.width			= dstPib->width			 = srcPib->ppb.width;
	Ctx->FormatInfo.height			= dstPib->height		 = srcPib->ppb.height;
	Ctx->FormatInfo.chroma_format	= dstPib->chroma_format	 = srcPib->ppb.chroma_format;
	Ctx->FormatInfo.pulldown		= dstPib->pulldown		 = srcPib->ppb.pulldown;
	Ctx->FormatInfo.flags			= dstPib->flags			 = srcPib->ppb.flags;
	Ctx->FormatInfo.sess_num		= dstPib->sess_num		 = srcPib->ptsStcOffset;
	Ctx->FormatInfo.aspect_ratio	= dstPib->aspect_ratio	 = srcPib->ppb.aspect_ratio;
	Ctx->FormatInfo.colour_primaries = dstPib->colour_primaries = srcPib->ppb.colour_primaries;
	Ctx->FormatInfo.picture_meta_payload= dstPib->picture_meta_payload	= srcPib->ppb.picture_meta_payload;

	/* FIX_ME:: Add extensions part.. */

	/* Retrieve Timestamp */
	// NAREN - FIXME - We should not copy the timestamp for Format Change since it is a dummy picture with no timestamp
#if 0
	if(srcPib->ppb.flags & VDEC_FLAG_PICTURE_META_DATA_PRESENT){
		sNum = (U16) ( ((srcPib->ppb.picture_meta_payload & 0xFF) << 8) |
		((srcPib->ppb.picture_meta_payload& 0xFF00) >> 8) );
		DtsFetchMdata(Ctx,sNum,pOut);
}
#endif
}
static void dts_swap_buffer(uint32_t *dst, uint32_t *src, uint32_t cnt)
{
	uint32_t i=0;

	for (i=0; i < cnt; i++){
		dst[i] = BC_SWAP32(src[i]);
	}

}

static void DtsGetPibFrom422(uint8_t *pibBuff, uint8_t mode422)
{
	uint32_t		i;

	//First stripe has PIB data and second one has Extension PB. so total 256 bytes
	if (mode422 == OUTPUT_MODE422_YUY2) {
		for(i=0; i<256; i++) {
			pibBuff[i] = pibBuff[i*2];
		}
	} else if (mode422 == OUTPUT_MODE422_UYVY) {
		for(i=0; i<256; i++) {
			pibBuff[i] = pibBuff[(i*2)+1];
		}
	}
}

static BC_STATUS DtsGetPictureInfo(DTS_LIB_CONTEXT *Ctx, BC_DTS_PROC_OUT *pOut)
{

	uint16_t			sNum = 0;
	uint8_t*			pPicInfoLine = NULL;
	uint32_t			PictureNumber = 0;
	uint32_t			PicInfoLineNum;
	bool				bInterlaced = false;
	bool				bEos = false;

	if (Ctx->DevId == BC_PCI_DEVID_FLEA)
	{
		PicInfoLineNum = *(uint32_t *)(pOut->Ybuff);
	}
	else if (Ctx->b422Mode == OUTPUT_MODE422_YUY2)
	{
		PicInfoLineNum = ((uint32_t)(*(pOut->Ybuff + 6)) & 0xff)
						| (((uint32_t)(*(pOut->Ybuff + 4)) << 8)  & 0x0000ff00)
						| (((uint32_t)(*(pOut->Ybuff + 2)) << 16) & 0x00ff0000)
						| (((uint32_t)(*(pOut->Ybuff + 0)) << 24) & 0xff000000);
	}
	else if (Ctx->b422Mode == OUTPUT_MODE422_UYVY)
	{
		PicInfoLineNum = ((uint32_t)(*(pOut->Ybuff + 7)) & 0xff)
						| (((uint32_t)(*(pOut->Ybuff + 5)) << 8)  & 0x0000ff00)
						| (((uint32_t)(*(pOut->Ybuff + 3)) << 16) & 0x00ff0000)
						| (((uint32_t)(*(pOut->Ybuff + 1)) << 24) & 0xff000000);
	}
	else
	{
		PicInfoLineNum = ((uint32_t)(*(pOut->Ybuff + 3)) & 0xff)
						| (((uint32_t)(*(pOut->Ybuff + 2)) << 8)  & 0x0000ff00)
						| (((uint32_t)(*(pOut->Ybuff + 1)) << 16) & 0x00ff0000)
						| (((uint32_t)(*(pOut->Ybuff + 0)) << 24) & 0xff000000);
	}

	if (PicInfoLineNum == BC_EOS_DETECTED)  // EOS
	{
		const uint64_t markerBytes = sizeof(PicInfoLineNum) +
			sizeof(BC_PIC_INFO_BLOCK);
		const BC_DEC_YUV_BUFFS &source =
			Ctx->pOutData->u.DecOutData.OutPutBuffs;
		/* The sentinel is a control transfer, not a picture. Bound the PIB
		 * copy by both the completed DMA extent and the registered buffer.
		 */
		if (!pOut->Ybuff || source.YuvBuff != pOut->Ybuff ||
			(uint64_t)pOut->YBuffDoneSz * 4 < markerBytes ||
			source.YuvBuffSz < markerBytes)
		{
			Ctx->pOutData->RetSts = BC_STS_IO_XFR_ERROR;
			return BC_STS_IO_XFR_ERROR;
		}
		memcpy((uint32_t*)&pOut->PicInfo,(uint32_t*)(pOut->Ybuff + 4), sizeof(BC_PIC_INFO_BLOCK));
		/* Some BCM70015 scaler widths return an EOS control transfer with an
		 * alignment row. The driver still writes the sentinel at Y[0], while
		 * this legacy Y+4 PIB lookup can then see empty alignment bytes instead
		 * of VDEC_FLAG_EOS. The armed drain scopes that layout quirk; do not
		 * reinterpret an unarmed sentinel or any LINK output.
		 */
		DtsLock(Ctx);
		bEos = (pOut->PicInfo.flags & VDEC_FLAG_EOS) ||
			(Ctx->DevId == BC_PCI_DEVID_FLEA && Ctx->bEOSCheck);
		if (bEos)
			Ctx->bEOS = true;
		DtsUnLock(Ctx);
		if (bEos)
		{
			pOut->PicInfo.flags |= VDEC_FLAG_EOS;
			Ctx->pOutData->RetSts = BC_STS_NO_DATA;
			DebugLog_Trace(LDIL_DBG, "Found EOS \n");
			return BC_STS_NO_DATA;
		}
		Ctx->pOutData->RetSts = BC_STS_IO_XFR_ERROR;
		return BC_STS_IO_XFR_ERROR;
	}
	/*
	-- To take care of 16 byte alignment the firmware might put extra
	-- line so that the PIB starts with a line boundary. We will need to
	-- have additional checks for the following condition to take care of
	-- extra lines.
	*/

	if( ( (PicInfoLineNum != Ctx->HWOutPicHeight) && (PicInfoLineNum != (Ctx->HWOutPicHeight+1))) &&
		( (PicInfoLineNum != Ctx->HWOutPicHeight/2) && (PicInfoLineNum != (Ctx->HWOutPicHeight+1)/2)) )
	{
		return BC_STS_IO_XFR_ERROR;
	}

	if (Ctx->b422Mode) {
		pPicInfoLine = pOut->Ybuff + PicInfoLineNum * Ctx->HWOutPicWidth * 2;
	} else {
		pPicInfoLine = pOut->Ybuff + PicInfoLineNum * Ctx->HWOutPicWidth;
	}

	if (Ctx->DevId != BC_PCI_DEVID_FLEA)
	{
		DtsGetPibFrom422(pPicInfoLine, Ctx->b422Mode);
		PictureNumber = ((ULONG)(*(pPicInfoLine + 3)) & 0xff)
						| (((ULONG)(*(pPicInfoLine + 2)) << 8)  & 0x0000ff00)
						| (((ULONG)(*(pPicInfoLine + 1)) << 16) & 0x00ff0000)
						| (((ULONG)(*(pPicInfoLine + 0)) << 24) & 0xff000000);

	}else{
		/*The Metadata Is Linear in Flea.*/
		PictureNumber = ((ULONG)(*(pPicInfoLine + 0)) & 0xff)
						| (((ULONG)(*(pPicInfoLine + 1)) << 8)  & 0x0000ff00)
						| (((ULONG)(*(pPicInfoLine + 2)) << 16) & 0x00ff0000)
						| (((ULONG)(*(pPicInfoLine + 3)) << 24) & 0xff000000);
	}

	pOut->PoutFlags |= BC_POUT_FLAGS_PIB_VALID;

	if (Ctx->DevId != BC_PCI_DEVID_FLEA)
	{
		dts_swap_buffer((uint32_t*)&pOut->PicInfo,(uint32_t*)(pPicInfoLine + 4), 32);
		//copy ext PIB
		dts_swap_buffer((uint32_t*)&pOut->PicInfo.other,(uint32_t*)(pPicInfoLine +128), 32);
	}
	else
	{
		memcpy((uint32_t*)&pOut->PicInfo,(uint32_t*)(pPicInfoLine + 4), sizeof(BC_PIC_INFO_BLOCK));
	}

	if (Ctx->DevId == BC_PCI_DEVID_FLEA)
	{
		if (pOut->PicInfo.flags & VDEC_FLAG_BOTTOMFIELD)
			bInterlaced = true;
	}
	else
	{
		if (pOut->PicInfo.flags & VDEC_FLAG_INTERLACED_SRC)
			bInterlaced = true;
	}

	if(bInterlaced)
	{
		Ctx->VidParams.Progressive = FALSE;
		pOut->PicInfo.flags |= VDEC_FLAG_INTERLACED_SRC;
		if (PictureNumber & 0x40000000)
		{
			pOut->PoutFlags |= BC_POUT_FLAGS_FLD_BOT;
			pOut->PicInfo.flags |= VDEC_FLAG_BOTTOMFIELD;
		}
		else
		{
			pOut->PicInfo.flags &= ~VDEC_FLAG_BOTTOMFIELD;
			pOut->PicInfo.flags |= VDEC_FLAG_TOPFIELD;
		}
	}
	else
	{
		Ctx->VidParams.Progressive = TRUE;
		pOut->PicInfo.flags &= ~(VDEC_FLAG_BOTTOMFIELD | VDEC_FLAG_INTERLACED_SRC);
	}

	if(PictureNumber & 0x80000000)
	{
		pOut->PoutFlags |= BC_POUT_FLAGS_ENCRYPTED;
	}

	DtsGetFrameRate(Ctx, pOut);
	//DILDbg_Trace(BC_DIL_DBG_DETAIL, TEXT("DtsGetPictureInfo: PicInfo (W,H):(%d,%d) FR:%ld Flags:0x%x\n"),pOut->PicInfo.width, pOut->PicInfo.height,pOut->PicInfo.frame_rate, pOut->PicInfo.flags);

	/* Replace Y Component data*/
	if(Ctx->DevId == BC_PCI_DEVID_FLEA)
	{
		// In Flea from the HW we are getting Y and UV directly in the y component
			*((uint32_t *)&pOut->Ybuff[0]) = pOut->PicInfo.ycom;
	}
	else
	{
		//Replace Data Back by 422 Mode
		if (Ctx->b422Mode == OUTPUT_MODE422_YUY2)
		{
			//For YUY2
			*(pOut->Ybuff + 6) = ((char *)&pOut->PicInfo.ycom)[3];
			*(pOut->Ybuff + 4) = ((char *)&pOut->PicInfo.ycom)[2];
			*(pOut->Ybuff + 2) = ((char *)&pOut->PicInfo.ycom)[1];
			*(pOut->Ybuff + 0) = ((char *)&pOut->PicInfo.ycom)[0];
		}
		else if (Ctx->b422Mode == OUTPUT_MODE422_UYVY)
		{
			//For UYVY
			*(pOut->Ybuff + 7) = ((char *)&pOut->PicInfo.ycom)[3];
			*(pOut->Ybuff + 5) = ((char *)&pOut->PicInfo.ycom)[2];
			*(pOut->Ybuff + 3) = ((char *)&pOut->PicInfo.ycom)[1];
			*(pOut->Ybuff + 1) = ((char *)&pOut->PicInfo.ycom)[0];
		}
		else
		{
			//For NV12 or YV12
			*((uint32_t*)&pOut->Ybuff[0]) = pOut->PicInfo.ycom;
		}
	}

	if(Ctx->DevId == BC_PCI_DEVID_FLEA)
	{
		//Flea Mode
		if(pOut->PicInfo.timeStamp == 0xFFFFFFFF)
		{
			//For Pre-Load
			pOut->PicInfo.timeStamp = 0xFFFFFFFFFFFFFFFFLL;
		}
		else
		{
			//Normal PTS
			//Change PTS becuase of Shift PTS Issue in FW and 32-bit (ms) and 64-bit (100 ns) Scaling
			pOut->PicInfo.timeStamp = pOut->PicInfo.timeStamp * 2 * 10000;
		}
	}
	else
	{
		/* Retrieve Timestamp */
		if(pOut->PicInfo.flags & VDEC_FLAG_PICTURE_META_DATA_PRESENT)
		{
			sNum = (uint16_t) ( ( (pOut->PicInfo.picture_meta_payload & 0xFF) << 8) |
                                ((pOut->PicInfo.picture_meta_payload& 0xFF00) >> 8) );
			DtsFetchMdata(Ctx,sNum,pOut);
		}
	}
	return BC_STS_SUCCESS;
}

BC_STATUS DtsUpdateVidParams(DTS_LIB_CONTEXT *Ctx, BC_DTS_PROC_OUT *pOut)
{
	Ctx->VidParams.WidthInPixels = pOut->PicInfo.width;
	Ctx->VidParams.HeightInPixels = pOut->PicInfo.height;

	if (pOut->PicInfo.flags & VDEC_FLAG_INTERLACED_SRC)
		Ctx->VidParams.Progressive = FALSE;
	else
		Ctx->VidParams.Progressive = TRUE;

	return BC_STS_SUCCESS;
}


BOOL DtsCheckRptPic(DTS_LIB_CONTEXT *Ctx, BC_DTS_PROC_OUT *pOut)
{
	BOOL bRepeat = FALSE;
	uint8_t nCheckFlag = TOP_FIELD_FLAG;

	if (pOut->PicInfo.picture_number  <3)
		return FALSE;

	if (Ctx->bEOS == TRUE)
	{
		pOut->PicInfo.flags |= VDEC_FLAG_LAST_PICTURE;
		return TRUE;
	}

	if (Ctx->LastPicNum == pOut->PicInfo.picture_number && Ctx->LastSessNum == pOut->PicInfo.sess_num)
	{
		if (Ctx->VidParams.Progressive)
			nCheckFlag = PROGRESSIVE_FRAME_FLAG;
		else if((pOut->PicInfo.flags & VDEC_FLAG_BOTTOMFIELD) == VDEC_FLAG_BOTTOMFIELD)
			nCheckFlag = BOTTOM_FIELD_FLAG;
		else
			nCheckFlag = TOP_FIELD_FLAG;

		//Discard for PullDown
		int nShift = 2;
		uint8_t nFlag = Ctx->PullDownFlag;
		bool bFound = false;

		while(nFlag)
		{
			if((nFlag & 0x03) == nCheckFlag)
			{
				bFound = true;
				break;
			}
			nFlag = nFlag >> 2;
			nShift += 2;
		}

		if(!bFound)
			bRepeat = true;

		Ctx->PullDownFlag = Ctx->PullDownFlag >> nShift;
	}
	else
	{
		if (Ctx->VidParams.Progressive)
		{
			switch(pOut->PicInfo.pulldown)
			{
				case vdecFrame_X1:
					Ctx->PullDownFlag = 0x0003;  //Frame x 1 ==> 00000011
					break;
				case vdecFrame_X2:
					Ctx->PullDownFlag = 0x000f;  //Frame x 2 ==> 00001111
					break;
				case vdecFrame_X3:
					Ctx->PullDownFlag = 0x003f;  //Frame x 3 ==> 00111111
					break;
				case vdecFrame_X4:
					Ctx->PullDownFlag = 0x00ff;  //Frame x 4 ==> 11111111
					break;
				default:
					Ctx->PullDownFlag = 0x0003;  //Frame x 1 ==> 00000011
					break;
			}
		}
		else
		{
			switch(pOut->PicInfo.pulldown)
			{
				case vdecTop:
					Ctx->PullDownFlag = 0x0001;  //Top ==> 00000001
					break;
				case vdecBottom:
					Ctx->PullDownFlag = 0x0002;  //Bottom ==> 00000010
					break;
				case vdecTopBottom:
					Ctx->PullDownFlag = 0x0009;  //TopBottom ==> 00001001
					break;
				case vdecBottomTop:
					Ctx->PullDownFlag = 0x0006;  //BottomTop ==> 00000110
					break;
				case vdecTopBottomTop:
					Ctx->PullDownFlag = 0x0019;  //TopBottomTop ==> 00011001
					break;
				case vdecBottomTopBottom:
					Ctx->PullDownFlag = 0x0026;  //BottomTopBottom ==> 00100110
					break;
				default:
					Ctx->PullDownFlag = 0x0009;  //TopBottom ==> 00001001
					break;
			}
		}
		Ctx->EOSCnt = 0;
	}


	if (Ctx->bEOSCheck && !Ctx->bEOS)
	{
		if (bRepeat)
			Ctx->EOSCnt ++;

		if (Ctx->EOSCnt >= BC_EOS_PIC_COUNT)
		{
			Ctx->bEOS = true;
			pOut->PicInfo.flags |= VDEC_FLAG_LAST_PICTURE;
		}
	}
	Ctx->LastPicNum = pOut->PicInfo.picture_number;
	Ctx->LastSessNum = pOut->PicInfo.sess_num;

	return bRepeat;
}

static void DtsSetupProcOutInfo(DTS_LIB_CONTEXT *Ctx, BC_DTS_PROC_OUT *pOut, BC_IOCTL_DATA *pIo)
{
	if(!pOut || !pIo)
		return; // This is an internal function should never happen..

	if(Ctx->RegCfg.DbgOptions & BC_BIT(6))
	{
	/* Decoder PIC_INFO_ON mode, PIB is NOT embedded in frame */
		if(pIo->u.DecOutData.Flags & COMP_FLAG_PIB_VALID)
		{
			pOut->PoutFlags |= BC_POUT_FLAGS_PIB_VALID;
			DtsCopyAppPIB(Ctx, &pIo->u.DecOutData, pOut);
		}
		if(pIo->u.DecOutData.Flags & COMP_FLAG_DATA_ENC)
			pOut->PoutFlags |= BC_POUT_FLAGS_ENCRYPTED;

		if (pOut->PicInfo.flags & VDEC_FLAG_BOTTOMFIELD)
			pOut->PicInfo.flags |= VDEC_FLAG_INTERLACED_SRC;

		if (pOut->PicInfo.flags & VDEC_FLAG_INTERLACED_SRC)
			Ctx->VidParams.Progressive = FALSE;
		else
			Ctx->VidParams.Progressive = TRUE;
	}

	/* No change in Format Change method */
	if(pIo->u.DecOutData.Flags & COMP_FLAG_FMT_CHANGE)
	{
		if(pIo->u.DecOutData.Flags & COMP_FLAG_PIB_VALID)
		{
			pOut->PoutFlags |= BC_POUT_FLAGS_PIB_VALID;
			DtsCopyAppPIB(Ctx, &pIo->u.DecOutData, pOut);
		}else{
			DebugLog_Trace(LDIL_DBG,"Error: Can't handle F/C w/o PIB_VALID \n");
			return;
		}
		Ctx->HWOutPicHeight = pOut->PicInfo.height;
		// FW returns output picture's stride in PPB.resolution when Format changes.
		Ctx->HWOutPicWidth = DtsGetHWOutputStride(Ctx,(C011_PIB *)&(pIo->u.DecOutData.PibInfo));

		if (pOut->PicInfo.flags & VDEC_FLAG_BOTTOMFIELD)
			pOut->PicInfo.flags |= VDEC_FLAG_INTERLACED_SRC;

		pOut->PoutFlags |= BC_POUT_FLAGS_FMT_CHANGE;
		if(pIo->u.DecOutData.Flags & COMP_FLAG_DATA_VALID){
			DebugLog_Trace(LDIL_DBG,"Error: Data not expected with F/C \n");
			return;
		}
	}

	if(pIo->u.DecOutData.Flags & COMP_FLAG_DATA_VALID)
	{
		pOut->Ybuff = pIo->u.DecOutData.OutPutBuffs.YuvBuff;
		pOut->YBuffDoneSz = pIo->u.DecOutData.OutPutBuffs.YBuffDoneSz;
		pOut->YbuffSz = pIo->u.DecOutData.OutPutBuffs.UVbuffOffset;

		pOut->UVbuff = pIo->u.DecOutData.OutPutBuffs.YuvBuff +
					pIo->u.DecOutData.OutPutBuffs.UVbuffOffset;
		pOut->UVBuffDoneSz = pIo->u.DecOutData.OutPutBuffs.UVBuffDoneSz;
		pOut->UVbuffSz = (pIo->u.DecOutData.OutPutBuffs.YuvBuffSz -
					pIo->u.DecOutData.OutPutBuffs.UVbuffOffset);

		pOut->discCnt = pIo->u.DecOutData.BadFrCnt;
		if(Ctx->FixFlags & DTS_LOAD_FILE_PLAY_FW){
			/* Decoder PIC_INFO_OFF mode, PIB is embedded in frame */
			DtsGetPictureInfo(Ctx, pOut);
		}
	}
}

// Input Meta Data related funtions..
static BC_STATUS	DtsCreateMdataPool(DTS_LIB_CONTEXT *Ctx)
{
	uint32_t		i, mpSz =0;
	DTS_INPUT_MDATA		*temp=NULL;

	mpSz = BC_INPUT_MDATA_POOL_SZ * sizeof(DTS_INPUT_MDATA);

	if( (Ctx->MdataPoolPtr = malloc(mpSz)) == NULL){
		DebugLog_Trace(LDIL_DBG,"Failed to Alloc mem\n");
		return BC_STS_INSUFF_RES;
	}

	memset(Ctx->MdataPoolPtr,0,mpSz);

	temp = (DTS_INPUT_MDATA*)Ctx->MdataPoolPtr;

	Ctx->MDFreeHead = Ctx->MDPendHead = Ctx->MDPendTail = NULL;

	for(i=0; i<BC_INPUT_MDATA_POOL_SZ; i++){
		temp->flink = Ctx->MDFreeHead;
		Ctx->MDFreeHead = temp;
		temp++;
	}

	/* Initialize MData Pending list Params */
	Ctx->MDPendHead = DTS_MDATA_PEND_LINK(Ctx);
	Ctx->MDPendTail = DTS_MDATA_PEND_LINK(Ctx);
	Ctx->InMdataTag = 0;

	return BC_STS_SUCCESS;
}

static void DtsMdataSetIntTag(DTS_LIB_CONTEXT *Ctx, DTS_INPUT_MDATA	*temp)
{
	uint16_t stemp=0;
	DtsLock(Ctx);

	if(Ctx->InMdataTag == 0xFFFF){
		// Skip zero seqNum
		Ctx->InMdataTag = 0;
	}
	temp->IntTag = ++Ctx->InMdataTag & (DTS_MDATA_MAX_TAG|DTS_MDATA_TAG_MASK);
	stemp = (uint16_t)( temp->IntTag & DTS_MDATA_MAX_TAG );
	temp->Spes.SeqNum[0] = (uint8_t)(stemp & 0xFF);
	temp->Spes.SeqNum[1] = (uint8_t)((stemp & 0xFF00) >> 8);
	DtsUnLock(Ctx);
}

static uint32_t DtsMdataGetIntTag(DTS_LIB_CONTEXT *Ctx, uint16_t snum)
{
	uint32_t retTag=0;

	DtsLock(Ctx);
	retTag = ((Ctx->InMdataTag & DTS_MDATA_TAG_MASK) | snum) ;
	DtsUnLock(Ctx);

	return retTag;
}


static BC_STATUS DtsDeleteMdataPool(DTS_LIB_CONTEXT *Ctx)
{
	DTS_INPUT_MDATA		*temp=NULL;

	if(!Ctx || !Ctx->MdataPoolPtr){
		return BC_STS_INV_ARG;
	}
	DtsLock(Ctx);
	/* Remove all Pending elements */
	temp = Ctx->MDPendHead;

	while(temp && temp != DTS_MDATA_PEND_LINK(Ctx)){
		DtsRemoveMdata(Ctx,temp,FALSE);
		temp = Ctx->MDPendHead;
	}

	/* Delete Free Pool */
	Ctx->MDFreeHead = NULL;

	if(Ctx->MdataPoolPtr){
		free(Ctx->MdataPoolPtr);
		Ctx->MdataPoolPtr = NULL;
	}

	DtsUnLock(Ctx);

	return BC_STS_SUCCESS;
}

/*===================== Externs =================================*/

//-----------------------------------------------------------------------
// Name: DtsGetContext
// Description: Get internal context structure from user handle.
//-----------------------------------------------------------------------
DTS_LIB_CONTEXT	*	DtsGetContext(HANDLE userHnd)
{
	DTS_LIB_CONTEXT	*AppCtx = (DTS_LIB_CONTEXT *)userHnd;

	if(!AppCtx)
		return NULL;

	if(AppCtx->Sig != LIB_CTX_SIG)
		return NULL;

	return AppCtx;
}

//-----------------------------------------------------------------------
// Name: DtsIsPend
// Description: Check for Pending request..
//-----------------------------------------------------------------------
BOOL DtsIsPend(DTS_LIB_CONTEXT	*Ctx)
{
	BOOL res =FALSE;
	DtsLock(Ctx);
	res = (Ctx->ProcOutPending);
	DtsUnLock(Ctx);

	return res;
}

//-----------------------------------------------------------------------
// Name: DtsDrvIoctl
// Description: Wrapper for windows IOCTL.
//-----------------------------------------------------------------------
BOOL DtsDrvIoctl
(
	  HANDLE	userHandle,
	  DWORD		dwIoControlCode,
	  LPVOID	lpInBuffer,
	  DWORD		nInBufferSize,
	  LPVOID	lpOutBuffer,
	  DWORD		nOutBufferSize,
	  LPDWORD	lpBytesReturned,
	  BOOL		Async
)
{

	DTS_LIB_CONTEXT	*	Ctx = DtsGetContext(userHandle);
	//unused DWORD	dwTimeout = 0;
	BC_STATUS sts;

	if( !Ctx )
		return FALSE;

	if(Ctx->Sig != LIB_CTX_SIG)
		return FALSE;

	// == FIX ME ==
	// We need to take care of Async ioctl.
	// WILL need to take care of lb bytes returned.
	// Check the existing code.
	//
	if(BC_STS_SUCCESS != (sts = DtsDrvCmd(Ctx,dwIoControlCode,Async,(BC_IOCTL_DATA *)lpInBuffer,FALSE)))
	{
		DebugLog_Trace(LDIL_DBG, "DtsDrvCmd Failed with status %d\n", sts);
		return FALSE;
	}

	return TRUE;
}

//------------------------------------------------------------------------
// Name: DtsDrvCmd
// Description: Wrapper for windows IOCTL using the internal pre-allocated
//              IOCTL_DATA structure. And waits for the completion incase
//				Async path.
//------------------------------------------------------------------------
BC_STATUS DtsDrvCmd(DTS_LIB_CONTEXT	*Ctx,
					DWORD Code,
					BOOL Async,
					BC_IOCTL_DATA *pIoData,
					BOOL Rel)
{
	int rc;
	//DWORD	BytesReturned=0;
	BOOL	locRel=FALSE;//,bRes=0;
	//DWORD	dwTimeout = 0;
	BC_IOCTL_DATA *pIo = NULL;
	BC_STATUS	Sts = BC_STS_SUCCESS ;
	int i = 30;
	uint64_t statusGeneration = 0;
	bool statusObserveAllowed = false;

	if(!Ctx || Ctx->DevHandle < 0){
		DebugLog_Trace(LDIL_DBG,"Invalid arg..%p \n",Ctx);
		return BC_STS_INV_ARG;
	}

	if(!pIoData){
		if(! (pIo = DtsAllocIoctlData(Ctx)) ){
			return BC_STS_INSUFF_RES;
		}
		locRel = TRUE;
	}else{
		pIo = pIoData;
	}

	pIo->RetSts = BC_STS_SUCCESS;
	if (Code == BCM_IOC_GET_DRV_STAT) {
		DtsLock(Ctx);
		statusGeneration = Ctx->recoveryGeneration;
		statusObserveAllowed = !Ctx->recoveryPending &&
			Ctx->controlOwner != DTS_CONTROL_RECOVERY;
		DtsUnLock(Ctx);
	}

	//  We need to take care of async completion.
	// == FIX ME ==
	// We allow only one FW command at a time for LINK
	// prevent additional fw commands from other threads
	if(Ctx->DevId == BC_PCI_DEVID_LINK && Code == BCM_IOC_FW_CMD) {
		while(Ctx->fw_cmd_issued && (i > 0)) {
			usleep(100);
			i--;
		}
		if (i == 0) {
			if (locRel || Rel)
				DtsRelIoctlData(Ctx, pIo);
			return BC_STS_ERROR; // cannot issue second FW command while one is pending
		}
		Ctx->fw_cmd_issued = true;
	}
	rc = ioctl(Ctx->DevHandle, Code, pIo);
	Sts = pIo->RetSts;
	if (Sts == BC_STS_PWR_MGMT) {
		bool observe = Code != BCM_IOC_GET_DRV_STAT;
		if (Code == BCM_IOC_GET_DRV_STAT) {
			DtsLock(Ctx);
			observe = statusObserveAllowed &&
				statusGeneration == Ctx->recoveryGeneration &&
				!Ctx->recoveryPending &&
				Ctx->controlOwner != DTS_CONTROL_RECOVERY;
			if (observe)
				DtsObservePowerState(Ctx, BC_HW_SUSPEND);
			DtsUnLock(Ctx);
		}
		else if (observe)
			DtsObservePowerState(Ctx, BC_HW_SUSPEND);
	}

	if(Ctx->DevId == BC_PCI_DEVID_LINK && Code == BCM_IOC_FW_CMD) {
		Ctx->fw_cmd_issued = false; // FW commands complete synchronously
	}

	if (locRel || Rel)
		DtsRelIoctlData(Ctx, pIo);

	if (rc < 0) {
		DebugLog_Trace(LDIL_DBG,"IOCTL Command Failed %d cmd %x sts %d\n", rc, Code, Sts);
		return BC_STS_ERROR;
	}

	return Sts;
}

//------------------------------------------------------------------------
// Name: DtsRelIoctlData
// Description: Release IOCTL_DATA back to the pool.
//------------------------------------------------------------------------
void DtsRelIoctlData(DTS_LIB_CONTEXT *Ctx, BC_IOCTL_DATA *pIoData)
{
	DtsLock(Ctx);

	pIoData->next = Ctx->pIoDataFreeHd;
    Ctx->pIoDataFreeHd = pIoData;

	DtsUnLock(Ctx);
}

//------------------------------------------------------------------------
// Name: DtsAllocIoctlData
// Description: Acquire IOCTL_DATA From pool.
//------------------------------------------------------------------------
BC_IOCTL_DATA *DtsAllocIoctlData(DTS_LIB_CONTEXT *Ctx)
{
	BC_IOCTL_DATA *temp=NULL;
	DtsLock(Ctx);
    if((temp=Ctx->pIoDataFreeHd) != NULL){
        Ctx->pIoDataFreeHd = Ctx->pIoDataFreeHd->next;
        memset(temp,0,sizeof(*temp));
    }
	DtsUnLock(Ctx);
	if(!temp){
		DebugLog_Trace(LDIL_DBG,"DtsAllocIoctlData Error\n");
	}

    return temp;
}

static uint32_t DtsFreeIoctlDataPool(DTS_LIB_CONTEXT *Ctx)
{
	BC_IOCTL_DATA *current;
	uint32_t count = 0;

	DtsLock(Ctx);
	current = Ctx->pIoDataFreeHd;
	Ctx->pIoDataFreeHd = NULL;
	DtsUnLock(Ctx);

	while (current) {
		BC_IOCTL_DATA *next = current->next;

		free(current);
		current = next;
		count++;
	}

	return count;
}
//------------------------------------------------------------------------
// Name: DtsAllocMemPools
// Description: Allocate memory for application specific configs and RxBuffs
//------------------------------------------------------------------------
/* The recursive context mutex is initialized, but need not be held. */
static BC_STATUS DtsAllocMemPoolsInitialized(DTS_LIB_CONTEXT *Ctx)
{
	uint32_t	i;
	DTS_MPOOL_TYPE	*mp;
	BC_IOCTL_DATA *pIoData;
	BC_STATUS	sts = BC_STS_SUCCESS;
	DTS_RX_BUFFER_LAYOUT layout;
	const bool needsRxBuffers =
		(Ctx->OpMode == DTS_PLAYBACK_MODE || Ctx->OpMode == DTS_DIAG_MODE) &&
		(Ctx->CfgFlags & BC_MPOOL_INCL_YUV_BUFFS);

	/* Reject an invalid RX contract before acquiring any pool allocations.
	 * Contexts which do not request RX backing retain their legacy behavior. */
	if (needsRxBuffers) {
		sts = DtsGetRxBufferLayout(Ctx, &layout);
		if (sts != BC_STS_SUCCESS)
			return sts;
	}

	for(i=0; i< BC_IOCTL_DATA_POOL_SIZE; i++){
		pIoData = (BC_IOCTL_DATA *) malloc(sizeof(BC_IOCTL_DATA));
		if(!pIoData){
			DebugLog_Trace(LDIL_DBG,"DtsInitMemPools: ioctlData pool Alloc Failed\n");
			return BC_STS_INSUFF_RES;
		}
		DtsRelIoctlData(Ctx,pIoData);
	}

	Ctx->pOutData = (BC_IOCTL_DATA *) malloc(sizeof(BC_IOCTL_DATA));
	if(!Ctx->pOutData){
		DebugLog_Trace(LDIL_DBG,"DtsInitMemPools: pOutData \n");
		return BC_STS_INSUFF_RES;
	}

	if((Ctx->OpMode != DTS_PLAYBACK_MODE) && (Ctx->OpMode != DTS_DIAG_MODE))
		return BC_STS_SUCCESS;

	sts = DtsCreateMdataPool(Ctx);
	if(sts != BC_STS_SUCCESS){
		DebugLog_Trace(LDIL_DBG,"InMdata PoolCreation Failed:%x\n",sts);
		return sts;
	}

	if(!(Ctx->CfgFlags & BC_MPOOL_INCL_YUV_BUFFS)){
		return BC_STS_SUCCESS;
	}
	Ctx->MpoolCnt = DtsConfiguredRxPoolCount();

	Ctx->Mpools = (DTS_MPOOL_TYPE*)malloc(Ctx->MpoolCnt * sizeof(DTS_MPOOL_TYPE));
	if(!Ctx->Mpools){
		DebugLog_Trace(LDIL_DBG,"DtsInitMemPools: Mpool alloc failed\n");
		return BC_STS_INSUFF_RES;
	}

	memset(Ctx->Mpools,0,(Ctx->MpoolCnt * sizeof(DTS_MPOOL_TYPE)));

	for(i=0; i<Ctx->MpoolCnt; i++){
		mp = &Ctx->Mpools[i];
		mp->type = BC_MEM_DEC_YUVBUFF |BC_MEM_USER_MODE_ALLOC;
		mp->sz = layout.allocationBytes;
		mp->buff = (uint8_t *)malloc(mp->sz);
		if(!mp->buff){
			DebugLog_Trace(LDIL_DBG,"DtsInitMemPools: Mpool %x failed\n",mp->type);
			return BC_STS_INSUFF_RES;
		}
		//DebugLog_Trace(LDIL_DBG,"DtsInitMemPools: Alloc Mpool %x Buff:%p\n",mp->type,mp->buff);

		memset(mp->buff,0,mp->sz);
	}

	return BC_STS_SUCCESS;
}
BC_STATUS DtsAllocMemPools(DTS_LIB_CONTEXT *Ctx)
{
	if (!Ctx)
		return BC_STS_INV_ARG;
	const BC_STATUS sts = DtsInitLock(Ctx);
	if (sts != BC_STS_SUCCESS)
		return sts;
	return DtsAllocMemPoolsInitialized(Ctx);
}
BC_STATUS DtsAllocMemPools_dbg(DTS_LIB_CONTEXT *Ctx)
{
	uint32_t	i;
	BC_IOCTL_DATA *pIoData;

	if(!Ctx){
		return BC_STS_INV_ARG;
	}

	const BC_STATUS sts = DtsInitLock(Ctx);
	if (sts != BC_STS_SUCCESS)
		return sts;

	for(i=0; i< BC_IOCTL_DATA_POOL_SIZE; i++){
		pIoData = (BC_IOCTL_DATA *) malloc(sizeof(BC_IOCTL_DATA));
		if(!pIoData){
			DebugLog_Trace(LDIL_DBG,"DtsInitMemPools: ioctlData pool Alloc Failed\n");
			return BC_STS_INSUFF_RES;
		}
		DtsRelIoctlData(Ctx,pIoData);
	}

	Ctx->pOutData = (BC_IOCTL_DATA *) malloc(sizeof(BC_IOCTL_DATA));
	if(!Ctx->pOutData){
		DebugLog_Trace(LDIL_DBG,"DtsInitMemPools: pOutData \n");
		return BC_STS_INSUFF_RES;
	}

	return BC_STS_SUCCESS;
}
//------------------------------------------------------------------------
// Name: DtsReleaseMemPools
// Description: Release application specific allocations.
//------------------------------------------------------------------------
void DtsReleaseMemPools(DTS_LIB_CONTEXT *Ctx)
{
	uint32_t	i,cnt=0;
	DTS_MPOOL_TYPE	*mp;
	const bool quarantine_output = Ctx && Ctx->bMapOutBufDirty;


	if (!Ctx) //vgd
		return;

	/* DtsReleaseInterface retires driver ownership and closes the device
	 * before freeing any userspace capture backing here. */
	if (Ctx->Mpools) {
		for (i = 0; i < Ctx->MpoolCnt; i++){
			mp = &Ctx->Mpools[i];
			if (mp->buff){
				/* A failed destructive cleanup can mean DMA ownership is
				 * intentionally retained by the kernel. Do not make that
				 * backing available for allocator reuse. */
				if (!quarantine_output ||
				    !(mp->type & BC_MEM_DEC_YUVBUFF))
					free(mp->buff);
			}
		}
		if (quarantine_output)
			DebugLog_Trace(LDIL_DBG,
				"DtsReleaseMemPools: retaining capture backing after unconfirmed unmap\n");
		free(Ctx->Mpools);
	}

	/* Release IOCTL_DATA pool without treating normal exhaustion as an error. */
	cnt = DtsFreeIoctlDataPool(Ctx);

	if(cnt != BC_IOCTL_DATA_POOL_SIZE){
		DebugLog_Trace(LDIL_DBG,"DtsReleaseMemPools: pIoData MemPool Leak: %d..\n",cnt);
	}

	if(Ctx->pOutData)
		free(Ctx->pOutData);

	if(Ctx->MdataPoolPtr)
		DtsDeleteMdataPool(Ctx);

	if (Ctx->VidParams.pMetaData)
		free(Ctx->VidParams.pMetaData);

	DtsDelLock(Ctx);
}
void DtsReleaseMemPools_dbg(DTS_LIB_CONTEXT *Ctx)
{
	uint32_t	cnt=0;


	if(!Ctx || !Ctx->Mpools){
		return;
	}

	/* Release IOCTL_DATA pool without treating normal exhaustion as an error. */
	cnt = DtsFreeIoctlDataPool(Ctx);

	if(cnt != BC_IOCTL_DATA_POOL_SIZE){
		DebugLog_Trace(LDIL_DBG,"DtsReleaseMemPools: pIoData MemPool Leak: %d..\n",cnt);
	}

	if(Ctx->pOutData)
		free(Ctx->pOutData);

}
//------------------------------------------------------------------------
// Name: DtsAddOutBuff
// Description: Pass on user mode allocated Rx buffs to driver.
//------------------------------------------------------------------------
BC_STATUS DtsAddOutBuff(DTS_LIB_CONTEXT *Ctx, uint8_t *buff, uint32_t BuffSz, uint32_t flags)
{
	DTS_RX_BUFFER_LAYOUT layout;
	BC_IOCTL_DATA	*pIocData = NULL;
	BC_STATUS sts;

	sts = DtsValidateRxBuffer(Ctx, buff, BuffSz, &layout);
	if (sts != BC_STS_SUCCESS)
		return sts;

	if(!(pIocData = DtsAllocIoctlData(Ctx))) {
		DebugLog_Trace(LDIL_DBG,"Cannot Allocate IOCTL data\n");
		return BC_STS_INSUFF_RES;
	}

	pIocData->u.RxBuffs.YuvBuff = buff;
	pIocData->u.RxBuffs.YuvBuffSz = layout.registeredBytes;
	pIocData->u.RxBuffs.b422Mode = layout.outputFormat;
	pIocData->u.RxBuffs.UVbuffOffset = layout.uvOffset;

	return DtsDrvCmd(Ctx,BCM_IOC_ADD_RXBUFFS,0, pIocData, TRUE);
}
//------------------------------------------------------------------------
// Name: DtsRelRxBuff
// Description: Release Rx buffers back to driver.
//------------------------------------------------------------------------
BC_STATUS DtsRelRxBuff(DTS_LIB_CONTEXT *Ctx, BC_DEC_YUV_BUFFS *buff, BOOL SkipAddBuff)
{
	BC_STATUS	sts;

	if(!Ctx || !buff)
	{
		DebugLog_Trace(LDIL_DBG,"DtsRelRxBuff: Invalid Arguments\n");
		return BC_STS_INV_ARG;
	}
	DtsLock(Ctx);
	const bool residual = Ctx->outputPhase == DTS_OUTPUT_RETIRE_ONLY;
	DtsUnLock(Ctx);
	if (residual)
		return BC_STS_BUSY;

	if(SkipAddBuff){
		DtsDecPend(Ctx);
		return BC_STS_SUCCESS;
	}

	Ctx->pOutData->u.RxBuffs.b422Mode = Ctx->b422Mode;
	Ctx->pOutData->u.RxBuffs.UVBuffDoneSz =0;
	Ctx->pOutData->u.RxBuffs.YBuffDoneSz=0;

	sts = DtsDrvCmd(Ctx,BCM_IOC_ADD_RXBUFFS,0, Ctx->pOutData, FALSE);

	if(sts != BC_STS_SUCCESS)
	{
		DebugLog_Trace(LDIL_DBG,"DtsRelRxBuff: Failed Sts:%x .. \n",sts);
	}

	if(sts == BC_STS_SUCCESS)
		DtsDecPend(Ctx);

	return sts;
}

/* Operate only on a fetched host lease, after PIB extraction/first-word
 * repair. The capture/PIB source and subsequent repost remain YUY2.
 */
BC_STATUS DtsPrepareOutputPacking(DTS_LIB_CONTEXT *Ctx, BC_DTS_PROC_OUT *output,
	uint32_t allocationBytes)
{
	if (!Ctx || !output)
		return BC_STS_INV_ARG;
	output->b422Mode = Ctx->b422Mode;
	if (!Ctx->softwareUyvy)
		return BC_STS_SUCCESS;
	if (Ctx->DevId != BC_PCI_DEVID_FLEA || Ctx->b422Mode != OUTPUT_MODE422_YUY2)
		return BC_STS_INV_ARG;
	if ((output->PoutFlags & BC_POUT_FLAGS_FMT_CHANGE) ||
		(output->PicInfo.flags & VDEC_FLAG_EOS)) {
		output->b422Mode = OUTPUT_MODE422_UYVY;
		return BC_STS_SUCCESS;
	}
	if (output->PoutFlags & BC_POUT_FLAGS_ENCRYPTED)
		return BC_STS_NOT_IMPL;
	const uint32_t width = output->PicInfo.width, height = output->PicInfo.height;
	const bool field = !Ctx->VidParams.Progressive;
	if (!(output->PoutFlags & BC_POUT_FLAGS_PIB_VALID) || !output->Ybuff ||
		!width || (width & 1) || width > 1920 || !height || height > 1088 ||
		Ctx->HWOutPicWidth < width || Ctx->HWOutPicWidth > 1920 ||
		(field && (height & 1)))
		return BC_STS_IO_XFR_ERROR;
	const uint32_t rows = field ? height / 2 : height;
	const uint64_t rowBytes = (uint64_t)width * 2;
	const uint64_t pitch = (uint64_t)Ctx->HWOutPicWidth * 2;
	const uint64_t doneBytes = (uint64_t)output->YBuffDoneSz * 4;
	/* Check both the transfer's DWORD extent and the real registration.
	 * Division keeps pointer arithmetic representable on legacy i386.
	 */
	if (!rows || pitch > SIZE_MAX || doneBytes > allocationBytes ||
		doneBytes < rowBytes || allocationBytes < rowBytes ||
		(uint64_t)(rows - 1) > (doneBytes - rowBytes) / pitch ||
		(uint64_t)(rows - 1) > ((uint64_t)allocationBytes - rowBytes) / pitch)
		return BC_STS_IO_XFR_ERROR;
	for (uint32_t y = 0; y < rows; ++y) {
		uint8_t *row = output->Ybuff + (size_t)y * (size_t)pitch;
		size_t x = 0;
#if defined(__SSE2__)
		for (; rowBytes - x >= 16; x += 16) {
			const __m128i value = _mm_loadu_si128((const __m128i *)(row + x));
			_mm_storeu_si128((__m128i *)(row + x),
				_mm_or_si128(_mm_srli_epi16(value, 8), _mm_slli_epi16(value, 8)));
		}
#endif
		for (; x < rowBytes; x += 2) {
			const uint8_t first = row[x];
			row[x] = row[x + 1];
			row[x + 1] = first;
		}
	}
	output->b422Mode = OUTPUT_MODE422_UYVY;
	return BC_STS_SUCCESS;
}

//------------------------------------------------------------------------
// Name: DtsFetchOutInterruptible
// Description: Get uncompressed video data from hardware.
//              This function is interruptable procOut for
//              multi-threaded scenerios ONLY..
//              SUCCESS transfers one pending output to the caller. Every
//              failure leaves no caller-owned buffer. A failed cancellation
//              repost retains the pending count for teardown, not for a
//              rejected caller to release or a new fetch to reuse.
//------------------------------------------------------------------------
BC_STATUS DtsFetchOutInCall(DTS_LIB_CONTEXT *Ctx, BC_DTS_PROC_OUT *pOut, uint32_t dwTimeout)
{
	BC_STATUS sts = BC_STS_SUCCESS;

	if(!Ctx ||  !pOut)
		return BC_STS_INV_ARG;

	sts = DtsAcquireOutput(Ctx);
	if (sts != BC_STS_SUCCESS)
		return sts;

	memset(Ctx->pOutData,0,sizeof(*Ctx->pOutData));

	sts=DtsDrvCmd(Ctx,BCM_IOC_FETCH_RXBUFF,0,Ctx->pOutData,FALSE);

	if(sts == BC_STS_SUCCESS)
	{
		sts = Ctx->pOutData->RetSts;
		if(sts != BC_STS_SUCCESS)
		{
			DtsDecPend(Ctx);
		} else {
			bool invalidReceipt = false;
			/* FETCH may be the first successful suffix after S3.  Bind
			 * interpretation of its old buffer and all context-format updates
			 * to the same generation check used by recovery cancellation. */
			DtsLock(Ctx);
			const bool cancel = Ctx->CancelWaiting;
			const bool recoveryCancel = DtsRecoveryCanceledOutputLocked(Ctx);
			if (!cancel && !recoveryCancel) {
				const BC_DEC_YUV_BUFFS &buffer =
					Ctx->pOutData->u.DecOutData.OutPutBuffs;
				/* Reject malformed DATA_VALID receipts before deriving plane
				 * pointers or interpreting PIB.  LINK may have an empty UV
				 * extent, so offset == size remains valid. */
				invalidReceipt =
					(Ctx->pOutData->u.DecOutData.Flags & COMP_FLAG_DATA_VALID) &&
					(!buffer.YuvBuff || !buffer.YuvBuffSz ||
					 buffer.UVbuffOffset > buffer.YuvBuffSz);
				if (invalidReceipt)
					Ctx->pOutData->RetSts = BC_STS_IO_XFR_ERROR;
				else
					DtsSetupProcOutInfo(Ctx,pOut,Ctx->pOutData);
				/* Embedded PIB parsing can turn an ioctl SUCCESS into
				 * NO_DATA/IO_XFR_ERROR (notably the Flea EOS sentinel). */
				sts = Ctx->pOutData->RetSts;
			}
			DtsUnLock(Ctx);
			if (cancel || recoveryCancel) {
				(void)DtsRelRxBuff(Ctx,&Ctx->pOutData->u.RxBuffs,FALSE);
				pOut->Ybuff = pOut->UVbuff = NULL;
				return recoveryCancel ? BC_STS_BUSY : BC_STS_IO_USER_ABORT;
			}
			if (invalidReceipt) {
				/* FETCH already transferred a capture owner.  A rejected
				 * receipt must still retire it; failed repost keeps pending
				 * ownership for the existing retire-only teardown path. */
				const BC_STATUS released = DtsRelRxBuff(Ctx,
					&Ctx->pOutData->u.RxBuffs, FALSE);
				if (released != BC_STS_SUCCESS)
					sts = released;
			} else if (sts != BC_STS_SUCCESS)
				DtsDecPend(Ctx);
		}

	}else{
		DebugLog_Trace(LDIL_DBG,"DtsFetchOutInterruptible: Failed:%x\n",sts);
		DtsDecPend(Ctx);
	}

	if (sts != BC_STS_SUCCESS)
		pOut->Ybuff = pOut->UVbuff = NULL;
	return DtsNormalizeRecoveryOutputAbort(Ctx, pOut, sts);
}
//------------------------------------------------------------------------
// The old private entry point also owns a complete call phase. Public output
// loops use InCall so one phase spans internal drop/repost/refetch operations.
BC_STATUS DtsFetchOutInterruptible(DTS_LIB_CONTEXT *Ctx, BC_DTS_PROC_OUT *output, uint32_t timeout)
{
	if (!Ctx || !output)
		return BC_STS_INV_ARG;
	BC_STATUS status = DtsBeginOutputCall(Ctx);
	if (status != BC_STS_SUCCESS)
		return status;
	output->Ybuff = output->UVbuff = NULL;
	status = DtsFetchOutInCall(Ctx, output, timeout);
	return DtsPublishOutput(Ctx, output, status);
}

// Name: DtsCancelFetchOutInt
// Description: Cancel Pending ProcOut Request..
//------------------------------------------------------------------------
BC_STATUS DtsCancelFetchOutInt(DTS_LIB_CONTEXT *Ctx)
{
	bool pend = false;
	uint32_t cnt;

	DtsLock(Ctx);
	/* Only the caller that owns false->true may clear the cancellation flag.
	 * Refuse before fast success too: an older waiter can still access Ctx. */
	if (Ctx->CancelWaiting) {
		DtsUnLock(Ctx);
		return BC_STS_BUSY;
	}
	if (Ctx->outputPhase == DTS_OUTPUT_RETIRE_ONLY ||
	    (!Ctx->ProcOutPending && Ctx->outputPhase == DTS_OUTPUT_IDLE)) {
		Ctx->CancelWaiting = 0;
		DtsUnLock(Ctx);
		return BC_STS_SUCCESS;
	}

	Ctx->CancelWaiting = 1;
	DtsUnLock(Ctx);

	/* Worst case scenerio the timeout should happen.. */
	cnt = BC_PROC_OUTPUT_TIMEOUT / 100;

	do{
		usleep(100 * 1000);
		DtsLock(Ctx);
		pend = Ctx->outputPhase == DTS_OUTPUT_ACTIVE ||
			Ctx->outputPhase == DTS_OUTPUT_DELIVERING ||
			Ctx->outputPhase == DTS_OUTPUT_RETURNED ||
			(Ctx->ProcOutPending && Ctx->outputPhase != DTS_OUTPUT_RETIRE_ONLY);
		DtsUnLock(Ctx);
	}while( (pend) && (cnt--) );

	if(pend){
		DebugLog_Trace(LDIL_DBG,"DtsCancelFetchOutInt: TimeOut\n");
		DtsLock(Ctx);
		Ctx->CancelWaiting = 0;
		DtsUnLock(Ctx);
		return BC_STS_TIMEOUT;
	}

	DtsLock(Ctx);
	Ctx->CancelWaiting = 0;
	DtsUnLock(Ctx);

	return BC_STS_SUCCESS;
}

//------------------------------------------------------------------------
// Name: DtsUnmapYUVBuffs
// Description: Destructively release every driver-owned output buffer.
//------------------------------------------------------------------------
BC_STATUS DtsUnmapYUVBuffs(DTS_LIB_CONTEXT *Ctx)
{
	BC_IOCTL_DATA *pIocData;
	BC_STATUS sts;

	if (!Ctx)
		return BC_STS_INV_ARG;

	DtsLock(Ctx);
	if (Ctx->outputPhase == DTS_OUTPUT_ACTIVE ||
	    Ctx->outputPhase == DTS_OUTPUT_DELIVERING ||
	    Ctx->outputPhase == DTS_OUTPUT_RETURNED || Ctx->CancelWaiting ||
	    (Ctx->ProcOutPending && Ctx->outputPhase != DTS_OUTPUT_RETIRE_ONLY)) {
		DtsUnLock(Ctx);
		return BC_STS_BUSY;
	}
	if (!Ctx->bMapOutBufDirty) {
		const bool residual = Ctx->outputPhase == DTS_OUTPUT_RETIRE_ONLY && Ctx->ProcOutPending;
		DtsUnLock(Ctx);
		return residual ? BC_STS_BUSY : BC_STS_SUCCESS;
	}

	pIocData = DtsAllocIoctlData(Ctx);
	if (!pIocData) {
		DtsUnLock(Ctx);
		return BC_STS_INSUFF_RES;
	}

	/* Once destructive cleanup enters the kernel, a failed copy-out cannot
	 * tell whether the complete set still exists. Require a cleanup/re-map
	 * before treating it as complete again. */
	Ctx->bMapOutBufDone = false;
	pIocData->u.FlushRxCap.bDiscardOnly = FALSE;
	sts = DtsDrvCmd(Ctx, BCM_IOC_FLUSH_RX_CAP, 0, pIocData, TRUE);
	if (sts == BC_STS_SUCCESS) {
		Ctx->bMapOutBufDone = false;
		Ctx->bMapOutBufDirty = false;
		if (Ctx->outputPhase == DTS_OUTPUT_RETIRE_ONLY) {
			Ctx->ProcOutPending = 0;
			Ctx->outputPhase = DTS_OUTPUT_IDLE;
		}
	}
	DtsUnLock(Ctx);
	return sts;
}

//------------------------------------------------------------------------
// Name: DtsMapYUVBuffs
// Description: Pass user mode pre-allocated buffers to driver for mapping.
//------------------------------------------------------------------------
BC_STATUS DtsMapYUVBuffs(DTS_LIB_CONTEXT *Ctx)
{
	uint32_t i;
	BC_STATUS sts = BC_STS_SUCCESS;
	DTS_MPOOL_TYPE *mp;
	DTS_RX_BUFFER_LAYOUT layout;

	if (!Ctx)
		return BC_STS_INV_ARG;

	DtsLock(Ctx);
	if (Ctx->bMapOutBufDone)
		goto done;

	/* A failed rollback leaves ownership ambiguous. Prove that the old set
	 * is detached before attempting any new ADD_RXBUFFS command. */
	if (Ctx->bMapOutBufDirty) {
		sts = DtsUnmapYUVBuffs(Ctx);
		if (sts != BC_STS_SUCCESS)
			goto done;
	}

	if (!Ctx->Mpools || !(Ctx->CfgFlags & BC_MPOOL_INCL_YUV_BUFFS))
		goto done;

	/* Validate the complete set before the first ADD.  Once an ADD enters the
	 * kernel, even a failed copy-out makes ownership ambiguous and requires
	 * the destructive rollback below. */
	for (i = 0; i < Ctx->MpoolCnt; i++) {
		mp = &Ctx->Mpools[i];
		if (!(mp->type & BC_MEM_DEC_YUVBUFF))
			continue;
		sts = DtsValidateRxBuffer(Ctx, mp->buff, mp->sz, &layout);
		if (sts != BC_STS_SUCCESS)
			goto done;
	}

	for (i = 0; i < Ctx->MpoolCnt; i++) {
		mp = &Ctx->Mpools[i];
		if (mp->type & BC_MEM_DEC_YUVBUFF) {
			/* A negative ioctl result can follow successful driver admission,
			 * so ownership becomes dirty before entering the kernel. */
			Ctx->bMapOutBufDirty = true;
			sts = DtsAddOutBuff(Ctx, mp->buff, mp->sz, mp->type);
			if (sts != BC_STS_SUCCESS) {
				BC_STATUS rollback_sts;

				DebugLog_Trace(LDIL_DBG,
					"Map YUV buffs Failed [%x]\n", sts);
				rollback_sts = DtsUnmapYUVBuffs(Ctx);
				if (rollback_sts != BC_STS_SUCCESS)
					sts = rollback_sts;
				goto done;
			}
		}
	}

	Ctx->bMapOutBufDone = true;
done:
	DtsUnLock(Ctx);
	return sts;
}
//------------------------------------------------------------------------
// Name: DtsInitInterface
// Description: Do application specific allocation and other initialization.
//------------------------------------------------------------------------
BC_STATUS DtsInitInterface(int hDevice, HANDLE *RetCtx, uint32_t mode)
{

	DTS_LIB_CONTEXT *Ctx = NULL;
	BC_STATUS	sts = BC_STS_SUCCESS;
	pthread_t tx_thread;
	int ret = 0;

	if (!RetCtx)
		return BC_STS_INV_ARG;
	*RetCtx = NULL;

	Ctx = (DTS_LIB_CONTEXT*)malloc(sizeof(*Ctx));
	if(!Ctx){
		DebugLog_Trace(LDIL_DBG,"DtsInitInterface: Ctx alloc failed\n");
		return BC_STS_INSUFF_RES;
	}

	memset(Ctx,0,sizeof(*Ctx));

	/* Initialize Application specific params. */
	Ctx->Sig		= LIB_CTX_SIG;
	Ctx->DevHandle  = hDevice;
	Ctx->OpMode		= mode;
	Ctx->CfgFlags	= BC_DTS_DEF_CFG;
	Ctx->b422Mode	= OUTPUT_MODE420;
	Ctx->softwareUyvy = false;

	Ctx->VidParams.MediaSubType = BC_MSUBTYPE_INVALID;
	Ctx->VidParams.StartCodeSz = 0;
	Ctx->VidParams.StreamType = BC_STREAM_TYPE_ES;
//	Ctx->InSampleCount = 0;
	/* Set Pixel height & width */
	if(Ctx->CfgFlags & BC_PIX_WID_1080){
		Ctx->VidParams.HeightInPixels = 1080;
		Ctx->VidParams.WidthInPixels = 1920;
	}else{
		Ctx->VidParams.HeightInPixels = 720;
		Ctx->VidParams.WidthInPixels = 1280;
	}
	Ctx->VidParams.pMetaData = NULL;

	sts = DtsInitLock(Ctx);
	if (sts != BC_STS_SUCCESS) {
		/* Cleanup uses thLock; never publish a context without that mutex. */
		free(Ctx);
		return sts;
	}
	sts = DtsAllocMemPoolsInitialized(Ctx);
	if(sts != BC_STS_SUCCESS){
		DebugLog_Trace(LDIL_DBG,"DtsAllocMemPools failed Sts:%d\n",sts);
		*RetCtx = (HANDLE)Ctx;
		return sts;
	}

	if(!(Ctx->CfgFlags & BC_ADDBUFF_MOVE)){
		sts = DtsMapYUVBuffs(Ctx);
		if(sts != BC_STS_SUCCESS){
			DebugLog_Trace(LDIL_DBG,"DtsMapYUVBuffs failed Sts:%d\n",sts);
			*RetCtx = (HANDLE)Ctx;
			return sts;
		}
	}

	/* Return partial ownership to the caller on every remaining failure.
	 * No worker may observe incomplete buffers or fail an allocation later. */
	*RetCtx = (HANDLE)Ctx;
	sts = txBufInit(&Ctx->circBuf, CIRC_TX_BUF_SIZE);
	if(sts != BC_STS_SUCCESS)
		return sts;
	ret = posix_memalign((void**)&Ctx->alignBuf, 128, ALIGN_BUF_SIZE);
	if(ret)
		return BC_STS_INSUFF_RES;
	ret = posix_memalign((void**)&Ctx->txScratch, 128, CIRC_TX_BUF_SIZE);
	if(ret)
		return BC_STS_INSUFF_RES;

	/* Default attributes are joinable. A failed create leaves its output
	 * unspecified, so publish the thread ID only after successful creation. */
	ret = pthread_create(&tx_thread, NULL, txThreadProc, Ctx);
	if(ret)
		return BC_STS_INSUFF_RES;
	Ctx->htxThread = tx_thread;
	return BC_STS_SUCCESS;
}

//------------------------------------------------------------------------
// Name: DtsReleaseInterface
// Description: Do application specific Release and other initialization.
//------------------------------------------------------------------------
void DtsJoinTxThread(DTS_LIB_CONTEXT *Ctx)
{
	pthread_t tx_thread;

	DtsLock(Ctx);
	Ctx->txThreadExit = true;
	tx_thread = Ctx->htxThread;
	DtsUnLock(Ctx);
	txBufShutdown(&Ctx->circBuf);
	if (tx_thread)
		pthread_join(tx_thread, NULL);
	DtsLock(Ctx);
	Ctx->htxThread = 0;
	DtsUnLock(Ctx);
}

BC_STATUS DtsReleaseInterface(DTS_LIB_CONTEXT *Ctx)
{
	BC_STATUS cleanup_sts = BC_STS_SUCCESS;
	bool terminal_decoder_owner;

	if(!Ctx)
		return BC_STS_INV_ARG;

	terminal_decoder_owner = Ctx->OpMode == DTS_PLAYBACK_MODE &&
		(Ctx->State != BC_DEC_STATE_CLOSE ||
		 Ctx->recoveryRetireDecoderOwner) && Ctx->ProcessID != 0 &&
		bc_dil_glob_ptr && bc_dil_glob_ptr->g_bDecOpened &&
		bc_dil_glob_ptr->g_nProcID == Ctx->ProcessID;

	DtsJoinTxThread(Ctx);
	DtsReleasePESConverter((HANDLE)Ctx);
	// de-Allocate circular buffer
	txBufFree(&Ctx->circBuf);
	if(Ctx->alignBuf)
		free(Ctx->alignBuf);
	free(Ctx->txScratch);

	if(Ctx->DevHandle >= 0)
	{
		/* Try to establish an explicit unmap acknowledgement. A terminal
		 * DMA fault may retain kernel ownership even after RELEASE/close. */
		cleanup_sts = DtsUnmapYUVBuffs(Ctx);
		if (cleanup_sts != BC_STS_SUCCESS)
			DebugLog_Trace(LDIL_DBG,
				"DtsReleaseInterface: capture cleanup failed: %d\n",
				cleanup_sts);
		const BC_STATUS release_sts = DtsReleaseUserHandle(Ctx);
		if (cleanup_sts == BC_STS_SUCCESS)
			cleanup_sts = release_sts;

		if(0 != close(Ctx->DevHandle)) {
			DebugLog_Trace(LDIL_DBG,"DtsDeviceClose: Close Handle Failed with error %d\n",errno);
			if (cleanup_sts == BC_STS_SUCCESS)
				cleanup_sts = BC_STS_ERROR;
		}
		/* Linux consumes the descriptor even when close reports a late error.
		 * A retry could close a descriptor reused by another thread. */
		Ctx->DevHandle = -1;
	}

	/* A failed firmware close remains retryable while its handle is live.
	 * This path consumes that handle after releasing the driver session, so
	 * retire only the decoder ownership represented by this context. */
	if (terminal_decoder_owner) {
		DtsSetDecStat(false, Ctx->ProcessID);
		Ctx->recoveryRetireDecoderOwner = false;
	}

	/* Normal ownership is retired before userspace backing is freed. If an
	 * unmap was not confirmed, DtsReleaseMemPools quarantines that backing. */
	DtsReleaseMemPools(Ctx);

	DtsSetHwInitSts(BC_DIL_HWINIT_NOT_YET);

	DtsDelDilShMem();

	free(Ctx);

	return cleanup_sts;

}

//------------------------------------------------------------------------
// Name: DtsReleaseUserHandle
// Description: Notfiy the driver to release the user handle
// Should be called right before close
//------------------------------------------------------------------------
BC_STATUS DtsReleaseUserHandle(DTS_LIB_CONTEXT* Ctx)
{
	BC_IOCTL_DATA	pIo;
	BC_STATUS		sts = BC_STS_SUCCESS;

	memset(&pIo, 0, sizeof(BC_IOCTL_DATA));

	if( (sts=DtsDrvCmd(Ctx,BCM_IOC_RELEASE,0,&pIo,FALSE)) != BC_STS_SUCCESS){
		DebugLog_Trace(LDIL_DBG,"DtsReleaseUserHandle: Ioctl failed: %d\n",sts);
		return sts;
	}

	return sts;
}

//------------------------------------------------------------------------
// Name: DtsNotifyOperatingMode
// Description: Notfiy the operating mode to driver.
//------------------------------------------------------------------------
BC_STATUS DtsNotifyOperatingMode(HANDLE hDevice,uint32_t Mode)
{
	BC_IOCTL_DATA			*pIocData = NULL;
	DTS_LIB_CONTEXT			*Ctx = NULL;
	BC_STATUS				sts = BC_STS_SUCCESS;

	DTS_GET_CTX(hDevice,Ctx);

	if(!(pIocData = DtsAllocIoctlData(Ctx)))
		return BC_STS_INSUFF_RES;

	pIocData->u.NotifyMode.Mode = Mode ; /* Setting the 31st bit to indicate that this is not the timeout value */

	if( (sts=DtsDrvCmd(Ctx,BCM_IOC_NOTIFY_MODE,0,pIocData,FALSE)) != BC_STS_SUCCESS){
		DtsRelIoctlData(Ctx,pIocData);
		DebugLog_Trace(LDIL_DBG,"DtsNotifyMode: Ioctl failed: %d\n",sts);
		return sts;
	}

	DtsRelIoctlData(Ctx,pIocData);
	return sts;
}
//------------------------------------------------------------------------
// Name: DtsSetupConfiguration
// Description: Setup HW specific Configuration.
//------------------------------------------------------------------------
BC_STATUS DtsSetupConfig(DTS_LIB_CONTEXT *Ctx, uint32_t did, uint32_t rid, uint32_t FixFlags)
{
	Ctx->DevId = did;
	if (did == BC_PCI_DEVID_FLEA)
		Ctx->b422Mode = OUTPUT_MODE422_YUY2;
	Ctx->hwRevId = rid;
	Ctx->FixFlags = FixFlags;

	if(Ctx->DevId == BC_PCI_DEVID_LINK){
		Ctx->RegCfg.DbgOptions = BC_DTS_DEF_OPTIONS_LINK;
	}else{
		Ctx->RegCfg.DbgOptions = BC_DTS_DEF_OPTIONS;
	}
	DtsGetBCRegConfig(Ctx);

	//Forcing the use of certificate in case of Link.

	if( (Ctx->DevId == BC_PCI_DEVID_LINK)&& (!(Ctx->RegCfg.DbgOptions & BC_BIT(5))) ){

		Ctx->RegCfg.DbgOptions |= BC_BIT(5);
	}


	Ctx->capInfo.ColorCaps.Count = 0;
	if (Ctx->DevId == BC_PCI_DEVID_LINK)
	{
		Ctx->capInfo.ColorCaps.Count =3;
		Ctx->capInfo.ColorCaps.OutFmt[0] = OUTPUT_MODE420;
		Ctx->capInfo.ColorCaps.OutFmt[1] = OUTPUT_MODE422_YUY2;
		Ctx->capInfo.ColorCaps.OutFmt[2] = OUTPUT_MODE422_UYVY;
		Ctx->capInfo.flags = PES_CONV_SUPPORT;

		//Decoder Capability
		Ctx->capInfo.DecCaps = BC_DEC_FLAGS_H264 | BC_DEC_FLAGS_MPEG2 | BC_DEC_FLAGS_VC1;
	}
	else if(Ctx->DevId == BC_PCI_DEVID_DOZER)
	{
		Ctx->capInfo.ColorCaps.Count =1;
		Ctx->capInfo.ColorCaps.OutFmt[0] = OUTPUT_MODE420;
		Ctx->capInfo.flags = PES_CONV_SUPPORT;

		//Decoder Capability
		Ctx->capInfo.DecCaps = BC_DEC_FLAGS_H264 | BC_DEC_FLAGS_MPEG2 | BC_DEC_FLAGS_VC1;
	}

	Ctx->capInfo.Reserved1 = NULL;

	return BC_STS_SUCCESS;
}

//------------------------------------------------------------------------
// Name: DtsGetBCRegConfig
// Description: Setup Register Sub-Key values.
//------------------------------------------------------------------------
BC_STATUS DtsGetBCRegConfig(DTS_LIB_CONTEXT	*Ctx)
{

	return BC_STS_NOT_IMPL;
}

int dtscallback(struct dl_phdr_info *info, size_t size, void *data)
{
    int ret=0,dilpath_len=0;

	//char* pSearchStr = (char*)info->dlpi_name;
	char * temp=NULL;
	//temp = strstr((char *)pSearchStr,(const char *)"/libdil.so");
	if(NULL != (temp= (char *)strstr(info->dlpi_name,(const char *)"/libcrystal.so"))){
		/*we found the loaded dil, set teh return value to non-zero so that
		 the callback won't be called anymore*/
		ret = 1;

	}

	if(ret!=0){
		dilpath_len = (temp-info->dlpi_name)+1; //we want the slash also to be copied
		strncpy((char*)data, info->dlpi_name,dilpath_len);

	}

    return dilpath_len;
}


//------------------------------------------------------------------------
// Name: DtsGetFirmwareFiles
// Description: Setup Firmware Filenames.
//------------------------------------------------------------------------
BC_STATUS DtsGetFirmwareFiles(DTS_LIB_CONTEXT *Ctx)
{
	const char *fwfile;
#ifndef __APPLE__
	const char fwdir[] = "/lib/firmware/";
#else
	const char fwdir[] = "/usr/lib/";
#endif

	fwfile = Ctx->DevId == BC_PCI_DEVID_FLEA ?
		FWBINFILE_70015 : FWBINFILE_70012;

	if (snprintf(Ctx->FwBinFile, sizeof(Ctx->FwBinFile), "%s%s",
		fwdir, fwfile) >= (int)sizeof(Ctx->FwBinFile)) {
		DebugLog_Trace(LDIL_DATA,"DtsGetFirmwareFiles:Path is too large ....");
		return BC_STS_ERROR;
	}
	snprintf(Ctx->DilPath, sizeof(Ctx->DilPath), "%s", fwdir);

	return BC_STS_SUCCESS;

}

//------------------------------------------------------------------------
// Name: DtsAllocMdata
// Description: Get Mdata from free pool.
//------------------------------------------------------------------------
DTS_INPUT_MDATA	*DtsAllocMdata(DTS_LIB_CONTEXT *Ctx)
{
	DTS_INPUT_MDATA *temp = NULL;

	if(!Ctx)
		return temp;

	DtsLock(Ctx);
	if((temp=Ctx->MDFreeHead) != NULL)
	{
		Ctx->MDFreeHead = Ctx->MDFreeHead->flink;
		memset(temp, 0, sizeof(*temp));
	}
	else
	{
		//Use the Last Un-Fetched One
		DTS_INPUT_MDATA *last = NULL;
		last = Ctx->MDPendHead;

		//Check the Last Fetch Tag
		if((last) && (Ctx->MDLastFetchTag > (last->IntTag + MAX_DISORDER_GAP)))
		{
			//Remove
			DtsRemoveMdata(Ctx, last, FALSE);

			if((temp = Ctx->MDFreeHead) != NULL)
			{
				Ctx->MDFreeHead = Ctx->MDFreeHead->flink;
				memset(temp, 0, sizeof(*temp));
			}
		}
	}
	DtsUnLock(Ctx);

	return temp;
}
//------------------------------------------------------------------------
// Name: DtsFreeMdata
// Description: Free Mdata from to pool.
//------------------------------------------------------------------------
BC_STATUS DtsFreeMdata(DTS_LIB_CONTEXT *Ctx, DTS_INPUT_MDATA	*Mdata, BOOL sync)
{
	if(!Ctx || !Mdata){
		return BC_STS_INV_ARG;
	}
	if(sync)
		DtsLock(Ctx);
	Mdata->flink = Ctx->MDFreeHead;
	Ctx->MDFreeHead = Mdata;
	if(sync)
		DtsUnLock(Ctx);

	return BC_STS_SUCCESS;
}
//------------------------------------------------------------------------
// Name: DtsClrPendMdataList
// Description: Release all pending list back to free pool.
//------------------------------------------------------------------------
BC_STATUS DtsClrPendMdataList(DTS_LIB_CONTEXT *Ctx)
{
	DTS_INPUT_MDATA		*temp=NULL;
	int mdata_count = 0;

	if(!Ctx || !Ctx->MdataPoolPtr){
		return BC_STS_INV_ARG;
	}

	DtsLock(Ctx);
	/* Remove all Pending elements */
	temp = Ctx->MDPendHead;

	while(temp && temp != DTS_MDATA_PEND_LINK(Ctx)){
		//DebugLog_Trace(LDIL_DBG,"Clearing PendMdata %p %x \n", temp->Spes.SeqNum, temp->IntTag);
		DtsRemoveMdata(Ctx,temp,FALSE);
		temp = Ctx->MDPendHead;
		mdata_count++;
	}

	if (mdata_count)
		DebugLog_Trace(LDIL_DBG,"Clearing %d PendMdata entries \n", mdata_count);

	DtsUnLock(Ctx);

	return BC_STS_SUCCESS;
}
//------------------------------------------------------------------------
// Name: DtsPendMdataGarbageCollect
// Description: Garbage Collect Meta Data. This funtion is only called
//              internel when we run out of Mdata.
//------------------------------------------------------------------------
BC_STATUS DtsPendMdataGarbageCollect(DTS_LIB_CONTEXT *Ctx)
{
	DTS_INPUT_MDATA		*temp=NULL;
	int mdata_count = 0;

	if(!Ctx || !Ctx->MdataPoolPtr){
		return BC_STS_INV_ARG;
	}

	DtsLock(Ctx);

	/* Collect garbage it */
	temp = Ctx->MDPendHead;
	while(temp && temp != DTS_MDATA_PEND_LINK(Ctx)){
		//DebugLog_Trace(LDIL_DBG,"Clearing PendMdata %p %x \n", temp->Spes.SeqNum, temp->IntTag);
		if(mdata_count > (BC_INPUT_MDATA_POOL_SZ - BC_INPUT_MDATA_POOL_SZ_COLLECT)) {
			break;
		}
		DtsRemoveMdata(Ctx,temp,FALSE);
		temp = Ctx->MDPendHead;
		mdata_count++;
	}

	if (mdata_count)
		DebugLog_Trace(LDIL_DBG,"Clearing %d PendMdata entries \n", mdata_count);

	DtsUnLock(Ctx);

	return BC_STS_SUCCESS;
}
//------------------------------------------------------------------------
// Name: DtsInsertMdata
// Description: Insert Meta Data into list.
//------------------------------------------------------------------------
BC_STATUS DtsInsertMdata(DTS_LIB_CONTEXT *Ctx, DTS_INPUT_MDATA	*Mdata)
{
	if(!Ctx || !Mdata){
		return BC_STS_INV_ARG;
	}
	DtsLock(Ctx);
	Mdata->flink = DTS_MDATA_PEND_LINK(Ctx);
	Mdata->blink = Ctx->MDPendTail;
	Mdata->flink->blink = Mdata;
	Mdata->blink->flink = Mdata;
	DtsUnLock(Ctx);

	return BC_STS_SUCCESS;

}
//------------------------------------------------------------------------
// Name: DtsRemoveMdata
// Description: Remove Meta Data From List.
//------------------------------------------------------------------------
BC_STATUS DtsRemoveMdata(DTS_LIB_CONTEXT *Ctx, DTS_INPUT_MDATA	*Mdata, BOOL sync)
{
	if(!Ctx || !Mdata){
		return BC_STS_INV_ARG;
	}

	if(sync)
		DtsLock(Ctx);
	if(Ctx->MDPendHead != DTS_MDATA_PEND_LINK(Ctx))
	{
		Mdata->flink->blink = Mdata->blink;
		Mdata->blink->flink = Mdata->flink;
	}
	if(sync)
		DtsUnLock(Ctx);

	return DtsFreeMdata(Ctx,Mdata,sync);
}

//------------------------------------------------------------------------
// Name: DtsFetchMdata
// Description: Get Input Meta Data.
//
// FIX_ME:: Fill the pout part after FW upgrade with SeqNum feature..
//------------------------------------------------------------------------
BC_STATUS DtsFetchMdata(DTS_LIB_CONTEXT *Ctx, uint16_t snum, BC_DTS_PROC_OUT *pout)
{
	uint32_t		InTag;
	DTS_INPUT_MDATA		*temp=NULL;
	BC_STATUS	sts = BC_STS_NO_DATA;
	int16_t tsnum = 0;
	uint32_t i = 0;

	if(!Ctx || !pout){
		return BC_STS_INV_ARG;
	}

	if(!snum){
		// Zero is not a valid SeqNum.
		pout->PicInfo.timeStamp = 0;
		DebugLog_Trace(LDIL_DBG,"ZeroSnum \n");
		return BC_STS_NO_DATA;
	}

	InTag = DtsMdataGetIntTag(Ctx,snum);
	temp = Ctx->MDPendHead;

	DtsLock(Ctx);
	while(temp && temp != DTS_MDATA_PEND_LINK(Ctx)){
		if(temp->IntTag == InTag){
			pout->PicInfo.timeStamp = temp->appTimeStamp;
			sts = BC_STS_SUCCESS;
			DtsRemoveMdata(Ctx, temp, FALSE);

			//Reserve the Last Fetch Tag
			Ctx->MDLastFetchTag = InTag;
			break;
		}
		temp = temp->flink;
	}
	DtsUnLock(Ctx);
	// If we found a tag, clear out all the old entries - from (tag - 10) to (tag-110)
	// This is to work around the issue of lost pictures for which tags will never get freed
	if(sts == BC_STS_SUCCESS) {
		for(i = 0; i < 100; i++) {
			tsnum = snum - (10 + i);
			if(tsnum < 0)
				break;
			InTag = DtsMdataGetIntTag(Ctx, tsnum);
			temp = Ctx->MDPendHead;
			DtsLock(Ctx);
			while(temp && temp != DTS_MDATA_PEND_LINK(Ctx)){
				if(temp->IntTag == InTag){
					DtsRemoveMdata(Ctx, temp, FALSE);
					break;
				}
				temp = temp->flink;
			}
			DtsUnLock(Ctx);
		}
	}

	return sts;
}

//------------------------------------------------------------------------
// Name: DtsFetchTimeStampMdata
// Description: Get the Timestamp from the Meta Data field with the specifc picture number.
//				Do not change the mdata list in any way.
//
//------------------------------------------------------------------------
BC_STATUS DtsFetchTimeStampMdata(DTS_LIB_CONTEXT *Ctx, uint16_t snum, uint64_t *TimeStamp)
{
	uint32_t InTag;
	DTS_INPUT_MDATA *temp=NULL;
	BC_STATUS	sts = BC_STS_NO_DATA;

	if(!Ctx) {
		return BC_STS_INV_ARG;
	}

	if(!snum) {
		/* Zero is not a valid SeqNum. */
		*TimeStamp = 0;
		return BC_STS_NO_DATA;
	}

	InTag = DtsMdataGetIntTag(Ctx, snum);
	temp = Ctx->MDPendHead;

	DtsLock(Ctx);
	while(temp && temp != DTS_MDATA_PEND_LINK(Ctx)) {
		if(temp->IntTag == InTag) {

			*TimeStamp = temp->appTimeStamp;
			/* DebugLog_Trace(LDIL_DBG, "Found entry for %x %x tstamp: %x\n", */
			/*	snum, temp->IntTag, pout->PicInfo.timeStamp); */
			sts = BC_STS_SUCCESS;
			break;
		}
		temp = temp->flink;
	}
	DtsUnLock(Ctx);

	return sts;
}

//------------------------------------------------------------------------
// Name: DtsPrepareMdata
// Description: Insert Meta Data..
//------------------------------------------------------------------------
BC_STATUS DtsFillMdata(DTS_LIB_CONTEXT *Ctx, DTS_INPUT_MDATA *temp,
	uint64_t timeStamp, uint8_t **ppData, uint32_t *pSize)
{
	if (!Ctx || !temp || !ppData || !pSize)
		return BC_STS_INV_ARG;

	/* Store all app data.  DtsAllocMdata already cleared this owner. */
	DtsMdataSetIntTag(Ctx,temp);
	temp->appTimeStamp = timeStamp;

	/* Fill SPES data. */
	temp->Spes.StartCode[0] = 0;
	temp->Spes.StartCode[1] = 0;
	temp->Spes.StartCode[2] = 01;
	temp->Spes.StartCode[3] = 0xBD;
	temp->Spes.PacketLen = 0x07;
	temp->Spes.StartCodeEnd = 0x40;
	temp->Spes.Command = 0x0A;

	*ppData = (uint8_t*)(&temp->Spes);
	*pSize = sizeof(temp->Spes);
	return BC_STS_SUCCESS;
}

BC_STATUS DtsPrepareMdata(DTS_LIB_CONTEXT *Ctx, uint64_t timeStamp, DTS_INPUT_MDATA **mData, uint8_t** ppData, uint32_t *pSize)
{
	DTS_INPUT_MDATA		*temp = NULL;
	BC_STATUS		ret;

	if( !mData || !Ctx)
		return BC_STS_INV_ARG;

	/* Alloc clears all fields */
	if( (temp = DtsAllocMdata(Ctx)) == NULL)
	{
		DebugLog_Trace(LDIL_DBG,"COULD not find free MDATA try again\n");
		ret = DtsPendMdataGarbageCollect(Ctx);
		if(ret != BC_STS_SUCCESS) {
			return BC_STS_BUSY;
		}
		if( (temp = DtsAllocMdata(Ctx)) == NULL) {
			DebugLog_Trace(LDIL_DBG,"COULD not find free MDATA finaly failed\n");
			return BC_STS_BUSY;
		}
	}
	*mData = temp;
	return DtsFillMdata(Ctx, temp, timeStamp, ppData, pSize);
}


//------------------------------------------------------------------------
// Name: DtsPrepareMdataASFHdr
// Description: Insert Meta Data..
//------------------------------------------------------------------------
BC_STATUS DtsPrepareMdataASFHdr(DTS_LIB_CONTEXT *Ctx, DTS_INPUT_MDATA *mData, uint8_t* buf)
{
	if(buf==NULL)
		return BC_STS_INSUFF_RES;

	buf[0]=0;
	buf[1] = 0;
	buf[2] = 01;
	buf[3] = 0xE0;
	buf[4] = 0x0;
	buf[5]=35;
	buf[6] =0x80;
	buf[7]=0;
	buf[8]= 0;
	buf[9]=0x5a;buf[10]=0x5a;buf[11]=0x5a;buf[12]=0x5a;
	buf[13]=0x0; buf[14]=0x0;buf[15]=0x0;buf[16]=0x20;
	buf[17]=0x0; buf[18]=0x0;buf[19]=0x0;buf[20]=0x9;
	buf[21]=0x5a; buf[22]=0x5a;buf[23]=0x5a;buf[24]=0x5a;
	buf[25]=0xBD;
	buf[26]=0x40;
	buf[27]=mData->Spes.SeqNum[0];buf[28]=mData->Spes.SeqNum[1];
	buf[29]=mData->Spes.Command;
	buf[30]=buf[31]=buf[32]=buf[33]=buf[34]=buf[35]=buf[36]=buf[37]=buf[38]=buf[39]=buf[40]=0x0;
	return BC_STS_SUCCESS;
}

void DtsUpdateInStats(DTS_LIB_CONTEXT	*Ctx, uint32_t	size)
{
	BC_DTS_STATS *pDtsStat = DtsGetgStats( );

	pDtsStat->ipSampleCnt++;
	pDtsStat->ipTotalSize += size;
	pDtsStat->TxFifoBsyCnt = 0;

// 	Ctx->InSampleCount ++;
// 	if (Ctx->InSampleCount > 65530)
// 		Ctx->InSampleCount = 1;

	return;
}

void DtsUpdateOutStats(DTS_LIB_CONTEXT	*Ctx, BC_DTS_PROC_OUT *pOut)
{
	uint32_t fr23_976 = 0;
	BOOL	rptFrmCheck = TRUE;

	BC_DTS_STATS *pDtsStat = DtsGetgStats( );
	DtsLock(Ctx);
	Ctx->outputProgress++;
	DtsUnLock(Ctx);

	if(pOut->PicInfo.flags & VDEC_FLAG_LAST_PICTURE)
	{
		pDtsStat->eosDetected = 1;
	}

	if(!Ctx->CapState){
		/* Capture did not started yet..*/
		memset(pDtsStat,0,sizeof(*pDtsStat));
		if(pOut->PoutFlags & BC_POUT_FLAGS_FMT_CHANGE){

			/* Get the frame rate from vdecResolution enums */
			switch(pOut->PicInfo.frame_rate)
			{
			case vdecRESOLUTION_1080p23_976:
				fr23_976 = 1;
				break;
			case vdecRESOLUTION_1080i29_97:
				fr23_976 = 0;
				break;
			case vdecRESOLUTION_1080i25:
				fr23_976 = 0;
				break;
			//For Zero Frame Rate
			case vdecRESOLUTION_480p0:
			case vdecRESOLUTION_480i0:
			case vdecRESOLUTION_576p0:
			case vdecRESOLUTION_720p0:
			case vdecRESOLUTION_1080i0:
			case vdecRESOLUTION_1080p0:
				fr23_976 = 0;
				break;
			case vdecRESOLUTION_1080i:
				fr23_976 = 0;
				break;
			case vdecRESOLUTION_720p59_94:
				fr23_976 = 0;
				break;
			case vdecRESOLUTION_720p50:
				fr23_976 = 0;
				break;
			case vdecRESOLUTION_720p:
				fr23_976 = 0;
				break;
			case vdecRESOLUTION_720p23_976:
				fr23_976 = 1;
				break;
			case vdecRESOLUTION_CUSTOM ://no change in frame rate
				fr23_976 = Ctx->prevFrameRate;
				break;
			default:
				fr23_976 = 1;
				break;
			}

			/* Get the present frame rate */
			Ctx->prevFrameRate = fr23_976;

			if( ((pOut->PicInfo.flags & VDEC_FLAG_FIELDPAIR) ||
				(pOut->PicInfo.flags & VDEC_FLAG_INTERLACED_SRC)) && (!fr23_976)){
				/* Interlaced.*/
				Ctx->CapState = 1;
			}else{
				/* Progressive */
				Ctx->CapState = 2;
			}
		}
		return;
	}

	/* Done sizes describe the hardware transfer even when MODE converts to
	 * a different application format. Test the source layout, not the target. */
	if((!pOut->UVBuffDoneSz && !Ctx->b422Mode) || (!pOut->YBuffDoneSz)) {
		pDtsStat->opFrameDropped++;
	}else{
		pDtsStat->opFrameCaptured++;
	}

	if(pOut->discCnt){
		pDtsStat->opFrameDropped += pOut->discCnt;
	}

#ifdef _ENABLE_CODE_INSTRUMENTATION_

	/*
	 * For code instrumentation to be enabled
	 * we have to have unencripted PIBs.
	 */

	//if(!(Ctx->RegCfg.DbgOptions & BC_BIT(6))){
	//	/* New Scheme PIB is NOT available to DIL */
	//	return;
	//}
#else
	if(!(Ctx->RegCfg.DbgOptions & BC_BIT(6))){
		/* New Scheme PIB is NOT available to DIL */
		return;
	}
#endif

	/* PIB is not valid.. */
	if(!(pOut->PoutFlags & BC_POUT_FLAGS_PIB_VALID))
	{
		pDtsStat->pibMisses++;
		return;
	}

  	/* detect Even Odd field consistancy for interlaced..*/
	if(Ctx->CapState == 1){
		if( !Ctx->PibIntToggle && !(pOut->PoutFlags & BC_POUT_FLAGS_FLD_BOT))	{
			// Previous ODD & Current Even --- OK
			Ctx->PibIntToggle = 1;
		} else if (Ctx->PibIntToggle && (pOut->PoutFlags & BC_POUT_FLAGS_FLD_BOT)){
			//Previous Even and Current ODD --- OK
			rptFrmCheck = FALSE;
			Ctx->PibIntToggle = 0;
		} else if(!Ctx->PibIntToggle && (pOut->PoutFlags & BC_POUT_FLAGS_FLD_BOT)) {
            // Successive ODD
			if(!pOut->discCnt){
				if(Ctx->prevPicNum == pOut->PicInfo.picture_number)	{
					DebugLog_Trace(LDIL_DBG,"Succesive Odd=%d\n", pOut->PicInfo.picture_number);
					pDtsStat->reptdFrames++;
					rptFrmCheck = FALSE;
				}
			}
		} else if(Ctx->PibIntToggle && !(pOut->PoutFlags & BC_POUT_FLAGS_FLD_BOT)) {
			//Successive EVEN..
			 if(!pOut->discCnt){
				if(Ctx->prevPicNum == pOut->PicInfo.picture_number)	{
					DebugLog_Trace(LDIL_DBG,"Succesive Even=%d\n", pOut->PicInfo.picture_number);
					pDtsStat->reptdFrames++;
					rptFrmCheck = FALSE;
				}

			}
		}
	}

	if(!rptFrmCheck){
		Ctx->prevPicNum = pOut->PicInfo.picture_number;
		return;
	}

	if(Ctx->prevPicNum == pOut->PicInfo.picture_number) {
		/* Picture Number repetetion..*/
		DebugLog_Trace(LDIL_DBG,"Repetition=%d\n", pOut->PicInfo.picture_number);
		pDtsStat->reptdFrames++;
	}

	if(((Ctx->prevPicNum +1) != pOut->PicInfo.picture_number)&& !(pOut->discCnt) ){
			/* Discontguous Picture Numbers Frames/PIBs got dropped..*/
			pDtsStat->discCounter = pOut->PicInfo.picture_number - Ctx->prevPicNum ;
	}

	Ctx->prevPicNum = pOut->PicInfo.picture_number;

	return;
}

/********************************************************************************/
/* TX Circular Buffer routines */
// Init the circular buffer to be on 128 byte boundary
BC_STATUS txBufInit(pTXBUFFER txBuf, uint32_t sizeInit)
{
	int ret = 0;
	pthread_condattr_t wakeAttr;
	if(txBuf->buffer != NULL)
		return BC_STS_INV_ARG;
	ret = posix_memalign((void**)&txBuf->buffer, 128, sizeInit);
	if(ret)
		return BC_STS_INSUFF_RES;
	if(txBuf->buffer == NULL)
		return BC_STS_INSUFF_RES;

	txBuf->basePointer = txBuf->buffer;
	txBuf->endPointer = txBuf->basePointer + sizeInit - 1;
	txBuf->readPointer = txBuf->writePointer = 0;
	txBuf->freeSize = txBuf->totalSize = sizeInit;
	txBuf->busySize = 0;
	txBuf->wakePending = false;
	txBuf->wakeShutdown = false;

	ret = pthread_mutex_init(&txBuf->flushLock, NULL);
	if(ret)
		goto free_buffer;
	ret = pthread_mutex_init(&txBuf->pushpopLock, NULL);
	if(ret)
		goto destroy_flush_lock;
	ret = pthread_mutex_init(&txBuf->wakeLock, NULL);
	if(ret)
		goto destroy_pushpop_lock;
	ret = pthread_condattr_init(&wakeAttr);
	if(ret)
		goto destroy_wake_lock;
	ret = pthread_condattr_setclock(&wakeAttr, CLOCK_MONOTONIC);
	if(!ret)
		ret = pthread_cond_init(&txBuf->wakeCond, &wakeAttr);
	pthread_condattr_destroy(&wakeAttr);
	if(ret)
		goto destroy_wake_lock;

	return BC_STS_SUCCESS;

destroy_wake_lock:
	pthread_mutex_destroy(&txBuf->wakeLock);
destroy_pushpop_lock:
	pthread_mutex_destroy(&txBuf->pushpopLock);
destroy_flush_lock:
	pthread_mutex_destroy(&txBuf->flushLock);
free_buffer:
	free(txBuf->buffer);
	txBuf->buffer = NULL;
	txBuf->basePointer = NULL;
	txBuf->endPointer = NULL;
	txBuf->freeSize = txBuf->totalSize = txBuf->busySize = 0;
	return BC_STS_INSUFF_RES;
}

void txBufWake(pTXBUFFER txBuf)
{
	if(txBuf == NULL || txBuf->buffer == NULL)
		return;
	pthread_mutex_lock(&txBuf->wakeLock);
	txBuf->wakePending = true;
	pthread_cond_signal(&txBuf->wakeCond);
	pthread_mutex_unlock(&txBuf->wakeLock);
}

bool txBufWaitForWake(pTXBUFFER txBuf, uint32_t timeoutMs)
{
	struct timespec deadline;
	int ret;

	if(txBuf == NULL || txBuf->buffer == NULL)
		return false;
	if(clock_gettime(CLOCK_MONOTONIC, &deadline) != 0)
		return true;
	deadline.tv_sec += timeoutMs / 1000;
	deadline.tv_nsec += (long)(timeoutMs % 1000) * 1000000L;
	if(deadline.tv_nsec >= 1000000000L) {
		deadline.tv_sec++;
		deadline.tv_nsec -= 1000000000L;
	}

	pthread_mutex_lock(&txBuf->wakeLock);
	while(!txBuf->wakePending && !txBuf->wakeShutdown) {
		ret = pthread_cond_timedwait(&txBuf->wakeCond, &txBuf->wakeLock,
			&deadline);
		if(ret != 0)
			break;
	}
	txBuf->wakePending = false;
	const bool running = !txBuf->wakeShutdown;
	pthread_mutex_unlock(&txBuf->wakeLock);
	return running;
}

void txBufShutdown(pTXBUFFER txBuf)
{
	if(txBuf == NULL || txBuf->buffer == NULL)
		return;
	pthread_mutex_lock(&txBuf->wakeLock);
	txBuf->wakeShutdown = true;
	pthread_cond_broadcast(&txBuf->wakeCond);
	pthread_mutex_unlock(&txBuf->wakeLock);
}

// Push the number of bytes specified on to the circular buffer
// This routine copies the data so that the orginial buffer can be released

// Assume here that Flush of this buffer always happens from the same thread that does procinput
// So don't lock pushing of new data against flush

BC_STATUS txBufPush(pTXBUFFER txBuf, uint8_t* bufToPush, uint32_t sizeToPush)
{
	uint32_t mcpySz = 0, sizeTop = 0;
	uint8_t* bufRemain = bufToPush;

	if(txBuf == NULL || bufToPush == NULL)
		return BC_STS_INV_ARG;

	if(txBuf->freeSize < sizeToPush)
		return BC_STS_INSUFF_RES;

	// How much will fit before we need to wrap
	sizeTop = (uint32_t)(txBuf->endPointer - (txBuf->basePointer + txBuf->writePointer) + 1);
	if(sizeToPush <= sizeTop)
		mcpySz = sizeToPush;
	else
		mcpySz = sizeTop;

	memcpy(txBuf->basePointer + txBuf->writePointer, bufToPush, mcpySz);

	txBuf->writePointer = (txBuf->writePointer + mcpySz) % txBuf->totalSize;

	pthread_mutex_lock(&txBuf->pushpopLock);
	txBuf->busySize += mcpySz;
	txBuf->freeSize -= mcpySz;
	pthread_mutex_unlock(&txBuf->pushpopLock);

	if((sizeToPush - mcpySz) != 0)
	{
		// Can only get here if we wrap at the top
		// writePointer should be 0
		bufRemain += mcpySz;
		mcpySz = sizeToPush - mcpySz;
		sizeTop = txBuf->readPointer;
		memcpy(txBuf->basePointer, bufRemain, mcpySz);
		txBuf->writePointer = mcpySz;
		pthread_mutex_lock(&txBuf->pushpopLock);
		txBuf->busySize += mcpySz;
		txBuf->freeSize -= mcpySz;
		pthread_mutex_unlock(&txBuf->pushpopLock);
	}

	txBufWake(txBuf);
	return BC_STS_SUCCESS;
}

// Pops data from the circular buffer
// Returns the number of bytes popped
// We have to do a copy since we require the HW buffer to be 128 byte aligned for Flea
BC_STATUS txBufPop(pTXBUFFER txBuf, uint8_t* bufToPop, uint32_t sizeToPop)
{
	uint32_t popSz = 0, sizeTop = 0;
	uint8_t* bufRemain = bufToPop;

	if(txBuf == NULL)
		return BC_STS_INV_ARG;

	pthread_mutex_lock(&txBuf->flushLock);

	if(sizeToPop > txBuf->busySize) {
		// A flush can invalidate the size sampled by the TX worker.
		pthread_mutex_unlock(&txBuf->flushLock);
		return BC_STS_INV_ARG;
	}

	sizeTop = (uint32_t)(txBuf->endPointer - (txBuf->basePointer + txBuf->readPointer) + 1);
	if(sizeToPop <= sizeTop)
		popSz = sizeToPop;
	else
		popSz = sizeTop;

	memcpy(bufToPop, txBuf->basePointer + txBuf->readPointer, popSz);

	txBuf->readPointer = (txBuf->readPointer + popSz) % txBuf->totalSize;

	pthread_mutex_lock(&txBuf->pushpopLock);
	txBuf->busySize -= popSz;
	txBuf->freeSize += popSz;
	pthread_mutex_unlock(&txBuf->pushpopLock);

	if((sizeToPop - popSz) != 0)
	{
		// Can only get here if we wrap at the top
		// readPointer should be 0
		bufRemain += popSz;
		popSz = sizeToPop - popSz;
		sizeTop = txBuf->writePointer;
		memcpy(bufRemain, txBuf->basePointer, popSz);
		txBuf->readPointer = popSz;
		pthread_mutex_lock(&txBuf->pushpopLock);
		txBuf->busySize -= popSz;
		txBuf->freeSize += popSz;
		pthread_mutex_unlock(&txBuf->pushpopLock);
	}

	pthread_mutex_unlock(&txBuf->flushLock);
	return BC_STS_SUCCESS;
}

// Assume here that Flush of this buffer always happens from the same thread that does procinput
// So don't lock pushing of new data against flush
BC_STATUS txBufFlush(pTXBUFFER txBuf)
{
	if(txBuf->buffer == NULL)
		return BC_STS_INV_ARG;
	pthread_mutex_lock(&txBuf->flushLock);
	txBuf->readPointer = txBuf->writePointer = 0;
	txBuf->freeSize = txBuf->totalSize;
	txBuf->busySize = 0;
	pthread_mutex_unlock(&txBuf->flushLock);
	return BC_STS_SUCCESS;
}

BC_STATUS txBufFree(pTXBUFFER txBuf)
{
	if(txBuf->buffer == NULL)
		return BC_STS_INV_ARG;
	pthread_cond_destroy(&txBuf->wakeCond);
	pthread_mutex_destroy(&txBuf->wakeLock);
	txBuf->basePointer = NULL;
	txBuf->endPointer = NULL;
	txBuf->readPointer = txBuf->writePointer = 0;
	txBuf->freeSize = 0;
	txBuf->busySize = 0;
	free(txBuf->buffer);
	txBuf->buffer = NULL;
	pthread_mutex_destroy(&txBuf->flushLock);
	pthread_mutex_destroy(&txBuf->pushpopLock);
	return BC_STS_SUCCESS;
}

static uint64_t DtsMonotonicMs()
{
	struct timespec now;
	if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
		return 0;
	return (uint64_t)now.tv_sec * 1000 + (uint64_t)now.tv_nsec / 1000000;
}

static bool DtsFleaMpeg4EosReady(DTS_LIB_CONTEXT *Ctx,
	uint64_t *fence, uint64_t *generation, uint64_t *progress,
	uint32_t *outputBuffers)
{
	DtsLock(Ctx);
	const bool ready = Ctx->DevId == BC_PCI_DEVID_FLEA &&
		(Ctx->VidParams.MediaSubType == BC_MSUBTYPE_DIVX ||
		 Ctx->VidParams.MediaSubType == BC_MSUBTYPE_DIVX311) &&
		Ctx->State == BC_DEC_STATE_START && Ctx->bEOSCheck && !Ctx->bEOS &&
		Ctx->eosTxComplete && !Ctx->txDmaFault && !Ctx->txPending &&
		!Ctx->ProcOutPending && Ctx->outputPhase == DTS_OUTPUT_IDLE && !Ctx->CancelWaiting &&
		Ctx->txBytesRetired >= Ctx->eosTxFence &&
		DtsTxFreeSize((HANDLE)Ctx) == Ctx->circBuf.totalSize;
	*fence = Ctx->eosTxFence;
	*generation = Ctx->eosDrainGeneration;
	*progress = Ctx->outputProgress;
	*outputBuffers = Ctx->MpoolCnt;
	DtsUnLock(Ctx);
	return ready;
}

static bool DtsCompleteFleaMpeg4Eos(DTS_LIB_CONTEXT *Ctx,
	uint64_t fence, uint64_t generation, uint64_t progress)
{
	DtsLock(Ctx);
	const bool ready = Ctx->DevId == BC_PCI_DEVID_FLEA &&
		(Ctx->VidParams.MediaSubType == BC_MSUBTYPE_DIVX ||
		 Ctx->VidParams.MediaSubType == BC_MSUBTYPE_DIVX311) &&
		Ctx->State == BC_DEC_STATE_START && Ctx->bEOSCheck && !Ctx->bEOS &&
		Ctx->eosTxComplete && !Ctx->txDmaFault && !Ctx->txPending &&
		!Ctx->ProcOutPending && Ctx->outputPhase == DTS_OUTPUT_IDLE && !Ctx->CancelWaiting &&
		Ctx->eosTxFence == fence &&
		Ctx->eosDrainGeneration == generation &&
		Ctx->outputProgress == progress &&
		Ctx->txBytesRetired >= Ctx->eosTxFence &&
		DtsTxFreeSize((HANDLE)Ctx) == Ctx->circBuf.totalSize;
	if (ready)
		Ctx->bEOS = true;
	DtsUnLock(Ctx);
	return ready;
}

void DtsObservePowerState(DTS_LIB_CONTEXT *Ctx, uint8_t powerState)
{
	if (!Ctx)
		return;

	DtsLock(Ctx);
	/* SUSPEND is the earliest reliable userspace discontinuity signal. RESUME
	 * is also accepted as the first sample because processes may be frozen for
	 * the whole suspended interval. The TX worker alone executes recovery. */
	if (powerState == BC_HW_SUSPEND && !Ctx->recoverySuspendSeen) {
		Ctx->recoverySuspendSeen = true;
		Ctx->recoveryResumeSeen = false;
		if (Ctx->recoveryStatus == BC_STS_SUCCESS &&
		    Ctx->controlOwner != DTS_CONTROL_RECOVERY) {
			Ctx->recoveryPending = true;
			Ctx->recoveryGeneration++;
			Ctx->recoveryCancelGeneration = Ctx->recoveryGeneration;
		}
	} else if (powerState == BC_HW_RESUME && !Ctx->recoveryResumeSeen) {
		Ctx->recoveryResumeSeen = true;
		Ctx->recoverySuspendSeen = false;
		if (Ctx->recoveryStatus == BC_STS_SUCCESS &&
		    !Ctx->recoveryPending && Ctx->controlOwner != DTS_CONTROL_RECOVERY) {
			Ctx->recoveryPending = true;
			Ctx->recoveryGeneration++;
			Ctx->recoveryCancelGeneration = Ctx->recoveryGeneration;
		}
	} else if (powerState == BC_HW_RUNNING &&
		   Ctx->recoveryStatus == BC_STS_SUCCESS &&
		   !Ctx->recoveryPending && Ctx->controlOwner != DTS_CONTROL_RECOVERY) {
		/* A confirmed running interval separates two RESUME-only epochs. */
		Ctx->recoveryResumeSeen = false;
		Ctx->recoverySuspendSeen = false;
	}
	DtsUnLock(Ctx);
}

class DtsRecoveryCall {
	DTS_LIB_CONTEXT *context;
	BC_STATUS status;
	bool active;
	bool pending;
public:
	DtsRecoveryCall(DTS_LIB_CONTEXT *ctx, bool resumeObserved) : context(ctx),
		status(BC_STS_SUCCESS), active(false), pending(false) {
		/* Latch and own the very first RESUME observation in one thLock
		 * critical section. This leaves no window for a public lifecycle
		 * call to enter between detection and recovery ownership.
		 */
		DtsLock(context);
		if (context->recoveryStatus != BC_STS_SUCCESS) {
			/* A completed failed epoch is terminal quarantine.  A PM status
			 * observed by its own failing stage may have re-latched pending;
			 * consume it without ever replaying Setup/Open/START. */
			context->recoveryPending = false;
			status = context->recoveryStatus;
			DtsUnLock(context);
			return;
		}
		if (resumeObserved && !context->recoveryResumeSeen) {
			context->recoveryResumeSeen = true;
			context->recoverySuspendSeen = false;
			/* Some drivers can expose RESUME before userspace samples SUSPEND. */
			if (context->recoveryStatus == BC_STS_SUCCESS &&
			    !context->recoveryPending) {
				context->recoveryPending = true;
				context->recoveryGeneration++;
				context->recoveryCancelGeneration =
					context->recoveryGeneration;
			}
		} else if (!resumeObserved && context->recoveryStatus == BC_STS_SUCCESS &&
			   !context->recoveryPending &&
			   context->controlOwner != DTS_CONTROL_RECOVERY) {
			context->recoveryResumeSeen = false;
			context->recoverySuspendSeen = false;
		}
		/* SUSPEND closes admission immediately, but reconstruction must wait
		 * for a confirmed RESUME sample. A transient RUNNING sample while the
		 * machine is going down is not permission to touch firmware. */
		pending = context->recoveryPending &&
			(resumeObserved || context->recoveryResumeSeen);
		if (pending) {
			if (context->controlOwner == DTS_CONTROL_NONE &&
			    DtsStopIsUnresolved(context)) {
				/* A suspend-aborted STOP cannot safely be replayed and cannot
				 * coexist with channel reconstruction. Consume this epoch into
				 * fail-closed quarantine once its old owner has retired. */
				status = context->stopStatus != BC_STS_SUCCESS ?
					context->stopStatus : BC_STS_IO_ERROR;
				context->recoveryPending = false;
				context->recoveryStatus = status;
			} else {
				status = DtsAcquireControlLocked(context, DTS_CONTROL_RECOVERY);
				active = status == BC_STS_SUCCESS;
			}
			/* Owner contention is retryable. Once an attempt owns control,
			 * consume this RESUME observation before issuing any command so
			 * a stage failure cannot blindly replay START on the next poll.
			 */
			if (active)
				context->recoveryPending = false;
		}
		DtsUnLock(context);
	}
	~DtsRecoveryCall() {
		if (active)
			DtsReleaseControl(context);
	}
	BC_STATUS Status() const { return status; }
	bool Pending() const { return pending; }
};

BC_STATUS DtsWaitForInputCalls(DTS_LIB_CONTEXT *Ctx)
{
	for (unsigned int polls = 0; ; ++polls) {
		DtsLock(Ctx);
		const bool active = Ctx->inputCalls != 0;
		DtsUnLock(Ctx);
		if (!active)
			return BC_STS_SUCCESS;
		if (polls == 500)
			return BC_STS_TIMEOUT;
		bc_sleep_ms(10);
	}
}

static BC_STATUS DtsRestoreAfterResume(DTS_LIB_CONTEXT *Ctx)
{
	HANDLE hDevice = (HANDLE)Ctx;
	BC_STATUS sts;
	DtsLock(Ctx);
	const uint32_t priorState = Ctx->State;
	const bool activeSession = priorState == BC_DEC_STATE_START ||
		priorState == BC_DEC_STATE_PAUSE;
	if (activeSession)
		Ctx->recoveryRetireDecoderOwner = true;
	DtsUnLock(Ctx);
	/* A lifecycle call admitted before the PM latch may have intentionally
	 * stopped/closed the session while recovery waited for its control lease.
	 * Setup was still lost across S3, so silently consuming the epoch would
	 * allow a later Open/Start against uninitialized hardware. Require terminal
	 * device retirement instead of resurrecting an intentionally stopped one. */
	if (!activeSession)
		return BC_STS_PWR_MGMT;

	/* The outer recovery lease has already closed new input/output/control
	 * admission. Let calls admitted before the RESUME observation retire
	 * without retaining thLock across either wait.
	 */
	DtsLock(Ctx);
	Ctx->recoveryCancelGeneration = Ctx->recoveryGeneration;
	DtsUnLock(Ctx);
	/* Stamp the transition before draining input. A FETCH already in the
	 * kernel can be aborted by suspend while input retirement is still in
	 * progress; its final no-buffer seam must already know this generation. */
	sts = DtsWaitForInputCalls(Ctx);
	if (sts != BC_STS_SUCCESS)
		return sts;
	sts = DtsCancelFetchOutInt(Ctx);
	if (sts != BC_STS_SUCCESS)
		return sts;

	DtsLock(Ctx);
	const bool resendEos = Ctx->bEOSCheck;
	DtsUnLock(Ctx);

	DtsSetCoreClock(hDevice, 180); /* Link's established recovery clock. */
	sts = DtsSetupHardware(hDevice, true);
	if (sts != BC_STS_SUCCESS)
		return sts;

	/* Suspend retains capture registrations, while a failed repost can leave
	 * a retirement-only userspace lease. INIT is the first driver state where
	 * destructive capture cleanup is admitted. Require that acknowledgement
	 * before rebuilding the channel; StartCapture will register a fresh set. */
	sts = DtsFlushRxCapture(hDevice, false);
	if (sts != BC_STS_SUCCESS)
		return sts;

	/* Firmware reset invalidated the old channel. Publish the logical close
	 * under the same lock used by lifecycle admission, then rebuild it while
	 * retaining (but not mutex-holding) the recovery lease.
	 */
	DtsLock(Ctx);
	Ctx->State = BC_DEC_STATE_CLOSE;
	DtsUnLock(Ctx);
	sts = DtsOpenDecoder(hDevice, 0);
	if (sts != BC_STS_SUCCESS)
		return sts;

	sts = DtsRecoveryFlushInputRing(Ctx);
	if (sts != BC_STS_SUCCESS)
		return sts;

	sts = DtsRecoveryRestoreInputState(Ctx);
	if (sts != BC_STS_SUCCESS)
		return sts;

	if (Ctx->DevId == BC_PCI_DEVID_FLEA)
		sts = DtsSetFleaIn422Mode(hDevice);
	else if (Ctx->DevId == BC_PCI_DEVID_LINK)
		sts = DtsSetLinkIn422Mode(hDevice);
	else
		sts = BC_STS_NOT_IMPL;
	if (sts != BC_STS_SUCCESS)
		return sts;

	sts = DtsStartDecoder(hDevice);
	if (sts != BC_STS_SUCCESS)
		return sts;
	sts = DtsStartCapture(hDevice);
	if (sts != BC_STS_SUCCESS)
		return sts;
	if (resendEos) {
		sts = DtsSendEOS(hDevice, 0);
		if (sts != BC_STS_SUCCESS)
			return sts;
	}
	if (priorState == BC_DEC_STATE_PAUSE) {
		sts = DtsPauseDecoder(hDevice);
		if (sts != BC_STS_SUCCESS)
			return sts;
	}
	DtsLock(Ctx);
	Ctx->recoveryRetireDecoderOwner = false;
	DtsUnLock(Ctx);
	return BC_STS_SUCCESS;
}

// TX Thread
// This thread has dual purpose. First is to send TX data. Second is to detect if we have restarted from any suspend/hibernate action
// and to restore the HW state
void * txThreadProc(void *ctx)
{
	DTS_LIB_CONTEXT* Ctx = (DTS_LIB_CONTEXT*)ctx;
	uint8_t *const localBuffer = Ctx->txScratch;
	uint32_t szDataToSend;
	BC_STATUS sts;
	uint32_t dramOff;
	uint8_t encrypted = 0;
	HANDLE hDevice = (HANDLE)Ctx;
	BC_DTS_STATUS pStat = {};
	uint32_t waitForPictCount = 0;
	uint32_t numPicCaptured = 0;
	uint64_t fleaIdleSince = 0;
	uint64_t fleaIdleFence = 0;
	uint64_t fleaIdleGeneration = 0;
	uint64_t fleaIdleProgress = 0;
	bool fleaIdleTiming = false;
	/* Input push is a latched condition wake, so normal idle status checks do
	 * not set submission latency. Keep EOS observation on its legacy cadence. */
	const uint32_t idleWaitMs = 50;

	for (;;)
	{
		DtsLock(Ctx);
		const bool exiting = Ctx->txThreadExit;
		DtsUnLock(Ctx);
		if (exiting)
			break;
		// First check the status of the HW
		// Get the real HW free size and also mark as we want TX information only
		pStat.cpbEmptySize = (0x3U << 30);

		sts = DtsGetDriverStatus(hDevice, &pStat);
		if(sts != BC_STS_SUCCESS)
		{
			pStat.cpbEmptySize = 0;
			fleaIdleTiming = false;
			DebugLog_Trace(LDIL_ERR,"txThreadProc: Got status %d from GetDriverStatus\n", sts);
			usleep(2 * 1000);
			continue;
		}

		//DebugLog_Trace(LDIL_ERR,"txThreadProc: Got hw size %u and data size %u\n", pStat.cpbEmptySize, Ctx->circBuf.busySize);

		if(pStat.PowerStateChange == BC_HW_SUSPEND)
		{
			DtsObservePowerState(Ctx, BC_HW_SUSPEND);
			// HW is in suspend mode, sleep 30 ms and then try again
			fleaIdleTiming = false;
			usleep(30 * 1000);
			continue;
		}

		{
			DtsRecoveryCall recovery(Ctx,
				pStat.PowerStateChange == BC_HW_RESUME);
			if (recovery.Pending()) {
				if (recovery.Status() != BC_STS_SUCCESS) {
					usleep(2 * 1000);
					continue;
				}
				DebugLog_Trace(LDIL_ERR,"Trying to resume from S3/S5\n");
				sts = DtsRestoreAfterResume(Ctx);
				DtsLock(Ctx);
				Ctx->recoveryStatus = sts;
				Ctx->recoveryPending = false;
				DtsUnLock(Ctx);
				if (sts != BC_STS_SUCCESS) {
					DebugLog_Trace(LDIL_ERR,
						"Cannot Recover from S3/S5 RESUME failed %d\n", sts);
					usleep(1000 * 1000);
					continue;
				}
				DebugLog_Trace(LDIL_ERR,"Resume from S3/S5 Done\n");
				/* The replacement channel has no relationship to the old
				 * firmware picture counter or EOS idle interval.  Establish a
				 * fresh baseline before observing the replayed drain. */
				waitForPictCount = 0;
				numPicCaptured = UINT32_MAX;
				fleaIdleTiming = false;
				/* The status sample predates reset/reopen; reacquire it before
				 * making any TX or EOS decision against the restored channel.
				 */
				continue;
			}
		}

		DtsLock(Ctx);
		const bool recoveryFailed = Ctx->recoveryStatus != BC_STS_SUCCESS;
		DtsUnLock(Ctx);
		if (recoveryFailed) {
			/* A failed attempt is one-shot. Leave the ring and firmware
			 * quarantined until terminal teardown consumes this handle. */
			fleaIdleTiming = false;
			usleep(30 * 1000);
			continue;
		}

		/* Retain BCM70012's legacy no-progress fallback. */
		DtsLock(Ctx);
		const bool linkEosCheck = Ctx->DevId == BC_PCI_DEVID_LINK &&
			Ctx->bEOSCheck;
		DtsUnLock(Ctx);
		if(linkEosCheck)
		{
			if(numPicCaptured == pStat.FramesCaptured)
				waitForPictCount++;

			if(waitForPictCount >= BC_EOS_PIC_COUNT) {
				DtsLock(Ctx);
				if (Ctx->bEOSCheck)
					Ctx->bEOS = true;
				DtsUnLock(Ctx);
			}

			usleep(30 * 1000);
		}
		else
			waitForPictCount = 0;

		/* BCM70015 firmware occasionally consumes a complete MPEG-4 drain
		 * sequence without returning its EOS buffer. Only recover after the
		 * fenced TX DMA completed and every output buffer is back on the free
		 * list for a continuous monotonic second. Other codecs still require
		 * their real firmware EOS marker.
		 */
		uint64_t currentFence = 0;
		uint64_t currentGeneration = 0;
		uint64_t currentProgress = 0;
		uint32_t outputBuffers = 0;
		const bool fleaReady = DtsFleaMpeg4EosReady(Ctx,
			&currentFence, &currentGeneration, &currentProgress, &outputBuffers);
		const bool fleaQueuesIdle = pStat.PowerStateChange == BC_HW_RUNNING &&
			outputBuffers != 0 && pStat.ReadyListCount == 0 &&
			pStat.FreeListCount == outputBuffers;
		if (fleaReady && fleaQueuesIdle)
		{
			const uint64_t now = DtsMonotonicMs();
			if (!now)
			{
				fleaIdleTiming = false;
			}
			else if (!fleaIdleTiming || currentFence != fleaIdleFence ||
				currentGeneration != fleaIdleGeneration ||
				currentProgress != fleaIdleProgress)
			{
				fleaIdleSince = now;
				fleaIdleFence = currentFence;
				fleaIdleGeneration = currentGeneration;
				fleaIdleProgress = currentProgress;
				fleaIdleTiming = true;
			}
			else if (now - fleaIdleSince >= BC_FLEA_EOS_IDLE_MS)
			{
				BC_DTS_STATUS confirm = {};
				confirm.cpbEmptySize = (0x3U << 30);
				const BC_STATUS confirmSts = DtsGetDriverStatus(hDevice, &confirm);
				const bool queuesStillIdle = confirmSts == BC_STS_SUCCESS &&
					confirm.PowerStateChange == BC_HW_RUNNING &&
					confirm.ReadyListCount == 0 &&
					confirm.FreeListCount == outputBuffers;
				if (queuesStillIdle && DtsCompleteFleaMpeg4Eos(Ctx,
					fleaIdleFence, fleaIdleGeneration, fleaIdleProgress))
				{
					DebugLog_Trace(LDIL_INFO,
						"FLEA MPEG-4 EOS marker missing; completed after idle fence\n");
				}
				fleaIdleTiming = false;
			}
			usleep(30 * 1000);
		}
		else
			fleaIdleTiming = false;

		if(numPicCaptured != pStat.FramesCaptured)
		{
			waitForPictCount = 0;
			numPicCaptured = pStat.FramesCaptured;
		}

		// Check if we have data to send.
		if(Ctx->circBuf.busySize != 0)
		{
			if(pStat.cpbEmptySize == 0)
			{
				usleep(3000);
				continue;
			}

			/* Own the complete pop-to-DMA interval before dropping thLock.
			 * Flush closes admission with this same mutex and then waits
			 * without it, so pooled ioctl/output completion remains live.
			 */
			DtsLock(Ctx);
			const bool admitted =
				DtsRecoveryAdmissionStatus(Ctx) == BC_STS_SUCCESS &&
				!Ctx->txQuiescing && !DtsStopIsUnresolved(Ctx) &&
				(Ctx->State == BC_DEC_STATE_START || Ctx->State == BC_DEC_STATE_PAUSE);
			if (admitted)
				Ctx->txPending = true;
			DtsUnLock(Ctx);
			if (!admitted) {
				usleep(5 * 1000);
				continue;
			}

			if(Ctx->circBuf.busySize < pStat.cpbEmptySize)
				szDataToSend = Ctx->circBuf.busySize;
			else
				szDataToSend = pStat.cpbEmptySize;
			if(BC_STS_SUCCESS != txBufPop(&Ctx->circBuf, localBuffer, szDataToSend)) {
				DtsLock(Ctx);
				Ctx->txPending = false;
				DtsUnLock(Ctx);
				usleep(5 * 1000);
				continue;
			}
			if(Ctx->VidParams.VideoAlgo == BC_VID_ALGO_VC1MP)
				encrypted |= 0x2;
			sts = DtsTxDmaText(hDevice, localBuffer, szDataToSend, &dramOff, encrypted);
			if(sts == BC_STS_SUCCESS)
				DtsUpdateInStats(Ctx, szDataToSend);
			else
			{
				// signal error to the next procinput
				DebugLog_Trace(LDIL_ERR,"txThreadProc: Got status %d from TxDmaText\n", sts);
			}
			DtsLock(Ctx);
			Ctx->txBytesRetired += szDataToSend;
			if(sts != BC_STS_SUCCESS)
			{
				Ctx->txDmaFault = true;
				Ctx->eosTxComplete = false;
			}
			else if(Ctx->bEOSCheck && !Ctx->txDmaFault &&
				Ctx->eosTxFence > Ctx->eosTxStart &&
				Ctx->txBytesRetired >= Ctx->eosTxFence)
			{
				Ctx->eosTxComplete = true;
			}
			Ctx->txPending = false;
			DtsUnLock(Ctx);
		} else {
			DtsLock(Ctx);
			const bool eosCheck = Ctx->bEOSCheck;
			DtsUnLock(Ctx);
			if(!txBufWaitForWake(&Ctx->circBuf,
				eosCheck ? 5 : idleWaitMs))
				break;
		}
	}

	return FALSE;
}

DRVIFLIB_INT_API BC_STATUS DtsGetHWFeatures(uint32_t *pciids)
{
	int drvHandle = -1;
	BC_IOCTL_DATA pIo;
	int rc;

	if (!pciids)
		return BC_STS_INV_ARG;

	memset(&pIo, 0, sizeof(BC_IOCTL_DATA));

	drvHandle = open(CRYSTALHD_API_DEV_NAME, O_RDWR);
	if(drvHandle < 0)
	{
		DebugLog_Trace(LDIL_ERR,"DtsGetHWFeatures: Create File Failed\n");
		return BC_STS_ERROR;
	}

	/* Device identification is available to playback users. Raw PCI config
	 * access is a privileged diagnostic interface in the maintained driver. */
	rc = ioctl(drvHandle, BCM_IOC_GET_HWTYPE, &pIo);
	if(rc < 0){
		DebugLog_Trace(LDIL_ERR,"ioctl to get HW features failed\n");
		close(drvHandle);
		return BC_STS_ERROR;
	}

	if(pIo.RetSts == BC_STS_SUCCESS) {
		*pciids = ((uint32_t)pIo.u.hwType.PciDevId << 16) |
			pIo.u.hwType.PciVenId;
		close(drvHandle);
		return BC_STS_SUCCESS;
	}
	else {
		DebugLog_Trace(LDIL_ERR, "error in getting pciids\n");
		close(drvHandle);
		return BC_STS_ERROR;
	}
}

/*====================== Debug Routines ========================================*/
//
// Test routine to verify mdata functions ..
//
void DtsTestMdata(DTS_LIB_CONTEXT	*gCtx)
{
	uint32_t					i;
	BC_STATUS			sts = BC_STS_SUCCESS;
	DTS_INPUT_MDATA		*im = NULL;
	uint8_t 					*temp;
	uint32_t					ulSize;


	//sts = DtsCreateMdataPool(gCtx);
	//if(sts != BC_STS_SUCCESS){
	//	DebugLog_Trace(LDIL_DBG,"PoolCreation Failed:%x\n",sts);
	//	return;
	//}
	//DebugLog_Trace(LDIL_DBG,"PoolCreation done\n");

	DebugLog_Trace(LDIL_DBG,"Inserting Elements for Sequential Fetch..\n");
	//gCtx->InMdataTag = 0x11;
	//for(i=0x11; i < 0x21; i++){
	//gCtx->InMdataTag = 0;
	for(i=0; i < 64; i++){
		sts = DtsPrepareMdata(gCtx,i, &im, &temp, &ulSize);
		if(sts != BC_STS_SUCCESS){
			DtsFreeMdata(gCtx,im,TRUE);
			DebugLog_Trace(LDIL_DBG,"DtsPrepareMdata Failed:%x\n",sts);
			return;
		}
		DtsInsertMdata(gCtx,im);
	}

//	DtsFetchMdata(gCtx,10,&gpout);
#if 1
	BC_DTS_PROC_OUT		gpout;

	DebugLog_Trace(LDIL_DBG,"Fetch Begin\n");
	//for(i=0x12; i < 0x22; i++){
	for(i=1; i < 64; i++){
		sts = DtsFetchMdata(gCtx,i,&gpout);
		if(sts != BC_STS_SUCCESS){
			DebugLog_Trace(LDIL_DBG,"DtsFetchMdata Failed:%x SNum:%x \n",sts,i);
			//return;
		}
	}
#endif
//	DebugLog_Trace(LDIL_DBG,"Deleting Pool...\n");
//	DtsDeleteMdataPool(gCtx);

}

BOOL DtsDbgCheckPointers(DTS_LIB_CONTEXT *Ctx,BC_IOCTL_DATA *pIo)
{
	uint32_t		i;
	BOOL	bRet = FALSE;
	DTS_MPOOL_TYPE	*mp;

	if(!Ctx->Mpools || !(Ctx->CfgFlags & BC_MPOOL_INCL_YUV_BUFFS)){
		return FALSE;
	}

	for(i=0; i<Ctx->MpoolCnt; i++){
		mp = &Ctx->Mpools[i];
		if(mp->type & BC_MEM_DEC_YUVBUFF){
			if(mp->buff == pIo->u.DecOutData.OutPutBuffs.YuvBuff){
				bRet = TRUE;
				break;
			}
		}
	}

	return bRet;

}
