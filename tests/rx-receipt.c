/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Execute the extracted research RX receipt store with no device or MMIO. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "crystalhd_fw_research.h"

typedef uint32_t u32;
typedef uint64_t u64;
typedef int BC_STATUS;
#define BC_STS_SUCCESS 0
#define BC_STS_ERROR (-1)
#define COMP_FLAG_PIB_VALID 2U
#define DMA_ENGINE_CNT 2U
#define GFP_KERNEL 0
#define U64_MAX UINT64_MAX
#define READ_ONCE(value) __atomic_load_n(&(value), __ATOMIC_ACQUIRE)
#define WRITE_ONCE(target, value) \
	__atomic_store_n(&(target), (value), __ATOMIC_RELEASE)
#define min_t(type, left, right) \
	((type)(left) < (type)(right) ? (type)(left) : (type)(right))

typedef pthread_mutex_t spinlock_t;
#define spin_lock_init(lock) assert(pthread_mutex_init((lock), NULL) == 0)
#define spin_lock_irqsave(lock, flags) do { \
	(flags) = 0x1234UL; assert(pthread_mutex_lock((lock)) == 0); \
} while (0)
#define spin_unlock_irqrestore(lock, flags) do { \
	assert((flags) == 0x1234UL); assert(pthread_mutex_unlock((lock)) == 0); \
} while (0)

struct dma_descriptor { unsigned last_rec_indicator; };
struct dma_desc_mem {
	struct dma_descriptor *pdma_desc_start;
	u32 sz;
};
struct crystalhd_rx_metadata {
	u64 firmware_timestamp;
	u32 picture_number, picture_flags, picture_width, picture_height;
	u32 row_width, pib_line;
	u32 session_number;
	bool valid, eos_trailer;
};
struct crystalhd_pib {
	u32 picture_number, flags, width, height, sess_num;
};
struct crystalhd_rx_dma_pkt {
	struct dma_desc_mem desc_mem;
	u64 capture_epoch;
	u32 pkt_tag, flags, y_done_sz, uv_done_sz;
	struct crystalhd_pib pib;
	struct crystalhd_rx_metadata metadata;
	u64 receipt_run_id;
	u32 receipt_index_plus_one;
	u32 receipt_y_descriptor_count;
	u32 receipt_uv_descriptor_count;
};
struct crystalhd_rx_receipt_key {
	u64 run_id;
	u32 index_plus_one;
};
struct crystalhd_rx_receipt_store;
struct crystalhd_adp {
	u64 generation;
	_Atomic(struct crystalhd_rx_receipt_store *) rx_receipts;
	u64 rx_receipt_last_run_id;
};
struct crystalhd_hw { struct crystalhd_adp *adp; };

static _Atomic unsigned allocations, frees;
static _Atomic u64 fake_ns;

static pthread_mutex_t rcu_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t rcu_cond = PTHREAD_COND_INITIALIZER;
static unsigned rcu_readers;
static _Thread_local bool pause_next_rcu_lookup;

static struct {
	pthread_mutex_t lock;
	pthread_cond_t cond;
	bool watching;
	bool lookup_observed;
	bool release_lookup;
	bool grace_started;
} race = {
	.lock = PTHREAD_MUTEX_INITIALIZER,
	.cond = PTHREAD_COND_INITIALIZER,
};

static void rcu_read_lock(void)
{
	assert(pthread_mutex_lock(&rcu_lock) == 0);
	rcu_readers++;
	assert(pthread_mutex_unlock(&rcu_lock) == 0);
}

static void rcu_read_unlock(void)
{
	assert(pthread_mutex_lock(&rcu_lock) == 0);
	assert(rcu_readers);
	rcu_readers--;
	assert(pthread_cond_broadcast(&rcu_cond) == 0);
	assert(pthread_mutex_unlock(&rcu_lock) == 0);
}

static void synchronize_rcu(void)
{
	assert(pthread_mutex_lock(&race.lock) == 0);
	if (race.watching) {
		race.grace_started = true;
		assert(pthread_cond_broadcast(&race.cond) == 0);
	}
	assert(pthread_mutex_unlock(&race.lock) == 0);

	assert(pthread_mutex_lock(&rcu_lock) == 0);
	while (rcu_readers)
		assert(pthread_cond_wait(&rcu_cond, &rcu_lock) == 0);
	assert(pthread_mutex_unlock(&rcu_lock) == 0);
}

static struct crystalhd_rx_receipt_store *
test_rcu_dereference(struct crystalhd_rx_receipt_store *store)
{
	if (!pause_next_rcu_lookup)
		return store;
	pause_next_rcu_lookup = false;
	assert(pthread_mutex_lock(&race.lock) == 0);
	assert(race.watching && !race.lookup_observed);
	race.lookup_observed = true;
	assert(pthread_cond_broadcast(&race.cond) == 0);
	while (!race.release_lookup)
		assert(pthread_cond_wait(&race.cond, &race.lock) == 0);
	assert(pthread_mutex_unlock(&race.lock) == 0);
	return store;
}

#define rcu_dereference(target) \
	test_rcu_dereference(atomic_load_explicit(&(target), memory_order_acquire))
#define rcu_dereference_protected(target, condition) \
	(assert(condition), atomic_load_explicit(&(target), memory_order_acquire))
#define rcu_assign_pointer(target, value) \
	atomic_store_explicit(&(target), (value), memory_order_release)

static void *kzalloc(size_t size, int flags)
{
	void *memory;
	assert(flags == GFP_KERNEL);
	memory = calloc(1, size);
	if (memory)
		atomic_fetch_add_explicit(&allocations, 1, memory_order_relaxed);
	return memory;
}
static void *kvcalloc(size_t count, size_t size, int flags)
{
	assert(flags == GFP_KERNEL && (!size || count <= SIZE_MAX / size));
	return kzalloc(count * size, flags);
}
static void kfree(void *memory)
{
	if (memory) {
		atomic_fetch_add_explicit(&frees, 1, memory_order_relaxed);
		free(memory);
	}
}
#define kvfree(memory) kfree(memory)
static u64 ktime_get_ns(void)
{
	return atomic_fetch_add_explicit(&fake_ns, 1, memory_order_relaxed) + 1;
}

#include "rx-receipt-functions.h"

static void arm(struct crystalhd_adp *adp, u32 capacity,
		struct crystalhd_fw_research_rx_receipt_arm *request)
{
	*request = (struct crystalhd_fw_research_rx_receipt_arm){
		.version = CRYSTALHD_FW_RESEARCH_RX_RECEIPT_VERSION,
		.size = sizeof(*request),
		.capacity = capacity,
	};
	assert(crystalhd_rx_receipt_arm(adp, request) == 0);
}

static struct timespec race_deadline(void)
{
	struct timespec deadline;

	assert(clock_gettime(CLOCK_REALTIME, &deadline) == 0);
	deadline.tv_sec += 5;
	return deadline;
}

static void race_begin(void)
{
	assert(pthread_mutex_lock(&race.lock) == 0);
	assert(!race.watching);
	race.watching = true;
	race.lookup_observed = false;
	race.release_lookup = false;
	race.grace_started = false;
	assert(pthread_mutex_unlock(&race.lock) == 0);
}

static void race_wait_for_lookup(void)
{
	struct timespec deadline = race_deadline();
	int rc;

	assert(pthread_mutex_lock(&race.lock) == 0);
	while (!race.lookup_observed) {
		rc = pthread_cond_timedwait(&race.cond, &race.lock, &deadline);
		assert(rc == 0);
	}
	assert(pthread_mutex_unlock(&race.lock) == 0);
}

static void race_wait_for_grace_or_done(_Atomic bool *writer_done)
{
	struct timespec deadline = race_deadline();
	int rc;

	assert(pthread_mutex_lock(&race.lock) == 0);
	while (!race.grace_started &&
	       !atomic_load_explicit(writer_done, memory_order_acquire)) {
		rc = pthread_cond_timedwait(&race.cond, &race.lock, &deadline);
		assert(rc == 0);
	}
	assert(race.grace_started);
	assert(!atomic_load_explicit(writer_done, memory_order_acquire));
	assert(pthread_mutex_unlock(&race.lock) == 0);
}

static void race_release_reader(void)
{
	assert(pthread_mutex_lock(&race.lock) == 0);
	race.release_lookup = true;
	assert(pthread_cond_broadcast(&race.cond) == 0);
	assert(pthread_mutex_unlock(&race.lock) == 0);
}

static void race_end(void)
{
	assert(pthread_mutex_lock(&race.lock) == 0);
	assert(race.lookup_observed && race.release_lookup &&
	       race.grace_started);
	race.watching = false;
	assert(pthread_mutex_unlock(&race.lock) == 0);
}

static void race_writer_done(_Atomic bool *done)
{
	atomic_store_explicit(done, true, memory_order_release);
	assert(pthread_mutex_lock(&race.lock) == 0);
	assert(pthread_cond_broadcast(&race.cond) == 0);
	assert(pthread_mutex_unlock(&race.lock) == 0);
}

struct irq_reader_context {
	struct crystalhd_hw *hw;
	u64 result;
};

static void *irq_reader_thread(void *argument)
{
	struct irq_reader_context *context = argument;

	pause_next_rcu_lookup = true;
	context->result = crystalhd_rx_receipt_irq_entry(context->hw);
	return NULL;
}

struct cancel_writer_context {
	struct crystalhd_adp *adp;
	u64 generation;
	u64 run_id;
	int result;
	_Atomic bool done;
};

static void *cancel_writer_thread(void *argument)
{
	struct cancel_writer_context *context = argument;

	context->result = crystalhd_rx_receipt_cancel_empty_arm(context->adp,
		context->generation, context->run_id);
	race_writer_done(&context->done);
	return NULL;
}

struct cleanup_writer_context {
	struct crystalhd_adp *adp;
	_Atomic bool done;
};

static void *cleanup_writer_thread(void *argument)
{
	struct cleanup_writer_context *context = argument;

	crystalhd_rx_receipt_cleanup(context->adp);
	race_writer_done(&context->done);
	return NULL;
}

struct read_reader_context {
	struct crystalhd_adp *adp;
	struct crystalhd_fw_research_rx_receipt_read request;
	int result;
};

static void *read_reader_thread(void *argument)
{
	struct read_reader_context *context = argument;

	pause_next_rcu_lookup = true;
	context->result = crystalhd_rx_receipt_read(context->adp,
		&context->request);
	return NULL;
}

struct arm_writer_context {
	struct crystalhd_adp *adp;
	struct crystalhd_fw_research_rx_receipt_arm request;
	int result;
	_Atomic bool done;
};

static void *arm_writer_thread(void *argument)
{
	struct arm_writer_context *context = argument;

	context->result = crystalhd_rx_receipt_arm(context->adp,
		&context->request);
	race_writer_done(&context->done);
	return NULL;
}

static void test_cancel_waits_for_irq_lookup(void)
{
	struct crystalhd_adp adp = { .generation = 101 };
	struct crystalhd_hw hw = { .adp = &adp };
	struct crystalhd_fw_research_rx_receipt_arm request;
	struct irq_reader_context reader = { .hw = &hw };
	struct cancel_writer_context writer;
	pthread_t reader_thread, writer_thread;
	unsigned frees_before;

	arm(&adp, 1, &request);
	writer = (struct cancel_writer_context){
		.adp = &adp,
		.generation = adp.generation,
		.run_id = request.run_id,
	};
	frees_before = atomic_load_explicit(&frees, memory_order_relaxed);
	race_begin();
	assert(pthread_create(&reader_thread, NULL, irq_reader_thread, &reader) ==
	       0);
	race_wait_for_lookup();
	assert(pthread_create(&writer_thread, NULL, cancel_writer_thread, &writer) ==
	       0);
	race_wait_for_grace_or_done(&writer.done);
	assert(atomic_load_explicit(&frees, memory_order_relaxed) == frees_before);
	race_release_reader();
	assert(pthread_join(reader_thread, NULL) == 0);
	assert(pthread_join(writer_thread, NULL) == 0);
	race_end();
	assert(reader.result == 0 && writer.result == 0);
	assert(!atomic_load_explicit(&adp.rx_receipts, memory_order_acquire));
	assert(atomic_load_explicit(&frees, memory_order_relaxed) ==
	       frees_before + 2);
}

static void test_cleanup_waits_for_irq_lookup(void)
{
	struct crystalhd_adp adp = { .generation = 102 };
	struct crystalhd_hw hw = { .adp = &adp };
	struct crystalhd_fw_research_rx_receipt_arm request;
	struct irq_reader_context reader = { .hw = &hw };
	struct cleanup_writer_context writer = { .adp = &adp };
	pthread_t reader_thread, writer_thread;
	unsigned frees_before;

	arm(&adp, 1, &request);
	frees_before = atomic_load_explicit(&frees, memory_order_relaxed);
	race_begin();
	assert(pthread_create(&reader_thread, NULL, irq_reader_thread, &reader) ==
	       0);
	race_wait_for_lookup();
	assert(pthread_create(&writer_thread, NULL, cleanup_writer_thread, &writer) ==
	       0);
	race_wait_for_grace_or_done(&writer.done);
	assert(atomic_load_explicit(&frees, memory_order_relaxed) == frees_before);
	race_release_reader();
	assert(pthread_join(reader_thread, NULL) == 0);
	assert(pthread_join(writer_thread, NULL) == 0);
	race_end();
	assert(reader.result != 0);
	assert(!atomic_load_explicit(&adp.rx_receipts, memory_order_acquire));
	assert(atomic_load_explicit(&frees, memory_order_relaxed) ==
	       frees_before + 2);
}

static void test_replacement_waits_for_sealed_read(void)
{
	struct crystalhd_adp adp = { .generation = 103 };
	struct crystalhd_hw hw = { .adp = &adp };
	struct crystalhd_fw_research_rx_receipt_arm first;
	struct crystalhd_fw_research_rx_receipt_seal seal = {0};
	struct crystalhd_rx_dma_pkt packet = { .capture_epoch = 77 };
	struct read_reader_context reader = {0};
	struct arm_writer_context writer;
	pthread_t reader_thread, writer_thread;
	unsigned allocations_before, frees_before;

	arm(&adp, 1, &first);
	crystalhd_rx_receipt_reserve(&hw, &packet, 0, 88, 3, NULL);
	seal.run_id = first.run_id;
	assert(crystalhd_rx_receipt_seal(&adp, &seal) == 0);
	reader.adp = &adp;
	reader.request.run_id = first.run_id;
	writer = (struct arm_writer_context){
		.adp = &adp,
		.request = {
			.version = CRYSTALHD_FW_RESEARCH_RX_RECEIPT_VERSION,
			.size = sizeof(writer.request),
			.capacity = 1,
		},
	};
	allocations_before = atomic_load_explicit(&allocations,
		memory_order_relaxed);
	frees_before = atomic_load_explicit(&frees, memory_order_relaxed);
	race_begin();
	assert(pthread_create(&reader_thread, NULL, read_reader_thread, &reader) ==
	       0);
	race_wait_for_lookup();
	assert(pthread_create(&writer_thread, NULL, arm_writer_thread, &writer) ==
	       0);
	race_wait_for_grace_or_done(&writer.done);
	assert(atomic_load_explicit(&allocations, memory_order_relaxed) ==
	       allocations_before + 2);
	assert(atomic_load_explicit(&frees, memory_order_relaxed) == frees_before);
	race_release_reader();
	assert(pthread_join(reader_thread, NULL) == 0);
	assert(pthread_join(writer_thread, NULL) == 0);
	race_end();
	assert(reader.result == 0 && reader.request.returned == 1 &&
	       reader.request.records[0].capture_epoch == 77 &&
	       reader.request.records[0].rx_sequence == 88);
	assert(writer.result == 0 && writer.request.run_id == first.run_id + 1);
	assert(atomic_load_explicit(&frees, memory_order_relaxed) ==
	       frees_before + 2);
	crystalhd_rx_receipt_cleanup(&adp);
}

static void test_lifetime_races(void)
{
	unsigned outstanding = atomic_load_explicit(&allocations,
		memory_order_relaxed) -
		atomic_load_explicit(&frees, memory_order_relaxed);

	assert(!outstanding);
	test_cancel_waits_for_irq_lookup();
	test_cleanup_waits_for_irq_lookup();
	test_replacement_waits_for_sealed_read();
	assert(atomic_load_explicit(&allocations, memory_order_relaxed) ==
	       atomic_load_explicit(&frees, memory_order_relaxed));
}

int main(void)
{
	struct crystalhd_adp adp = { .generation = 91 };
	struct crystalhd_hw hw = { .adp = &adp };
	struct crystalhd_fw_research_rx_receipt_arm first, second;
	struct crystalhd_fw_research_rx_receipt_seal seal = {0};
	struct crystalhd_fw_research_rx_receipt_read read = {0};
	struct crystalhd_rx_dma_pkt packet = {0}, other = {0}, overflow = {0};
	struct crystalhd_rx_receipt_key packet_key = {0}, other_key = {0};
	struct crystalhd_rx_receipt_store *active_store;
	struct dma_descriptor descriptors[5] = {0};
	u64 irq_ns;
	u32 required_stages;

	_Static_assert(sizeof(struct crystalhd_fw_research_rx_receipt) == 144,
		       "fixed receipt wire layout");
	_Static_assert(sizeof(struct crystalhd_fw_research_rx_receipt_read) == 4672,
		       "fixed read chunk wire layout");

	arm(&adp, 2, &first);
	assert(first.generation == 91 && first.run_id == 1);
	assert(crystalhd_rx_receipt_cancel_empty_arm(&adp, 92, first.run_id) ==
	       -ESTALE);
	assert(crystalhd_rx_receipt_cancel_empty_arm(&adp, 91, first.run_id + 1) ==
	       -ESTALE);
	assert(crystalhd_rx_receipt_cancel_empty_arm(&adp, 91, first.run_id) == 0 &&
	       !adp.rx_receipts);
	arm(&adp, 2, &first);
	assert(first.run_id == 2);
	second = (struct crystalhd_fw_research_rx_receipt_arm){
		.version = CRYSTALHD_FW_RESEARCH_RX_RECEIPT_VERSION,
		.size = sizeof(second), .capacity = 1,
	};
	/* An unsealed replacement is refused and preserves the active run. */
	assert(crystalhd_rx_receipt_arm(&adp, &second) == -EBUSY);
	descriptors[2].last_rec_indicator = 1;
	descriptors[4].last_rec_indicator = 1;
	packet.desc_mem.pdma_desc_start = descriptors;
	packet.desc_mem.sz = sizeof(descriptors);
	packet.capture_epoch = 7;
	packet.pkt_tag = 0x55;
	crystalhd_rx_receipt_descriptors(&packet, 3);
	assert(packet.receipt_y_descriptor_count == 3 &&
	       packet.receipt_uv_descriptor_count == 2);
	crystalhd_rx_receipt_reserve(&hw, &packet, 0, 44, 2, &packet_key);
	assert(packet.receipt_run_id == first.run_id &&
	       packet.receipt_index_plus_one == 1 &&
	       packet_key.run_id == first.run_id && packet_key.index_plus_one == 1);
	irq_ns = crystalhd_rx_receipt_irq_entry(&hw);
	crystalhd_rx_receipt_irq(&hw, 0, irq_ns, BC_STS_SUCCESS);
	crystalhd_rx_receipt_fire_return(&hw, &packet_key);
	packet.y_done_sz = 100;
	packet.uv_done_sz = 25;
	crystalhd_rx_receipt_complete(&hw, &packet, BC_STS_SUCCESS);
	packet.metadata = (struct crystalhd_rx_metadata){
		.firmware_timestamp = 123456,
		.picture_number = 9,
		.picture_flags = 3,
		.picture_width = 1920,
		.picture_height = 1080,
		.row_width = 1920,
		.pib_line = 1080,
		.session_number = 6,
		.valid = true,
		.eos_trailer = true,
	};
	crystalhd_rx_receipt_parse(&hw, &packet, true);
	crystalhd_rx_receipt_disposition(&hw, &packet,
		CRYSTALHD_FW_RESEARCH_RX_DISPOSITION_OUTPUT);

	other.capture_epoch = 8;
	other.pkt_tag = 0x56;
	/* Recycled packet fields are stale on an error completion and must not be
	 * attributed to this transfer by the observer.
	 */
	other.y_done_sz = 777;
	other.uv_done_sz = 333;
	other.flags = 0x55aa;
	crystalhd_rx_receipt_reserve(&hw, &other, 1, 45, 2, &other_key);
	crystalhd_rx_receipt_fire_return(&hw, &other_key);
	irq_ns = crystalhd_rx_receipt_irq_entry(&hw);
	crystalhd_rx_receipt_irq(&hw, 1, irq_ns, BC_STS_ERROR);
	crystalhd_rx_receipt_complete(&hw, &other, BC_STS_ERROR);
	active_store = atomic_load_explicit(&adp.rx_receipts,
		memory_order_acquire);
	assert(active_store->records[1].y_done_words == 0 &&
	       active_store->records[1].uv_done_words == 0 &&
	       active_store->records[1].packet_flags == 0);
	other.flags = 0;
	crystalhd_rx_receipt_parse(&hw, &other, false);
	crystalhd_rx_receipt_disposition(&hw, &other,
		CRYSTALHD_FW_RESEARCH_RX_DISPOSITION_RECYCLE_ZERO);

	crystalhd_rx_receipt_reserve(&hw, &overflow, 0, 46, 2, NULL);
	assert(!overflow.receipt_run_id && !overflow.receipt_index_plus_one);
	assert(crystalhd_rx_receipt_cancel_empty_arm(&adp, 91, first.run_id) ==
	       -EBUSY);
	seal.run_id = first.run_id;
	assert(crystalhd_rx_receipt_seal(&adp, &seal) == 0);
	assert(seal.sealed && seal.record_count == 2 && seal.overflow_count == 1);
	read.run_id = first.run_id;
	assert(crystalhd_rx_receipt_read(&adp, &read) == 0);
	assert(read.returned == 2 && read.record_count == 2 && read.sealed);
	assert(read.records[0].ordinal == 0 && read.records[0].rx_sequence == 44 &&
	       read.records[0].y_descriptor_count == 3 &&
	       read.records[0].uv_descriptor_count == 2 &&
	       read.records[0].session_number == 6 &&
	       read.records[0].eos_trailer == 1 &&
	       read.records[0].disposition ==
		CRYSTALHD_FW_RESEARCH_RX_DISPOSITION_OUTPUT);
	assert((read.records[0].stage_flags &
		CRYSTALHD_FW_RESEARCH_RX_STAGE_CLOCK_INVERTED) != 0);
	required_stages = CRYSTALHD_FW_RESEARCH_RX_STAGE_RESERVED |
		CRYSTALHD_FW_RESEARCH_RX_STAGE_FIRE_RETURN |
		CRYSTALHD_FW_RESEARCH_RX_STAGE_IRQ_ENTRY |
		CRYSTALHD_FW_RESEARCH_RX_STAGE_COMPLETE |
		CRYSTALHD_FW_RESEARCH_RX_STAGE_PARSED |
		CRYSTALHD_FW_RESEARCH_RX_STAGE_DISPOSITION;
	assert((read.records[0].stage_flags & required_stages) ==
	       required_stages);
	assert(read.records[1].completion_status == BC_STS_ERROR &&
	       read.records[1].completion_status < 0 &&
	       read.records[1].y_done_words == 0 &&
	       read.records[1].uv_done_words == 0 &&
	       read.records[1].packet_flags == 0 &&
	       (read.records[1].stage_flags &
		CRYSTALHD_FW_RESEARCH_RX_STAGE_PARSE_FAILED));

	/* A new idle run replaces the sealed store, advances identity, and makes
	 * stale packet keys harmless rather than aliasing the new run.
	 */
	seal = (struct crystalhd_fw_research_rx_receipt_seal){
		.run_id = first.run_id,
	};
	assert(crystalhd_rx_receipt_seal(&adp, &seal) == -EALREADY);
	seal = (struct crystalhd_fw_research_rx_receipt_seal){
		.flags = CRYSTALHD_FW_RESEARCH_RX_RECEIPT_SEAL_CURRENT,
	};
	assert(crystalhd_rx_receipt_seal(&adp, &seal) == 0 &&
	       seal.run_id == first.run_id && seal.record_count == 2);
	arm(&adp, 1, &second);
	assert(second.run_id == 3 && frees == 6);
	crystalhd_rx_receipt_fire_return(&hw, &packet_key);
	seal = (struct crystalhd_fw_research_rx_receipt_seal){0};
	assert(crystalhd_rx_receipt_seal(&adp, &seal) == -EINVAL);
	seal = (struct crystalhd_fw_research_rx_receipt_seal){ .run_id = 4 };
	assert(crystalhd_rx_receipt_seal(&adp, &seal) == -ESTALE);
	seal = (struct crystalhd_fw_research_rx_receipt_seal){
		.flags = CRYSTALHD_FW_RESEARCH_RX_RECEIPT_SEAL_CURRENT,
	};
	assert(crystalhd_rx_receipt_seal(&adp, &seal) == 0 &&
	       seal.run_id == 3 && seal.record_count == 0);
	crystalhd_rx_receipt_cleanup(&adp);
	assert(!adp.rx_receipts && allocations == frees);
	test_lifetime_races();
	puts("RX receipt store: lifecycle, overflow, stale keys and RCU races passed");
	return 0;
}
