/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Link the real libcrystalhd_priv.cpp/libcrystalhd_if.cpp with function-section
 * garbage collection and --wrap=pthread_mutex_lock for lock-order observation.
 * Only ring allocation/copy/reset functions are reachable: no device, firmware,
 * shared-memory setup, or TX worker is started by this executable.
 */
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <pthread.h>
#include "7411d.h"
#include "libcrystalhd_if.h"
#include "libcrystalhd_int_if.h"
#include "libcrystalhd_priv.h"

static unsigned failures;
static pthread_mutex_t *observed_flush_lock;
static pthread_mutex_t *observed_counter_lock;
static unsigned observed_lock_order;

extern "C" int __real_pthread_mutex_lock(pthread_mutex_t *mutex);
extern "C" int __wrap_pthread_mutex_lock(pthread_mutex_t *mutex)
{
    /* Observe, but never replace, the real mutex operations. The getter must
     * synchronize with both reset and producer/consumer counter updates.
     */
    if (mutex == observed_flush_lock)
        observed_lock_order = observed_lock_order * 10 + 1;
    if (mutex == observed_counter_lock)
        observed_lock_order = observed_lock_order * 10 + 2;
    return __real_pthread_mutex_lock(mutex);
}

static void check(bool condition, const char *message)
{
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++failures;
    }
}

static void check_unlocked(pthread_mutex_t *mutex, const char *message)
{
    const int result = pthread_mutex_trylock(mutex);
    check(result == 0, message);
    /* All operations in this test run on one thread. If the old error path
     * leaked this lock, it belongs to this same thread: release it so the
     * regression reports failure rather than hanging during the next reset.
     */
    if (result == 0 || result == EBUSY)
        pthread_mutex_unlock(mutex);
}

static void test_invalid_pop_preserves_ring()
{
    TXBUFFER ring = {};
    unsigned char input[24];
    unsigned char output[25];
    std::memset(input, 0x5a, sizeof(input));
    std::memset(output, 0xa5, sizeof(output));
    if (txBufInit(&ring, CIRC_TX_BUF_SIZE) != BC_STS_SUCCESS) {
        check(false, "initialize actual-capacity ring");
        return;
    }
    check(txBufPush(&ring, input, sizeof(input)) == BC_STS_SUCCESS,
          "queue bytes before invalid pop");
    check(txBufPop(&ring, output, sizeof(output)) == BC_STS_INV_ARG,
          "reject a pop larger than available data");
    check_unlocked(&ring.flushLock, "invalid pop must release flushLock");
    check_unlocked(&ring.pushpopLock, "invalid pop must leave pushpopLock free");
    check(ring.readPointer == 0 && ring.writePointer == sizeof(input) &&
              ring.busySize == sizeof(input) &&
              ring.freeSize == CIRC_TX_BUF_SIZE - sizeof(input),
          "invalid pop must not change ring accounting");
    bool untouched = true;
    for (unsigned char byte : output)
        untouched = untouched && byte == 0xa5;
    check(untouched, "invalid pop must not copy any bytes");
    check(txBufPop(&ring, output, sizeof(input)) == BC_STS_SUCCESS &&
              std::memcmp(input, output, sizeof(input)) == 0,
          "valid pop still returns the original bytes after rejected pop");
    check(txBufFlush(&ring) == BC_STS_SUCCESS,
          "reset still completes after rejected pop");
    check(txBufFree(&ring) == BC_STS_SUCCESS, "free first ring");
}

static void test_flush_invalidates_pending_pop_size()
{
    TXBUFFER ring = {};
    unsigned char old_input[32];
    unsigned char new_input[16];
    unsigned char output[32];
    std::memset(old_input, 0x12, sizeof(old_input));
    std::memset(new_input, 0x34, sizeof(new_input));
    std::memset(output, 0xa5, sizeof(output));
    if (txBufInit(&ring, CIRC_TX_BUF_SIZE) != BC_STS_SUCCESS) {
        check(false, "initialize ring for flush interleaving");
        return;
    }
    check(txBufPush(&ring, old_input, sizeof(old_input)) == BC_STS_SUCCESS,
          "queue old generation input");
    /* txThreadProc chooses this size before taking flushLock. A flush can
     * empty the ring before txBufPop acquires that lock; reproduce exactly
     * that ordering without a scheduler-dependent thread race.
     */
    const unsigned pending_size = ring.busySize;
    check(txBufFlush(&ring) == BC_STS_SUCCESS, "flush before pending pop");
    check(txBufPop(&ring, output, pending_size) == BC_STS_INV_ARG,
          "reject size sampled before flush");
    check_unlocked(&ring.flushLock,
                   "stale post-flush pop must release flushLock");
    check(ring.readPointer == 0 && ring.writePointer == 0 &&
              ring.busySize == 0 && ring.freeSize == CIRC_TX_BUF_SIZE,
          "stale pop must preserve the flushed ring");
    check(output[0] == 0xa5 && output[sizeof(output) - 1] == 0xa5,
          "stale pop must not expose flushed bytes");
    check(txBufPush(&ring, new_input, sizeof(new_input)) == BC_STS_SUCCESS,
          "queue fresh input after stale pop");
    check(txBufPop(&ring, output, sizeof(new_input)) == BC_STS_SUCCESS &&
              std::memcmp(new_input, output, sizeof(new_input)) == 0,
          "fresh input survives the rejected stale pop");
    check(txBufFlush(&ring) == BC_STS_SUCCESS,
          "a second flush must not wait on the leaked error-path lock");
    check(txBufFree(&ring) == BC_STS_SUCCESS, "free flushed ring");
}

static void test_wrapped_copy()
{
    TXBUFFER ring = {};
    unsigned char first[96];
    unsigned char second[64];
    unsigned char output[80];
    for (unsigned i = 0; i < sizeof(first); ++i)
        first[i] = static_cast<unsigned char>(i);
    for (unsigned i = 0; i < sizeof(second); ++i)
        second[i] = static_cast<unsigned char>(128 + i);
    if (txBufInit(&ring, 128) != BC_STS_SUCCESS) {
        check(false, "initialize wrapping ring");
        return;
    }
    check(txBufPush(&ring, first, sizeof(first)) == BC_STS_SUCCESS &&
              txBufPop(&ring, output, 80) == BC_STS_SUCCESS &&
              std::memcmp(first, output, 80) == 0,
          "copy initial bytes before wrap");
    check(txBufPush(&ring, second, sizeof(second)) == BC_STS_SUCCESS &&
              txBufPop(&ring, output, sizeof(output)) == BC_STS_SUCCESS &&
              std::memcmp(first + 80, output, 16) == 0 &&
              std::memcmp(second, output + 16, sizeof(second)) == 0,
          "wrapped push/pop preserves both byte ranges");
    check(ring.busySize == 0 && ring.freeSize == ring.totalSize &&
              ring.readPointer == ring.writePointer,
          "wrapped ring accounting remains balanced");
    check_unlocked(&ring.flushLock, "successful wrapped pop releases flushLock");
    check_unlocked(&ring.pushpopLock, "wrapped copy releases pushpopLock");
    check(txBufFree(&ring) == BC_STS_SUCCESS, "free wrapping ring");
}

static void test_free_size_snapshot()
{
    DTS_LIB_CONTEXT context = {};
    unsigned char input[16] = {};
    unsigned char output[8] = {};
    context.Sig = LIB_CTX_SIG;
    if (txBufInit(&context.circBuf, CIRC_TX_BUF_SIZE) != BC_STS_SUCCESS) {
        check(false, "initialize getter test ring");
        return;
    }
    observed_flush_lock = &context.circBuf.flushLock;
    observed_counter_lock = &context.circBuf.pushpopLock;
    observed_lock_order = 0;
    check(DtsTxFreeSize(&context) == CIRC_TX_BUF_SIZE,
          "public getter reports initial ring capacity");
    check(observed_lock_order == 12,
          "public getter locks reset then counter mutex before snapshot");
    observed_flush_lock = observed_counter_lock = NULL;
    check_unlocked(&context.circBuf.flushLock,
                   "getter releases reset mutex");
    check_unlocked(&context.circBuf.pushpopLock,
                   "getter releases counter mutex");
    check(txBufPush(&context.circBuf, input, sizeof(input)) == BC_STS_SUCCESS &&
              DtsTxFreeSize(&context) == CIRC_TX_BUF_SIZE - sizeof(input),
          "public getter observes producer byte accounting");
    check(txBufPop(&context.circBuf, output, sizeof(output)) == BC_STS_SUCCESS &&
              DtsTxFreeSize(&context) == CIRC_TX_BUF_SIZE - sizeof(output),
          "public getter observes consumer byte accounting");
    check(txBufFlush(&context.circBuf) == BC_STS_SUCCESS &&
              DtsTxFreeSize(&context) == CIRC_TX_BUF_SIZE,
          "public getter observes a flushed ring");
    check(txBufFree(&context.circBuf) == BC_STS_SUCCESS, "free getter ring");
}

int main()
{
    test_invalid_pop_preserves_ring();
    test_flush_invalidates_pending_pop_size();
    test_wrapped_copy();
    test_free_size_snapshot();
    if (failures != 0)
        return 1;
    std::puts("PASS: actual TX ring invalid-pop, flush, wrap, and getter tests");
    return 0;
}
