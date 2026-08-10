// SPDX-License-Identifier: GPL-2.0-only

#include <linux/atomic.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/poll.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/version.h>
#include <linux/vmalloc.h>
#include <linux/wait.h>

#include "mqsim_ipc.h"
#include "mqsim_ipc_protocol.h"

static bool mqsim_ipc_enable;
static unsigned int mqsim_ipc_timeout_us = 1000;

module_param(mqsim_ipc_enable, bool, 0644);
MODULE_PARM_DESC(mqsim_ipc_enable, "Enable NVMeVirt -> MQSim userspace timing IPC");
module_param(mqsim_ipc_timeout_us, uint, 0644);
MODULE_PARM_DESC(mqsim_ipc_timeout_us, "Timeout for one MQSim timing IPC request in usec");

static struct nvmev_mqsim_shm *mqsim_shm;
static struct proc_dir_entry *mqsim_proc_entry;
static bool mqsim_daemon_connected;

static DEFINE_MUTEX(mqsim_request_lock);
static DECLARE_WAIT_QUEUE_HEAD(mqsim_req_wq);
static DECLARE_WAIT_QUEUE_HEAD(mqsim_reply_wq);
static atomic64_t mqsim_next_request_id = ATOMIC64_INIT(1);
static atomic64_t mqsim_requests = ATOMIC64_INIT(0);
static atomic64_t mqsim_replies = ATOMIC64_INIT(0);
static atomic64_t mqsim_timeouts = ATOMIC64_INIT(0);
static atomic64_t mqsim_fallbacks = ATOMIC64_INIT(0);
static atomic64_t mqsim_send_errors = ATOMIC64_INIT(0);
static atomic64_t mqsim_req_ring_full = ATOMIC64_INIT(0);
static atomic64_t mqsim_resp_ring_empty = ATOMIC64_INIT(0);

static u64 mqsim_pending_request_id;
static u64 mqsim_pending_latency_ns;
static int mqsim_pending_status;
static bool mqsim_pending_done;
static int mqsim_last_error;

bool nvmev_mqsim_ipc_enabled(void)
{
	return mqsim_ipc_enable;
}

static inline bool ring_full(const struct nvmev_mqsim_ring_header *ring)
{
	u32 head = READ_ONCE(ring->head);
	u32 tail = READ_ONCE(ring->tail);

	return tail - head >= READ_ONCE(ring->size);
}

static inline bool ring_empty(const struct nvmev_mqsim_ring_header *ring)
{
	return READ_ONCE(ring->head) == READ_ONCE(ring->tail);
}

static int mqsim_push_request(const struct nvmev_mqsim_io_msg *msg)
{
	struct nvmev_mqsim_ring_header *ring = &mqsim_shm->req_ring;
	u32 tail;

	if (!mqsim_daemon_connected)
		return -ENOTCONN;

	if (ring_full(ring)) {
		atomic64_inc(&mqsim_req_ring_full);
		WRITE_ONCE(ring->dropped, READ_ONCE(ring->dropped) + 1);
		return -ENOSPC;
	}

	tail = READ_ONCE(ring->tail);
	memcpy(&mqsim_shm->req_entries[tail & NVMEV_MQSIM_RING_MASK], msg, sizeof(*msg));
	smp_wmb();
	WRITE_ONCE(ring->tail, tail + 1);
	wake_up_interruptible(&mqsim_req_wq);
	return 0;
}

static void mqsim_consume_responses(void)
{
	struct nvmev_mqsim_ring_header *ring = &mqsim_shm->resp_ring;

	while (!ring_empty(ring)) {
		struct nvmev_mqsim_io_msg msg;
		u32 head = READ_ONCE(ring->head);

		smp_rmb();
		memcpy(&msg, &mqsim_shm->resp_entries[head & NVMEV_MQSIM_RING_MASK], sizeof(msg));
		WRITE_ONCE(ring->head, head + 1);

		if (msg.magic != NVMEV_MQSIM_MAGIC || msg.version != NVMEV_MQSIM_VERSION ||
		    msg.type != NVMEV_MQSIM_MSG_IO_REPLY)
			continue;

		if (msg.request_id != mqsim_pending_request_id)
			continue;

		mqsim_pending_latency_ns = msg.latency_ns;
		mqsim_pending_status = msg.status;
		mqsim_pending_done = true;
		atomic64_inc(&mqsim_replies);
		wake_up(&mqsim_reply_wq);
	}
}

static int mqsim_dev_open(struct inode *inode, struct file *file)
{
	if (mqsim_daemon_connected)
		return -EBUSY;

	mqsim_daemon_connected = true;
	NVMEV_INFO("MQSim shared-memory IPC daemon connected\n");
	return 0;
}

static int mqsim_dev_release(struct inode *inode, struct file *file)
{
	mqsim_daemon_connected = false;
	mqsim_pending_done = true;
	mqsim_pending_status = -ENOTCONN;
	wake_up(&mqsim_reply_wq);
	NVMEV_INFO("MQSim shared-memory IPC daemon disconnected\n");
	return 0;
}

static int mqsim_dev_mmap(struct file *file, struct vm_area_struct *vma)
{
	unsigned long size = vma->vm_end - vma->vm_start;

	if (size > PAGE_ALIGN(sizeof(*mqsim_shm)))
		return -EINVAL;

	return remap_vmalloc_range(vma, mqsim_shm, 0);
}

static __poll_t mqsim_dev_poll(struct file *file, poll_table *wait)
{
	__poll_t mask = 0;

	poll_wait(file, &mqsim_req_wq, wait);
	if (!ring_empty(&mqsim_shm->req_ring))
		mask |= EPOLLIN | EPOLLRDNORM;
	return mask;
}

static long mqsim_dev_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	switch (cmd) {
	case NVMEV_MQSIM_IOCTL_NOTIFY_RESP:
		mqsim_consume_responses();
		return 0;
	default:
		return -ENOTTY;
	}
}

static const struct file_operations mqsim_dev_fops = {
	.owner = THIS_MODULE,
	.open = mqsim_dev_open,
	.release = mqsim_dev_release,
	.mmap = mqsim_dev_mmap,
	.poll = mqsim_dev_poll,
	.unlocked_ioctl = mqsim_dev_ioctl,
};

static struct miscdevice mqsim_miscdev = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = NVMEV_MQSIM_DEVICE_NAME,
	.fops = &mqsim_dev_fops,
	.mode = 0600,
};

static int nvmev_mqsim_proc_read(struct seq_file *m, void *data)
{
	seq_printf(m, "enabled: %u\n", mqsim_ipc_enable ? 1 : 0);
	seq_printf(m, "transport: shared_memory\n");
	seq_printf(m, "timeout_us: %u\n", mqsim_ipc_timeout_us);
	seq_printf(m, "daemon_connected: %u\n", mqsim_daemon_connected ? 1 : 0);
	seq_printf(m, "ring_size: %u\n", NVMEV_MQSIM_RING_SIZE);
	seq_printf(m, "req_head: %u\n", READ_ONCE(mqsim_shm->req_ring.head));
	seq_printf(m, "req_tail: %u\n", READ_ONCE(mqsim_shm->req_ring.tail));
	seq_printf(m, "resp_head: %u\n", READ_ONCE(mqsim_shm->resp_ring.head));
	seq_printf(m, "resp_tail: %u\n", READ_ONCE(mqsim_shm->resp_ring.tail));
	seq_printf(m, "requests: %lld\n", atomic64_read(&mqsim_requests));
	seq_printf(m, "replies: %lld\n", atomic64_read(&mqsim_replies));
	seq_printf(m, "timeouts: %lld\n", atomic64_read(&mqsim_timeouts));
	seq_printf(m, "fallbacks: %lld\n", atomic64_read(&mqsim_fallbacks));
	seq_printf(m, "send_errors: %lld\n", atomic64_read(&mqsim_send_errors));
	seq_printf(m, "req_ring_full: %lld\n", atomic64_read(&mqsim_req_ring_full));
	seq_printf(m, "resp_ring_empty: %lld\n", atomic64_read(&mqsim_resp_ring_empty));
	seq_printf(m, "last_error: %d\n", mqsim_last_error);
	return 0;
}

static int nvmev_mqsim_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, nvmev_mqsim_proc_read, NULL);
}

#if LINUX_VERSION_CODE > KERNEL_VERSION(5, 0, 0)
static const struct proc_ops mqsim_proc_fops = {
	.proc_open = nvmev_mqsim_proc_open,
	.proc_read = seq_read,
	.proc_lseek = seq_lseek,
	.proc_release = single_release,
};
#else
static const struct file_operations mqsim_proc_fops = {
	.open = nvmev_mqsim_proc_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};
#endif

static void nvmev_mqsim_shm_init(void)
{
	memset(mqsim_shm, 0, sizeof(*mqsim_shm));
	mqsim_shm->magic = NVMEV_MQSIM_MAGIC;
	mqsim_shm->version = NVMEV_MQSIM_VERSION;
	mqsim_shm->ring_size = NVMEV_MQSIM_RING_SIZE;
	mqsim_shm->req_ring.size = NVMEV_MQSIM_RING_SIZE;
	mqsim_shm->resp_ring.size = NVMEV_MQSIM_RING_SIZE;
}

int nvmev_mqsim_ipc_init(void)
{
	int ret;

	mqsim_shm = vmalloc_user(sizeof(*mqsim_shm));
	if (!mqsim_shm)
		return -ENOMEM;
	nvmev_mqsim_shm_init();

	ret = misc_register(&mqsim_miscdev);
	if (ret) {
		vfree(mqsim_shm);
		mqsim_shm = NULL;
		return ret;
	}

	NVMEV_INFO("MQSim shared-memory IPC initialized at /dev/%s, enabled=%d\n",
		   NVMEV_MQSIM_DEVICE_NAME, mqsim_ipc_enable);

	if (nvmev_vdev && nvmev_vdev->proc_root)
		mqsim_proc_entry = proc_create("mqsim_ipc", 0444, nvmev_vdev->proc_root,
					       &mqsim_proc_fops);

	return 0;
}

void nvmev_mqsim_ipc_exit(void)
{
	if (mqsim_proc_entry) {
		remove_proc_entry("mqsim_ipc", nvmev_vdev->proc_root);
		mqsim_proc_entry = NULL;
	}

	misc_deregister(&mqsim_miscdev);
	mqsim_daemon_connected = false;

	if (mqsim_shm) {
		vfree(mqsim_shm);
		mqsim_shm = NULL;
	}
}

int nvmev_mqsim_query_latency(struct nvmev_request *req, u64 *latency_ns)
{
	struct nvme_rw_command *rw = &req->cmd->rw;
	struct nvmev_mqsim_io_msg msg = { 0 };
	long timeout_jiffies;
	long wait_ret;
	int ret;

	if (!mqsim_ipc_enable)
		return -EOPNOTSUPP;

	if (rw->opcode != nvme_cmd_read && rw->opcode != nvme_cmd_write)
		return -EOPNOTSUPP;

	mutex_lock(&mqsim_request_lock);
	atomic64_inc(&mqsim_requests);

	mqsim_pending_request_id = atomic64_inc_return(&mqsim_next_request_id);
	mqsim_pending_latency_ns = 0;
	mqsim_pending_status = 0;
	mqsim_pending_done = false;

	msg.magic = NVMEV_MQSIM_MAGIC;
	msg.version = NVMEV_MQSIM_VERSION;
	msg.type = NVMEV_MQSIM_MSG_IO_REQUEST;
	msg.request_id = mqsim_pending_request_id;
	msg.submit_time_ns = req->nsecs_start;
	msg.opcode = rw->opcode == nvme_cmd_read ? NVMEV_MQSIM_IO_READ : NVMEV_MQSIM_IO_WRITE;
	msg.nsid = rw->nsid;
	msg.sqid = req->sq_id;
	msg.command_id = rw->command_id;
	msg.slba = rw->slba;
	msg.nlb = rw->length + 1;

	ret = mqsim_push_request(&msg);
	if (ret < 0) {
		atomic64_inc(&mqsim_send_errors);
		atomic64_inc(&mqsim_fallbacks);
		mqsim_last_error = ret;
		goto out_unlock;
	}

	timeout_jiffies = usecs_to_jiffies(mqsim_ipc_timeout_us);
	if (timeout_jiffies <= 0)
		timeout_jiffies = 1;

	wait_ret = wait_event_timeout(mqsim_reply_wq, mqsim_pending_done, timeout_jiffies);
	if (wait_ret == 0) {
		ret = -ETIMEDOUT;
		atomic64_inc(&mqsim_timeouts);
		atomic64_inc(&mqsim_fallbacks);
		mqsim_last_error = ret;
		goto out_unlock;
	}

	if (mqsim_pending_status < 0) {
		ret = mqsim_pending_status;
		atomic64_inc(&mqsim_fallbacks);
		mqsim_last_error = ret;
		goto out_unlock;
	}

	*latency_ns = mqsim_pending_latency_ns;
	ret = 0;

out_unlock:
	mutex_unlock(&mqsim_request_lock);
	return ret;
}
