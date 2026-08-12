// SPDX-License-Identifier: GPL-2.0-only

#include <linux/atomic.h>
#include <linux/fs.h>
#include <linux/hashtable.h>
#include <linux/miscdevice.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/poll.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
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
MODULE_PARM_DESC(mqsim_ipc_timeout_us, "Legacy MQSim IPC timeout parameter; async mode completes pending I/O on daemon disconnect");

static struct nvmev_mqsim_shm *mqsim_shm;
static struct proc_dir_entry *mqsim_proc_entry;
static bool mqsim_daemon_connected;
static bool mqsim_misc_registered;

static DEFINE_MUTEX(mqsim_request_lock);
static DECLARE_WAIT_QUEUE_HEAD(mqsim_req_wq);
static atomic64_t mqsim_next_request_id = ATOMIC64_INIT(1);
static atomic64_t mqsim_requests = ATOMIC64_INIT(0);
static atomic64_t mqsim_replies = ATOMIC64_INIT(0);
static atomic64_t mqsim_timeouts = ATOMIC64_INIT(0);
static atomic64_t mqsim_fallbacks = ATOMIC64_INIT(0);
static atomic64_t mqsim_send_errors = ATOMIC64_INIT(0);
static atomic64_t mqsim_req_ring_full = ATOMIC64_INIT(0);
static atomic64_t mqsim_resp_ring_empty = ATOMIC64_INIT(0);
static atomic64_t mqsim_late_replies = ATOMIC64_INIT(0);
static atomic64_t mqsim_pending_count = ATOMIC64_INIT(0);
static atomic64_t mqsim_max_pending = ATOMIC64_INIT(0);
static int mqsim_last_error;

struct mqsim_pending_io {
	struct hlist_node node;
	u64 request_id;
	u64 submit_time_ns;
	u64 fallback_target_ns;
	unsigned int worker_id;
	unsigned int work_entry;
};

static DEFINE_HASHTABLE(mqsim_pending_table, 10);
static DEFINE_SPINLOCK(mqsim_pending_lock);

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

static void mqsim_update_max_pending(s64 pending)
{
	s64 old;

	old = atomic64_read(&mqsim_max_pending);
	while (pending > old) {
		if (atomic64_cmpxchg(&mqsim_max_pending, old, pending) == old)
			break;
		old = atomic64_read(&mqsim_max_pending);
	}
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

		{
			struct mqsim_pending_io *pending;
			struct mqsim_pending_io *completed = NULL;
			u64 target_ns;

			spin_lock(&mqsim_pending_lock);
			hash_for_each_possible(mqsim_pending_table, pending, node, msg.request_id) {
				if (pending->request_id != msg.request_id)
					continue;

				hash_del(&pending->node);
				atomic64_dec(&mqsim_pending_count);
				completed = pending;
				break;
			}
			spin_unlock(&mqsim_pending_lock);

			if (!completed) {
				atomic64_inc(&mqsim_late_replies);
				continue;
			}

			if (msg.status < 0) {
				target_ns = completed->fallback_target_ns;
				atomic64_inc(&mqsim_fallbacks);
				mqsim_last_error = msg.status;
			} else {
				target_ns = completed->submit_time_ns + msg.latency_ns;
				atomic64_inc(&mqsim_replies);
			}

			nvmev_mqsim_complete_io(completed->worker_id, completed->work_entry,
						completed->request_id, target_ns);
			kfree(completed);
		}
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
	struct mqsim_pending_io *pending;
	struct hlist_node *tmp;
	int bucket;

	mqsim_daemon_connected = false;

	spin_lock(&mqsim_pending_lock);
	hash_for_each_safe(mqsim_pending_table, bucket, tmp, pending, node) {
		hash_del(&pending->node);
		atomic64_dec(&mqsim_pending_count);

		atomic64_inc(&mqsim_fallbacks);
		nvmev_mqsim_complete_io(pending->worker_id, pending->work_entry,
					pending->request_id, pending->fallback_target_ns);
		kfree(pending);
	}
	spin_unlock(&mqsim_pending_lock);

	mqsim_last_error = -ENOTCONN;
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
	seq_printf(m, "pending: %lld\n", atomic64_read(&mqsim_pending_count));
	seq_printf(m, "max_pending: %lld\n", atomic64_read(&mqsim_max_pending));
	seq_printf(m, "timeouts: %lld\n", atomic64_read(&mqsim_timeouts));
	seq_printf(m, "fallbacks: %lld\n", atomic64_read(&mqsim_fallbacks));
	seq_printf(m, "send_errors: %lld\n", atomic64_read(&mqsim_send_errors));
	seq_printf(m, "req_ring_full: %lld\n", atomic64_read(&mqsim_req_ring_full));
	seq_printf(m, "resp_ring_empty: %lld\n", atomic64_read(&mqsim_resp_ring_empty));
	seq_printf(m, "late_replies: %lld\n", atomic64_read(&mqsim_late_replies));
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
	hash_init(mqsim_pending_table);

	ret = misc_register(&mqsim_miscdev);
	if (ret) {
		vfree(mqsim_shm);
		mqsim_shm = NULL;
		return ret;
	}
	mqsim_misc_registered = true;

	NVMEV_INFO("MQSim shared-memory IPC initialized at /dev/%s, enabled=%d\n",
		   NVMEV_MQSIM_DEVICE_NAME, mqsim_ipc_enable);

	if (nvmev_vdev && nvmev_vdev->proc_root)
		mqsim_proc_entry = proc_create("mqsim_ipc", 0444, nvmev_vdev->proc_root,
					       &mqsim_proc_fops);

	return 0;
}

void nvmev_mqsim_ipc_exit(void)
{
	struct mqsim_pending_io *pending;
	struct hlist_node *tmp;
	int bucket;

	if (mqsim_proc_entry && nvmev_vdev && nvmev_vdev->proc_root) {
		remove_proc_entry("mqsim_ipc", nvmev_vdev->proc_root);
		mqsim_proc_entry = NULL;
	}

	if (mqsim_misc_registered) {
		misc_deregister(&mqsim_miscdev);
		mqsim_misc_registered = false;
	}
	mqsim_daemon_connected = false;

	spin_lock(&mqsim_pending_lock);
	hash_for_each_safe(mqsim_pending_table, bucket, tmp, pending, node) {
		hash_del(&pending->node);
		atomic64_dec(&mqsim_pending_count);
		nvmev_mqsim_complete_io(pending->worker_id, pending->work_entry,
					pending->request_id, pending->fallback_target_ns);
		kfree(pending);
	}
	spin_unlock(&mqsim_pending_lock);

	if (mqsim_shm) {
		vfree(mqsim_shm);
		mqsim_shm = NULL;
	}
}

int nvmev_mqsim_submit_async(struct nvmev_request *req, unsigned int worker_id,
			     unsigned int work_entry, u64 fallback_target_ns,
			     u64 *request_id)
{
	struct nvme_rw_command *rw = &req->cmd->rw;
	struct nvmev_mqsim_io_msg msg = { 0 };
	struct mqsim_pending_io *pending;
	s64 pending_now;
	int ret;

	if (!mqsim_ipc_enable)
		return -EOPNOTSUPP;

	if (rw->opcode != nvme_cmd_read && rw->opcode != nvme_cmd_write)
		return -EOPNOTSUPP;

	pending = kzalloc(sizeof(*pending), GFP_KERNEL);
	if (!pending)
		return -ENOMEM;

	mutex_lock(&mqsim_request_lock);

	pending->request_id = atomic64_inc_return(&mqsim_next_request_id);
	pending->submit_time_ns = req->nsecs_start;
	pending->fallback_target_ns = fallback_target_ns;
	pending->worker_id = worker_id;
	pending->work_entry = work_entry;
	if (request_id)
		*request_id = pending->request_id;

	msg.magic = NVMEV_MQSIM_MAGIC;
	msg.version = NVMEV_MQSIM_VERSION;
	msg.type = NVMEV_MQSIM_MSG_IO_REQUEST;
	msg.request_id = pending->request_id;
	msg.submit_time_ns = req->nsecs_start;
	msg.opcode = rw->opcode == nvme_cmd_read ? NVMEV_MQSIM_IO_READ : NVMEV_MQSIM_IO_WRITE;
	msg.nsid = rw->nsid;
	msg.sqid = req->sq_id;
	msg.command_id = rw->command_id;
	msg.slba = rw->slba;
	msg.nlb = rw->length + 1;

	nvmev_mqsim_bind_io(worker_id, work_entry, pending->request_id);
	spin_lock(&mqsim_pending_lock);
	hash_add(mqsim_pending_table, &pending->node, pending->request_id);
	pending_now = atomic64_inc_return(&mqsim_pending_count);
	mqsim_update_max_pending(pending_now);
	spin_unlock(&mqsim_pending_lock);

	ret = mqsim_push_request(&msg);
	if (ret < 0) {
		spin_lock(&mqsim_pending_lock);
		hash_del(&pending->node);
		atomic64_dec(&mqsim_pending_count);
		spin_unlock(&mqsim_pending_lock);
		atomic64_inc(&mqsim_send_errors);
		atomic64_inc(&mqsim_fallbacks);
		mqsim_last_error = ret;
		kfree(pending);
		goto out_unlock;
	}

	atomic64_inc(&mqsim_requests);
	ret = 0;

out_unlock:
	mutex_unlock(&mqsim_request_lock);
	return ret;
}
