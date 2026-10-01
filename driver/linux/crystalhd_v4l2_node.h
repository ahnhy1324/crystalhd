/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef _CRYSTALHD_V4L2_NODE_H_
#define _CRYSTALHD_V4L2_NODE_H_

struct device;
struct v4l2_device;
struct crystalhd_v4l2;
struct crystalhd_v4l2_ctx;
struct crystalhd_v4l2_node;

void crystalhd_v4l2_parent_get(struct crystalhd_v4l2 *parent);
void crystalhd_v4l2_parent_put(struct crystalhd_v4l2 *parent);
void crystalhd_v4l2_ctx_bind(struct crystalhd_v4l2_ctx *ctx,
			    void *private, void (*release)(void *private));
int crystalhd_v4l2_node_register(struct crystalhd_v4l2 *parent,
				struct v4l2_device *device, struct device *dma_dev,
				u16 chip, u64 generation,
				struct crystalhd_v4l2_node **out);
void crystalhd_v4l2_node_unregister(struct crystalhd_v4l2_node *node);
void crystalhd_v4l2_node_destroy(struct crystalhd_v4l2_node *node);

#endif
