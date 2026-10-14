/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Exact guarded command observer with synthetic registers; no device opens. */
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "crystalhd_fw_research.h"

typedef uint32_t u32;
typedef uint64_t u64;
typedef int BC_STATUS;

#define BC_STS_SUCCESS 0
#define BC_STS_FW_CMD_ERR 11
#define BC_STS_IO_ERROR 5
#define eCMD_C011_INIT UINT32_C(0x73763001)
#define BC_LINK_INIT 1U
#define FLEA_PS_ACTIVE 3U
#define BCHP_DECODE_CPUDMA2_0_REG_DMA_STATUS UINT32_C(0x00851840)
#define BCHP_DECODE_CPUREGS2_0_REG_SDRAM_STATUS UINT32_C(0x00800fd0)
#define BCHP_INTR_INTR_STATUS UINT32_C(0x00500700)
#define BCHP_INTR_INTR_STATUS_ARM_MBOX0_INTR_MASK UINT32_C(0x00010000)
#define CRYSTALHD_FW_RESEARCH_CPUDMA_ACTIVE_MASK UINT32_C(0x0000000f)
#define CRYSTALHD_FW_RESEARCH_SDRAM_BUSY_MASK UINT32_C(0x00000001)
#define READ_ONCE(value) (value)
#define lockdep_assert_held(target) \
	assert(lock_depth && (target) == &hardware.lock)

typedef pthread_mutex_t spinlock_t;
static _Thread_local unsigned lock_depth;
#define spin_lock_irqsave(target, saved_flags) do { \
	(saved_flags) = 0x1234UL; assert((target) == &hardware.lock); \
	assert(!pthread_mutex_lock(target)); lock_depth++; \
} while (0)
#define spin_unlock_irqrestore(target, saved_flags) do { \
	assert((target) == &hardware.lock && (saved_flags) == 0x1234UL && lock_depth == 1); \
	lock_depth--; assert(!pthread_mutex_unlock(target)); \
} while (0)

typedef struct {
	u32 cmd[64];
	u32 rsp[64];
	u32 flags;
	u32 add_data;
} BC_FW_CMD;

struct crystalhd_hw;
struct crystalhd_adp;
struct crystalhd_cmd {
	struct crystalhd_adp *adp;
	struct crystalhd_hw *hw_ctx;
	const void *session_owner;
	bool session_module_pinned;
	u32 state;
};
struct crystalhd_adp {
	u64 generation;
	bool present, hw_accessible;
	struct crystalhd_cmd cmds;
};
struct crystalhd_fw_command_observer {
	u64 generation;
	u32 request_words[3];
	int hook_status;
	bool posted;
	struct crystalhd_fw_research_command_observation *result;
};
struct crystalhd_hw {
	spinlock_t lock;
	struct crystalhd_adp *adp;
	bool dev_started, dma_fault;
	bool fwcmd_pending, fwcmd_trans_active, fwcmd_active, fwcmd_poisoned;
	u32 FleaPowerState;
	bool FleaPowerTransition;
	struct crystalhd_fw_command_observer *fwcmd_observer;
	u32 (*pfnReadDevRegister)(struct crystalhd_adp *, u32);
};

static struct crystalhd_hw hardware;
static struct crystalhd_adp adapter;
#define command_context (adapter.cmds)
static u64 now_ns;
static unsigned reads, forbidden_reads, checks, failures;
static bool complete_during_cpudma;
static unsigned ready_calls, complete_at_ready_call;

static int crystalhd_fw_research_state_context(struct crystalhd_cmd *ctx,
	u64 generation, struct crystalhd_hw *hw)
{
	ready_calls++;
	if (complete_at_ready_call == ready_calls)
		hardware.fwcmd_pending = false;
	return ctx == &command_context && ctx->adp == &adapter &&
		hw == &hardware && ctx->hw_ctx == hw && adapter.present &&
		adapter.hw_accessible && adapter.generation == generation &&
		ctx->session_owner && ctx->session_module_pinned &&
		ctx->state == BC_LINK_INIT ? 0 : -ESTALE;
}

static int crystalhd_fw_research_state_ready(struct crystalhd_cmd *ctx,
	u64 generation, struct crystalhd_hw *hw)
{
	int rc = crystalhd_fw_research_state_context(ctx, generation, hw);

	if (rc)
		return rc;
	if (hw->dma_fault)
		return -EIO;
	if (hw->fwcmd_pending || hw->fwcmd_poisoned)
		return -EBUSY;
	return hw->FleaPowerState == FLEA_PS_ACTIVE ? 0 : -EAGAIN;
}

static int crystalhd_status_to_errno(BC_STATUS status)
{
	return status == BC_STS_SUCCESS ? 0 : -EIO;
}

static u64 ktime_get_ns(void)
{
	return ++now_ns;
}

static u32 read_register(struct crystalhd_adp *adp, u32 address)
{
	assert(adp == &adapter && lock_depth == 1);
	reads++;
	switch (address) {
	case BCHP_DECODE_CPUDMA2_0_REG_DMA_STATUS:
		if (complete_during_cpudma) {
			complete_during_cpudma = false;
			hardware.fwcmd_pending = false;
		}
		return UINT32_C(0xabc00000) | (reads & 0xfU);
	case BCHP_DECODE_CPUREGS2_0_REG_SDRAM_STATUS:
		return UINT32_C(0xdef00000) | (reads & 1U);
	case BCHP_INTR_INTR_STATUS:
		return UINT32_C(0x55550000);
	default:
		forbidden_reads++;
		return 0;
	}
}

#include "fw-command-observer-functions.h"

static void check(bool condition, const char *reason)
{
	checks++;
	if (!condition) {
		failures++;
		fprintf(stderr, "FAIL: %s\n", reason);
	}
}

static void reset_fixture(void)
{
	memset(&hardware, 0, sizeof(hardware));
	memset(&adapter, 0, sizeof(adapter));
	memset(&command_context, 0, sizeof(command_context));
	assert(!pthread_mutex_init(&hardware.lock, NULL));
	adapter.generation = 37;
	adapter.present = true;
	adapter.hw_accessible = true;
	command_context.adp = &adapter;
	command_context.hw_ctx = &hardware;
	command_context.session_owner = &adapter;
	command_context.session_module_pinned = true;
	command_context.state = BC_LINK_INIT;
	hardware.adp = &adapter;
	hardware.dev_started = true;
	hardware.FleaPowerState = FLEA_PS_ACTIVE;
	hardware.pfnReadDevRegister = read_register;
	now_ns = reads = forbidden_reads = ready_calls = 0;
	complete_at_ready_call = 0;
	complete_during_cpudma = false;
}

static void destroy_fixture(void)
{
	assert(!hardware.fwcmd_observer && !lock_depth);
	assert(!pthread_mutex_destroy(&hardware.lock));
}

static void begin_pending(void)
{
	hardware.fwcmd_pending = true;
	hardware.fwcmd_active = true;
	hardware.fwcmd_trans_active = true;
}

static void end_pending(void)
{
	hardware.fwcmd_pending = false;
	hardware.fwcmd_active = false;
	hardware.fwcmd_trans_active = false;
}

static void success_case(void)
{
	struct crystalhd_fw_research_command_observation result;
	struct crystalhd_fw_command_observer observer;
	BC_FW_CMD fw = { .cmd = { eCMD_C011_INIT, 1, 64 } };
	unsigned int i;

	reset_fixture();
	check(!crystalhd_fw_research_observer_arm(&command_context, 37,
		fw.cmd, &observer, &result), "exact fresh INIT arms");
	begin_pending();
	crystalhd_fw_research_command_posted(&hardware, &fw);
	check(result.attempted && result.sample_count ==
		CRYSTALHD_FW_RESEARCH_COMMAND_SAMPLE_MAX,
		"fixed bound produces all admitted pairs");
	check(reads == 2U * CRYSTALHD_FW_RESEARCH_COMMAND_SAMPLE_MAX + 1U &&
		!forbidden_reads, "only two status registers and one host status are read");
	check(result.host_intr_valid &&
		result.host_intr_arm_mbox0 == BCHP_INTR_INTR_STATUS_ARM_MBOX0_INTR_MASK,
		"host aggregate sample is masked and taken once");
	for (i = 0; i < result.sample_count; i++)
		check(result.samples[i].cpudma_active <= 0xfU &&
		      result.samples[i].sdram_busy <= 1U &&
		      result.samples[i].cpudma_ns <= result.samples[i].sdram_ns,
		      "each pair is masked and ordered");
	fw.rsp[0] = fw.cmd[0];
	fw.rsp[1] = fw.cmd[1];
	fw.rsp[2] = 0;
	end_pending();
	check(!crystalhd_fw_research_observer_finish(&command_context,
		&observer, &fw, BC_STS_SUCCESS), "matching ordinary reply finishes");
	check(!result.status && result.header_matches && !result.read_complete &&
		!hardware.fwcmd_observer,
		"finish preserves samples but leaves whole-run completion unpublished");
	destroy_fixture();
}

static void guard_failure_case(void)
{
	struct crystalhd_fw_research_command_observation result;
	struct crystalhd_fw_command_observer observer;
	BC_FW_CMD fw = { .cmd = { eCMD_C011_INIT, 1, 64 } };

	reset_fixture();
	check(!crystalhd_fw_research_observer_arm(&command_context, 37,
		fw.cmd, &observer, &result), "fault case arms");
	begin_pending();
	complete_during_cpudma = true;
	crystalhd_fw_research_command_posted(&hardware, &fw);
	fw.rsp[0] = fw.cmd[0];
	fw.rsp[1] = fw.cmd[1];
	end_pending();
	check(crystalhd_fw_research_observer_finish(&command_context,
		&observer, &fw, BC_STS_SUCCESS) < 0,
		"constituent-read guard change rejects observation");
	check(result.attempted && result.status && !result.sample_count &&
		!result.read_complete && !result.host_intr_valid,
		"refusal publishes no partial raw sample");
	check(!forbidden_reads, "guard failure never broadens register surface");
	destroy_fixture();
}

static void completion_gap_case(void)
{
	struct crystalhd_fw_research_command_observation result;
	struct crystalhd_fw_command_observer observer;
	BC_FW_CMD fw = { .cmd = { eCMD_C011_INIT, 1, 64 } };

	reset_fixture();
	check(!crystalhd_fw_research_observer_arm(&command_context, 37,
		fw.cmd, &observer, &result), "completion-gap case arms");
	begin_pending();
	/* Arm is readiness call 1; call 8 is before the second SDRAM read. */
	complete_at_ready_call = 8;
	crystalhd_fw_research_command_posted(&hardware, &fw);
	check(result.sample_count == 1 && !observer.hook_status,
		"completion between constituent reads retains only the complete pair");
	fw.rsp[0] = fw.cmd[0];
	fw.rsp[1] = fw.cmd[1];
	end_pending();
	check(!crystalhd_fw_research_observer_finish(&command_context,
		&observer, &fw, BC_STS_SUCCESS),
		"ordinary completion after a complete pair remains authoritative");
	check(result.sample_count == 1 && !result.status,
		"clean completion boundary preserves admitted evidence");
	destroy_fixture();
}

static void identity_and_default_cases(void)
{
	struct crystalhd_fw_research_command_observation result;
	struct crystalhd_fw_command_observer observer;
	BC_FW_CMD wrong = { .cmd = { eCMD_C011_INIT, 2, 64 } };
	BC_FW_CMD fw = { .cmd = { eCMD_C011_INIT, 1, 64 } };

	reset_fixture();
	crystalhd_fw_research_command_posted(&hardware, &fw);
	check(!reads, "default-unarmed hook performs no register access");
	check(crystalhd_fw_research_observer_arm(&command_context, 37,
		wrong.cmd, &observer, &result) == -EINVAL &&
		!hardware.fwcmd_observer && !reads,
		"non-fresh sequence fails before hardware access");
	hardware.FleaPowerTransition = true;
	check(crystalhd_fw_research_observer_arm(&command_context, 37,
		fw.cmd, &observer, &result) == -EAGAIN &&
		!hardware.fwcmd_observer && !reads,
		"power transition refuses arm before hardware access");
	hardware.FleaPowerTransition = false;
	check(!crystalhd_fw_research_observer_arm(&command_context, 37,
		fw.cmd, &observer, &result), "identity mismatch case arms");
	begin_pending();
	wrong.cmd[1] = 1;
	wrong.cmd[2] = 65;
	crystalhd_fw_research_command_posted(&hardware, &wrong);
	end_pending();
	check(crystalhd_fw_research_observer_finish(&command_context,
		&observer, &wrong, BC_STS_SUCCESS) < 0 && !result.sample_count,
		"changed frozen request is rejected without a register read");
	check(!reads && !forbidden_reads, "identity refusal is device-free");
	destroy_fixture();
}

int main(void)
{
	success_case();
	guard_failure_case();
	completion_gap_case();
	identity_and_default_cases();
	printf("Firmware command observer: %u checks, %u failures (no hardware)\n",
	       checks, failures);
	return failures != 0;
}
