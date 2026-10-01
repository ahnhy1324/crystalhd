// SPDX-License-Identifier: GPL-2.0-or-later
/* Extracted parent lifetime paths; no video nodes or physical device required. */
#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

typedef uint64_t u64;
struct crystalhd_v4l2;
struct crystalhd_v4l2_ctx;
typedef enum { BC_STS_SUCCESS, BC_STS_BUSY, BC_STS_ERROR,
               BC_STS_IO_ERROR, BC_STS_ERR_USAGE } BC_STATUS;
struct crystalhd_session_owner_ops;
struct crystalhd_cmd {
    const void *session_owner, *session_lifetime_owner;
    const struct crystalhd_session_owner_ops *session_lifetime_ops;
};
struct device { void *driver_data; unsigned refs; };
struct pci_dev { struct device dev; };
struct crystalhd_adp {
    struct pci_dev *pdev;
    struct crystalhd_v4l2 *v4l2;
    u64 generation;
    char name[32];
    bool hw_accessible;
    bool present;
    struct crystalhd_cmd cmds;
};
struct crystalhd_device_access { struct crystalhd_adp *adp; bool exclusive; };

static unsigned scenarios, checks;
static void check(bool condition, const char *message)
{
    checks++;
    if (!condition) {
        fprintf(stderr, "V4L2 parent: %s\n", message);
        abort();
    }
}

#ifdef TEST_V4L2_DISABLED
#include "crystalhd_v4l2.h"

int main(void)
{
    struct crystalhd_adp adp, before;

    memset(&adp, 0xa5, sizeof(adp));
    memcpy(&before, &adp, sizeof(before));
    check(!crystalhd_v4l2_register(NULL), "disabled registration is a no-op");
    check(!crystalhd_v4l2_register(&adp), "disabled registration ignores adapter fields");
    crystalhd_v4l2_unregister(NULL);
    crystalhd_v4l2_unregister(&adp);
    crystalhd_v4l2_unregister(&adp);
    check(!memcmp(&adp, &before, sizeof(adp)), "disabled stubs leave adapter bytes unchanged");
    check(!crystalhd_v4l2_init(), "disabled frontend initialization is a no-op");
    crystalhd_v4l2_resume_ready(NULL);
    crystalhd_v4l2_resume_ready(&adp);
    crystalhd_v4l2_cleanup();
    crystalhd_v4l2_cleanup();
    check(!memcmp(&adp, &before, sizeof(adp)),
          "disabled readiness and workqueue lifecycle hooks leave adapter bytes unchanged");
    scenarios++;
    printf("V4L2 parent disabled: %u scenarios, %u checks, 0 failures\n",
           scenarios, checks);
    return 0;
}
#else
#define GFP_KERNEL 0
#define container_of(ptr, type, member) \
    ((type *)((char *)(ptr) - offsetof(type, member)))
#define READ_ONCE(value) (value)
#define WRITE_ONCE(value, update) ((value) = (update))
#define __init
#define __exit
#define printk(...) ((void)0)
#define WARN_ON_ONCE(condition) ((condition) ? (abort(), 1) : 0)
#define WQ_UNBOUND 1
#define WQ_MEM_RECLAIM 2
typedef unsigned spinlock_t;
struct mutex { unsigned held; };
struct kref { unsigned refs; };
struct list_head { struct list_head *next, *prev; };
struct work_struct { void (*function)(struct work_struct *); };
struct workqueue_struct { bool live; };
static unsigned spin_depth, mutex_depth;
static void (*after_spin_unlock)(void);
static void (*after_mutex_unlock)(void);
static void spin_lock_init(spinlock_t *lock) { *lock = 0; }
static void spin_lock_model(spinlock_t *lock)
{ assert(!*lock && !spin_depth); *lock = 1; spin_depth++; }
static void spin_unlock_model(spinlock_t *lock)
{
    void (*hook)(void) = after_spin_unlock;
    assert(*lock == 1 && spin_depth == 1);
    *lock = 0; spin_depth--;
    if (hook) { after_spin_unlock = NULL; hook(); }
}
#define spin_lock_irqsave(lock, flags) do { (flags) = 0; spin_lock_model(lock); } while (0)
#define spin_unlock_irqrestore(lock, flags) do { (void)(flags); spin_unlock_model(lock); } while (0)
static void mutex_init(struct mutex *lock) { lock->held = 0; }
static void mutex_lock(struct mutex *lock)
{ assert(!lock->held); lock->held = 1; mutex_depth++; }
static void mutex_unlock(struct mutex *lock)
{
    void (*hook)(void) = after_mutex_unlock;

    assert(lock->held && mutex_depth); lock->held = 0; mutex_depth--;
    if (hook) { after_mutex_unlock = NULL; hook(); }
}
static void kref_init(struct kref *ref) { ref->refs = 1; }
static void kref_get(struct kref *ref) { assert(ref->refs); ref->refs++; }
static int kref_put(struct kref *ref, void (*release)(struct kref *))
{
    assert(ref->refs);
    if (--ref->refs) return 0;
    release(ref);
    return 1;
}
static void INIT_LIST_HEAD(struct list_head *head) { head->next = head->prev = head; }
static bool list_empty(const struct list_head *head) { return head->next == head; }
static void list_add_tail(struct list_head *node, struct list_head *head)
{
    assert(node->next == node && node->prev == node);
    node->prev = head->prev; node->next = head;
    head->prev->next = node; head->prev = node;
}
static void list_del_init(struct list_head *node)
{
    assert(node->next != node && node->prev != node);
    node->prev->next = node->next; node->next->prev = node->prev;
    INIT_LIST_HEAD(node);
}
#define list_for_each_entry(pos, head, member) \
    for (pos = container_of((head)->next, __typeof__(*pos), member); \
         &(pos)->member != (head); \
         pos = container_of((pos)->member.next, __typeof__(*pos), member))
#define INIT_WORK(work, callback) ((work)->function = (callback))
struct v4l2_device {
    struct device *dev;
    char name[36];
    void (*release)(struct v4l2_device *device);
    unsigned refs;
    bool registered;
};
#include "parent-owner-type.h"
#include "parent-binding.h"

static struct crystalhd_adp *adapter;
static struct pci_dev *pci;
static size_t page_size;
static bool writer_held, fail_allocation;
static int registration_error;
static unsigned allocations, frees, registrations, unregistrations, parent_puts, releases;
static struct crystalhd_v4l2 *live[4];
static char events[128];
static unsigned event_count;
static struct crystalhd_adp *g_adp_info;
static int chd_device_lock;
static bool probe_tail_active, master_live, l0s_live, irq_live;
static bool chdev_live, bars_live, pci_live, adapter_retired;
static unsigned master_enables, master_clears, l0s_releases, irq_releases;
static unsigned chdev_releases, bar_releases, pci_disables, adapter_retires;
static unsigned writer_releases, drvdata_publishes, drvdata_clears;
static struct crystalhd_v4l2_ctx *native_live[8];
static unsigned native_allocations, native_frees, native_module_refs, native_module_gets;
static unsigned native_module_puts, native_module_attempts, module_token;
static bool native_module_allowed = true, fail_native_allocation;
static struct workqueue_struct workqueue;
static struct work_struct *queued_work[16], *running_work;
static unsigned queued_count, work_runs, workqueue_allocations, workqueue_destroys;
static bool fail_workqueue;
static bool access_active;
static unsigned access_enters, access_exits, core_acquires, core_releases;
static BC_STATUS acquire_status, release_status;
static bool acquire_retains_error, run_between_retired_put;
static void (*enter_hook)(void), (*exit_hook)(void);
static int pci_registration_result;
static unsigned pci_registrations, pci_unregistrations;
static int bc_chd_driver;
static struct crystalhd_v4l2_ctx *idle_tail_context;
#define THIS_MODULE (&module_token)

static void event(char value)
{
    assert(event_count + 1 < sizeof(events));
    events[event_count++] = value;
    events[event_count] = 0;
}

static bool try_module_get(const void *module)
{
    assert(module == THIS_MODULE && !spin_depth);
    native_module_attempts++;
    if (!native_module_allowed) return false;
    native_module_refs++; native_module_gets++;
    return true;
}

static void module_put(const void *module)
{
    assert(module == THIS_MODULE && native_module_refs && !spin_depth &&
           !mutex_depth && !writer_held && !access_active);
    native_module_refs--; native_module_puts++;
}

static struct workqueue_struct *alloc_workqueue(const char *name, unsigned flags, int max_active)
{
    assert(name && !writer_held && !access_active && !spin_depth && !mutex_depth);
    assert(flags == (WQ_UNBOUND | WQ_MEM_RECLAIM) && !max_active && !workqueue.live);
    workqueue_allocations++;
    if (fail_workqueue) return NULL;
    workqueue.live = true;
    return &workqueue;
}

static bool queue_work(struct workqueue_struct *queue, struct work_struct *work)
{
    assert(queue == &workqueue && queue->live && work->function);
    for (unsigned i = 0; i < queued_count; i++)
        if (queued_work[i] == work) return false;
    assert(queued_count < sizeof(queued_work) / sizeof(queued_work[0]));
    queued_work[queued_count++] = work;
    return true;
}

static void run_one_work(void)
{
    struct work_struct *work;

    assert(queued_count && !running_work && !spin_depth);
    work = queued_work[0];
    memmove(queued_work, queued_work + 1, --queued_count * sizeof(*queued_work));
    running_work = work;
    work_runs++;
    work->function(work);
    running_work = NULL; /* The callback may already have freed its work storage. */
}

static void flush_workqueue(struct workqueue_struct *queue)
{
    unsigned limit = 32;

    assert(queue == &workqueue && queue->live && !writer_held && !access_active &&
           !spin_depth && !mutex_depth);
    while (queued_count) { assert(limit--); run_one_work(); }
}

static void destroy_workqueue(struct workqueue_struct *queue)
{
    flush_workqueue(queue); /* The kernel destroy primitive drains queued work. */
    assert(queue == &workqueue && queue->live && !writer_held && !access_active &&
           !spin_depth && !mutex_depth && !queued_count && !native_module_refs);
    workqueue.live = false;
    workqueue_destroys++;
}

static void *kzalloc(size_t size, int flags)
{
    struct crystalhd_v4l2 *parent;
    (void)flags;
    if (size == sizeof(struct crystalhd_v4l2_ctx)) {
        struct crystalhd_v4l2_ctx *ctx;

        assert(!spin_depth && native_module_refs);
        if (fail_native_allocation) return NULL;
        ctx = calloc(1, size);
        assert(ctx);
        for (unsigned i = 0; i < 8; i++) {
            if (!native_live[i]) {
                native_live[i] = ctx;
                native_allocations++;
                return ctx;
            }
        }
        abort();
    }
    assert(writer_held && size == sizeof(*parent));
    if (probe_tail_active)
        assert(!adapter->hw_accessible);
    event('A');
    if (fail_allocation)
        return NULL;
    parent = calloc(1, size);
    assert(parent);
    for (unsigned i = 0; i < 4; i++) {
        if (!live[i]) {
            live[i] = parent;
            allocations++;
            return parent;
        }
    }
    abort();
}

static void kfree(void *ptr)
{
    struct crystalhd_v4l2 *parent = ptr;
    for (unsigned i = 0; i < 8; i++) {
        if (native_live[i] == ptr) {
            struct crystalhd_v4l2_ctx *ctx = ptr;

            assert(!writer_held && !access_active && !spin_depth && !mutex_depth);
            assert(!ctx->ref.refs && !ctx->core_reference_held);
            if (ctx->parent)
                assert(ctx->cleanup_committed && list_empty(&ctx->pending));
            else
                assert(!ctx->pending.next && !ctx->pending.prev);
            assert(native_module_refs);
            native_live[i] = NULL;
            native_frees++;
            free(ptr);
            return;
        }
    }
    if (ptr == adapter) {
        assert(probe_tail_active && writer_held && !adapter_retired);
        assert(!master_live && !l0s_live && !irq_live && !chdev_live &&
               !bars_live && !pci_live);
        assert(!adapter->v4l2 && !adapter->hw_accessible && !g_adp_info &&
               !pci->dev.driver_data && pci->dev.refs == 1);
        adapter_retired = true;
        adapter_retires++;
        event('a');
        /* Keep the mapped fixture for postconditions and the delayed-put test. */
        return;
    }
    for (unsigned i = 0; i < 4; i++) {
        if (live[i] == ptr) {
            assert(!parent->device.dev && !parent->device.registered &&
                   !parent->device.refs);
            live[i] = NULL;
            frees++;
            event('F');
            free(ptr);
            return;
        }
    }
    /* Includes foreign frees and double releases. */
    abort();
}

static void *pci_get_drvdata(struct pci_dev *pdev)
{
    assert(writer_held && pdev == pci);
    return pdev->dev.driver_data;
}

static ssize_t strscpy(char *dst, const char *src, size_t size)
{
    size_t length = strlen(src);
    assert(size);
    if (length >= size) {
        memcpy(dst, src, size - 1);
        dst[size - 1] = 0;
        return -E2BIG;
    }
    memcpy(dst, src, length + 1);
    return (ssize_t)length;
}

/* Model the media core contract: preserve nonnull drvdata, take a PCI device
 * reference on registration, drop it on unregister, and keep the V4L2 kref
 * separate. Injected register errors occur before any successful registration.
 */
static int v4l2_device_register(struct device *dev, struct v4l2_device *device)
{
    struct crystalhd_v4l2 *parent =
        container_of(device, struct crystalhd_v4l2, device);
    assert(writer_held && dev == &pci->dev && !adapter->v4l2);
    if (probe_tail_active)
        assert(!adapter->hw_accessible && master_live && l0s_live &&
               irq_live && chdev_live && bars_live && pci_live);
    assert(parent->generation == adapter->generation && !parent->disconnected);
    assert(device->release && !strcmp(device->name, adapter->name));
    assert(dev->driver_data == adapter && !device->refs && !device->registered);
    registrations++;
    event('R');
    if (registration_error)
        return registration_error;
    dev->refs++;
    device->dev = dev;
    device->refs = 1;
    device->registered = true;
    if (!dev->driver_data)
        dev->driver_data = device;
    return 0;
}

static void v4l2_device_unregister(struct v4l2_device *device)
{
    struct crystalhd_v4l2 *parent =
        container_of(device, struct crystalhd_v4l2, device);
    assert(writer_held && !adapter->v4l2 && parent->disconnected);
    assert(device->registered && device->dev == &pci->dev && device->refs);
    assert(pci->dev.refs == 2);
    unregistrations++;
    event('U');
    if (device->dev->driver_data == device)
        device->dev->driver_data = NULL;
    device->dev->refs--;
    device->dev = NULL;
    device->registered = false;
    device->name[0] = 0;
}

static void v4l2_device_get(struct v4l2_device *device)
{
    assert(device->refs);
    device->refs++;
}

static int v4l2_device_put(struct v4l2_device *device)
{
    assert(device->refs);
    parent_puts++;
    event('P');
    if (--device->refs)
        return 0;
    assert(!device->registered && !device->dev);
    assert(device->release);
    releases++;
    event('L');
    device->release(device);
    return 1;
}

static int crystalhd_device_enter(u64 generation, bool exclusive,
                                  struct crystalhd_device_access *access)
{
    void (*hook)(void) = enter_hook;
    int rc = 0;

    assert(access && !access_active && !writer_held && !spin_depth && exclusive);
    memset(access, 0, sizeof(*access));
    access_enters++;
    if (!g_adp_info || !generation || generation != g_adp_info->generation ||
        !g_adp_info->present)
        rc = -ENODEV;
    else if (!g_adp_info->hw_accessible)
        rc = -EAGAIN;
    else {
        access_active = true;
        access->adp = g_adp_info;
        access->exclusive = true;
    }
    if (hook) { enter_hook = NULL; hook(); }
    return rc;
}

static void crystalhd_device_exit(struct crystalhd_device_access *access)
{
    void (*hook)(void) = exit_hook;

    assert(access && access->adp == adapter && access->exclusive && access_active && !spin_depth);
    memset(access, 0, sizeof(*access));
    access_active = false;
    access_exits++;
    if (hook) { exit_hook = NULL; hook(); }
}

static BC_STATUS crystalhd_session_acquire_ref_locked(struct crystalhd_cmd *cmd,
        const void *owner, const struct crystalhd_session_owner_ops *ops)
{
    assert(access_active && cmd == &adapter->cmds && owner && ops && ops->get &&
           ops->retired && ops->put && mutex_depth && !spin_depth);
    core_acquires++;
    if (cmd->session_lifetime_owner) return BC_STS_BUSY;
    if (acquire_status == BC_STS_SUCCESS || acquire_retains_error) {
        if (acquire_status == BC_STS_SUCCESS) cmd->session_owner = owner;
        ops->get(owner);
        cmd->session_lifetime_owner = owner;
        cmd->session_lifetime_ops = ops;
    }
    return acquire_status;
}

static void model_core_retire(void)
{
    const struct crystalhd_session_owner_ops *ops = adapter->cmds.session_lifetime_ops;
    const void *owner = adapter->cmds.session_lifetime_owner;
    unsigned freed = native_frees, puts = native_module_puts;

    assert(ops && owner && (access_active || writer_held));
    memset(&adapter->cmds, 0, sizeof(adapter->cmds));
    ops->retired(owner);
    if (run_between_retired_put && queued_count) {
        assert(!running_work);
        run_one_work();
        check(native_frees == freed && native_module_puts == puts,
              "worker between retired and put cannot finalize the core-held context");
    }
    ops->put(owner);
    check(native_frees == freed && native_module_puts == puts,
          "core put cannot free context or module code under the device/session barrier");
}

static BC_STATUS crystalhd_session_release_locked(struct crystalhd_cmd *cmd, const void *owner)
{
    assert(access_active && cmd == &adapter->cmds && owner && mutex_depth && !spin_depth);
    core_releases++;
    if (cmd->session_owner != owner) return BC_STS_ERR_USAGE;
    if (release_status != BC_STS_SUCCESS) return release_status;
    model_core_retire();
    return BC_STS_SUCCESS;
}

static int crystalhd_status_to_errno(BC_STATUS status)
{
    switch (status) {
    case BC_STS_SUCCESS: return 0;
    case BC_STS_BUSY: return -EBUSY;
    case BC_STS_ERR_USAGE: return -EINVAL;
    default: return -EIO;
    }
}

#include "parent-functions.h"

static int pci_register_driver(const void *driver)
{
    assert(driver == &bc_chd_driver && workqueue.live && !writer_held &&
           !spin_depth && !mutex_depth && !access_active);
    pci_registrations++;
    return pci_registration_result;
}

static void pci_unregister_driver(const void *driver)
{
    assert(driver == &bc_chd_driver && workqueue.live && !writer_held &&
           !spin_depth && !mutex_depth && !access_active);
    pci_unregistrations++;
}

#include "parent-module-functions.h"

static void assert_probe_cleanup(void)
{
    assert(probe_tail_active && writer_held && !adapter_retired);
    assert(!adapter->hw_accessible && !adapter->v4l2 && g_adp_info == adapter);
    assert(!pci->dev.driver_data && pci->dev.refs == 1);
}

static void pci_set_master(struct pci_dev *pdev)
{
    assert(pdev == pci && probe_tail_active && writer_held && !adapter_retired);
    assert(g_adp_info == adapter && !pci->dev.driver_data && !adapter->hw_accessible);
    assert(l0s_live && irq_live && chdev_live && bars_live && pci_live);
    master_live = true;
    master_enables++;
    event('M');
}

static void pci_set_drvdata(struct pci_dev *pdev, void *data)
{
    assert(pdev == pci && probe_tail_active && writer_held && !adapter_retired);
    assert(g_adp_info == adapter && !adapter->hw_accessible && !adapter->v4l2);
    assert(master_live && l0s_live && irq_live && chdev_live && bars_live && pci_live);
    assert(pci->dev.refs == 1);
    if (data) {
        assert(data == adapter && !pci->dev.driver_data);
        drvdata_publishes++;
        event('D');
    } else {
        assert(pci->dev.driver_data == adapter);
        drvdata_clears++;
        event('d');
    }
    pci->dev.driver_data = data;
}

static void pci_clear_master(struct pci_dev *pdev)
{
    assert_probe_cleanup();
    assert(pdev == pci && master_live && l0s_live && irq_live &&
           chdev_live && bars_live && pci_live);
    master_live = false;
    master_clears++;
    event('m');
}

static void chd_release_l0s(struct crystalhd_adp *adp)
{
    assert_probe_cleanup();
    assert(adp == adapter && !master_live && l0s_live && irq_live &&
           chdev_live && bars_live && pci_live);
    l0s_live = false;
    l0s_releases++;
    event('l');
}

static int chd_dec_disable_int(struct crystalhd_adp *adp)
{
    assert_probe_cleanup();
    assert(adp == adapter && !master_live && !l0s_live && irq_live &&
           chdev_live && bars_live && pci_live);
    irq_live = false;
    irq_releases++;
    event('i');
    return 0;
}

static void chd_dec_release_chdev(struct crystalhd_adp *adp)
{
    assert_probe_cleanup();
    assert(adp == adapter && !master_live && !l0s_live && !irq_live &&
           chdev_live && bars_live && pci_live);
    chdev_live = false;
    chdev_releases++;
    event('c');
}

static void chd_pci_release_mem(struct crystalhd_adp *adp)
{
    assert_probe_cleanup();
    assert(adp == adapter && !master_live && !l0s_live && !irq_live &&
           !chdev_live && bars_live && pci_live);
    bars_live = false;
    bar_releases++;
    event('b');
}

static void pci_disable_device(struct pci_dev *pdev)
{
    assert_probe_cleanup();
    assert(pdev == pci && !master_live && !l0s_live && !irq_live &&
           !chdev_live && !bars_live && pci_live);
    pci_live = false;
    pci_disables++;
    event('p');
}

static void up_write(int *lock)
{
    assert(lock == &chd_device_lock && writer_held && probe_tail_active);
    if (g_adp_info) {
        assert(g_adp_info == adapter && !adapter_retired && adapter->hw_accessible &&
               adapter->v4l2 && pci->dev.driver_data == adapter && pci->dev.refs == 2);
        assert(master_live && l0s_live && irq_live && chdev_live && bars_live && pci_live);
    } else {
        assert(adapter_retired && !adapter->hw_accessible && !adapter->v4l2 &&
               !pci->dev.driver_data && pci->dev.refs == 1);
        assert(!master_live && !l0s_live && !irq_live && !chdev_live &&
               !bars_live && !pci_live);
    }
    writer_held = false;
    writer_releases++;
    event('w');
}

#include "parent-probe-tail.h"

static void reset(void)
{
    for (unsigned i = 0; i < 4; i++)
        assert(!live[i]);
    for (unsigned i = 0; i < 8; i++)
        assert(!native_live[i]);
    assert(!workqueue.live && !queued_count && !running_work && !native_module_refs);
    scenarios++;
    memset(adapter, 0, sizeof(*adapter));
    memset(pci, 0, sizeof(*pci));
    adapter->pdev = pci;
    adapter->generation = UINT64_C(0x100000029);
    memcpy(adapter->name, "crystalhd_pci_e:7:2:1", sizeof("crystalhd_pci_e:7:2:1"));
    pci->dev.driver_data = adapter;
    pci->dev.refs = 1;
    writer_held = true;
    fail_allocation = false;
    registration_error = 0;
    allocations = frees = registrations = unregistrations = parent_puts = releases = 0;
    event_count = 0;
    events[0] = 0;
    g_adp_info = adapter;
    probe_tail_active = master_live = l0s_live = irq_live = false;
    chdev_live = bars_live = pci_live = adapter_retired = false;
    master_enables = master_clears = l0s_releases = irq_releases = 0;
    chdev_releases = bar_releases = pci_disables = adapter_retires = 0;
    writer_releases = drvdata_publishes = drvdata_clears = 0;
    native_allocations = native_frees = native_module_gets = native_module_puts = 0;
    native_module_attempts = 0;
    native_module_allowed = true;
    fail_native_allocation = fail_workqueue = false;
    spin_depth = mutex_depth = 0;
    after_spin_unlock = NULL;
    after_mutex_unlock = NULL;
    work_runs = workqueue_allocations = workqueue_destroys = 0;
    access_active = false;
    access_enters = access_exits = core_acquires = core_releases = 0;
    acquire_status = release_status = BC_STS_SUCCESS;
    acquire_retains_error = run_between_retired_put = false;
    enter_hook = exit_hook = NULL;
    pci_registration_result = 0;
    pci_registrations = pci_unregistrations = 0;
    idle_tail_context = NULL;
}

static void invalid_cases(void)
{
    for (unsigned invalid = 0; invalid < 5; invalid++) {
        struct crystalhd_adp *argument;
        reset();
        argument = adapter;
        if (invalid == 0)
            argument = NULL;
        else if (invalid == 1)
            adapter->generation = 0;
        else if (invalid == 2)
            adapter->pdev = NULL;
        else if (invalid == 3)
            pci->dev.driver_data = NULL;
        else
            pci->dev.driver_data = (void *)(uintptr_t)1;
        check(crystalhd_v4l2_register(argument) == -EINVAL,
              "invalid adapter, generation, PCI device or drvdata rejected");
        check(!adapter->v4l2 && !allocations && !registrations &&
              !unregistrations && !parent_puts && !frees && pci->dev.refs == 1,
              "invalid registration has no lifetime side effects");
        crystalhd_v4l2_unregister(NULL);
        crystalhd_v4l2_unregister(adapter);
        check(!event_count, "unregister absent parent is harmless");
    }
}

static void failure_cases(void)
{
    reset();
    fail_allocation = true;
    check(crystalhd_v4l2_register(adapter) == -ENOMEM, "parent allocation failure propagated");
    check(!adapter->v4l2 && !allocations && !registrations && !frees &&
          pci->dev.refs == 1 && pci->dev.driver_data == adapter && !strcmp(events, "A"),
          "allocation failure leaves PCI ownership and output unchanged");

    for (unsigned failure = 0; failure < 2; failure++) {
        reset();
        registration_error = failure ? -EIO : -ENOMEM;
        check(crystalhd_v4l2_register(adapter) == registration_error,
              "media registration error propagated unchanged");
        crystalhd_v4l2_unregister(adapter);
        check(!adapter->v4l2 && allocations == 1 && frees == 1 && registrations == 1 &&
              !unregistrations && !parent_puts && !releases && pci->dev.refs == 1 &&
              pci->dev.driver_data == adapter && !strcmp(events, "ARF"),
              "failed registration frees only unpublished parent without a phantom put");
    }
}

static void ordinary_case(void)
{
    struct crystalhd_v4l2 *parent;
    reset();
    check(!crystalhd_v4l2_register(adapter), "parent registration succeeds");
    parent = adapter->v4l2;
    check(parent && parent->generation == UINT64_C(0x100000029) && !parent->disconnected &&
          parent->device.refs == 1 && parent->device.dev == &pci->dev &&
          pci->dev.refs == 2 && pci->dev.driver_data == adapter,
          "published parent copies full generation and preserves PCI drvdata");
    memset(adapter->name, 'x', sizeof(adapter->name));
    check(!strcmp(parent->device.name, "crystalhd_pci_e:7:2:1"),
          "parent name is copied, not borrowed from adapter");
    check(crystalhd_v4l2_register(adapter) == -EBUSY && adapter->v4l2 == parent &&
          allocations == 1 && registrations == 1 && pci->dev.refs == 2,
          "duplicate registration preserves the original parent and references");
    crystalhd_v4l2_unregister(adapter);
    crystalhd_v4l2_unregister(adapter);
    check(!adapter->v4l2 && allocations == 1 && frees == 1 && unregistrations == 1 &&
          parent_puts == 1 && releases == 1 && pci->dev.refs == 1 &&
          pci->dev.driver_data == adapter && !strcmp(events, "ARUPLF"),
          "unregister detaches before media teardown and drops the initial ref exactly once");
}

static void retained_case(void)
{
    struct crystalhd_v4l2 *parent;
    reset();
    check(!crystalhd_v4l2_register(adapter), "register parent for delayed release");
    parent = adapter->v4l2;
    v4l2_device_get(&parent->device);
    v4l2_device_get(&parent->device);
    crystalhd_v4l2_unregister(adapter); /* PM fail-close */
    crystalhd_v4l2_unregister(adapter); /* subsequent PCI remove */
    check(!adapter->v4l2 && parent->disconnected && !parent->device.dev &&
          parent->generation == UINT64_C(0x100000029) && parent->device.refs == 2 &&
          pci->dev.refs == 1 && pci->dev.driver_data == adapter && !frees &&
          unregistrations == 1 && parent_puts == 1,
          "synthetic frontend refs survive fail-close and repeated removal");
    writer_held = false;
    assert(!mprotect(adapter, page_size, PROT_NONE));
    assert(!mprotect(pci, page_size, PROT_NONE));
    check(!v4l2_device_put(&parent->device) && !frees && !releases,
          "nonfinal put needs neither adapter nor PCI storage");
    check(v4l2_device_put(&parent->device) == 1 && frees == 1 && releases == 1 &&
          !strcmp(events, "ARUPPPLF"),
          "final release is parent-memory-only without device writer lock");
    assert(!mprotect(adapter, page_size, PROT_READ | PROT_WRITE));
    assert(!mprotect(pci, page_size, PROT_READ | PROT_WRITE));
}

static void reprobe_case(void)
{
    struct crystalhd_v4l2 *old_parent, *new_parent;
    struct crystalhd_adp *same_address = adapter;
    reset();
    check(!crystalhd_v4l2_register(adapter), "register old parent before reprobe");
    old_parent = adapter->v4l2;
    v4l2_device_get(&old_parent->device);
    crystalhd_v4l2_unregister(adapter);
    adapter->generation = UINT64_C(0x200000029);
    check(!crystalhd_v4l2_register(adapter), "register new generation at same adapter address");
    new_parent = adapter->v4l2;
    check(adapter == same_address && new_parent != old_parent &&
          old_parent->generation == UINT64_C(0x100000029) && old_parent->disconnected &&
          new_parent->generation == UINT64_C(0x200000029) && !new_parent->disconnected &&
          !old_parent->device.dev && new_parent->device.dev == &pci->dev,
          "same-address reprobe keeps generations and connectivity independent");
    writer_held = false;
    check(v4l2_device_put(&old_parent->device) == 1 && frees == 1 && releases == 1 &&
          adapter->v4l2 == new_parent && new_parent->device.refs == 1 &&
          pci->dev.refs == 2 && pci->dev.driver_data == adapter,
          "old parent final put cannot disturb new adapter generation");
    writer_held = true;
    crystalhd_v4l2_unregister(adapter);
    check(allocations == 2 && frees == 2 && registrations == 2 &&
          unregistrations == 2 && parent_puts == 3 && releases == 2 &&
          !adapter->v4l2 && pci->dev.refs == 1,
          "both probe generations release their own initial and surviving references");
}

static void probe_tail_cases(void)
{
    for (unsigned failure = 0; failure < 3; failure++) {
        int expected = failure == 1 ? -ENOMEM : failure == 2 ? -EIO : 0;
        reset();
        probe_tail_active = true;
        l0s_live = irq_live = chdev_live = bars_live = pci_live = true;
        pci->dev.driver_data = NULL;
        fail_allocation = failure == 1;
        registration_error = failure == 2 ? -EIO : 0;
        check(probe_publication_tail(pci, adapter) == expected,
              "actual probe publication tail propagates parent setup result");
        check(!writer_held && writer_releases == 1 && master_enables == 1 &&
              drvdata_publishes == 1,
              "probe publishes PCI ownership under writer and releases writer exactly once");
        if (!failure) {
            check(adapter->hw_accessible && adapter->v4l2 && g_adp_info == adapter &&
                  pci->dev.driver_data == adapter && pci->dev.refs == 2 &&
                  master_live && l0s_live && irq_live && chdev_live && bars_live && pci_live &&
                  !adapter_retired && !drvdata_clears && !master_clears &&
                  !l0s_releases && !irq_releases && !chdev_releases && !bar_releases &&
                  !pci_disables && !adapter_retires && !frees && !unregistrations &&
                  !parent_puts && !strcmp(events, "MDARw"),
                  "successful probe exposes readiness only after parent publication without cleanup");
            /* The success fixture still owns its parent; retire it separately. */
            writer_held = true;
            crystalhd_v4l2_unregister(adapter);
            check(frees == 1 && pci->dev.refs == 1,
                  "successful probe parent can be unregistered normally");
        } else {
            check(!adapter->hw_accessible && !adapter->v4l2 && !g_adp_info &&
                  !pci->dev.driver_data && pci->dev.refs == 1 &&
                  !master_live && !l0s_live && !irq_live && !chdev_live &&
                  !bars_live && !pci_live && adapter_retired,
                  "failed probe never exposes readiness and retires all device ownership");
            check(drvdata_clears == 1 && master_clears == 1 && l0s_releases == 1 &&
                  irq_releases == 1 && chdev_releases == 1 && bar_releases == 1 &&
                  pci_disables == 1 && adapter_retires == 1 && !unregistrations &&
                  !parent_puts && !releases && allocations == (failure == 2) &&
                  frees == (failure == 2) && registrations == (failure == 2),
                  "failed probe releases each acquired resource exactly once");
            check(!strcmp(events, failure == 1 ? "MDAdmlicbpaw" : "MDARFdmlicbpaw"),
                  "literal probe failure unwind preserves detach, cleanup and unlock order");
        }
    }
}

static void native_start(void)
{
    reset();
    writer_held = false;
    check(!crystalhd_v4l2_init(), "native close workqueue initializes outside device barriers");
    writer_held = true;
    check(!crystalhd_v4l2_register(adapter), "native fixture registers an independent parent");
    adapter->present = adapter->hw_accessible = true;
    writer_held = false;
}

static struct crystalhd_v4l2_ctx *native_create(struct crystalhd_v4l2 *parent)
{
    struct crystalhd_v4l2_ctx *ctx = NULL;

    v4l2_device_get(&parent->device); /* Explicit caller reference during create. */
    check(!crystalhd_v4l2_ctx_create(parent, &ctx) && ctx && ctx->parent == parent &&
          ctx->generation == parent->generation && ctx->ref.refs == 1 &&
          ctx->owner_state == CRYSTALHD_V4L2_OWNER_NEVER && !ctx->core_reference_held,
          "created context owns immutable generation, one base reference and no adapter pointer");
    v4l2_device_put(&parent->device);
    return ctx;
}

static void native_finish(void)
{
    assert(!writer_held && !access_active);
    writer_held = true;
    crystalhd_v4l2_unregister(adapter);
    writer_held = false;
    flush_workqueue(&workqueue);
    check(native_allocations == native_frees && native_module_gets == native_module_puts &&
          !native_module_refs && !queued_count && !adapter->cmds.session_lifetime_owner,
          "native close retires every context, parent and independent module reference");
    crystalhd_v4l2_cleanup();
    check(!workqueue.live && workqueue_destroys == 1 && allocations == frees,
          "native workqueue drains only outside barriers after parent disconnection");
}

static void module_lifecycle_cases(void)
{
    for (unsigned failure = 0; failure < 3; failure++) {
        reset();
        writer_held = false;
        fail_workqueue = failure == 1;
        pci_registration_result = failure == 2 ? -EIO : 0;
        check(chd_dec_module_init() == (failure == 1 ? -ENOMEM : failure == 2 ? -EIO : 0),
              "actual module initialization propagates workqueue and PCI registration failures");
        check(workqueue_allocations == 1 && pci_registrations == (failure != 1) &&
              !pci_unregistrations && workqueue_destroys == (failure == 2),
              "failed workqueue creation never registers PCI; failed PCI registration drains its queue");
        if (!failure) {
            chd_dec_module_cleanup();
            check(pci_unregistrations == 1 && workqueue_destroys == 1,
                  "actual module exit unregisters PCI before destroying the close queue");
        }
        check(!workqueue.live && !crystalhd_v4l2_close_wq,
              "failed initialization and successful unload leave no workqueue pointer");
        crystalhd_v4l2_cleanup();
    }
}

static void native_constructor_cases(void)
{
    for (unsigned failure = 0; failure < 5; failure++) {
        struct crystalhd_v4l2_ctx *ctx = (void *)(uintptr_t)1;
        struct crystalhd_v4l2 *parent;
        int expected = failure < 2 ? -EINVAL : failure == 3 ? -ENOMEM : -ENODEV;

        native_start();
        parent = adapter->v4l2;
        v4l2_device_get(&parent->device);
        if (failure == 2) native_module_allowed = false;
        if (failure == 3) fail_native_allocation = true;
        if (failure == 4) {
            writer_held = true;
            crystalhd_v4l2_unregister(adapter);
            writer_held = false;
        }
        check(crystalhd_v4l2_ctx_create(failure ? parent : NULL,
                  failure == 1 ? NULL : &ctx) == expected &&
              (failure == 1 || !ctx) && !native_module_refs &&
              native_allocations == native_frees &&
              native_module_gets == native_module_puts,
              "constructor failures clear output and balance allocation/module/parent ownership");
        check(parent->device.refs == (failure == 4 ? 1U : 2U) &&
              !access_enters && !core_acquires && !queued_count,
              "constructor failures never borrow an adapter or publish deferred work");
        v4l2_device_put(&parent->device);
        native_finish();
    }
}

static void native_never_adopted_cases(void)
{
    for (unsigned failure = 0; failure < 4; failure++) {
        struct crystalhd_v4l2_ctx *ctx;

        native_start();
        ctx = native_create(adapter->v4l2);
        if (failure == 1) acquire_status = BC_STS_BUSY;
        if (failure == 2) acquire_status = BC_STS_ERROR;
        if (failure == 3) adapter->hw_accessible = false;
        if (failure)
            check(crystalhd_v4l2_ctx_acquire(ctx) ==
                      (failure == 1 ? -EBUSY : failure == 2 ? -EIO : -EAGAIN) &&
                  ctx->owner_state == CRYSTALHD_V4L2_OWNER_NEVER && !ctx->core_reference_held,
                  "nonadopting acquire errors preserve safe never-owned close state");
        kref_get(&ctx->ref); /* Test-only reference makes repeat calls valid memory accesses. */
        crystalhd_v4l2_ctx_close(ctx);
        check(ctx->closing && !list_empty(&ctx->pending) && queued_count == 1,
              "close publishes a pending registry reference before dropping the base reference");
        crystalhd_v4l2_ctx_close(ctx);
        check(crystalhd_v4l2_ctx_acquire(ctx) == -ENODEV && queued_count == 1,
              "repeated close is coalesced and a closing context cannot acquire ownership");
        flush_workqueue(&workqueue);
        check(ctx->cleanup_committed && list_empty(&ctx->pending) && !core_releases &&
              ctx->ref.refs == 1 && !native_frees,
              "never-adopted close needs no hardware access and commits cleanup exactly once");
        kref_put(&ctx->ref, crystalhd_v4l2_ctx_release);
        native_finish();
    }
    crystalhd_v4l2_ctx_close(NULL);
    check(crystalhd_v4l2_ctx_acquire(NULL) == -EINVAL, "NULL native context admission is rejected");
}

static void native_ordinary_case(void)
{
    struct crystalhd_v4l2_ctx *ctx;

    native_start();
    ctx = native_create(adapter->v4l2);
    check(!crystalhd_v4l2_ctx_acquire(ctx) && ctx->core_reference_held &&
          ctx->owner_state == CRYSTALHD_V4L2_OWNER_HELD && ctx->ref.refs == 2,
          "successful acquire holds one actual callback-owned context reference");
    check(crystalhd_v4l2_ctx_acquire(ctx) == -EBUSY && ctx->ref.refs == 2,
          "repeated acquisition does not adopt a second core reference");
    crystalhd_v4l2_ctx_close(ctx);
    flush_workqueue(&workqueue);
    check(native_frees == 1 && core_releases == 1 && access_enters == access_exits &&
          !adapter->cmds.session_owner && !adapter->cmds.session_lifetime_owner,
          "ordinary deferred close releases core ownership and destroys only after device exit");
    native_finish();
}

static void work_before_base_put(void)
{
    assert(!mutex_depth && queued_count == 1);
    run_one_work();
    check(!native_frees && !native_module_puts,
          "worker finishing before close returns leaves the caller base reference intact");
}

static void native_close_worker_order_cases(void)
{
    for (unsigned acquired = 0; acquired < 2; acquired++) {
        struct crystalhd_v4l2_ctx *ctx;

        native_start();
        ctx = native_create(adapter->v4l2);
        if (acquired)
            check(!crystalhd_v4l2_ctx_acquire(ctx), "prepare an acquired worker-before-close-return case");
        after_mutex_unlock = work_before_base_put;
        crystalhd_v4l2_ctx_close(ctx);
        check(native_frees == 1 && native_module_puts == 1 && !queued_count &&
              work_runs == 1 && core_releases == acquired,
              "close may perform the final put after an earlier worker, only after its mutex is released");
        native_finish();
    }
}

static void resume_hook(void)
{
    assert(!access_active);
    adapter->hw_accessible = true;
    writer_held = true;
    crystalhd_v4l2_resume_ready(adapter);
    writer_held = false;
}

static void resume_at_idle_worker_tail(void)
{
    struct crystalhd_v4l2_ctx *ctx = idle_tail_context;

    assert(ctx && running_work == &ctx->close_work && !ctx->work_active &&
           !queued_count && !spin_depth && !mutex_depth && ctx->ref.refs == 3);
    resume_hook();
    check(running_work == &ctx->close_work && ctx->work_active && queued_count == 1 &&
          queued_work[0] == &ctx->close_work && ctx->ref.refs == 4,
          "resume requeues the still-running idle work item with distinct old and new worker references");
}

static void arm_idle_worker_tail_resume(void)
{
    assert(!access_active && !adapter->hw_accessible && !after_spin_unlock);
    /* EAGAIN has been computed. The next parent unlock is the worker's
     * work_active=false publication, before its old reference is dropped.
     */
    after_spin_unlock = resume_at_idle_worker_tail;
}

static void native_resume_cases(void)
{
    for (unsigned ordering = 0; ordering < 4; ordering++) {
        struct crystalhd_v4l2_ctx *ctx;

        native_start();
        ctx = native_create(adapter->v4l2);
        check(!crystalhd_v4l2_ctx_acquire(ctx), "prepare held native ownership before suspend-close");
        adapter->hw_accessible = false;
        if (ordering == 0) resume_hook(); /* Resume before pending registration. */
        crystalhd_v4l2_ctx_close(ctx);
        if (ordering == 1) enter_hook = resume_hook; /* EAGAIN result, worker still active. */
        if (ordering == 3) {
            idle_tail_context = ctx;
            enter_hook = arm_idle_worker_tail_resume;
        }
        run_one_work();
        if (ordering == 2) {
            check(!native_frees && !queued_count && !ctx->work_active &&
                  !list_empty(&ctx->pending) && ctx->core_reference_held,
                  "EAGAIN parks reachable pending ownership without polling or discarding the lease");
            resume_hook(); /* Resume after worker becomes idle. */
            flush_workqueue(&workqueue);
        }
        if (ordering == 3) {
            check(!running_work && !native_frees && ctx->work_active && queued_count == 1 &&
                  ctx->ref.refs == 3 && ctx->core_reference_held && !list_empty(&ctx->pending),
                  "old worker return drops only its own reference while the requeued worker remains owned");
            flush_workqueue(&workqueue);
            idle_tail_context = NULL;
        }
        check(native_frees == 1 && core_releases == 1 && !queued_count &&
              access_enters == (ordering ? 3U : 2U),
              "all resume/registration/worker-idle orderings eventually release exactly once");
        native_finish();
    }
}

static void retire_at_worker_exit(void)
{
    writer_held = true;
    model_core_retire();
    writer_held = false;
}

static void native_retirement_races(void)
{
    for (unsigned unpublished = 0; unpublished < 2; unpublished++) {
        for (unsigned before_close = 0; before_close < 2; before_close++) {
            struct crystalhd_v4l2_ctx *ctx;

            native_start();
            ctx = native_create(adapter->v4l2);
            acquire_retains_error = unpublished;
            acquire_status = unpublished ? BC_STS_ERROR : BC_STS_SUCCESS;
            check(crystalhd_v4l2_ctx_acquire(ctx) == (unpublished ? -EIO : 0) &&
                  ctx->core_reference_held && ctx->owner_state == CRYSTALHD_V4L2_OWNER_HELD &&
                  (adapter->cmds.session_owner == NULL) == (bool)unpublished,
                  "published and failed-unpublished acquisition retain the same stable core lifetime");
            /* A previously disconnected/quarantined parent can receive safe
             * retirement later. Its worker cannot enter the writer-held device.
             */
            writer_held = true;
            crystalhd_v4l2_unregister(adapter);
            writer_held = false;
            if (!before_close) crystalhd_v4l2_ctx_close(ctx);
            writer_held = true;
            run_between_retired_put = true;
            model_core_retire();
            writer_held = false;
            check(!native_frees && !native_module_puts,
                  "retired notification and core put never perform final destruction under terminal writer");
            if (before_close) crystalhd_v4l2_ctx_close(ctx);
            flush_workqueue(&workqueue);
            check(native_frees == 1 && native_module_puts == 1 && !core_releases &&
                  !queued_count && allocations == frees,
                  "core put re-kicks an earlier retirement worker and finalizes after all barriers");
            native_finish();
        }
    }

    native_start();
    {
        struct crystalhd_v4l2_ctx *ctx = native_create(adapter->v4l2);

        check(!crystalhd_v4l2_ctx_acquire(ctx), "prepare retirement racing the active worker's admission exit");
        release_status = BC_STS_IO_ERROR;
        exit_hook = retire_at_worker_exit;
        crystalhd_v4l2_ctx_close(ctx);
        run_one_work();
        check(native_frees == 1 && work_runs == 1 && core_releases == 1 && !queued_count,
              "retirement at device exit is observed before the worker can lose its final kick");
    }
    native_finish();
}

static void native_reprobe_case(void)
{
    struct crystalhd_v4l2_ctx *ctx;
    struct crystalhd_v4l2 *new_parent;

    native_start();
    ctx = native_create(adapter->v4l2);
    writer_held = true;
    crystalhd_v4l2_unregister(adapter);
    adapter->generation++;
    check(!crystalhd_v4l2_register(adapter), "reprobe publishes a new parent at the same adapter address");
    new_parent = adapter->v4l2;
    writer_held = false;
    check(crystalhd_v4l2_ctx_acquire(ctx) == -ENODEV && !access_enters && !core_acquires,
          "disconnected old context rejects admission before dereferencing a reprobed adapter");
    crystalhd_v4l2_ctx_close(ctx);
    flush_workqueue(&workqueue);
    check(adapter->v4l2 == new_parent && new_parent->device.refs == 1 && native_frees == 1,
          "old context destruction cannot put or acquire the new generation's parent");
    native_finish();
}

static void native_admission_identity_cases(void)
{
    for (unsigned mismatch = 0; mismatch < 3; mismatch++) {
        struct crystalhd_v4l2_ctx *ctx;
        struct crystalhd_v4l2 *parent;

        native_start();
        parent = adapter->v4l2;
        ctx = native_create(parent);
        if (mismatch == 0) adapter->generation++;
        if (mismatch == 1) adapter->v4l2 = NULL;
        if (mismatch == 2) g_adp_info = NULL;
        check(crystalhd_v4l2_ctx_acquire(ctx) == -ENODEV && !core_acquires &&
              access_enters == 1 && access_exits == (mismatch == 1) &&
              !access_active && ctx->generation == parent->generation &&
              ctx->owner_state == CRYSTALHD_V4L2_OWNER_NEVER && ctx->ref.refs == 1,
              "generation, admitted parent identity and absent adapter gates reject without a core lease");
        /* Restore fixture binding visibility only; no core/DMA owner was
         * acquired or released by any denied admission in these cases.
         */
        adapter->v4l2 = parent;
        g_adp_info = adapter;
        crystalhd_v4l2_ctx_close(ctx);
        flush_workqueue(&workqueue);
        native_finish();
    }

    native_start();
    {
        struct crystalhd_v4l2_ctx *ctx = native_create(adapter->v4l2);

        writer_held = true;
        crystalhd_v4l2_unregister(adapter);
        writer_held = false;
        assert(!mprotect(adapter, page_size, PROT_NONE));
        assert(!mprotect(pci, page_size, PROT_NONE));
        crystalhd_v4l2_ctx_close(ctx);
        flush_workqueue(&workqueue);
        check(native_frees == 1 && native_module_puts == 1 && frees == 1 && !access_enters,
              "disconnected native final destruction needs neither adapter nor PCI storage");
        assert(!mprotect(adapter, page_size, PROT_READ | PROT_WRITE));
        assert(!mprotect(pci, page_size, PROT_READ | PROT_WRITE));
    }
    native_finish();
}

static void native_quarantine_child(void)
{
    struct crystalhd_v4l2_ctx *ctx;
    struct crystalhd_v4l2 *parent;
    pid_t child = fork();
    int status;

    assert(child >= 0);
    if (!child) {
        native_start();
        parent = adapter->v4l2;
        ctx = native_create(parent);
        check(!crystalhd_v4l2_ctx_acquire(ctx), "prepare permanent native DMA quarantine");
        adapter->present = false;
        crystalhd_v4l2_ctx_close(ctx);
        flush_workqueue(&workqueue);
        check(ctx->core_reference_held && !ctx->work_active && !list_empty(&ctx->pending) &&
              !native_frees && native_module_refs == 1 && !core_releases && !queued_count,
              "ENODEV cannot convert held ownership into destruction permission");
        writer_held = true;
        crystalhd_v4l2_unregister(adapter);
        writer_held = false;
        flush_workqueue(&workqueue);
        check(parent->disconnected && !parent->device.dev && parent->device.refs == 1 &&
              !list_empty(&parent->pending) && ctx->core_reference_held && ctx->ref.refs == 2 &&
              !native_frees && !frees && native_module_refs == 1 && !queued_count,
              "permanent quarantine stays reachable through core and parent registry references");
        /* No cleanup: retained storage and callback code remain owned. */
        exit(0);
    }
    assert(waitpid(child, &status, 0) == child);
    scenarios++;
    check(WIFEXITED(status) && !WEXITSTATUS(status),
          "isolated permanent quarantine exits without fabricated release or fault clearing");
}

int main(void)
{
    long size = sysconf(_SC_PAGESIZE);
    assert(size > 0);
    page_size = (size_t)size;
    adapter = mmap(NULL, page_size, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    pci = mmap(NULL, page_size, PROT_READ | PROT_WRITE,
               MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(adapter != MAP_FAILED && pci != MAP_FAILED);
    invalid_cases();
    failure_cases();
    ordinary_case();
    retained_case();
    reprobe_case();
    probe_tail_cases();
    module_lifecycle_cases();
    native_constructor_cases();
    native_never_adopted_cases();
    native_ordinary_case();
    native_close_worker_order_cases();
    native_resume_cases();
    native_retirement_races();
    native_reprobe_case();
    native_admission_identity_cases();
    native_quarantine_child();
    for (unsigned i = 0; i < 4; i++)
        assert(!live[i]);
    assert(!munmap(adapter, page_size));
    assert(!munmap(pci, page_size));
    printf("V4L2 parent: %u scenarios, %u checks, 0 failures\n", scenarios, checks);
    return 0;
}
#endif
