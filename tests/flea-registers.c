// SPDX-License-Identifier: GPL-2.0-or-later
/* Actual Flea register helpers with MMIO confined to allocated backing. */
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define KERN_ERR ""
#define printk(...) ((void)0)
#define dev_err(dev, ...) ((void)(dev))
#define ARRAY_SIZE(array) (sizeof(array) / sizeof((array)[0]))
#include "flea-register-addresses.h"

_Static_assert(FLEA_GISB_DIRECT_BASE == 0x50, "direct alias");
_Static_assert(FLEA_GISB_INDIRECT_ADDRESS == 0xfff8, "address selector");
_Static_assert(FLEA_GISB_INDIRECT_DATA == 0xfffc, "indirect data");

enum { MMIO_SIZE = 0x10000 };
typedef pthread_mutex_t spinlock_t;
struct pci_dev { int dev; };
struct crystalhd_adp {
	struct pci_dev *pdev;
	uint32_t pci_i2o_len;
	void *i2o_addr;
	spinlock_t gisb_lock;
};
struct mmio_event {
	char operation;
	size_t offset;
	uint32_t value;
	unsigned int actor;
};

static unsigned char *mmio;
static struct mmio_event events[4];
static unsigned int mmio_count, scenarios, race_failures;
static _Atomic unsigned int checks, lock_attempts, unlocked_mmio;
static pthread_mutex_t audit = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed = PTHREAD_COND_INITIALIZER;
static spinlock_t *adapter_lock;
static _Thread_local unsigned int actor, lock_depth;
static _Thread_local bool irq_disabled;
static bool racing, first_selected, second_boundary, second_blocked;
static uint32_t selected, bank_a, bank_b;
enum { REGISTER_A = 0x000e0018, REGISTER_B = 0x00070044 };

#define CHECK(condition) do { atomic_fetch_add(&checks, 1); if (!(condition)) { \
	fprintf(stderr, "Flea registers line %u: %s\n", __LINE__, #condition); \
	abort(); \
} } while (0)

static void must(int rc)
{
	CHECK(rc == 0);
}

static struct timespec deadline(void)
{
	struct timespec limit;

	must(clock_gettime(CLOCK_REALTIME, &limit));
	limit.tv_sec += 5;
	return limit;
}

/* The shim exposes blocked acquisition, not a scheduler-dependent sleep.
 * Without the production lock, worker B instead announces completion.
 */
static void test_spin_lock(spinlock_t *lock)
{
	struct timespec limit = deadline();
	int rc;

	CHECK(lock == adapter_lock);
	CHECK(irq_disabled && !lock_depth);
	atomic_fetch_add(&lock_attempts, 1);
	rc = pthread_mutex_trylock(lock);
	if (rc == EBUSY) {
		if (racing && actor == 2) {
			must(pthread_mutex_lock(&audit));
			second_blocked = true;
			second_boundary = true;
			must(pthread_cond_broadcast(&changed));
			must(pthread_mutex_unlock(&audit));
		}
		must(pthread_mutex_timedlock(lock, &limit));
	} else {
		must(rc);
	}
	lock_depth = 1;
}

static void test_spin_unlock(spinlock_t *lock)
{
	CHECK(lock == adapter_lock);
	CHECK(irq_disabled && lock_depth == 1);
	lock_depth = 0;
	must(pthread_mutex_unlock(lock));
}

#define spin_lock_irqsave(lock, flags) do { \
	(flags) = irq_disabled; irq_disabled = true; test_spin_lock(lock); \
} while (0)
#define spin_unlock_irqrestore(lock, flags) do { \
	test_spin_unlock(lock); irq_disabled = !!(flags); \
} while (0)

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
	events[mmio_count++] = (struct mmio_event){operation, offset, value, actor};
	if (lock_depth)
		CHECK(irq_disabled);
	else
		atomic_fetch_add(&unlocked_mmio, 1);
}

static uint32_t *selected_bank(void)
{
	CHECK(selected == (REGISTER_A | 0x10000000U) ||
	      selected == (REGISTER_B | 0x10000000U));
	return selected == (REGISTER_A | 0x10000000U) ? &bank_a : &bank_b;
}

static uint32_t readl(const void *address)
{
	size_t offset = mmio_offset(address);
	uint32_t value;

	must(pthread_mutex_lock(&audit));
	if (racing && offset == FLEA_GISB_INDIRECT_DATA)
		value = *selected_bank();
	else if (racing && offset == FLEA_GISB_INDIRECT_ADDRESS)
		value = selected;
	else
		memcpy(&value, mmio + offset, sizeof(value));
	record('R', offset, value);
	must(pthread_mutex_unlock(&audit));
	return value;
}

static void writel(uint32_t value, void *address)
{
	size_t offset = mmio_offset(address);

	must(pthread_mutex_lock(&audit));
	record('W', offset, value);
	memcpy(mmio + offset, &value, sizeof(value));
	if (racing && offset == FLEA_GISB_INDIRECT_DATA)
		*selected_bank() = value;
	if (racing && offset == FLEA_GISB_INDIRECT_ADDRESS) {
		selected = value;
		if (actor == 1) {
			struct timespec limit = deadline();

			CHECK(!first_selected);
			first_selected = true;
			must(pthread_cond_broadcast(&changed));
			while (!second_boundary)
				must(pthread_cond_timedwait(&changed, &audit, &limit));
		}
	}
	must(pthread_mutex_unlock(&audit));
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
	unsigned int attempts = atomic_load(&lock_attempts);
	uint32_t value;

	begin();
	value = crystalhd_flea_reg_rd(adp, reg_off);
	CHECK(mmio_count == 0);
	CHECK(value == 0);
	begin();
	crystalhd_flea_reg_wr(adp, reg_off, 0x87654321);
	CHECK(mmio_count == 0);
	CHECK(atomic_load(&lock_attempts) == attempts);
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

struct worker {
	struct crystalhd_adp *adp;
	uint32_t reg_off, write_value, read_value;
	unsigned int id;
	bool write, initial_irq;
};

static void *register_worker(void *opaque)
{
	struct worker *worker = opaque;
	struct timespec limit = deadline();

	actor = worker->id;
	irq_disabled = worker->initial_irq;
	CHECK(!lock_depth);
	if (actor == 2) {
		must(pthread_mutex_lock(&audit));
		while (!first_selected)
			must(pthread_cond_timedwait(&changed, &audit, &limit));
		must(pthread_mutex_unlock(&audit));
	}
	if (worker->write)
		crystalhd_flea_reg_wr(worker->adp, worker->reg_off, worker->write_value);
	else
		worker->read_value = crystalhd_flea_reg_rd(worker->adp, worker->reg_off);
	CHECK(!lock_depth);
	CHECK(irq_disabled == worker->initial_irq);
	if (actor == 2) {
		must(pthread_mutex_lock(&audit));
		second_boundary = true;
		must(pthread_cond_broadcast(&changed));
		must(pthread_mutex_unlock(&audit));
	}
	return NULL;
}

static void regression_check(bool condition, const char *description)
{
	atomic_fetch_add(&checks, 1);
	if (!condition) {
		if (!race_failures)
			fprintf(stderr, "Flea GISB regression: %s\n", description);
		race_failures++;
	}
}

/* A always pauses between selector and data. B either blocks in the real
 * helper's adapter lock or completes before A resumes. Both outcomes have
 * explicit handshakes; scheduling and machine speed cannot hide the race.
 */
static void concurrent(struct crystalhd_adp *adp, bool write_a, bool write_b,
		       bool initial_irq, uint32_t direct_alias)
{
	const uint32_t initial_a = 0xa1a2a3a4, initial_b = 0xb1b2b3b4;
	const uint32_t value_a = 0x11223344, value_b = 0x55667788;
	uint32_t expected_a = write_a ? value_a : initial_a;
	uint32_t expected_b = write_b ? value_b : initial_b;
	uint32_t expected_selected = REGISTER_B | 0x10000000U;
	uint32_t expected_b_read = initial_b;
	struct worker workers[2] = {
		{.adp = adp, .reg_off = REGISTER_A, .write_value = value_a,
		 .id = 1, .write = write_a, .initial_irq = initial_irq},
		{.adp = adp, .reg_off = direct_alias ? direct_alias : REGISTER_B,
		 .write_value = value_b, .id = 2, .write = write_b,
		 .initial_irq = !initial_irq},
	};
	pthread_t threads[2];
	unsigned int b_events = direct_alias ? 1U : 2U, i;

	if (direct_alias) {
		uint32_t offset = direct_alias & 0xffff;

		CHECK(offset == FLEA_GISB_INDIRECT_ADDRESS ||
		      offset == FLEA_GISB_INDIRECT_DATA);
		expected_b = initial_b;
		expected_selected = REGISTER_A | 0x10000000U;
		if (offset == FLEA_GISB_INDIRECT_ADDRESS) {
			workers[1].write_value = REGISTER_B | 0x10000000U;
			expected_b_read = REGISTER_A | 0x10000000U;
			if (write_b)
				expected_selected = REGISTER_B | 0x10000000U;
		} else {
			expected_b_read = expected_a;
			if (write_b)
				expected_a = value_b;
		}
	}
	begin();
	bank_a = initial_a;
	bank_b = initial_b;
	selected = REGISTER_A | 0x10000000U;
	first_selected = second_boundary = second_blocked = false;
	racing = true;
	must(pthread_create(&threads[0], NULL, register_worker, &workers[0]));
	must(pthread_create(&threads[1], NULL, register_worker, &workers[1]));
	must(pthread_join(threads[0], NULL));
	must(pthread_join(threads[1], NULL));
	racing = false;
	regression_check(write_a || workers[0].read_value == initial_a,
			 "first read used another transaction's selector/data");
	regression_check(bank_a == expected_a && bank_b == expected_b,
			 "a write reached the wrong register or the wrong serialized value");
	regression_check(write_b || workers[1].read_value == expected_b_read,
			 "second read did not observe the completed first transaction");
	regression_check(selected == expected_selected, "selector ordering changed");
	CHECK(mmio_count == 2U + b_events);
	for (i = 0; i < mmio_count; i++)
		regression_check(events[i].actor == (i < 2 ? 1U : 2U),
				 "another helper entered between selector and data");
	regression_check(second_blocked, "competing transaction did not block");
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
	struct crystalhd_adp adp = {.pdev = &pci};
	unsigned int i, j, k, race_start;
	unsigned long flags;

	mmio = calloc(1, MMIO_SIZE);
	CHECK(mmio != NULL);
	adp.i2o_addr = mmio;
	adapter_lock = &adp.gisb_lock;
	must(pthread_mutex_init(adapter_lock, NULL));
	/* Exercise the shim independently so old production code compiles under
	 * -Werror and the red result is a real wrong-register counterexample.
	 */
	spin_lock_irqsave(adapter_lock, flags);
	spin_unlock_irqrestore(adapter_lock, flags);
	CHECK(!irq_disabled);
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
	race_start = scenarios;
	adp.pci_i2o_len = MMIO_SIZE;
	for (i = 0; i < 2; i++) {
		for (j = 0; j < 2; j++) {
			for (k = 0; k < 2; k++) {
				concurrent(&adp, i, j, k, 0);
				concurrent(&adp, i, j, k, FLEA_GISB_INDIRECT_ADDRESS);
				concurrent(&adp, i, j, k, FLEA_GISB_INDIRECT_DATA);
				concurrent(&adp, i, j, k,
					   0x00500000U | FLEA_GISB_INDIRECT_ADDRESS);
				concurrent(&adp, i, j, k,
					   0x00500000U | FLEA_GISB_INDIRECT_DATA);
			}
		}
	}
	regression_check(atomic_load(&unlocked_mmio) == 0,
			 "a valid register access bypassed the adapter lock");
	must(pthread_mutex_destroy(adapter_lock));
	must(pthread_cond_destroy(&changed));
	must(pthread_mutex_destroy(&audit));
	free(mmio);
	printf("Flea registers: %u scenarios, %u checks\n", scenarios, atomic_load(&checks));
	printf("Flea GISB concurrency: %u scenarios, %u failures\n",
	       scenarios - race_start, race_failures);
	return race_failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
