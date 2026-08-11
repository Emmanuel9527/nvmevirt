/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef NVMEVIRT_MQSIM_IPC_H
#define NVMEVIRT_MQSIM_IPC_H

#include "nvmev.h"

int nvmev_mqsim_ipc_init(void);
void nvmev_mqsim_ipc_exit(void);
bool nvmev_mqsim_ipc_enabled(void);
int nvmev_mqsim_submit_async(struct nvmev_request *req, unsigned int worker_id,
			     unsigned int work_entry, u64 fallback_target_ns,
			     u64 *request_id);

#endif /* NVMEVIRT_MQSIM_IPC_H */
