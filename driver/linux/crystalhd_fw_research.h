/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef _CRYSTALHD_FW_RESEARCH_H_
#define _CRYSTALHD_FW_RESEARCH_H_

#ifdef CRYSTALHD_ENABLE_FW_RESEARCH
int crystalhd_fw_research_init(void);
void crystalhd_fw_research_cleanup(void);
/* Metadata only; returns no adapter reference and performs no hardware I/O. */
int crystalhd_fw_research_generation(u64 *generation);
#endif

#endif
