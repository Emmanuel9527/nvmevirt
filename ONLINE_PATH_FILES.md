# NVMeVirt Files Used by the MQSim Online Path

This note documents the NVMeVirt files that matter for the current NVMeVirt + MQSim native-online integration. The active path is: a host NVMe read/write command enters NVMeVirt, NVMeVirt models controller-side frontend timing, sends request metadata to the MQSim userspace daemon, receives MQSim backend latency, models controller-side completion timing, and finally completes the host request.

## Build And Entry Points

| File | Role |
|---|---|
| `Kbuild` | Selects the NVMeVirt build target and lists object files linked into `nvmev.ko`. The MQSim integration currently builds `mqsim_ipc.o` and `controller_timing.o` into the kernel module. |
| `Makefile` | Invokes the kernel build system for this module. Use this when rebuilding `nvmev.ko` after changing NVMeVirt source files. |
| `main.c` | Module initialization and teardown live here. It parses module parameters, initializes namespaces/device state, calls `nvmev_mqsim_ipc_init()`, and starts NVMeVirt I/O workers. |
| `nvmev.h` | Main shared NVMeVirt data structures and function declarations. Important structs include `nvmev_config`, `nvmev_request`, `nvmev_result`, `nvmev_io_work`, `nvmev_io_worker`, and `nvmev_dev`. |

## Host-Facing NVMe Path

| File | Role |
|---|---|
| `pci.c` / `pci.h` | Emulates the PCIe-facing NVMe device interface. This is part of how Linux sees NVMeVirt as an NVMe controller. |
| `admin.c` | Handles NVMe admin commands such as identify/setup behavior. This affects what capabilities NVMeVirt exposes to the host. |
| `nvme.h` | Contains NVMe command/register structure definitions used by NVMeVirt. The I/O path reads host commands through these structures. |
| `io.c` | Main host I/O path. It parses SQ entries, calls the local namespace timing callback, enters the controller timing model for MQSim IPC mode, enqueues work entries, submits requests to MQSim, and finally fills CQ entries when completion time arrives. |
| `dma.c` / `dma.h` | Optional data movement support for host memory transfers. In the current read-latency experiments, the timing focus is mostly on command/completion and MQSim latency rather than DMA modeling. |

## MQSim IPC Bridge

| File | Role |
|---|---|
| `mqsim_ipc_protocol.h` | Defines the shared-memory ABI between NVMeVirt and `MQSimIPCDaemon`. `nvmev_mqsim_io_msg` carries request id, simulated submit time, kernel wall timestamp, opcode, SLBA, NLB, and reply latency/status. |
| `mqsim_ipc.h` | Public interface used by `io.c`. It exposes IPC initialization, enable checks, timing-stat recording, and async request submission to MQSim. |
| `mqsim_ipc.c` | Implements the shared-memory request/response rings, pending request table, daemon open/mmap/poll/ioctl path, and `/proc/nvmev/mqsim_ipc` statistics. It converts MQSim backend latency into NVMeVirt completion time and now applies controller completion timing before waking the I/O worker. |

## Controller Timing Model

| File | Role |
|---|---|
| `controller_timing.h` | Public API for the lightweight controller timing model. `io.c` calls the frontend function before MQSim submission, and `mqsim_ipc.c` calls the completion function after MQSim replies. |
| `controller_timing.c` | Queue-aware controller timing model. It uses configurable frontend/completion slots, service times, and queue-depth counters; by default it uses 3 frontend slots and 3 completion slots, matching the CSD IP reference where Host R/W uses CPU 0,1,2. The default controller service estimate is 5 us frontend plus 4 us completion; DSP service time exists as a parameter but defaults to 0 because the DSP role is not decided yet. |

## NVMeVirt Path Statistics

| File | Role |
|---|---|
| `path_stats.h` | Public API for NVMeVirt CPU/path monitoring. `io.c` records SQ batch size, worker enqueue counts, worker queue scans, data-copy/perform-I/O cost, CQ fill cost, lateness, and IRQ cost through this interface. |
| `path_stats.c` | Implements `/proc/nvmev/path_stats`. This is for measuring real NVMeVirt kernel path behavior, not simulated controller timing; it helps identify whether fio QD growth is caused by worker queue buildup, worker lateness, data-copy cost, CQ fill cost, or interrupt signaling. |

## Local Timing / Fallback Paths

| File | Role |
|---|---|
| `simple_ftl.c` / `simple_ftl.h` | Current build target uses `CONFIG_NVMEVIRT_NVM`, which links the simple NVM-style FTL. In MQSim IPC mode, its `proc_io_cmd()` result is kept mainly as fallback timing if MQSim IPC fails. |
| `ssd.c` / `ssd.h` / `ssd_config.h` | Conventional SSD timing path used when building the SSD target instead of the current NVM target. These files are useful background, but they are not the main timing source in the current MQSim native-online path. |
| `conv_ftl.c` / `conv_ftl.h` | Conventional SSD FTL path used by `CONFIG_NVMEVIRT_SSD`. Not on the current active MQSim IPC path unless the build target is changed. |
| `channel_model.c` / `channel_model.h` | Backend channel model for NVMeVirt's conventional SSD/ZNS paths. MQSim native-online experiments currently use MQSim's own backend channel/chip/die timing instead. |

## Other Build Targets Not On The Current Path

| File | Role |
|---|---|
| `zns_ftl.c`, `zns_ftl.h`, `zns_read_write.c`, `zns_mgmt_send.c`, `zns_mgmt_recv.c`, `nvme_zns.h` | Zoned Namespace implementation. Useful for NVMeVirt generally, but not used by the current MQSim native-online read experiments. |
| `kv_ftl.c`, `kv_ftl.h`, `append_only.c`, `append_only.h`, `bitmap.c`, `bitmap.h`, `nvme_kv.h` | Key-value SSD implementation. Not part of the current DiskANN/fio block read path. |
| `pqueue/` | Helper priority queue implementation used by some alternative NVMeVirt SSD paths. Not central to the current MQSim IPC path. |

## Runtime Outputs And Generated Files

| File | Role |
|---|---|
| `nvmev.ko` | Built kernel module. This is generated output, not source, but it is what gets loaded for experiments. |
| `*.o`, `*.mod`, `Module.symvers`, `modules.order` | Kernel build artifacts. These are useful locally but normally should not be committed unless the project intentionally tracks binaries. |

## Current Timing Interpretation

The current model still uses `nsecs_target` as the final delivery timestamp consumed by the NVMeVirt I/O worker. However, in MQSim IPC mode, `nsecs_target` is no longer the timing model itself: it is the output of controller frontend timing + MQSim backend timing + controller completion timing.

Useful runtime counters are exposed through:

```bash
cat /proc/nvmev/mqsim_ipc
```

Important fields include `cmd_proc_avg_ns`, `enqueue_avg_ns`, `submit_call_avg_ns`, `roundtrip_avg_ns`, `ctrl_frontend_wait_avg_ns`, `ctrl_frontend_queue_max`, `ctrl_completion_wait_avg_ns`, and `ctrl_completion_queue_max`.

Real NVMeVirt CPU/path counters are exposed through:

```bash
cat /proc/nvmev/path_stats
```

Important fields include `sq_batch_submitted_avg`, `sq_batch_accepted_avg`, per-worker `AvgQLen`, `MaxQLen`, `MaxActive`, `PerformAvg(ns)`, `FillCQAvg(ns)`, `LateAvg(ns)`, and `IRQAvg(ns)`.

The current first-order controller timing defaults are:

```text
ctrl_command_fetch_ns = 1000
ctrl_command_decode_ns = 1000
ctrl_dispatch_overhead_ns = 3000
ctrl_cqe_write_ns = 1000
ctrl_interrupt_ns = 3000
ctrl_dsp_ns = 0
```

These are intentionally conservative microsecond-scale estimates rather than hardware-accurate values for a specific controller. They should be calibrated with fio and DiskANN measurements.
