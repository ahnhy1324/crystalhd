// SPDX-License-Identifier: GPL-2.0-or-later
/* Compile the actual driver helper against a deterministic PCI model. */
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef uint16_t u16;
typedef uint32_t u32;
#define BIT(n) (1U << (n))
#define PCI_VENDOR_ID_BROADCOM 0x14e4
#define BC_PCI_DEVID_FLEA 0x1615
#define PCI_EXP_TYPE_ENDPOINT 0
#define PCI_EXP_TYPE_ROOT_PORT 4
#define PCI_EXP_TYPE_DOWNSTREAM 6
#define PCI_EXP_LNKCTL 0x10
#define PCI_EXP_LNKCTL_ASPM_L0S 1
#define PCI_EXP_LNKCTL_ASPM_L1 2
#define PCIE_LINK_STATE_L0S TEST_API_L0S
#define PCI_FUNC(devfn) ((devfn) & 7)
#define MOCK_UNUSED __attribute__((unused))
#ifdef CONFIG_PCIEASPM
#define IS_ENABLED(option) 1
#else
#define IS_ENABLED(option) 0
#endif

struct pci_dev;
struct device { int unused; };
struct pci_bus {
	struct pci_dev *self;
	struct pci_dev *children[3];
	unsigned int count;
};
struct pci_dev {
	unsigned int vendor, device, devfn;
	bool multifunction, express;
	int type, index, irq;
	struct device dev;
	u16 lnkctl;
	struct pci_bus *bus, *subordinate;
	unsigned int refs;
};

enum operation { READ_CONFIG, WRITE_CONFIG, CALL_API };
struct event { enum operation op; int device; unsigned int value; };
static struct event events[128];
static unsigned int event_count, reads, writes, api_calls, walks;
static unsigned int fail_read, fail_write, mismatch_read, ignored_write;
static bool failed_write_mutates;
static int api_result, read_error, write_error;
static bool api_applies;
static struct pci_dev endpoint, parent, sibling;
static struct pci_bus root_bus, link_bus;
static unsigned int checks, failures, groups;
static const char *case_name;

#define CHECK(condition) do { \
	checks++; \
	if (!(condition)) { \
		fprintf(stderr, "%s:%d: %s: %s\n", __FILE__, __LINE__, \
			case_name, #condition); \
		failures++; \
	} \
} while (0)

static void record(enum operation op, struct pci_dev *dev, unsigned int value)
{
	if (event_count < sizeof(events) / sizeof(events[0]))
		events[event_count++] = (struct event){op, dev->index, value};
}

static void reset_model(const char *name, u16 child_bits, u16 parent_bits)
{
	case_name = name;
	groups++;
	memset(events, 0, sizeof(events));
	event_count = reads = writes = api_calls = walks = 0;
	fail_read = fail_write = mismatch_read = ignored_write = 0;
	failed_write_mutates = false;
	api_result = -EPERM;
	read_error = write_error = -EIO;
	api_applies = false;
	root_bus = (struct pci_bus){0};
	link_bus = (struct pci_bus){0};
	parent = (struct pci_dev){.vendor = 0x8086, .device = 0x1234,
		.express = true, .type = PCI_EXP_TYPE_ROOT_PORT, .index = 1,
		.lnkctl = parent_bits, .bus = &root_bus, .subordinate = &link_bus};
	endpoint = (struct pci_dev){.vendor = PCI_VENDOR_ID_BROADCOM,
		.device = BC_PCI_DEVID_FLEA, .express = true,
		.type = PCI_EXP_TYPE_ENDPOINT, .index = 0,
		.lnkctl = child_bits, .bus = &link_bus};
	sibling = endpoint;
	sibling.devfn = 8;
	sibling.index = 2;
	link_bus.self = &parent;
	link_bus.children[0] = &endpoint;
	link_bus.count = 1;
}

static struct pci_dev *pci_dev_get(struct pci_dev *dev)
{ dev->refs++; return dev; }
static void pci_dev_put(struct pci_dev *dev)
{ CHECK(dev->refs > 0); dev->refs--; }

static bool MOCK_UNUSED pci_is_pcie(struct pci_dev *dev)
{ return dev && dev->express; }
static int MOCK_UNUSED pci_pcie_type(struct pci_dev *dev)
{ return dev->type; }
static void MOCK_UNUSED pci_walk_bus(struct pci_bus *bus,
		int (*visit)(struct pci_dev *, void *), void *data)
{
	unsigned int i;
	walks++;
	for (i = 0; i < bus->count; i++)
		if (visit(bus->children[i], data))
			break;
}

static int pcie_capability_read_word(struct pci_dev *dev, int reg, u16 *value)
{
	CHECK(dev == &endpoint || dev == &parent);
	CHECK(reg == PCI_EXP_LNKCTL);
	reads++;
	record(READ_CONFIG, dev, dev->lnkctl);
	if (reads == fail_read)
		return read_error;
	*value = dev->lnkctl;
	if (reads == mismatch_read)
		*value ^= PCI_EXP_LNKCTL_ASPM_L0S;
	return 0;
}

static int MOCK_UNUSED pcie_capability_write_word(struct pci_dev *dev,
		int reg, u16 value)
{
	CHECK(dev == &endpoint || dev == &parent);
	CHECK(reg == PCI_EXP_LNKCTL);
	writes++;
	record(WRITE_CONFIG, dev, value);
	if (writes == fail_write) {
		if (failed_write_mutates)
			dev->lnkctl = value;
		return write_error;
	}
	if (writes != ignored_write)
		dev->lnkctl = value;
	return 0;
}

static int MOCK_UNUSED pcie_capability_clear_and_set_word(struct pci_dev *dev,
		int reg, u16 clear, u16 set)
{
	u16 value;
	int error = pcie_capability_read_word(dev, reg, &value);
	if (error)
		return error;
	return pcie_capability_write_word(dev, reg, (value & ~clear) | set);
}
static int MOCK_UNUSED pcie_capability_clear_word(struct pci_dev *dev,
		int reg, u16 clear)
{ return pcie_capability_clear_and_set_word(dev, reg, clear, 0); }
static int MOCK_UNUSED pcie_capability_set_word(struct pci_dev *dev,
		int reg, u16 set)
{ return pcie_capability_clear_and_set_word(dev, reg, 0, set); }

static int MOCK_UNUSED pci_disable_link_state(struct pci_dev *dev, int state)
{
	CHECK(dev == &endpoint);
	CHECK(state == PCIE_LINK_STATE_L0S);
	api_calls++;
	record(CALL_API, dev, state);
	if (!api_result && api_applies) {
		endpoint.lnkctl &= ~PCI_EXP_LNKCTL_ASPM_L0S;
		parent.lnkctl &= ~PCI_EXP_LNKCTL_ASPM_L0S;
	}
	return api_result;
}

#include "crystalhd_l0s.h"

static void check_original(u16 child, u16 port)
{
	CHECK(endpoint.lnkctl == child);
	CHECK(parent.lnkctl == port);
}

static void check_released(struct crystalhd_l0s_state *state)
{
	CHECK(!state->parent);
	CHECK(!parent.refs);
}

static void test_eligibility(void)
{
	unsigned int which;
	struct crystalhd_l0s_state state;

	reset_model("disabled option has no side effects", 0x43, 0x43);
	state = (struct crystalhd_l0s_state){0};
	CHECK(crystalhd_l0s_init(&endpoint, &state, false) == 0);
	CHECK(crystalhd_l0s_apply(&endpoint, &state) == 0);
	CHECK(crystalhd_l0s_restore(&endpoint, &state) == 0);
	CHECK(crystalhd_l0s_release(&endpoint, &state) == 0);
	CHECK(!reads && !writes && !api_calls && !walks);
	check_original(0x43, 0x43);
	check_released(&state);

	for (which = 0; which < 12; which++) {
		reset_model("unsupported device or topology", 0x43, 0x43);
		state = (struct crystalhd_l0s_state){0};
		switch (which) {
		case 0: endpoint.vendor = 0xffff; break;
		case 1: endpoint.device = 0x1612; break;
		case 2: endpoint.express = false; break;
		case 3: endpoint.type = PCI_EXP_TYPE_DOWNSTREAM; break;
		case 4: endpoint.multifunction = true; break;
		case 5: endpoint.devfn = 1; break;
		case 6: endpoint.bus = NULL; break;
		case 7: link_bus.self = NULL; break;
		case 8: parent.express = false; break;
		case 9: parent.type = PCI_EXP_TYPE_DOWNSTREAM; break;
		case 10: parent.subordinate = &root_bus; break;
		case 11: link_bus.count = 0; break;
		}
		CHECK(crystalhd_l0s_init(&endpoint, &state, true) == -EOPNOTSUPP);
		CHECK(!reads && !writes && !api_calls);
		CHECK(crystalhd_l0s_release(&endpoint, &state) == 0);
		check_original(0x43, 0x43);
		check_released(&state);
	}
	/* Reject extra functions both before and after our endpoint in the walk. */
	for (which = 0; which < 2; which++) {
		reset_model("shared link is rejected", 0x43, 0x43);
		state = (struct crystalhd_l0s_state){0};
		link_bus.count = 2;
		link_bus.children[which] = &endpoint;
		link_bus.children[!which] = &sibling;
		CHECK(crystalhd_l0s_init(&endpoint, &state, true) == -EOPNOTSUPP);
		CHECK(!reads && !writes && !api_calls && walks == 1);
		CHECK(crystalhd_l0s_release(&endpoint, &state) == 0);
		check_released(&state);
	}
}

static void test_raw_lifecycle(void)
{
	unsigned int child_bit, parent_bit, cycle, i;
	for (child_bit = 0; child_bit < 2; child_bit++) {
		for (parent_bit = 0; parent_bit < 2; parent_bit++) {
			struct crystalhd_l0s_state state = {0};
			u16 child = 0x442 | child_bit, port = 0x842 | parent_bit;
			reset_model("asymmetric raw lifecycle and bit preservation", child, port);
			CHECK(crystalhd_l0s_init(&endpoint, &state, true) == 0);
			CHECK(state.saved && state.raw_active && !state.core_owned);
			CHECK(state.endpoint_l0s == child_bit);
			CHECK(state.parent_l0s == parent_bit);
			CHECK(parent.refs == 1);
			check_original(child & ~1U, port & ~1U);
#ifdef CONFIG_PCIEASPM
			CHECK(api_calls == 1);
#else
			CHECK(api_calls == 0);
#endif
			/* Disable downstream endpoint before upstream port. */
			for (i = 0; i < event_count && events[i].op != WRITE_CONFIG; i++)
				;
			CHECK(i < event_count && events[i].device == 0);
			CHECK(crystalhd_l0s_apply(&endpoint, &state) == 0);
			CHECK(state.endpoint_l0s == child_bit && state.parent_l0s == parent_bit);
			/* An unrelated Link Control change must survive restoration. */
			endpoint.lnkctl ^= 0x100;
			parent.lnkctl ^= 0x200;
			child ^= 0x100;
			port ^= 0x200;
			for (cycle = 0; cycle < 3; cycle++) {
				unsigned int before = event_count;
				CHECK(crystalhd_l0s_restore(&endpoint, &state) == 0);
				CHECK(!state.raw_active && state.saved);
				check_original(child, port);
				/* Restore upstream first; preserve independent original bits. */
				for (i = before; i < event_count && events[i].op != WRITE_CONFIG; i++)
					;
				CHECK(i < event_count && events[i].device == 1);
				before = event_count;
				CHECK(crystalhd_l0s_restore(&endpoint, &state) == 0);
				CHECK(event_count == before);
				CHECK(crystalhd_l0s_apply(&endpoint, &state) == 0);
				check_original(child & ~1U, port & ~1U);
			}
			CHECK(crystalhd_l0s_release(&endpoint, &state) == 0);
			check_original(child, port);
			check_released(&state);
			CHECK(crystalhd_l0s_release(&endpoint, &state) == 0);
		}
	}
}

static void test_core_ownership(void)
{
#ifdef CONFIG_PCIEASPM
	struct crystalhd_l0s_state state;
	unsigned int which;
	reset_model("PCI-core-owned state is never raw-restored", 0x43, 0x43);
	state = (struct crystalhd_l0s_state){0};
	api_result = 0;
	api_applies = true;
	CHECK(crystalhd_l0s_init(&endpoint, &state, true) == 0);
	CHECK(state.core_owned && !state.raw_active && !state.saved);
	CHECK(!writes && api_calls == 1);
	check_original(0x42, 0x42);
	CHECK(crystalhd_l0s_restore(&endpoint, &state) == 0);
	CHECK(!writes && api_calls == 1);
	/* A later API denial must not turn a core-owned setting into a raw one. */
	api_result = -EPERM;
	CHECK(crystalhd_l0s_apply(&endpoint, &state) == -EPERM);
	CHECK(!writes && !state.raw_active);
	CHECK(crystalhd_l0s_release(&endpoint, &state) == 0);
	check_original(0x42, 0x42);
	check_released(&state);

	for (which = 0; which < 5; which++) {
		reset_model("ineffective successful API cannot authorize raw fallback", 0x43, 0x43);
		state = (struct crystalhd_l0s_state){0};
		api_result = 0;
		api_applies = false;
		/* Either direction left enabled is insufficient, not merely the EP. */
		if (which == 1) endpoint.lnkctl = 0x42;
		if (which == 2) parent.lnkctl = 0x42;
		if (which == 3) fail_read = 3;
		if (which == 4) fail_read = 4;
		CHECK(crystalhd_l0s_init(&endpoint, &state, true) == -EIO);
		CHECK(state.core_owned && !state.raw_active && !writes);
		CHECK(crystalhd_l0s_release(&endpoint, &state) == 0);
		CHECK(!writes);
		check_released(&state);
	}
	for (which = 0; which < 3; which++) {
		static const int errors[] = {-EINVAL, -EIO, -EOPNOTSUPP};
		reset_model("unknown API errors never authorize raw fallback", 0x43, 0x43);
		state = (struct crystalhd_l0s_state){0};
		api_result = errors[which];
		CHECK(crystalhd_l0s_init(&endpoint, &state, true) == errors[which]);
		CHECK(!writes && !state.raw_active && !state.core_owned);
		CHECK(crystalhd_l0s_release(&endpoint, &state) == 0);
		check_original(0x43, 0x43);
		check_released(&state);
	}
#endif
}

static void test_apply_faults(void)
{
	struct crystalhd_l0s_state state;
	unsigned int read_count, write_count, which, after_write;
	reset_model("count actual production raw apply operations", 0x43, 0x43);
	state = (struct crystalhd_l0s_state){0};
	CHECK(crystalhd_l0s_init(&endpoint, &state, true) == 0);
	read_count = reads;
	write_count = writes;
	CHECK(read_count >= 4 && write_count == 2);
	CHECK(crystalhd_l0s_release(&endpoint, &state) == 0);
	for (which = 1; which <= read_count; which++) {
		reset_model("each raw apply read failure rolls back", 0x43, 0x43);
		state = (struct crystalhd_l0s_state){0};
		fail_read = which;
		CHECK(crystalhd_l0s_init(&endpoint, &state, true) == -EIO);
		CHECK(!state.raw_active);
		check_original(0x43, 0x43);
		CHECK(crystalhd_l0s_release(&endpoint, &state) == 0);
		check_released(&state);
	}
	for (after_write = 0; after_write < 2; after_write++) {
		for (which = 1; which <= write_count; which++) {
			reset_model("each raw apply write failure rolls back", 0x43, 0x43);
			state = (struct crystalhd_l0s_state){0};
			fail_write = which;
			failed_write_mutates = after_write;
			CHECK(crystalhd_l0s_init(&endpoint, &state, true) == -EIO);
			CHECK(!state.raw_active);
			check_original(0x43, 0x43);
			CHECK(crystalhd_l0s_release(&endpoint, &state) == 0);
			check_released(&state);
		}
	}
	for (which = 1; which <= write_count; which++) {
		reset_model("ignored write must fail its readback", 0x43, 0x43);
		state = (struct crystalhd_l0s_state){0};
		ignored_write = which;
		CHECK(crystalhd_l0s_init(&endpoint, &state, true) == -EIO);
		CHECK(!state.raw_active);
		check_original(0x43, 0x43);
		CHECK(crystalhd_l0s_release(&endpoint, &state) == 0);
		check_released(&state);
	}
	for (which = 0; which < 2; which++) {
		reset_model("unavailable configuration space is rejected", 0x43, 0x43);
		state = (struct crystalhd_l0s_state){0};
		if (which) parent.lnkctl = 0xffff;
		else endpoint.lnkctl = 0xffff;
		CHECK(crystalhd_l0s_init(&endpoint, &state, true) == -ENODEV);
		CHECK(!writes && !api_calls);
		CHECK(crystalhd_l0s_release(&endpoint, &state) == 0);
		check_released(&state);
	}
}

static void test_restore_faults(void)
{
	unsigned int kind, which;
	/* Four reads and two writes: the actual helper's two masked RMW+readbacks. */
	for (kind = 0; kind < 4; kind++) {
		unsigned int count = kind == 0 ? 4 : 2;
		for (which = 1; which <= count; which++) {
			struct crystalhd_l0s_state state = {0};
			unsigned int before;
			reset_model("restore failure retains ownership for retry", 0x43, 0x43);
			CHECK(crystalhd_l0s_init(&endpoint, &state, true) == 0);
			before = event_count;
			if (kind == 0) fail_read = reads + which;
			else if (kind == 3) ignored_write = writes + which;
			else {
				fail_write = writes + which;
				failed_write_mutates = kind == 2;
			}
			CHECK(crystalhd_l0s_restore(&endpoint, &state) == -EIO);
			CHECK(state.raw_active && state.parent == &parent && parent.refs == 1);
			CHECK(event_count > before);
			/* Clear the injected fault and retry before releasing the parent. */
			fail_read = fail_write = ignored_write = 0;
			CHECK(crystalhd_l0s_restore(&endpoint, &state) == 0);
			CHECK(!state.raw_active);
			check_original(0x43, 0x43);
			CHECK(crystalhd_l0s_release(&endpoint, &state) == 0);
			check_released(&state);
		}
	}
	/* Both cleanup operations are attempted even if restoring the port fails. */
	{
		struct crystalhd_l0s_state state = {0};
		reset_model("final release reports restore failure and releases reference", 0x43, 0x43);
		CHECK(crystalhd_l0s_init(&endpoint, &state, true) == 0);
		fail_write = writes + 1;
		CHECK(crystalhd_l0s_release(&endpoint, &state) == -EIO);
		CHECK(endpoint.lnkctl == 0x43 && parent.lnkctl == 0x42);
		check_released(&state);
	}
}

static void test_fault_edges(void)
{
	struct crystalhd_l0s_state state;
	unsigned int which;
	for (which = 0; which < 4; which++) {
		u16 child = 0x40 | which, port = 0x840 | (3 - which);
		reset_model("L1 is preserved in every enabled/disabled combination", child, port);
		state = (struct crystalhd_l0s_state){0};
		CHECK(crystalhd_l0s_init(&endpoint, &state, true) == 0);
		check_original(child & ~1U, port & ~1U);
		CHECK(crystalhd_l0s_release(&endpoint, &state) == 0);
		check_original(child, port);
		check_released(&state);
	}
	for (which = 0; which < 2; which++) {
		reset_model("positive PCIBIOS error becomes negative errno", 0x43, 0x43);
		state = (struct crystalhd_l0s_state){0};
		if (which) {
			fail_write = 2;
			write_error = 0x87; /* PCIBIOS_SET_FAILED */
			failed_write_mutates = true;
		} else {
			fail_read = 1;
			read_error = 0x86; /* PCIBIOS_DEVICE_NOT_FOUND */
		}
		CHECK(crystalhd_l0s_init(&endpoint, &state, true) == -EIO);
		check_original(0x43, 0x43);
		CHECK(crystalhd_l0s_release(&endpoint, &state) == 0);
		check_released(&state);
	}
	reset_model("failed apply plus failed rollback retains recoverable ownership", 0x43, 0x43);
	state = (struct crystalhd_l0s_state){0};
	fail_write = 2;
	failed_write_mutates = true;
	/* Initial2 reads, endpoint RMW+readback, port RMW, rollback port RMW. */
	fail_read = 6;
	CHECK(crystalhd_l0s_init(&endpoint, &state, true) == -EIO);
	CHECK(state.raw_active && state.saved && parent.refs == 1);
	CHECK(endpoint.lnkctl == 0x43 && parent.lnkctl == 0x42);
	fail_read = fail_write = 0;
	CHECK(crystalhd_l0s_restore(&endpoint, &state) == 0);
	check_original(0x43, 0x43);
	CHECK(crystalhd_l0s_release(&endpoint, &state) == 0);
	check_released(&state);

	reset_model("resume apply failure restores saved pre-workaround state", 0x42, 0x43);
	state = (struct crystalhd_l0s_state){0};
	CHECK(crystalhd_l0s_init(&endpoint, &state, true) == 0);
	CHECK(crystalhd_l0s_restore(&endpoint, &state) == 0);
	check_original(0x42, 0x43);
	fail_write = writes + 2;
	failed_write_mutates = true;
	CHECK(crystalhd_l0s_apply(&endpoint, &state) == -EIO);
	CHECK(state.saved && !state.raw_active && !state.endpoint_l0s && state.parent_l0s);
	check_original(0x42, 0x43);
	CHECK(crystalhd_l0s_release(&endpoint, &state) == 0);
	check_released(&state);
}

/* The runner extracts complete IRQ/PM functions from crystalhd_lnx.c. */
struct crystalhd_hw { bool dma_fault; };
struct crystalhd_cmd { int cin_wait_exit; struct crystalhd_hw *hw_ctx; };
struct crystalhd_adp {
	struct pci_dev *pdev;
	const char *name;
	int user_lock;
	bool irq_registered;
	int msi;
	int present;
	struct crystalhd_cmd cmds;
	struct crystalhd_l0s_state l0s;
};
#define KERN_ERR ""
#define IRQF_SHARED 0x80UL
#define printk(...) ((void)0)
#define dev_err(...) mock_log(__VA_ARGS__)
#define dev_warn(...) mock_log(__VA_ARGS__)
#define READ_ONCE(value) (value)
#define WRITE_ONCE(value, update) ((value) = (update))
#define BC_STS_SUCCESS 0
#define PCI_D0 0
typedef int BC_STATUS;
typedef int pm_message_t;
typedef int crystalhd_ioctl_data;
static void mock_log(const void *dev, ...) { (void)dev; }
static struct crystalhd_adp irq_adapter;
static unsigned int msi_requests, msi_disables, irq_requests, irq_frees;
static int msi_result, irq_result;
static bool msi_active, irq_active;
static bool pm_tracking, pm_master, pm_bound, pm_alloc_fail;
static int pm_enable_error, pm_suspend_error, pm_resume_error, pm_pending;
static int chd_device_lock, pm_lock_depth, pm_user_lock_depth;
static unsigned int pm_user_locks, pm_user_unlocks;
static unsigned int pm_allocs, pm_frees, pm_saves, pm_enables, pm_disables;
static unsigned int pm_suspends, pm_resumes, pm_clears, pm_waits;
static crystalhd_ioctl_data pm_data;
static struct crystalhd_hw pm_hw;
static char pm_events[128];
static unsigned int pm_event_count;
static void pm_event(char event)
{
	if (pm_tracking && pm_event_count + 1 < sizeof(pm_events)) {
		pm_events[pm_event_count++] = event;
		pm_events[pm_event_count] = '\0';
	}
}

static int chd_dec_isr(int irq, void *argument)
{
	(void)irq;
	(void)argument;
	return 0;
}
static int pci_enable_msi(struct pci_dev *dev)
{
	CHECK(dev == &endpoint && !msi_active);
	msi_requests++;
	if (!msi_result) msi_active = true;
	return msi_result;
}
static void pci_disable_msi(struct pci_dev *dev)
{
	CHECK(dev == &endpoint && msi_active);
	msi_disables++;
	msi_active = false;
}
static int request_irq(unsigned int irq, int (*handler)(int, void *),
		unsigned long flags, const char *name, void *argument)
{
	CHECK(irq == (unsigned int)endpoint.irq && handler == chd_dec_isr);
	CHECK(flags == IRQF_SHARED && name == irq_adapter.name);
	CHECK(argument == &irq_adapter && !irq_active);
	irq_requests++;
	if (pm_tracking) {
		CHECK(pm_user_lock_depth == 1);
		CHECK(!pm_master);
		CHECK(!(endpoint.lnkctl & 1) && !(parent.lnkctl & 1));
		pm_event('Q');
	}
	if (!irq_result) irq_active = true;
	return irq_result;
}
static void free_irq(unsigned int irq, void *argument)
{
	CHECK(irq == (unsigned int)endpoint.irq && argument == &irq_adapter);
	CHECK(irq_active);
	irq_frees++;
	if (pm_tracking)
		CHECK(pm_user_lock_depth == !!irq_adapter.present);
	irq_active = false;
	pm_event('F');
}

static void down_write(int *lock)
{
	if (lock == &irq_adapter.user_lock) {
		CHECK(pm_user_lock_depth == 0);
		pm_user_lock_depth++;
		pm_user_locks++;
		return;
	}
	CHECK(lock == &chd_device_lock && pm_lock_depth == 0);
	CHECK(!pm_user_lock_depth);
	CHECK(!irq_adapter.present && irq_adapter.cmds.cin_wait_exit);
	pm_lock_depth++;
	pm_event('L');
}
static void up_write(int *lock)
{
	if (lock == &irq_adapter.user_lock) {
		CHECK(pm_user_lock_depth == 1);
		pm_user_lock_depth--;
		pm_user_unlocks++;
		return;
	}
	CHECK(lock == &chd_device_lock && pm_lock_depth == 1);
	CHECK(!pm_user_lock_depth);
	pm_lock_depth--;
	pm_event('U');
}
static void pci_clear_master(struct pci_dev *dev)
{
	CHECK(dev == &endpoint);
	pm_master = false;
	pm_clears++;
	pm_event('C');
}
static void pci_set_master(struct pci_dev *dev)
{
	CHECK(dev == &endpoint && !pm_master && irq_adapter.irq_registered);
	if (pm_tracking)
		CHECK(pm_user_lock_depth == 1);
	CHECK(!(endpoint.lnkctl & 1) && !(parent.lnkctl & 1));
	pm_master = true;
	pm_event('M');
}
static int pci_wait_for_pending_transaction(struct pci_dev *dev)
{
	CHECK(dev == &endpoint && !pm_master && pm_lock_depth == 1);
	pm_waits++;
	pm_event('W');
	return pm_pending;
}
static void *pci_get_drvdata(struct pci_dev *dev)
{
	CHECK(dev == &endpoint);
	return pm_bound ? &irq_adapter : NULL;
}
static crystalhd_ioctl_data *chd_dec_alloc_iodata(struct crystalhd_adp *adp, bool isr)
{
	CHECK(adp == &irq_adapter && !isr && pm_user_lock_depth == 1);
	pm_allocs++;
	return pm_alloc_fail ? NULL : &pm_data;
}
static void chd_dec_free_iodata(struct crystalhd_adp *adp,
		crystalhd_ioctl_data *data, bool isr)
{
	CHECK(adp == &irq_adapter && data == &pm_data && !isr &&
	      pm_user_lock_depth == 1);
	pm_frees++;
}
static BC_STATUS crystalhd_suspend(struct crystalhd_cmd *cmd,
		crystalhd_ioctl_data *data)
{
	CHECK(cmd == &irq_adapter.cmds && data == &pm_data &&
	      pm_user_lock_depth == 1);
	pm_suspends++;
	pm_event('D');
	return pm_suspend_error;
}
static BC_STATUS crystalhd_resume(struct crystalhd_cmd *cmd)
{
	CHECK(cmd == &irq_adapter.cmds && irq_adapter.irq_registered);
	CHECK(pm_user_lock_depth == 1);
	CHECK(pm_master == !pm_hw.dma_fault);
	pm_resumes++;
	pm_event('H');
	return pm_resume_error;
}
static void pci_save_state(struct pci_dev *dev)
{
	CHECK(dev == &endpoint && !irq_active && !irq_adapter.irq_registered);
	CHECK(pm_user_lock_depth == 1);
	CHECK(endpoint.lnkctl == 0x43 && parent.lnkctl == 0x43);
	pm_saves++;
	pm_event('S');
}
static void pci_restore_state(struct pci_dev *dev)
{
	CHECK(dev == &endpoint && pm_user_lock_depth == 1);
	/* Saved command can restore MASTER; actual callback must clear it. */
	pm_master = true;
	pm_event('R');
}
static int pci_enable_device(struct pci_dev *dev)
{
	CHECK(dev == &endpoint && !pm_master && pm_user_lock_depth == 1);
	pm_enables++;
	pm_event('E');
	return pm_enable_error;
}
static void pci_disable_device(struct pci_dev *dev)
{
	CHECK(dev == &endpoint && !irq_active);
	CHECK(pm_user_lock_depth == !!irq_adapter.present);
	pm_disables++;
	pm_event('X');
}
static int pci_choose_state(struct pci_dev *dev, pm_message_t state)
{ CHECK(dev == &endpoint); return state; }
static void pci_set_power_state(struct pci_dev *dev, int state)
{
	CHECK(dev == &endpoint && (state == PCI_D0 || state == 3));
	CHECK(pm_user_lock_depth == 1);
	pm_event('P');
}
#include "l0s-irq-functions.h"

static void reset_irq_model(const char *name)
{
	reset_model(name, 0x43, 0x43);
	endpoint.irq = 19;
	irq_adapter = (struct crystalhd_adp){.pdev = &endpoint, .name = "crystalhd-test"};
	msi_requests = msi_disables = irq_requests = irq_frees = 0;
	msi_result = irq_result = 0;
	msi_active = irq_active = false;
	pm_tracking = false;
}

static void test_irq_lifecycle(void)
{
	unsigned int which;
	reset_irq_model("invalid IRQ lifecycle arguments have no side effects");
	CHECK(chd_dec_enable_int(NULL) == -EINVAL);
	CHECK(chd_dec_disable_int(NULL) == -EINVAL);
	irq_adapter.pdev = NULL;
	CHECK(chd_dec_enable_int(&irq_adapter) == -EINVAL);
	CHECK(chd_dec_disable_int(&irq_adapter) == -EINVAL);
	CHECK(!msi_requests && !irq_requests && !msi_disables && !irq_frees);

	reset_irq_model("default or missing IRQ registration cannot be freed");
	CHECK(chd_dec_disable_int(&irq_adapter) == 0);
	CHECK(chd_dec_disable_int(&irq_adapter) == 0);
	CHECK(!irq_frees && !msi_disables && !irq_requests && !msi_requests);
	/* A partial setup may own MSI without owning a registered interrupt. */
	irq_adapter.msi = 1;
	msi_active = true;
	CHECK(chd_dec_disable_int(&irq_adapter) == 0);
	CHECK(chd_dec_disable_int(&irq_adapter) == 0);
	CHECK(!irq_frees && msi_disables == 1 && !irq_adapter.msi);

	reset_irq_model("successful IRQ registration is freed exactly once");
	for (which = 1; which <= 3; which++) {
		CHECK(chd_dec_enable_int(&irq_adapter) == 0);
		CHECK(irq_adapter.irq_registered && irq_adapter.msi);
		CHECK(irq_active && msi_active);
		CHECK(chd_dec_disable_int(&irq_adapter) == 0);
		CHECK(chd_dec_disable_int(&irq_adapter) == 0);
		CHECK(!irq_adapter.irq_registered && !irq_adapter.msi);
		CHECK(!irq_active && !msi_active);
		CHECK(irq_requests == which && irq_frees == which);
		CHECK(msi_requests == which && msi_disables == which);
	}

	reset_irq_model("MSI failure allows successful INTx fallback");
	msi_result = -ENOSPC;
	CHECK(chd_dec_enable_int(&irq_adapter) == 0);
	CHECK(irq_adapter.irq_registered && !irq_adapter.msi);
	CHECK(irq_active && !msi_active);
	CHECK(chd_dec_disable_int(&irq_adapter) == 0);
	CHECK(chd_dec_disable_int(&irq_adapter) == 0);
	CHECK(irq_frees == 1 && !msi_disables);

	for (which = 0; which < 2; which++) {
		reset_irq_model("failed IRQ request unwinds only resources it owns");
		msi_result = which ? -ENOSPC : 0;
		irq_result = -EBUSY;
		CHECK(chd_dec_enable_int(&irq_adapter) == -EBUSY);
		CHECK(!irq_adapter.irq_registered && !irq_adapter.msi);
		CHECK(!irq_active && !msi_active);
		CHECK(msi_disables == (which ? 0U : 1U));
		CHECK(chd_dec_disable_int(&irq_adapter) == 0);
		CHECK(chd_dec_disable_int(&irq_adapter) == 0);
		CHECK(!irq_frees && msi_disables == (which ? 0U : 1U));
	}

	reset_irq_model("failed resume followed by remove cannot double-free IRQ");
	CHECK(chd_dec_enable_int(&irq_adapter) == 0);
	CHECK(chd_dec_disable_int(&irq_adapter) == 0);
	irq_result = -EBUSY;
	CHECK(chd_dec_enable_int(&irq_adapter) == -EBUSY);
	CHECK(chd_dec_disable_int(&irq_adapter) == 0);
	CHECK(chd_dec_disable_int(&irq_adapter) == 0);
	CHECK(irq_requests == 2 && irq_frees == 1);
	CHECK(msi_requests == 2 && msi_disables == 2);
	CHECK(!irq_adapter.irq_registered && !irq_adapter.msi);
	CHECK(!irq_active && !msi_active);
}

static void reset_pm_model(const char *name)
{
	reset_irq_model(name);
	pm_master = pm_bound = true;
	pm_alloc_fail = false;
	pm_enable_error = pm_suspend_error = pm_resume_error = 0;
	pm_pending = 1;
	pm_lock_depth = pm_user_lock_depth = 0;
	pm_user_locks = pm_user_unlocks = 0;
	pm_allocs = pm_frees = pm_saves = pm_enables = pm_disables = 0;
	pm_suspends = pm_resumes = pm_clears = pm_waits = 0;
	pm_event_count = 0;
	pm_events[0] = '\0';
	pm_hw = (struct crystalhd_hw){0};
	irq_adapter.present = 1;
	irq_adapter.cmds.hw_ctx = &pm_hw;
	CHECK(crystalhd_l0s_init(&endpoint, &irq_adapter.l0s, true) == 0);
	CHECK(chd_dec_enable_int(&irq_adapter) == 0);
	pm_tracking = true;
}

static void finish_pm_model(void)
{
	pm_tracking = false;
	fail_read = fail_write = ignored_write = mismatch_read = 0;
	CHECK(chd_dec_disable_int(&irq_adapter) == 0);
	CHECK(crystalhd_l0s_release(&endpoint, &irq_adapter.l0s) == 0);
	check_released(&irq_adapter.l0s);
	CHECK(!irq_active && !msi_active && !pm_lock_depth &&
	      !pm_user_lock_depth && pm_user_locks == pm_user_unlocks);
}

static void check_pm_closed(void)
{
	unsigned int old_events = pm_event_count, old_reads = reads;
	CHECK(!irq_adapter.present && irq_adapter.cmds.cin_wait_exit);
	CHECK(!pm_master && !irq_active && !msi_active && !pm_lock_depth);
	CHECK(!irq_adapter.irq_registered && !irq_adapter.msi);
	CHECK(pm_waits == 1);
	CHECK(chd_dec_pci_suspend(&endpoint, 3) == -ENODEV);
	CHECK(chd_dec_pci_resume(&endpoint) == -ENODEV);
	CHECK(pm_event_count == old_events && reads == old_reads);
}

static void test_pm_lifecycle(void)
{
	unsigned int which;
	reset_pm_model("actual PM callbacks restore then reapply around IRQ/mastering");
	CHECK(chd_dec_pci_suspend(&endpoint, 3) == 0);
	CHECK(strcmp(pm_events, "DFSXP") == 0);
	CHECK(pm_allocs == 1 && pm_frees == 1 && pm_saves == 1);
	CHECK(!irq_adapter.l0s.raw_active && irq_adapter.present);
	pm_event_count = 0; pm_events[0] = '\0';
	CHECK(chd_dec_pci_resume(&endpoint) == 0);
	CHECK(strcmp(pm_events, "PRCEQMH") == 0);
	CHECK(irq_adapter.l0s.raw_active && irq_adapter.present);
	CHECK(pm_master && irq_active && msi_active && pm_resumes == 1);
	finish_pm_model();

	for (which = 0; which < 2; which++) {
		reset_pm_model("unavailable PM adapter rejects without hardware operations");
		if (which) irq_adapter.present = 0;
		else pm_bound = false;
		CHECK(chd_dec_pci_suspend(&endpoint, 3) == -ENODEV);
		CHECK(chd_dec_pci_resume(&endpoint) == -ENODEV);
		CHECK(!pm_event_count && !pm_allocs && !pm_suspends && !pm_enables);
		finish_pm_model();
	}

	for (which = 0; which < 2; which++) {
		reset_pm_model("pre-existing suspend allocation/decoder failure does not save PCI state");
		if (which) pm_suspend_error = 1;
		else pm_alloc_fail = true;
		CHECK(chd_dec_pci_suspend(&endpoint, 3) == -ENODEV);
		CHECK(!pm_saves && !pm_disables);
		CHECK(pm_allocs == 1 && pm_frees == which && pm_suspends == which);
		if (which) {
			CHECK(strcmp(pm_events, "DLCWFU") == 0);
			check_pm_closed();
		} else {
			CHECK(!pm_waits && irq_adapter.present && irq_active);
		}
		finish_pm_model();
	}

	for (which = 0; which < 2; which++) {
		reset_pm_model("suspend restore failure cancels before file barrier and disables DMA/IRQ");
		fail_write = writes + 1;
		pm_pending = which; /* pending-transaction timeout must not skip IRQ cleanup */
		CHECK(chd_dec_pci_suspend(&endpoint, 3) == -EIO);
		CHECK(strcmp(pm_events, "DLCWFU") == 0);
		CHECK(!pm_saves && !pm_disables && pm_allocs == pm_frees);
		check_pm_closed();
		CHECK(chd_dec_disable_int(&irq_adapter) == 0);
		CHECK(irq_frees == 1 && msi_disables == 1);
		finish_pm_model();
	}

	for (which = 0; which < 5; which++) {
		int expected = which == 3 || which == 4 ? -ENODEV : -EIO;
		reset_pm_model("each resumed resource failure fails closed and rolls back raw L0s");
		CHECK(chd_dec_pci_suspend(&endpoint, 3) == 0);
		pm_event_count = 0; pm_events[0] = '\0';
		if (which == 0) pm_enable_error = -EIO;
		else if (which == 1) fail_write = writes + 2;
		else if (which == 2) irq_result = -EIO;
		else {
			pm_resume_error = 1;
			pm_hw.dma_fault = which == 4;
		}
		CHECK(chd_dec_pci_resume(&endpoint) == expected);
		CHECK(strncmp(pm_events, "PRCE", 4) == 0);
		CHECK(strstr(pm_events, "LCW") != NULL);
		CHECK(pm_resumes == (which >= 3 ? 1U : 0U));
		check_pm_closed();
		check_original(0x43, 0x43);
		CHECK(!irq_adapter.l0s.raw_active);
		CHECK(chd_dec_disable_int(&irq_adapter) == 0);
		CHECK(irq_frees == (which >= 3 ? 2U : 1U));
		finish_pm_model();
	}
}

int main(void)
{
	test_eligibility();
	test_raw_lifecycle();
	test_core_ownership();
	test_apply_faults();
	test_restore_faults();
	test_fault_edges();
	test_irq_lifecycle();
	test_pm_lifecycle();
	printf("L0s production-helper tests: %u groups, %u checks, %u failures\n",
		groups, checks, failures);
	return failures ? 1 : 0;
}
