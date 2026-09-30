/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Execute the production Flea and Link firmware download functions with all
 * hardware accesses mocked. No module is loaded and no device is opened.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bc_dts_types.h"
struct _BC_DTS_PROC_OUT;
#include "bc_dts_defs.h"
#include "crystalhd_ioctl_limits.h"

#include "FleaDefs.h"
#include "bcm_70012_regs.h"

#ifndef FLEA_BLOB_SIZE
#error FLEA_BLOB_SIZE is required
#endif
#ifndef LINK_BLOB_SIZE
#error LINK_BLOB_SIZE is required
#endif

#define KERN_ERR ""
#define printk(...) ((void)0)
#define dev_dbg(dev, ...) ((void)(dev))
#define dev_err(dev, ...) ((void)(dev))
#define dev_info(dev, ...) ((void)(dev))
#define cpu_to_be32(value) __builtin_bswap32(value)
#define msleep_interruptible(milliseconds) test_sleep(milliseconds)

#define HOST_TO_FW_PIC_DEL_INFO_ADDR 0x400U
#define HOST_TO_FW_FLL_ADDR 0x500U
#define TS_Host2CpuSnd 0x00000100U
#define Hst2CpuMbx1 0x00100f00U
#define Cpu2HstMbx1 0x00100f04U
#define BC_FWIMG_ST_ADDR 0U

struct device { int unused; };
struct pci_dev { struct device dev; };
struct crystalhd_adp { struct pci_dev *pdev; };
struct crystalhd_hw {
	struct crystalhd_adp *adp;
	uint32_t (*pfnReadDevRegister)(struct crystalhd_adp *, uint32_t);
	void (*pfnWriteDevRegister)(struct crystalhd_adp *, uint32_t, uint32_t);
	uint32_t (*pfnReadFPGARegister)(struct crystalhd_adp *, uint32_t);
	void (*pfnWriteFPGARegister)(struct crystalhd_adp *, uint32_t, uint32_t);
	BC_STATUS (*pfnDevDRAMWrite)(struct crystalhd_hw *, uint32_t,
					 uint32_t, uint32_t *);
	uint32_t fwcmdPostAddr, fwcmdPostMbox, fwcmdRespMbox;
	uint32_t FleaRxPicDelAddr, FleaFLLUpdateAddr;
};

struct image {
	uint32_t guard_before;
	uint32_t words[16];
	uint32_t guard_after;
};

enum link_result {
	LINK_SUCCESS,
	LINK_READY_TIMEOUT,
	LINK_SIGNATURE_MISMATCH,
	LINK_VALIDATION_TIMEOUT,
};

static unsigned checks, groups, failures;
static unsigned dev_reads, dev_writes, fpga_reads, fpga_writes;
static unsigned dram_writes, sleeps, heartbeats, cmac_writes;
static unsigned arm_run_writes, flea_status_reads, link_status_reads;
static unsigned link_payload_writes;
static unsigned link_signature_writes, link_processor_starts;
static uint32_t dram_words, dram_first, dram_last;
static uint32_t cmac_offsets[4], cmac_values[4];
static uint32_t link_signature_offsets[8], link_signature_values[8];
static uint32_t link_payload_first, link_payload_last;
static uint32_t *expected_image;
static BC_STATUS dram_status;
static bool heartbeat_ok;
static enum link_result link_result;
static struct pci_dev pci;
static struct crystalhd_adp adapter;
static struct crystalhd_hw hardware;
static struct image firmware, firmware_before;

static void check(bool condition, const char *message)
{
	checks++;
	if (!condition) {
		failures++;
		fprintf(stderr, "FAIL: %s\n", message);
	}
}

static int test_sleep(unsigned int milliseconds)
{
	check(milliseconds == 1 || milliseconds == 5 || milliseconds == 10,
	      "production waits use the expected bounded intervals");
	sleeps++;
	return 0;
}

static uint32_t read_device(struct crystalhd_adp *adp, uint32_t offset)
{
	check(adp == &adapter, "Flea read uses the admitted adapter");
	dev_reads++;
	if (offset == BCHP_WRAP_MISC_INTR2_PCI_STATUS)
		return flea_status_reads++ ? BOOT_VER_DONE_BIT :
			SCRAM_KEY_DONE_INT_BIT;
	return 0;
}

static void write_device(struct crystalhd_adp *adp, uint32_t offset,
			 uint32_t value)
{
	check(adp == &adapter, "Flea write uses the admitted adapter");
	dev_writes++;
	if (offset <= BCHP_SCRUB_CTRL_BI_CMAC_127_96 &&
	    offset >= BCHP_SCRUB_CTRL_BI_CMAC_31_0) {
		if (cmac_writes < 4) {
			cmac_offsets[cmac_writes] = offset;
			cmac_values[cmac_writes] = value;
		}
		cmac_writes++;
	}
	if (offset == BCHP_ARMCR4_BRIDGE_REG_BRIDGE_CTL &&
	    (value & ARM_RUN_REQ_BIT))
		arm_run_writes++;
}

static BC_STATUS write_dram(struct crystalhd_hw *hw, uint32_t offset,
			    uint32_t words, uint32_t *buffer)
{
	check(hw == &hardware && offset == FW_DOWNLOAD_START_ADDR &&
	      buffer == expected_image && words,
	      "Flea DRAM write receives the exact image and start address");
	dram_writes++;
	dram_words = words;
	dram_first = buffer[0];
	dram_last = buffer[words - 1];
	return dram_status;
}

static uint32_t read_fpga(struct crystalhd_adp *adp, uint32_t offset)
{
	check(adp == &adapter, "Link read uses the admitted adapter");
	fpga_reads++;
	if (offset == OTP_CMD)
		return BC_BIT(1);
	if (offset == DCI_CMD)
		return 0;
	if (offset != DCI_STATUS)
		return 0;

	link_status_reads++;
	if (link_result == LINK_READY_TIMEOUT)
		return 0;
	if (link_status_reads == 1)
		return BC_BIT(4);
	if (link_status_reads == 2)
		return link_result == LINK_SIGNATURE_MISMATCH ? 0 : BC_BIT(9);
	return link_result == LINK_VALIDATION_TIMEOUT ? 0 : BC_BIT(0);
}

static void write_fpga(struct crystalhd_adp *adp, uint32_t offset,
		       uint32_t value)
{
	check(adp == &adapter, "Link write uses the admitted adapter");
	fpga_writes++;
	if (offset == DCI_FIRMWARE_DATA) {
		if (!link_payload_writes)
			link_payload_first = value;
		link_payload_last = value;
		link_payload_writes++;
	}
	if (offset <= DCI_SIGNATURE_DATA_7 &&
	    offset >= DCI_SIGNATURE_DATA_0) {
		if (link_signature_writes < 8) {
			link_signature_offsets[link_signature_writes] = offset;
			link_signature_values[link_signature_writes] = value;
		}
		link_signature_writes++;
	}
	if (offset == DCI_CMD && (value & BC_BIT(4)))
		link_processor_starts++;
}

static bool crystalhd_flea_detect_fw_alive(struct crystalhd_hw *hw)
{
	check(hw == &hardware, "heartbeat probe uses the admitted hardware");
	heartbeats++;
	return heartbeat_ok;
}

#include "fw-download-functions.h"

static uint32_t patterned_word(size_t index)
{
	return 0x10203040U ^ (uint32_t)(index * 0x9e3779b9U);
}

static void reset(void)
{
	memset(&pci, 0, sizeof(pci));
	adapter = (struct crystalhd_adp){ .pdev = &pci };
	hardware = (struct crystalhd_hw){
		.adp = &adapter,
		.pfnReadDevRegister = read_device,
		.pfnWriteDevRegister = write_device,
		.pfnReadFPGARegister = read_fpga,
		.pfnWriteFPGARegister = write_fpga,
		.pfnDevDRAMWrite = write_dram,
		.fwcmdPostAddr = 0x11111111,
		.fwcmdPostMbox = 0x22222222,
		.fwcmdRespMbox = 0x33333333,
		.FleaRxPicDelAddr = 0x44444444,
		.FleaFLLUpdateAddr = 0x55555555,
	};
	memset(&firmware, 0, sizeof(firmware));
	firmware.guard_before = 0x12345678;
	firmware.guard_after = 0x87654321;
	for (unsigned n = 0; n < sizeof(firmware.words) / sizeof(firmware.words[0]); n++)
		firmware.words[n] = patterned_word(n);
	firmware_before = firmware;
	expected_image = firmware.words;
	dev_reads = dev_writes = fpga_reads = fpga_writes = 0;
	dram_writes = sleeps = heartbeats = cmac_writes = 0;
	arm_run_writes = flea_status_reads = link_status_reads = 0;
	link_payload_writes = 0;
	link_signature_writes = link_processor_starts = 0;
	dram_words = dram_first = dram_last = 0;
	memset(cmac_offsets, 0, sizeof(cmac_offsets));
	memset(cmac_values, 0, sizeof(cmac_values));
	memset(link_signature_offsets, 0, sizeof(link_signature_offsets));
	memset(link_signature_values, 0, sizeof(link_signature_values));
	link_payload_first = link_payload_last = 0;
	dram_status = BC_STS_SUCCESS;
	heartbeat_ok = true;
	link_result = LINK_SUCCESS;
}

static bool no_effects(void)
{
	return !dev_reads && !dev_writes && !fpga_reads && !fpga_writes &&
	       !dram_writes && !sleeps && !heartbeats && !cmac_writes &&
	       !arm_run_writes && !link_status_reads && !link_payload_writes &&
	       !link_signature_writes && !link_processor_starts;
}

static bool state_unchanged(void)
{
	return hardware.fwcmdPostAddr == 0x11111111 &&
	       hardware.fwcmdPostMbox == 0x22222222 &&
	       hardware.fwcmdRespMbox == 0x33333333 &&
	       hardware.FleaRxPicDelAddr == 0x44444444 &&
	       hardware.FleaFLLUpdateAddr == 0x55555555 &&
	       !memcmp(&firmware, &firmware_before, sizeof(firmware));
}

static void helper_boundaries(void)
{
	groups++;
	check(CRYSTALHD_FLEA_MIN_FIRMWARE_SIZE ==
	      FLEA_FW_SIG_LEN_IN_BYTES + LENGTH_FIELD_SIZE + 4U,
	      "Flea minimum contains one payload dword and its real trailer");
	check(CRYSTALHD_LINK_MIN_FIRMWARE_SIZE ==
	      CRYSTALHD_LINK_FIRMWARE_TRAILER_SIZE + 4U,
	      "Link minimum contains one payload dword and its trailer");
	check(crystalhd_valid_firmware_image(firmware.words,
	      CRYSTALHD_FLEA_MIN_FIRMWARE_SIZE,
	      CRYSTALHD_FLEA_MIN_FIRMWARE_SIZE) &&
	      crystalhd_valid_firmware_image(firmware.words,
	      CRYSTALHD_LINK_MIN_FIRMWARE_SIZE,
	      CRYSTALHD_LINK_MIN_FIRMWARE_SIZE),
	      "both exact model minima are valid");
	check(crystalhd_valid_firmware_image(firmware.words, FLEA_BLOB_SIZE,
	      CRYSTALHD_FLEA_MIN_FIRMWARE_SIZE) &&
	      crystalhd_valid_firmware_image(firmware.words, LINK_BLOB_SIZE,
	      CRYSTALHD_LINK_MIN_FIRMWARE_SIZE),
	      "both repository firmware blobs satisfy the shared limits");
	check(crystalhd_valid_firmware_image(firmware.words,
	      CRYSTALHD_MAX_FIRMWARE_SIZE,
	      CRYSTALHD_LINK_MIN_FIRMWARE_SIZE),
	      "the exact firmware maximum is accepted");
	check(!crystalhd_valid_firmware_image(firmware.words,
	      CRYSTALHD_MAX_FIRMWARE_SIZE + 4U,
	      CRYSTALHD_LINK_MIN_FIRMWARE_SIZE) &&
	      !crystalhd_valid_firmware_image((uint8_t *)firmware.words + 1,
	      CRYSTALHD_LINK_MIN_FIRMWARE_SIZE,
	      CRYSTALHD_LINK_MIN_FIRMWARE_SIZE),
	      "oversized and unaligned images are rejected");
}

static void invalid_flea(void)
{
	static const uint32_t extra[] = {
		CRYSTALHD_FLEA_MIN_FIRMWARE_SIZE + 1,
		CRYSTALHD_FLEA_MIN_FIRMWARE_SIZE + 2,
		CRYSTALHD_FLEA_MIN_FIRMWARE_SIZE + 3,
		CRYSTALHD_MAX_FIRMWARE_SIZE - 3,
		CRYSTALHD_MAX_FIRMWARE_SIZE - 2,
		CRYSTALHD_MAX_FIRMWARE_SIZE - 1,
		CRYSTALHD_MAX_FIRMWARE_SIZE + 1,
		CRYSTALHD_MAX_FIRMWARE_SIZE + 2,
		CRYSTALHD_MAX_FIRMWARE_SIZE + 3,
		CRYSTALHD_MAX_FIRMWARE_SIZE + 4,
		UINT32_MAX,
	};

	groups++;
	for (uint32_t size = 0; size < CRYSTALHD_FLEA_MIN_FIRMWARE_SIZE; size++) {
		reset();
		check(crystalhd_flea_download_fw(&hardware,
		      (uint8_t *)firmware.words, size) == BC_STS_INV_ARG &&
		      no_effects() && state_unchanged(),
		      "every undersized Flea image is effect-free");
	}
	for (unsigned n = 0; n < sizeof(extra) / sizeof(extra[0]); n++) {
		reset();
		check(crystalhd_flea_download_fw(&hardware,
		      (uint8_t *)firmware.words, extra[n]) == BC_STS_INV_ARG &&
		      no_effects() && state_unchanged(),
		      "misaligned and oversized Flea images are effect-free");
	}
	reset();
	check(crystalhd_flea_download_fw(&hardware, NULL,
	      CRYSTALHD_FLEA_MIN_FIRMWARE_SIZE) == BC_STS_INV_ARG &&
	      no_effects() && state_unchanged(),
	      "Flea rejects a missing image before hardware access");
	reset();
	check(crystalhd_flea_download_fw(&hardware,
	      (uint8_t *)firmware.words + 1,
	      CRYSTALHD_FLEA_MIN_FIRMWARE_SIZE) == BC_STS_INV_ARG &&
	      no_effects() && state_unchanged(),
	      "Flea rejects an unaligned image before hardware access");
	reset();
	check(crystalhd_flea_download_fw(NULL, (uint8_t *)firmware.words,
	      CRYSTALHD_FLEA_MIN_FIRMWARE_SIZE) == BC_STS_INV_ARG && no_effects(),
	      "Flea rejects missing hardware");
	reset();
	hardware.adp = NULL;
	check(crystalhd_flea_download_fw(&hardware, (uint8_t *)firmware.words,
	      CRYSTALHD_FLEA_MIN_FIRMWARE_SIZE) == BC_STS_INV_ARG &&
	      no_effects() && state_unchanged(),
	      "Flea rejects a missing adapter before hardware access");
	reset();
	adapter.pdev = NULL;
	check(crystalhd_flea_download_fw(&hardware, (uint8_t *)firmware.words,
	      CRYSTALHD_FLEA_MIN_FIRMWARE_SIZE) == BC_STS_INV_ARG &&
	      no_effects() && state_unchanged(),
	      "Flea rejects a missing PCI device before hardware access");
	for (unsigned callback = 0; callback < 3; callback++) {
		reset();
		if (callback == 0)
			hardware.pfnReadDevRegister = NULL;
		else if (callback == 1)
			hardware.pfnWriteDevRegister = NULL;
		else
			hardware.pfnDevDRAMWrite = NULL;
		check(crystalhd_flea_download_fw(&hardware,
		      (uint8_t *)firmware.words,
		      CRYSTALHD_FLEA_MIN_FIRMWARE_SIZE) == BC_STS_INV_ARG &&
		      no_effects() && state_unchanged(),
		      "Flea rejects missing callbacks before hardware access");
	}
}

static void invalid_link(void)
{
	static const uint32_t extra[] = {
		CRYSTALHD_LINK_MIN_FIRMWARE_SIZE + 1,
		CRYSTALHD_LINK_MIN_FIRMWARE_SIZE + 2,
		CRYSTALHD_LINK_MIN_FIRMWARE_SIZE + 3,
		CRYSTALHD_MAX_FIRMWARE_SIZE - 3,
		CRYSTALHD_MAX_FIRMWARE_SIZE - 2,
		CRYSTALHD_MAX_FIRMWARE_SIZE - 1,
		CRYSTALHD_MAX_FIRMWARE_SIZE + 1,
		CRYSTALHD_MAX_FIRMWARE_SIZE + 2,
		CRYSTALHD_MAX_FIRMWARE_SIZE + 3,
		CRYSTALHD_MAX_FIRMWARE_SIZE + 4,
		UINT32_MAX,
	};

	groups++;
	for (uint32_t size = 0; size < CRYSTALHD_LINK_MIN_FIRMWARE_SIZE; size++) {
		reset();
		check(crystalhd_link_download_fw(&hardware,
		      (uint8_t *)firmware.words, size) == BC_STS_INV_ARG &&
		      no_effects() && state_unchanged(),
		      "every undersized Link image is effect-free");
	}
	for (unsigned n = 0; n < sizeof(extra) / sizeof(extra[0]); n++) {
		reset();
		check(crystalhd_link_download_fw(&hardware,
		      (uint8_t *)firmware.words, extra[n]) == BC_STS_INV_ARG &&
		      no_effects() && state_unchanged(),
		      "misaligned and oversized Link images are effect-free");
	}
	reset();
	check(crystalhd_link_download_fw(&hardware, NULL,
	      CRYSTALHD_LINK_MIN_FIRMWARE_SIZE) == BC_STS_INV_ARG &&
	      no_effects() && state_unchanged(),
	      "Link rejects a missing image before hardware access");
	reset();
	check(crystalhd_link_download_fw(&hardware,
	      (uint8_t *)firmware.words + 1,
	      CRYSTALHD_LINK_MIN_FIRMWARE_SIZE) == BC_STS_INV_ARG &&
	      no_effects() && state_unchanged(),
	      "Link rejects an unaligned image before hardware access");
	reset();
	check(crystalhd_link_download_fw(NULL, (uint8_t *)firmware.words,
	      CRYSTALHD_LINK_MIN_FIRMWARE_SIZE) == BC_STS_INV_ARG && no_effects(),
	      "Link rejects missing hardware");
	reset();
	hardware.adp = NULL;
	check(crystalhd_link_download_fw(&hardware, (uint8_t *)firmware.words,
	      CRYSTALHD_LINK_MIN_FIRMWARE_SIZE) == BC_STS_INV_ARG &&
	      no_effects() && state_unchanged(),
	      "Link rejects a missing adapter before hardware access");
	reset();
	adapter.pdev = NULL;
	check(crystalhd_link_download_fw(&hardware, (uint8_t *)firmware.words,
	      CRYSTALHD_LINK_MIN_FIRMWARE_SIZE) == BC_STS_INV_ARG &&
	      no_effects() && state_unchanged(),
	      "Link rejects a missing PCI device before hardware access");
	for (unsigned callback = 0; callback < 2; callback++) {
		reset();
		if (callback == 0)
			hardware.pfnReadFPGARegister = NULL;
		else
			hardware.pfnWriteFPGARegister = NULL;
		check(crystalhd_link_download_fw(&hardware,
		      (uint8_t *)firmware.words,
		      CRYSTALHD_LINK_MIN_FIRMWARE_SIZE) == BC_STS_INV_ARG &&
		      no_effects() && state_unchanged(),
		      "Link rejects missing callbacks before hardware access");
	}
}

static void flea_results(void)
{
	uint32_t end = GetScrubEndAddr(CRYSTALHD_FLEA_MIN_FIRMWARE_SIZE);

	groups++;
	reset();
	check(crystalhd_flea_download_fw(&hardware,
	      (uint8_t *)firmware.words,
	      CRYSTALHD_FLEA_MIN_FIRMWARE_SIZE) == BC_STS_SUCCESS,
	      "minimum Flea image completes the production path");
	check(dram_writes == 1 && dram_words == 1 &&
	      dram_first == firmware.words[0] && dram_last == firmware.words[0] &&
	      cmac_writes == 4 &&
	      arm_run_writes == 1 && heartbeats == 1 &&
	      hardware.fwcmdPostAddr == end + 1 + DDRADDR_4_FWCMDS &&
	      hardware.fwcmdPostMbox == FW_CMD_POST_MBOX &&
	      hardware.fwcmdRespMbox == FW_CMD_RES_MBOX &&
	      hardware.FleaRxPicDelAddr == end + 1 + HOST_TO_FW_PIC_DEL_INFO_ADDR &&
	      hardware.FleaFLLUpdateAddr == end + 1 + HOST_TO_FW_FLL_ADDR &&
	      !memcmp(&firmware, &firmware_before, sizeof(firmware)),
	      "Flea writes one payload dword, four CMAC words and starts firmware");
	for (unsigned n = 0; n < 4; n++)
		check(cmac_offsets[n] == BCHP_SCRUB_CTRL_BI_CMAC_127_96 - n * 4 &&
		      cmac_values[n] == __builtin_bswap32(firmware.words[2 + n]),
		      "Flea writes every distinct CMAC word to its exact register in order");

	reset();
	dram_status = BC_STS_IO_ERROR;
	check(crystalhd_flea_download_fw(&hardware,
	      (uint8_t *)firmware.words,
	      CRYSTALHD_FLEA_MIN_FIRMWARE_SIZE) == BC_STS_IO_ERROR,
	      "Flea propagates the exact DRAM write error");
	check(dram_writes == 1 && !cmac_writes && !arm_run_writes && !heartbeats &&
	      !memcmp(&firmware, &firmware_before, sizeof(firmware)),
	      "Flea stops before signature, processor start and heartbeat after DRAM failure");
}

static void link_results(void)
{
	groups++;
	reset();
	check(crystalhd_link_download_fw(&hardware,
	      (uint8_t *)firmware.words,
	      CRYSTALHD_LINK_MIN_FIRMWARE_SIZE) == BC_STS_SUCCESS,
	      "minimum Link image completes the production path");
	check(link_payload_writes == 1 &&
	      link_payload_first == firmware.words[0] &&
	      link_payload_last == firmware.words[0] &&
	      link_signature_writes == 8 &&
	      link_processor_starts == 1 && hardware.fwcmdPostAddr == TS_Host2CpuSnd &&
	      hardware.fwcmdPostMbox == Hst2CpuMbx1 &&
	      hardware.fwcmdRespMbox == Cpu2HstMbx1 &&
	      !memcmp(&firmware, &firmware_before, sizeof(firmware)),
	      "Link writes the exact minimum layout and publishes mailboxes after validation");
	for (unsigned n = 0; n < 8; n++)
		check(link_signature_offsets[n] == DCI_SIGNATURE_DATA_7 - n * 4 &&
		      link_signature_values[n] ==
			__builtin_bswap32(firmware.words[2 + n]),
		      "Link skips its length word and writes distinct signature words in order");

	reset();
	link_result = LINK_SIGNATURE_MISMATCH;
	check(crystalhd_link_download_fw(&hardware,
	      (uint8_t *)firmware.words,
	      CRYSTALHD_LINK_MIN_FIRMWARE_SIZE) == BC_STS_FW_AUTH_FAILED &&
	      !link_processor_starts && state_unchanged(),
	      "Link signature mismatch remains an authentication failure");

	reset();
	link_result = LINK_VALIDATION_TIMEOUT;
	check(crystalhd_link_download_fw(&hardware,
	      (uint8_t *)firmware.words,
	      CRYSTALHD_LINK_MIN_FIRMWARE_SIZE) == BC_STS_TIMEOUT,
	      "Link validation timeout is no longer reported as success");
	check(link_status_reads == 1002 && !link_processor_starts && state_unchanged(),
	      "Link timeout neither starts the processor nor publishes command mailboxes");

	reset();
	link_result = LINK_READY_TIMEOUT;
	check(crystalhd_link_download_fw(&hardware,
	      (uint8_t *)firmware.words,
	      CRYSTALHD_LINK_MIN_FIRMWARE_SIZE) == BC_STS_TIMEOUT &&
	      link_status_reads == 1000 && !link_payload_writes &&
	      !link_signature_writes && state_unchanged(),
	      "Link download-ready timeout still stops before image writes");
}

static void sized_flea(uint32_t size, const char *description)
{
	uint32_t payload_words =
		(size - CRYSTALHD_FLEA_FIRMWARE_TRAILER_SIZE) / 4;
	uint32_t signature_word = size / 4 - 4;
	uint32_t *image = malloc(size);

	check(image != NULL, "allocate an exact-sized Flea image");
	if (!image)
		return;
	for (uint32_t n = 0; n < size / 4; n++)
		image[n] = patterned_word(n);

	reset();
	expected_image = image;
	check(crystalhd_flea_download_fw(&hardware, (uint8_t *)image, size) ==
	      BC_STS_SUCCESS, description);
	check(dram_writes == 1 && dram_words == payload_words &&
	      dram_first == image[0] && dram_last == image[payload_words - 1] &&
	      cmac_writes == 4 && arm_run_writes == 1 && heartbeats == 1,
	      "Flea traverses the exact payload and trailer bounds");
	for (unsigned n = 0; n < 4; n++)
		check(cmac_offsets[n] == BCHP_SCRUB_CTRL_BI_CMAC_127_96 - n * 4 &&
		      cmac_values[n] == __builtin_bswap32(image[signature_word + n]),
		      "Flea sized traversal preserves CMAC address and value order");
	free(image);
}

static void sized_link(uint32_t size, const char *description)
{
	uint32_t payload_words =
		(size - CRYSTALHD_LINK_FIRMWARE_TRAILER_SIZE) / 4;
	uint32_t signature_word = payload_words + 1;
	uint32_t *image = malloc(size);

	check(image != NULL, "allocate an exact-sized Link image");
	if (!image)
		return;
	for (uint32_t n = 0; n < size / 4; n++)
		image[n] = patterned_word(n);

	reset();
	expected_image = image;
	check(crystalhd_link_download_fw(&hardware, (uint8_t *)image, size) ==
	      BC_STS_SUCCESS, description);
	check(link_payload_writes == payload_words &&
	      link_payload_first == image[0] &&
	      link_payload_last == image[payload_words - 1] &&
	      link_signature_writes == 8 && link_processor_starts == 1,
	      "Link traverses the exact payload and trailer bounds");
	for (unsigned n = 0; n < 8; n++)
		check(link_signature_offsets[n] == DCI_SIGNATURE_DATA_7 - n * 4 &&
		      link_signature_values[n] ==
			__builtin_bswap32(image[signature_word + n]),
		      "Link sized traversal preserves signature address and value order");
	free(image);
}

static void sized_images(void)
{
	groups++;
	sized_flea(FLEA_BLOB_SIZE,
		   "Flea traverses an exact repository-sized image");
	sized_flea(CRYSTALHD_MAX_FIRMWARE_SIZE,
		   "Flea traverses the exact accepted maximum image");
	sized_link(LINK_BLOB_SIZE,
		   "Link traverses an exact repository-sized image");
	sized_link(CRYSTALHD_MAX_FIRMWARE_SIZE,
		   "Link traverses the exact accepted maximum image");
}

int main(void)
{
	reset();
	helper_boundaries();
	invalid_flea();
	invalid_link();
	flea_results();
	link_results();
	sized_images();
	printf("Firmware download safety passed (%u groups, %u checks, %u failures)\n",
	       groups, checks, failures);
	return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
