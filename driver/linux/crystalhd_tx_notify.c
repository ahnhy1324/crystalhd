/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Isolated host-only copied-value ledger. No register/DRAM access or allocation
 * in record paths; storage survives ordinary and failed device close. */
#include <linux/debugfs.h>
#include <linux/err.h>
#include <linux/ktime.h>
#include <linux/module.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/types.h>
#include "crystalhd_tx_notify.h"

struct chd_tx_notify_record {
	u64 time_ns, context;
	u32 event, count, value[CHD_TX_NOTIFY_VALUES];
};
struct chd_tx_notify_store {
	u64 calls, dropped;
	u32 count;
	bool saturated, invalid;
	struct chd_tx_notify_record records[CHD_TX_NOTIFY_CAPACITY];
};
static struct chd_tx_notify_store tx_store;
static DEFINE_SPINLOCK(tx_lock);
static struct dentry *tx_dir;
static u64 tx_now(void) { return ktime_get_mono_fast_ns(); }
static void tx_lock_irq(unsigned long *flags) { spin_lock_irqsave(&tx_lock, *flags); }
static void tx_unlock_irq(unsigned long flags) { spin_unlock_irqrestore(&tx_lock, flags); }

/* TX notify core begin: exercised verbatim by the device-free harness. */
void crystalhd_tx_notify(struct crystalhd_hw *hw, uint32_t event,
	const uint32_t *values, uint32_t count)
{
	struct chd_tx_notify_record *r;
	unsigned long flags;
	u32 i;

	tx_lock_irq(&flags);
	if (tx_store.calls != ~(u64)0)
		tx_store.calls++;
	else
		tx_store.saturated = true;
	if (!values || !count || count > CHD_TX_NOTIFY_VALUES ||
	    event < CHD_TX_OPEN || event > CHD_TX_NOTIFY_LAST) {
		tx_store.invalid = true;
		goto drop;
	}
	if (tx_store.count == CHD_TX_NOTIFY_CAPACITY)
		goto drop;
	r = &tx_store.records[tx_store.count++];
	r->time_ns = tx_now();
	/* Identity only: never dereference this pointer, including at readout. */
	r->context = (unsigned long)hw;
	r->event = event;
	r->count = count;
	for (i = 0; i < count; i++)
		r->value[i] = values[i];
	tx_unlock_irq(flags);
	return;
drop:
	if (tx_store.dropped != ~(u64)0)
		tx_store.dropped++;
	else
		tx_store.saturated = true;
	tx_unlock_irq(flags);
}
/* TX notify core end. */

static int tx_show(struct seq_file *seq, void *unused)
{
	struct chd_tx_notify_store *snapshot;
	unsigned long flags;
	u32 i, j;
	(void)unused;

	snapshot = kvzalloc(sizeof(*snapshot), GFP_KERNEL);
	if (!snapshot)
		return -ENOMEM;
	tx_lock_irq(&flags);
	*snapshot = tx_store;
	tx_unlock_irq(flags);
	seq_printf(seq, "tx-notify=1 capacity=%u count=%u calls=%llu dropped=%llu saturated=%u invalid=%u read_ns=%llu extra_device_reads=0 extra_device_writes=0\n",
		CHD_TX_NOTIFY_CAPACITY, snapshot->count, snapshot->calls,
		snapshot->dropped, snapshot->saturated, snapshot->invalid, tx_now());
#ifdef CRYSTALHD_RX_NOTIFY_RESEARCH
	seq_puts(seq, "rx-notify=1 backend=existing-receipt-hooks fifo_queries_added=0\n");
#endif
	for (i = 0; i < snapshot->count; i++) {
		const struct chd_tx_notify_record *r = &snapshot->records[i];
		seq_printf(seq, "record=%u time_ns=%llu context=%llx event=%u values=%u",
			i, r->time_ns, r->context, r->event, r->count);
		for (j = 0; j < r->count; j++)
			seq_printf(seq, " v%u=%08x", j, r->value[j]);
		seq_putc(seq, '\n');
	}
	kvfree(snapshot);
	return 0;
}
static int tx_open(struct inode *inode, struct file *file)
{
	return single_open(file, tx_show, inode->i_private);
}
static const struct file_operations tx_fops = {
	.owner = THIS_MODULE, .open = tx_open, .read = seq_read,
	.llseek = seq_lseek, .release = single_release,
};
int crystalhd_tx_notify_init(void)
{
	struct dentry *file;

	tx_dir = debugfs_create_dir("crystalhd-tx-notify", NULL);
	if (IS_ERR_OR_NULL(tx_dir))
		return tx_dir ? PTR_ERR(tx_dir) : -ENOMEM;
	file = debugfs_create_file("trace", 0400, tx_dir, NULL, &tx_fops);
	if (IS_ERR_OR_NULL(file)) {
		int status = file ? PTR_ERR(file) : -ENOMEM;
		debugfs_remove_recursive(tx_dir);
		tx_dir = NULL;
		return status;
	}
	return 0;
}
void crystalhd_tx_notify_cleanup(void)
{
	debugfs_remove_recursive(tx_dir);
	tx_dir = NULL;
}
