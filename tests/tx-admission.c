/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Source-extracted command and hardware TX paths with deterministic IRQ and
 * FIFO boundaries. PCI suspend/release cannot interleave with a live input
 * ioctl: user_lock excludes them; tx_lock excludes a second input ioctl.
 * Flush may cancel a BUSY retry. It is not a persistent admission latch.
 */
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "bc_dts_types.h"
struct _BC_DTS_PROC_OUT;
#include "bc_dts_defs.h"
#include "tx-admission-types.h"

#define KERN_ERR ""
#define READ_ONCE(value) (value)
#define WRITE_ONCE(value, next) ((value) = (next))
#define printk(...) ((void)0)
#define dev_err(dev, ...) ((void)(dev))
#define dev_dbg(dev, ...) ((void)(dev))
#define eCMD_C011_CMD_BASE 0x73763000U
#define eCMD_C011_DEC_CHAN_FLUSH (eCMD_C011_CMD_BASE + 0x104U)
#define eCMD_C011_DEC_CHAN_PAUSE (eCMD_C011_CMD_BASE + 0x11dU)
typedef struct { unsigned wakeups; } wait_queue_head_t;
typedef union { uint64_t full_addr; } addr_64;
struct device { int unused; };
struct pci_dev { struct device dev; int irq; };
struct crystalhd_adp { struct pci_dev *pdev; bool present; };
struct crystalhd_dio_req {
    struct { uint32_t xfr_len; int comp_sts, ev_sts; } uinfo;
};
typedef void (*hw_comp_callback)(struct crystalhd_dio_req *, wait_queue_head_t *, BC_STATUS);
struct dma_desc_mem { uint64_t phy_addr; };
struct tx_dma_pkt {
    struct dma_desc_mem desc_mem;
    hw_comp_callback call_back;
    struct crystalhd_dio_req *dio_req;
    wait_queue_head_t *cb_event;
    uint32_t list_tag;
};
struct crystalhd_dioq { struct tx_dma_pkt *packet; };
typedef struct { uint32_t cmd[64]; } BC_FW_CMD;
struct crystalhd_hw {
    struct crystalhd_adp *adp;
    bool dma_fault;
    unsigned lock;
    struct crystalhd_dioq *tx_freeq, *tx_actq;
    struct { unsigned cin_busy; } stats;
    struct { uint32_t DramBuffAdd, HostXferSzInBytes; } TxFwInputBuffInfo;
    uint32_t tx_list_post_index, tx_ioq_tag_seed;
    enum LIST_STATUS TxList0Sts, TxList1Sts;
    bool (*pfnCheckInputFIFO)(struct crystalhd_hw *, uint32_t, uint32_t *, bool, uint8_t *);
    void (*pfnStartTxDMA)(struct crystalhd_hw *, uint8_t, addr_64);
    BC_STATUS (*pfnStopTxDMA)(struct crystalhd_hw *);
    BC_STATUS (*pfnDoFirmwareCmd)(struct crystalhd_hw *, BC_FW_CMD *);
    BC_STATUS (*pfnIssuePause)(struct crystalhd_hw *, bool);
    int fetch_sem;
};
struct crystalhd_cmd {
    uint32_t state, tx_list_id, cin_wait_exit;
    struct crystalhd_adp *adp;
    struct crystalhd_hw *hw_ctx;
};
typedef struct {
    struct { union {
        struct { void *pDmaBuff; uint32_t BuffSz; uint8_t Encrypted; } ProcInput;
        BC_FW_CMD fwCmd;
    } u; } udata;
} crystalhd_ioctl_data;

static unsigned checks, failures;
static struct pci_dev endpoint = { .irq = 19 };
static struct crystalhd_adp adapter;
static struct crystalhd_hw hardware;
static struct crystalhd_cmd context;
static struct crystalhd_dio_req request;
static struct tx_dma_pkt packet;
static struct crystalhd_dioq freeq, activeq;
static uint32_t input[16];
static crystalhd_ioctl_data input_ioctl;
static struct {
    unsigned maps, unmaps, descriptors, starts, stops, syncs, sleeps, waits;
    unsigned wakes, irq_depth, irq_disables, irq_enables, fifo_calls, busy;
    unsigned firmware_calls, firmware_depth, bus_clears, bus_drains;
    int wait_result, sleep_result;
    bool mapped, immediate_completion, completion_before_timeout, flush_on_busy;
    bool drain_ok;
    BC_STATUS map_status, descriptor_status, active_add_status, stop_status;
    BC_STATUS completion_status, firmware_status;
    uint8_t seen_flags;
    uint32_t seen_destination;
} run;

static void Check(bool condition, const char *why)
{
    checks++;
    if (!condition) { failures++; fprintf(stderr, "FAIL: %s\n", why); }
}
static struct device *chddev(void) { return &endpoint.dev; }
static BC_STATUS bc_cproc_do_fw_cmd(struct crystalhd_cmd *, crystalhd_ioctl_data *);
static BC_STATUS crystalhd_hw_tx_req_complete(struct crystalhd_hw *, uint32_t, BC_STATUS);
static void crystalhd_hw_dma_fatal_stop(struct crystalhd_hw *);

static void Complete(void)
{
    Check(activeq.packet == &packet && packet.list_tag != 0,
          "IRQ completion finds the published TX owner");
    Check(crystalhd_hw_tx_req_complete(&hardware, packet.list_tag,
                                      run.completion_status) == BC_STS_SUCCESS,
          "IRQ completion retires the real active request");
}
static void Unlock(unsigned *lock)
{
    Check(lock == &hardware.lock && *lock == 1, "TX publication releases its spinlock");
    *lock = 0;
    if (run.immediate_completion && activeq.packet) {
        run.immediate_completion = false;
        Complete();
    }
}
#define spin_lock_irqsave(lock, flags) do { \
    (flags) = 0; Check(*(lock) == 0, "TX publication acquires a free spinlock"); \
    *(lock) = 1; \
} while (0)
#define spin_unlock_irqrestore(lock, flags) do { (void)(flags); Unlock(lock); } while (0)
#define crystalhd_create_event(event) (*(event) = (wait_queue_head_t){0})
static void crystalhd_set_event(wait_queue_head_t *event)
{
    Check(event && packet.dio_req == &request,
          "completion wakes the event while the request still belongs to the callback");
    event->wakeups++; run.wakes++;
}
static int Wait(wait_queue_head_t *event, int condition, unsigned timeout)
{
    if (timeout == 100) {
        Check(!condition && !event->wakeups, "FIFO retry uses a separate idle event");
        run.sleeps++;
        return run.sleep_result;
    }
    Check(timeout == 3000 && context.tx_list_id != 0,
          "submitted TX waits with its published cancellation tag");
    run.waits++;
    if (run.wait_result && !run.completion_before_timeout)
        return run.wait_result;
    if (!condition)
        Complete();
    Check(request.uinfo.ev_sts && event->wakeups == 1,
          "normal completion signals the submitted request exactly once");
    return run.wait_result;
}
#define crystalhd_wait_on_event(event, condition, timeout, result, nosig) do { \
    (void)(nosig); (result) = Wait(event, condition, timeout); \
} while (0)
static void synchronize_irq(int irq)
{
    Check(irq == endpoint.irq && !hardware.lock && !activeq.packet && run.mapped,
          "successful completion drains the ISR before DMA unmap and stack-event return");
    run.syncs++;
}
static void disable_irq(int irq)
{
    Check(irq == endpoint.irq && !hardware.lock && !run.irq_depth,
          "cancellation drains IRQ callbacks without holding the hardware spinlock");
    run.irq_depth++; run.irq_disables++;
}
static void enable_irq(int irq)
{
    Check(irq == endpoint.irq && run.irq_depth == 1 && !activeq.packet,
          "cancellation reenables IRQ after request ownership is retired");
    run.irq_depth--; run.irq_enables++;
}
static void pci_clear_master(struct pci_dev *pdev)
{
    Check(pdev == &endpoint && hardware.dma_fault,
          "failed DMA stop poisons admission before revoking bus mastering");
    run.bus_clears++;
}
static bool pci_wait_for_pending_transaction(struct pci_dev *pdev)
{
    Check(pdev == &endpoint && run.bus_clears, "fatal stop drains outstanding PCI transactions");
    run.bus_drains++;
    return run.drain_ok;
}
static void *crystalhd_dioq_fetch(struct crystalhd_dioq *queue)
{
    struct tx_dma_pkt *owned = queue->packet;
    Check(queue == &freeq, "submission takes a packet from the free queue");
    queue->packet = NULL;
    return owned;
}
static void *crystalhd_dioq_find_and_fetch(struct crystalhd_dioq *queue, uint32_t tag)
{
    struct tx_dma_pkt *owned = queue->packet;
    Check(queue == &activeq, "completion searches the active ownership queue");
    if (!owned || owned->list_tag != tag)
        return NULL;
    queue->packet = NULL;
    return owned;
}
static BC_STATUS crystalhd_dioq_add(struct crystalhd_dioq *queue,
                                  struct tx_dma_pkt *owned, bool wake, uint32_t tag)
{
    Check(owned == &packet && !queue->packet && !wake,
          "a packet is returned to exactly one queue");
    if (queue == &activeq) {
        Check(hardware.lock && tag == owned->list_tag && tag,
              "active ownership is published under lock before DMA can start");
        if (run.active_add_status != BC_STS_SUCCESS)
            return run.active_add_status;
    } else {
        Check(queue == &freeq && !tag && !owned->dio_req && !owned->cb_event &&
              !owned->call_back && !owned->list_tag,
              "free packets retain no request, callback, event or tag ownership");
    }
    queue->packet = owned;
    return BC_STS_SUCCESS;
}
static BC_STATUS crystalhd_xlat_sgl_to_dma_desc(struct crystalhd_dio_req *dio,
        struct dma_desc_mem *desc, uint32_t *index, struct device *dev, uint32_t destination)
{
    Check(dio == &request && desc == &packet.desc_mem && index && dev == &endpoint.dev,
          "TX descriptor construction uses its request and reserved packet");
    run.descriptors++; run.seen_destination = destination;
    return run.descriptor_status;
}
static BC_STATUS crystalhd_map_dio(struct crystalhd_adp *adp, void *bytes, uint32_t size,
        uint32_t offset, bool packed, bool tx, struct crystalhd_dio_req **dio)
{
    Check(adp == &adapter && bytes == input && size == sizeof(input) && !offset &&
          !packed && tx && !run.mapped, "input mapping owns the submitted buffer once");
    run.maps++;
    if (run.map_status != BC_STS_SUCCESS)
        return run.map_status;
    run.mapped = true;
    request = (struct crystalhd_dio_req){ .uinfo.xfr_len = size };
    *dio = &request;
    return BC_STS_SUCCESS;
}
static void crystalhd_unmap_dio(struct crystalhd_adp *adp, struct crystalhd_dio_req *dio)
{
    Check(adp == &adapter && dio == &request && run.mapped && !activeq.packet &&
          !packet.dio_req && !packet.cb_event && !packet.call_back && !context.tx_list_id,
          "input unmaps exactly once after all TX/callback ownership has retired");
    if (run.starts)
        Check(run.syncs || run.irq_disables,
              "DMA ownership is not unmapped before completion or cancellation drains IRQ");
    run.mapped = false; run.unmaps++;
}
static BC_STATUS crystalhd_hw_fw_cmd_enter(struct crystalhd_hw *hw)
{
    Check(hw == &hardware && !run.firmware_depth, "flush enters firmware serialization");
    run.firmware_depth++;
    return BC_STS_SUCCESS;
}
static void crystalhd_hw_fw_cmd_leave(struct crystalhd_hw *hw)
{
    Check(hw == &hardware && run.firmware_depth == 1, "flush releases firmware serialization");
    run.firmware_depth--;
}
static int down_interruptible(int *sem) { Check(*sem == 1, "capture semaphore is free"); *sem = 0; return 0; }
static void down(int *sem) { (void)down_interruptible(sem); }
static void up(int *sem) { Check(*sem == 0, "capture semaphore is held"); *sem = 1; }

#include "tx-admission-hardware.h"
#include "tx-admission-command.h"

static BC_STATUS Flush(bool cancel)
{
    crystalhd_ioctl_data command = {0};
    command.udata.u.fwCmd.cmd[0] = eCMD_C011_DEC_CHAN_FLUSH;
    command.udata.u.fwCmd.cmd[3] = cancel;
    return bc_cproc_do_fw_cmd(&context, &command);
}
static bool Fifo(struct crystalhd_hw *hw, uint32_t size, uint32_t *index,
                 bool update, uint8_t *flags)
{
    Check(hw == &hardware && size == sizeof(input) && index && !update,
          "pre-submit FIFO admission receives the whole mapped input");
    run.fifo_calls++; run.seen_flags = *flags;
    if (run.busy) {
        run.busy--;
        if (run.flush_on_busy) {
            run.flush_on_busy = false;
            Check(Flush(true) == run.firmware_status,
                  "a concurrent firmware flush publishes cancellation before BUSY retry");
        }
        return true;
    }
    return false;
}
static void Start(struct crystalhd_hw *hw, uint8_t index, addr_64 descriptor)
{
    Check(hw == &hardware && hardware.lock && activeq.packet == &packet &&
          packet.dio_req == &request && packet.call_back && packet.cb_event &&
          packet.list_tag == hardware.tx_ioq_tag_seed + index &&
          descriptor.full_addr == packet.desc_mem.phy_addr &&
          hw->TxFwInputBuffInfo.HostXferSzInBytes == sizeof(input),
          "DMA starts only after request, callback, tag and length publication");
    run.starts++;
}
static BC_STATUS Stop(struct crystalhd_hw *hw)
{
    Check(hw == &hardware && run.irq_depth && !hardware.lock && run.mapped,
          "cancel stops DMA with IRQ drained, buffer still mapped and no spinlock held");
    run.stops++;
    return run.stop_status;
}
static BC_STATUS Firmware(struct crystalhd_hw *hw, BC_FW_CMD *command)
{
    Check(hw == &hardware && run.firmware_depth == 1 &&
          command->cmd[0] == eCMD_C011_DEC_CHAN_FLUSH,
          "the real flush command reaches the serialized firmware boundary");
    run.firmware_calls++;
    return run.firmware_status;
}
static BC_STATUS Pause(struct crystalhd_hw *hw, bool pause)
{
    (void)hw; (void)pause;
    Check(false, "TX-only tests never enter the capture pause path");
    return BC_STS_ERROR;
}
static void Reset(void)
{
    memset(&run, 0, sizeof(run));
    memset(&request, 0, sizeof(request));
    packet = (struct tx_dma_pkt){ .desc_mem.phy_addr = 0x1000 };
    freeq.packet = &packet; activeq.packet = NULL;
    adapter = (struct crystalhd_adp){ .pdev = &endpoint, .present = true };
    hardware = (struct crystalhd_hw){ .adp = &adapter,
        .tx_freeq = &freeq, .tx_actq = &activeq, .tx_ioq_tag_seed = 0x100,
        .pfnCheckInputFIFO = Fifo, .pfnStartTxDMA = Start, .pfnStopTxDMA = Stop,
        .pfnDoFirmwareCmd = Firmware, .pfnIssuePause = Pause, .fetch_sem = 1,
        .TxFwInputBuffInfo.DramBuffAdd = 0x8000 };
    context = (struct crystalhd_cmd){ .state = BC_LINK_READY, .adp = &adapter, .hw_ctx = &hardware };
    input_ioctl = (crystalhd_ioctl_data){ .udata.u.ProcInput = {
        .pDmaBuff = input, .BuffSz = sizeof(input), .Encrypted = 0x80 } };
    run.drain_ok = true;
}
static void Balanced(void)
{
    Check(!run.mapped && !activeq.packet && freeq.packet == &packet &&
          !context.tx_list_id && !hardware.lock && !run.irq_depth && !run.firmware_depth &&
          !packet.dio_req && !packet.call_back && !packet.cb_event && !packet.list_tag,
          "completed input leaves balanced mapping, packet, locks and callback ownership");
}
static void Admission(void)
{
    wait_queue_head_t event = {0};
    uint32_t tag = 0;

    Reset();
    Check(bc_cproc_proc_input(NULL, &input_ioctl) == BC_STS_INV_ARG &&
          bc_cproc_proc_input(&context, NULL) == BC_STS_INV_ARG,
          "invalid command arguments are rejected before mapping");
    context.state = BC_LINK_SUSPEND;
    Check(bc_cproc_proc_input(&context, &input_ioctl) == BC_STS_PWR_MGMT && !run.maps,
          "suspended input is rejected before DMA mapping");
    context.state = BC_LINK_READY;
    input_ioctl.udata.u.ProcInput.pDmaBuff = (unsigned char *)input + 1;
    Check(bc_cproc_proc_input(&context, &input_ioctl) == BC_STS_NOT_IMPL && !run.maps,
          "unaligned input is rejected before DMA mapping");
    input_ioctl.udata.u.ProcInput.pDmaBuff = input;
    input_ioctl.udata.u.ProcInput.BuffSz = 0;
    Check(bc_cproc_proc_input(&context, &input_ioctl) == BC_STS_INV_ARG && !run.maps,
          "empty input is rejected before DMA mapping");
    Check(crystalhd_hw_post_tx(NULL, &request, bc_proc_in_completion,
                              &event, &tag, 0) == BC_STS_INV_ARG &&
          crystalhd_hw_post_tx(&hardware, NULL, bc_proc_in_completion,
                              &event, &tag, 0) == BC_STS_INV_ARG &&
          crystalhd_hw_post_tx(&hardware, &request, NULL,
                              &event, &tag, 0) == BC_STS_INV_ARG &&
          crystalhd_hw_post_tx(&hardware, &request, bc_proc_in_completion,
                              NULL, &tag, 0) == BC_STS_INV_ARG &&
          crystalhd_hw_post_tx(&hardware, &request, bc_proc_in_completion,
                              &event, NULL, 0) == BC_STS_INV_ARG && !run.fifo_calls,
          "incomplete hardware admission arguments cannot reach FIFO or packet ownership");
    Balanced();

    Reset(); run.map_status = BC_STS_INSUFF_RES;
    Check(bc_cproc_proc_input(&context, &input_ioctl) == BC_STS_INSUFF_RES &&
          run.maps == 1 && !run.unmaps && !run.fifo_calls,
          "failed mapping is propagated without unmapping an unowned request");
    Balanced();

    Reset(); hardware.dma_fault = true;
    Check(bc_cproc_proc_input(&context, &input_ioctl) == BC_STS_IO_ERROR &&
          run.maps == 1 && run.unmaps == 1 && !run.fifo_calls && !run.starts,
          "a fatal DMA latch rejects TX before checking FIFO or starting hardware");
    Balanced();
}
static void Completion(void)
{
    for (unsigned immediate = 0; immediate < 2; immediate++) {
        Reset(); run.immediate_completion = immediate;
        Check(bc_cproc_proc_input(&context, &input_ioctl) == BC_STS_SUCCESS &&
              run.starts == 1 && run.syncs == 1 && run.wakes == 1 && run.unmaps == 1 &&
              run.seen_flags == 0x80 && run.seen_destination == 0x8000,
              "normal and immediate IRQ completions preserve flags and retire before unmap");
        Check(crystalhd_hw_tx_req_complete(&hardware, 0x100, BC_STS_SUCCESS) == BC_STS_NO_DATA &&
              run.wakes == 1, "a duplicate IRQ cannot callback into a retired stack event");
        Balanced();
    }
    Reset(); run.completion_status = BC_STS_IO_ERROR;
    Check(bc_cproc_proc_input(&context, &input_ioctl) == BC_STS_IO_ERROR && run.syncs == 1,
          "a completed hardware error is propagated after IRQ retirement");
    Balanced();

    Reset();
    for (unsigned i = 0; i < 3; i++) {
        Check(bc_cproc_proc_input(&context, &input_ioctl) == BC_STS_SUCCESS &&
              hardware.tx_list_post_index == (i + 1) % DMA_ENGINE_CNT,
              "successive inputs alternate DMA list tags and safely reuse the freed packet");
        Balanced();
    }
}
static void Rollback(void)
{
    Reset(); freeq.packet = NULL;
    Check(bc_cproc_proc_input(&context, &input_ioctl) == BC_STS_INSUFF_RES &&
          !run.descriptors && !run.starts && run.unmaps == 1,
          "an exhausted packet pool rejects input without building or starting DMA");
    freeq.packet = &packet;
    Balanced();

    Reset(); run.descriptor_status = BC_STS_NOT_IMPL;
    Check(bc_cproc_proc_input(&context, &input_ioctl) == BC_STS_NOT_IMPL && !run.starts,
          "descriptor failure returns the free packet without starting DMA");
    Balanced();
    Reset(); run.active_add_status = BC_STS_INSUFF_RES;
    Check(bc_cproc_proc_input(&context, &input_ioctl) == BC_STS_INSUFF_RES && !run.starts,
          "active-queue failure clears callback ownership before returning the packet");
    Balanced();
}
static void BusyAndFlush(void)
{
    Reset(); run.busy = 2;
    Check(bc_cproc_proc_input(&context, &input_ioctl) == BC_STS_SUCCESS &&
          run.sleeps == 2 && run.fifo_calls == 3 && hardware.stats.cin_busy == 2 &&
          run.descriptors == 1 && run.starts == 1,
          "BUSY retries retain one mapping and allocate descriptors only after admission");
    Balanced();
    Reset(); run.busy = 1; run.sleep_result = -EINTR;
    Check(bc_cproc_proc_input(&context, &input_ioctl) == BC_STS_IO_USER_ABORT &&
          !run.starts && run.unmaps == 1, "a signal during BUSY wait aborts without posting DMA");
    Balanced();
    for (unsigned rejected = 0; rejected < 2; rejected++) {
        Reset(); run.busy = 1; run.flush_on_busy = true;
        run.firmware_status = rejected ? BC_STS_TIMEOUT : BC_STS_SUCCESS;
        Check(bc_cproc_proc_input(&context, &input_ioctl) == BC_STS_CMD_CANCELLED &&
              run.firmware_calls == 1 && !run.sleeps && !run.starts && !context.cin_wait_exit,
              "flush cancels one BUSY input retry even if firmware reports failure");
        Balanced();
        Check(bc_cproc_proc_input(&context, &input_ioctl) == BC_STS_SUCCESS,
              "the consumed flush flag does not permanently reject a subsequent input");
        Balanced();
    }
    Reset(); run.busy = 1;
    Check(Flush(false) == BC_STS_SUCCESS && !context.cin_wait_exit &&
          bc_cproc_proc_input(&context, &input_ioctl) == BC_STS_SUCCESS,
          "non-cancelling flush preserves normal BUSY retry admission");
    Balanced();
    Reset();
    Check(Flush(true) == BC_STS_SUCCESS && context.cin_wait_exit &&
          bc_cproc_proc_input(&context, &input_ioctl) == BC_STS_SUCCESS &&
          !context.cin_wait_exit && !run.sleeps,
          "a cancelling flush flag applies to BUSY retry, not immediately admissible input");
    Balanced();
    Reset(); context.cin_wait_exit = 1; context.state = BC_LINK_SUSPEND;
    Check(bc_cproc_codein_sleep(&context) == BC_STS_PWR_MGMT && context.cin_wait_exit,
          "suspended retry reports PM before consuming the one-shot flush flag");
    adapter.present = false;
    Check(bc_cproc_codein_sleep(&context) == BC_STS_CMD_CANCELLED &&
          bc_cproc_codein_sleep(&context) == BC_STS_CMD_CANCELLED && !run.sleeps,
          "removal cancels every retry even if another caller consumed a flush flag");
    Balanced();
}
static void Cancellation(void)
{
    const int wait_errors[] = {-EBUSY, -EINTR, -EIO};
    const BC_STATUS expected[] = {BC_STS_TIMEOUT, BC_STS_IO_USER_ABORT, BC_STS_IO_ERROR};
    for (unsigned i = 0; i < sizeof(wait_errors) / sizeof(wait_errors[0]); i++) {
        Reset(); run.wait_result = wait_errors[i];
        Check(bc_cproc_proc_input(&context, &input_ioctl) == expected[i] &&
              run.stops == 1 && run.irq_disables == 1 && run.irq_enables == 1 &&
              !run.wakes && !run.syncs && run.unmaps == 1,
              "timeout, signal and wait errors cancel once before returning the mapped input");
        Balanced();
    }
    Reset(); run.wait_result = -EBUSY; run.completion_before_timeout = true;
    Check(bc_cproc_proc_input(&context, &input_ioctl) == BC_STS_TIMEOUT &&
          run.wakes == 1 && run.stops == 1 && run.unmaps == 1,
          "IRQ winning a timeout cancellation still releases request ownership exactly once");
    Balanced();
    for (unsigned drain = 0; drain < 2; drain++) {
        Reset(); run.wait_result = -EBUSY; run.stop_status = BC_STS_IO_ERROR;
        run.drain_ok = drain;
        Check(bc_cproc_proc_input(&context, &input_ioctl) == BC_STS_TIMEOUT &&
              hardware.dma_fault && run.bus_clears == 1 && run.bus_drains == 1 &&
              run.unmaps == 1, "failed stop revokes DMA before returning the original timeout");
        Balanced();
        Check(bc_cproc_proc_input(&context, &input_ioctl) == BC_STS_IO_ERROR &&
              run.starts == 1 && run.maps == 2 && run.unmaps == 2,
              "fatal stop rejects subsequent DMA admission until session recovery");
        Balanced();
    }
}
int main(void)
{
    Admission(); Completion(); Rollback(); BusyAndFlush(); Cancellation();
    printf("TX admission: %u checks, %u failures\n", checks, failures);
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
