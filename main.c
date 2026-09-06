// SPDX-License-Identifier: GPL-2.0-only

#include <linux/kernel.h>
#include <linux/kthread.h>
#include <linux/types.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/uaccess.h>
#include <linux/version.h>
#include <linux/cpumask.h>
#include <linux/err.h>

#ifdef CONFIG_X86
#include <asm/e820/types.h>
#include <asm/e820/api.h>
#endif

#include "nvmev.h"
#include "conv_ftl.h"
#include "zns_ftl.h"
#include "simple_ftl.h"
#include "kv_ftl.h"
#include "dma.h"
#include "mqsim_ipc.h"
#include "path_stats.h"

/****************************************************************
 * Memory Layout
 ****************************************************************
 * virtDev
 *  - PCI header
 *    -> BAR at 1MiB area
 *  - PCI capability descriptors
 *
 * +--- memmap_start
 * |
 * v
 * +--------------+------------------------------------------+
 * | <---1MiB---> | <---------- Storage Area --------------> |
 * +--------------+------------------------------------------+
 *
 * 1MiB area for metadata
 *  - BAR : 1 page
 *	- DBS : 1 page
 *	- MSI-x table: 16 bytes/entry * 32
 *
 * Storage area
 *
 * NVMeVirt stores device data in a reserved physical DRAM range instead of
 * a regular file. The first 1 MiB is used for virtual PCI/NVMe metadata,
 * and the rest is exposed as the virtual SSD backing store.
 *
 ****************************************************************/

/****************************************************************
 * Argument
 ****************************************************************
 * 1. Memmap start
 * 2. Memmap size
 *
 * This file also defines the module parameters passed at insmod time.
 * memmap_start and memmap_size must match the physical memory range reserved
 * by the kernel command line.
 ****************************************************************/

/*
 * Global virtual device handle.
 *
 * Other NVMeVirt components use this pointer to access shared state such as
 * queues, namespaces, BARs, and the storage mapping.
 */
struct nvmev_dev *nvmev_vdev = NULL;
static bool storage_mapped_with_ioremap;

/*
 * Reserved memory range passed at insmod time.
 *
 * These values are physical addresses/sizes. NVMEV_STORAGE_INIT() maps the
 * range into kernel virtual address space with memremap().
 */
static unsigned long memmap_start = 0;
static unsigned long memmap_size = 0;

/*
 * Read/write latency knobs for NVMeVirt's local timing model.
 *
 * When MQSim IPC is enabled and the daemon replies successfully, io.c uses
 * the daemon-provided latency to override the local completion target.
 */
static unsigned int read_time = 1;
static unsigned int read_delay = 1;
static unsigned int read_trailing = 0;

static unsigned int write_time = 1;
static unsigned int write_delay = 1;
static unsigned int write_trailing = 0;

/*
 * Parallel resources for NVMeVirt's local performance model.
 *
 * These are simplified bandwidth/parallelism knobs and should not be treated
 * as NAND LUNs or dies.
 */
static unsigned int nr_io_units = 8;
static unsigned int io_unit_shift = 12;

/*
 * CPU binding for NVMeVirt kernel threads.
 *
 * The first CPU is used for the dispatcher, and the remaining CPUs are used
 * for I/O workers. Experiments often pair this with isolcpus to reduce
 * scheduler noise.
 */
static char *cpus;
static unsigned int debug = 0;

/*
 * Selects the data movement path.
 *
 * false uses memcpy between host PRP buffers and the backing store; true tries
 * to use the ioat DMA engine.
 */
int io_using_dma = false;

/*
 * Custom parser for memory-sized module parameters.
 *
 * memparse() accepts strings such as "64G" or "128M".
 */
static int set_parse_mem_param(const char *val, const struct kernel_param *kp)
{
	unsigned long *arg = (unsigned long *)kp->arg;
	*arg = memparse(val, NULL);
	return 0;
}

/*
 * Hooks the custom parser into module_param_cb().
 *
 * .set converts the user-provided string to an unsigned long, and .get exposes
 * the current value through sysfs parameter files.
 */
static struct kernel_param_ops ops_parse_mem_param = {
	.set = set_parse_mem_param,
	.get = param_get_ulong,
};

/*
 * Module parameters configurable through insmod/modprobe.
 *
 * Example:
 *   sudo insmod nvmev.ko memmap_start=128G memmap_size=64G cpus=7,8
 *
 * 0444/0644 are sysfs permissions. 0644 parameters can be updated after module
 * load; 0444 parameters are read-only after load.
 */
module_param_cb(memmap_start, &ops_parse_mem_param, &memmap_start, 0444);
MODULE_PARM_DESC(memmap_start, "Reserved memory address");
module_param_cb(memmap_size, &ops_parse_mem_param, &memmap_size, 0444);
MODULE_PARM_DESC(memmap_size, "Reserved memory size");
module_param(read_time, uint, 0644);
MODULE_PARM_DESC(read_time, "Read time in nanoseconds");
module_param(read_delay, uint, 0644);
MODULE_PARM_DESC(read_delay, "Read delay in nanoseconds");
module_param(read_trailing, uint, 0644);
MODULE_PARM_DESC(read_trailing, "Read trailing in nanoseconds");
module_param(write_time, uint, 0644);
MODULE_PARM_DESC(write_time, "Write time in nanoseconds");
module_param(write_delay, uint, 0644);
MODULE_PARM_DESC(write_delay, "Write delay in nanoseconds");
module_param(write_trailing, uint, 0644);
MODULE_PARM_DESC(write_trailing, "Write trailing in nanoseconds");
module_param(nr_io_units, uint, 0444);
MODULE_PARM_DESC(nr_io_units, "Number of I/O units that operate in parallel");
module_param(io_unit_shift, uint, 0444);
MODULE_PARM_DESC(io_unit_shift, "Size of each I/O unit (2^)");
module_param(cpus, charp, 0444);
MODULE_PARM_DESC(cpus, "CPU list for process, completion(int.) threads, Seperated by Comma(,)");
module_param(debug, uint, 0644);

/*
 * Polls NVMe doorbell registers and dispatches newly submitted commands.
 *
 * Host submissions advance SQ tail doorbells, and host completion processing
 * advances CQ head doorbells. This function detects those changes and routes
 * them to the admin or I/O queue handlers.
 *
 * Returns true if any queue event was processed.
 */
static bool nvmev_proc_dbs(void)
{
	int qid;
	int dbs_idx;
	int new_db;
	int old_db;
	bool updated = false;

	/*
	 * Admin submission queue doorbell.
	 *
	 * Queue 0 handles control commands such as identify, create queue, and
	 * set features; it is not used for normal data I/O.
	 */
	new_db = nvmev_vdev->dbs[0];
	if (new_db != nvmev_vdev->old_dbs[0]) {
		nvmev_proc_admin_sq(new_db, nvmev_vdev->old_dbs[0]);
		nvmev_vdev->old_dbs[0] = new_db;
		updated = true;
	}

	/*
	 * Admin completion queue doorbell.
	 *
	 * The host advances the CQ head after consuming admin completions, and
	 * NVMeVirt records that progress here.
	 */
	new_db = nvmev_vdev->dbs[1];
	if (new_db != nvmev_vdev->old_dbs[1]) {
		nvmev_proc_admin_cq(new_db, nvmev_vdev->old_dbs[1]);
		nvmev_vdev->old_dbs[1] = new_db;
		updated = true;
	}

	/*
	 * I/O submission queues.
	 *
	 * Queues with qid >= 1 are data-path queues. When an SQ tail changes,
	 * nvmev_proc_io_sq() parses new commands, asks the namespace timing model,
	 * and enqueues work for the I/O workers.
	 */
	for (qid = 1; qid <= nvmev_vdev->nr_sq; qid++) {
		if (nvmev_vdev->sqes[qid] == NULL)
			continue;
		dbs_idx = qid * 2;
		new_db = nvmev_vdev->dbs[dbs_idx];
		old_db = nvmev_vdev->old_dbs[dbs_idx];
		if (new_db != old_db) {
			nvmev_vdev->old_dbs[dbs_idx] = nvmev_proc_io_sq(qid, new_db, old_db);
			updated = true;
		}
	}

	/*
	 * I/O completion queues.
	 *
	 * The host advances CQ heads after consuming CQEs. NVMeVirt uses this path
	 * mainly to maintain in-flight queue statistics.
	 */
	for (qid = 1; qid <= nvmev_vdev->nr_cq; qid++) {
		if (nvmev_vdev->cqes[qid] == NULL)
			continue;
		dbs_idx = qid * 2 + 1;
		new_db = nvmev_vdev->dbs[dbs_idx];
		old_db = nvmev_vdev->old_dbs[dbs_idx];
		if (new_db != old_db) {
			nvmev_proc_io_cq(qid, new_db, old_db);
			nvmev_vdev->old_dbs[dbs_idx] = new_db;
			updated = true;
		}
	}

	return updated;
}

/*
 * Main loop of the dispatcher kernel thread.
 *
 * The dispatcher polls host-visible state: BAR accesses and doorbell updates.
 * It does not move payload data or fill I/O completions directly; it acts as
 * the front-end event pump for the admin and I/O paths.
 */
static int nvmev_dispatcher(void *data)
{
	static unsigned long last_dispatched_time = 0;

	NVMEV_INFO("nvmev_dispatcher started on cpu %d (node %d)\n",
		   nvmev_vdev->config.cpu_nr_dispatcher,
		   cpu_to_node(nvmev_vdev->config.cpu_nr_dispatcher));

	while (!kthread_should_stop()) {
		if (nvmev_proc_bars())
			last_dispatched_time = jiffies;
		if (nvmev_proc_dbs())
			last_dispatched_time = jiffies;

		if (CONFIG_NVMEVIRT_IDLE_TIMEOUT != 0 &&
		    time_after(jiffies, last_dispatched_time + (CONFIG_NVMEVIRT_IDLE_TIMEOUT * HZ)))
			schedule_timeout_interruptible(1);
		else
			cond_resched();
	}

	return 0;
}

/*
 * Creates and starts the dispatcher thread.
 *
 * If CPU binding is configured, the dispatcher is bound to the first CPU in
 * the cpus parameter to reduce scheduling noise.
 */
static int NVMEV_DISPATCHER_INIT(struct nvmev_dev *nvmev_vdev)
{
	struct task_struct *task;

	task = kthread_create(nvmev_dispatcher, NULL, "nvmev_dispatcher");
	if (IS_ERR(task)) {
		int ret = PTR_ERR(task);

		nvmev_vdev->nvmev_dispatcher = NULL;
		NVMEV_ERROR("Failed to create dispatcher thread: %d\n", ret);
		return ret;
	}

	nvmev_vdev->nvmev_dispatcher = task;
	if (nvmev_vdev->config.cpu_nr_dispatcher != -1)
		kthread_bind(task, nvmev_vdev->config.cpu_nr_dispatcher);
	wake_up_process(task);
	return 0;
}

/*
 * Stops the dispatcher thread during unload or error cleanup.
 */
static void NVMEV_DISPATCHER_FINAL(struct nvmev_dev *nvmev_vdev)
{
	if (!IS_ERR_OR_NULL(nvmev_vdev->nvmev_dispatcher)) {
		kthread_stop(nvmev_vdev->nvmev_dispatcher);
		nvmev_vdev->nvmev_dispatcher = NULL;
	}
}

#ifdef CONFIG_X86
/*
 * x86-specific reserved memory validation.
 *
 * NVMeVirt requires the configured physical range to be E820_TYPE_RESERVED so
 * Linux will not allocate it as normal RAM. A failure here usually means the
 * boot-time memmap setting and insmod parameters do not match.
 */
static int __validate_configs_arch(void)
{
	unsigned long resv_start_bytes;
	unsigned long resv_end_bytes;

	resv_start_bytes = memmap_start;
	resv_end_bytes = resv_start_bytes + memmap_size - 1;

	if (e820__mapped_any(resv_start_bytes, resv_end_bytes, E820_TYPE_RAM)) {
		NVMEV_ERROR("[mem %#010lx-%#010lx] is usable, not reseved region\n",
			    (unsigned long)resv_start_bytes, (unsigned long)resv_end_bytes);
		return -EPERM;
	}

	// check whether the kernel supports E820_TYPE_RESERVED_KERN first
	// https://lore.kernel.org/all/20250214090651.3331663-5-rppt@kernel.org/
#ifdef E820_TYPE_RESERVED_KERN
	if (e820__mapped_any(resv_start_bytes, resv_end_bytes, E820_TYPE_RESERVED_KERN)) {
		NVMEV_ERROR("[mem %#010lx-%#010lx] is reserved kernel region\n",
			    (unsigned long)resv_start_bytes, (unsigned long)resv_end_bytes);
		return -EPERM;
	}
#endif

	if (!e820__mapped_any(resv_start_bytes, resv_end_bytes, E820_TYPE_RESERVED)) {
		NVMEV_ERROR("[mem %#010lx-%#010lx] is not reseved region\n",
			    (unsigned long)resv_start_bytes, (unsigned long)resv_end_bytes);
		return -EPERM;
	}
	return 0;
}
#else
/*
 * Non-x86 reserved memory validation is not implemented yet.
 */
static int __validate_configs_arch(void)
{
	/* TODO: Validate architecture-specific configurations */
	return 0;
}
#endif

/*
 * Validates module parameters before device initialization.
 *
 * This checks the reserved memory range, I/O unit configuration, and local
 * read/write timing knobs. It does not map the backing store yet.
 */
static int __validate_configs(void)
{
	if (!memmap_start) {
		NVMEV_ERROR("[memmap_start] should be specified\n");
		return -EINVAL;
	}

	if (!memmap_size) {
		NVMEV_ERROR("[memmap_size] should be specified\n");
		return -EINVAL;
	} else if (memmap_size <= MB(1)) {
		NVMEV_ERROR("[memmap_size] should be bigger than 1 MiB\n");
		return -EINVAL;
	}

	if (__validate_configs_arch()) {
		return -EPERM;
	}

	if (nr_io_units == 0 || io_unit_shift == 0) {
		NVMEV_ERROR("Need non-zero IO unit size and at least one IO unit\n");
		return -EINVAL;
	}
	if (read_time == 0) {
		NVMEV_ERROR("Need non-zero read time\n");
		return -EINVAL;
	}
	if (write_time == 0) {
		NVMEV_ERROR("Need non-zero write time\n");
		return -EINVAL;
	}
	if (!cpus) {
		NVMEV_ERROR("[cpus] should specify one dispatcher CPU and at least one I/O worker CPU\n");
		return -EINVAL;
	}

	return 0;
}

/*
 * Prints local performance model settings when CONFIG_NVMEV_VERBOSE is set.
 *
 * These values still matter as fallback timing when MQSim IPC is disabled or
 * unavailable.
 */
static void __print_perf_configs(void)
{
#ifdef CONFIG_NVMEV_VERBOSE
	unsigned long unit_perf_kb =
			nvmev_vdev->config.nr_io_units << (nvmev_vdev->config.io_unit_shift - 10);
	struct nvmev_config *cfg = &nvmev_vdev->config;

	NVMEV_INFO("=============== Configurations ===============\n");
	NVMEV_INFO("* IO units : %d x %d\n",
			cfg->nr_io_units, 1 << cfg->io_unit_shift);
	NVMEV_INFO("* I/O times\n");
	NVMEV_INFO("  Read     : %u + %u x + %u ns\n",
				cfg->read_delay, cfg->read_time, cfg->read_trailing);
	NVMEV_INFO("  Write    : %u + %u x + %u ns\n",
				cfg->write_delay, cfg->write_time, cfg->write_trailing);
	NVMEV_INFO("* Bandwidth\n");
	NVMEV_INFO("  Read     : %lu MiB/s\n",
			(1000000000UL / (cfg->read_time + cfg->read_delay + cfg->read_trailing)) * unit_perf_kb >> 10);
	NVMEV_INFO("  Write    : %lu MiB/s\n",
			(1000000000UL / (cfg->write_time + cfg->write_delay + cfg->write_trailing)) * unit_perf_kb >> 10);
#endif
}

/*
 * Computes the number of pending entries for a doorbell index.
 *
 * Queue doorbells are ring-buffer positions, so wraparound must be handled.
 */
static int __get_nr_entries(int dbs_idx, int queue_size)
{
	int diff = nvmev_vdev->dbs[dbs_idx] - nvmev_vdev->old_dbs[dbs_idx];
	if (diff < 0) {
		diff += queue_size;
	}
	return diff;
}

/*
 * Read handler for procfs control/status files under /proc/nvmev/.
 *
 * Exposes local timing settings, I/O unit configuration, and queue statistics.
 */
static int __proc_file_read(struct seq_file *m, void *data)
{
	const char *filename = m->private;
	struct nvmev_config *cfg = &nvmev_vdev->config;

	if (strcmp(filename, "read_times") == 0) {
		seq_printf(m, "%u + %u x + %u", cfg->read_delay, cfg->read_time,
			   cfg->read_trailing);
	} else if (strcmp(filename, "write_times") == 0) {
		seq_printf(m, "%u + %u x + %u", cfg->write_delay, cfg->write_time,
			   cfg->write_trailing);
	} else if (strcmp(filename, "io_units") == 0) {
		seq_printf(m, "%u x %u", cfg->nr_io_units, cfg->io_unit_shift);
	} else if (strcmp(filename, "stat") == 0) {
		int i;
		unsigned int nr_in_flight = 0;
		unsigned int nr_dispatch = 0;
		unsigned int nr_dispatched = 0;
		unsigned long long total_io = 0;
		for (i = 1; i <= nvmev_vdev->nr_sq; i++) {
			struct nvmev_submission_queue *sq = nvmev_vdev->sqes[i];
			if (!sq)
				continue;

			seq_printf(m, "%2d: %2u %4u %4u %4u %4u %llu\n", i,
				   __get_nr_entries(i * 2, sq->queue_size), sq->stat.nr_in_flight,
				   sq->stat.max_nr_in_flight, sq->stat.nr_dispatch,
				   sq->stat.nr_dispatched, sq->stat.total_io);

			nr_in_flight += sq->stat.nr_in_flight;
			nr_dispatch += sq->stat.nr_dispatch;
			nr_dispatched += sq->stat.nr_dispatched;
			total_io += sq->stat.total_io;

			barrier();
			sq->stat.max_nr_in_flight = 0;
		}
		seq_printf(m, "total: %u %u %u %llu\n", nr_in_flight, nr_dispatch, nr_dispatched,
			   total_io);
	} else if (strcmp(filename, "debug") == 0) {
		/* Left for later use */
	}

	return 0;
}

/*
 * Write handler for procfs control files under /proc/nvmev/.
 *
 * Users can update selected local timing knobs at runtime. When MQSim IPC is
 * enabled, daemon-provided timing overrides these values on the I/O path.
 */
static ssize_t __proc_file_write(struct file *file, const char __user *buf, size_t len,
				 loff_t *offp)
{
	ssize_t count = len;
	const char *filename = file->f_path.dentry->d_name.name;
	char input[128];
	unsigned int ret;
	unsigned long long *old_stat;
	struct nvmev_config *cfg = &nvmev_vdev->config;
	size_t nr_copied;

	nr_copied = copy_from_user(input, buf, min(len, sizeof(input)));

	if (!strcmp(filename, "read_times")) {
		ret = sscanf(input, "%u %u %u", &cfg->read_delay, &cfg->read_time,
			     &cfg->read_trailing);
		//adjust_ftl_latency(0, cfg->read_time);
	} else if (!strcmp(filename, "write_times")) {
		ret = sscanf(input, "%u %u %u", &cfg->write_delay, &cfg->write_time,
			     &cfg->write_trailing);
		//adjust_ftl_latency(1, cfg->write_time);
	} else if (!strcmp(filename, "io_units")) {
		ret = sscanf(input, "%d %d", &cfg->nr_io_units, &cfg->io_unit_shift);
		if (ret < 1)
			goto out;

		old_stat = nvmev_vdev->io_unit_stat;
		nvmev_vdev->io_unit_stat =
			kzalloc(sizeof(*nvmev_vdev->io_unit_stat) * cfg->nr_io_units, GFP_KERNEL);

		mdelay(100); /* XXX: Delay the free of old stat so that outstanding
						 * requests accessing the unit_stat are all returned
						 */
		kfree(old_stat);
	} else if (!strcmp(filename, "stat")) {
		int i;
		for (i = 1; i <= nvmev_vdev->nr_sq; i++) {
			struct nvmev_submission_queue *sq = nvmev_vdev->sqes[i];
			if (!sq)
				continue;

			memset(&sq->stat, 0x00, sizeof(sq->stat));
		}
	} else if (!strcmp(filename, "debug")) {
		/* Left for later use */
	}

out:
	__print_perf_configs();

	return count;
}

/*
 * procfs open handler shared by all NVMeVirt procfs files.
 */
static int __proc_file_open(struct inode *inode, struct file *file)
{
	return single_open(file, __proc_file_read, (char *)file->f_path.dentry->d_name.name);
}

/*
 * procfs file operations.
 *
 * Linux 5.0+ uses struct proc_ops, while older kernels use
 * struct file_operations.
 */
#if LINUX_VERSION_CODE > KERNEL_VERSION(5, 0, 0)
static const struct proc_ops proc_file_fops = {
	.proc_open = __proc_file_open,
	.proc_write = __proc_file_write,
	.proc_read = seq_read,
	.proc_lseek = seq_lseek,
	.proc_release = single_release,
};
#else
static const struct file_operations proc_file_fops = {
	.open = __proc_file_open,
	.write = __proc_file_write,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};
#endif

static void NVMEV_STORAGE_FINAL(struct nvmev_dev *nvmev_vdev);

/*
 * Initializes the backing store and procfs control interface.
 *
 * memremap() maps the reserved physical DRAM range into kernel virtual address
 * space. Namespaces later point into this mapped range as their backing store.
 */
static int NVMEV_STORAGE_INIT(struct nvmev_dev *nvmev_vdev)
{
	NVMEV_INFO("Storage: %#010lx-%#010lx (%lu MiB)\n",
			nvmev_vdev->config.storage_start,
			nvmev_vdev->config.storage_start + nvmev_vdev->config.storage_size,
			BYTE_TO_MB(nvmev_vdev->config.storage_size));

	nvmev_vdev->io_unit_stat = kzalloc(
		sizeof(*nvmev_vdev->io_unit_stat) * nvmev_vdev->config.nr_io_units, GFP_KERNEL);
	if (!nvmev_vdev->io_unit_stat)
		return -ENOMEM;

	nvmev_vdev->storage_mapped = memremap(nvmev_vdev->config.storage_start,
					      nvmev_vdev->config.storage_size, MEMREMAP_WB);
	if (!nvmev_vdev->storage_mapped) {
		NVMEV_ERROR("Failed to map storage memory as write-back; retrying write-through\n");
		nvmev_vdev->storage_mapped = memremap(nvmev_vdev->config.storage_start,
						      nvmev_vdev->config.storage_size, MEMREMAP_WT);
	}
	if (!nvmev_vdev->storage_mapped) {
		NVMEV_ERROR("Failed to map storage memory as write-through; retrying write-combine\n");
		nvmev_vdev->storage_mapped = memremap(nvmev_vdev->config.storage_start,
						      nvmev_vdev->config.storage_size, MEMREMAP_WC);
	}
	if (!nvmev_vdev->storage_mapped) {
		NVMEV_ERROR("Failed to map storage memory as write-combine; retrying uncached ioremap\n");
		nvmev_vdev->storage_mapped = ioremap(nvmev_vdev->config.storage_start,
						     nvmev_vdev->config.storage_size);
		storage_mapped_with_ioremap = nvmev_vdev->storage_mapped != NULL;
	}

	if (nvmev_vdev->storage_mapped == NULL) {
		NVMEV_ERROR("Failed to map storage memory.\n");
		kfree(nvmev_vdev->io_unit_stat);
		nvmev_vdev->io_unit_stat = NULL;
		return -ENOMEM;
	}

	nvmev_vdev->proc_root = proc_mkdir("nvmev", NULL);
	if (!nvmev_vdev->proc_root) {
		NVMEV_ERROR("Failed to create /proc/nvmev\n");
		NVMEV_STORAGE_FINAL(nvmev_vdev);
		return -ENOMEM;
	}

	nvmev_vdev->proc_read_times =
		proc_create("read_times", 0664, nvmev_vdev->proc_root, &proc_file_fops);
	nvmev_vdev->proc_write_times =
		proc_create("write_times", 0664, nvmev_vdev->proc_root, &proc_file_fops);
	nvmev_vdev->proc_io_units =
		proc_create("io_units", 0664, nvmev_vdev->proc_root, &proc_file_fops);
	nvmev_vdev->proc_stat = proc_create("stat", 0444, nvmev_vdev->proc_root, &proc_file_fops);
	nvmev_vdev->proc_debug = proc_create("debug", 0444, nvmev_vdev->proc_root, &proc_file_fops);
	if (!nvmev_vdev->proc_read_times || !nvmev_vdev->proc_write_times ||
	    !nvmev_vdev->proc_io_units || !nvmev_vdev->proc_stat ||
	    !nvmev_vdev->proc_debug) {
		NVMEV_ERROR("Failed to create one or more /proc/nvmev entries\n");
		NVMEV_STORAGE_FINAL(nvmev_vdev);
		return -ENOMEM;
	}
	if (nvmev_path_stats_init(nvmev_vdev->proc_root, nvmev_vdev->config.nr_io_workers)) {
		NVMEV_ERROR("Failed to create /proc/nvmev/path_stats\n");
		NVMEV_STORAGE_FINAL(nvmev_vdev);
		return -ENOMEM;
	}

	return 0;
}

/*
 * Releases storage-related resources.
 *
 * This removes procfs files and unmaps the kernel virtual mapping created by
 * memremap(). It does not erase the reserved physical memory contents.
 */
static void NVMEV_STORAGE_FINAL(struct nvmev_dev *nvmev_vdev)
{
	if (!nvmev_vdev)
		return;

	if (nvmev_vdev->proc_root) {
		nvmev_path_stats_exit(nvmev_vdev->proc_root);
		if (nvmev_vdev->proc_read_times) {
			remove_proc_entry("read_times", nvmev_vdev->proc_root);
			nvmev_vdev->proc_read_times = NULL;
		}
		if (nvmev_vdev->proc_write_times) {
			remove_proc_entry("write_times", nvmev_vdev->proc_root);
			nvmev_vdev->proc_write_times = NULL;
		}
		if (nvmev_vdev->proc_io_units) {
			remove_proc_entry("io_units", nvmev_vdev->proc_root);
			nvmev_vdev->proc_io_units = NULL;
		}
		if (nvmev_vdev->proc_stat) {
			remove_proc_entry("stat", nvmev_vdev->proc_root);
			nvmev_vdev->proc_stat = NULL;
		}
		if (nvmev_vdev->proc_debug) {
			remove_proc_entry("debug", nvmev_vdev->proc_root);
			nvmev_vdev->proc_debug = NULL;
		}
		remove_proc_entry("nvmev", NULL);
		nvmev_vdev->proc_root = NULL;
	}

	if (nvmev_vdev->storage_mapped) {
		if (storage_mapped_with_ioremap)
			iounmap(nvmev_vdev->storage_mapped);
		else
			memunmap(nvmev_vdev->storage_mapped);
	}
	nvmev_vdev->storage_mapped = NULL;
	storage_mapped_with_ioremap = false;

	if (nvmev_vdev->io_unit_stat) {
		kfree(nvmev_vdev->io_unit_stat);
		nvmev_vdev->io_unit_stat = NULL;
	}
}

/*
 * Loads module parameters into nvmev_config.
 *
 * The first 1 MiB of the reserved range is kept for virtual device metadata;
 * the remaining bytes become namespace backing storage. The cpus string is
 * also split here into one dispatcher CPU and one or more I/O worker CPUs.
 */
static bool __load_configs(struct nvmev_config *config)
{
	bool first = true;
	unsigned int cpu_nr;
	char *cpu;

	if (__validate_configs() < 0) {
		return false;
	}

#if (BASE_SSD == KV_PROTOTYPE)
	memmap_size -= KV_MAPPING_TABLE_SIZE; // Reserve space for KV mapping table
#endif

	config->memmap_start = memmap_start;
	config->memmap_size = memmap_size;
	// storage space starts from 1M offset
	config->storage_start = memmap_start + MB(1);
	config->storage_size = memmap_size - MB(1);

	config->read_time = read_time;
	config->read_delay = read_delay;
	config->read_trailing = read_trailing;
	config->write_time = write_time;
	config->write_delay = write_delay;
	config->write_trailing = write_trailing;
	config->nr_io_units = nr_io_units;
	config->io_unit_shift = io_unit_shift;

	config->nr_io_workers = 0;
	config->cpu_nr_dispatcher = -1;

	while ((cpu = strsep(&cpus, ",")) != NULL) {
		int parse_ret;

		if (!*cpu) {
			NVMEV_ERROR("[cpus] contains an empty CPU entry\n");
			return false;
		}

		parse_ret = kstrtouint(cpu, 10, &cpu_nr);
		if (parse_ret) {
			NVMEV_ERROR("[cpus] invalid CPU entry: %s\n", cpu);
			return false;
		}
		if (cpu_nr >= nr_cpu_ids || !cpu_possible(cpu_nr)) {
			NVMEV_ERROR("[cpus] CPU %u is not possible on this system\n", cpu_nr);
			return false;
		}

		if (first) {
			config->cpu_nr_dispatcher = cpu_nr;
		} else {
			if (config->nr_io_workers >= ARRAY_SIZE(config->cpu_nr_io_workers)) {
				NVMEV_ERROR("[cpus] too many I/O worker CPUs; max=%zu\n",
					    ARRAY_SIZE(config->cpu_nr_io_workers));
				return false;
			}
			config->cpu_nr_io_workers[config->nr_io_workers] = cpu_nr;
			config->nr_io_workers++;
		}
		first = false;
	}

	if (config->cpu_nr_dispatcher == -1 || config->nr_io_workers == 0) {
		NVMEV_ERROR("[cpus] should specify one dispatcher CPU and at least one I/O worker CPU\n");
		return false;
	}

	return true;
}

/*
 * Creates NVMe namespaces and attaches each namespace to backing storage.
 *
 * NVMeVirt supports several namespace/device models: simple NVM, conventional
 * SSD, ZNS, and KV SSD. Each init_namespace() implementation sets up
 * ns[i].mapped, ns[i].size, and the namespace-specific proc_io_cmd() callback.
 */
static int NVMEV_NAMESPACE_INIT(struct nvmev_dev *nvmev_vdev)
{
	unsigned long long remaining_capacity = nvmev_vdev->config.storage_size;
	void *ns_addr = nvmev_vdev->storage_mapped;
	const int nr_ns = NR_NAMESPACES; // XXX: allow for dynamic nr_ns
	const unsigned int disp_no = nvmev_vdev->config.cpu_nr_dispatcher;
	int i;
	unsigned long long size;

	struct nvmev_ns *ns = kmalloc(sizeof(struct nvmev_ns) * nr_ns, GFP_KERNEL);
	if (!ns)
		return -ENOMEM;

	for (i = 0; i < nr_ns; i++) {
		if (NS_CAPACITY(i) == 0)
			size = remaining_capacity;
		else
			size = min(NS_CAPACITY(i), remaining_capacity);

		if (NS_SSD_TYPE(i) == SSD_TYPE_NVM)
			simple_init_namespace(&ns[i], i, size, ns_addr, disp_no);
		else if (NS_SSD_TYPE(i) == SSD_TYPE_CONV)
			conv_init_namespace(&ns[i], i, size, ns_addr, disp_no);
		else if (NS_SSD_TYPE(i) == SSD_TYPE_ZNS)
			zns_init_namespace(&ns[i], i, size, ns_addr, disp_no);
		else if (NS_SSD_TYPE(i) == SSD_TYPE_KV)
			kv_init_namespace(&ns[i], i, size, ns_addr, disp_no);
		else
			BUG_ON(1);

		remaining_capacity -= size;
		ns_addr += size;
		NVMEV_INFO("ns %d/%d: size %lld MiB\n", i, nr_ns, BYTE_TO_MB(ns[i].size));
	}

	nvmev_vdev->ns = ns;
	nvmev_vdev->nr_ns = nr_ns;
	nvmev_vdev->mdts = MDTS;
	return 0;
}

/*
 * Removes namespaces and releases namespace-specific model state.
 */
static void NVMEV_NAMESPACE_FINAL(struct nvmev_dev *nvmev_vdev)
{
	struct nvmev_ns *ns = nvmev_vdev->ns;
	const int nr_ns = NR_NAMESPACES; // XXX: allow for dynamic nvmev_vdev->nr_ns
	int i;

	if (!ns)
		return;

	for (i = 0; i < nr_ns; i++) {
		if (NS_SSD_TYPE(i) == SSD_TYPE_NVM)
			simple_remove_namespace(&ns[i]);
		else if (NS_SSD_TYPE(i) == SSD_TYPE_CONV)
			conv_remove_namespace(&ns[i]);
		else if (NS_SSD_TYPE(i) == SSD_TYPE_ZNS)
			zns_remove_namespace(&ns[i]);
		else if (NS_SSD_TYPE(i) == SSD_TYPE_KV)
			kv_remove_namespace(&ns[i]);
		else
			BUG_ON(1);
	}

	kfree(ns);
	nvmev_vdev->ns = NULL;
	nvmev_vdev->nr_ns = 0;
}

/*
 * Prints the base SSD type selected by Kbuild.
 *
 * Kbuild enables one CONFIG_NVMEVIRT_* target at a time and uses BASE_SSD to
 * choose the device model compiled into this module.
 */
static void __print_base_config(void)
{
	const char *type = "unknown";
	switch (BASE_SSD) {
	case INTEL_OPTANE:
		type = "NVM SSD";
		break;
	case SAMSUNG_970PRO:
		type = "Samsung 970 Pro SSD";
		break;
	case ZNS_PROTOTYPE:
		type = "ZNS SSD Prototype";
		break;
	case KV_PROTOTYPE:
		type = "KVSSD Prototype";
		break;
	case WD_ZN540:
		type = "WD ZN540 ZNS SSD";
		break;
	}

	NVMEV_INFO("Version %x.%x for >> %s <<\n",
			(NVMEV_VERSION & 0xff00) >> 8, (NVMEV_VERSION & 0x00ff), type);
}

static void NVMEV_PCI_FINAL(struct nvmev_dev *nvmev_vdev)
{
	if (nvmev_vdev && nvmev_vdev->virt_bus != NULL) {
		pci_stop_root_bus(nvmev_vdev->virt_bus);
		pci_remove_root_bus(nvmev_vdev->virt_bus);
		nvmev_vdev->virt_bus = NULL;
	}
}

/*
 * Kernel module load entry point.
 *
 * The high-level initialization order is:
 * 1. Print the selected base SSD type.
 * 2. Allocate the nvmev_dev object.
 * 3. Parse and validate module parameters.
 * 4. Map reserved memory and initialize backing storage.
 * 5. Create namespaces.
 * 6. Initialize the MQSim IPC netlink endpoint.
 * 7. Initialize the DMA engine if requested.
 * 8. Create the virtual PCI/NVMe device.
 * 9. Start I/O workers and the dispatcher.
 * 10. Publish the virtual PCI bus/device to the Linux PCI core.
 */
static int NVMeV_init(void)
{
	int ret = 0;

	__print_base_config();

	nvmev_vdev = VDEV_INIT();
	if (!nvmev_vdev)
		return -EINVAL;

	if (!__load_configs(&nvmev_vdev->config)) {
		ret = -EINVAL;
		goto err_vdev;
	}

	NVMEV_INFO("NVMeVirt-MQSim: mqsim_ipc_enable=%d memmap_start=%#lx memmap_size=%#lx\n",
		   nvmev_mqsim_ipc_enabled() ? 1 : 0,
		   nvmev_vdev->config.memmap_start, nvmev_vdev->config.memmap_size);

	ret = NVMEV_STORAGE_INIT(nvmev_vdev);
	if (ret)
		goto err_vdev;

	ret = NVMEV_NAMESPACE_INIT(nvmev_vdev);
	if (ret)
		goto err_storage;

	ret = nvmev_mqsim_ipc_init();
	if (ret)
		goto err_namespace;

	if (io_using_dma) {
		if (ioat_dma_chan_set("dma7chan0") != 0) {
			io_using_dma = false;
			NVMEV_ERROR("Cannot use DMA engine, Fall back to memcpy\n");
		}
	}

	if (!NVMEV_PCI_INIT(nvmev_vdev)) {
		ret = -EIO;
		goto err_pci;
	}

	__print_perf_configs();

	ret = NVMEV_IO_WORKER_INIT(nvmev_vdev);
	if (ret)
		goto err_pci;

	ret = NVMEV_DISPATCHER_INIT(nvmev_vdev);
	if (ret)
		goto err_workers;

	pci_bus_add_devices(nvmev_vdev->virt_bus);

	NVMEV_INFO("Virtual NVMe device created\n");

	return 0;

err_workers:
	NVMEV_IO_WORKER_FINAL(nvmev_vdev);
err_pci:
	NVMEV_PCI_FINAL(nvmev_vdev);
	nvmev_mqsim_ipc_exit();
err_namespace:
	NVMEV_NAMESPACE_FINAL(nvmev_vdev);
err_storage:
	NVMEV_STORAGE_FINAL(nvmev_vdev);
err_vdev:
	VDEV_FINALIZE(nvmev_vdev);
	nvmev_vdev = NULL;
	return ret;
}

/*
 * Kernel module unload entry point.
 *
 * The cleanup order is roughly the reverse of initialization: remove the
 * virtual PCI device, stop kernel threads, remove namespaces and IPC, release
 * storage mappings, and finally free queue/device structures.
 */
static void NVMeV_exit(void)
{
	int i;

	NVMEV_PCI_FINAL(nvmev_vdev);

	NVMEV_DISPATCHER_FINAL(nvmev_vdev);

	nvmev_mqsim_ipc_exit();
	NVMEV_IO_WORKER_FINAL(nvmev_vdev);
	NVMEV_NAMESPACE_FINAL(nvmev_vdev);
	NVMEV_STORAGE_FINAL(nvmev_vdev);

	if (io_using_dma) {
		ioat_dma_cleanup();
	}

	for (i = 0; i < nvmev_vdev->nr_sq; i++) {
		kfree(nvmev_vdev->sqes[i]);
	}

	for (i = 0; i < nvmev_vdev->nr_cq; i++) {
		kfree(nvmev_vdev->cqes[i]);
	}

	VDEV_FINALIZE(nvmev_vdev);

	NVMEV_INFO("Virtual NVMe device closed\n");
}

/*
 * Linux kernel module metadata.
 *
 * module_init/module_exit register load/unload entry points. MODULE_LICENSE()
 * affects exported-symbol access and kernel taint state.
 */
MODULE_LICENSE("GPL v2");
module_init(NVMeV_init);
module_exit(NVMeV_exit);
