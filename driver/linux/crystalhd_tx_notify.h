/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef CRYSTALHD_TX_NOTIFY_H
#define CRYSTALHD_TX_NOTIFY_H

struct crystalhd_hw;
enum crystalhd_tx_notify_event {
	CHD_TX_OPEN = 1, CHD_TX_ADVERTISEMENT, CHD_TX_DESCRIPTOR,
	CHD_TX_PREPARE, CHD_TX_START, CHD_TX_IRQ, CHD_TX_DONE,
	CHD_RX_START, CHD_RX_PICQ, CHD_RX_META, CHD_RX_RESERVE,
	CHD_RX_DOORBELL, CHD_RX_FIRE_RETURN, CHD_RX_IRQ_RAW, CHD_RX_IRQ,
	CHD_RX_COMPLETE, CHD_RX_READY, CHD_RX_PARSE, CHD_RX_DISPOSITION,
	CHD_RX_STATUS
};
#ifdef CRYSTALHD_RX_NOTIFY_RESEARCH
#define CHD_TX_NOTIFY_CAPACITY 32768U
#define CHD_TX_NOTIFY_LAST CHD_RX_STATUS
#else
#define CHD_TX_NOTIFY_CAPACITY 8192U
#define CHD_TX_NOTIFY_LAST CHD_TX_DONE
#endif
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
#ifdef CRYSTALHD_RX_NOTIFY_RESEARCH
#define CHD_RX_NOTE(hw, event, ...) CHD_TX_NOTE(hw, event, __VA_ARGS__)
#else
#define CHD_RX_NOTE(hw, event, ...) do { \
	(void)sizeof(hw); (void)sizeof(event); \
	(void)sizeof((const uint32_t[]){__VA_ARGS__}); \
} while (0)
#endif
#endif
