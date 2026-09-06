/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef NVMEVIRT_CONTROLLER_TIMING_H
#define NVMEVIRT_CONTROLLER_TIMING_H

#include <linux/seq_file.h>
#include <linux/types.h>

bool nvmev_ctrl_timing_enabled(void);
u64 nvmev_ctrl_timing_frontend_done(u64 arrival_ns, u32 sqid);
u64 nvmev_ctrl_timing_completion_done(u64 backend_done_ns, u32 cqid);
void nvmev_ctrl_timing_proc_print(struct seq_file *m);

#endif /* NVMEVIRT_CONTROLLER_TIMING_H */
