/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Actual shared retention protocol, with numeric MMIO fault fixtures only. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint32_t u32;
typedef unsigned long long u64;
typedef unsigned int BC_STATUS;
#define READ_ONCE(v) (v)
#define BC_STS_SUCCESS 0U
#define BC_STS_IO_ERROR 7U
#define ListStsFree 0U
#define sts_free 0U
#define CHECK(e) do { assert(e); checks++; } while (0)
#include "pwm-retention-types.h"
#include "crystalhd_pm_arbiter.h"
#include "crystalhd_pm_retention.h"
#include "crystalhd_pm_shadow_ring.h"
#include "70015/magnum/basemodules/chp/70015/rdb/a0/bchp_pri_arb_control_regs.h"
#include "70015/magnum/basemodules/chp/70015/rdb/a0/bchp_ddr23_ctl_regs_0.h"

struct fixture {
	u32 present, master, params, refresh, baseline_memc;
	u32 read_count, write_count, commands, release_writes;
	u32 bad_read, bad_read2, bad_kind, busy_phase, busy_reads, invalid_phase;
	u32 busy_phase2, busy_reads2;
	u32 polls[14], delays[14], changed_unowned, faults;
};
struct crystalhd_hw {
	u64 FleaShadowSession;
	struct fixture *adp;
	int lock;
	u32 (*pfnReadDevRegister)(struct fixture *, u32);
	void (*pfnWriteDevRegister)(struct fixture *, u32, u32);
	u32 FleaPowerTransition, FleaPowerState, dma_fault, dev_started;
	u32 FleaTxActive, FleaIrqActive, FleaRawActive, fwcmd_trans_active;
	u32 fwcmd_active, FwCmdCnt, fwcmd_poisoned, TxList0Sts, TxList1Sts;
	u32 rx_list_sts[2], EmptyCnt, SingleThreadAppFIFOEmpty;
};
static unsigned int checks, reports, down_refusals;
static int shadow_lock, shadow_locked;
static bool arbiter_attempted;
static u64 now;
static struct fixture *current;
static struct chd_pm_retention_report *current_report, saved_report;
#define spin_lock_irqsave(lock, flags) do { \
	(void)(lock); (flags)=0; CHECK(!shadow_locked); shadow_locked=1; \
} while (0)
#define spin_unlock_irqrestore(lock, flags) do { \
	(void)(lock); (void)(flags); CHECK(shadow_locked); shadow_locked=0; \
} while (0)
#define lockdep_assert_held(lock) CHECK(*(lock) == 1)
static u64 ktime_get_mono_fast_ns(void) { return ++now; }
static void udelay(u32 amount)
{
	CHECK(!shadow_locked && current_report && (amount == 10 || amount == 5));
	CHECK(current_report->phase < 14);
	current->delays[current_report->phase] += amount;
	now += (u64)amount*1000;
}
static u32 read_register(struct fixture *f, u32 address)
{
	u32 value, reserved, phase = current_report->phase;
	CHECK(f == current && !shadow_locked && phase < 14);
	f->read_count++;
	now += 100;
	if (address == BCHP_PRI_ARB_CONTROL_REGS_MASTER_CTL) {
		value=f->master; reserved=0xfffffffeU;
	} else if (address == BCHP_DDR23_CTL_REGS_0_PARAMS2) {
		if ((f->changed_unowned & 1) && phase == CHD_RET_PARAMS_REQUEST) {
			f->params ^= 4; f->changed_unowned &= ~1U;
		}
		value=f->params; reserved=0x00800000U;
	} else if (address == BCHP_PRI_ARB_CONTROL_REGS_REFRESH_CTL_0) {
		if ((f->changed_unowned & 2) && phase == CHD_RET_REFRESH_DISABLE) {
			f->refresh ^= 2; f->changed_unowned &= ~2U;
		}
		value=f->refresh; reserved=0xffffe000U;
	} else {
		CHECK(address == BCHP_DDR23_CTL_REGS_0_CTL_STATUS);
		reserved=~0x73U;
		if (phase == CHD_RET_BASELINE) value=f->baseline_memc;
		else {
			if (phase == CHD_RET_CKE_LOW)
				CHECK(f->commands == 1 && (f->params & 0x10000000) &&
				      !(f->refresh & 0x1000) && f->master == 1);
			if (phase == CHD_RET_CKE_HIGH && current_report->ddr_requested)
				CHECK(f->commands >= 1 && !(f->params & 0x10000000) &&
				      (f->refresh & 0x1000) && current_report->settle_us == 5);
			u32 count=++f->polls[phase];
			CHECK(count <= 21 && (phase == CHD_RET_READINESS || phase == CHD_RET_CKE_LOW || phase == CHD_RET_CKE_HIGH));
			value=phase == CHD_RET_CKE_LOW ? 0x71 : 0x73;
			if (f->busy_phase == phase && count <= f->busy_reads)
				value=phase == CHD_RET_READINESS ? 0x32 : phase == CHD_RET_CKE_LOW ? 0x73 : 0x71;
			if (f->busy_phase2 == phase && count <= f->busy_reads2)
				value=phase == CHD_RET_READINESS ? 0x32 : phase == CHD_RET_CKE_LOW ? 0x73 : 0x71;
			if (f->invalid_phase == phase) value=~0U;
		}
	}
	if (f->bad_read == f->read_count || f->bad_read2 == f->read_count) {
		if (f->bad_kind == 1) return ~0U;
		if (f->bad_kind == 2) return value | reserved;
		if (address == BCHP_PRI_ARB_CONTROL_REGS_MASTER_CTL) return value ^ 1;
		if (address == BCHP_DDR23_CTL_REGS_0_PARAMS2) return value ^ 0x10000000;
		if (address == BCHP_PRI_ARB_CONTROL_REGS_REFRESH_CTL_0) return value ^ 0x1000;
		return value ^ 2;
	}
	return value;
}
static void write_register(struct fixture *f, u32 address, u32 value)
{
	u32 phase=current_report->phase;
	CHECK(f == current && !shadow_locked && f->write_count < 8);
	f->write_count++;
	now += 100;
	if (address == BCHP_PRI_ARB_CONTROL_REGS_MASTER_CTL) {
		CHECK((value & ~1U) == (f->master & ~1U));
		if (!(value & 1)) {
			CHECK(phase == CHD_RET_ARBITER_RESTORE && current_report->wake.result == CHD_RET_POLL_REACHED);
			CHECK(!(f->params & 0x10000000) && (f->refresh & 0x1000));
			f->release_writes++;
		}
		f->master=value;
	} else if (address == BCHP_DDR23_CTL_REGS_0_PARAMS2) {
		CHECK((value & ~0x10000000U) == (f->params & ~0x10000000U) && f->master == 1);
		if (phase == CHD_RET_PARAMS_RESTORE) CHECK(f->refresh & 0x1000);
		f->params=value;
	} else if (address == BCHP_PRI_ARB_CONTROL_REGS_REFRESH_CTL_0) {
		CHECK((value & ~0x1000U) == (f->refresh & ~0x1000U) && f->master == 1);
		if (phase == CHD_RET_REFRESH_DISABLE)
			CHECK(f->commands == 1 && (f->params & 0x10000000));
		f->refresh=value;
	} else {
		CHECK(address == BCHP_DDR23_CTL_REGS_0_REFRESH_CMD && value == 0x60 && f->master == 1);
		CHECK(phase == CHD_RET_ENTRY_COMMAND || phase == CHD_RET_WAKE_COMMAND);
		if (phase == CHD_RET_ENTRY_COMMAND)
			CHECK((f->params & 0x10000000) && (f->refresh & 0x1000));
		if (phase == CHD_RET_WAKE_COMMAND)
			CHECK(!(f->params & 0x10000000) && (f->refresh & 0x1000) && current_report->settle_us == 5);
		f->commands++;
	}
}

/* Test alias only installs the fixture's report pointer; shared body is exact. */
#define crystalhd_flea_retention_cycle actual_retention_cycle
#include "crystalhd_pm_retention_ops.h"
#undef crystalhd_flea_retention_cycle
static enum FLEA_POWER_DOWN_RESULT crystalhd_flea_retention_cycle(
	struct crystalhd_hw *hw, struct chd_pm_retention_report *r)
{
	current_report=r;
	return actual_retention_cycle(hw, r);
}
static void crystalhd_pm_retention_record(struct crystalhd_hw *hw,
	const struct chd_pm_retention_report *r)
{
	CHECK(hw->lock == 1 && !shadow_locked);
	saved_report=*r;
	reports++;
}
static void crystalhd_pm_shadow_trace(struct crystalhd_hw *hw, u32 stage,
	u32 request, u32 old, u32 selected, u32 status, u32 fll)
{
	(void)request; (void)old; (void)selected; (void)status; (void)fll;
	CHECK(hw->lock == 1 && !shadow_locked && stage == CHD_PM_DOWN_REFUSED);
	down_refusals++;
}
static void crystalhd_hw_publish_fault(struct crystalhd_hw *hw)
{
	CHECK(hw->lock == 1 && !shadow_locked);
	hw->dma_fault=1; hw->adp->present=0; hw->adp->faults++;
}
#include "pwm-retention-claim.h"
#include "pwm-retention-wrapper.h"

static struct crystalhd_hw prepare(struct fixture *f)
{
	struct crystalhd_hw hw={0};
	memset(f,0,sizeof(*f));
	f->present=1; f->params=0x25534512; f->refresh=0x1068; f->baseline_memc=0x73;
	current=f; current_report=NULL;
	hw.adp=f; hw.lock=1; hw.FleaShadowSession=4;
	hw.pfnReadDevRegister=read_register; hw.pfnWriteDevRegister=write_register;
	hw.FleaPowerTransition=1; hw.FleaPowerState=FLEA_PS_LP_PENDING; hw.dev_started=1;
	return hw;
}
int main(void)
{
	struct fixture f;
	struct crystalhd_hw hw;
	struct chd_pm_retention_report r;
	u32 i,j,phase;
	hw=prepare(&f);
	CHECK(crystalhd_flea_retention_cycle(&hw,&r) == FLEA_PD_REFUSED);
	CHECK(r.result == CHD_RET_COMPLETED_CYCLE && !r.failure && !r.rollback_failure &&
	      r.phase == CHD_RET_FINISHED && f.read_count == 18 && f.write_count == 8 &&
	      f.commands == 2 && f.release_writes == 1 && f.master == 0 && r.settle_us == 5 &&
	      r.readiness.polls == 1 && r.entry.polls == 1 && r.wake.polls == 1);
	for (i=1;i<=18;i++) for (j=1;j<=2;j++) {
		hw=prepare(&f); f.bad_read=i; f.bad_kind=j;
		enum FLEA_POWER_DOWN_RESULT result=crystalhd_flea_retention_cycle(&hw,&r);
		if (i<=4) CHECK(result == FLEA_PD_REFUSED && !f.write_count && r.failure == CHD_RET_BASELINE);
		else if (i<=11) CHECK(result == FLEA_PD_REFUSED && r.result == CHD_RET_CHECKED_ROLLBACK &&
			!r.rollback_failure && f.master == 0 && f.release_writes == 1);
		else CHECK(result == FLEA_PD_FAULT && r.result == CHD_RET_UNCONFIRMED && r.rollback_failure &&
			f.release_writes == (i==18) && f.master == (i==18 ? 0U : 1U));
		CHECK(r.down_result != FLEA_PD_ENTERED && r.phase != 0);
	}
	const u32 post[] = {5,8,10,13,15,18};
	for (i=0;i<sizeof(post)/sizeof(post[0]);i++) {
		hw=prepare(&f); f.bad_read=post[i]; f.bad_kind=3;
		CHECK(crystalhd_flea_retention_cycle(&hw,&r) == (post[i]<=10 ? FLEA_PD_REFUSED : FLEA_PD_FAULT));
		CHECK(r.failure || r.rollback_failure);
	}
	for (i=0;i<4;i++) {
		hw=prepare(&f);
		if (i==0) f.master=1;
		if (i==1) f.params|=0x10000000;
		if (i==2) f.refresh&=~0x1000U;
		if (i==3) f.baseline_memc=0x71;
		CHECK(crystalhd_flea_retention_cycle(&hw,&r) == FLEA_PD_REFUSED && !f.write_count);
	}
	for (phase=0;phase<3;phase++) for(i=0;i<=21;i++) {
		hw=prepare(&f);
		f.busy_phase=phase==0 ? CHD_RET_READINESS : phase==1 ? CHD_RET_CKE_LOW : CHD_RET_CKE_HIGH;
		f.busy_reads=i;
		enum FLEA_POWER_DOWN_RESULT result=crystalhd_flea_retention_cycle(&hw,&r);
		const struct chd_pm_retention_poll *p=phase==0 ? &r.readiness : phase==1 ? &r.entry : &r.wake;
		CHECK(p->polls == (i==21 ? 21U : i+1) && p->delay_us == (i==21 ? 200U : i*10));
		if (i<21) CHECK(result == FLEA_PD_REFUSED && r.result == CHD_RET_COMPLETED_CYCLE);
		else if(phase<2) CHECK(result == FLEA_PD_REFUSED && r.result == CHD_RET_CHECKED_ROLLBACK &&
			p->result == CHD_RET_POLL_TIMEOUT && f.master == 0);
		else CHECK(result == FLEA_PD_FAULT && p->result == CHD_RET_POLL_TIMEOUT && !f.release_writes);
	}
	for (i=0;i<3;i++) {
		hw=prepare(&f); f.invalid_phase=i==0 ? CHD_RET_READINESS : i==1 ? CHD_RET_CKE_LOW : CHD_RET_CKE_HIGH;
		CHECK(crystalhd_flea_retention_cycle(&hw,&r) == (i==2 ? FLEA_PD_FAULT : FLEA_PD_REFUSED));
		CHECK((i==2 ? r.wake.result : i==1 ? r.entry.result : r.readiness.result) == CHD_RET_POLL_INVALID);
		if(i==0) CHECK(!r.ddr_requested && !r.settle_us && !f.commands && f.write_count == 2);
	}
	hw=prepare(&f); f.changed_unowned=3;
	CHECK(crystalhd_flea_retention_cycle(&hw,&r) == FLEA_PD_REFUSED);
	/* Mutated live-unowned fields survive entry, recovery, and the final release. */
	CHECK((r.params_entry_live & ~0x10000000U) == (0x25534512U ^ 4U) &&
	      (r.refresh_entry_live & ~0x1000U) == (0x0068U ^ 2U) &&
	      f.params == (0x25534512U ^ 4U) && f.refresh == (0x1068U ^ 2U));
	hw=prepare(&f); arbiter_attempted=false;
	CHECK(crystalhd_flea_runtime_power_dn(&hw) == FLEA_PD_REFUSED && reports==1 &&
	      saved_report.result == CHD_RET_COMPLETED_CYCLE && !hw.dma_fault);
	for(i=0;i<3;i++) {
		u32 reads=f.read_count,writes=f.write_count;
		CHECK(crystalhd_flea_runtime_power_dn(&hw) == FLEA_PD_REFUSED &&
		      reads==f.read_count && writes==f.write_count && reports==1);
	}
	hw=prepare(&f); arbiter_attempted=false; f.bad_read=16; f.bad_kind=1;
	CHECK(crystalhd_flea_runtime_power_dn(&hw) == FLEA_PD_FAULT && hw.dma_fault &&
	      hw.FleaPowerState==FLEA_PS_FAULT && !f.present && !f.release_writes && f.master==1);
	CHECK(!shadow_locked);
	/* Preserve both the partial-entry failure and a later rollback failure. */
	for(i=0;i<3;i++) {
		hw=prepare(&f); f.bad_read=8; f.bad_kind=1;
		f.bad_read2=i==0 ? 9U : i==1 ? 11U : 15U;
		CHECK(crystalhd_flea_retention_cycle(&hw,&r) == FLEA_PD_FAULT &&
		      r.failure==CHD_RET_PARAMS_REQUEST &&
		      r.rollback_failure==(i==0 ? CHD_RET_REFRESH_RESTORE : i==1 ? CHD_RET_PARAMS_RESTORE : CHD_RET_ARBITER_RESTORE));
		CHECK(f.release_writes==(i==2) && f.master==(i==2 ? 0U : 1U));
	}
	hw=prepare(&f); f.busy_phase=CHD_RET_CKE_LOW; f.busy_reads=21;
	f.busy_phase2=CHD_RET_CKE_HIGH; f.busy_reads2=21;
	CHECK(crystalhd_flea_retention_cycle(&hw,&r) == FLEA_PD_FAULT &&
	      r.failure==CHD_RET_CKE_LOW && r.rollback_failure==CHD_RET_CKE_HIGH &&
	      r.entry.polls==21 && r.wake.polls==21 && r.entry.delay_us==200 &&
	      r.wake.delay_us==200 && !f.release_writes && f.master==1);
	printf("pwm-retention: %u checks PASS; exact protocol/order/rollback; no device\n",checks);
	return 0;
}
