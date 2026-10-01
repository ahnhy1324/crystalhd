// SPDX-License-Identifier: GPL-2.0-or-later
/* Extracted admission helpers with real locks; not physical hot-unplug testing. */
#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

typedef uint64_t u64;
#define U64_MAX UINT64_MAX
#define READ_ONCE(value) __atomic_load_n(&(value), __ATOMIC_SEQ_CST)
#define WRITE_ONCE(value, update) \
    __atomic_store_n(&(value), (update), __ATOMIC_SEQ_CST)
#define dev_err(...) ((void)0)
#define dev_info(...) ((void)0)

struct rw_semaphore {
    pthread_rwlock_t native;
    unsigned readers, writers;
};
struct crystalhd_adp {
    struct rw_semaphore user_lock;
    pthread_mutex_t legacy_gate;
    struct { const void *session_owner, *session_lifetime_owner, *session_lifetime_ops; } cmds;
    u64 generation;
    unsigned present;
    bool hw_accessible;
};
struct crystalhd_user;
#include "lifetime-binding.h"
#include "access-binding.h"

static struct rw_semaphore chd_device_lock;
static struct crystalhd_adp adapter, *g_adp_info;
static struct crystalhd_adp *chd_dma_quarantine;
static u64 chd_device_generation;
static pthread_mutex_t audit = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed = PTHREAD_COND_INITIALIZER;
static _Thread_local unsigned global_read, global_write, user_read, user_write;
static char events[128];
static size_t event_count;
static struct crystalhd_device_access *watched_access;
static bool drop_present, drop_ready, writer_blocked, writer_finished;
static unsigned scenarios, checks;
static void (*user_unlocked_hook)(void);

static void check(bool condition, const char *message)
{
    checks++;
    if (!condition) {
        fprintf(stderr, "Device access: %s\n", message);
        abort();
    }
}

static void must(int rc)
{
    if (rc) {
        fprintf(stderr, "Device access: pthread/clock error %d\n", rc);
        abort();
    }
}

static int mutex_trylock(pthread_mutex_t *lock)
{
    int rc = pthread_mutex_trylock(lock);
    if (rc == EBUSY) return 0;
    must(rc); return 1;
}
static void mutex_lock(pthread_mutex_t *lock) { must(pthread_mutex_lock(lock)); }
static void mutex_unlock(pthread_mutex_t *lock) { must(pthread_mutex_unlock(lock)); }

static struct timespec deadline(void)
{
    struct timespec limit;
    must(clock_gettime(CLOCK_REALTIME, &limit));
    limit.tv_sec += 10;
    return limit;
}

static void event(char value)
{
    assert(event_count + 1 < sizeof(events));
    events[event_count++] = value;
    events[event_count] = 0;
}

static void down_read(struct rw_semaphore *lock)
{
    struct timespec limit = deadline();
    bool global = lock == &chd_device_lock;

    assert(!global_write && !user_read && !user_write);
    assert(global ? !global_read : global_read == 1);
    assert(global || lock == &adapter.user_lock);
    must(pthread_rwlock_timedrdlock(&lock->native, &limit));
    must(pthread_mutex_lock(&audit));
    assert(!lock->writers);
    lock->readers++;
    if (global) {
        global_read++;
        event('G');
    } else {
        user_read++;
        event('R');
        if (drop_present)
            WRITE_ONCE(adapter.present, 0);
        if (drop_ready)
            adapter.hw_accessible = false;
    }
    must(pthread_mutex_unlock(&audit));
}

static void down_write(struct rw_semaphore *lock)
{
    struct timespec limit = deadline();
    bool global = lock == &chd_device_lock;

    assert(!global_write && !user_read && !user_write);
    assert(global ? !global_read : global_read == 1);
    assert(global || lock == &adapter.user_lock);
    must(pthread_rwlock_timedwrlock(&lock->native, &limit));
    must(pthread_mutex_lock(&audit));
    assert(!lock->readers && !lock->writers);
    lock->writers++;
    if (global) {
        global_write++;
        event('X');
    } else {
        user_write++;
        event('W');
        if (drop_present)
            WRITE_ONCE(adapter.present, 0);
        if (drop_ready)
            adapter.hw_accessible = false;
    }
    must(pthread_mutex_unlock(&audit));
}

static void assert_empty_before_unlock(void)
{
    if (watched_access)
        assert(!watched_access->adp && !watched_access->exclusive);
}

static int down_read_trylock(struct rw_semaphore *lock)
{
    int rc;

    assert(lock == &chd_device_lock && !global_read && !global_write &&
           !user_read && !user_write);
    rc = pthread_rwlock_tryrdlock(&lock->native);
    if (rc == EBUSY) return 0;
    must(rc);
    must(pthread_mutex_lock(&audit));
    assert(!lock->writers);
    lock->readers++; global_read++; event('G');
    must(pthread_mutex_unlock(&audit));
    return 1;
}

static int down_write_trylock(struct rw_semaphore *lock)
{
    int rc;

    assert(lock == &adapter.user_lock && global_read == 1 &&
           !global_write && !user_read && !user_write);
    rc = pthread_rwlock_trywrlock(&lock->native);
    if (rc == EBUSY) return 0;
    must(rc);
    must(pthread_mutex_lock(&audit));
    assert(!lock->writers && !lock->readers);
    lock->writers++; user_write++; event('W');
    if (drop_present) WRITE_ONCE(adapter.present, 0);
    if (drop_ready) adapter.hw_accessible = false;
    must(pthread_mutex_unlock(&audit));
    return 1;
}

static void up_read(struct rw_semaphore *lock)
{
    must(pthread_mutex_lock(&audit));
    assert(lock->readers && !lock->writers);
    assert_empty_before_unlock();
    if (lock == &chd_device_lock) {
        assert(global_read == 1 && !user_read && !user_write);
        global_read--;
        event('g');
    } else {
        assert(lock == &adapter.user_lock && global_read == 1);
        assert(user_read == 1 && !user_write);
        user_read--;
        event('r');
    }
    lock->readers--;
    must(pthread_mutex_unlock(&audit));
    must(pthread_rwlock_unlock(&lock->native));
    if (lock == &adapter.user_lock && user_unlocked_hook) {
        void (*hook)(void) = user_unlocked_hook;
        user_unlocked_hook = NULL;
        hook();
    }
}

static void up_write(struct rw_semaphore *lock)
{
    must(pthread_mutex_lock(&audit));
    assert(lock->writers == 1 && !lock->readers);
    assert_empty_before_unlock();
    if (lock == &chd_device_lock) {
        assert(global_write == 1 && !global_read && !user_read && !user_write);
        global_write--;
        event('x');
    } else {
        assert(lock == &adapter.user_lock && global_read == 1);
        assert(user_write == 1 && !user_read);
        user_write--;
        event('w');
    }
    lock->writers--;
    must(pthread_mutex_unlock(&audit));
    must(pthread_rwlock_unlock(&lock->native));
}

#define lockdep_assert_held_write(lock) \
    assert((lock) == &chd_device_lock && global_write == 1)
#include "access-functions.h"

static int probe_admission_fixture(void)
{
    u64 generation;
    int rc;

#include "probe-admission.h"
out:
    up_write(&chd_device_lock);
    return rc;
}

static void reset(void)
{
    assert(!chd_device_lock.readers && !chd_device_lock.writers);
    assert(!adapter.user_lock.readers && !adapter.user_lock.writers);
    assert(!global_read && !global_write && !user_read && !user_write);
    scenarios++;
    event_count = 0;
    events[0] = 0;
    watched_access = NULL;
    drop_present = drop_ready = writer_blocked = writer_finished = false;
    chd_device_generation = 41;
    adapter.generation = 41;
    adapter.present = 1;
    adapter.hw_accessible = true;
    g_adp_info = &adapter;
    memset(&adapter.cmds, 0, sizeof(adapter.cmds));
}

static void balanced(const char *expected)
{
    check(!strcmp(events, expected), "literal acquisition/unlock order");
    check(!chd_device_lock.readers && !chd_device_lock.writers &&
          !adapter.user_lock.readers && !adapter.user_lock.writers &&
          !global_read && !global_write && !user_read && !user_write,
          "all admission locks balanced");
}

static void success_cases(void)
{
    for (unsigned exclusive = 0; exclusive < 2; exclusive++) {
        struct crystalhd_device_access access = {0};

        reset();
        watched_access = &access;
        check(!crystalhd_device_enter(41, exclusive, &access),
              "current ready device admits access");
        check(access.adp == &adapter && access.exclusive == !!exclusive,
              "success publishes exact adapter and lock mode");
        check(chd_device_lock.readers == 1 && !chd_device_lock.writers &&
              adapter.user_lock.readers == !exclusive &&
              adapter.user_lock.writers == exclusive,
              "success retains both requested locks");
        check(!strcmp(events, exclusive ? "GW" : "GR"),
              "global reader precedes requested user lock");
        crystalhd_device_exit(&access);
        check(!access.adp && !access.exclusive, "exit empties handle");
        crystalhd_device_exit(&access);
        crystalhd_device_exit(NULL);
        balanced(exclusive ? "GWwg" : "GRrg");
    }
}

static void failure_cases(void *unreadable)
{
    for (unsigned exclusive = 0; exclusive < 2; exclusive++) {
        for (unsigned failure = 0; failure < 10; failure++) {
            /* Inactive, but deliberately dirty: failures must clear output. */
            struct crystalhd_device_access access = {&adapter, true};
            u64 generation = 41;
            int expected = -ENODEV;
            bool user_locked = failure >= 5 && failure <= 8;

            reset();
            watched_access = &access;
            switch (failure) {
            case 0:
                g_adp_info = unreadable;
                check(crystalhd_device_enter(41, exclusive, NULL) == -EINVAL,
                      "null output rejected before locks or adapter access");
                balanced("");
                continue;
            case 1:
                generation = 0;
                g_adp_info = unreadable;
                break;
            case 2:
                generation = 40;
                g_adp_info = unreadable;
                break;
            case 3:
                g_adp_info = NULL;
                break;
            case 4:
                adapter.present = 0;
                adapter.hw_accessible = false;
                break;
            case 5:
                drop_present = true;
                break;
            case 6:
                drop_present = drop_ready = true;
                break;
            case 7:
                adapter.hw_accessible = false;
                expected = -EAGAIN;
                break;
            case 8:
                drop_ready = true;
                expected = -EAGAIN;
                break;
            case 9:
                generation = UINT64_C(0x100000029);
                g_adp_info = unreadable;
                break;
            }
            check(crystalhd_device_enter(generation, exclusive, &access) == expected,
                  "admission failure and ENODEV precedence");
            check(!access.adp && !access.exclusive,
                  "failure clears all published handle fields");
            crystalhd_device_exit(&access);
            balanced(user_locked ? (exclusive ? "GWwg" : "GRrg") : "Gg");
        }
    }
}

static void generation_cases(void)
{
    u64 generation = 99;
    struct crystalhd_device_access access = {0};
    struct crystalhd_adp *same_address = &adapter;

    reset();
    chd_device_generation = 0;
    down_write(&chd_device_lock);
    check(crystalhd_device_reserve_generation(NULL) == -EINVAL &&
          chd_device_generation == 0, "null generation output consumes no token");
    check(!crystalhd_device_reserve_generation(&generation) && generation == 1 &&
          chd_device_generation == 1, "first token is one, never zero");
    /* A failed probe may consume its reservation, but cannot reuse it. */
    generation = 99;
    check(!crystalhd_device_reserve_generation(&generation) && generation == 2 &&
          chd_device_generation == 2, "failed-probe reservation is not recycled");
    up_write(&chd_device_lock);
    balanced("Xx");

    reset();
    chd_device_generation = UINT64_C(0xffffffff);
    down_write(&chd_device_lock);
    check(!crystalhd_device_reserve_generation(&generation) &&
          generation == UINT64_C(0x100000000) &&
          chd_device_generation == UINT64_C(0x100000000),
          "generation crosses 32-bit boundary without truncation");
    up_write(&chd_device_lock);
    balanced("Xx");

    reset();
    chd_device_generation = UINT64_MAX - 1;
    down_write(&chd_device_lock);
    check(!crystalhd_device_reserve_generation(&generation) &&
          generation == UINT64_MAX && chd_device_generation == UINT64_MAX,
          "last nonzero token can be reserved");
    for (unsigned attempt = 0; attempt < 2; attempt++) {
        generation = 99;
        check(crystalhd_device_reserve_generation(&generation) == -EOVERFLOW &&
              !generation && chd_device_generation == UINT64_MAX,
              "overflow clears output, saturates counter and cannot revive tokens");
    }
    up_write(&chd_device_lock);
    adapter.generation = UINT64_MAX;
    watched_access = &access;
    check(!crystalhd_device_enter(UINT64_MAX, true, &access) &&
          access.adp == &adapter && access.exclusive,
          "last generation remains usable after reservation overflow");
    crystalhd_device_exit(&access);
    balanced("XxGWwg");

    reset();
    down_write(&chd_device_lock);
    check(!crystalhd_device_reserve_generation(&generation) && generation == 42,
          "reprobe gets the next generation");
    adapter.generation = generation;
    up_write(&chd_device_lock);
    watched_access = &access;
    check(g_adp_info == same_address &&
          crystalhd_device_enter(41, false, &access) == -ENODEV,
          "exact adapter pointer reuse does not revive stale generation");
    check(!access.adp && !access.exclusive, "stale same-address output stays empty");
    check(!crystalhd_device_enter(42, false, &access) && access.adp == same_address,
          "same address is usable with its new token");
    crystalhd_device_exit(&access);
    balanced("XxGgGRrg");
}

static void wait_for(bool *condition)
{
    struct timespec limit = deadline();
    must(pthread_mutex_lock(&audit));
    while (!*condition)
        must(pthread_cond_timedwait(&changed, &audit, &limit));
    must(pthread_mutex_unlock(&audit));
}

static void *remove_writer(void *unused)
{
    int rc;
    (void)unused;

    /* Removal cancels core work before waiting for active operations. */
    WRITE_ONCE(adapter.present, 0);
    /* EBUSY is a direct lock oracle, not a sleep-based absence assertion. */
    rc = pthread_rwlock_trywrlock(&chd_device_lock.native);
    assert(rc == EBUSY);
    must(pthread_mutex_lock(&audit));
    assert(chd_device_lock.readers == 1 && !chd_device_lock.writers);
    writer_blocked = true;
    must(pthread_cond_broadcast(&changed));
    must(pthread_mutex_unlock(&audit));

    down_write(&chd_device_lock);
    adapter.hw_accessible = false;
    g_adp_info = NULL;
    up_write(&chd_device_lock);
    must(pthread_mutex_lock(&audit));
    writer_finished = true;
    must(pthread_cond_broadcast(&changed));
    must(pthread_mutex_unlock(&audit));
    return NULL;
}

static void removal_cases(void)
{
    for (unsigned exclusive = 0; exclusive < 2; exclusive++) {
        struct crystalhd_device_access access = {0};
        struct crystalhd_file surviving_file = {
            .user = (struct crystalhd_user *)(uintptr_t)1, .generation = 41,
        };
        const u64 surviving_token = 41;
        pthread_t writer;

        reset();
        watched_access = &access;
        check(!crystalhd_device_enter(surviving_token, exclusive, &access),
              "active access begins before removal writer");
        must(pthread_create(&writer, NULL, remove_writer, NULL));
        wait_for(&writer_blocked);
        /* The writer's EBUSY probe proves exclusion while access is live. */
        check(access.adp == &adapter && access.exclusive == !!exclusive,
              "borrowed adapter remains valid while removal writer is excluded");
        check(READ_ONCE(access.adp->present) == 0 &&
              chd_device_lock.readers == 1 && !chd_device_lock.writers &&
              adapter.user_lock.readers == !exclusive &&
              adapter.user_lock.writers == exclusive,
              "removal cancellation is visible while borrowed adapter locks remain held");
        crystalhd_device_exit(&access);
        wait_for(&writer_finished);
        must(pthread_join(writer, NULL));
        check(!g_adp_info && !adapter.present && !adapter.hw_accessible,
              "removal writer proceeds immediately after the access lifetime");
        check(surviving_file.generation == 41 && surviving_file.user &&
              surviving_token == 41,
              "surviving token and file binding need no locks or adapter ownership");
        check(crystalhd_device_enter(surviving_file.generation, exclusive,
                                     &access) == -ENODEV &&
              !access.adp && !access.exclusive,
              "surviving file token cannot access removed device");
        balanced(exclusive ? "GWwgXxGg" : "GRrgXxGg");
    }
}

static void probe_admission_cases(void *unreadable)
{
    for (unsigned variant = 0; variant < 5; variant++) {
        int expected = variant == 0 ? 0 : variant == 4 ? -EOVERFLOW : -EBUSY;
        u64 original;

        reset();
        g_adp_info = variant == 1 || variant == 3 ? unreadable : NULL;
        chd_dma_quarantine = variant == 2 || variant == 3 ? unreadable : NULL;
        if (variant == 4) chd_device_generation = U64_MAX;
        original = chd_device_generation;
        check(probe_admission_fixture() == expected,
              "actual probe prefix rejects occupied/quarantined/exhausted devices before setup");
        check(chd_device_generation == original + (variant == 0),
              "quarantine rejection consumes no generation and dereferences neither stale pointer");
        balanced("Xx");
        chd_dma_quarantine = NULL;
    }
}

static void nonblocking_cases(void)
{
    for (unsigned failure = 0; failure < 12; failure++) {
        struct crystalhd_device_access access = {0};
        int expected = 0;
        const char *order = "GWwg";
        u64 generation = 41;

        reset();
        watched_access = &access;
        switch (failure) {
        case 1: generation = 0; expected = -ENODEV; order = "Gg"; break;
        case 2: generation = 40; expected = -ENODEV; order = "Gg"; break;
        case 3: g_adp_info = NULL; expected = -ENODEV; order = "Gg"; break;
        case 4: adapter.present = 0; expected = -ENODEV; order = "Gg"; break;
        case 5: drop_present = true; expected = -ENODEV; break;
        case 6: adapter.hw_accessible = false; expected = -EAGAIN; break;
        case 7: drop_ready = true; expected = -EAGAIN; break;
        case 8:
            /* An established TX holds the native reader. A failed claim
             * must not enqueue a writer that would obstruct its RX peer.
             */
            must(pthread_rwlock_rdlock(&adapter.user_lock.native));
            expected = -EBUSY; order = "Gg"; break;
        case 9:
            must(pthread_rwlock_wrlock(&chd_device_lock.native));
            expected = -EBUSY; order = ""; break;
        case 10:
            must(pthread_rwlock_wrlock(&adapter.user_lock.native));
            expected = -EBUSY; order = "Gg"; break;
        case 11:
            mutex_lock(&adapter.legacy_gate);
            expected = -EBUSY; order = "Gg"; break;
        }
        check(crystalhd_device_try_enter_exclusive(generation, &access) == expected,
              "new-session admission is nonblocking and preserves lifetime/readiness guards");
        if (!expected) {
            check(access.adp == &adapter && access.exclusive && user_write == 1,
                  "successful new-session admission retains exclusive owner-check protection");
            crystalhd_device_exit(&access);
        } else {
            check(!access.adp && !access.exclusive,
                  "failed new-session admission publishes no borrowed adapter");
        }
        if (failure == 8) {
            /* RX can still enter while the original TX reader remains. */
            must(pthread_rwlock_tryrdlock(&adapter.user_lock.native));
            must(pthread_rwlock_unlock(&adapter.user_lock.native));
            must(pthread_rwlock_unlock(&adapter.user_lock.native));
        }
        if (failure == 9)
            must(pthread_rwlock_unlock(&chd_device_lock.native));
        if (failure == 10)
            must(pthread_rwlock_unlock(&adapter.user_lock.native));
        if (failure == 11)
            mutex_unlock(&adapter.legacy_gate);
        balanced(order);
    }
    check(crystalhd_device_try_enter_exclusive(41, NULL) == -EINVAL,
          "new-session admission validates output handle");
}

static void legacy_admission_cases(void)
{
    for (unsigned native = 0; native < 2; native++) {
        for (unsigned writer = 0; writer < 2; writer++) {
            bool exclusive = writer, held;
            reset();
            if (native) {
                adapter.cmds.session_owner = &adapter;
                adapter.cmds.session_lifetime_owner = &adapter;
                adapter.cmds.session_lifetime_ops = &adapter;
                /* A real outstanding TX reader remains held throughout. */
                must(pthread_rwlock_rdlock(&adapter.user_lock.native));
            }
            down_read(&chd_device_lock);
            held = crystalhd_legacy_enter(&adapter, &exclusive);
            check(held == !!native && exclusive == (writer && !native),
                  "native-held legacy admission never requests a writer");
            check(mutex_trylock(&adapter.legacy_gate) == !native,
                  "native slot operations retain gate; ordinary admission releases it");
            if (!native) mutex_unlock(&adapter.legacy_gate);
            if (native) {
                must(pthread_rwlock_tryrdlock(&adapter.user_lock.native));
                must(pthread_rwlock_unlock(&adapter.user_lock.native));
            }
            crystalhd_legacy_exit(&adapter, exclusive, held);
            up_read(&chd_device_lock);
            if (native) must(pthread_rwlock_unlock(&adapter.user_lock.native));
            check(mutex_trylock(&adapter.legacy_gate), "legacy admission gate released on exit");
            mutex_unlock(&adapter.legacy_gate);
            balanced(writer && !native ? "GRrWwg" : "GRrg");
        }
    }
}

static void *native_claim_in_upgrade(void *opaque)
{
    struct crystalhd_device_access access = {0};
    int *result = opaque;
    *result = crystalhd_device_try_enter_exclusive(41, &access);
    assert(!access.adp && !global_read && !user_write);
    return NULL;
}

static void compete_in_upgrade(void)
{
    pthread_t thread;
    int result = 0;
    must(pthread_create(&thread, NULL, native_claim_in_upgrade, &result));
    must(pthread_join(thread, NULL));
    check(result == -EBUSY, "native claim cannot publish in legacy read-to-write upgrade gap");
}

static void legacy_upgrade_race(void)
{
    bool exclusive = true;
    reset();
    down_read(&chd_device_lock);
    user_unlocked_hook = compete_in_upgrade;
    check(!crystalhd_legacy_enter(&adapter, &exclusive) && exclusive,
          "ordinary legacy writer keeps original exclusive callback contract");
    crystalhd_legacy_exit(&adapter, exclusive, false);
    up_read(&chd_device_lock);
    balanced("GRrGgWwg");
}

int main(void)
{
    long page_size = sysconf(_SC_PAGESIZE);
    void *unreadable;

    assert(page_size > 0);
    unreadable = mmap(NULL, (size_t)page_size, PROT_NONE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(unreadable != MAP_FAILED);
    must(pthread_rwlock_init(&chd_device_lock.native, NULL));
    must(pthread_rwlock_init(&adapter.user_lock.native, NULL));
    must(pthread_mutex_init(&adapter.legacy_gate, NULL));
    success_cases();
    nonblocking_cases();
    legacy_admission_cases();
    legacy_upgrade_race();
    failure_cases(unreadable);
    generation_cases();
    removal_cases();
    probe_admission_cases(unreadable);
    must(pthread_rwlock_destroy(&adapter.user_lock.native));
    must(pthread_mutex_destroy(&adapter.legacy_gate));
    must(pthread_rwlock_destroy(&chd_device_lock.native));
    must(pthread_cond_destroy(&changed));
    must(pthread_mutex_destroy(&audit));
    assert(!munmap(unreadable, (size_t)page_size));
    printf("Device access: %u scenarios, %u checks, 0 failures\n", scenarios, checks);
    return 0;
}
