/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Production ioctl admission and BUSY retry, real pthread rwlock/mutex.
 * Hardware always returns BUSY; condition barriers control retry sleep.
 * No device is opened and no native hardware hang is claimed. */
#define _GNU_SOURCE
#include "bc_dts_types.h"
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
struct _BC_DTS_PROC_OUT;
#include "7411d.h"
#include "bc_dts_defs.h"
typedef struct C011_PIB C011_PIB;
#include "bc_dts_glob_lnx.h"
typedef uint8_t u8;
typedef uint32_t u32;
typedef uint64_t u64;
#define READ_ONCE(x) __atomic_load_n(&(x), __ATOMIC_ACQUIRE)
#define WRITE_ONCE(x, v) __atomic_store_n(&(x), (v), __ATOMIC_RELEASE)
#define ERESTARTSYS 512
#define CAP_SYS_RAWIO 1
#define EPOLLERR 8
#define EPOLLHUP 16
#define dev_dbg(dev, ...) ((void)(dev))
#define dev_err(dev, ...) ((void)(dev))
#define time_after(a, b) ((long)((b) - (a)) < 0)
#define time_after_eq(a, b) ((long)((a) - (b)) >= 0)
#define msecs_to_jiffies(x) ((unsigned long)(x))
static unsigned long jiffies;
typedef int wait_queue_head_t;
struct device {
    int ignored;
};
struct pci_dev {
    struct device dev;
    int irq;
};
struct crystalhd_tx_buffer;
struct crystalhd_tx_buffer_ops {
    void (*get)(const struct crystalhd_tx_buffer *);
    void (*put)(const struct crystalhd_tx_buffer *);
};
struct crystalhd_tx_buffer {
    uint32_t bytes;
    void *cookie;
    const struct crystalhd_tx_buffer_ops *ops;
};
struct crystalhd_hw {
    int ignored;
};
struct crystalhd_adp;
struct crystalhd_user {
    unsigned uid, in_use;
};
struct crystalhd_cmd {
    unsigned state, cin_wait_exit, tx_list_id;
    struct crystalhd_adp *adp;
    struct crystalhd_hw *hw_ctx;
    const void *session_owner, *session_lifetime_owner, *session_lifetime_ops;
};
struct mutex {
    pthread_mutex_t real;
};
struct crystalhd_adp {
    bool present;
    struct pci_dev *pdev;
    pthread_rwlock_t user_lock;
    struct mutex tx_lock, legacy_gate;
    struct crystalhd_cmd cmds;
};

struct file {
    struct crystalhd_file *private_data;
};

typedef BC_STATUS (*crystalhd_cmd_proc)(struct crystalhd_cmd *, crystalhd_ioctl_data *);
#include "tx-release-types.h"
static struct pci_dev pci;
static struct crystalhd_adp adp;
static struct crystalhd_hw hw;
static struct crystalhd_user owner = {.uid = 0, .in_use = 1};
static struct crystalhd_file binding = {.user = &owner, .generation = 1};
static struct file fd = {.private_data = &binding};
static uint64_t chd_device_generation = 1;
static _Thread_local int actor;
static pthread_mutex_t control = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed = PTHREAD_COND_INITIALIZER;
static int release_requests;
static int read_held, write_pending, write_entered, posts[6], sleeps[6], wake[6], done[6],
    signals[6], refs;
static long result[6];
static void check(bool yes, const char *why) {
    if (!yes) {
        fprintf(stderr, "FAIL: %s\n", why);
        abort();
    }
}
static void down_read(pthread_rwlock_t *l) {
    check(!pthread_rwlock_rdlock(l), "read lock");
    pthread_mutex_lock(&control);
    read_held++;
    pthread_cond_broadcast(&changed);
    pthread_mutex_unlock(&control);
}
static void up_read(pthread_rwlock_t *l) {
    pthread_mutex_lock(&control);
    read_held--;
    pthread_cond_broadcast(&changed);
    pthread_mutex_unlock(&control);
    check(!pthread_rwlock_unlock(l), "read unlock");
}
static void down_write(pthread_rwlock_t *l) {
    pthread_mutex_lock(&control);
    write_pending++;
    pthread_cond_broadcast(&changed);
    pthread_mutex_unlock(&control);
    check(!pthread_rwlock_wrlock(l), "write lock");
    pthread_mutex_lock(&control);
    write_entered++;
    pthread_cond_broadcast(&changed);
    pthread_mutex_unlock(&control);
}
static void up_write(pthread_rwlock_t *l) { check(!pthread_rwlock_unlock(l), "write unlock"); }
static int mutex_lock_interruptible(struct mutex *l) { return pthread_mutex_lock(&l->real); }
static void mutex_lock(struct mutex *l) {
    pthread_mutex_lock(&control);
    if (l == &binding.release_lock) release_requests++;
    pthread_cond_broadcast(&changed);
    pthread_mutex_unlock(&control);
    check(!pthread_mutex_lock(&l->real), "mutex lock");
}
static void mutex_unlock(struct mutex *l) {
    check(!pthread_mutex_unlock(&l->real), "mutex unlock");
}
static struct crystalhd_adp *chd_get_adp(void) { return &adp; }
static struct device *chddev(void) { return &pci.dev; }
static bool capable(int cap) {
    (void)cap;
    return true;
}
static bool crystalhd_rawio_command(unsigned cmd) {
    (void)cmd;
    return false;
}
static bool crystalhd_legacy_color_command(struct crystalhd_adp *a, unsigned uid, unsigned cmd) {
    (void)a;
    (void)uid;
    (void)cmd;
    return false;
}
static BC_STATUS legacy_handler(struct crystalhd_cmd *c, crystalhd_ioctl_data *data)
{ (void)c; (void)data; return BC_STS_SUCCESS; }
static BC_STATUS crystalhd_native_legacy_notify(struct crystalhd_cmd *c, crystalhd_ioctl_data *data)
{ (void)c; (void)data; check(false,"legacy fixture cannot enter native notify"); return BC_STS_ERR_USAGE; }
static BC_STATUS crystalhd_native_legacy_release(struct crystalhd_cmd *c, crystalhd_ioctl_data *data)
{ (void)c; (void)data; check(false,"legacy fixture cannot enter native release"); return BC_STS_ERR_USAGE; }
static crystalhd_cmd_proc crystalhd_get_cmd_proc(struct crystalhd_cmd *c, unsigned cmd,
                                                 struct crystalhd_user *u) {
    (void)c;
    (void)cmd;
    (void)u;
    return legacy_handler;
}
static void crystalhd_rx_poll_wake(struct crystalhd_adp *a, int events) {
    (void)a;
    (void)events;
}
static void crystalhd_set_event(wait_queue_head_t *e) { (void)e; }
#define crystalhd_create_event(e) (*(e) = 0)
static int wait_retry(void) {
    pthread_mutex_lock(&control);
    int n = ++sleeps[actor];
    pthread_cond_broadcast(&changed);
    while (wake[actor] < n)
        pthread_cond_wait(&changed, &control);
    int rc = READ_ONCE(signals[actor]) ? -EINTR : -EBUSY;
    pthread_mutex_unlock(&control);
    return rc;
}
#define crystalhd_wait_on_event(e, c, t, r, n)                                                     \
    do {                                                                                           \
        (void)(e);                                                                                 \
        (void)(c);                                                                                 \
        (void)(t);                                                                                 \
        (void)(n);                                                                                 \
        (r) = wait_retry();                                                                        \
    } while (0)
#define crystalhd_wait_on_event_until(e, c, t, r, n) crystalhd_wait_on_event(e, c, t, r, n)
static bool signal_pending(int ignored) {
    (void)ignored;
    return READ_ONCE(signals[actor]);
}
#define current 0
static void buffer_get(const struct crystalhd_tx_buffer *b) {
    (void)b;
    __atomic_add_fetch(&refs, 1, __ATOMIC_SEQ_CST);
}
static void buffer_put(const struct crystalhd_tx_buffer *b) {
    (void)b;
    __atomic_sub_fetch(&refs, 1, __ATOMIC_SEQ_CST);
}
static void crystalhd_tx_buffer_get(const struct crystalhd_tx_buffer *b) { b->ops->get(b); }
static void crystalhd_tx_buffer_put(struct crystalhd_adp *a, const struct crystalhd_tx_buffer *b) {
    (void)a;
    b->ops->put(b);
}
static BC_STATUS crystalhd_hw_post_tx(struct crystalhd_hw *h, const struct crystalhd_tx_buffer *b,
                                      void (*fn)(void *, BC_STATUS), void *cookie, uint32_t *id,
                                      u8 flags) {
    (void)h;
    (void)b;
    (void)fn;
    (void)cookie;
    (void)id;
    (void)flags;
    pthread_mutex_lock(&control);
    posts[actor]++;
    pthread_cond_broadcast(&changed);
    pthread_mutex_unlock(&control);
    return BC_STS_BUSY;
}
static void crystalhd_hw_cancel_all_tx(struct crystalhd_hw *h) {
    (void)h;
    abort();
}
static bool crystalhd_hw_retain_tx_buffer(struct crystalhd_hw *h,
                                          const struct crystalhd_tx_buffer *b, void *c) {
    (void)h;
    (void)b;
    (void)c;
    abort();
}
static void synchronize_irq(int irq) {
    (void)irq;
    abort();
}
static int release_status;
static int data_refs;
#define GFP_KERNEL 0
static void *kzalloc(size_t bytes, int flags)
{
    check(bytes == sizeof(crystalhd_ioctl_data) && flags == GFP_KERNEL,
          "private RELEASE envelope allocation");
    void *memory = calloc(1, bytes);
    check(memory != NULL, "test envelope allocation succeeds");
    __atomic_add_fetch(&data_refs, 1, __ATOMIC_SEQ_CST);
    return memory;
}
static void kfree(void *memory)
{
    free(memory);
    __atomic_sub_fetch(&data_refs, 1, __ATOMIC_SEQ_CST);
}
static int chd_dec_proc_user_data(struct crystalhd_adp *a, crystalhd_ioctl_data *d,
                                  unsigned long ua, bool set, bool compat) {
    (void)a;
    (void)d;
    (void)set;
    (void)compat;
    return ua ? 0 : -EFAULT;
}
static int chd_dec_api_cmd(struct crystalhd_adp *, unsigned long, unsigned, unsigned,
                           crystalhd_cmd_proc, bool, crystalhd_ioctl_data *, const uint32_t *);

static BC_STATUS crystalhd_flea_request_tx_wrap(struct crystalhd_hw *device, uint32_t bytes)
{ (void)device; (void)bytes; check(false,"legacy cancellation cannot request typed WRAP"); return BC_STS_NO_DATA; }
#include "tx-release-functions.h"
static int chd_dec_api_cmd(struct crystalhd_adp *a, unsigned long ua, unsigned uid, unsigned cmd,
                           crystalhd_cmd_proc fn, bool compat, crystalhd_ioctl_data *prepared,
                           const uint32_t *stop) {
    (void)ua;
    (void)fn;
    (void)compat;
    if (cmd == BCM_IOC_PROC_INPUT) {
        static const struct crystalhd_tx_buffer_ops ops = {buffer_get, buffer_put};
        const struct crystalhd_tx_buffer b = {.cookie=&adp, .ops=&ops};
        return crystalhd_tx_transfer_common(&a->cmds, &b, 0, false, 0, stop);
    }
    if (cmd == BCM_IOC_RELEASE) {
        kfree(prepared);
        if (release_status)
            return release_status;
        if (uid == owner.uid) {
            WRITE_ONCE(a->cmds.cin_wait_exit, 1);
            owner.in_use = 0;
        }
        return BC_STS_SUCCESS;
    }
    abort();
}
static struct file *release_fd = &fd;
static void *call(void *arg) {
    actor = (int)(intptr_t)arg;
    result[actor] = chd_dec_ioctl_common(
        actor >= 3 ? release_fd : &fd, actor >= 3 ? BCM_IOC_RELEASE : BCM_IOC_PROC_INPUT, 1, false);
    pthread_mutex_lock(&control);
    done[actor] = 1;
    pthread_cond_broadcast(&changed);
    pthread_mutex_unlock(&control);
    return NULL;
}
static void until(int *value, int minimum) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += 10;
    pthread_mutex_lock(&control);
    while (*value < minimum) {
        int rc = pthread_cond_timedwait(&changed, &control, &ts);
        check(rc == 0, "milestone reached before watchdog");
    }
    pthread_mutex_unlock(&control);
}
static void advance(int id) {
    pthread_mutex_lock(&control);
    wake[id] = sleeps[id];
    pthread_cond_broadcast(&changed);
    pthread_mutex_unlock(&control);
}
static void launch(pthread_t *t, int id) {
    check(!pthread_create(t, NULL, call, (void *)(intptr_t)id), "thread create");
}
static void reset(void) {
    adp.present = true;
    adp.pdev = &pci;
    adp.cmds = (struct crystalhd_cmd){.adp = &adp, .hw_ctx = &hw};
    owner.in_use = 1;
    binding.user = &owner;
    binding.tx_stopping = 0;
    release_fd = &fd;
    read_held = write_pending = write_entered = refs = 0;
    release_requests = release_status = 0;
    for (int i = 0; i < 6; i++)
        posts[i] = sleeps[i] = wake[i] = done[i] = signals[i] = 0;
    check(!pthread_rwlock_init(&adp.user_lock, NULL), "rwlock init");
    check(!pthread_mutex_init(&adp.tx_lock.real, NULL), "mutex init");
    check(!pthread_mutex_init(&adp.legacy_gate.real, NULL), "legacy gate init");
    check(!pthread_mutex_init(&binding.release_lock.real, NULL), "release mutex init");
}
static void finish(pthread_t *threads, int n) {
    for (int i = 0; i < n; i++)
        check(!pthread_join(threads[i], NULL), "join");
    check(refs == 0 && read_held == 0 && data_refs == 0, "no backing or reader left");
    check(!pthread_rwlock_destroy(&adp.user_lock), "rwlock destroy");
    check(!pthread_mutex_destroy(&adp.tx_lock.real), "mutex destroy");
    check(!pthread_mutex_destroy(&adp.legacy_gate.real), "legacy gate destroy");
    check(!pthread_mutex_destroy(&binding.release_lock.real), "release mutex destroy");
}
int main(void) {
    pthread_t threads[4];
    reset();
    launch(&threads[0], 1);
    until(&sleeps[1], 1);
    launch(&threads[1], 2);
    until(&read_held, 2);
    launch(&threads[2], 3);
    until(&write_pending, 1);
    check(READ_ONCE(binding.tx_stopping) == 1,
          "RELEASE publishes persistent file stopping before exclusive admission");
    advance(1);
    finish(threads, 3);
    check(result[1] == BC_STS_CMD_CANCELLED && result[2] == BC_STS_CMD_CANCELLED &&
              result[3] == BC_STS_SUCCESS,
          "both old readers cancel and RELEASE completes");
    check(posts[1] == 1 && posts[2] == 0,
          "no BUSY retry or queued input posts after observed stop");
    puts("FIX PASS: BUSY+queued input cancels, RELEASE admitted, references balanced");
    reset();
    struct crystalhd_user monitor = {.uid = 1, .in_use = 1};
    struct crystalhd_file mb = {.user = &monitor, .generation = 1};
    struct file mf = {.private_data = &mb};
    check(!pthread_mutex_init(&mb.release_lock.real, NULL), "monitor release mutex init");
    release_fd = &mf;
    launch(&threads[0], 1);
    until(&sleeps[1], 1);
    launch(&threads[1], 3);
    until(&write_pending, 1);
    check(READ_ONCE(mb.tx_stopping) == 1 && READ_ONCE(binding.tx_stopping) == 0,
          "monitor RELEASE cannot stop decoder file");
    WRITE_ONCE(signals[1], 1);
    advance(1);
    finish(threads, 2);
    check(result[1] == BC_STS_IO_USER_ABORT && result[3] == BC_STS_SUCCESS && owner.in_use == 1,
          "unrelated release preserves decoder ownership");
    check(!pthread_mutex_destroy(&mb.release_lock.real), "monitor release mutex destroy");
    puts("CONTROL PASS: unrelated monitor release does not cancel decoder input");
    reset();
    release_status = BC_STS_PWR_MGMT;
    launch(&threads[0], 1);
    until(&sleeps[1], 1);
    launch(&threads[1], 3);
    until(&write_pending, 1);
    advance(1);
    finish(threads, 2);
    check(result[1] == BC_STS_CMD_CANCELLED && result[3] == BC_STS_PWR_MGMT &&
              binding.user == &owner && binding.tx_stopping == 0,
          "failed release retains handle without poisoning future input");
    puts("CONTROL PASS: deferred RELEASE clears its stop intent");
    reset();
    launch(&threads[0], 1);
    until(&sleeps[1], 1);
    launch(&threads[1], 3);
    until(&write_pending, 1);
    launch(&threads[2], 4);
    until(&release_requests, 2);
    advance(1);
    finish(threads, 3);
    check(result[1] == BC_STS_CMD_CANCELLED && result[3] == BC_STS_SUCCESS &&
              result[4] == -ENODATA && binding.tx_stopping == 0,
          "concurrent releases serialize and retire binding once");
    puts("CONTROL PASS: concurrent RELEASE intents do not overwrite each other");
    reset();
    actor = 3;
    check(chd_dec_ioctl_common(&fd, BCM_IOC_RELEASE, 0, false) == -EFAULT &&
              binding.tx_stopping == 0,
          "malformed RELEASE does not publish stopping or acquire writer");
    check(write_pending == 0, "bad envelope does not wait behind input");
    finish(threads, 0);
    puts("CONTROL PASS: malformed RELEASE leaves input unchanged");
    reset();
    launch(&threads[0], 1);
    until(&sleeps[1], 1);
    WRITE_ONCE(adp.cmds.cin_wait_exit, 1);
    advance(1);
    until(&done[1], 1);
    finish(threads, 1);
    check(result[1] == BC_STS_CMD_CANCELLED && binding.tx_stopping == 0,
          "ordinary flush keeps one-shot cancellation");
    puts("CONTROL PASS: flush remains independent of file stopping");
    reset();
    launch(&threads[0], 1);
    until(&sleeps[1], 1);
    WRITE_ONCE(signals[1], 1);
    advance(1);
    finish(threads, 1);
    check(result[1] == BC_STS_IO_USER_ABORT && binding.tx_stopping == 0,
          "signal abort still independent");
    puts("CONTROL PASS: interrupted input retains legacy result");
    reset();
    launch(&threads[0], 1);
    until(&sleeps[1], 1);
    WRITE_ONCE(adp.present, false);
    advance(1);
    finish(threads, 1);
    check(result[1] == BC_STS_CMD_CANCELLED, "removal cancels without relying on file token");
    puts("CONTROL PASS: removal remains independent");
    reset();
    check(binding.tx_stopping == 0, "new file starts unstopped");
    launch(&threads[0], 1);
    until(&sleeps[1], 1);
    WRITE_ONCE(signals[1], 1);
    advance(1);
    finish(threads, 1);
    check(result[1] == BC_STS_IO_USER_ABORT && posts[1] == 1, "subsequent session enters normally");
    puts("CONTROL PASS: later file/session has no stale stop intent");
    return 0;
}
