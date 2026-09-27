/* SPDX-License-Identifier: GPL-2.0 */
#ifndef CRYSTALHD_L0S_H
#define CRYSTALHD_L0S_H

/* PCI types/helpers are supplied by the includer. Keep the state machine
 * directly usable by the hardware-free PCI fault-injection tests.
 */
struct crystalhd_l0s_state {
	struct pci_dev *parent;
	u16 endpoint_l0s;
	u16 parent_l0s;
	bool saved;
	bool raw_active;
	bool core_owned;
};

struct crystalhd_l0s_bus_check {
	struct pci_dev *endpoint;
	bool found;
	bool shared;
};

static inline int crystalhd_l0s_check_child(struct pci_dev *dev, void *data)
{
	struct crystalhd_l0s_bus_check *check = data;

	if (dev != check->endpoint) {
		check->shared = true;
		return 1;
	}
	check->found = true;
	return 0;
}

static inline int crystalhd_l0s_read(struct pci_dev *dev, u16 *value)
{
	int rc = pcie_capability_read_word(dev, PCI_EXP_LNKCTL, value);

	if (rc)
		return rc < 0 ? rc : -EIO;
	return *value == 0xffff ? -ENODEV : 0;
}

static inline int crystalhd_l0s_write(struct pci_dev *dev, u16 bit)
{
	u16 value;
	int rc;

	rc = pcie_capability_clear_and_set_word(dev, PCI_EXP_LNKCTL,
					      PCI_EXP_LNKCTL_ASPM_L0S, bit);
	if (rc)
		return rc < 0 ? rc : -EIO;
	rc = crystalhd_l0s_read(dev, &value);
	if (rc)
		return rc;
	return (value & PCI_EXP_LNKCTL_ASPM_L0S) == bit ? 0 : -EIO;
}

static inline int crystalhd_l0s_restore(struct pci_dev *dev,
				      struct crystalhd_l0s_state *state)
{
	int rc, endpoint_rc;

	/* pci_enable_link_state() is NOT an inverse of the core disable API.
	 * Never overwrite a PCI-core-owned policy with saved raw registers.
	 */
	if (!state->raw_active || state->core_owned)
		return 0;
	if (!state->saved || !state->parent)
		return -EINVAL;

	rc = crystalhd_l0s_write(state->parent, state->parent_l0s);
	endpoint_rc = crystalhd_l0s_write(dev, state->endpoint_l0s);
	if (!rc)
		rc = endpoint_rc;
	if (!rc)
		state->raw_active = false;
	return rc;
}

static inline int crystalhd_l0s_apply(struct pci_dev *dev,
				    struct crystalhd_l0s_state *state)
{
	u16 endpoint, parent;
	int rc, rollback;

	if (!state->parent)
		return 0; /* Option disabled: no PCI configuration access. */
	rc = crystalhd_l0s_read(dev, &endpoint);
	if (rc)
		return rc;
	rc = crystalhd_l0s_read(state->parent, &parent);
	if (rc)
		return rc;

#ifdef CONFIG_PCIEASPM
	rc = pci_disable_link_state(dev, PCIE_LINK_STATE_L0S);
	if (!rc) {
		state->core_owned = true;
		rc = crystalhd_l0s_read(dev, &endpoint);
		if (!rc)
			rc = crystalhd_l0s_read(state->parent, &parent);
		if (rc)
			return rc;
		return ((endpoint | parent) & PCI_EXP_LNKCTL_ASPM_L0S) ?
			-EIO : 0;
	}
	/* Only an explicit ownership denial permits the opt-in raw fallback.
	 * Unknown errors and an ineffective successful call must not do so.
	 */
	if (rc != -EPERM || state->core_owned)
		return rc;
#endif
	if (!state->saved) {
		state->endpoint_l0s = endpoint & PCI_EXP_LNKCTL_ASPM_L0S;
		state->parent_l0s = parent & PCI_EXP_LNKCTL_ASPM_L0S;
		state->saved = true;
	}
	/* Record ownership before the first attempted write, including writes
	 * which report an error after actually changing configuration space.
	 */
	state->raw_active = true;
	rc = crystalhd_l0s_write(dev, 0);
	if (!rc)
		rc = crystalhd_l0s_write(state->parent, 0);
	if (rc) {
		rollback = crystalhd_l0s_restore(dev, state);
		if (rollback)
			return rollback;
	}
	return rc;
}

static inline int crystalhd_l0s_init(struct pci_dev *dev,
				   struct crystalhd_l0s_state *state,
				   bool requested)
{
	struct crystalhd_l0s_bus_check check = { .endpoint = dev };
	struct pci_dev *parent;

	if (!requested)
		return 0;
	if (dev->vendor != PCI_VENDOR_ID_BROADCOM || dev->device != 0x1615 ||
	    !pci_is_pcie(dev) || pci_pcie_type(dev) != PCI_EXP_TYPE_ENDPOINT ||
	    dev->multifunction || PCI_FUNC(dev->devfn) || !dev->bus)
		return -EOPNOTSUPP;
	parent = dev->bus->self;
	if (!parent || !pci_is_pcie(parent) ||
	    pci_pcie_type(parent) != PCI_EXP_TYPE_ROOT_PORT ||
	    parent->subordinate != dev->bus)
		return -EOPNOTSUPP;
	/* pci_walk_bus() holds the PCI topology read lock. Do not take the
	 * rescan/remove mutex here: probe/remove may already be beneath it.
	 */
	pci_walk_bus(dev->bus, crystalhd_l0s_check_child, &check);
	if (!check.found || check.shared)
		return -EOPNOTSUPP;
	state->parent = pci_dev_get(parent);
	return crystalhd_l0s_apply(dev, state);
}

static inline int crystalhd_l0s_release(struct pci_dev *dev,
				      struct crystalhd_l0s_state *state)
{
	int rc = crystalhd_l0s_restore(dev, state);

	if (state->parent) {
		pci_dev_put(state->parent);
		state->parent = NULL;
	}
	return rc;
}

#endif
