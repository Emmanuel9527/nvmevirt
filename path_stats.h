/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef NVMEVIRT_PATH_STATS_H
#define NVMEVIRT_PATH_STATS_H

#include <linux/proc_fs.h>
#include <linux/types.h>

int nvmev_path_stats_init(struct proc_dir_entry *proc_root, unsigned int nr_workers);
void nvmev_path_stats_exit(struct proc_dir_entry *proc_root);

void nvmev_path_stats_record_sq_batch(u32 sqid, u32 submitted, u32 accepted);
void nvmev_path_stats_record_worker_enqueue(u32 worker_id);
void nvmev_path_stats_record_worker_scan(u32 worker_id, u32 queued, u32 active);
void nvmev_path_stats_record_perform_io(u32 worker_id, u64 ns);
void nvmev_path_stats_record_fill_cq(u32 worker_id, u64 ns, u64 late_ns);
void nvmev_path_stats_record_irq(u32 worker_id, u64 ns);

#endif /* NVMEVIRT_PATH_STATS_H */
