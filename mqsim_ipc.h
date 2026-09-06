/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef NVMEVIRT_MQSIM_IPC_H
#define NVMEVIRT_MQSIM_IPC_H

#include "nvmev.h"

int nvmev_mqsim_ipc_init(void);
void nvmev_mqsim_ipc_exit(void);
bool nvmev_mqsim_ipc_enabled(void);
void nvmev_mqsim_record_cmd_path_timing(u64 proc_ns, u64 enqueue_ns,
					u64 submit_call_ns);
int nvmev_mqsim_submit_async(struct nvmev_request *req, unsigned int worker_id,
			     unsigned int work_entry, u64 backend_submit_time_ns,
			     u64 fallback_target_ns, u64 *request_id);

#endif /* NVMEVIRT_MQSIM_IPC_H */
