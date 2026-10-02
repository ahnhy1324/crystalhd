// SPDX-License-Identifier: GPL-2.0-or-later
/* Actual Flea register helpers with MMIO confined to allocated backing. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define KERN_ERR ""
#define printk(...) ((void)0)
#define dev_err(dev, ...) ((void)(dev))
#define ARRAY_SIZE(array) (sizeof(array) / sizeof((array)[0]))
#include "flea-register-addresses.h"

_Static_assert(FLEA_GISB_DIRECT_BASE == 0x50, "direct alias");
_Static_assert(FLEA_GISB_INDIRECT_ADDRESS == 0xfff8, "address selector");
_Static_assert(FLEA_GISB_INDIRECT_DATA == 0xfffc, "indirect data");

enum { MMIO_SIZE = 0x10000 };
struct pci_dev { int dev; };
struct crystalhd_adp {
	struct pci_dev *pdev;
	uint32_t pci_i2o_len;
	void *i2o_addr;
};
struct mmio_event {
	char operation;
	size_t offset;
	uint32_t value;
};

static unsigned char *mmio;
static struct mmio_event events[2];
static unsigned int mmio_count, scenarios, checks;

#define CHECK(condition) do { checks++; if (!(condition)) { \
	fprintf(stderr, "Flea registers line %u: %s\n", __LINE__, #condition); \
	abort(); \
} } while (0)

static size_t mmio_offset(const void *address)
{
	uintptr_t pointer = (uintptr_t)address;
	uintptr_t base = (uintptr_t)mmio;

	CHECK(pointer >= base);
	CHECK(pointer - base <= MMIO_SIZE - sizeof(uint32_t));
	return pointer - base;
}

static void record(char operation, size_t offset, uint32_t value)
{
	CHECK(mmio_count < ARRAY_SIZE(events));
	events[mmio_count++] = (struct mmio_event){operation, offset, value};
}

static uint32_t readl(const void *address)
{
	size_t offset = mmio_offset(address);
	uint32_t value;

	memcpy(&value, mmio + offset, sizeof(value));
	record('R', offset, value);
	return value;
}

static void writel(uint32_t value, void *address)
{
	size_t offset = mmio_offset(address);

	record('W', offset, value);
	memcpy(mmio + offset, &value, sizeof(value));
}

#include "flea-register-functions.h"

static void begin(void)
{
	mmio_count = 0;
	memset(events, 0, sizeof(events));
	scenarios++;
}

static void event(unsigned int index, char operation, size_t offset,
		  uint32_t value)
{
	CHECK(index < mmio_count);
	CHECK(events[index].operation == operation);
	CHECK(events[index].offset == offset);
	CHECK(events[index].value == value);
}

static uint32_t backing(size_t offset)
{
	uint32_t value;

	CHECK(offset <= MMIO_SIZE - sizeof(value));
	memcpy(&value, mmio + offset, sizeof(value));
	return value;
}

static void rejected(struct crystalhd_adp *adp, uint32_t reg_off)
{
	uint32_t value;

	begin();
	value = crystalhd_flea_reg_rd(adp, reg_off);
	CHECK(mmio_count == 0);
	CHECK(value == 0);
	begin();
	crystalhd_flea_reg_wr(adp, reg_off, 0x87654321);
	CHECK(mmio_count == 0);
}

static void direct(struct crystalhd_adp *adp, uint32_t reg_off,
		   uint32_t read_value, uint32_t write_value)
{
	size_t offset = reg_off & 0xffff;
	uint32_t value;

	CHECK(offset <= MMIO_SIZE - sizeof(value));
	memcpy(mmio + offset, &read_value, sizeof(read_value));
	begin();
	value = crystalhd_flea_reg_rd(adp, reg_off);
	CHECK(mmio_count == 1);
	CHECK(value == read_value);
	event(0, 'R', offset, read_value);
	CHECK(backing(offset) == read_value);
	begin();
	crystalhd_flea_reg_wr(adp, reg_off, write_value);
	CHECK(mmio_count == 1);
	event(0, 'W', offset, write_value);
	CHECK(backing(offset) == write_value);
}

static void indirect(struct crystalhd_adp *adp, uint32_t reg_off,
		     uint32_t read_value, uint32_t write_value)
{
	uint32_t selector = reg_off | 0x10000000;
	uint32_t value;

	memcpy(mmio + FLEA_GISB_INDIRECT_DATA, &read_value, sizeof(read_value));
	begin();
	value = crystalhd_flea_reg_rd(adp, reg_off);
	CHECK(mmio_count == 2);
	CHECK(value == read_value);
	event(0, 'W', FLEA_GISB_INDIRECT_ADDRESS, selector);
	event(1, 'R', FLEA_GISB_INDIRECT_DATA, read_value);
	CHECK(backing(FLEA_GISB_INDIRECT_ADDRESS) == selector);
	CHECK(backing(FLEA_GISB_INDIRECT_DATA) == read_value);
	begin();
	crystalhd_flea_reg_wr(adp, reg_off, write_value);
	CHECK(mmio_count == 2);
	event(0, 'W', FLEA_GISB_INDIRECT_ADDRESS, selector);
	event(1, 'W', FLEA_GISB_INDIRECT_DATA, write_value);
	CHECK(backing(FLEA_GISB_INDIRECT_ADDRESS) == selector);
	CHECK(backing(FLEA_GISB_INDIRECT_DATA) == write_value);
}

int main(void)
{
	static const uint32_t aliases[] = {0, 0x00500000};
	static const uint32_t indirect_addresses[] = {
		0x00010000, 0x00510000, 0x00540000, 0x00800110, 0x10540400,
	};
	static const uint32_t short_lengths[] = {0, 3, 4, 0xfff8, 0xfffc, 0xffff};
	static const uint32_t full_lengths[] = {0x10000, 0x10004, UINT32_MAX};
	static const uint32_t values[] = {0, 1, 0xdeadbeef, UINT32_MAX};
	struct pci_dev pci = {0};
	struct crystalhd_adp adp = {&pci, 0, NULL};
	unsigned int i, j, k;

	mmio = calloc(1, MMIO_SIZE);
	CHECK(mmio != NULL);
	adp.i2o_addr = mmio;
	rejected(NULL, 0);
	rejected(NULL, 0x00500000);
	rejected(NULL, 0x00540000);
	for (i = 0; i < ARRAY_SIZE(aliases); i++) {
		for (adp.pci_i2o_len = 0; adp.pci_i2o_len < 4; adp.pci_i2o_len++)
			rejected(&adp, aliases[i]);
		adp.pci_i2o_len = 4;
		for (j = 0; j < ARRAY_SIZE(values); j++)
			direct(&adp, aliases[i], values[j], values[ARRAY_SIZE(values) - j - 1]);
		for (j = 1; j <= 4; j++)
			rejected(&adp, aliases[i] + j);
		adp.pci_i2o_len = 8;
		direct(&adp, aliases[i] + 4, 0x12345678, 0x87654321);
		for (j = 5; j <= 8; j++)
			rejected(&adp, aliases[i] + j);
		adp.pci_i2o_len = MMIO_SIZE;
		direct(&adp, aliases[i] + 0xfffc, 0x12345678, 0x87654321);
		for (j = 0xfffd; j <= 0xffff; j++)
			rejected(&adp, aliases[i] + j);
	}
	for (i = 0; i < ARRAY_SIZE(indirect_addresses); i++) {
		for (j = 0; j < ARRAY_SIZE(short_lengths); j++) {
			adp.pci_i2o_len = short_lengths[j];
			rejected(&adp, indirect_addresses[i]);
		}
		for (j = 0; j < ARRAY_SIZE(full_lengths); j++) {
			/* The reported BAR may be larger; all accesses still use the
			 * same allocated 64 KiB window, checked by the MMIO mocks.
			 */
			adp.pci_i2o_len = full_lengths[j];
			for (k = 0; k < ARRAY_SIZE(values); k++)
				indirect(&adp, indirect_addresses[i], values[k],
					 values[ARRAY_SIZE(values) - k - 1]);
		}
	}
	free(mmio);
	printf("Flea registers: %u scenarios, %u checks\n", scenarios, checks);
	return 0;
}
