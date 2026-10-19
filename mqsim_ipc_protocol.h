/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef NVMEVIRT_MQSIM_IPC_PROTOCOL_H
#define NVMEVIRT_MQSIM_IPC_PROTOCOL_H

#ifdef __KERNEL__
#include <linux/ioctl.h>
#include <linux/types.h>
#else
#include <linux/ioctl.h>
#include <linux/types.h>
#endif

#define NVMEV_MQSIM_MAGIC 0x4e4d5153u /* "NMQS" */
#define NVMEV_MQSIM_VERSION 3u
#define NVMEV_MQSIM_DEVICE_NAME "nvmev-mqsim-ipc"
#define NVMEV_MQSIM_RING_SIZE 1024u
#define NVMEV_MQSIM_RING_MASK (NVMEV_MQSIM_RING_SIZE - 1u)
#define NVMEV_MQSIM_VUC_RING_SIZE 64u
#define NVMEV_MQSIM_VUC_RING_MASK (NVMEV_MQSIM_VUC_RING_SIZE - 1u)
#define NVMEV_MQSIM_VUC_SLOT_COUNT 16u
#define NVMEV_MQSIM_VUC_SLOT_BYTES (64u * 1024u)
#define NVMEV_MQSIM_IOCTL_NOTIFY_RESP _IO('N', 0x51)

enum nvmev_mqsim_msg_type {
	NVMEV_MQSIM_MSG_HELLO = 1,
	NVMEV_MQSIM_MSG_IO_REQUEST = 2,
	NVMEV_MQSIM_MSG_IO_REPLY = 3,
	NVMEV_MQSIM_MSG_VUC_REQUEST = 4,
	NVMEV_MQSIM_MSG_VUC_REPLY = 5,
};

enum nvmev_mqsim_io_opcode {
	NVMEV_MQSIM_IO_READ = 1,
	NVMEV_MQSIM_IO_WRITE = 2,
};

#define NVMEV_MQSIM_VUC_FETCH 0xC0u
#define NVMEV_MQSIM_VUC_EXPAND 0xC1u
#define NVMEV_MQSIM_VUC_SUCCESS 0u
#define NVMEV_MQSIM_VUC_INVALID_BUFFER 1u
#define NVMEV_MQSIM_VUC_INVALID_ARGUMENT 3u
#define NVMEV_MQSIM_VUC_RESULT_OVERFLOW 4u
#define NVMEV_MQSIM_VUC_RESOURCE_BUSY 5u

struct nvmev_mqsim_io_msg {
	__u32 magic;
	__u16 version;
	__u16 type;

	__u64 request_id;
	__u64 submit_time_ns;
	__u64 kernel_submit_wall_ns;

	__u32 opcode;
	__u32 nsid;
	__u32 sqid;
	__u32 command_id;

	__u64 slba;
	__u32 nlb;
	__u32 reserved;

	/* Reply-only fields. latency_ns is relative to submit_time_ns. */
	__u64 latency_ns;
	__s32 status;
	__u32 flags;
	/* VUC entries reference one bounded shared payload slot. */
	__u32 vuc_opcode;
	__u32 vuc_slot;
	__u32 payload_bytes;
	__u32 response_bytes;
	__u64 query_id;
	__u64 batch_id;
} __attribute__((packed));

struct nvmev_mqsim_vuc_slot {
	__u8 payload[NVMEV_MQSIM_VUC_SLOT_BYTES];
};

struct nvmev_mqsim_ring_header {
	__u32 size;
	__u32 head;
	__u32 tail;
	__u32 dropped;
};

struct nvmev_mqsim_shm {
	__u32 magic;
	__u32 version;
	__u32 ring_size;
	__u32 reserved;

	struct nvmev_mqsim_ring_header req_ring;
	struct nvmev_mqsim_ring_header resp_ring;
	struct nvmev_mqsim_ring_header vuc_req_ring;
	struct nvmev_mqsim_ring_header vuc_resp_ring;

	struct nvmev_mqsim_io_msg req_entries[NVMEV_MQSIM_RING_SIZE];
	struct nvmev_mqsim_io_msg resp_entries[NVMEV_MQSIM_RING_SIZE];
	struct nvmev_mqsim_io_msg vuc_req_entries[NVMEV_MQSIM_VUC_RING_SIZE];
	struct nvmev_mqsim_io_msg vuc_resp_entries[NVMEV_MQSIM_VUC_RING_SIZE];
	struct nvmev_mqsim_vuc_slot vuc_slots[NVMEV_MQSIM_VUC_SLOT_COUNT];
};

#endif /* NVMEVIRT_MQSIM_IPC_PROTOCOL_H */
