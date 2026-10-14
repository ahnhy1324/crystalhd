/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef _CRYSTALHD_FW_RESEARCH_H_
#define _CRYSTALHD_FW_RESEARCH_H_

#ifdef CRYSTALHD_ENABLE_FW_RESEARCH
int crystalhd_fw_research_init(void);
void crystalhd_fw_research_cleanup(void);
/* Metadata only; returns no adapter reference and performs no hardware I/O. */
int crystalhd_fw_research_generation(u64 *generation);
/* Called only after Flea has posted the exact command mailbox.  An unarmed
 * observer returns immediately; the ordinary IRQ/reply path remains the sole
 * command-completion authority.
 */
void crystalhd_fw_research_command_posted(struct crystalhd_hw *hw,
					  const BC_FW_CMD *fw_cmd);
#else
static inline void crystalhd_fw_research_command_posted(
	struct crystalhd_hw *hw, const BC_FW_CMD *fw_cmd)
{
	(void)hw;
	(void)fw_cmd;
}
#endif

#endif
