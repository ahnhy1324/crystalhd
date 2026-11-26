/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Reuse the existing MMIO/fault environment, not copied PM implementations. */
#define main legacy_flea_power_main
#include "flea-power.c"
#undef main

static unsigned research_cases;

static struct crystalhd_hw research_setup(unsigned attempts, unsigned milliseconds)
{
    struct crystalhd_hw hw = transition_setup(FLEA_PS_ACTIVE);
    memset(&pm_ledger, 0, sizeof(pm_ledger)); /* A fresh simulated module. */
    memset(&pm_observation, 0, sizeof(pm_observation));
    pm_research_attempts = attempts;
    pm_research_ms = milliseconds;
    research_time = 1000000;
    research_locked = false;
    real_init_power_state(&hw);
    device.actual_power_up = true;
    check(real_set_next_power_state(&hw, FLEA_EVT_START_DEVICE) == BC_STS_SUCCESS,
          "actual start establishes active state");
    research_cases++;
    return hw;
}

static void research_enter(struct crystalhd_hw *hw)
{
    check(real_set_next_power_state(hw, FLEA_EVT_PAUSE) == BC_STS_SUCCESS &&
          hw->FleaPowerState == FLEA_PS_LP_COMPLETE && hw->FleaPowerSaved.valid &&
          pm_ledger.phase == CHD_PM_ASLEEP,
          "actual down enters with a retained saved register baseline");
}

static void research_resume(struct crystalhd_hw *hw)
{
    check(real_set_next_power_state(hw, FLEA_EVT_RESUME) == BC_STS_SUCCESS &&
          hw->FleaPowerState == FLEA_PS_ACTIVE && !hw->FleaPowerSaved.valid &&
          pm_ledger.phase == CHD_PM_IDLE && device.last_fll == device.free_queue_count,
          "actual physical restore and current FLL precede ACTIVE");
}

static void limits_and_claim(void)
{
    check(pm_limits_valid(0, 0) && pm_limits_valid(1, 1) &&
          pm_limits_valid(CHD_PM_MAX_ATTEMPTS, CHD_PM_MAX_MS),
          "OFF and finite hard endpoints are accepted");
    const unsigned invalid[][2] = {{0,1},{1,0},{CHD_PM_MAX_ATTEMPTS+1,1},
        {1,CHD_PM_MAX_MS+1},{UINT32_MAX,1},{1,UINT32_MAX}};
    for (unsigned i = 0; i < ARRAY_SIZE(invalid); i++)
        check(!pm_limits_valid(invalid[i][0],invalid[i][1]), "partial/unbounded options are rejected");
    struct crystalhd_hw hw = research_setup(0, 0);
    check(!hw.FleaEnablePWM && !hw.FleaResearchSession && !pm_ledger.claimed,
          "same experimental build defaults to OFF");
    check(pm_observation.initializations==1 && !pm_observation.enabled_initializations &&
          hw.FleaResearchContext==1 && !pm_observation.count,
          "OFF initialization is observed without claiming or a selector owner snapshot");
    hw = research_setup(8, 5000);
    check(hw.FleaEnablePWM && !hw.FleaResearchSession && !pm_ledger.claimed &&
          !pm_ledger.count && !pm_ledger.start_ns,
          "temporary setup initialization only arms host fences, never claims a session");
    check(real_set_next_power_state(&hw,FLEA_EVT_STOP_DEVICE)==BC_STS_SUCCESS,
          "temporary setup closes without physical runtime down");
    crystalhd_pm_research_retire(&hw);
    struct crystalhd_hw other=transition_setup(FLEA_PS_ACTIVE);
    real_init_power_state(&other);
    device.actual_power_up=true;
    check(real_set_next_power_state(&other,FLEA_EVT_START_DEVICE)==BC_STS_SUCCESS &&
          !pm_ledger.claimed && !pm_ledger.count,
          "real first stream can initialize after the throwaway setup context");
    check(hw.FleaResearchContext==1 && other.FleaResearchContext==2 &&
          pm_observation.initializations==2 && pm_observation.enabled_initializations==2,
          "throwaway and real contexts remain distinct without consuming physical admission");
    research_time+=UINT64_C(6000000000); /* Long firmware/startup delay. */
    research_enter(&other);
    check(pm_ledger.start_ns==research_time &&
          pm_ledger.deadline_ns==research_time+UINT64_C(5000000000),
          "finite window starts at first actual down admission, not hwopen/startup");
    const unsigned count=pm_ledger.count;
    hw=transition_setup(FLEA_PS_ACTIVE);
    check(!crystalhd_pm_research_session(&hw) && !hw.FleaResearchSession &&
          pm_ledger.count==count,"reopen cannot replenish module-wide claim");
    check(!crystalhd_pm_research_down_begin(&hw) &&
          !crystalhd_pm_research_up_begin(&hw) &&
          crystalhd_pm_research_down_end(&hw,FLEA_PD_ENTERED)==FLEA_PD_FAULT &&
          crystalhd_pm_research_up_end(&hw,BC_STS_SUCCESS)==BC_STS_IO_ERROR &&
          pm_ledger.count==count && pm_ledger.attempts==1,
          "unclaimed context cannot consume or alter the owned ledger");
    hw=research_setup(1,1);
    research_time=UINT64_MAX-999;
    check(real_set_next_power_state(&hw,FLEA_EVT_PAUSE)==BC_STS_SUCCESS &&
          !pm_ledger.claimed && !device.reads,
          "absolute deadline arithmetic cannot wrap or permit target IO");
}

static void finite_cycles(void)
{
    struct crystalhd_hw hw = research_setup(8, 5000);
    for (unsigned i=0; i<8; i++) {
        research_enter(&hw);
        research_time += 10000;
        research_resume(&hw);
        research_time += 10000;
    }
    const unsigned reads=device.reads,writes=device.writes;
    check(real_set_next_power_state(&hw,FLEA_EVT_PAUSE)==BC_STS_SUCCESS &&
          hw.FleaPowerState==FLEA_PS_LP_PENDING && pm_ledger.stop==CHD_PM_LIMIT &&
          pm_ledger.attempts==8 && pm_ledger.entered==8 && pm_ledger.up_attempts==8 &&
          pm_ledger.up_completed==8 && pm_ledger.fll_completed==8 &&
          device.reads==reads && device.writes==writes,
          "ninth attempt is rejected before any physical register IO");
    check(pm_ledger.count==42 && !pm_ledger.dropped,
          "eight complete cycles plus first limit fit immutable receipts");
    for(unsigned i=0;i<pm_ledger.count;i++) {
        const struct chd_pm_record *r=&pm_ledger.records[i];
        check(r->session==1 && r->time_ns>=pm_ledger.start_ns,
              "every copied receipt has the owning session and monotonic time");
        if(r->event==CHD_PM_DOWN_END)
            check(r->state==FLEA_PS_LP_COMPLETE && r->snapshot && !r->tx && !r->irq &&
                  !r->raw && !r->cleanup,"entry receipt records actual low state without owners");
        if(r->event==CHD_PM_FLL)
            check(r->state==FLEA_PS_ACTIVE && !r->snapshot && r->fll==4,
                  "FLL receipt is only complete after ACTIVE publication");
    }
    research_resume(&hw);
    crystalhd_pm_research_retire(&hw);
    check(pm_ledger.retired && pm_ledger.count==43 && pm_ledger.stop==CHD_PM_LIMIT,
          "retirement preserves exhausted budget and cannot restart the experiment");
}

static void deadline_and_capacity(void)
{
    struct crystalhd_hw hw=research_setup(2,1);
    research_enter(&hw);
    research_resume(&hw);
    research_time=pm_ledger.deadline_ns;
    const unsigned reads=device.reads,writes=device.writes;
    check(real_set_next_power_state(&hw,FLEA_EVT_PAUSE)==BC_STS_SUCCESS &&
          pm_ledger.attempts==1 && pm_ledger.stop==CHD_PM_DEADLINE &&
          device.reads==reads && device.writes==writes,
          "exact absolute deadline blocks the next physical attempt");
    hw=research_setup(1,1);
    research_enter(&hw);
    research_time=pm_ledger.deadline_ns+100000;
    research_resume(&hw);
    check(pm_ledger.up_completed==1 && pm_ledger.fll_completed==1 &&
          !hw.dma_fault,"expired admission window does not block outstanding wake/FLL");
    hw=research_setup(1,1);
    pm_ledger.count=CHD_PM_RECORDS-8; /* Deliberate capacity boundary fixture. */
    check(real_set_next_power_state(&hw,FLEA_EVT_PAUSE)==BC_STS_SUCCESS &&
          !pm_ledger.attempts && !device.reads && pm_ledger.stop==CHD_PM_CAPACITY &&
          !pm_ledger.dropped,"insufficient reserved receipt space refuses before physical IO");
}

static void moving_owners_and_cleanup(void)
{
    struct crystalhd_hw hw=research_setup(2,5000);
    hw.FleaTxActive=1;
    for(unsigned i=0;i<1000;i++)
        check(real_set_next_power_state(&hw,FLEA_EVT_CMD_COMP)==BC_STS_SUCCESS,
              "busy owner selection is an ordinary nonphysical decision");
    check(!pm_ledger.attempts && !pm_ledger.count && pm_ledger.stop==CHD_PM_RUNNING,
          "busy IRQ/owner activity neither spends attempts nor fills receipts");
    hw.FleaTxActive=0;
    bool held;
    check(crystalhd_flea_cleanup_begin(&hw,&held)==BC_STS_SUCCESS && held,
          "actual cleanup exclusion retains the research-enabled device");
    check(real_set_next_power_state(&hw,FLEA_EVT_PAUSE)==BC_STS_BUSY &&
          !pm_ledger.attempts && !device.reads,
          "cleanup hold blocks entry before a ledger claim or target IO");
    crystalhd_flea_cleanup_end(&hw,held);
    research_enter(&hw);
}

static void refusal_and_faults(void)
{
    struct crystalhd_hw hw=research_setup(8,5000);
    device.idle=false;
    check(real_set_next_power_state(&hw,FLEA_EVT_PAUSE)==BC_STS_SUCCESS &&
          pm_ledger.attempts==1 && pm_ledger.refused==1 &&
          pm_ledger.stop==CHD_PM_REFUSAL && !hw.FleaPowerSaved.valid && !hw.dma_fault,
          "actual MEMC refusal stops future attempts without inventing a fault");
    const unsigned reads=device.reads,writes=device.writes;
    for(unsigned i=0;i<10;i++)
        check(real_set_next_power_state(&hw,FLEA_EVT_CMD_COMP)==BC_STS_SUCCESS,
              "stopped physical experiment preserves ordinary accepted pause");
    check(pm_ledger.attempts==1 && device.reads==reads && device.writes==writes,
          "physical refusal never retries target registers");
    for(unsigned when=0;when<2;when++) {
        hw=research_setup(2,5000);
        if(when)research_enter(&hw);
        device.fail_fll_write=true;
        check(real_set_next_power_state(&hw,when?FLEA_EVT_RESUME:FLEA_EVT_PAUSE)==BC_STS_IO_ERROR &&
              hw.dma_fault && hw.FleaPowerState==FLEA_PS_FAULT &&
              pm_ledger.stop==CHD_PM_FAILURE && pm_ledger.fault_recorded &&
              !pm_ledger.fll_completed,
              "failed zero/restored FLL is retained as fault, never ACTIVE");
    }
    hw=research_setup(2,5000);
    research_enter(&hw);
    device.pll_sample_count=1;device.pll_samples[0]=0;
    check(real_set_next_power_state(&hw,FLEA_EVT_RESUME)==BC_STS_IO_ERROR &&
          hw.FleaPowerState==FLEA_PS_FAULT && hw.FleaPowerSaved.valid &&
          pm_ledger.phase==CHD_PM_UNCERTAIN && pm_ledger.up_attempts==1 &&
          !pm_ledger.up_completed && pm_ledger.fault_recorded,
          "actual PLL wake failure preserves snapshot and forbids false completion");
    const unsigned fault_reads=device.reads;
    check(real_set_next_power_state(&hw,FLEA_EVT_RESUME)==BC_STS_IO_ERROR &&
          device.reads==fault_reads && pm_ledger.up_attempts==1,
          "uncertain wake cannot silently retry");
    bool held;
    check(crystalhd_flea_cleanup_begin(&hw,&held)==BC_STS_IO_ERROR && !held,
          "ordinary cleanup cannot treat uncertain wake as ACTIVE");
}

static void generic_fault_stops_admission(void)
{
    struct crystalhd_hw hw=research_setup(2,5000);
    research_enter(&hw);
    crystalhd_hw_publish_fault(&hw);
    check(hw.dma_fault && pm_ledger.stop==CHD_PM_FAILURE && pm_ledger.fault_recorded &&
          pm_ledger.phase==CHD_PM_ASLEEP && hw.FleaPowerSaved.valid,
          "generic fault records the outstanding sleeping phase without pretending wake");
    const unsigned count=pm_ledger.count,reads=device.reads;
    crystalhd_hw_publish_fault(&hw);
    check(pm_ledger.count==count &&
          real_set_next_power_state(&hw,FLEA_EVT_RESUME)==BC_STS_IO_ERROR &&
          device.reads==reads && !pm_ledger.up_attempts,
          "repeated generic fault neither floods records nor authorizes recovery IO");
}

static const struct chd_pm_selection *observed(unsigned reason, unsigned mask)
{
    for(unsigned i=0;i<pm_observation.count;i++)
        if(pm_observation.selections[i].reason==reason &&
           pm_observation.selections[i].blockers==mask)
            return &pm_observation.selections[i];
    return NULL;
}

static void selection_observation(void)
{
    struct crystalhd_hw hw=research_setup(2,5000);
    for(unsigned i=0;i<1000;i++) {
        research_time++;
        check(real_set_next_power_state(&hw,FLEA_EVT_CMD_COMP)==BC_STS_SUCCESS,
              "no-intent completion remains a no-op");
    }
    const struct chd_pm_selection *r=observed(CHD_PM_SEL_NO_INTENT,0);
    check(r && r->count==1000 && r->context==1 && r->state==FLEA_PS_ACTIVE &&
          r->target==FLEA_PS_ACTIVE && r->first_ns<r->last_ns &&
          !pm_ledger.count && !pm_ledger.claimed && !device.reads,
          "repeated no-intent decisions aggregate without spending physical budget");
    hw.hw_pause_issued=true; hw.fwcmd_trans_active=true;
    check(real_set_next_power_state(&hw,FLEA_EVT_CMD_COMP)==BC_STS_SUCCESS &&
          observed(CHD_PM_SEL_PRODUCER,CHD_PM_BLOCK_FW_TRANS),
          "inflight producer suppression is separate from missing pause intent");
    hw.hw_pause_issued=false; hw.fwcmd_trans_active=false; hw.rx_list_sts[0]=rx_waiting_y_intr;
    check(real_set_next_power_state(&hw,FLEA_EVT_PAUSE)==BC_STS_SUCCESS &&
          hw.FleaPowerState==FLEA_PS_LP_PENDING &&
          observed(CHD_PM_SEL_DOWN_BUSY,CHD_PM_BLOCK_RX0) && !pm_ledger.count,
          "actual pause names busy RX and preserves accepted pending state");
    hw.rx_list_sts[0]=sts_free;
    research_enter(&hw);
    check(observed(CHD_PM_SEL_DOWN_READY,0) && observed(CHD_PM_SEL_SETTER,0),
          "selector readiness and final setter check remain separate observations");

    hw=research_setup(2,5000);
    bool held;
    check(crystalhd_flea_cleanup_begin(&hw,&held)==BC_STS_SUCCESS && held,
          "observer fixture obtains real cleanup hold");
    check(real_set_next_power_state(&hw,FLEA_EVT_PAUSE)==BC_STS_BUSY &&
          observed(CHD_PM_SEL_CLEANUP,0) && !pm_ledger.count,
          "cleanup rejection records its actual locked hold");
    crystalhd_flea_cleanup_end(&hw,held);
    hw.FleaPowerTransition=true; hw.FleaTxActive=1;
    check(private_set_power_state(&hw,FLEA_PS_LP_COMPLETE)==BC_STS_SUCCESS &&
          observed(CHD_PM_SEL_SETTER,CHD_PM_BLOCK_TX) && !pm_ledger.count &&
          !device.reads && hw.FleaPowerState==FLEA_PS_LP_PENDING,
          "final setter movement is observed without entering physical helper");
    hw.FleaPowerTransition=false;

    hw=research_setup(2,5000);
    for(unsigned i=1;i<CHD_PM_BANK_SELECTIONS;i++) {
        real_init_power_state(&hw);
        check(real_set_next_power_state(&hw,FLEA_EVT_START_DEVICE)==BC_STS_SUCCESS,
              "independent initialization observations fill bounded first-key table");
    }
    const struct chd_pm_selection first=pm_observation.selections[0];
    check(pm_observation.count==CHD_PM_BANK_SELECTIONS && !pm_observation.overflow &&
          pm_observation.routine_count==CHD_PM_BANK_SELECTIONS && !pm_observation.intent_count,
          "exact routine observation bank capacity is retained");
    real_init_power_state(&hw);
    check(real_set_next_power_state(&hw,FLEA_EVT_START_DEVICE)==BC_STS_SUCCESS &&
          pm_observation.overflow==1 &&
          !memcmp(&first,&pm_observation.selections[0],sizeof(first)),
          "overflow is counted and never overwrites first raw observation");
    research_enter(&hw);
    check(pm_ledger.attempts==1 && pm_ledger.entered==1 && !pm_ledger.dropped &&
          pm_observation.intent_count==2 && !pm_observation.intent_overflow &&
          pm_observation.routine_overflow==1 &&
          observed(CHD_PM_SEL_DOWN_READY,0)->bank==1 && observed(CHD_PM_SEL_SETTER,0)->bank==1,
          "full startup bank cannot consume pause observations or physical attempt ledger");
    pm_observation.calls=UINT64_MAX; pm_observation.overflow=UINT64_MAX;
    research_resume(&hw);
    check(pm_observation.calls==UINT64_MAX && pm_observation.overflow==UINT64_MAX &&
          pm_observation.saturated && pm_ledger.up_completed==1 && pm_ledger.fll_completed==1,
          "saturating observer counters never wrap or prevent ordinary wake");
}

static void full_profile_budget(void)
{
    struct crystalhd_hw hw = research_setup(CHD_PM_MAX_ATTEMPTS, CHD_PM_MAX_MS);
    for (unsigned i=0; i<CHD_PM_MAX_ATTEMPTS; ++i) {
        research_enter(&hw);
        research_time += 500000000ULL;
        research_resume(&hw);
        research_time += 1000000ULL;
    }
    const unsigned reads=device.reads, writes=device.writes;
    check(real_set_next_power_state(&hw,FLEA_EVT_PAUSE)==BC_STS_SUCCESS &&
          pm_ledger.stop==CHD_PM_LIMIT && pm_ledger.attempts==CHD_PM_MAX_ATTEMPTS &&
          pm_ledger.up_completed==CHD_PM_MAX_ATTEMPTS &&
          pm_ledger.fll_completed==CHD_PM_MAX_ATTEMPTS &&
          device.reads==reads && device.writes==writes && !pm_ledger.dropped,
          "complete profile budget retains each wake/FLL and blocks further physical IO");
    crystalhd_pm_research_retire(&hw);
    check(pm_ledger.retired && pm_ledger.count==5*CHD_PM_MAX_ATTEMPTS+3 &&
          pm_ledger.count<CHD_PM_RECORDS && !pm_ledger.dropped,
          "full profile and terminal retirement fit the fixed receipt array");
}

int main(void)
{
    full_profile_budget();
    selection_observation();
    limits_and_claim();
    finite_cycles();
    deadline_and_capacity();
    moving_owners_and_cleanup();
    refusal_and_faults();
    generic_fault_stops_admission();
    printf("PASS: %u finite-PM fixtures; actual ledger and selector/down/up/FLL; mocked MMIO only\n",
           research_cases);
    return 0;
}
