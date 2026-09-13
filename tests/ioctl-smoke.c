// SPDX-License-Identifier: LGPL-2.1-or-later
#define _GNU_SOURCE

#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <linux/capability.h>

#include "bc_dts_defs.h"
#include "7411d.h"
typedef struct C011_PIB C011_PIB;
#include "bc_dts_glob_lnx.h"
#include "crystalhd_ioctl_limits.h"

static unsigned int checks;

static void fail(const char *name)
{
	fprintf(stderr, "%zu-bit: %s: %s\n", sizeof(void *) * 8, name,
		strerror(errno));
	exit(EXIT_FAILURE);
}

static void expect_errno(int fd, const char *name, unsigned long cmd,
			 void *data, int expected)
{
	errno = 0;
	if (ioctl(fd, cmd, data) != -1 || errno != expected) {
		fprintf(stderr, "%s: expected errno %d, got %d\n",
			name, expected, errno);
		fail(name);
	}
	checks++;
}

static void command(int fd, const char *name, unsigned long cmd,
		    BC_IOCTL_DATA *data)
{
	if (ioctl(fd, cmd, data) < 0)
		fail(name);
	if (data->RetSts != BC_STS_SUCCESS) {
		fprintf(stderr, "%s: driver status %d\n", name, data->RetSts);
		exit(EXIT_FAILURE);
	}
	checks++;
}

static int has_rawio(void)
{
	struct __user_cap_header_struct header = {
		.version = _LINUX_CAPABILITY_VERSION_3,
		.pid = 0,
	};
	struct __user_cap_data_struct caps[2] = {{0}};

	if (syscall(SYS_capget, &header, caps) < 0)
		fail("capget");
	return !!(caps[CAP_SYS_RAWIO / 32].effective &
		  (1U << (CAP_SYS_RAWIO % 32)));
}

static void playback_checks(const char *device, int flea, int rawio)
{
	BC_IOCTL_DATA data = {0};
	uint32_t original, expected;
	long page_size = sysconf(_SC_PAGESIZE);
	void *mapping;
	unsigned int mode;
	int fd = open(device, O_RDWR | O_CLOEXEC);

	if (fd < 0)
		fail("playback open");
	data.u.NotifyMode.Mode = DTS_PLAYBACK_MODE;
	command(fd, "playback mode", BCM_IOC_NOTIFY_MODE, &data);
	if (flea && !rawio) {
		memset(&data, 0, sizeof(data));
		data.u.regAcc.Offset = CRYSTALHD_FLEA_COLOR_REGISTER;
		command(fd, "legacy color read", BCM_IOC_REG_RD, &data);
		original = data.u.regAcc.Value;
		for (mode = 0; mode <= 2; mode += 2) {
			/* Deliberately change every unrelated bit in the request:
			 * the compatibility handler must preserve them in hardware.
			 */
			data.u.regAcc.Value = (~original & ~2U) | mode;
			command(fd, "legacy masked color write", BCM_IOC_REG_WR, &data);
			command(fd, "legacy color readback", BCM_IOC_REG_RD, &data);
			expected = (original & ~3U) | mode;
			if (data.u.regAcc.Value != expected) {
				fprintf(stderr, "color control: expected %#x, got %#x\n",
					expected, data.u.regAcc.Value);
				exit(EXIT_FAILURE);
			}
		}
		data.u.regAcc.Offset += 4;
		expect_errno(fd, "adjacent register read", BCM_IOC_REG_RD, &data, EPERM);
		expect_errno(fd, "adjacent register write", BCM_IOC_REG_WR, &data, EPERM);
	}

	/* Neither request can reach hardware posting: pinning must fail at
	 * the inaccessible page and release any earlier successfully pinned page.
	 */
	if (page_size <= 0 || (unsigned long)page_size > UINT_MAX / 2)
		fail("page size");
	mapping = mmap(NULL, (size_t)page_size * 2, PROT_READ | PROT_WRITE,
		       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (mapping == MAP_FAILED)
		fail("pin probe mmap");
	memset(mapping, 0, (size_t)page_size);
	if (mprotect((char *)mapping + page_size, (size_t)page_size, PROT_NONE))
		fail("pin probe guard page");
	memset(&data, 0, sizeof(data));
	data.u.RxBuffs.b422Mode = OUTPUT_MODE422_YUY2;
	data.u.RxBuffs.YuvBuff = mapping;
	data.u.RxBuffs.YuvBuffSz = (uint32_t)page_size * 2;
	if (ioctl(fd, BCM_IOC_ADD_RXBUFFS, &data) || data.RetSts != BC_STS_ERROR)
		fail("partial pin must fail");
	checks++;
	data.u.RxBuffs.YuvBuff = (uint8_t *)mapping + page_size;
	data.u.RxBuffs.YuvBuffSz = (uint32_t)page_size;
	if (ioctl(fd, BCM_IOC_ADD_RXBUFFS, &data) || data.RetSts != BC_STS_ERROR)
		fail("inaccessible pin must fail");
	checks++;
	if (munmap(mapping, (size_t)page_size * 2))
		fail("pin probe munmap");
	command(fd, "playback release", BCM_IOC_RELEASE, &data);
	if (close(fd))
		fail("playback close");
}

int main(int argc, char **argv)
{
	const char *device = CRYSTALHD_API_DEV_NAME;
	BC_IOCTL_DATA data = {0};
	unsigned long malformed;
	const unsigned long raw_commands[] = {
		BCM_IOC_REG_RD, BCM_IOC_REG_WR, BCM_IOC_FPGA_RD,
		BCM_IOC_FPGA_WR, BCM_IOC_MEM_RD, BCM_IOC_MEM_WR,
		BCM_IOC_WR_PCI_CFG,
	};
	long page_size;
	void *mapping;
	BC_IOCTL_DATA *boundary;
	unsigned int i;
	int fd, flea, rawio;

	if (argc == 2 && !strcmp(argv[1], "--help")) {
		printf("Usage: %s [/dev/crystalhd]\n"
		       "Checks version, hardware ID, monitor/playback open/release, "
		       "color controls and rejected ioctls/pins.\n"
		       "No firmware download or decode is performed. "
		       "Use an idle device with the rebuilt driver loaded.\n", argv[0]);
		return EXIT_SUCCESS;
	}
	if (argc > 2) {
		fprintf(stderr, "Usage: %s [/dev/crystalhd]\n", argv[0]);
		return EXIT_FAILURE;
	}
	if (argc == 2)
		device = argv[1];
	rawio = has_rawio();
	fd = open(device, O_RDWR | O_CLOEXEC);
	if (fd < 0)
		fail("open");

	/* Existing applications use both zero and the exact envelope size. */
	command(fd, "version (legacy size)", BCM_IOC_GET_VERSION, &data);
	printf("%zu-bit: driver %u.%u.%u\n", sizeof(void *) * 8,
	       data.u.VerInfo.DriverMajor, data.u.VerInfo.DriverMinor,
	       data.u.VerInfo.DriverRevision);
	memset(&data, 0, sizeof(data));
	data.IoctlDataSz = sizeof(data);
	data.next = (BC_IOCTL_DATA *)(uintptr_t)0x12345678U;
	command(fd, "hardware type", BCM_IOC_GET_HWTYPE, &data);
	if (data.u.hwType.PciVenId != 0x14e4 ||
	    (data.u.hwType.PciDevId != BC_PCI_DEVID_LINK &&
	     data.u.hwType.PciDevId != BC_PCI_DEVID_FLEA) ||
	    data.next != (BC_IOCTL_DATA *)(uintptr_t)0x12345678U) {
		fprintf(stderr, "hardware ID or ioctl pointer translation is invalid\n");
		return EXIT_FAILURE;
	}
	printf("%zu-bit: PCI %04x:%04x revision %u\n", sizeof(void *) * 8,
	       data.u.hwType.PciVenId, data.u.hwType.PciDevId, data.u.hwType.HwRev);
	flea = data.u.hwType.PciDevId == BC_PCI_DEVID_FLEA;
	memset(&data, 0, sizeof(data));
	data.u.pciCfg.Size = 4;
	command(fd, "legacy PCI identity", BCM_IOC_RD_PCI_CFG, &data);
	if (data.u.pciCfg.pci_cfg_space[0] != 0xe4 ||
	    data.u.pciCfg.pci_cfg_space[1] != 0x14) {
		fprintf(stderr, "legacy PCI identity is invalid\n");
		return EXIT_FAILURE;
	}

	malformed = _IOC(_IOC_READ | _IOC_WRITE, 'z', DRV_CMD_VERSION, sizeof(data));
	expect_errno(fd, "wrong magic", malformed, &data, ENOTTY);
	malformed = _IOC(_IOC_READ, BC_IOC_BASE, DRV_CMD_VERSION, sizeof(data));
	expect_errno(fd, "wrong direction", malformed, &data, ENOTTY);
	malformed = _IOC(_IOC_READ | _IOC_WRITE, BC_IOC_BASE,
			 DRV_CMD_VERSION, sizeof(data) - 1);
	expect_errno(fd, "wrong command size", malformed, &data, ENOTTY);
	malformed = _IOC(_IOC_READ | _IOC_WRITE, BC_IOC_BASE,
			 DRV_CMD_END, sizeof(data));
	expect_errno(fd, "unknown command", malformed, &data, ENOTTY);
	expect_errno(fd, "null envelope", BCM_IOC_GET_VERSION, NULL, EFAULT);
	data.IoctlDataSz = sizeof(data) - 1;
	expect_errno(fd, "wrong envelope size", BCM_IOC_GET_VERSION, &data, EINVAL);

	memset(&data, 0, sizeof(data));
	/* These fail during marshalling, before the firmware handler is called. */
	expect_errno(fd, "empty transfer", BCM_IOC_FW_DOWNLOAD, &data, E2BIG);
	data.u.devMem.NumDwords = CRYSTALHD_MAX_IOCTL_TRANSFER / 4 + 1;
	expect_errno(fd, "oversized transfer", BCM_IOC_FW_DOWNLOAD, &data, E2BIG);
	data.u.devMem.NumDwords = UINT_MAX;
	expect_errno(fd, "overflowing transfer", BCM_IOC_FW_DOWNLOAD, &data, E2BIG);

	page_size = sysconf(_SC_PAGESIZE);
	if (page_size < (long)sizeof(data))
		fail("page size");
	mapping = mmap(NULL, (size_t)page_size * 2, PROT_READ | PROT_WRITE,
		       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (mapping == MAP_FAILED ||
	    mprotect((char *)mapping + page_size, (size_t)page_size, PROT_NONE))
		fail("guard page");
	boundary = (BC_IOCTL_DATA *)((char *)mapping + page_size - sizeof(data));
	memset(boundary, 0, sizeof(*boundary));
	command(fd, "envelope at page boundary", BCM_IOC_GET_VERSION, boundary);
	expect_errno(fd, "truncated envelope", BCM_IOC_GET_VERSION,
		     (char *)boundary + sizeof(data) / 2, EFAULT);
	boundary->u.devMem.NumDwords = 1;
	expect_errno(fd, "unmapped appended payload", BCM_IOC_FW_DOWNLOAD,
		     boundary, EFAULT);
	if (munmap(mapping, (size_t)page_size * 2))
		fail("munmap");

	if (!rawio) {
		for (i = 0; i < sizeof(raw_commands) / sizeof(raw_commands[0]); i++)
			expect_errno(fd, "raw access permission", raw_commands[i], NULL, EPERM);
		memset(&data, 0, sizeof(data));
		data.u.pciCfg.Size = 4;
		data.u.pciCfg.Offset = 4;
		expect_errno(fd, "PCI diagnostic permission", BCM_IOC_RD_PCI_CFG,
			     &data, EPERM);
	} else {
		printf("CAP_SYS_RAWIO present; rerun as a desktop user to test permission denial\n");
	}

	memset(&data, 0, sizeof(data));
	data.u.NotifyMode.Mode = DTS_MONITOR_MODE;
	command(fd, "monitor mode", BCM_IOC_NOTIFY_MODE, &data);
	if (!rawio) {
		data.u.regAcc.Offset = CRYSTALHD_FLEA_COLOR_REGISTER;
		expect_errno(fd, "monitor color read denied", BCM_IOC_REG_RD, &data, EPERM);
		expect_errno(fd, "monitor color write denied", BCM_IOC_REG_WR, &data, EPERM);
	}
	command(fd, "release", BCM_IOC_RELEASE, &data);
	expect_errno(fd, "released handle", BCM_IOC_GET_VERSION, &data, ENODATA);
	if (close(fd))
		fail("close");
	/* Catch leaked open slots after ioctl release followed by close. */
	for (i = 0; i <= BC_LINK_MAX_OPENS; i++) {
		fd = open(device, O_RDWR | O_CLOEXEC);
		if (fd < 0 || close(fd))
			fail("reopen/close");
		checks++;
	}
	playback_checks(device, flea, rawio);
	printf("%zu-bit: PASS (%u checks)\n", sizeof(void *) * 8, checks);
	return EXIT_SUCCESS;
}
