// SPDX-License-Identifier: GPL-2.0-or-later
/* Actual Flea admission, WRAP request and mailbox publication; mock only
 * hardware I/O and IRQ boundaries. Does not emulate firmware execution.
 */
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "bc_dts_types.h"
struct _BC_DTS_PROC_OUT;
#include "bc_dts_defs.h"
#include "flea/DriverFwShare.h"
#define TX_WRAP_THRESHOLD (128 * 1024)
#define BC_PCI_DEVID_FLEA 0x1615
#define FLEA_PS_ACTIVE 1
#define READ_ONCE(x) (x)
#define printk(...) ((void)0)
#define dev_dbg(...) ((void)0)
#define lockdep_assert_held(lock) assert(*(lock))
struct pci_dev { unsigned device, irq; };
struct crystalhd_adp { struct pci_dev *pdev; bool present; int user_lock, tx_lock; };
struct crystalhd_hw {
 struct crystalhd_adp *adp;
 bool dma_fault, WakeUpDecodeDone, SingleThreadAppFIFOEmpty;
 unsigned FleaPowerState, EmptyCnt, TxBuffInfoAddr;
 int fetch_sem;
 TX_INPUT_BUFFER_INFO TxFwInputBuffInfo;
 BC_STATUS (*pfnDevDRAMRead)(struct crystalhd_hw *, uint32_t, uint32_t, uint32_t *);
 BC_STATUS (*pfnDevDRAMWrite)(struct crystalhd_hw *, uint32_t, uint32_t, const uint32_t *);
};
static bool irq_disabled, inject_mailbox, pending_mailbox, checking_native;
static unsigned reads, writes, wakes, checks;
static BC_STATUS read_status, write_status;
static TX_INPUT_BUFFER_INFO firmware;
static struct crystalhd_hw *current;
static void down(int *sem) { assert(!*sem && !irq_disabled); *sem = 1; }
static void up(int *sem) { assert(*sem && !irq_disabled); *sem = 0; }
void crystalhd_flea_update_tx_buff_info(struct crystalhd_hw *hw);
static void disable_irq(unsigned irq) { assert(irq == 7 && !irq_disabled); irq_disabled = true; }
static void enable_irq(unsigned irq)
{
 assert(irq == 7 && irq_disabled);
 irq_disabled = false;
 if (pending_mailbox) {
  pending_mailbox = false;
  assert(!current->TxFwInputBuffInfo.DramBuffAdd);
  firmware.DramBuffAdd = 0x200000;
  firmware.DramBuffSzInBytes = 2 * 1024 * 1024;
  firmware.Flags = 0;
  crystalhd_flea_update_tx_buff_info(current);
 }
}
static bool crystalhd_flea_wake_up_hw(struct crystalhd_hw *hw)
{ wakes++; hw->WakeUpDecodeDone = true; return true; }
static BC_STATUS Read(struct crystalhd_hw *hw, uint32_t address, uint32_t words, uint32_t *out)
{
 reads++;
 if (words == 1) {
  assert(address == hw->TxBuffInfoAddr + offsetof(TX_INPUT_BUFFER_INFO, Flags));
  if (checking_native) assert(irq_disabled && hw->fetch_sem);
  if (read_status != BC_STS_SUCCESS) return read_status;
  *out = firmware.Flags;
 } else {
  assert(!irq_disabled && address == hw->TxBuffInfoAddr);
  assert(words == (sizeof(firmware) - sizeof(firmware.Reserved)) / 4);
  memcpy(out, &firmware, words * 4);
 }
 return BC_STS_SUCCESS;
}
static BC_STATUS Write(struct crystalhd_hw *hw, uint32_t address, uint32_t words, const uint32_t *in)
{
 assert(words == 1 && address == hw->TxBuffInfoAddr + offsetof(TX_INPUT_BUFFER_INFO, Flags));
 if (checking_native) assert(irq_disabled && hw->fetch_sem);
 writes++;
 if (write_status != BC_STS_SUCCESS) return write_status;
 firmware.Flags = *in;
 if (inject_mailbox) {
  assert(irq_disabled); /* IRQ publication cannot interleave before cache clear. */
  pending_mailbox = true;
 }
 return BC_STS_SUCCESS;
}
#include "production.h"
static void Check(bool ok) { checks++; assert(ok); }
static void Setup(struct crystalhd_hw *hw, struct crystalhd_adp *adp, struct pci_dev *pci)
{
 *pci = (struct pci_dev){ BC_PCI_DEVID_FLEA, 7 };
 *adp = (struct crystalhd_adp){ pci, true, 1, 1 };
 *hw = (struct crystalhd_hw){ .adp = adp, .WakeUpDecodeDone = true,
  .FleaPowerState = FLEA_PS_ACTIVE, .TxBuffInfoAddr = 0x1000,
  .pfnDevDRAMRead = Read, .pfnDevDRAMWrite = Write };
 firmware = (TX_INPUT_BUFFER_INFO){ .DramBuffAdd = 0x100000,
  .DramBuffSzInBytes = 32768, .Flags = 0x80, .SeqNum = 173 };
 hw->TxFwInputBuffInfo = firmware;
 current = hw;
 reads = writes = wakes = 0;
 irq_disabled = inject_mailbox = pending_mailbox = false;
 checking_native = true;
 read_status = write_status = BC_STS_SUCCESS;
}
int main(void)
{
 struct crystalhd_hw hw;
 struct crystalhd_adp adp;
 struct pci_dev pci;
 uint32_t empty;
 uint8_t flags;
 Setup(&hw, &adp, &pci);
 for (unsigned i = 0; i < 3; i++) {
  flags = i == 2 ? 0x08 : 0;
  Check(crystalhd_flea_check_input_full(&hw, 65524, &empty, false, &flags));
 }
 Check(!reads && !writes && hw.TxFwInputBuffInfo.DramBuffSzInBytes == 32768);
 Check(crystalhd_flea_request_tx_wrap(&hw, 65524) == BC_STS_SUCCESS);
 Check(reads == 1 && writes == 1 && firmware.Flags == (0x80 | DFW_FLAGS_WRAP));
 Check(!hw.TxFwInputBuffInfo.DramBuffAdd && !hw.TxFwInputBuffInfo.DramBuffSzInBytes && !irq_disabled);
 Check(crystalhd_flea_request_tx_wrap(&hw, 65524) == BC_STS_NO_DATA && writes == 1);
 Setup(&hw, &adp, &pci);
 inject_mailbox = true;
 Check(crystalhd_flea_request_tx_wrap(&hw, 65524) == BC_STS_SUCCESS);
 Check(hw.TxFwInputBuffInfo.DramBuffAdd == 0x200000 && hw.TxFwInputBuffInfo.DramBuffSzInBytes == 2 * 1024 * 1024);
 flags = 0;
 Check(!crystalhd_flea_check_input_full(&hw, 65524, &empty, false, &flags) && (flags & 0x80));
 for (unsigned c = 0; c < 13; c++) {
  Setup(&hw, &adp, &pci);
  if (c == 0) pci.device = 0x1612;
  if (c == 1) hw.WakeUpDecodeDone = false;
  if (c == 2) hw.FleaPowerState = 0;
  if (c == 3) hw.TxFwInputBuffInfo.Flags |= DFW_FLAGS_TX_ABORT;
  if (c == 4) hw.TxFwInputBuffInfo.DramBuffAdd = 0;
  if (c == 5) hw.TxFwInputBuffInfo.DramBuffAdd++;
  if (c == 6) hw.TxFwInputBuffInfo.DramBuffSzInBytes = 0;
  if (c == 7) hw.TxFwInputBuffInfo.DramBuffSzInBytes = 65524;
  if (c == 8) hw.TxBuffInfoAddr = 0;
  if (c == 9) hw.TxBuffInfoAddr++;
  if (c == 10) hw.TxBuffInfoAddr = UINT32_MAX - 3;
  Check(crystalhd_flea_request_tx_wrap(&hw, c == 11 ? 0 : c == 12 ? 65527 : 65524) == BC_STS_NO_DATA);
  Check(!reads && !writes && !wakes && !irq_disabled);
 }
 for (unsigned c = 0; c < 5; c++) {
  Setup(&hw, &adp, &pci);
  if (c == 0) hw.dma_fault = true;
  if (c == 1) adp.present = false;
  if (c == 2) firmware.Flags |= DFW_FLAGS_TX_ABORT;
  if (c == 3) read_status = BC_STS_IO_ERROR;
  if (c == 4) write_status = BC_STS_IO_ERROR;
  Check(crystalhd_flea_request_tx_wrap(&hw, 65524) == BC_STS_IO_ERROR);
  Check(hw.TxFwInputBuffInfo.DramBuffAdd == 0x100000 && !irq_disabled);
 }
 Setup(&hw, &adp, &pci);
 checking_native = false;
 flags = 0x0c;
 Check(crystalhd_flea_check_input_full(&hw, 0, &empty, false, &flags));
 Check(writes == 1 && (firmware.Flags & DFW_FLAGS_WRAP) && !hw.TxFwInputBuffInfo.DramBuffAdd);
 printf("Typed TX wrap: %u checks passed\n", checks);
 return 0;
}
