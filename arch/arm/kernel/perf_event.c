#undef DEBUG

/*
 * ARM performance counter support.
 *
 * Copyright (C) 2009 picoChip Designs, Ltd., Jamie Iles
 * Copyright (C) 2010 ARM Ltd., Will Deacon <will.deacon@arm.com>
 *
 * This code is based on the sparc64 perf event code, which is in turn based
 * on the x86 code. Callchain code is based on the ARM OProfile backtrace
 * code.
 */
#define pr_fmt(fmt) "hw perfevents: " fmt

#include <linux/bitmap.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/export.h>
#include <linux/perf_event.h>
#include <linux/platform_device.h>
#include <linux/spinlock.h>
#include <linux/uaccess.h>
#include <linux/irq.h>

#include <asm/cputype.h>
#include <asm/irq.h>
#include <asm/irq_regs.h>
#include <asm/pmu.h>
#include <asm/stacktrace.h>

#include <linux/cpu_pm.h>

#define ARMPMU_MAX_HWEVENTS		32

static DEFINE_PER_CPU(struct perf_event * [ARMPMU_MAX_HWEVENTS], hw_events);
static DEFINE_PER_CPU(unsigned long [BITS_TO_LONGS(ARMPMU_MAX_HWEVENTS)], used_mask);
static DEFINE_PER_CPU(struct pmu_hw_events, cpu_hw_events);

#define to_arm_pmu(p) (container_of(p, struct arm_pmu, pmu))

static struct arm_pmu *cpu_pmu;

enum arm_perf_pmu_ids
armpmu_get_pmu_id(void)
{
	int id = -ENODEV;

	if (cpu_pmu != NULL)
		id = cpu_pmu->id;

	return id;
}
EXPORT_SYMBOL_GPL(armpmu_get_pmu_id);

int perf_num_counters(void)
{
	int max_events = 0;

	if (cpu_pmu != NULL)
		max_events = cpu_pmu->num_events;

	return max_events;
}
EXPORT_SYMBOL_GPL(perf_num_counters);

#define HW_OP_UNSUPPORTED		0xFFFF

#define C(_x) \
	PERF_COUNT_HW_CACHE_##_x

#define CACHE_OP_UNSUPPORTED		0xFFFF

static int
armpmu_map_cache_event(unsigned (*cache_map)
				      [PERF_COUNT_HW_CACHE_MAX]
				      [PERF_COUNT_HW_CACHE_OP_MAX]
				      [PERF_COUNT_HW_CACHE_RESULT_MAX],
		       u64 config)
{
	unsigned int cache_type, cache_op, cache_result, ret;

	cache_type = (config >>  0) & 0xff;
	if (cache_type >= PERF_COUNT_HW_CACHE_MAX)
		return -EINVAL;

	cache_op = (config >>  8) & 0xff;
	if (cache_op >= PERF_COUNT_HW_CACHE_OP_MAX)
		return -EINVAL;

	cache_result = (config >> 16) & 0xff;
	if (cache_result >= PERF_COUNT_HW_CACHE_RESULT_MAX)
		return -EINVAL;

	ret = (int)(*cache_map)[cache_type][cache_op][cache_result];

	if (ret == CACHE_OP_UNSUPPORTED)
		return -ENOENT;

	return ret;
}

static int
armpmu_map_event(const unsigned (*event_map)[PERF_COUNT_HW_MAX], u64 config)
{
	int mapping;

	if (config >= PERF_COUNT_HW_MAX)
		return -ENOENT;

	mapping = (*event_map)[config];
	return mapping == HW_OP_UNSUPPORTED ? -ENOENT : mapping;
}

static int
armpmu_map_raw_event(u32 raw_event_mask, u64 config)
{
	return (int)(config & raw_event_mask);
}

static int map_cpu_event(struct perf_event *event,
			 const unsigned (*event_map)[PERF_COUNT_HW_MAX],
			 unsigned (*cache_map)
					[PERF_COUNT_HW_CACHE_MAX]
					[PERF_COUNT_HW_CACHE_OP_MAX]
					[PERF_COUNT_HW_CACHE_RESULT_MAX],
			 u32 raw_event_mask)
{
	u64 config = event->attr.config;

	switch (event->attr.type) {
	case PERF_TYPE_HARDWARE:
		return armpmu_map_event(event_map, config);
	case PERF_TYPE_HW_CACHE:
		return armpmu_map_cache_event(cache_map, config);
	case PERF_TYPE_RAW:
		return armpmu_map_raw_event(raw_event_mask, config);
	}

	return -ENOENT;
}

int
armpmu_event_set_period(struct perf_event *event,
			struct hw_perf_event *hwc,
			int idx)
{
	struct arm_pmu *armpmu = to_arm_pmu(event->pmu);
	s64 left = local64_read(&hwc->period_left);
	s64 period = hwc->sample_period;
	int ret = 0;

	if (unlikely(left <= -period)) {
		left = period;
		local64_set(&hwc->period_left, left);
		hwc->last_period = period;
		ret = 1;
	}

	if (unlikely(left <= 0)) {
		left += period;
		local64_set(&hwc->period_left, left);
		hwc->last_period = period;
		ret = 1;
	}

	if (left > (s64)armpmu->max_period)
		left = armpmu->max_period;

	local64_set(&hwc->prev_count, (u64)-left);

	armpmu->write_counter(idx, (u64)(-left) & 0xffffffff);

	perf_event_update_userpage(event);

	return ret;
}

u64
armpmu_event_update(struct perf_event *event,
		    struct hw_perf_event *hwc,
		    int idx)
{
	struct arm_pmu *armpmu = to_arm_pmu(event->pmu);
	u64 delta, prev_raw_count, new_raw_count;

again:
	prev_raw_count = local64_read(&hwc->prev_count);
	new_raw_count = armpmu->read_counter(idx);

	if (local64_cmpxchg(&hwc->prev_count, prev_raw_count,
			     new_raw_count) != prev_raw_count)
		goto again;

	delta = (new_raw_count - prev_raw_count) & armpmu->max_period;

	local64_add(delta, &event->count);
	local64_sub(delta, &hwc->period_left);

	return new_raw_count;
}

static void
armpmu_read(struct perf_event *event)
{
	struct hw_perf_event *hwc = &event->hw;

	
	if (hwc->idx < 0)
		return;

	armpmu_event_update(event, hwc, hwc->idx);
}

static void
armpmu_stop(struct perf_event *event, int flags)
{
	struct arm_pmu *armpmu = to_arm_pmu(event->pmu);
	struct hw_perf_event *hwc = &event->hw;

	if (!(hwc->state & PERF_HES_STOPPED)) {
		armpmu->disable(hwc, hwc->idx);
		barrier(); 
		armpmu_event_update(event, hwc, hwc->idx);
		hwc->state |= PERF_HES_STOPPED | PERF_HES_UPTODATE;
	}
}

static void
armpmu_start(struct perf_event *event, int flags)
{
	struct arm_pmu *armpmu = to_arm_pmu(event->pmu);
	struct hw_perf_event *hwc = &event->hw;

	if (flags & PERF_EF_RELOAD)
		WARN_ON_ONCE(!(hwc->state & PERF_HES_UPTODATE));

	hwc->state = 0;
	armpmu_event_set_period(event, hwc, hwc->idx);
	armpmu->enable(hwc, hwc->idx, event->cpu);
}

static void
armpmu_del(struct perf_event *event, int flags)
{
	struct arm_pmu *armpmu = to_arm_pmu(event->pmu);
	struct pmu_hw_events *hw_events = armpmu->get_hw_events();
	struct hw_perf_event *hwc = &event->hw;
	int idx = hwc->idx;

	WARN_ON(idx < 0);

	armpmu_stop(event, PERF_EF_UPDATE);
	hw_events->events[idx] = NULL;
	clear_bit(idx, hw_events->used_mask);

	
	if (armpmu->clear_event_constraints)
		armpmu->clear_event_constraints(event);

	perf_event_update_userpage(event);
}

static int
armpmu_add(struct perf_event *event, int flags)
{
	struct arm_pmu *armpmu = to_arm_pmu(event->pmu);
	struct pmu_hw_events *hw_events = armpmu->get_hw_events();
	struct hw_perf_event *hwc = &event->hw;
	int idx;
	int err = 0;

	perf_pmu_disable(event->pmu);
	if (armpmu->test_set_event_constraints)
		if (armpmu->test_set_event_constraints(event) < 0) {
			pr_err("Event: %llx failed constraint check.\n",
					event->attr.config);
			event->state = PERF_EVENT_STATE_OFF;
			goto out;
		}

	
	idx = armpmu->get_event_idx(hw_events, hwc);
	if (idx < 0) {
		err = idx;
		goto out;
	}

	event->hw.idx = idx;
	armpmu->disable(hwc, idx);
	hw_events->events[idx] = event;

	hwc->state = PERF_HES_STOPPED | PERF_HES_UPTODATE;
	if (flags & PERF_EF_START)
		armpmu_start(event, PERF_EF_RELOAD);

	
	perf_event_update_userpage(event);

out:
	perf_pmu_enable(event->pmu);
	return err;
}

static int
validate_event(struct pmu_hw_events *hw_events,
	       struct perf_event *event)
{
	struct arm_pmu *armpmu = to_arm_pmu(event->pmu);
	struct hw_perf_event fake_event = event->hw;
	struct pmu *leader_pmu = event->group_leader->pmu;

	if (is_software_event(event))
		return 1;

	if (event->pmu != leader_pmu || event->state <= PERF_EVENT_STATE_OFF)
		return 1;

	return armpmu->get_event_idx(hw_events, &fake_event) >= 0;
}

static int
validate_group(struct perf_event *event)
{
	struct perf_event *sibling, *leader = event->group_leader;
	struct pmu_hw_events fake_pmu;
	DECLARE_BITMAP(fake_used_mask, ARMPMU_MAX_HWEVENTS);

	memset(fake_used_mask, 0, sizeof(fake_used_mask));
	fake_pmu.used_mask = fake_used_mask;

	if (!validate_event(&fake_pmu, leader))
		return -EINVAL;

	list_for_each_entry(sibling, &leader->sibling_list, group_entry) {
		if (!validate_event(&fake_pmu, sibling))
			return -EINVAL;
	}

	if (!validate_event(&fake_pmu, event))
		return -EINVAL;

	return 0;
}

static irqreturn_t armpmu_platform_irq(int irq, void *dev)
{
	struct arm_pmu *armpmu = (struct arm_pmu *) dev;
	struct platform_device *plat_device = armpmu->plat_device;
	struct arm_pmu_platdata *plat = dev_get_platdata(&plat_device->dev);

	return plat->handle_irq(irq, dev, armpmu->handle_irq);
}

int
armpmu_generic_request_irq(int irq, irq_handler_t *handle_irq)
{
        return request_irq(irq, *handle_irq,
                        IRQF_DISABLED | IRQF_NOBALANCING,
                        "armpmu", NULL);
}

void
armpmu_generic_free_irq(int irq)
{
        if (irq >= 0)
                free_irq(irq, NULL);
}

static void
armpmu_release_hardware(struct arm_pmu *armpmu)
{
	int i, irq, irqs;
	struct platform_device *pmu_device = armpmu->plat_device;

	irqs = min(pmu_device->num_resources, num_possible_cpus());

	for (i = 0; i < irqs; ++i) {
		if (!cpumask_test_and_clear_cpu(i, &armpmu->active_irqs))
			continue;
		irq = platform_get_irq(pmu_device, i);
		armpmu->free_pmu_irq(irq);
	}

	release_pmu(armpmu->type);
}

static int
armpmu_reserve_hardware(struct arm_pmu *armpmu)
{
	struct arm_pmu_platdata *plat;
	irq_handler_t handle_irq;
	int i, err, irq, irqs;
	struct platform_device *pmu_device = armpmu->plat_device;

	if (!pmu_device)
		return -ENODEV;

	err = reserve_pmu(armpmu->type);
	if (err) {
		pr_warning("unable to reserve pmu\n");
		return err;
	}

	plat = dev_get_platdata(&pmu_device->dev);
	if (plat && plat->handle_irq)
		handle_irq = armpmu_platform_irq;
	else
		handle_irq = armpmu->handle_irq;

	irqs = min(pmu_device->num_resources, num_possible_cpus());
	if (irqs < 1) {
		pr_err("no irqs for PMUs defined\n");
		return -ENODEV;
	}

	for (i = 0; i < irqs; ++i) {
		err = 0;
		irq = platform_get_irq(pmu_device, i);
		if (irq < 0)
			continue;

		if (irq_set_affinity(irq, cpumask_of(i)) && irqs > 1) {
			pr_warning("unable to set irq affinity (irq=%d, cpu=%u)\n",
				    irq, i);
			continue;
		}

		err = armpmu->request_pmu_irq(irq, &handle_irq);

                if (err) {
                        pr_warning("unable to request IRQ%d for %s perf "
                                "counters\n", irq, armpmu->name);

			armpmu_release_hardware(cpu_pmu);
                        return err;
                }

		cpumask_set_cpu(i, &armpmu->active_irqs);
	}

	return 0;
}

static void
hw_perf_event_destroy(struct perf_event *event)
{
	struct arm_pmu *armpmu = to_arm_pmu(event->pmu);
	atomic_t *active_events	 = &armpmu->active_events;
	struct mutex *pmu_reserve_mutex = &armpmu->reserve_mutex;

	if (atomic_dec_and_mutex_lock(active_events, pmu_reserve_mutex)) {
		armpmu_release_hardware(armpmu);
		mutex_unlock(pmu_reserve_mutex);
	}
}

static int
event_requires_mode_exclusion(struct perf_event_attr *attr)
{
	return attr->exclude_idle || attr->exclude_user ||
	       attr->exclude_kernel || attr->exclude_hv;
}

static int
__hw_perf_event_init(struct perf_event *event)
{
	struct arm_pmu *armpmu = to_arm_pmu(event->pmu);
	struct hw_perf_event *hwc = &event->hw;
	int mapping, err;

	mapping = armpmu->map_event(event);

	if (mapping < 0) {
		pr_debug("event %x:%llx not supported\n", event->attr.type,
			 event->attr.config);
		return mapping;
	}

	hwc->idx		= -1;
	hwc->config_base	= 0;
	hwc->config		= 0;
	hwc->event_base		= 0;

	if ((!armpmu->set_event_filter ||
	     armpmu->set_event_filter(hwc, &event->attr)) &&
	     event_requires_mode_exclusion(&event->attr)) {
		pr_debug("ARM performance counters do not support "
			 "mode exclusion\n");
		return -EPERM;
	}


	hwc->config_base	    |= (unsigned long)mapping;

	if (!hwc->sample_period) {
		hwc->sample_period  = armpmu->max_period >> 1;
		hwc->last_period    = hwc->sample_period;
		local64_set(&hwc->period_left, hwc->sample_period);
	}

	err = 0;
	if (event->group_leader != event) {
		err = validate_group(event);
		if (err)
			return -EINVAL;
	}

	return err;
}

static int armpmu_event_init(struct perf_event *event)
{
	struct arm_pmu *armpmu = to_arm_pmu(event->pmu);
	int err = 0;
	atomic_t *active_events = &armpmu->active_events;

	
	if (has_branch_stack(event))
		return -EOPNOTSUPP;

	if (armpmu->map_event(event) == -ENOENT)
		return -ENOENT;

	event->destroy = hw_perf_event_destroy;

	if (!atomic_inc_not_zero(active_events)) {
		mutex_lock(&armpmu->reserve_mutex);
		if (atomic_read(active_events) == 0)
			err = armpmu_reserve_hardware(armpmu);

		if (!err)
			atomic_inc(active_events);
		mutex_unlock(&armpmu->reserve_mutex);
	}

	if (err)
		return err;

	err = __hw_perf_event_init(event);
	if (err)
		hw_perf_event_destroy(event);

	return err;
}

static void armpmu_enable(struct pmu *pmu)
{
	struct arm_pmu *armpmu = to_arm_pmu(pmu);
	struct pmu_hw_events *hw_events = armpmu->get_hw_events();
	int enabled = bitmap_weight(hw_events->used_mask, armpmu->num_events);

	if (enabled)
		armpmu->start();
}

static void armpmu_disable(struct pmu *pmu)
{
	struct arm_pmu *armpmu = to_arm_pmu(pmu);
	armpmu->stop();
}

static void armpmu_init(struct arm_pmu *armpmu)
{
	atomic_set(&armpmu->active_events, 0);
	mutex_init(&armpmu->reserve_mutex);

	armpmu->pmu.pmu_enable = armpmu_enable;
	armpmu->pmu.pmu_disable = armpmu_disable;
	armpmu->pmu.event_init = armpmu_event_init;
	armpmu->pmu.add = armpmu_add;
	armpmu->pmu.del = armpmu_del;
	armpmu->pmu.start = armpmu_start;
	armpmu->pmu.stop = armpmu_stop;
	armpmu->pmu.read = armpmu_read;
}

int armpmu_register(struct arm_pmu *armpmu, char *name, int type)
{
	armpmu_init(armpmu);
	return perf_pmu_register(&armpmu->pmu, name, type);
}

#include "perf_event_xscale.c"
#include "perf_event_v6.c"
#include "perf_event_v7.c"
#include "perf_event_msm_krait.c"
#include "perf_event_msm.c"

static int __init
cpu_pmu_reset(void)
{
	if (cpu_pmu && cpu_pmu->reset)
		return on_each_cpu(cpu_pmu->reset, NULL, 1);
	return 0;
}
arch_initcall(cpu_pmu_reset);

static struct of_device_id armpmu_of_device_ids[] = {
	{.compatible = "arm,cortex-a9-pmu"},
	{.compatible = "arm,cortex-a8-pmu"},
	{.compatible = "arm,arm1136-pmu"},
	{.compatible = "arm,arm1176-pmu"},
	{},
};

static struct platform_device_id armpmu_plat_device_ids[] = {
	{.name = "cpu-arm-pmu"},
	{},
};

static int __devinit armpmu_device_probe(struct platform_device *pdev)
{
	if (!cpu_pmu)
		return -ENODEV;

	cpu_pmu->plat_device = pdev;
	return 0;
}

static struct platform_driver armpmu_driver = {
	.driver		= {
		.name	= "cpu-arm-pmu",
		.of_match_table = armpmu_of_device_ids,
	},
	.probe		= armpmu_device_probe,
	.id_table	= armpmu_plat_device_ids,
};

static int __init register_pmu_driver(void)
{
	return platform_driver_register(&armpmu_driver);
}
device_initcall(register_pmu_driver);

static struct pmu_hw_events *armpmu_get_cpu_events(void)
{
	return &__get_cpu_var(cpu_hw_events);
}

static void __init cpu_pmu_init(struct arm_pmu *armpmu)
{
	int cpu;
	for_each_possible_cpu(cpu) {
		struct pmu_hw_events *events = &per_cpu(cpu_hw_events, cpu);
		events->events = per_cpu(hw_events, cpu);
		events->used_mask = per_cpu(used_mask, cpu);
		raw_spin_lock_init(&events->pmu_lock);
	}
	armpmu->get_hw_events = armpmu_get_cpu_events;
	armpmu->type = ARM_PMU_DEVICE_CPU;
}

static int __cpuinit pmu_cpu_notify(struct notifier_block *b,
					unsigned long action, void *hcpu)
{
	if ((action & ~CPU_TASKS_FROZEN) != CPU_STARTING)
		return NOTIFY_DONE;

	if (cpu_pmu && cpu_pmu->reset)
		cpu_pmu->reset(NULL);

	return NOTIFY_OK;
}

static void armpmu_update_counters(void)
{
	struct pmu_hw_events *hw_events;
	int idx;

	if (!cpu_pmu)
		return;

	hw_events = cpu_pmu->get_hw_events();

	for (idx = 0; idx <= cpu_pmu->num_events; ++idx) {
		struct perf_event *event = hw_events->events[idx];

		if (!event)
			continue;

		armpmu_read(event);
	}
}

static int cpu_has_active_perf(void)
{
	struct pmu_hw_events *hw_events;
	int enabled;

	if (!cpu_pmu)
		return 0;

	hw_events = cpu_pmu->get_hw_events();
	enabled = bitmap_weight(hw_events->used_mask, cpu_pmu->num_events);

	if (enabled)
		
		return 1;

	return 0;
}

static struct notifier_block __cpuinitdata pmu_cpu_notifier = {
	.notifier_call = pmu_cpu_notify,
};

static int perf_cpu_pm_notifier(struct notifier_block *self, unsigned long cmd,
		void *v)
{
	switch (cmd) {
	case CPU_PM_ENTER:
		if (cpu_has_active_perf()) {
			armpmu_update_counters();
			perf_pmu_disable(&cpu_pmu->pmu);
		}
		break;

	case CPU_PM_ENTER_FAILED:
	case CPU_PM_EXIT:
		if (cpu_has_active_perf() && cpu_pmu->reset) {
			cpu_pmu->reset(NULL);
			perf_pmu_enable(&cpu_pmu->pmu);
		}
		break;
	}

	return NOTIFY_OK;
}

static struct notifier_block perf_cpu_pm_notifier_block = {
	.notifier_call = perf_cpu_pm_notifier,
};

static int __init
init_hw_perf_events(void)
{
	unsigned long cpuid = read_cpuid_id();
	unsigned long implementor = (cpuid & 0xFF000000) >> 24;
	unsigned long part_number = (cpuid & 0xFFF0);

	
	if (0x41 == implementor) {
		switch (part_number) {
		case 0xB360:	
		case 0xB560:	
		case 0xB760:	
			cpu_pmu = armv6pmu_init();
			break;
		case 0xB020:	
			cpu_pmu = armv6mpcore_pmu_init();
			break;
		case 0xC080:	
			cpu_pmu = armv7_a8_pmu_init();
			break;
		case 0xC090:	
			cpu_pmu = armv7_a9_pmu_init();
			break;
		case 0xC050:	
			cpu_pmu = armv7_a5_pmu_init();
			break;
		case 0xC0F0:	
			cpu_pmu = armv7_a15_pmu_init();
			break;
		case 0xC070:	
			cpu_pmu = armv7_a7_pmu_init();
			break;
		}
	
	} else if (0x69 == implementor) {
		part_number = (cpuid >> 13) & 0x7;
		switch (part_number) {
		case 1:
			cpu_pmu = xscale1pmu_init();
			break;
		case 2:
			cpu_pmu = xscale2pmu_init();
			break;
		}
	
	} else if (0x51 == implementor) {
		switch (part_number) {
		case 0x00F0:    
			cpu_pmu = armv7_scorpion_pmu_init();
			break;
		case 0x02D0:    
			cpu_pmu = armv7_scorpionmp_pmu_init();
			break;
		case 0x0490:    
		case 0x04D0:    
		case 0x06F0:    
			cpu_pmu = armv7_krait_pmu_init();
			break;
		}
	}


	if (cpu_pmu) {
		pr_info("enabled with %s PMU driver, %d counters available\n",
			cpu_pmu->name, cpu_pmu->num_events);
		cpu_pmu_init(cpu_pmu);
		register_cpu_notifier(&pmu_cpu_notifier);
		armpmu_register(cpu_pmu, "cpu", PERF_TYPE_RAW);
		cpu_pm_register_notifier(&perf_cpu_pm_notifier_block);
	} else {
		pr_info("no hardware support available\n");
	}

	return 0;
}
early_initcall(init_hw_perf_events);


struct frame_tail {
	struct frame_tail __user *fp;
	unsigned long sp;
	unsigned long lr;
} __attribute__((packed));

static struct frame_tail __user *
user_backtrace(struct frame_tail __user *tail,
	       struct perf_callchain_entry *entry)
{
	struct frame_tail buftail;

	
	if (!access_ok(VERIFY_READ, tail, sizeof(buftail)))
		return NULL;
	if (__copy_from_user_inatomic(&buftail, tail, sizeof(buftail)))
		return NULL;

	perf_callchain_store(entry, buftail.lr);

	if (tail + 1 >= buftail.fp)
		return NULL;

	return buftail.fp - 1;
}

void
perf_callchain_user(struct perf_callchain_entry *entry, struct pt_regs *regs)
{
	struct frame_tail __user *tail;


	tail = (struct frame_tail __user *)regs->ARM_fp - 1;

	while ((entry->nr < PERF_MAX_STACK_DEPTH) &&
	       tail && !((unsigned long)tail & 0x3))
		tail = user_backtrace(tail, entry);
}

static int
callchain_trace(struct stackframe *fr,
		void *data)
{
	struct perf_callchain_entry *entry = data;
	perf_callchain_store(entry, fr->pc);
	return 0;
}

void
perf_callchain_kernel(struct perf_callchain_entry *entry, struct pt_regs *regs)
{
	struct stackframe fr;

	fr.fp = regs->ARM_fp;
	fr.sp = regs->ARM_sp;
	fr.lr = regs->ARM_lr;
	fr.pc = regs->ARM_pc;
	walk_stackframe(&fr, callchain_trace, entry);
}
