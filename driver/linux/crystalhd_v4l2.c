// SPDX-License-Identifier: GPL-2.0-or-later
#include <media/v4l2-device.h>

#include "crystalhd_lnx.h"

struct crystalhd_v4l2 {
	struct v4l2_device device;
	u64 generation;
	bool disconnected;
};

static void crystalhd_v4l2_release(struct v4l2_device *device)
{
	struct crystalhd_v4l2 *parent =
		container_of(device, struct crystalhd_v4l2, device);

	kfree(parent);
}

int crystalhd_v4l2_register(struct crystalhd_adp *adp)
{
	struct crystalhd_v4l2 *parent;
	int rc;

	if (!adp || !adp->pdev || !adp->generation ||
	    pci_get_drvdata(adp->pdev) != adp)
		return -EINVAL;
	if (adp->v4l2)
		return -EBUSY;

	parent = kzalloc(sizeof(*parent), GFP_KERNEL);
	if (!parent)
		return -ENOMEM;
	parent->generation = adp->generation;
	parent->device.release = crystalhd_v4l2_release;
	strscpy(parent->device.name, adp->name, sizeof(parent->device.name));
	rc = v4l2_device_register(&adp->pdev->dev, &parent->device);
	if (rc) {
		kfree(parent);
		return rc;
	}
	adp->v4l2 = parent;
	return 0;
}

void crystalhd_v4l2_unregister(struct crystalhd_adp *adp)
{
	struct crystalhd_v4l2 *parent;

	if (!adp || !adp->v4l2)
		return;
	parent = adp->v4l2;
	adp->v4l2 = NULL;
	WRITE_ONCE(parent->disconnected, true);
	v4l2_device_unregister(&parent->device);
	v4l2_device_put(&parent->device);
}
