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
typedef uint16_t u16;
typedef int BC_STATUS;
#define PCI_COMMAND 0x04
#define PCI_COMMAND_MASTER 0x04
#define PCI_EXP_DEVSTA 0x0a
#define PCI_EXP_DEVSTA_TRPND 0x20
#define PCIBIOS_SUCCESSFUL 0
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
#define WARN_ON_ONCE(value) ((value) ? (++warnings, true) : false)
#define lockdep_assert_held_write(lock) assert((lock)->writers == 1)
#define ARRAY_SIZE(array) (sizeof(array) / sizeof((array)[0]))

struct mock_lock { unsigned int readers, writers, entries; };
struct device { int unused; };
struct pci_dev {
	struct device dev;
	int irq;
	void *data;
	unsigned int refs;
	bool state_saved, saved_master;
};
struct inode { int unused; };
struct file { void *private_data; };
struct crystalhd_user { uint32_t uid, in_use, mode; };
struct crystalhd_adp;
struct crystalhd_hw {
	void *rx_freeq, *rx_actq, *rx_rdyq, *tx_actq, *tx_freeq;
	void *rx_pkt_pool_head, *rx_fallback_head;
	struct {
		struct { void *pdma_desc_start; } desc_mem;
		void *buffer, *call_back, *cb_context;
	} tx_pkt_pool[2];
};
struct crystalhd_stream;
enum crystalhd_decoder_phase {
	CRYSTALHD_DECODER_COLD = 0,
	CRYSTALHD_DECODER_BOOTSTRAPPED,
	CRYSTALHD_DECODER_CHANNEL_CONFIGURED,
	CRYSTALHD_DECODER_CHANNEL_STARTED,
	CRYSTALHD_DECODER_RECOVERY_REQUIRED,
};
enum crystalhd_decoder_codec {
	CRYSTALHD_DECODER_CODEC_INVALID = -1,
	CRYSTALHD_DECODER_CODEC_H264 = 0,
};
struct crystalhd_cmd {
	struct crystalhd_adp *adp;
	struct crystalhd_hw *hw_ctx;
	struct crystalhd_user user[2];
	const void *session_owner;
	enum crystalhd_decoder_phase decoder_phase;
	enum crystalhd_decoder_codec decoder_codec;
	uint32_t fw_sequence, decoder_channel_id;
	struct crystalhd_stream *stream;
	uint32_t cin_wait_exit, pwr_state_change, state;
	bool retain_rx_on_suspend;
	bool session_module_pinned;
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
	void *ua_map_free_head;
	unsigned int cfg_users;
	int present, msi, chd_dec_major, lock;
	bool irq_registered;
	bool dma_terminal_quiesced;
};
#include "lifetime-binding.h"

enum allocation_kind { ADAPTER, HARDWARE, IODATA, BINDING, DIO_POOL, ELEM_POOL };
struct allocation { void *ptr; enum allocation_kind kind; };
static struct allocation allocations[16];
static unsigned int allocated[6], released[6], scenarios;
static struct pci_dev pci;
static struct crystalhd_adp *g_adp_info;
static struct crystalhd_adp *chd_dma_quarantine;
static struct mock_lock chd_device_lock;
static u64 chd_device_generation;
static int class_token, bar_tokens[2];
static int stream_token;
static void *crystalhd_class = &class_token;
static bool master, irq_live, msi_live, device_live, regions_live;
static bool chdev_live, class_live, bars_live[2];
static int pending_result, l0s_result;
static unsigned int master_clears, pending_waits, irq_frees, msi_disables;
static unsigned int dma_frees, l0s_releases, device_disables, region_releases;
static unsigned int bar_unmaps, binding_cancellations, warnings;
static unsigned int stream_releases;
static bool stream_live;
static bool frontend_live;
static unsigned int frontend_releases;
static int module_token;
#define THIS_MODULE (&module_token)
static unsigned int session_admissions, module_refs, module_puts;
static unsigned int pci_gets, pci_puts;
static unsigned int saved_invalidations, core_restores;
static bool dma_drained;
static bool ignore_master_clear, express;
static int command_error, status_error;
static u16 command_output, status_output;
static unsigned int command_reads, status_reads;
static bool chd_dec_session_dma_absent(struct crystalhd_adp *adp);

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

static void assert_excluded(void)
{
	assert(chd_device_lock.writers == 1);
	assert(!chd_device_lock.readers && !irq_live && !msi_live);
	assert(!pci.state_saved);
	assert(g_adp_info && !g_adp_info->present && g_adp_info->cmds.cin_wait_exit);
}

static void assert_quiesced(void)
{
	assert_excluded();
	if (g_adp_info->dma_terminal_quiesced)
		assert(!master && dma_drained);
	else
		assert(chd_dec_session_dma_absent(g_adp_info));
}

/* Admission itself is exercised by command-pm. This fixture starts with an
 * already-admitted legacy or generic session and tests its actual teardown.
 */
static void model_session_admission(struct crystalhd_cmd *cmd)
{
	assert(cmd == &g_adp_info->cmds && !cmd->session_module_pinned);
	assert(!module_refs);
	cmd->session_module_pinned = true;
	module_refs++;
	session_admissions++;
}

static void module_put(void *module)
{
	struct crystalhd_cmd *cmd = &g_adp_info->cmds;

	assert_quiesced();
	assert(module == THIS_MODULE && module_refs == 1);
	/* The extracted unpin helper clears the token before the final put. */
	assert(!cmd->session_module_pinned && !cmd->session_owner);
	assert(!cmd->retain_rx_on_suspend && !cmd->adp && !cmd->hw_ctx);
	assert(!g_adp_info->fill_byte_pool && !g_adp_info->elem_pool_head);
	assert(!cmd->stream && !stream_live);
	assert(cmd->state == BC_LINK_INVALID);
	assert(cmd->decoder_phase == CRYSTALHD_DECODER_COLD);
	assert(cmd->decoder_codec == CRYSTALHD_DECODER_CODEC_INVALID);
	assert(!cmd->fw_sequence && !cmd->decoder_channel_id);
	assert(allocated[HARDWARE] == released[HARDWARE]);
	assert(allocated[DIO_POOL] == released[DIO_POOL]);
	assert(allocated[ELEM_POOL] == released[ELEM_POOL]);
	module_refs--;
	module_puts++;
}

static void crystalhd_v4l2_unregister(struct crystalhd_adp *adp)
{
	assert_excluded();
	assert(adp == g_adp_info);
	if (adp->cmds.session_module_pinned) {
		assert(!adp->dma_terminal_quiesced && !dma_drained);
		assert(chd_dma_quarantine == adp && pci.refs == 1);
		assert(adp->cmds.adp == adp && module_refs == 1);
	} else {
		/* Actual command deletion, not just RX retirement, precedes detach. */
		assert(!adp->cmds.adp && !adp->cmds.hw_ctx && !adp->cmds.session_owner);
		assert(!adp->fill_byte_pool && !adp->elem_pool_head && !stream_live);
	}
	if (frontend_live) {
		frontend_live = false;
		frontend_releases++;
	}
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
				assert(!frontend_live);
				assert(!adp->cmds.session_owner);
				assert(!adp->cmds.retain_rx_on_suspend);
				assert(!adp->cmds.session_module_pinned && !module_refs);
				assert(adp->cmds.decoder_phase ==
				       CRYSTALHD_DECODER_COLD);
				assert(adp->cmds.decoder_codec ==
				       CRYSTALHD_DECODER_CODEC_INVALID);
				assert(!adp->cmds.fw_sequence &&
				       !adp->cmds.decoder_channel_id);
			} else if (kind != BINDING) {
				if (kind == IODATA)
					assert_excluded();
				else
					assert_quiesced();
				if (kind == DIO_POOL || kind == ELEM_POOL ||
				    (kind == HARDWARE && g_adp_info->cmds.session_module_pinned))
					assert(g_adp_info->cmds.session_module_pinned && module_refs == 1);
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
	assert(dev == &pci && !data);
	assert(chd_device_lock.writers == 1);
	assert(!device_live || (chd_dma_quarantine == g_adp_info && module_refs == 1));
	dev->data = data;
}
static struct pci_dev *pci_dev_get(struct pci_dev *dev)
{
	assert_excluded();
	assert(dev == &pci && !pci.refs && !dma_drained);
	assert(g_adp_info->cmds.session_module_pinned && module_refs == 1);
	pci.refs++;
	pci_gets++;
	return dev;
}
static void pci_dev_put(struct pci_dev *dev)
{
	assert(dev == &pci && pci.refs == 1);
	assert(!g_adp_info && !pci.data && !chd_dma_quarantine && !module_refs);
	assert(!chd_device_lock.writers && !device_live && released[ADAPTER]);
	pci.refs--;
	pci_puts++;
}
static void pci_clear_master(struct pci_dev *dev)
{
	assert(dev == &pci && chd_device_lock.writers == 1);
	assert(!g_adp_info->present && g_adp_info->cmds.cin_wait_exit);
	if (!ignore_master_clear)
		master = false;
	dma_drained = false;
	master_clears++;
}
static int pci_wait_for_pending_transaction(struct pci_dev *dev)
{
	assert(dev == &pci && !master && chd_device_lock.writers == 1);
	pending_waits++;
	return pending_result;
}
static int pci_read_config_word(struct pci_dev *dev, int reg, u16 *value)
{
	assert(dev == &pci && reg == PCI_COMMAND && chd_device_lock.writers == 1);
	command_reads++;
	*value = command_error ? command_output :
		command_output | (master ? PCI_COMMAND_MASTER : 0);
	return command_error;
}
static bool pci_is_pcie(struct pci_dev *dev)
{
	assert(dev == &pci);
	return express;
}
static int pcie_capability_read_word(struct pci_dev *dev, int reg, u16 *value)
{
	assert(dev == &pci && reg == PCI_EXP_DEVSTA && !master);
	assert(chd_device_lock.writers == 1 && pending_waits && pending_result);
	status_reads++;
	*value = status_output;
	dma_drained = !status_error && !(status_output & PCI_EXP_DEVSTA_TRPND);
	return status_error;
}
static int pci_load_saved_state(struct pci_dev *dev, void *state)
{
	assert(dev == &pci && !state && chd_device_lock.writers == 1);
	assert(g_adp_info && !g_adp_info->present && !irq_live && !msi_live);
	dev->state_saved = false;
	saved_invalidations++;
	return 0;
}
static void simulate_pci_core_restore(void)
{
	bool old_master = master;

	/* Robustness schedule, not a claim that DPM resumes a failed-suspend
	 * device: PCI core may restore a valid image before a driver callback.
	 */
	if (pci.state_saved) {
		master = pci.saved_master;
		pci.state_saved = false;
		core_restores++;
	}
	assert(master == old_master && !pci.state_saved && !core_restores);
}
static void free_irq(unsigned int irq, void *arg)
{
	assert(irq == (unsigned int)pci.irq && arg == g_adp_info);
	assert(chd_device_lock.writers == 1 && master_clears);
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
	assert(hw == g_adp_info->cmds.hw_ctx);
	if (hw->rx_freeq) {
		assert(g_adp_info->cmds.session_module_pinned && module_refs == 1);
		hw->rx_freeq = NULL;
		dma_frees++;
	} else {
		assert(chd_dec_session_dma_absent(g_adp_info));
		assert(!module_refs);
	}
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
	assert_excluded();
	assert(class_ptr == crystalhd_class && device_number == MKDEV(240, 0));
	assert(chdev_live && class_live && !frontend_live);
	assert(!g_adp_info->cmds.adp ||
	       (chd_dma_quarantine == g_adp_info && module_refs == 1));
	chdev_live = false;
}
static void unregister_chrdev(int major, const char *name)
{
	assert_excluded();
	assert(major == 240 && !strcmp(name, CRYSTALHD_API_NAME));
	assert(!chdev_live && class_live);
}
static void class_destroy(void *class_ptr)
{
	assert_excluded();
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
static void crystalhd_stream_release(struct crystalhd_cmd *cmd)
{
	if (!cmd || !cmd->stream)
		return;
	assert(cmd == &g_adp_info->cmds &&
	       cmd->stream == (struct crystalhd_stream *)&stream_token &&
	       stream_live);
	assert(cmd->session_module_pinned && module_refs == 1);
	cmd->stream = NULL;
	stream_live = false;
	stream_releases++;
}
#include "lifetime-command.h"
#include "lifetime-functions.h"

static struct crystalhd_adp *attach(bool playback, bool msi)
{
	unsigned int i;
	struct crystalhd_adp *adp = allocate(sizeof(*adp), ADAPTER);
	assert(!g_adp_info && !pci.data);
	assert(!chd_dma_quarantine && !pci.refs);
	g_adp_info = pci.data = adp;
	adp->pdev = &pci;
	pci.state_saved = pci.saved_master = true;
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
		model_session_admission(&adp->cmds);
		adp->cmds.session_owner = &adp->cmds.user[0];
		adp->cmds.retain_rx_on_suspend = true;
		adp->cmds.hw_ctx = allocate(sizeof(*adp->cmds.hw_ctx), HARDWARE);
		adp->cmds.hw_ctx->rx_freeq = adp;
		adp->fill_byte_pool = allocate(1, DIO_POOL);
		adp->elem_pool_head = allocate(1, ELEM_POOL);
		adp->cmds.stream = (struct crystalhd_stream *)&stream_token;
		stream_live = true;
	}
	assert(!!adp->cmds.session_owner == playback);
	chd_device_generation++; /* Successful probe publication, modeled here. */
	master = irq_live = device_live = regions_live = true;
	chdev_live = class_live = bars_live[0] = bars_live[1] = true;
	frontend_live = true;
	msi_live = msi;
	return adp;
}

static void reset(void)
{
	unsigned int i;
	for (i = 0; i < sizeof(allocations) / sizeof(allocations[0]); i++)
		assert(!allocations[i].ptr);
	assert(!g_adp_info && !pci.data);
	assert(!module_refs && session_admissions == module_puts);
	session_admissions = module_refs = module_puts = 0;
	memset(allocated, 0, sizeof(allocated));
	memset(released, 0, sizeof(released));
	chd_device_lock = (struct mock_lock){0};
	pci = (struct pci_dev){.irq = 19};
	pending_result = 1;
	ignore_master_clear = false;
	express = true;
	command_error = status_error = 0;
	command_output = status_output = 0;
	command_reads = status_reads = 0;
	l0s_result = 0;
	master_clears = pending_waits = irq_frees = msi_disables = 0;
	dma_frees = l0s_releases = device_disables = region_releases = 0;
	bar_unmaps = binding_cancellations = warnings = 0;
	stream_releases = 0;
	stream_live = false;
	frontend_live = false;
	frontend_releases = 0;
	pci_gets = pci_puts = 0;
	saved_invalidations = core_restores = 0;
	dma_drained = false;
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
	assert(!g_adp_info && !pci.data && !irq_live && !msi_live);
	assert(!session_admissions || !master);
	assert(!device_live && !regions_live && !chdev_live && !class_live);
	assert(!bars_live[0] && !bars_live[1]);
	assert(!stream_live);
	assert(!module_refs && module_puts == session_admissions);
	assert(!chd_dma_quarantine && !pci.refs && pci_gets == pci_puts);
	assert(!frontend_live && frontend_releases == released[ADAPTER]);
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
		/* No-session failures own no DMA backing; pinned failure retention
		 * is exercised separately instead of pretending release is safe.
		 */
		pending_result = playback || !(which & 4);
		/* Restoration failure is diagnostic, not an excuse to leak resources. */
		l0s_result = which == 7 ? -EIO : 0;
		chd_dec_pci_remove(&pci);
		assert(binding_cancellations == 1 && master_clears == 1 && pending_waits == 1);
		assert(irq_frees == 1 && msi_disables == msi);
		assert(dma_frees == playback && l0s_releases == 1);
		assert(stream_releases == playback);
		assert(module_puts == playback && !module_refs);
		assert(bar_unmaps == 2 && region_releases == 1 && device_disables == 1);
		assert(warnings == (unsigned int)!!l0s_result);
		assert_released();
		/* drvdata is cleared; a second callback cannot release any resource twice. */
		chd_dec_pci_remove(&pci);
		assert(irq_frees == 1 && region_releases == 1 && device_disables == 1);
		assert(module_puts == playback && !module_refs);
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
		assert(chd_dec_fail_closed(adp, -EIO) == (bool)which);
		assert(chd_dec_fail_closed(adp, -EIO) == (bool)which);
		assert(adp->dma_terminal_quiesced == (bool)which);
		assert(!pci.state_saved && pci.saved_master && saved_invalidations == 2);
		simulate_pci_core_restore();
		assert(adp->cmds.session_module_pinned == !which);
		assert(module_refs == !which && module_puts == which);
		assert(!adp->present && adp->cmds.cin_wait_exit);
		assert(!master && !irq_live && !msi_live && irq_frees == 1 && msi_disables == 1);
		assert(dma_frees == which && !l0s_releases && !device_disables);
		assert(!!adp->cmds.hw_ctx == !which && !!adp->fill_byte_pool == !which &&
		       !!adp->elem_pool_head == !which && !!adp->cmds.adp == !which);
		assert(adp->cmds.session_owner == (which ? NULL : &adp->cmds.user[0]));
		assert(chd_dec_close(NULL, &first) == 0 && !first.private_data);
		assert(adp->cfg_users == 1 && !adp->cmds.user[0].in_use);
		assert(adp->cmds.user[0].mode == DTS_MODE_INV && adp->cmds.user[1].in_use);
		assert(adp->cmds.session_module_pinned == !which && module_refs == !which);
		/* Accounting-only close neither revives a tombstone nor unpins an
		 * unsafe session retained for a later removal drain attempt.
		 */
		assert(adp->cmds.session_owner == (which ? NULL : &adp->cmds.user[0]));
		assert(chd_dec_close(NULL, &second) == 0 && !second.private_data);
		assert(!adp->cfg_users && !adp->cmds.user[1].in_use);
		assert(dma_frees == which && released[HARDWARE] == which && released[DIO_POOL] == which);
		assert(stream_live == !which && stream_releases == which);
		assert(adp->cmds.session_owner == (which ? NULL : &adp->cmds.user[0]));
		assert(adp->cmds.session_module_pinned == !which && module_refs == !which);
		assert(module_puts == which && pci_gets == !which && !pci_puts);
		/* A successful proof is latched; a prior failure must retry and can
		 * now reclaim quarantine. Never recheck config after a safe tombstone.
		 */
		pending_result = !which;
		chd_dec_pci_remove(&pci);
		assert(binding_cancellations == 3);
		assert(master_clears == (which ? 1U : 3U) && pending_waits == master_clears);
		assert(irq_frees == 1 && msi_disables == 1 && dma_frees == 1);
		assert(stream_releases == 1 && !stream_live);
		assert(module_puts == 1 && !module_refs);
		assert(pci_gets == !which && pci_puts == !which);
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
		adp->cmds.retain_rx_on_suspend = false;
		adp->cmds.decoder_phase = CRYSTALHD_DECODER_CHANNEL_STARTED;
		adp->cmds.decoder_codec = CRYSTALHD_DECODER_CODEC_H264;
		adp->cmds.fw_sequence = 5;
		assert(!adp->cfg_users);
		assert(adp->cmds.session_module_pinned && module_refs == 1 && !module_puts);
		if (fail_first) {
			assert(chd_dec_fail_closed(adp, -EIO));
			assert(!adp->present && !adp->cmds.session_owner && !adp->cmds.adp);
			assert(!adp->cmds.session_module_pinned && !module_refs && module_puts == 1);
		}
		chd_dec_pci_remove(&pci);
		assert(dma_frees == 1 && released[HARDWARE] == 1 &&
		       released[DIO_POOL] == 1 && released[ELEM_POOL] == 1);
		assert(stream_releases == 1 && !stream_live);
		assert(module_puts == 1 && !module_refs);
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
		assert(module_puts == 1 && !module_refs);
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
		assert(module_puts == 1 && !module_refs);
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

static void test_unpinned_context(void)
{
	struct crystalhd_cmd empty = {0};

	reset();
	crystalhd_session_unpin(&empty);
	crystalhd_session_unpin(&empty);
	assert(!empty.session_module_pinned && !module_refs && !module_puts);
	assert_released();
}

static void test_permanent_quarantine(void)
{
	struct crystalhd_adp *adp;
	struct crystalhd_hw *hw;
	void *dio, *elements;
	struct file first, stale;
	unsigned int clears, waits;

	reset();
	adp = attach(true, true);
	first = bind_user(adp, 0);
	stale = bind_user(adp, 1);
	hw = adp->cmds.hw_ctx;
	dio = adp->fill_byte_pool;
	elements = adp->elem_pool_head;
	/* Even an apparently empty pending queue cannot rescue a rejected
	 * MASTER clear. Removal must retain the complete live DMA inventory.
	 */
	ignore_master_clear = true;
	pending_result = 1;
	assert(!chd_dec_fail_closed(adp, -EIO));
	assert(!chd_dec_fail_closed(adp, -EIO));
	simulate_pci_core_restore();
	assert(chd_dma_quarantine == adp && pci.refs == 1 && pci_gets == 1 && !pci_puts);
	assert(!adp->dma_terminal_quiesced && !dma_drained);
	assert(master && !pending_waits && !status_reads);
	assert(chd_dec_close(NULL, &first) == 0 && !first.private_data);
	assert(module_refs == 1 && !module_puts && !dma_frees && !stream_releases);
	chd_dec_pci_remove(&pci);
	assert(!g_adp_info && !pci.data && !frontend_live && !chdev_live && !class_live);
	assert(chd_dma_quarantine == adp && pci.refs == 1 && pci_gets == 1 && !pci_puts);
	assert(adp->cmds.adp == adp && adp->cmds.hw_ctx == hw && hw->rx_freeq == adp);
	assert(adp->fill_byte_pool == dio && adp->elem_pool_head == elements);
	assert(adp->cmds.session_owner == &adp->cmds.user[0] && adp->cmds.session_module_pinned);
	assert(adp->cmds.stream == (struct crystalhd_stream *)&stream_token && stream_live);
	assert(adp->mem_addr == &bar_tokens[0] && adp->i2o_addr == &bar_tokens[1]);
	assert(bars_live[0] && bars_live[1] && device_live && regions_live && adp->l0s.owned);
	assert(!released[ADAPTER] && !released[HARDWARE] && !released[DIO_POOL] &&
	       !released[ELEM_POOL] && released[IODATA] == allocated[IODATA]);
	assert(!dma_frees && !stream_releases && !module_puts && module_refs == 1);
	assert(!l0s_releases && !bar_unmaps && !device_disables && !region_releases);
	clears = master_clears;
	waits = pending_waits;
	chd_dec_pci_remove(&pci);
	assert(chd_dec_close(NULL, &stale) == 0 && !stale.private_data);
	assert(master_clears == clears && pending_waits == waits && irq_frees == 1);
	assert(pci_gets == 1 && !pci_puts && module_refs == 1 && !module_puts);
	assert(chd_dma_quarantine == adp && !chd_device_lock.readers && !chd_device_lock.writers);
	/* Deliberately last: retained allocations remain reachable until process
	 * exit, matching quarantine. No fake DMA stop or test-only release occurs.
	 */
}

static void test_no_session_inventory(void)
{
	struct crystalhd_adp *adp;
	struct crystalhd_hw hw = {0};
	unsigned int field;
	int token;

	reset();
	adp = attach(false, false);
	adp->present = 0;
	adp->cmds.cin_wait_exit = 1;
	down_write(&chd_device_lock);
	assert(chd_dec_session_dma_absent(adp));
	adp->cmds.hw_ctx = &hw;
	assert(chd_dec_session_dma_absent(adp));
	for (field = 0; field < 21; field++) {
		unsigned int before = warnings;

		switch (field) {
		case 0: adp->cmds.session_owner = &token; break;
		case 1: adp->cmds.stream = (struct crystalhd_stream *)&token; break;
		case 2: adp->fill_byte_pool = &token; break;
		case 3: adp->elem_pool_head = &token; break;
		case 4: adp->ua_map_free_head = &token; break;
		case 5: adp->cmds.adp = NULL; break;
		case 6: hw.rx_pkt_pool_head = &token; break;
		case 7: hw.rx_fallback_head = &token; break;
		case 8: hw.rx_actq = &token; break;
		case 9: hw.rx_rdyq = &token; break;
		case 10: hw.rx_freeq = &token; break;
		case 11: hw.tx_actq = &token; break;
		case 12: hw.tx_freeq = &token; break;
		default:
			switch ((field - 13) % 4) {
			case 0: hw.tx_pkt_pool[(field - 13) / 4].desc_mem.pdma_desc_start = &token; break;
			case 1: hw.tx_pkt_pool[(field - 13) / 4].buffer = &token; break;
			case 2: hw.tx_pkt_pool[(field - 13) / 4].call_back = &token; break;
			case 3: hw.tx_pkt_pool[(field - 13) / 4].cb_context = &token; break;
			}
		}
		assert(!chd_dec_session_dma_absent(adp) && warnings == before + 1);
		assert(!module_refs && !module_puts && !pci_gets && !pci_puts);
		adp->cmds.session_owner = NULL;
		adp->cmds.stream = NULL;
		adp->fill_byte_pool = adp->elem_pool_head = adp->ua_map_free_head = NULL;
		adp->cmds.adp = adp;
		memset(&hw, 0, sizeof(hw));
	}
	adp->cmds.hw_ctx = NULL;
	up_write(&chd_device_lock);
	pending_result = 0;
	chd_dec_pci_remove(&pci);
	assert(!dma_frees && !module_refs && !module_puts && !pci_gets);
	assert_released();
}

static void test_terminal_proof_and_recovery(void)
{
	struct crystalhd_adp *adp;
	int external_owner;
	unsigned int waits, commands, statuses, clears;

	reset();
	adp = attach(true, true);
	adp->cmds.session_owner = &external_owner;
	adp->cmds.retain_rx_on_suspend = false;
	adp->present = 0;
	adp->cmds.cin_wait_exit = 1;
	down_write(&chd_device_lock);
	adp->present = 1; /* Deliberately violate the private helper's prerequisite. */
	assert(!chd_dec_quiesce_terminal_dma(adp));
	assert(!adp->dma_terminal_quiesced && !master_clears && !pending_waits && irq_live);
	adp->present = 0;
	up_write(&chd_device_lock);
	pending_result = 0;
	assert(!chd_dec_fail_closed(adp, -EIO));
	assert(chd_dma_quarantine == adp && pci.refs == 1 && module_refs == 1);
	pending_result = 1;
	assert(chd_dec_fail_closed(adp, -EIO));
	assert(adp->dma_terminal_quiesced && !adp->cmds.adp && !module_refs && module_puts == 1);
	assert(chd_dma_quarantine == adp && pci.refs == 1 && pci_gets == 1 && !pci_puts);
	waits = pending_waits;
	commands = command_reads;
	statuses = status_reads;
	clears = master_clears;
	pending_result = 0;
	command_error = status_error = -EIO;
	chd_dec_pci_remove(&pci);
	assert(pending_waits == waits && pci_puts == 1 && dma_frees == 1 && module_puts == 1);
	assert(command_reads == commands && status_reads == statuses && master_clears == clears);
	assert_released();
}

enum fence_fault {
	CLEAR_IGNORED, COMMAND_ERROR_ZERO, COMMAND_ERROR_ONES, COMMAND_ALL_ONES,
	NOT_PCIE, WAIT_TIMEOUT, STATUS_ERROR_ZERO, STATUS_ERROR_ONES,
	STATUS_PENDING, STATUS_ALL_ONES, FENCE_FAULT_COUNT
};

static void set_fence_fault(enum fence_fault fault)
{
	ignore_master_clear = fault == CLEAR_IGNORED;
	express = fault != NOT_PCIE;
	command_error = fault == COMMAND_ERROR_ZERO || fault == COMMAND_ERROR_ONES ? -EIO : 0;
	command_output = fault == COMMAND_ERROR_ONES || fault == COMMAND_ALL_ONES ? 0xffff : 0;
	pending_result = fault != WAIT_TIMEOUT;
	status_error = fault == STATUS_ERROR_ZERO || fault == STATUS_ERROR_ONES ? -EIO : 0;
	status_output = fault == STATUS_ERROR_ONES || fault == STATUS_ALL_ONES ? 0xffff :
		fault == STATUS_PENDING ? PCI_EXP_DEVSTA_TRPND : 0;
}

static void test_fence_faults(void)
{
	enum fence_fault fault;

	for (fault = 0; fault < FENCE_FAULT_COUNT; fault++) {
		struct crystalhd_adp *adp;
		struct crystalhd_hw *hw;
		struct file binding;
		void *dio, *elements;
		unsigned int attempt;

		reset();
		adp = attach(true, true);
		binding = bind_user(adp, 0);
		hw = adp->cmds.hw_ctx;
		dio = adp->fill_byte_pool;
		elements = adp->elem_pool_head;
		set_fence_fault(fault);
		for (attempt = 1; attempt <= 2; attempt++) {
			assert(!chd_dec_fail_closed(adp, -EIO));
			assert(!adp->dma_terminal_quiesced && !dma_drained);
			assert(master == (fault == CLEAR_IGNORED));
			assert(command_reads == attempt && master_clears == attempt);
			assert(pending_waits == (fault >= WAIT_TIMEOUT ? attempt : 0));
			assert(status_reads == (fault > WAIT_TIMEOUT ? attempt : 0));
			assert(!pci.state_saved && !irq_live && !msi_live);
			assert(chd_dma_quarantine == adp && pci_gets == 1 && pci.refs == 1 && !pci_puts);
			assert(module_refs == 1 && !module_puts && !dma_frees && !stream_releases);
			assert(adp->cmds.hw_ctx == hw && hw->rx_freeq == adp);
			assert(adp->fill_byte_pool == dio && adp->elem_pool_head == elements);
			assert(adp->cmds.stream == (struct crystalhd_stream *)&stream_token && stream_live);
			assert(adp->cmds.session_owner == &adp->cmds.user[0] && adp->cmds.session_module_pinned);
			assert(bars_live[0] && bars_live[1] && device_live && regions_live && adp->l0s.owned);
			assert(!released[ADAPTER] && !released[HARDWARE] && !released[DIO_POOL] && !released[ELEM_POOL]);
			simulate_pci_core_restore();
		}
		assert(chd_dec_close(NULL, &binding) == 0 && !binding.private_data);
		assert(module_refs == 1 && !module_puts && !dma_frees);
		/* Recovery must use a fresh complete proof, not the failed wait or
		 * read. Unrelated successful COMMAND/DEVSTA bits are harmless.
		 */
		ignore_master_clear = false;
		express = true;
		command_error = status_error = 0;
		command_output = 0x0103;
		status_output = 0x0009;
		pending_result = 1;
		chd_dec_pci_remove(&pci);
		assert(pci_gets == 1 && pci_puts == 1 && dma_frees == 1 && module_puts == 1);
		assert_released();
	}

	for (fault = 0; fault < FENCE_FAULT_COUNT; fault++) {
		struct crystalhd_adp *adp;

		reset();
		adp = attach(false, true);
		adp->cmds.hw_ctx = allocate(sizeof(*adp->cmds.hw_ctx), HARDWARE);
		set_fence_fault(fault);
		chd_dec_pci_remove(&pci);
		/* Empty inventory, not a false DMA fence, authorizes this cleanup.
		 * A rejected clear can leave MASTER set but there is no owned DMA.
		 */
		assert(master == (fault == CLEAR_IGNORED) && !dma_drained);
		assert(!pci_gets && !pci_puts && !module_refs && !module_puts && !dma_frees);
		assert(released[HARDWARE] == 1);
		assert_released();
	}
}

static void test_empty_preopened_context(void)
{
	struct crystalhd_adp *adp;

	reset();
	adp = attach(false, true);
	adp->cmds.hw_ctx = allocate(sizeof(*adp->cmds.hw_ctx), HARDWARE);
	pending_result = 0;
	chd_dec_pci_remove(&pci);
	assert(released[HARDWARE] == 1 && !dma_frees && !module_refs && !module_puts);
	assert(!pci_gets && !pci_puts);
	assert_released();
}

int main(void)
{
	test_remove();
	test_fail_stop_then_remove();
	test_external_owner_teardown();
	test_stale_close();
	test_unpinned_context();
	test_no_session_inventory();
	test_terminal_proof_and_recovery();
	test_fence_faults();
	test_empty_preopened_context();
	test_permanent_quarantine();
	printf("Device lifetime: %u scenarios passed\n", scenarios);
	return 0;
}
