/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Actual arbiter-only helper/one-shot wrapper with device-free MMIO fixtures. */
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
#include "pwm-arbiter-types.h"
#include "crystalhd_pm_arbiter.h"
#include "crystalhd_pm_shadow_ring.h"
#include "70015/magnum/basemodules/chp/70015/rdb/a0/bchp_pri_arb_control_regs.h"
#include "70015/magnum/basemodules/chp/70015/rdb/a0/bchp_ddr23_ctl_regs_0.h"

struct fixture {
	u32 present, master[4], memc[21], writes[2];
	u32 master_reads, memc_reads, write_count, delays, delay_us;
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
static unsigned int checks, fault_publications, down_refusals, report_count;
static int shadow_lock, shadow_locked;
static bool arbiter_attempted;
static u64 now;
static struct fixture *current;
static struct chd_pm_arbiter_report saved_report;

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
	CHECK(!shadow_locked && current && amount == 10);
	current->delays++;
	current->delay_us += amount;
	now += (u64)amount * 1000;
}
static u32 read_register(struct fixture *f, u32 address)
{
	CHECK(!shadow_locked && f == current);
	now += 100;
	if (address == BCHP_PRI_ARB_CONTROL_REGS_MASTER_CTL) {
		CHECK(f->master_reads < 4);
		return f->master[f->master_reads++];
	}
	CHECK(address == BCHP_DDR23_CTL_REGS_0_CTL_STATUS && f->memc_reads < 21);
	return f->memc[f->memc_reads++];
}
static void write_register(struct fixture *f, u32 address, u32 value)
{
	CHECK(!shadow_locked && f == current);
	CHECK(address == BCHP_PRI_ARB_CONTROL_REGS_MASTER_CTL && f->write_count < 2);
	f->writes[f->write_count++] = value;
	now += 100;
}
static void crystalhd_pm_arbiter_record(struct crystalhd_hw *hw,
	const struct chd_pm_arbiter_report *report)
{
	CHECK(hw->lock == 1 && !shadow_locked);
	saved_report = *report;
	report_count++;
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
	hw->dma_fault = 1;
	hw->adp->present = 0;
	fault_publications++;
}

#include "pwm-arbiter-claim.h"
#include "pwm-arbiter-functions.h"

static struct crystalhd_hw prepare(struct fixture *f)
{
	struct crystalhd_hw hw = {0};
	u32 i;
	memset(f, 0, sizeof(*f));
	f->present = 1;
	f->master[0] = 0; f->master[1] = 1; f->master[2] = 1; f->master[3] = 0;
	for (i=0; i<21; i++) f->memc[i] = 0x73;
	current = f;
	hw.adp = f;
	hw.FleaShadowSession = 4;
	hw.lock = 1;
	hw.pfnReadDevRegister = read_register;
	hw.pfnWriteDevRegister = write_register;
	hw.FleaPowerTransition = 1;
	hw.FleaPowerState = FLEA_PS_LP_PENDING;
	hw.dev_started = 1;
	return hw;
}

int main(void)
{
	struct fixture f;
	struct crystalhd_hw hw;
	struct chd_pm_arbiter_report r;
	u32 i, j;

	for (i=0; i<3; i++) {
		hw = prepare(&f);
		f.master[0] = i == 0 ? 1U : i == 1 ? 2U : ~0U;
		CHECK(crystalhd_flea_arbiter_readiness(&hw, &r) == FLEA_PD_REFUSED);
		CHECK(r.result == CHD_ARB_BASELINE_INVALID && !r.touched && !r.polls &&
		      r.restore == CHD_ARB_RESTORE_NOT_ATTEMPTED && f.master_reads == 1 &&
		      !f.write_count && !f.memc_reads && !f.delay_us);
	}
	for (i=0; i<3; i++) {
		hw = prepare(&f);
		f.master[1] = i == 0 ? 0U : i == 1 ? 2U : ~0U;
		CHECK(crystalhd_flea_arbiter_readiness(&hw, &r) == FLEA_PD_REFUSED);
		CHECK(r.result == CHD_ARB_DISABLE_UNCONFIRMED && r.touched && !r.polls &&
		      r.restore == CHD_ARB_RESTORE_CONFIRMED && f.master_reads == 4 &&
		      f.write_count == 2 && f.writes[0] == 1 && f.writes[1] == 0 &&
		      !f.memc_reads && !f.delay_us);
	}
	for (i=0; i<128; i++) {
		hw = prepare(&f);
		f.memc[0] = i;
		CHECK(crystalhd_flea_arbiter_readiness(&hw, &r) == FLEA_PD_REFUSED);
		CHECK(r.restore == CHD_ARB_RESTORE_CONFIRMED && f.write_count == 2 &&
		      f.writes[0] == 1 && f.writes[1] == 0 && r.polls <= 21 && r.delay_us <= 200);
		if (i & ~0x73U)
			CHECK(r.result == CHD_ARB_MEMC_INVALID && r.polls == 1 && !r.delay_us);
		else if (i == 0x73)
			CHECK(r.result == CHD_ARB_GUARDS_REACHED && r.polls == 1 && !r.delay_us);
		else
			CHECK(r.result == CHD_ARB_GUARDS_REACHED && r.polls == 2 && r.delay_us == 10);
		CHECK(r.memc_first == i && r.memc_last == (r.polls == 1 ? i : 0x73));
	}
	hw = prepare(&f); f.memc[0] = ~0U;
	CHECK(crystalhd_flea_arbiter_readiness(&hw, &r) == FLEA_PD_REFUSED &&
	      r.result == CHD_ARB_MEMC_INVALID && r.polls == 1 && !r.delay_us);
	for (i=0; i<21; i++) {
		hw = prepare(&f);
		for (j=0; j<i; j++) f.memc[j] = 0x32;
		CHECK(crystalhd_flea_arbiter_readiness(&hw, &r) == FLEA_PD_REFUSED);
		CHECK(r.result == CHD_ARB_GUARDS_REACHED && r.polls == i+1 &&
		      f.memc_reads == i+1 && r.delay_us == i*10 && f.delays == i &&
		      r.restore == CHD_ARB_RESTORE_CONFIRMED);
	}
	for (i=0; i<4; i++) {
		hw = prepare(&f);
		for (j=0; j<21; j++) f.memc[j] = i == 0 ? 0x32U : i == 1 ? 0x33U : i == 2 ? 0x71U : 0U;
		CHECK(crystalhd_flea_arbiter_readiness(&hw, &r) == FLEA_PD_REFUSED);
		CHECK(r.result == CHD_ARB_TIMEOUT && r.polls == 21 && f.memc_reads == 21 &&
		      r.delay_us == 200 && f.delays == 20 && f.master_reads == 4 &&
		      f.write_count == 2 && r.restore == CHD_ARB_RESTORE_CONFIRMED &&
		      r.end_ns - r.begin_ns >= (u64)r.delay_us*1000);
	}
	for (i=0; i<2; i++) {
		hw = prepare(&f); f.master[2] = i ? ~0U : 2U;
		CHECK(crystalhd_flea_arbiter_readiness(&hw, &r) == FLEA_PD_FAULT);
		CHECK(r.restore == CHD_ARB_RESTORE_UNCONFIRMED && f.write_count == 1 &&
		      f.master_reads == 3 && r.restore_readback == CHD_PM_SHADOW_UNKNOWN);
	}
	for (i=0; i<3; i++) {
		hw = prepare(&f); f.master[3] = i == 0 ? 1U : i == 1 ? 2U : ~0U;
		CHECK(crystalhd_flea_arbiter_readiness(&hw, &r) == FLEA_PD_FAULT);
		CHECK(r.restore == CHD_ARB_RESTORE_UNCONFIRMED && f.write_count == 2 &&
		      f.master_reads == 4 && f.writes[1] == 0);
	}
	/* The actual module-lifetime claim is never reset by the wrapper/session. */
	hw = prepare(&f);
	CHECK(crystalhd_flea_runtime_power_dn(&hw) == FLEA_PD_REFUSED &&
	      report_count == 1 && saved_report.result == CHD_ARB_GUARDS_REACHED &&
	      !hw.dma_fault && hw.FleaPowerState == FLEA_PS_LP_PENDING);
	for (i=0; i<3; i++) {
		u32 reads = f.master_reads + f.memc_reads, writes = f.write_count;
		CHECK(crystalhd_flea_runtime_power_dn(&hw) == FLEA_PD_REFUSED &&
		      reads == f.master_reads + f.memc_reads && writes == f.write_count && report_count == 1);
	}
	CHECK(down_refusals == 4 && !fault_publications && !shadow_locked);
	CHECK(saved_report.session == 4);
	/* Host owner/refusal checks cannot consume the one permitted MMIO attempt. */
	for (i=0; i<17; i++) {
		u32 *owners[] = {
			&hw.FleaTxActive, &hw.FleaIrqActive, &hw.FleaRawActive,
			&hw.fwcmd_trans_active, &hw.fwcmd_active, &hw.FwCmdCnt,
			&hw.fwcmd_poisoned, &hw.TxList0Sts, &hw.TxList1Sts,
			&hw.rx_list_sts[0], &hw.rx_list_sts[1], &hw.EmptyCnt,
			&hw.SingleThreadAppFIFOEmpty,
		};
		hw = prepare(&f);
		arbiter_attempted = false;
		if (i<13) *owners[i] = 1;
		else if (i==13) hw.FleaPowerTransition = 0;
		else if (i==14) hw.FleaPowerState = FLEA_PS_ACTIVE;
		else if (i==15) f.present = 0;
		else hw.dev_started = 0;
		CHECK(crystalhd_flea_runtime_power_dn(&hw) == FLEA_PD_REFUSED &&
		      !arbiter_attempted && !f.master_reads && !f.memc_reads && !f.write_count);
	}
	/* Fresh test-only module instance: a failed restore retains terminal fault. */
	arbiter_attempted = false;
	hw = prepare(&f); f.master[3] = 1;
	CHECK(crystalhd_flea_runtime_power_dn(&hw) == FLEA_PD_FAULT &&
	      hw.FleaPowerState == FLEA_PS_FAULT && hw.dma_fault && !f.present &&
	      fault_publications == 1 && saved_report.restore == CHD_ARB_RESTORE_UNCONFIRMED);
	CHECK(!shadow_locked);
	printf("pwm-arbiter: %u checks PASS; one-shot, 21 MEMC reads/200us delay max; no device\n", checks);
	return 0;
}
