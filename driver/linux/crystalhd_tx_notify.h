/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef CRYSTALHD_TX_NOTIFY_H
#define CRYSTALHD_TX_NOTIFY_H

struct crystalhd_hw;
enum crystalhd_tx_notify_event {
	CHD_TX_OPEN = 1, CHD_TX_ADVERTISEMENT, CHD_TX_DESCRIPTOR,
	CHD_TX_PREPARE, CHD_TX_START, CHD_TX_IRQ, CHD_TX_DONE
};
#define CHD_TX_NOTIFY_CAPACITY 8192U
#define CHD_TX_NOTIFY_VALUES 12U

#ifdef CRYSTALHD_TX_NOTIFY_RESEARCH
int crystalhd_tx_notify_init(void);
void crystalhd_tx_notify_cleanup(void);
void crystalhd_tx_notify(struct crystalhd_hw *hw, uint32_t event,
	const uint32_t *values, uint32_t count);
#define CHD_TX_NOTE(hw, event, ...) crystalhd_tx_notify((hw), (event), \
	(const uint32_t[]){__VA_ARGS__}, \
	sizeof((const uint32_t[]){__VA_ARGS__}) / sizeof(uint32_t))
#else
/* Unevaluated operands keep the default build free of observer loads/calls. */
#define CHD_TX_NOTE(hw, event, ...) do { \
	(void)sizeof(hw); (void)sizeof(event); \
	(void)sizeof((const uint32_t[]){__VA_ARGS__}); \
} while (0)
#endif
#endif
