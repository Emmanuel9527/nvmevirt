// SPDX-License-Identifier: GPL-2.0-only

#include <linux/atomic.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/version.h>

#include "path_stats.h"

struct nvmev_path_worker_stats {
	atomic64_t enqueued;
	atomic64_t queue_scan_count;
	atomic64_t queue_len_total;
	atomic64_t queue_len_max;
	atomic64_t active_len_total;
	atomic64_t active_len_max;
	atomic64_t perform_io_count;
	atomic64_t perform_io_total_ns;
	atomic64_t perform_io_max_ns;
	atomic64_t fill_cq_count;
	atomic64_t fill_cq_total_ns;
	atomic64_t fill_cq_max_ns;
	atomic64_t late_count;
	atomic64_t late_total_ns;
	atomic64_t late_max_ns;
	atomic64_t irq_count;
	atomic64_t irq_total_ns;
	atomic64_t irq_max_ns;
};

static struct proc_dir_entry *path_stats_proc_entry;
static struct nvmev_path_worker_stats *worker_stats;
static unsigned int worker_stats_count;

static atomic64_t sq_batch_count = ATOMIC64_INIT(0);
static atomic64_t sq_batch_submitted_total = ATOMIC64_INIT(0);
static atomic64_t sq_batch_submitted_max = ATOMIC64_INIT(0);
static atomic64_t sq_batch_accepted_total = ATOMIC64_INIT(0);
static atomic64_t sq_batch_accepted_max = ATOMIC64_INIT(0);

static void update_max(atomic64_t *counter, s64 value)
{
	s64 old = atomic64_read(counter);

	while (value > old) {
		if (atomic64_cmpxchg(counter, old, value) == old)
			break;
		old = atomic64_read(counter);
	}
}

static s64 avg_or_zero(atomic64_t *total, s64 count)
{
	return count ? atomic64_read(total) / count : 0;
}

static struct nvmev_path_worker_stats *get_worker(u32 worker_id)
{
	if (!worker_stats || worker_id >= worker_stats_count)
		return NULL;
	return &worker_stats[worker_id];
}

void nvmev_path_stats_record_sq_batch(u32 sqid, u32 submitted, u32 accepted)
{
	(void)sqid;

	atomic64_inc(&sq_batch_count);
	atomic64_add(submitted, &sq_batch_submitted_total);
	update_max(&sq_batch_submitted_max, submitted);
	atomic64_add(accepted, &sq_batch_accepted_total);
	update_max(&sq_batch_accepted_max, accepted);
}

void nvmev_path_stats_record_worker_enqueue(u32 worker_id)
{
	struct nvmev_path_worker_stats *stats = get_worker(worker_id);

	if (!stats)
		return;
	atomic64_inc(&stats->enqueued);
}

void nvmev_path_stats_record_worker_scan(u32 worker_id, u32 queued, u32 active)
{
	struct nvmev_path_worker_stats *stats = get_worker(worker_id);

	if (!stats)
		return;
	atomic64_inc(&stats->queue_scan_count);
	atomic64_add(queued, &stats->queue_len_total);
	update_max(&stats->queue_len_max, queued);
	atomic64_add(active, &stats->active_len_total);
	update_max(&stats->active_len_max, active);
}

void nvmev_path_stats_record_perform_io(u32 worker_id, u64 ns)
{
	struct nvmev_path_worker_stats *stats = get_worker(worker_id);

	if (!stats)
		return;
	atomic64_inc(&stats->perform_io_count);
	atomic64_add(ns, &stats->perform_io_total_ns);
	update_max(&stats->perform_io_max_ns, ns);
}

void nvmev_path_stats_record_fill_cq(u32 worker_id, u64 ns, u64 late_ns)
{
	struct nvmev_path_worker_stats *stats = get_worker(worker_id);

	if (!stats)
		return;
	atomic64_inc(&stats->fill_cq_count);
	atomic64_add(ns, &stats->fill_cq_total_ns);
	update_max(&stats->fill_cq_max_ns, ns);
	if (late_ns > 0) {
		atomic64_inc(&stats->late_count);
		atomic64_add(late_ns, &stats->late_total_ns);
		update_max(&stats->late_max_ns, late_ns);
	}
}

void nvmev_path_stats_record_irq(u32 worker_id, u64 ns)
{
	struct nvmev_path_worker_stats *stats = get_worker(worker_id);

	if (!stats)
		return;
	atomic64_inc(&stats->irq_count);
	atomic64_add(ns, &stats->irq_total_ns);
	update_max(&stats->irq_max_ns, ns);
}

static int nvmev_path_stats_proc_read(struct seq_file *m, void *data)
{
	s64 batch_count = atomic64_read(&sq_batch_count);
	unsigned int i;

	seq_puts(m, "NVMeVirt path stats\n");
	seq_puts(m, "===================\n");
	seq_printf(m, "sq_batch_count: %lld\n", batch_count);
	seq_printf(m, "sq_batch_submitted_avg: %lld\n",
		   avg_or_zero(&sq_batch_submitted_total, batch_count));
	seq_printf(m, "sq_batch_submitted_max: %lld\n",
		   atomic64_read(&sq_batch_submitted_max));
	seq_printf(m, "sq_batch_accepted_avg: %lld\n",
		   avg_or_zero(&sq_batch_accepted_total, batch_count));
	seq_printf(m, "sq_batch_accepted_max: %lld\n",
		   atomic64_read(&sq_batch_accepted_max));
	seq_puts(m, "\n");

	seq_puts(m, "Per-worker CPU/path table:\n");
	seq_printf(m, "%8s %12s %12s %12s %12s %12s %16s %16s %16s %16s %14s %14s %12s %14s %14s\n",
		   "Worker", "Enqueued", "Scans", "AvgQLen", "MaxQLen",
		   "MaxActive", "PerformAvg(ns)", "PerformMax(ns)",
		   "FillCQAvg(ns)", "FillCQMax(ns)", "LateCount",
		   "LateAvg(ns)", "LateMax(ns)", "IRQAvg(ns)", "IRQMax(ns)");
	seq_puts(m, "================================================================================================================================================================================\n");

	for (i = 0; i < worker_stats_count; i++) {
		struct nvmev_path_worker_stats *stats = &worker_stats[i];
		s64 scans = atomic64_read(&stats->queue_scan_count);
		s64 perform_count = atomic64_read(&stats->perform_io_count);
		s64 fill_count = atomic64_read(&stats->fill_cq_count);
		s64 late_count = atomic64_read(&stats->late_count);
		s64 irq_count = atomic64_read(&stats->irq_count);

		seq_printf(m, "%8u %12lld %12lld %12lld %12lld %12lld %16lld %16lld %16lld %16lld %14lld %14lld %12lld %14lld %14lld\n",
			   i,
			   atomic64_read(&stats->enqueued),
			   scans,
			   avg_or_zero(&stats->queue_len_total, scans),
			   atomic64_read(&stats->queue_len_max),
			   atomic64_read(&stats->active_len_max),
			   avg_or_zero(&stats->perform_io_total_ns, perform_count),
			   atomic64_read(&stats->perform_io_max_ns),
			   avg_or_zero(&stats->fill_cq_total_ns, fill_count),
			   atomic64_read(&stats->fill_cq_max_ns),
			   late_count,
			   avg_or_zero(&stats->late_total_ns, late_count),
			   atomic64_read(&stats->late_max_ns),
			   avg_or_zero(&stats->irq_total_ns, irq_count),
			   atomic64_read(&stats->irq_max_ns));
	}

	return 0;
}

static int nvmev_path_stats_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, nvmev_path_stats_proc_read, NULL);
}

#if LINUX_VERSION_CODE > KERNEL_VERSION(5, 0, 0)
static const struct proc_ops path_stats_proc_fops = {
	.proc_open = nvmev_path_stats_proc_open,
	.proc_read = seq_read,
	.proc_lseek = seq_lseek,
	.proc_release = single_release,
};
#else
static const struct file_operations path_stats_proc_fops = {
	.open = nvmev_path_stats_proc_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};
#endif

int nvmev_path_stats_init(struct proc_dir_entry *proc_root, unsigned int nr_workers)
{
	worker_stats_count = nr_workers;
	worker_stats = kcalloc(worker_stats_count, sizeof(*worker_stats), GFP_KERNEL);
	if (!worker_stats)
		return -ENOMEM;

	path_stats_proc_entry = proc_create("path_stats", 0444, proc_root,
					    &path_stats_proc_fops);
	if (!path_stats_proc_entry) {
		kfree(worker_stats);
		worker_stats = NULL;
		worker_stats_count = 0;
		return -ENOMEM;
	}
	return 0;
}

void nvmev_path_stats_exit(struct proc_dir_entry *proc_root)
{
	if (path_stats_proc_entry && proc_root)
		remove_proc_entry("path_stats", proc_root);
	path_stats_proc_entry = NULL;
	kfree(worker_stats);
	worker_stats = NULL;
	worker_stats_count = 0;
}
