// SPDX-License-Identifier: GPL-2.0-or-later
#include <media/v4l2-device.h>
#include <linux/kref.h>
#include <linux/workqueue.h>

#include "crystalhd_lnx.h"
#include "crystalhd_v4l2_node.h"

struct crystalhd_v4l2 {
	struct v4l2_device device;
	u64 generation;
	bool disconnected;
	spinlock_t close_lock; /* Pending registry and context lifetime state. */
	struct list_head pending;
	struct crystalhd_v4l2_node *node;
};

enum crystalhd_v4l2_owner_state {
	CRYSTALHD_V4L2_OWNER_NEVER,
	CRYSTALHD_V4L2_OWNER_HELD,
	CRYSTALHD_V4L2_OWNER_RETIRED,
};

struct crystalhd_v4l2_ctx {
	struct kref ref;
	struct crystalhd_v4l2 *parent;
	u64 generation;
	struct mutex lock; /* Serializes acquire, close and worker admission. */
	struct work_struct close_work;
	struct list_head pending;
	/* All following state, and pending membership, use parent->close_lock. */
	enum crystalhd_v4l2_owner_state owner_state;
	bool core_reference_held;
	bool closing;
	bool work_active;
	bool kick_pending;
	bool cleanup_committed;
	void (*release_private)(void *private);
	void *private;
};

static struct workqueue_struct *crystalhd_v4l2_close_wq;

int crystalhd_v4l2_init(void)
{
	crystalhd_v4l2_close_wq = alloc_workqueue("crystalhd-close",
						  WQ_UNBOUND | WQ_MEM_RECLAIM, 0);
	return crystalhd_v4l2_close_wq ? 0 : -ENOMEM;
}

void crystalhd_v4l2_cleanup(void)
{
	/* Module exit calls this after PCI unregister, outside device barriers.
	 * Draining also fences a worker's last module_put through its return.
	 */
	if (crystalhd_v4l2_close_wq)
		destroy_workqueue(crystalhd_v4l2_close_wq);
	crystalhd_v4l2_close_wq = NULL;
}

static void crystalhd_v4l2_ctx_release(struct kref *ref)
{
	struct crystalhd_v4l2_ctx *ctx =
		container_of(ref, struct crystalhd_v4l2_ctx, ref);
	struct crystalhd_v4l2 *parent = ctx->parent;

	WARN_ON_ONCE(!ctx->cleanup_committed || ctx->core_reference_held ||
		     ctx->work_active || !list_empty(&ctx->pending));
	if (ctx->release_private)
		ctx->release_private(ctx->private);
	kfree(ctx);
	v4l2_device_put(&parent->device);
	module_put(THIS_MODULE);
}

static void crystalhd_v4l2_queue_close_locked(struct crystalhd_v4l2_ctx *ctx)
{
	if (!ctx->closing || ctx->cleanup_committed)
		return;
	ctx->kick_pending = true;
	if (ctx->work_active)
		return;
	ctx->work_active = true;
	kref_get(&ctx->ref);
	WARN_ON_ONCE(!queue_work(crystalhd_v4l2_close_wq, &ctx->close_work));
}

static void crystalhd_v4l2_owner_get(const void *owner)
{
	struct crystalhd_v4l2_ctx *ctx = (struct crystalhd_v4l2_ctx *)owner;
	unsigned long flags;

	spin_lock_irqsave(&ctx->parent->close_lock, flags);
	kref_get(&ctx->ref);
	ctx->core_reference_held = true;
	ctx->owner_state = CRYSTALHD_V4L2_OWNER_HELD;
	spin_unlock_irqrestore(&ctx->parent->close_lock, flags);
}

static void crystalhd_v4l2_owner_retired(const void *owner)
{
	struct crystalhd_v4l2_ctx *ctx = (struct crystalhd_v4l2_ctx *)owner;
	unsigned long flags;

	spin_lock_irqsave(&ctx->parent->close_lock, flags);
	ctx->owner_state = CRYSTALHD_V4L2_OWNER_RETIRED;
	crystalhd_v4l2_queue_close_locked(ctx);
	spin_unlock_irqrestore(&ctx->parent->close_lock, flags);
}

static void crystalhd_v4l2_owner_put(const void *owner)
{
	struct crystalhd_v4l2_ctx *ctx = (struct crystalhd_v4l2_ctx *)owner;
	unsigned long flags;

	spin_lock_irqsave(&ctx->parent->close_lock, flags);
	/* Before close the caller owns a base reference; afterward the pending
	 * registry retains one until this put finishes. Never destroy under the
	 * core's device/session barriers, even if retirement work already ran.
	 */
	kref_put(&ctx->ref, crystalhd_v4l2_ctx_release);
	ctx->core_reference_held = false;
	crystalhd_v4l2_queue_close_locked(ctx);
	spin_unlock_irqrestore(&ctx->parent->close_lock, flags);
}

static const struct crystalhd_session_owner_ops crystalhd_v4l2_owner_ops = {
	.get = crystalhd_v4l2_owner_get,
	.retired = crystalhd_v4l2_owner_retired,
	.put = crystalhd_v4l2_owner_put,
};

static bool crystalhd_v4l2_ctx_retired(struct crystalhd_v4l2_ctx *ctx)
{
	return !ctx->core_reference_held &&
		(ctx->owner_state == CRYSTALHD_V4L2_OWNER_NEVER ||
		 ctx->owner_state == CRYSTALHD_V4L2_OWNER_RETIRED);
}

static void crystalhd_v4l2_close_worker(struct work_struct *work)
{
	struct crystalhd_v4l2_ctx *ctx =
		container_of(work, struct crystalhd_v4l2_ctx, close_work);
	struct crystalhd_v4l2 *parent = ctx->parent;
	struct crystalhd_device_access access;
	unsigned long flags;
	bool try_release;

	for (;;) {
		mutex_lock(&ctx->lock);
		spin_lock_irqsave(&parent->close_lock, flags);
		ctx->kick_pending = false;
		try_release = !crystalhd_v4l2_ctx_retired(ctx) &&
			      !parent->disconnected;
		spin_unlock_irqrestore(&parent->close_lock, flags);

		if (try_release &&
		    !crystalhd_device_enter(ctx->generation, true, &access)) {
			if (access.adp->v4l2 == parent)
				crystalhd_session_release_locked(&access.adp->cmds, ctx);
			crystalhd_device_exit(&access);
		}
		mutex_unlock(&ctx->lock);

		spin_lock_irqsave(&parent->close_lock, flags);
		if (crystalhd_v4l2_ctx_retired(ctx)) {
			ctx->cleanup_committed = true;
			ctx->work_active = false;
			ctx->kick_pending = false;
			list_del_init(&ctx->pending);
			spin_unlock_irqrestore(&parent->close_lock, flags);
			/* Pending registry and worker references; no device/user or
			 * frontend mutex remains held during final destruction.
			 */
			kref_put(&ctx->ref, crystalhd_v4l2_ctx_release);
			kref_put(&ctx->ref, crystalhd_v4l2_ctx_release);
			return;
		}
		/* A resume or owner put racing the attempt must not be lost when
		 * queue_work observes this worker already active.
		 */
		if (ctx->kick_pending) {
			spin_unlock_irqrestore(&parent->close_lock, flags);
			continue;
		}
		ctx->work_active = false;
		spin_unlock_irqrestore(&parent->close_lock, flags);
		kref_put(&ctx->ref, crystalhd_v4l2_ctx_release);
		return;
	}
}

int crystalhd_v4l2_ctx_create(struct crystalhd_v4l2 *parent,
			      struct crystalhd_v4l2_ctx **out)
{
	struct crystalhd_v4l2_ctx *ctx;
	unsigned long flags;

	if (out)
		*out = NULL;
	if (!parent || !out)
		return -EINVAL;
	if (!try_module_get(THIS_MODULE))
		return -ENODEV;
	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx) {
		module_put(THIS_MODULE);
		return -ENOMEM;
	}
	spin_lock_irqsave(&parent->close_lock, flags);
	if (parent->disconnected) {
		spin_unlock_irqrestore(&parent->close_lock, flags);
		kfree(ctx);
		module_put(THIS_MODULE);
		return -ENODEV;
	}
	v4l2_device_get(&parent->device);
	spin_unlock_irqrestore(&parent->close_lock, flags);
	ctx->parent = parent;
	ctx->generation = parent->generation;
	kref_init(&ctx->ref);
	mutex_init(&ctx->lock);
	INIT_WORK(&ctx->close_work, crystalhd_v4l2_close_worker);
	INIT_LIST_HEAD(&ctx->pending);
	*out = ctx;
	return 0;
}

int crystalhd_v4l2_ctx_acquire(struct crystalhd_v4l2_ctx *ctx)
{
	struct crystalhd_device_access access;
	unsigned long flags;
	BC_STATUS sts;
	int rc;

	if (!ctx)
		return -EINVAL;
	mutex_lock(&ctx->lock);
	spin_lock_irqsave(&ctx->parent->close_lock, flags);
	rc = ctx->closing || ctx->parent->disconnected ? -ENODEV : 0;
	spin_unlock_irqrestore(&ctx->parent->close_lock, flags);
	if (rc)
		goto unlock;
	rc = crystalhd_device_try_enter_exclusive(ctx->generation, &access);
	if (rc)
		goto unlock;
	if (access.adp->v4l2 != ctx->parent) {
		rc = -ENODEV;
	} else {
		sts = crystalhd_session_acquire_ref_locked(&access.adp->cmds,
							   ctx, &crystalhd_v4l2_owner_ops);
		rc = crystalhd_status_to_errno(sts);
	}
	crystalhd_device_exit(&access);
unlock:
	mutex_unlock(&ctx->lock);
	return rc;
}

void crystalhd_v4l2_ctx_close(struct crystalhd_v4l2_ctx *ctx)
{
	unsigned long flags;

	if (!ctx)
		return;
	mutex_lock(&ctx->lock);
	spin_lock_irqsave(&ctx->parent->close_lock, flags);
	if (ctx->closing) {
		spin_unlock_irqrestore(&ctx->parent->close_lock, flags);
		mutex_unlock(&ctx->lock);
		return;
	}
	ctx->closing = true;
	kref_get(&ctx->ref);
	list_add_tail(&ctx->pending, &ctx->parent->pending);
	crystalhd_v4l2_queue_close_locked(ctx);
	spin_unlock_irqrestore(&ctx->parent->close_lock, flags);
	mutex_unlock(&ctx->lock);
	/* Keep the caller's base reference until mutex_unlock itself returns. */
	kref_put(&ctx->ref, crystalhd_v4l2_ctx_release);
}

void crystalhd_v4l2_resume_ready(struct crystalhd_adp *adp)
{
	struct crystalhd_v4l2 *parent = adp->v4l2;
	struct crystalhd_v4l2_ctx *ctx;
	unsigned long flags;

	if (!parent)
		return;
	spin_lock_irqsave(&parent->close_lock, flags);
	list_for_each_entry(ctx, &parent->pending, pending)
		crystalhd_v4l2_queue_close_locked(ctx);
	spin_unlock_irqrestore(&parent->close_lock, flags);
}

static void crystalhd_v4l2_release(struct v4l2_device *device)
{
	struct crystalhd_v4l2 *parent =
		container_of(device, struct crystalhd_v4l2, device);

	crystalhd_v4l2_node_destroy(parent->node);
	kfree(parent);
}

void crystalhd_v4l2_parent_get(struct crystalhd_v4l2 *parent)
{
	v4l2_device_get(&parent->device);
}

void crystalhd_v4l2_parent_put(struct crystalhd_v4l2 *parent)
{
	v4l2_device_put(&parent->device);
}

void crystalhd_v4l2_ctx_bind(struct crystalhd_v4l2_ctx *ctx,
			    void *private, void (*release)(void *private))
{
	/* The unpublished context's sole caller installs its deferred cleanup. */
	ctx->private = private;
	ctx->release_private = release;
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
	spin_lock_init(&parent->close_lock);
	INIT_LIST_HEAD(&parent->pending);
	parent->device.release = crystalhd_v4l2_release;
	strscpy(parent->device.name, adp->name, sizeof(parent->device.name));
	rc = v4l2_device_register(&adp->pdev->dev, &parent->device);
	if (rc) {
		kfree(parent);
		return rc;
	}
	adp->v4l2 = parent;
	rc = crystalhd_v4l2_node_register(parent, &parent->device,
					&adp->pdev->dev, adp->pdev->device,
					parent->generation, &parent->node);
	if (rc) {
		adp->v4l2 = NULL;
		v4l2_device_unregister(&parent->device);
		v4l2_device_put(&parent->device);
	}
	return rc;
}

void crystalhd_v4l2_unregister(struct crystalhd_adp *adp)
{
	struct crystalhd_v4l2 *parent;
	struct crystalhd_v4l2_ctx *ctx;
	unsigned long flags;

	if (!adp || !adp->v4l2)
		return;
	parent = adp->v4l2;
	adp->v4l2 = NULL;
	spin_lock_irqsave(&parent->close_lock, flags);
	parent->disconnected = true;
	list_for_each_entry(ctx, &parent->pending, pending)
		crystalhd_v4l2_queue_close_locked(ctx);
	spin_unlock_irqrestore(&parent->close_lock, flags);
	crystalhd_v4l2_node_unregister(parent->node);
	v4l2_device_unregister(&parent->device);
	v4l2_device_put(&parent->device);
}
