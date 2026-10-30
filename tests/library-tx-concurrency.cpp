/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Actual ring producer/pop/getter with the public input path's thLock order.
 * No decoder/device setup or fabricated counter implementation is used. */
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdarg>
#include <cstdlib>
#include <cstring>
#include <pthread.h>
#include <thread>
#include "7411d.h"
#include "libcrystalhd_if.h"
#include "libcrystalhd_int_if.h"
#include "libcrystalhd_priv.h"

extern "C" int __wrap_open(const char *, int, ...) { std::abort(); }
extern "C" int __wrap_openat(int, const char *, int, ...) { std::abort(); }
extern bc_dil_glob_s *bc_dil_glob_ptr;
extern "C" int __wrap_ioctl(int fd, unsigned long command, ...) {
    if (fd != 99 || command != BCM_IOC_GET_DRV_STAT) std::abort();
    va_list args; va_start(args, command); BC_IOCTL_DATA *data = va_arg(args, BC_IOCTL_DATA *); va_end(args);
    data->u.drvStat = {}; data->u.drvStat.pwr_state_change = BC_HW_RUNNING; data->RetSts = BC_STS_SUCCESS;
    return 0;
}
BC_STATUS DtsFWPauseVideo(HANDLE, uint32_t) { std::abort(); }
static pthread_mutex_t *observed_counter;
static thread_local bool observe_lock;
static std::atomic<bool> counter_attempt(false);
extern "C" int __real_pthread_mutex_lock(pthread_mutex_t *);
extern "C" int __wrap_pthread_mutex_lock(pthread_mutex_t *lock) {
    if (observe_lock && lock == observed_counter) counter_attempt.store(true, std::memory_order_release);
    return __real_pthread_mutex_lock(lock);
}
static std::atomic<unsigned> failures(0);
static void check(bool good, const char *message) {
    if (!good) { std::fprintf(stderr, "FAIL: %s\n", message); ++failures; }
}
static unsigned char byte_at(size_t n) {
    return static_cast<unsigned char>((n * 131U + (n >> 8) * 17U) ^ 0x5aU);
}
template<class Operation>
static void requires_counter_lock(TXBUFFER *ring, const char *label, Operation operation) {
    observed_counter = &ring->pushpopLock; counter_attempt = false;
    std::atomic<bool> done(false);
    pthread_mutex_lock(observed_counter);
    std::thread caller([&] { observe_lock = true; operation(); done.store(true, std::memory_order_release); });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!counter_attempt.load(std::memory_order_acquire) && !done.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
    // Rejection/status must attempt the real mutex before reading publication counters.
    check(counter_attempt.load(std::memory_order_acquire), label);
    pthread_mutex_unlock(observed_counter); caller.join(); observed_counter = NULL;
}
static void deterministic_guards(DTS_LIB_CONTEXT *context) {
    unsigned char full[4096] = {}, value = 0;
    check(txBufPush(&context->circBuf, full, sizeof(full)) == BC_STS_SUCCESS, "fill actual ring for rejection guard");
    requires_counter_lock(&context->circBuf, "full push checks freeSize under counter mutex", [&] {
        DtsLock(context);
        check(txBufPush(&context->circBuf, &value, 1) == BC_STS_INSUFF_RES, "full push status preserved");
        DtsUnLock(context);
    });
    check(txBufFlush(&context->circBuf) == BC_STS_SUCCESS, "reset before empty-pop guard");
    requires_counter_lock(&context->circBuf, "empty pop checks busySize under counter mutex", [&] {
        check(txBufPop(&context->circBuf, &value, 1) == BC_STS_INV_ARG, "empty pop status preserved");
    });
    requires_counter_lock(&context->circBuf, "public status snapshots freeSize under counter mutex", [&] {
        BC_DTS_STATUS status = {}; status.cpbEmptySize = 1U << 30;
        check(DtsGetDriverStatus(context, &status) == BC_STS_SUCCESS && status.cpbEmptySize == 4096, "public status snapshot value preserved");
    });
}
int main(int argc, char **argv) {
    const bool serial = argc == 2 && !std::strcmp(argv[1], "serial");
    const bool status_mode = argc == 2 && !std::strcmp(argv[1], "status");
    DTS_LIB_CONTEXT context = {};
    context.Sig = LIB_CTX_SIG;
    bc_dil_glob_s globals = {}; BC_IOCTL_DATA pooled = {}; bc_dil_glob_ptr = &globals;
    context.DevHandle = 99; context.DevId = BC_PCI_DEVID_FLEA; context.VidParams.VideoAlgo = BC_VID_ALGO_H264;
    context.pIoDataFreeHd = &pooled;
    pthread_mutexattr_t attr;
    check(pthread_mutexattr_init(&attr) == 0, "mutex attributes");
    check(pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE) == 0, "recursive public context lock");
    check(pthread_mutex_init(&context.thLock, &attr) == 0, "context lock");
    pthread_mutexattr_destroy(&attr);
    check(txBufInit(&context.circBuf, 4096) == BC_STS_SUCCESS, "real ring init");
    if (failures) return 1;
    deterministic_guards(&context);
    const size_t extent = 1024 * 1024;
    std::atomic<bool> start(false), stop(false), consumer_done(false);
    auto producer = [&] {
        unsigned char input[193];
        while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
        for (size_t offset = 0; offset < extent && !stop; ) {
            const size_t n = std::min(extent - offset, 1 + (offset * 13U + 31U) % sizeof(input));
            for (size_t i = 0; i < n; ++i) input[i] = byte_at(offset + i);
            // Match DtsAlignSendData: thLock -> free snapshot -> actual enqueue.
            DtsLock(&context);
            BC_STATUS result = BC_STS_INSUFF_RES;
            if (DtsTxFreeSize(&context) >= n)
                result = txBufPush(&context.circBuf, input, n);
            DtsUnLock(&context);
            if (result == BC_STS_SUCCESS) offset += n;
            else if (result != BC_STS_INSUFF_RES) { check(false, "producer status"); stop = true; }
            else std::this_thread::yield();
        }
    };
    auto consumer = [&] {
        unsigned char output[127];
        while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
        for (size_t offset = 0; offset < extent && !stop; ) {
            const uint32_t free = DtsTxFreeSize(&context);
            check(free <= 4096, "free snapshot inside ring capacity");
            const size_t n = std::min(std::min(extent - offset, sizeof(output)), size_t(4096 - free));
            if (!n) { std::this_thread::yield(); continue; }
            if (txBufPop(&context.circBuf, output, n) != BC_STS_SUCCESS) {
                check(false, "single consumer may pop its published snapshot"); stop = true; break;
            }
            for (size_t i = 0; i < n; ++i) if (output[i] != byte_at(offset + i)) {
                check(false, "FIFO payload order across concurrent wrap"); stop = true; break;
            }
            offset += n;
        }
        consumer_done.store(true, std::memory_order_release);
    };
    if (serial) {
        unsigned char value[64], output[64];
        for (size_t offset = 0; offset < extent; offset += sizeof(value)) {
            for (size_t i = 0; i < sizeof(value); ++i) value[i] = byte_at(offset + i);
            DtsLock(&context);
            check(txBufPush(&context.circBuf, value, sizeof(value)) == BC_STS_SUCCESS, "serial actual push");
            DtsUnLock(&context);
            check(txBufPop(&context.circBuf, output, sizeof(output)) == BC_STS_SUCCESS && !std::memcmp(value, output, sizeof(value)), "serial actual pop bytes");
        }
    } else {
        std::thread push(producer), pop(consumer);
        std::thread observer;
        if (status_mode) observer = std::thread([&] {
            while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
            while (!consumer_done.load(std::memory_order_acquire)) {
                BC_DTS_STATUS status = {}; status.cpbEmptySize = 1U << 30;
                check(DtsGetDriverStatus(&context, &status) == BC_STS_SUCCESS && status.cpbEmptySize <= 4096, "actual public status software free snapshot");
            }
        });
        start.store(true, std::memory_order_release);
        push.join(); pop.join(); if (observer.joinable()) observer.join();
    }
    check(DtsTxFreeSize(&context) == 4096 && context.circBuf.busySize == 0 && context.circBuf.readPointer == context.circBuf.writePointer, "joined ring accounting is empty and balanced");
    check(txBufFree(&context.circBuf) == BC_STS_SUCCESS, "real ring free after joins");
    pthread_mutex_destroy(&context.thLock);
    bc_dil_glob_ptr = NULL;
    if (failures) return 1;
    std::printf("PASS: actual TX ring %s, %zu FIFO bytes; no device setup\n", serial ? "serial control" : status_mode ? "SPSC plus actual public status" : "SPSC concurrent", extent);
    return 0;
}
