// SPDX-License-Identifier: GPL-2.0-only

#include <linux/atomic.h>
#include <linux/module.h>
#include <linux/spinlock.h>

#include "controller_timing.h"

#define NVMEV_CTRL_TIMING_MAX_SLOTS 32

static bool ctrl_timing_enable = true;
static unsigned int ctrl_frontend_slots = 3;
static unsigned int ctrl_completion_slots = 3;
static unsigned int ctrl_submission_queue_depth = 1024;
static unsigned int ctrl_completion_queue_depth = 1024;
static unsigned long ctrl_command_fetch_ns = 1000;
static unsigned long ctrl_command_decode_ns = 1000;
static unsigned long ctrl_dispatch_overhead_ns = 3000;
static unsigned long ctrl_cqe_write_ns = 1000;
static unsigned long ctrl_interrupt_ns = 3000;
static unsigned long ctrl_dsp_ns;

module_param(ctrl_timing_enable, bool, 0644);
MODULE_PARM_DESC(ctrl_timing_enable, "Enable queue-aware NVMeVirt controller timing model for MQSim IPC path");
module_param(ctrl_frontend_slots, uint, 0644);
MODULE_PARM_DESC(ctrl_frontend_slots, "Controller frontend service slots; default 3 from CSD IP Host R/W CPU 0,1,2");
module_param(ctrl_completion_slots, uint, 0644);
MODULE_PARM_DESC(ctrl_completion_slots, "Controller completion service slots; default 3");
module_param(ctrl_submission_queue_depth, uint, 0644);
MODULE_PARM_DESC(ctrl_submission_queue_depth, "Modeled controller submission queue capacity");
module_param(ctrl_completion_queue_depth, uint, 0644);
MODULE_PARM_DESC(ctrl_completion_queue_depth, "Modeled controller completion queue capacity");
module_param(ctrl_command_fetch_ns, ulong, 0644);
MODULE_PARM_DESC(ctrl_command_fetch_ns, "Modeled command fetch time in ns");
module_param(ctrl_command_decode_ns, ulong, 0644);
MODULE_PARM_DESC(ctrl_command_decode_ns, "Modeled command decode time in ns");
module_param(ctrl_dispatch_overhead_ns, ulong, 0644);
MODULE_PARM_DESC(ctrl_dispatch_overhead_ns, "Modeled backend dispatch overhead in ns");
module_param(ctrl_cqe_write_ns, ulong, 0644);
MODULE_PARM_DESC(ctrl_cqe_write_ns, "Modeled CQE write time in ns");
module_param(ctrl_interrupt_ns, ulong, 0644);
MODULE_PARM_DESC(ctrl_interrupt_ns, "Modeled interrupt/notification time in ns");
module_param(ctrl_dsp_ns, ulong, 0644);
MODULE_PARM_DESC(ctrl_dsp_ns, "Modeled DSP service time in ns; default 0 because DSP usage is not defined yet");

static DEFINE_SPINLOCK(ctrl_timing_lock);
static u64 frontend_slot_free_ns[NVMEV_CTRL_TIMING_MAX_SLOTS];
static u64 completion_slot_free_ns[NVMEV_CTRL_TIMING_MAX_SLOTS];

static atomic64_t frontend_count = ATOMIC64_INIT(0);
static atomic64_t frontend_wait_total_ns = ATOMIC64_INIT(0);
static atomic64_t frontend_wait_max_ns = ATOMIC64_INIT(0);
static atomic64_t frontend_service_total_ns = ATOMIC64_INIT(0);
static atomic64_t frontend_service_max_ns = ATOMIC64_INIT(0);
static atomic64_t frontend_queue_max = ATOMIC64_INIT(0);
static atomic64_t frontend_queue_over_depth = ATOMIC64_INIT(0);

static atomic64_t completion_count = ATOMIC64_INIT(0);
static atomic64_t completion_wait_total_ns = ATOMIC64_INIT(0);
static atomic64_t completion_wait_max_ns = ATOMIC64_INIT(0);
static atomic64_t completion_service_total_ns = ATOMIC64_INIT(0);
static atomic64_t completion_service_max_ns = ATOMIC64_INIT(0);
static atomic64_t completion_queue_max = ATOMIC64_INIT(0);
static atomic64_t completion_queue_over_depth = ATOMIC64_INIT(0);

static unsigned int clamp_slots(unsigned int slots)
{
	if (slots == 0)
		return 1;
	if (slots > NVMEV_CTRL_TIMING_MAX_SLOTS)
		return NVMEV_CTRL_TIMING_MAX_SLOTS;
	return slots;
}

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

static unsigned int count_busy_slots(u64 *slots, unsigned int slot_count, u64 now_ns)
{
	unsigned int i;
	unsigned int busy = 0;

	for (i = 0; i < slot_count; i++) {
		if (slots[i] > now_ns)
			busy++;
	}
	return busy;
}

static u64 reserve_slot(u64 *slots, unsigned int slot_count, u64 arrival_ns,
			u64 service_ns, atomic64_t *count, atomic64_t *wait_total,
			atomic64_t *wait_max, atomic64_t *service_total,
			atomic64_t *service_max, atomic64_t *queue_max,
			atomic64_t *over_depth, unsigned int queue_depth)
{
	unsigned long flags;
	unsigned int i;
	unsigned int best = 0;
	unsigned int busy;
	u64 start_ns;
	u64 wait_ns;
	u64 done_ns;

	slot_count = clamp_slots(slot_count);

	spin_lock_irqsave(&ctrl_timing_lock, flags);
	for (i = 1; i < slot_count; i++) {
		if (slots[i] < slots[best])
			best = i;
	}

	busy = count_busy_slots(slots, slot_count, arrival_ns);
	update_max(queue_max, busy);
	if (queue_depth > 0 && busy >= queue_depth)
		atomic64_inc(over_depth);

	start_ns = slots[best] > arrival_ns ? slots[best] : arrival_ns;
	wait_ns = start_ns - arrival_ns;
	done_ns = start_ns + service_ns;
	slots[best] = done_ns;
	spin_unlock_irqrestore(&ctrl_timing_lock, flags);

	atomic64_inc(count);
	atomic64_add(wait_ns, wait_total);
	update_max(wait_max, wait_ns);
	atomic64_add(service_ns, service_total);
	update_max(service_max, service_ns);

	return done_ns;
}

bool nvmev_ctrl_timing_enabled(void)
{
	return ctrl_timing_enable;
}

u64 nvmev_ctrl_timing_frontend_done(u64 arrival_ns, u32 sqid)
{
	u64 service_ns;

	if (!ctrl_timing_enable)
		return arrival_ns;

	service_ns = ctrl_command_fetch_ns + ctrl_command_decode_ns +
		     ctrl_dispatch_overhead_ns + ctrl_dsp_ns;
	return reserve_slot(frontend_slot_free_ns, ctrl_frontend_slots, arrival_ns,
			    service_ns, &frontend_count, &frontend_wait_total_ns,
			    &frontend_wait_max_ns, &frontend_service_total_ns,
			    &frontend_service_max_ns, &frontend_queue_max,
			    &frontend_queue_over_depth, ctrl_submission_queue_depth);
}

u64 nvmev_ctrl_timing_completion_done(u64 backend_done_ns, u32 cqid)
{
	u64 service_ns;

	if (!ctrl_timing_enable)
		return backend_done_ns;

	service_ns = ctrl_cqe_write_ns + ctrl_interrupt_ns;
	return reserve_slot(completion_slot_free_ns, ctrl_completion_slots, backend_done_ns,
			    service_ns, &completion_count, &completion_wait_total_ns,
			    &completion_wait_max_ns, &completion_service_total_ns,
			    &completion_service_max_ns, &completion_queue_max,
			    &completion_queue_over_depth, ctrl_completion_queue_depth);
}

void nvmev_ctrl_timing_proc_print(struct seq_file *m)
{
	s64 f_count = atomic64_read(&frontend_count);
	s64 c_count = atomic64_read(&completion_count);

	seq_printf(m, "ctrl_timing_enable: %u\n", ctrl_timing_enable ? 1 : 0);
	seq_printf(m, "ctrl_frontend_slots: %u\n", clamp_slots(ctrl_frontend_slots));
	seq_printf(m, "ctrl_completion_slots: %u\n", clamp_slots(ctrl_completion_slots));
	seq_printf(m, "ctrl_submission_queue_depth: %u\n", ctrl_submission_queue_depth);
	seq_printf(m, "ctrl_completion_queue_depth: %u\n", ctrl_completion_queue_depth);
	seq_printf(m, "ctrl_command_fetch_ns: %lu\n", ctrl_command_fetch_ns);
	seq_printf(m, "ctrl_command_decode_ns: %lu\n", ctrl_command_decode_ns);
	seq_printf(m, "ctrl_dispatch_overhead_ns: %lu\n", ctrl_dispatch_overhead_ns);
	seq_printf(m, "ctrl_cqe_write_ns: %lu\n", ctrl_cqe_write_ns);
	seq_printf(m, "ctrl_interrupt_ns: %lu\n", ctrl_interrupt_ns);
	seq_printf(m, "ctrl_dsp_ns: %lu\n", ctrl_dsp_ns);
	seq_printf(m, "ctrl_frontend_count: %lld\n", f_count);
	seq_printf(m, "ctrl_frontend_wait_avg_ns: %lld\n",
		   avg_or_zero(&frontend_wait_total_ns, f_count));
	seq_printf(m, "ctrl_frontend_wait_max_ns: %lld\n",
		   atomic64_read(&frontend_wait_max_ns));
	seq_printf(m, "ctrl_frontend_service_avg_ns: %lld\n",
		   avg_or_zero(&frontend_service_total_ns, f_count));
	seq_printf(m, "ctrl_frontend_service_max_ns: %lld\n",
		   atomic64_read(&frontend_service_max_ns));
	seq_printf(m, "ctrl_frontend_queue_max: %lld\n",
		   atomic64_read(&frontend_queue_max));
	seq_printf(m, "ctrl_frontend_queue_over_depth: %lld\n",
		   atomic64_read(&frontend_queue_over_depth));
	seq_printf(m, "ctrl_completion_count: %lld\n", c_count);
	seq_printf(m, "ctrl_completion_wait_avg_ns: %lld\n",
		   avg_or_zero(&completion_wait_total_ns, c_count));
	seq_printf(m, "ctrl_completion_wait_max_ns: %lld\n",
		   atomic64_read(&completion_wait_max_ns));
	seq_printf(m, "ctrl_completion_service_avg_ns: %lld\n",
		   avg_or_zero(&completion_service_total_ns, c_count));
	seq_printf(m, "ctrl_completion_service_max_ns: %lld\n",
		   atomic64_read(&completion_service_max_ns));
	seq_printf(m, "ctrl_completion_queue_max: %lld\n",
		   atomic64_read(&completion_queue_max));
	seq_printf(m, "ctrl_completion_queue_over_depth: %lld\n",
		   atomic64_read(&completion_queue_over_depth));
}
