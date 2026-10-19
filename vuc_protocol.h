/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef NVMEV_VUC_PROTOCOL_H
#define NVMEV_VUC_PROTOCOL_H

#ifdef __KERNEL__
#include <linux/types.h>
typedef __u32 nvmev_u32;
typedef __u64 nvmev_u64;
typedef __u8 nvmev_u8;
#else
#include <stdint.h>
typedef uint32_t nvmev_u32;
typedef uint64_t nvmev_u64;
typedef uint8_t nvmev_u8;
#endif

#define NVMEV_VUC_ABI_VERSION 2u
#define NVMEV_VUC_OPCODE_FETCH_BATCH_RESULT 0xC0u
#define NVMEV_VUC_OPCODE_EXPAND_AND_FILTER_BATCH 0xC1u
/* ABI v2: reserved0 selects phase; wire struct sizes are unchanged.
 * NBR_PAGES returns words [page_id, valid_records, 1024 page words] per page.
 * node_list contains representative logical node IDs; at most 3 unique pages.
 * RERANK reads only supplied nodes' vectors, with no neighbor expansion. */
#define NVMEV_VUC_PHASE_COMBINED 0u
#define NVMEV_VUC_PHASE_NBR_PAGES 1u
#define NVMEV_VUC_PHASE_RERANK 2u
#define NVMEV_VUC_NBR_PAGE_WORDS 1026u
#define NVMEV_VUC_MAX_NBR_PAGES 15u
#define NVMEV_VUC_MAX_BATCH_NODES 64u
/* Reserved out-of-range SLBA used to carry C1 via a standard NVMe WRITE.
 * Linux maps vendor passthrough payloads as controller-to-host by default;
 * this staging WRITE gives the H2C payload the correct DMA direction. */
#define NVMEV_VUC_STAGE_SLBA 0xfffffffffffffff0ULL

enum nvmev_vuc_status {
	NVMEV_VUC_SUCCESS = 0,
	NVMEV_VUC_INVALID_BUFFER = 1,
	NVMEV_VUC_INVALID_NODE = 2,
	NVMEV_VUC_INVALID_ARGUMENT = 3,
	NVMEV_VUC_RESULT_OVERFLOW = 4,
	NVMEV_VUC_RESOURCE_BUSY = 5,
	NVMEV_VUC_TIMEOUT = 6,
};

enum nvmev_vuc_threshold_format {
	NVMEV_VUC_THRESHOLD_FP32 = 1,
};

struct nvmev_vuc_expand_filter_request {
	nvmev_u32 abi_version;
	nvmev_u32 opcode;
	nvmev_u64 query_id;
	nvmev_u64 batch_id;
	nvmev_u64 submit_time_ns;
	nvmev_u32 node_count;
	nvmev_u32 top_k;
	nvmev_u64 node_list_ref;
	nvmev_u64 node_list_bytes;
	nvmev_u64 query_ref;
	nvmev_u64 query_bytes;
	float threshold_fp32;
	nvmev_u32 threshold_format;
	nvmev_u64 neighbor_ids_ref;
	nvmev_u32 neighbor_ids_capacity;
	nvmev_u32 reserved0;
	nvmev_u64 candidates_ref;
	nvmev_u32 candidate_capacity;
	nvmev_u32 timeout_ms;
} __attribute__((packed));

struct nvmev_vuc_candidate {
	nvmev_u32 node_id;
	nvmev_u32 source_node_id;
	float distance;
} __attribute__((packed));

struct nvmev_vuc_completion {
	nvmev_u32 abi_version;
	nvmev_u32 status;
	nvmev_u64 query_id;
	nvmev_u64 batch_id;
	nvmev_u64 completion_time_ns;
	nvmev_u32 discovered_neighbor_count;
	nvmev_u32 candidate_count;
	nvmev_u64 input_bytes;
	nvmev_u64 output_bytes;
	nvmev_u64 c3_queue_wait_ns;
	nvmev_u64 input_dma_ns;
	nvmev_u64 nand_read_ns;
	nvmev_u64 neighbor_nand_ns;
	nvmev_u64 vector_nand_ns;
	nvmev_u64 neighbor_dma_ns;
	nvmev_u64 neighbor_parse_ns;
	nvmev_u64 vector_dma_ns;
	nvmev_u64 dsp_queue_wait_ns;
	nvmev_u64 dsp_compute_ns;
	nvmev_u64 output_dma_ns;
	nvmev_u64 idma_queue_wait_ns;
	nvmev_u64 dbuf_wait_ns;
	nvmev_u64 parser_queue_wait_ns;
	nvmev_u32 result_valid;
	nvmev_u32 reserved;
} __attribute__((packed));

/* Expansion payload is staged Host-to-Controller with a standard WRITE;
 * its semantic VUC operation is opcode 0xC1. References are slot offsets. */
struct nvmev_vuc_pcie_expand_payload {
	struct nvmev_vuc_expand_filter_request request;
	nvmev_u8 data[];
} __attribute__((packed));

/* C0 CDW10/11 are query_id, CDW12/13 are batch_id, and CDW14 is capacity.
 * Controller-to-Host data is this header followed by neighbor IDs/candidates. */
struct nvmev_vuc_pcie_result_header {
	struct nvmev_vuc_completion completion;
	nvmev_u32 total_bytes;
	nvmev_u32 reserved;
} __attribute__((packed));

#endif /* NVMEV_VUC_PROTOCOL_H */
