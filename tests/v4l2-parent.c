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
#include <unistd.h>

typedef uint64_t u64;
struct crystalhd_v4l2;
struct device { void *driver_data; unsigned refs; };
struct pci_dev { struct device dev; };
struct crystalhd_adp {
    struct pci_dev *pdev;
    struct crystalhd_v4l2 *v4l2;
    u64 generation;
    char name[32];
    bool hw_accessible;
};

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
struct v4l2_device {
    struct device *dev;
    char name[36];
    void (*release)(struct v4l2_device *device);
    unsigned refs;
    bool registered;
};
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

static void event(char value)
{
    assert(event_count + 1 < sizeof(events));
    events[event_count++] = value;
    events[event_count] = 0;
}

static void *kzalloc(size_t size, int flags)
{
    struct crystalhd_v4l2 *parent;
    (void)flags;
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
    assert(device->refs && !device->registered && !device->dev);
    parent_puts++;
    event('P');
    if (--device->refs)
        return 0;
    assert(device->release);
    releases++;
    event('L');
    device->release(device);
    return 1;
}

#include "parent-functions.h"

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
    for (unsigned i = 0; i < 4; i++)
        assert(!live[i]);
    assert(!munmap(adapter, page_size));
    assert(!munmap(pci, page_size));
    printf("V4L2 parent: %u scenarios, %u checks, 0 failures\n", scenarios, checks);
    return 0;
}
#endif
