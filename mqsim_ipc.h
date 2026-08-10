/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef NVMEVIRT_MQSIM_IPC_H
#define NVMEVIRT_MQSIM_IPC_H

#include "nvmev.h"

int nvmev_mqsim_ipc_init(void);
void nvmev_mqsim_ipc_exit(void);
bool nvmev_mqsim_ipc_enabled(void);
int nvmev_mqsim_query_latency(struct nvmev_request *req, u64 *latency_ns);

#endif /* NVMEVIRT_MQSIM_IPC_H */
