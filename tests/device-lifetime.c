// SPDX-License-Identifier: GPL-2.0-or-later
/* Deterministic callback/resource model, not physical hot-unplug validation. */
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef uint64_t u64;
typedef int BC_STATUS;
#define BC_STS_SUCCESS 0
#define BC_STS_INV_ARG 1
#define BC_STS_ERR_USAGE 2
#define BC_LINK_INVALID 0
#define BC_HW_RUNNING 0
#define DTS_MODE_INV UINT32_MAX
#define DTS_DIAG_MODE 1
#define DTS_PLAYBACK_MODE 2
#define CRYSTALHD_API_NAME "crystalhd"
#define KERN_ERR ""
#define READ_ONCE(value) (value)
#define WRITE_ONCE(value, update) ((value) = (update))
#define spin_lock_irqsave(lock, flags) ((void)(lock), (flags) = 0)
#define spin_unlock_irqrestore(lock, flags) ((void)(lock), (void)(flags))
#define MKDEV(major, minor) (((major) << 8) | (minor))
#define printk(...) ((void)0)
#define dev_dbg(...) mock_log(__VA_ARGS__)
#define dev_info(...) mock_log(__VA_ARGS__)
#define dev_err(...) mock_log(__VA_ARGS__)
#define dev_warn(...) mock_log(__VA_ARGS__)

struct mock_lock { unsigned int readers, writers, entries; };
struct device { int unused; };
struct pci_dev { struct device dev; int irq; void *data; };
struct inode { int unused; };
struct file { void *private_data; };
struct crystalhd_user { uint32_t uid, in_use, mode; };
struct crystalhd_adp;
struct crystalhd_hw { void *rx_freeq; };
enum crystalhd_decoder_phase {
	CRYSTALHD_DECODER_COLD = 0,
	CRYSTALHD_DECODER_BOOTSTRAPPED,
	CRYSTALHD_DECODER_CHANNEL_CONFIGURED,
	CRYSTALHD_DECODER_RECOVERY_REQUIRED,
};
struct crystalhd_cmd {
	struct crystalhd_adp *adp;
	struct crystalhd_hw *hw_ctx;
	struct crystalhd_user user[2];
	const void *session_owner;
	enum crystalhd_decoder_phase decoder_phase;
	uint32_t fw_sequence, decoder_channel_id;
	uint32_t cin_wait_exit, pwr_state_change, state;
};
typedef struct crystalhd_ioctl_data {
	struct crystalhd_ioctl_data *next;
} crystalhd_ioctl_data;
struct crystalhd_l0s_state { bool owned; };
struct crystalhd_adp {
	struct pci_dev *pdev;
	struct crystalhd_cmd cmds;
	struct mock_lock user_lock;
	struct crystalhd_l0s_state l0s;
	crystalhd_ioctl_data *idata_free_head;
	void *mem_addr, *i2o_addr, *fill_byte_pool, *elem_pool_head;
	unsigned int cfg_users;
	int present, msi, chd_dec_major, lock;
	bool irq_registered;
};
#include "lifetime-binding.h"

enum allocation_kind { ADAPTER, HARDWARE, IODATA, BINDING, DIO_POOL, ELEM_POOL };
struct allocation { void *ptr; enum allocation_kind kind; };
static struct allocation allocations[16];
static unsigned int allocated[6], released[6], scenarios;
static struct pci_dev pci;
static struct crystalhd_adp *g_adp_info;
static struct mock_lock chd_device_lock;
static u64 chd_device_generation;
static int class_token, bar_tokens[2];
static void *crystalhd_class = &class_token;
static bool master, irq_live, msi_live, device_live, regions_live;
static bool chdev_live, class_live, bars_live[2];
static int pending_result, l0s_result;
static unsigned int master_clears, pending_waits, irq_frees, msi_disables;
static unsigned int dma_frees, l0s_releases, device_disables, region_releases;
static unsigned int bar_unmaps, binding_cancellations, warnings;

static void *allocate(size_t size, enum allocation_kind kind)
{
	unsigned int i;
	void *ptr = calloc(1, size);
	assert(ptr);
	for (i = 0; i < sizeof(allocations) / sizeof(allocations[0]); i++) {
		if (!allocations[i].ptr) {
			allocations[i] = (struct allocation){ptr, kind};
			allocated[kind]++;
			return ptr;
		}
	}
	abort();
}

static void assert_quiesced(void)
{
	assert(chd_device_lock.writers == 1);
	assert(!chd_device_lock.readers && !master && !irq_live && !msi_live);
	assert(g_adp_info && !g_adp_info->present && g_adp_info->cmds.cin_wait_exit);
}

static void kfree(void *ptr)
{
	unsigned int i;
	if (!ptr)
		return;
	for (i = 0; i < sizeof(allocations) / sizeof(allocations[0]); i++) {
		if (allocations[i].ptr == ptr) {
			enum allocation_kind kind = allocations[i].kind;
			if (kind == ADAPTER) {
				struct crystalhd_adp *adp = ptr;
				assert(chd_device_lock.writers == 1);
				assert(!g_adp_info && !pci.data && !device_live);
				assert(!regions_live && !bars_live[0] && !bars_live[1]);
				assert(!chdev_live && !class_live);
				assert(!adp->cmds.session_owner);
			} else if (kind != BINDING) {
				assert_quiesced();
			}
			released[kind]++;
			allocations[i].ptr = NULL;
			free(ptr);
			return;
		}
	}
	/* A double release or release of a foreign allocation must fail. */
	abort();
}

static struct crystalhd_adp *chd_get_adp(void) { return g_adp_info; }
static struct device *chddev(void)
{
	assert(g_adp_info && g_adp_info->pdev == &pci);
	return &pci.dev;
}
static void mock_log(const struct device *dev, const char *format, ...)
{
	assert(dev == &pci.dev);
	if (strstr(format, "pending") || strstr(format, "restoration failed"))
		warnings++;
}

static void down_read(struct mock_lock *lock)
{
	assert(lock == &chd_device_lock && !lock->writers);
	lock->readers++;
	lock->entries++;
}
static void up_read(struct mock_lock *lock)
{
	assert(lock == &chd_device_lock && lock->readers == 1 && !lock->writers);
	lock->readers--;
}
static void down_write(struct mock_lock *lock)
{
	assert(!lock->writers && !lock->readers);
	if (lock == &chd_device_lock) {
		/* Cancellation must already be visible to an in-flight FIFO waiter
		 * before the removal/fail-stop callback waits for the write lock.
		 */
		if (pci.data) {
			struct crystalhd_adp *adp = pci.data;
			assert(!adp->present && adp->cmds.cin_wait_exit);
			binding_cancellations++;
		}
	} else {
		assert(g_adp_info && lock == &g_adp_info->user_lock);
		assert(chd_device_lock.readers == 1);
	}
	lock->writers++;
	lock->entries++;
}
static void up_write(struct mock_lock *lock)
{
	assert(lock->writers == 1 && !lock->readers);
	lock->writers--;
}

static void *pci_get_drvdata(struct pci_dev *dev)
{ assert(dev == &pci); return dev->data; }
static void pci_set_drvdata(struct pci_dev *dev, void *data)
{
	assert(dev == &pci && !data && !device_live);
	assert(chd_device_lock.writers == 1);
	dev->data = data;
}
static void pci_clear_master(struct pci_dev *dev)
{
	assert(dev == &pci && chd_device_lock.writers == 1);
	assert(!g_adp_info->present && g_adp_info->cmds.cin_wait_exit);
	master = false;
	master_clears++;
}
static int pci_wait_for_pending_transaction(struct pci_dev *dev)
{
	assert(dev == &pci && !master && chd_device_lock.writers == 1);
	pending_waits++;
	return pending_result;
}
static void free_irq(unsigned int irq, void *arg)
{
	assert(irq == (unsigned int)pci.irq && arg == g_adp_info);
	assert(chd_device_lock.writers == 1 && !master && pending_waits);
	assert(irq_live);
	irq_live = false;
	irq_frees++;
}
static void pci_disable_msi(struct pci_dev *dev)
{
	assert(dev == &pci && !irq_live && msi_live);
	msi_live = false;
	msi_disables++;
}
static void crystalhd_hw_free_dma_rings(struct crystalhd_hw *hw)
{
	assert_quiesced();
	assert(hw == g_adp_info->cmds.hw_ctx && hw->rx_freeq);
	hw->rx_freeq = NULL;
	dma_frees++;
}
static void crystalhd_destroy_dio_pool(struct crystalhd_adp *adp)
{
	assert_quiesced();
	assert(adp == g_adp_info && !adp->cmds.hw_ctx && adp->fill_byte_pool);
	kfree(adp->fill_byte_pool);
	adp->fill_byte_pool = NULL;
}
static void crystalhd_delete_elem_pool(struct crystalhd_adp *adp)
{
	assert_quiesced();
	assert(adp == g_adp_info && !adp->cmds.hw_ctx && adp->elem_pool_head);
	kfree(adp->elem_pool_head);
	adp->elem_pool_head = NULL;
}
static void device_destroy(void *class_ptr, int device_number)
{
	assert_quiesced();
	assert(class_ptr == crystalhd_class && device_number == MKDEV(240, 0));
	assert(chdev_live && class_live && !g_adp_info->cmds.adp);
	assert(!g_adp_info->cmds.session_owner);
	chdev_live = false;
}
static void unregister_chrdev(int major, const char *name)
{
	assert_quiesced();
	assert(major == 240 && !strcmp(name, CRYSTALHD_API_NAME));
	assert(!chdev_live && class_live);
}
static void class_destroy(void *class_ptr)
{
	assert_quiesced();
	assert(class_ptr == crystalhd_class && !chdev_live && class_live);
	class_live = false;
}
static int crystalhd_l0s_release(struct pci_dev *dev,
		struct crystalhd_l0s_state *state)
{
	assert_quiesced();
	assert(dev == &pci && state == &g_adp_info->l0s && state->owned);
	assert(!chdev_live && !class_live && !g_adp_info->idata_free_head);
	assert(device_live && regions_live && bars_live[0] && bars_live[1]);
	state->owned = false;
	l0s_releases++;
	return l0s_result;
}
static void iounmap(void *address)
{
	unsigned int index = address == &bar_tokens[0] ? 0 : 1;
	assert_quiesced();
	assert(address == &bar_tokens[index] && bars_live[index]);
	assert(l0s_releases == 1 && device_live && regions_live);
	bars_live[index] = false;
	bar_unmaps++;
}
static void pci_release_regions(struct pci_dev *dev)
{
	assert_quiesced();
	assert(dev == &pci && regions_live && !bars_live[0] && !bars_live[1]);
	regions_live = false;
	region_releases++;
}
static void pci_disable_device(struct pci_dev *dev)
{
	assert_quiesced();
	assert(dev == &pci && device_live && !regions_live);
	device_live = false;
	device_disables++;
}

/* No close after fail-stop/removal may enter the normal hardware close path. */
static int bc_get_userhandle_count(struct crystalhd_cmd *cmd) { abort(); }
static void disable_irq(int irq) { abort(); }
static void enable_irq(int irq) { abort(); }
static void crystalhd_hw_stop_capture(struct crystalhd_hw *hw, bool discard)
{ abort(); }
static BC_STATUS crystalhd_hw_close(struct crystalhd_hw *hw)
{ abort(); }
#include "lifetime-command.h"
#include "lifetime-functions.h"

static struct crystalhd_adp *attach(bool playback, bool msi)
{
	unsigned int i;
	struct crystalhd_adp *adp = allocate(sizeof(*adp), ADAPTER);
	assert(!g_adp_info && !pci.data);
	g_adp_info = pci.data = adp;
	adp->pdev = &pci;
	adp->present = 1;
	adp->irq_registered = true;
	adp->msi = msi;
	adp->chd_dec_major = 240;
	adp->mem_addr = &bar_tokens[0];
	adp->i2o_addr = &bar_tokens[1];
	adp->l0s.owned = true;
	adp->cmds.adp = adp;
	adp->cmds.state = 1;
	for (i = 0; i < 3; i++) {
		crystalhd_ioctl_data *data = allocate(sizeof(*data), IODATA);
		data->next = adp->idata_free_head;
		adp->idata_free_head = data;
	}
	if (playback) {
		adp->cmds.session_owner = &adp->cmds.user[0];
		adp->cmds.hw_ctx = allocate(sizeof(*adp->cmds.hw_ctx), HARDWARE);
		adp->cmds.hw_ctx->rx_freeq = adp;
		adp->fill_byte_pool = allocate(1, DIO_POOL);
		adp->elem_pool_head = allocate(1, ELEM_POOL);
	}
	assert(!!adp->cmds.session_owner == playback);
	chd_device_generation++; /* Successful probe publication, modeled here. */
	master = irq_live = device_live = regions_live = true;
	chdev_live = class_live = bars_live[0] = bars_live[1] = true;
	msi_live = msi;
	return adp;
}

static void reset(void)
{
	unsigned int i;
	for (i = 0; i < sizeof(allocations) / sizeof(allocations[0]); i++)
		assert(!allocations[i].ptr);
	assert(!g_adp_info && !pci.data);
	memset(allocated, 0, sizeof(allocated));
	memset(released, 0, sizeof(released));
	chd_device_lock = (struct mock_lock){0};
	pci = (struct pci_dev){.irq = 19};
	pending_result = 1;
	l0s_result = 0;
	master_clears = pending_waits = irq_frees = msi_disables = 0;
	dma_frees = l0s_releases = device_disables = region_releases = 0;
	bar_unmaps = binding_cancellations = warnings = 0;
	scenarios++;
}

static struct file bind_user(struct crystalhd_adp *adp, unsigned int uid)
{
	struct crystalhd_file *binding = allocate(sizeof(*binding), BINDING);
	assert(uid < 2 && !adp->cmds.user[uid].in_use);
	adp->cmds.user[uid] = (struct crystalhd_user){uid, 1, DTS_PLAYBACK_MODE};
	adp->cfg_users++;
	binding->user = &adp->cmds.user[uid];
	binding->generation = chd_device_generation;
	return (struct file){.private_data = binding};
}

static void assert_released(void)
{
	unsigned int i;
	assert(!g_adp_info && !pci.data && !master && !irq_live && !msi_live);
	assert(!device_live && !regions_live && !chdev_live && !class_live);
	assert(!bars_live[0] && !bars_live[1]);
	assert(!chd_device_lock.readers && !chd_device_lock.writers);
	for (i = 0; i < 6; i++)
		assert(allocated[i] == released[i]);
}

static void test_remove(void)
{
	unsigned int which;
	reset();
	chd_dec_pci_remove(&pci);
	assert(chd_device_lock.entries == 1 && !master_clears && !pending_waits);
	assert_released();
	for (which = 0; which < 8; which++) {
		bool playback = which & 1, msi = which & 2;
		reset();
		attach(playback, msi);
		pending_result = !(which & 4);
		/* Restoration failure is diagnostic, not an excuse to leak resources. */
		l0s_result = which == 7 ? -EIO : 0;
		chd_dec_pci_remove(&pci);
		assert(binding_cancellations == 1 && master_clears == 1 && pending_waits == 1);
		assert(irq_frees == 1 && msi_disables == msi);
		assert(dma_frees == playback && l0s_releases == 1);
		assert(bar_unmaps == 2 && region_releases == 1 && device_disables == 1);
		assert(warnings == (unsigned int)!pending_result + (unsigned int)!!l0s_result);
		assert_released();
		/* drvdata is cleared; a second callback cannot release any resource twice. */
		chd_dec_pci_remove(&pci);
		assert(irq_frees == 1 && region_releases == 1 && device_disables == 1);
		assert_released();
	}
}

static void test_fail_stop_then_remove(void)
{
	unsigned int which;
	for (which = 0; which < 2; which++) {
		struct crystalhd_adp *adp;
		struct file first, second;
		reset();
		adp = attach(true, true);
		first = bind_user(adp, 0);
		second = bind_user(adp, 1);
		pending_result = which;
		chd_dec_fail_closed(adp, -EIO);
		chd_dec_fail_closed(adp, -EIO);
		assert(!adp->present && adp->cmds.cin_wait_exit);
		assert(!master && !irq_live && !msi_live && irq_frees == 1 && msi_disables == 1);
		assert(!dma_frees && !l0s_releases && !device_disables);
		assert(adp->cmds.hw_ctx && adp->fill_byte_pool && adp->elem_pool_head);
		assert(adp->cmds.session_owner == &adp->cmds.user[0]);
		assert(chd_dec_close(NULL, &first) == 0 && !first.private_data);
		assert(adp->cfg_users == 1 && !adp->cmds.user[0].in_use);
		assert(adp->cmds.user[0].mode == DTS_MODE_INV && adp->cmds.user[1].in_use);
		/* Accounting-only close leaves resource ownership for quiesced removal. */
		assert(adp->cmds.session_owner == &adp->cmds.user[0]);
		assert(chd_dec_close(NULL, &second) == 0 && !second.private_data);
		assert(!adp->cfg_users && !adp->cmds.user[1].in_use);
		assert(!dma_frees && !released[HARDWARE] && !released[DIO_POOL]);
		assert(adp->cmds.session_owner == &adp->cmds.user[0]);
		chd_dec_pci_remove(&pci);
		assert(binding_cancellations == 3 && master_clears == 3 && pending_waits == 3);
		assert(irq_frees == 1 && msi_disables == 1 && dma_frees == 1);
		assert_released();
	}
}

static void test_external_owner_teardown(void)
{
	unsigned int fail_first;

	for (fail_first = 0; fail_first < 2; fail_first++) {
		struct crystalhd_adp *adp;
		int external_owner;

		reset();
		adp = attach(true, true);
		adp->cmds.session_owner = &external_owner;
		assert(!adp->cfg_users);
		if (fail_first) {
			chd_dec_fail_closed(adp, -EIO);
			assert(!adp->present &&
			       adp->cmds.session_owner == &external_owner);
		}
		chd_dec_pci_remove(&pci);
		assert(dma_frees == 1 && released[HARDWARE] == 1 &&
		       released[DIO_POOL] == 1 && released[ELEM_POOL] == 1);
		assert_released();
	}
}

static void test_stale_close(void)
{
	unsigned int which;
	for (which = 0; which < 2; which++) {
		struct crystalhd_adp *adp;
		struct file stale;
		reset();
		adp = attach(true, true);
		stale = bind_user(adp, 0);
		chd_dec_pci_remove(&pci);
		assert(released[ADAPTER] == 1 && !released[BINDING]);
		if (which) {
			/* Its old user pointer refers to freed storage. A replacement
			 * generation must not make that binding valid again.
			 */
			adp = attach(false, false);
			adp->cfg_users = 1;
			adp->cmds.user[0] = (struct crystalhd_user){0, 1, DTS_PLAYBACK_MODE};
		}
		assert(chd_dec_close(NULL, &stale) == 0 && !stale.private_data);
		assert(released[BINDING] == 1);
		if (which) {
			assert(adp->cfg_users == 1 && adp->cmds.user[0].in_use);
			assert(!adp->cmds.session_owner);
			assert(!adp->user_lock.entries && master && irq_live);
			/* Per-attachment counters used by cleanup assertions. */
			l0s_releases = 0;
			chd_dec_pci_remove(&pci);
		}
		assert_released();
	}
}

int main(void)
{
	test_remove();
	test_fail_stop_then_remove();
	test_external_owner_teardown();
	test_stale_close();
	printf("Device lifetime: %u scenarios passed\n", scenarios);
	return 0;
}
