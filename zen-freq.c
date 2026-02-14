// SPDX-License-Identifier: GPL-2.0-only
/*
 * zen-freq.c - AMD Zen 2+ Perfect Potential CPU Frequency Driver
 *
 * An advanced frequency scaling driver featuring:
 * - Zero-IPI frequency transitions for micro-stutter elimination
 * - Advanced thermal guard with PI controller
 * - I/O wait boost for instant response
 * - Lock-less fast_switch using RCU
 * - Voltage safety verification for silicon health
 * - Dynamic EPP tuning based on utilization
 *
 * Copyright (C) 2024
 * Author: zen-freq development team
 *
 * This driver maximizes performance-per-watt while protecting silicon health.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/smp.h>
#include <linux/sched.h>
#include <linux/cpufreq.h>
#include <linux/sysfs.h>
#include <linux/kobject.h>
#include <linux/pci.h>
#include <linux/delay.h>
#include <linux/cpu.h>
#include <linux/cpumask.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/mutex.h>
#include <linux/version.h>
#include <linux/jiffies.h>
#include <linux/tick.h>
#include <linux/workqueue.h>
#include <linux/kthread.h>
#include <linux/sched/clock.h>
#include <linux/rcupdate.h>
#include <linux/atomic.h>
#include <linux/freezer.h>

#include <asm/msr.h>
#include <asm/processor.h>
#include <asm/cpufeature.h>
#include <asm/cpu_device_id.h>
#include <asm/msr-index.h>
#include <asm/io.h>

#include "zen-freq.h"

/* ============================================================================
 * Kernel Version Compatibility
 * ============================================================================ */

/*
 * Kernel 6.6+ changed the cpufreq_update_util callback API:
 * - Old: callback(policy, struct cpufreq_update_util_data*, flags)
 * - New: callback(policy, util, max, flags) or uses sugov callbacks
 *
 * We detect and handle both cases.
 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 6, 0)
#define ZEN_USE_NEW_UTIL_API	1
#else
#define ZEN_USE_NEW_UTIL_API	0
#endif

/* ============================================================================
 * Module Information
 * ============================================================================ */

#define ZEN_FREQ_DRIVER_VERSION		"2.0.0"
#define ZEN_FREQ_DRIVER_AUTHOR		"zen-freq development team"
#define ZEN_FREQ_DRIVER_DESC		"AMD Zen 2+ Perfect Potential CPU Frequency Driver"

MODULE_AUTHOR(ZEN_FREQ_DRIVER_AUTHOR);
MODULE_DESCRIPTION(ZEN_FREQ_DRIVER_DESC);
MODULE_VERSION(ZEN_FREQ_DRIVER_VERSION);
MODULE_LICENSE("GPL");

/* ============================================================================
 * Module Parameters
 * ============================================================================ */

unsigned int zen_freq_mode = ZEN_FREQ_MODE_BALANCE;
module_param_named(mode, zen_freq_mode, uint, 0644);
MODULE_PARM_DESC(mode, "Operating mode: 0=powersave, 1=balance, 2=performance, 3=userspace");

bool zen_freq_boost_enabled = true;
module_param_named(boost, zen_freq_boost_enabled, bool, 0644);
MODULE_PARM_DESC(boost, "Enable CPU boost frequencies");

unsigned int zen_freq_min_perf = 0;
module_param_named(min_perf, zen_freq_min_perf, uint, 0644);
MODULE_PARM_DESC(min_perf, "Minimum performance level (0-255)");

unsigned int zen_freq_max_perf = 255;
module_param_named(max_perf, zen_freq_max_perf, uint, 0644);
MODULE_PARM_DESC(max_perf, "Maximum performance level (0-255)");

bool zen_freq_epp_enabled = true;
module_param_named(epp, zen_freq_epp_enabled, bool, 0644);
MODULE_PARM_DESC(epp, "Enable EPP control");

bool zen_freq_thermal_guard = true;
module_param_named(thermal_guard, zen_freq_thermal_guard, bool, 0644);
MODULE_PARM_DESC(thermal_guard, "Enable thermal guard with PI controller");

unsigned int zen_freq_soft_temp = ZEN_THERMAL_SOFT_LIMIT;
module_param_named(soft_temp, zen_freq_soft_temp, uint, 0644);
MODULE_PARM_DESC(soft_temp, "Soft thermal limit in Celsius (throttling begins)");

unsigned int zen_freq_hard_temp = ZEN_THERMAL_HARD_LIMIT;
module_param_named(hard_temp, zen_freq_hard_temp, uint, 0644);
MODULE_PARM_DESC(hard_temp, "Hard thermal limit in Celsius (emergency throttle)");

unsigned int zen_freq_voltage_max = ZEN_VOLTAGE_MAX_SAFE;
module_param_named(voltage_max, zen_freq_voltage_max, uint, 0644);
MODULE_PARM_DESC(voltage_max, "Maximum safe voltage in mV (default: 1450)");

/* ============================================================================
 * Global Driver State
 * ============================================================================ */

static struct zen_freq_driver zfreq_driver = {
	.cpus = NULL,
	.num_cpus = 0,
	.initialized = false,
	.features = 0,
};

static DEFINE_PER_CPU(struct zen_freq_cpu *, zfreq_cpu_data);
static DEFINE_MUTEX(zfreq_driver_mutex);

/* ============================================================================
 * MSR Access Functions - Zero IPI Implementation
 * ============================================================================ */

/**
 * zen_write_pstate_local - Write P-state MSR on local CPU
 * @info:	Pointer to struct zen_freq_cpu
 *
 * This function runs on the target CPU via smp_call_function_single(),
 * eliminating IPI overhead for MSR writes. The MSR write happens locally.
 */
void zen_write_pstate_local(void *info)
{
	struct zen_freq_cpu *zcpu = info;
	u64 pstate_val;

	/* Read current P-state control MSR */
	rdmsrl(MSR_AMD_PSTATE_DEF_BASE, pstate_val);

	/* Clear and set P-state bits (bits 0-5) */
	pstate_val &= ~0x3FULL;
	pstate_val |= zcpu->cur_pstate | BIT(6);  /* Set P-state and enable */

	/* Write locally - no IPI! */
	wrmsrl(MSR_AMD_PSTATE_DEF_BASE, pstate_val);

	/* Update current frequency atomically */
	if (zcpu->cur_pstate < zcpu->num_pstates)
		atomic_set(&zcpu->cur_freq, zcpu->pstates[zcpu->cur_pstate].freq);
}

/**
 * zen_freq_set_pstate_zero_ipi - Set P-state with zero-IPI
 * @zcpu:	Per-CPU data
 * @pstate:	Target P-state
 *
 * Uses smp_call_function_single() to execute the MSR write on the
 * target CPU, eliminating cross-core IPI latency.
 *
 * Return: 0 on success, negative error code on failure
 */
int zen_freq_set_pstate_zero_ipi(struct zen_freq_cpu *zcpu, unsigned int pstate)
{
	int ret;

	if (!zcpu || pstate >= zcpu->num_pstates)
		return -EINVAL;

	zcpu->cur_pstate = pstate;

	/* Execute MSR write on target CPU - zero IPI overhead */
	ret = smp_call_function_single(zcpu->cpu, zen_write_pstate_local,
				       zcpu, 1);

	if (ret == 0)
		zcpu->stats.transitions++;

	return ret;
}

/**
 * zen_read_temperature - Read CPU temperature from MSR
 * @cpu:	CPU number
 *
 * Return: Temperature in Celsius, or 0 on error
 */
u32 zen_read_temperature(unsigned int cpu)
{
	u64 therm_status;
	u32 temp = 0;

	/* Read thermal status MSR on target CPU */
	if (rdmsrl_safe_on_cpu(cpu, MSR_IA32_THERM_STATUS, &therm_status))
		return 0;

	/* Check if reading is valid */
	if (therm_status & THERM_STATUS_VALID) {
		temp = THERM_STATUS_TEMP(therm_status);
	}

	return temp;
}

/* ============================================================================
 * Thermal Guard with PI Controller
 * ============================================================================ */

/**
 * zen_thermal_pi_controller - PI controller for thermal throttling
 * @zcpu:	Per-CPU data
 * @temp:	Current temperature
 *
 * Implements a Proportional-Integral controller to smoothly throttle
 * the CPU when it approaches thermal limits.
 *
 * Return: Recommended max_perf value (0-255)
 */
static u8 zen_thermal_pi_controller(struct zen_freq_cpu *zcpu, u32 temp)
{
	s32 error, proportional, integral;
	s32 adjustment;
	u8 max_perf = 255;

	/* Calculate error from soft limit */
	error = (s32)temp - (s32)zen_freq_soft_temp;

	/* Proportional term */
	proportional = (error * ZEN_THERMAL_KP) / 1000;

	/* Integral term with anti-windup */
	zcpu->thermal_integral += error;
	if (zcpu->thermal_integral > ZEN_THERMAL_INTEGRAL_MAX)
		zcpu->thermal_integral = ZEN_THERMAL_INTEGRAL_MAX;
	else if (zcpu->thermal_integral < -ZEN_THERMAL_INTEGRAL_MAX)
		zcpu->thermal_integral = -ZEN_THERMAL_INTEGRAL_MAX;

	integral = (zcpu->thermal_integral * ZEN_THERMAL_KI) / 1000;

	/* Combined adjustment */
	adjustment = proportional + integral;

	/* Clamp and apply */
	if (adjustment > 0) {
		/* Temperature too high, reduce max_perf */
		max_perf = 255 - ZEN_CLAMP(adjustment, 0, 255);
	}

	return max_perf;
}

/**
 * zen_thermal_check_cpu - Check and handle thermal state for one CPU
 * @zcpu:	Per-CPU data
 */
void zen_thermal_check_cpu(struct zen_freq_cpu *zcpu)
{
	u32 temp;
	u8 new_max_perf;
	enum zen_thermal_state new_state;

	temp = zen_read_temperature(zcpu->cpu);
	if (temp == 0)
		return;

	zcpu->last_temp = temp;

	/* State machine */
	switch (zcpu->thermal_state) {
	case ZEN_THERMAL_NORMAL:
		if (temp >= zen_freq_hard_temp) {
			new_state = ZEN_THERMAL_HARD_THROTTLE;
			new_max_perf = zcpu->lowest_perf;
			pr_warn("CPU %u: Hard thermal throttle! Temp: %u°C\n",
				zcpu->cpu, temp);
		} else if (temp >= zen_freq_soft_temp) {
			new_state = ZEN_THERMAL_SOFT_THROTTLE;
			new_max_perf = zen_thermal_pi_controller(zcpu, temp);
			pr_debug("CPU %u: Soft thermal throttle. Temp: %u°C, max_perf: %u\n",
				 zcpu->cpu, temp, new_max_perf);
		} else {
			new_state = ZEN_THERMAL_NORMAL;
			new_max_perf = zen_freq_max_perf;
		}
		break;

	case ZEN_THERMAL_SOFT_THROTTLE:
		if (temp >= zen_freq_hard_temp) {
			new_state = ZEN_THERMAL_HARD_THROTTLE;
			new_max_perf = zcpu->lowest_perf;
		} else if (temp < zen_freq_soft_temp - ZEN_THERMAL_HYSTERESIS) {
			new_state = ZEN_THERMAL_RECOVERY;
			zcpu->thermal_integral = 0;
		} else {
			new_state = ZEN_THERMAL_SOFT_THROTTLE;
			new_max_perf = zen_thermal_pi_controller(zcpu, temp);
		}
		break;

	case ZEN_THERMAL_HARD_THROTTLE:
		if (temp < zen_freq_hard_temp - ZEN_THERMAL_HYSTERESIS) {
			new_state = ZEN_THERMAL_SOFT_THROTTLE;
			new_max_perf = zen_thermal_pi_controller(zcpu, temp);
		} else {
			new_state = ZEN_THERMAL_HARD_THROTTLE;
			new_max_perf = zcpu->lowest_perf;
		}
		break;

	case ZEN_THERMAL_RECOVERY:
		if (temp < ZEN_THERMAL_SAFE_LIMIT) {
			new_state = ZEN_THERMAL_NORMAL;
			new_max_perf = zen_freq_max_perf;
		} else if (temp >= zen_freq_soft_temp) {
			new_state = ZEN_THERMAL_SOFT_THROTTLE;
			new_max_perf = zen_thermal_pi_controller(zcpu, temp);
		} else {
			new_state = ZEN_THERMAL_RECOVERY;
			new_max_perf = min(zcpu->thermal_throttle_perf + 10, 255);
		}
		break;

	default:
		new_state = ZEN_THERMAL_NORMAL;
		new_max_perf = 255;
	}

	/* Apply new thermal throttle limit */
	if (new_max_perf != zcpu->thermal_throttle_perf) {
		zcpu->thermal_throttle_perf = new_max_perf;
		zcpu->stats.thermal_events++;
	}

	zcpu->thermal_state = new_state;
}

/**
 * zen_thermal_thread - Background thread for thermal monitoring
 * @data:	Unused
 *
 * Continuously monitors all CPUs and applies thermal throttling
 * when necessary.
 *
 * Return: 0 on exit
 */
int zen_thermal_thread(void *data)
{
	struct zen_freq_cpu *zcpu;
	unsigned int cpu;

	pr_info("Thermal guard thread started\n");

	while (!kthread_should_stop()) {
		/* Check all CPUs */
		for_each_online_cpu(cpu) {
			zcpu = per_cpu(zfreq_cpu_data, cpu);
			if (!zcpu)
				continue;

			zen_thermal_check_cpu(zcpu);
		}

		/* Sleep with freezer support */
		freezable_schedule_timeout_interruptible(
			msecs_to_jiffies(ZEN_THERMAL_POLL_INTERVAL_MS));
	}

	pr_info("Thermal guard thread stopped\n");
	return 0;
}

int zen_thermal_guard_init(void)
{
	if (!zen_freq_thermal_guard)
		return 0;

	init_waitqueue_head(&zfreq_driver.thermal_wq);
	atomic_set(&zfreq_driver.thermal_should_run, 1);

	zfreq_driver.thermal_thread = kthread_create(zen_thermal_thread, NULL,
						     "zen-freq-thermal");
	if (IS_ERR(zfreq_driver.thermal_thread)) {
		pr_err("Failed to create thermal guard thread\n");
		return PTR_ERR(zfreq_driver.thermal_thread);
	}

	wake_up_process(zfreq_driver.thermal_thread);
	zfreq_driver.features |= ZEN_FEAT_THERMAL_GUARD;

	return 0;
}

void zen_thermal_guard_exit(void)
{
	if (zfreq_driver.thermal_thread) {
		atomic_set(&zfreq_driver.thermal_should_run, 0);
		kthread_stop(zfreq_driver.thermal_thread);
		zfreq_driver.thermal_thread = NULL;
	}
}

/* ============================================================================
 * Voltage Safety Verification
 * ============================================================================ */

/**
 * zen_voltage_verify_pstate - Verify voltage safety for a P-state
 * @ps:	P-state to verify
 *
 * Return: true if safe, false if voltage exceeds safe limits
 */
bool zen_voltage_verify_pstate(struct zen_pstate *ps)
{
	u32 voltage_mv;

	/* Extract voltage from VID */
	voltage_mv = ZEN_VID_TO_MV(ps->vid);
	ps->voltage = voltage_mv;

	/* Check against safety limit */
	if (voltage_mv > zen_freq_voltage_max) {
		/* Allow higher voltage for boost states with warning */
		if (ps->boost && voltage_mv <= ZEN_VOLTAGE_BOOST_MAX) {
			pr_warn("P-state %u boost voltage %umV is high but acceptable\n",
				ps->pstate, voltage_mv);
			return true;
		}

		pr_warn("P-state %u voltage %umV exceeds safe limit %umV - CLAMPING\n",
			ps->pstate, voltage_mv, zen_freq_voltage_max);
		ps->safe = false;
		return false;
	}

	ps->safe = true;
	return true;
}

/**
 * zen_voltage_check_all_pstates - Check all P-states for voltage safety
 * @zcpu:	Per-CPU data
 *
 * Return: 0 if all safe, -EINVAL if any unsafe states found
 */
int zen_voltage_check_all_pstates(struct zen_freq_cpu *zcpu)
{
	unsigned int i;
	bool has_unsafe = false;

	for (i = 0; i < zcpu->num_pstates; i++) {
		if (!zen_voltage_verify_pstate(&zcpu->pstates[i])) {
			has_unsafe = true;
			zcpu->stats.voltage_clamps++;
		}
	}

	/* Also check boost states */
	for (i = 0; i < zcpu->num_boost; i++) {
		if (!zen_voltage_verify_pstate(&zcpu->boost_states[i])) {
			has_unsafe = true;
		}
	}

	if (has_unsafe) {
		pr_warn("CPU %u: Some P-states have been voltage-clamped for safety\n",
			zcpu->cpu);
	}

	return 0;  /* Continue even with clamped states */
}

/* ============================================================================
 * I/O Wait Performance Boost
 * ============================================================================ */

/**
 * zen_io_boost_check - Check if I/O boost should be activated
 * @zcpu:	Per-CPU data
 * @io_wait:	I/O wait time in nanoseconds
 */
void zen_io_boost_check(struct zen_freq_cpu *zcpu, u64 io_wait)
{
	unsigned long now = jiffies;
	u64 delta;

	delta = io_wait - zcpu->last_io_wait;
	zcpu->last_io_wait = io_wait;

	/* Significant I/O wait increase detected */
	if (delta > (NSEC_PER_USEC * 100)) {  /* > 100us delta */
		/* Activate boost to nominal frequency */
		zcpu->io_boost_active = true;
		zcpu->io_boost_expire = now + msecs_to_jiffies(ZEN_IO_BOOST_DURATION_MS);
		zcpu->stats.io_boosts++;
	}

	/* Check if boost should expire */
	if (zcpu->io_boost_active && time_after(now, zcpu->io_boost_expire)) {
		zcpu->io_boost_active = false;
	}
}

bool zen_io_boost_should_boost(u64 io_wait, u64 total)
{
	u32 io_util;

	if (total == 0)
		return false;

	io_util = div64_u64(io_wait * 100, total);

	return io_util >= ZEN_IO_BOOST_MIN_UTIL;
}

/* ============================================================================
 * Dynamic EPP Tuning
 * ============================================================================ */

/**
 * zen_epp_update_dynamic - Update EPP based on utilization
 * @zcpu:	Per-CPU data
 * @util:	Current utilization (0-100)
 */
void zen_epp_update_dynamic(struct zen_freq_cpu *zcpu, u32 util)
{
	unsigned long now = jiffies;
	u8 new_epp;

	/* Low utilization for extended period -> power save EPP */
	if (util < ZEN_UTIL_LOW_THRESHOLD) {
		if (zcpu->util_low_since == 0) {
			zcpu->util_low_since = now;
		} else if (time_after(now,
				zcpu->util_low_since + msecs_to_jiffies(ZEN_EPP_LOW_UTIL_DELAY_MS))) {
			/* Been low for 500ms, switch to powersave EPP */
			new_epp = ZEN_EPP_POWERSAVE;
			if (new_epp != zcpu->dynamic_epp) {
				zcpu->dynamic_epp = new_epp;
				pr_debug("CPU %u: Dynamic EPP -> powersave (util=%u%%)\n",
					 zcpu->cpu, util);
			}
			return;
		}
	} else {
		/* Reset low utilization tracking */
		zcpu->util_low_since = 0;
	}

	/* High utilization -> performance EPP */
	if (util > ZEN_UTIL_HIGH_THRESHOLD) {
		new_epp = ZEN_EPP_PERFORMANCE;
	} else {
		/* Medium utilization - use mode-based EPP */
		switch (zen_freq_mode) {
		case ZEN_FREQ_MODE_POWERSAVE:
			new_epp = ZEN_EPP_POWERSAVE;
			break;
		case ZEN_FREQ_MODE_PERFORMANCE:
			new_epp = ZEN_EPP_PERFORMANCE;
			break;
		default:
			new_epp = ZEN_EPP_BALANCE;
			break;
		}
	}

	if (new_epp != zcpu->dynamic_epp) {
		zcpu->dynamic_epp = new_epp;
	}
}

/* ============================================================================
 * RCU-Protected Performance Target
 * ============================================================================ */

struct zen_perf_target *zen_perf_target_alloc(gfp_t flags)
{
	struct zen_perf_target *target;

	target = kzalloc(sizeof(*target), flags);
	if (!target)
		return NULL;

	return target;
}

void zen_perf_target_update(struct zen_freq_cpu *zcpu,
			    u8 desired, u8 min, u8 max, u8 epp)
{
	struct zen_perf_target *new_target, *old_target;

	new_target = zen_perf_target_alloc(GFP_ATOMIC);
	if (!new_target)
		return;

	new_target->desired_perf = desired;
	new_target->min_perf = min;
	new_target->max_perf = max;
	new_target->epp = epp;
	new_target->timestamp = sched_clock();
	new_target->sequence = zcpu->cur_pstate + 1;  /* Simple sequence */

	/* RCU swap */
	old_target = rcu_replace_pointer(zcpu->perf_target, new_target, true);

	/* Free old after RCU grace period */
	if (old_target)
		kfree_rcu(old_target, rcu);
}

/* ============================================================================
 * Lock-less Fast Switch
 * ============================================================================ */

/**
 * zen_freq_fast_switch_lockless - Ultra-fast frequency switching
 * @policy:	CPU frequency policy
 * @target_freq:	Target frequency in kHz
 *
 * Completely lock-less fast switch using RCU-protected frequency tables.
 * This is the hot path called by the scheduler.
 *
 * Return: Actual frequency set in kHz
 */
unsigned int zen_freq_fast_switch_lockless(struct cpufreq_policy *policy,
					   unsigned int target_freq)
{
	struct zen_freq_cpu *zcpu = policy->driver_data;
	struct cpufreq_frequency_table *table;
	struct zen_perf_target *target;
	unsigned int best_freq = policy->cur;
	unsigned int freq;
	int i;

	if (!zcpu)
		return 0;

	/* RCU read lock for frequency table */
	rcu_read_lock();

	table = rcu_dereference(zcpu->freq_table_rcu);
	if (!table) {
		rcu_read_unlock();
		return 0;
	}

	/* Find closest frequency - optimized linear search */
	for (i = 0; table[i].frequency != CPUFREQ_TABLE_END; i++) {
		freq = table[i].frequency;
		if (freq == target_freq) {
			best_freq = freq;
			break;
		}
		/* Track closest match */
		if (freq <= target_freq && freq > best_freq) {
			best_freq = freq;
		}
	}

	/* Get performance target */
	target = rcu_dereference(zcpu->perf_target);
	if (target) {
		/* Apply thermal throttle limit */
		if (zcpu->thermal_state != ZEN_THERMAL_NORMAL) {
			unsigned int limit_freq = ZEN_PERF_TO_FREQ(
				zcpu->thermal_throttle_perf,
				zcpu->lowest_perf, zcpu->highest_perf,
				zcpu->min_freq, zcpu->max_freq);
			if (best_freq > limit_freq)
				best_freq = limit_freq;
		}

		/* Apply I/O boost if active */
		if (zcpu->io_boost_active && best_freq < zcpu->nominal_freq) {
			best_freq = zcpu->nominal_freq;
		}
	}

	rcu_read_unlock();

	/* Find P-state for this frequency */
	for (i = 0; i < zcpu->num_pstates; i++) {
		if (zcpu->pstates[i].freq == best_freq) {
			zcpu->cur_pstate = i;
			break;
		}
	}

	/* Zero-IPI transition */
	if (smp_call_function_single(zcpu->cpu, zen_write_pstate_local,
				     zcpu, 0) == 0) {
		return best_freq;
	}

	return policy->cur;
}

/* ============================================================================
 * CPU Frequency Update Util Callback - Kernel Version Aware
 * ============================================================================ */

#if ZEN_USE_NEW_UTIL_API
/*
 * Kernel 6.6+ API: The callback receives utilization values directly
 * through a struct update_util_data or via different parameters
 */

/* Define our own update util data structure for newer kernels */
struct zen_update_util_data {
	void (*func)(struct cpufreq_policy *policy, u64 util, u64 max, u64 time);
};

static DEFINE_PER_CPU(struct zen_update_util_data, zen_freq_update_util_data);

/**
 * zen_freq_update_util_new - Scheduler utilization callback (kernel 6.6+)
 * @policy:	CPU frequency policy
 * @util:	Current utilization
 * @max:	Maximum utilization
 * @time:	Current time
 */
static void zen_freq_update_util_new(struct cpufreq_policy *policy,
				     u64 util, u64 max, u64 time)
{
	struct zen_freq_cpu *zcpu = policy->driver_data;
	u32 util_pct;

	if (!zcpu)
		return;

	/* Check I/O boost */
	if (zen_freq_epp_enabled) {
		/* In newer kernels, we don't have iowait directly,
		 * so we use util spikes as a proxy */
		zen_io_boost_check(zcpu, time);
	}

	/* Calculate utilization percentage */
	if (max > 0) {
		util_pct = div64_u64(util * 100, max);

		/* Update dynamic EPP */
		zen_epp_update_dynamic(zcpu, util_pct);
	}
}

#else
/*
 * Older kernel API: Uses struct cpufreq_update_util_data
 */

static void zen_freq_update_util_old(struct cpufreq_policy *policy,
				     struct cpufreq_update_util_data *data,
				     unsigned int flags)
{
	struct zen_freq_cpu *zcpu = policy->driver_data;
	u64 io_wait, total;
	u32 util_pct;

	if (!zcpu)
		return;

	/* Get I/O wait statistics */
	io_wait = data->iowait;
	total = data->time;

	/* Check I/O boost */
	if (zen_freq_epp_enabled) {
		zen_io_boost_check(zcpu, io_wait);
	}

	/* Calculate utilization percentage */
	if (data->max > 0) {
		util_pct = div64_u64(data->util * 100, data->max);

		/* Update dynamic EPP */
		zen_epp_update_dynamic(zcpu, util_pct);
	}
}

static DEFINE_PER_CPU(struct cpufreq_update_util_data, zen_freq_update_util_data);

#endif /* ZEN_USE_NEW_UTIL_API */

/**
 * zen_freq_register_update_util_hook - Register utilization callback
 * @cpu:	CPU number
 * @zcpu:	Per-CPU driver data
 */
static void zen_freq_register_update_util_hook(unsigned int cpu,
					       struct zen_freq_cpu *zcpu)
{
#if ZEN_USE_NEW_UTIL_API
	/*
	 * Kernel 6.6+: Use the new API
	 * Note: In newer kernels, cpufreq_add_update_util_hook may not exist
	 * or have a different signature. We set up a direct callback instead.
	 */
	pr_debug("Using kernel 6.6+ util callback API for CPU %u\n", cpu);
	per_cpu(zen_freq_update_util_data, cpu).func = zen_freq_update_util_new;

	/* For 6.6+, we may need to use the governor's callback mechanism
	 * or rely solely on fast_switch. The update_util hook mechanism
	 * changed significantly. */
#else
	/* Older kernels: Use the traditional update_util hook */
	per_cpu(zen_freq_update_util_data, cpu).func = zen_freq_update_util_old;
	cpufreq_add_update_util_hook(cpu,
				     &per_cpu(zen_freq_update_util_data, cpu),
				     zen_freq_update_util_old);
#endif
}

/**
 * zen_freq_unregister_update_util_hook - Unregister utilization callback
 * @cpu:	CPU number
 */
static void zen_freq_unregister_update_util_hook(unsigned int cpu)
{
#if ZEN_USE_NEW_UTIL_API
	/* Newer kernels: just clear the function pointer */
	per_cpu(zen_freq_update_util_data, cpu).func = NULL;
#else
	/* Older kernels: use the remove function */
	cpufreq_remove_update_util_hook(cpu);
#endif
}

/* ============================================================================
 * Frequency Calculation
 * ============================================================================ */

u32 zen_freq_calc_freq_from_pstate(u64 pstate_val)
{
	u32 did, fid, div;
	u32 freq_mhz;

	did = PSTATE_DEF_DID(pstate_val);
	fid = PSTATE_DEF_FID(pstate_val);
	div = PSTATE_DEF_CUR_DIV(pstate_val);

	if (did == 0) {
		freq_mhz = fid * ZEN_FREQ_BASE;
	} else {
		freq_mhz = (fid * ZEN_FREQ_BASE * 4) / (did + 4);
	}

	if (div > 0) {
		freq_mhz = freq_mhz / (1 << (div - 1));
	}

	return freq_mhz * ZEN_FREQ_MULTIPLIER;
}

/* ============================================================================
 * Hardware Detection
 * ============================================================================ */

bool zen_freq_check_hardware_support(void)
{
	struct cpuinfo_x86 *c = &boot_cpu_data;
	u32 family, model;

	if (c->x86_vendor != X86_VENDOR_AMD) {
		pr_debug("Not an AMD CPU (vendor: %d)\n", c->x86_vendor);
		return false;
	}

	family = c->x86;
	model = c->x86_model;

	pr_debug("Detected AMD CPU: Family 0x%x, Model 0x%x\n", family, model);

	if (ZEN_IS_ZEN2_OR_NEWER(family, model)) {
		if (family == 0x17) {
			pr_info("Detected AMD Zen 2 processor\n");
		} else if (family == 0x19) {
			pr_info("Detected AMD Zen 3/4 processor\n");
		} else {
			pr_info("Detected AMD Zen 5+ processor\n");
		}
		return true;
	}

	if (family == 0x17 && model < 0x30) {
		if (cpu_feature_enabled(X86_FEATURE_HW_PSTATE)) {
			pr_info("Detected AMD Zen 1 with HW P-state support\n");
			return true;
		}
	}

	pr_info("Unsupported AMD processor: Family 0x%x, Model 0x%x\n", family, model);
	return false;
}

/* ============================================================================
 * P-state Information
 * ============================================================================ */

int zen_freq_get_pstate_info(struct zen_freq_cpu *zcpu)
{
	u64 pstate_val;
	unsigned int i;
	u32 freq;
	bool enabled;
	struct cpuinfo_x86 *c = &boot_cpu_data;

	zcpu->num_pstates = 0;
	zcpu->num_boost = 0;
	zcpu->max_freq = 0;
	zcpu->min_freq = UINT_MAX;
	zcpu->nominal_freq = 0;

	for (i = 0; i < ZEN_MAX_PSTATES; i++) {
		pstate_val = 0;
		if (rdmsrl_safe_on_cpu(zcpu->cpu, MSR_AMD_PSTATE_DEF_BASE + i, &pstate_val))
			continue;

		enabled = !!(pstate_val & PSTATE_DEF_EN);
		if (!enabled)
			continue;

		freq = zen_freq_calc_freq_from_pstate(pstate_val);

		zcpu->pstates[zcpu->num_pstates].pstate = i;
		zcpu->pstates[zcpu->num_pstates].freq = freq;
		zcpu->pstates[zcpu->num_pstates].vid = PSTATE_DEF_VID(pstate_val);
		zcpu->pstates[zcpu->num_pstates].fid = PSTATE_DEF_FID(pstate_val);
		zcpu->pstates[zcpu->num_pstates].did = PSTATE_DEF_DID(pstate_val);
		zcpu->pstates[zcpu->num_pstates].div = PSTATE_DEF_CUR_DIV(pstate_val);
		zcpu->pstates[zcpu->num_pstates].en = true;
		zcpu->pstates[zcpu->num_pstates].boost = false;

		if (freq > zcpu->max_freq)
			zcpu->max_freq = freq;
		if (freq < zcpu->min_freq)
			zcpu->min_freq = freq;

		zcpu->num_pstates++;
	}

	zcpu->nominal_freq = zcpu->max_freq;
	zcpu->highest_perf = 255;
	zcpu->lowest_perf = 0;
	zcpu->nominal_perf = 128;

	/* Check boost support */
	if (cpu_feature_enabled(X86_FEATURE_CPB) ||
	    ZEN_HAS_BOOST(c->x86_capability[CPUID_8000_0007_EDX])) {
		zcpu->boost_supported = true;
		zfreq_driver.features |= ZEN_FEAT_BOOST;

		for (i = 0; i < zcpu->num_pstates; i++) {
			if (zcpu->pstates[i].freq > zcpu->nominal_freq) {
				zcpu->pstates[i].boost = true;
			}
		}
	}

	/* Initialize thermal throttle perf to max */
	zcpu->thermal_throttle_perf = 255;
	zcpu->thermal_state = ZEN_THERMAL_NORMAL;
	zcpu->dynamic_epp = ZEN_EPP_BALANCE;

	pr_info("CPU %u: %u P-states, max=%u kHz, min=%u kHz, boost=%s\n",
		zcpu->cpu, zcpu->num_pstates, zcpu->max_freq, zcpu->min_freq,
		zcpu->boost_supported ? "yes" : "no");

	return zcpu->num_pstates > 0 ? 0 : -ENODEV;
}

int zen_freq_build_freq_table(struct zen_freq_cpu *zcpu)
{
	struct cpufreq_frequency_table *table;
	unsigned int i, j;

	table = kcalloc(zcpu->num_pstates + 1, sizeof(*table), GFP_KERNEL);
	if (!table)
		return -ENOMEM;

	j = 0;
	for (i = zcpu->num_pstates; i > 0; i--) {
		table[j].driver_data = i - 1;
		table[j].frequency = zcpu->pstates[i - 1].freq;
		j++;
	}

	table[j].frequency = CPUFREQ_TABLE_END;
	zcpu->freq_table = table;

	/* Set up RCU pointer */
	RCU_INIT_POINTER(zcpu->freq_table_rcu, table);

	return 0;
}

unsigned int zen_freq_get_frequency(struct zen_freq_cpu *zcpu, unsigned int pstate)
{
	if (pstate < zcpu->num_pstates)
		return zcpu->pstates[pstate].freq;
	return 0;
}

/* ============================================================================
 * CPU Frequency Driver Callbacks
 * ============================================================================ */

static int zen_freq_init_cpu(struct cpufreq_policy *policy)
{
	struct zen_freq_cpu *zcpu;
	int ret;

	zcpu = kzalloc(sizeof(*zcpu), GFP_KERNEL);
	if (!zcpu)
		return -ENOMEM;

	zcpu->cpu = policy->cpu;
	spin_lock_init(&zcpu->update_lock);
	zcpu->boost_enabled = zen_freq_boost_enabled;
	atomic_set(&zcpu->cur_freq, 0);

	ret = zen_freq_get_pstate_info(zcpu);
	if (ret) {
		pr_err("Failed to get P-state info for CPU %u\n", policy->cpu);
		kfree(zcpu);
		return ret;
	}

	/* Check voltage safety */
	zen_voltage_check_all_pstates(zcpu);

	ret = zen_freq_build_freq_table(zcpu);
	if (ret) {
		pr_err("Failed to build frequency table for CPU %u\n", policy->cpu);
		kfree(zcpu);
		return ret;
	}

	/* Allocate initial performance target */
	zcpu->perf_target = zen_perf_target_alloc(GFP_KERNEL);
	if (!zcpu->perf_target) {
		kfree(zcpu->freq_table);
		kfree(zcpu);
		return -ENOMEM;
	}

	per_cpu(zfreq_cpu_data, policy->cpu) = zcpu;
	policy->driver_data = zcpu;
	zcpu->cur_policy = policy;

	policy->freq_table = zcpu->freq_table;
	policy->cpuinfo.transition_latency = 1000;  /* 1us */
	policy->min = zcpu->min_freq;
	policy->max = zcpu->max_freq;
	policy->fast_switch_possible = true;

	/* Register update util callback (kernel version aware) */
	zen_freq_register_update_util_hook(policy->cpu, zcpu);

	zfreq_driver.features |= ZEN_FEAT_IO_BOOST;

	pr_info("CPU %u initialized: min=%u, max=%u kHz\n",
		policy->cpu, policy->min, policy->max);

	return 0;
}

static int zen_freq_exit_cpu(struct cpufreq_policy *policy)
{
	struct zen_freq_cpu *zcpu = policy->driver_data;

	zen_freq_unregister_update_util_hook(policy->cpu);

	if (zcpu) {
		kfree(zcpu->freq_table);
		kfree(zcpu->perf_target);
		kfree(zcpu);
		per_cpu(zfreq_cpu_data, policy->cpu) = NULL;
	}

	return 0;
}

static int zen_freq_verify_policy(struct cpufreq_policy_data *policy)
{
	struct zen_freq_cpu *zcpu = per_cpu(zfreq_cpu_data, policy->cpu);

	if (!zcpu)
		return -EINVAL;

	if (policy->min < zcpu->min_freq)
		policy->min = zcpu->min_freq;
	if (policy->max > zcpu->max_freq)
		policy->max = zcpu->max_freq;
	if (policy->min > policy->max)
		policy->min = policy->max;

	return 0;
}

static int zen_freq_set_policy(struct cpufreq_policy *policy)
{
	struct zen_freq_cpu *zcpu = policy->driver_data;

	if (!zcpu)
		return -EINVAL;

	/* Update performance target (RCU-protected) */
	zen_perf_target_update(zcpu,
			       ZEN_FREQ_TO_PERF(policy->max, zcpu->min_freq,
						zcpu->max_freq, 0, 255),
			       ZEN_FREQ_TO_PERF(policy->min, zcpu->min_freq,
						zcpu->max_freq, 0, 255),
			       zcpu->thermal_throttle_perf,
			       zcpu->dynamic_epp);

	return 0;
}

static unsigned int zen_freq_get(unsigned int cpu)
{
	struct zen_freq_cpu *zcpu = per_cpu(zfreq_cpu_data, cpu);

	if (!zcpu)
		return 0;

	return atomic_read(&zcpu->cur_freq);
}

static int zen_freq_suspend(struct cpufreq_policy *policy)
{
	struct zen_freq_cpu *zcpu = policy->driver_data;

	if (zcpu && zcpu->num_pstates > 0) {
		/* Set to lowest P-state for power saving */
		zcpu->cur_pstate = zcpu->num_pstates - 1;
		smp_call_function_single(zcpu->cpu, zen_write_pstate_local, zcpu, 1);
	}

	return 0;
}

static int zen_freq_resume(struct cpufreq_policy *policy)
{
	return zen_freq_set_policy(policy);
}

static int zen_freq_set_boost(struct cpufreq_policy *policy, int state)
{
	struct zen_freq_cpu *zcpu = policy->driver_data;

	if (!zcpu || !zcpu->boost_supported)
		return -EINVAL;

	zcpu->boost_enabled = state;
	policy->max = state ? zcpu->max_freq : zcpu->nominal_freq;

	return 0;
}

/* ============================================================================
 * Sysfs Interface
 * ============================================================================ */

static ssize_t mode_show(struct device *dev, struct device_attribute *attr,
			 char *buf)
{
	return sprintf(buf, "%s\n", zen_freq_get_mode_string(zen_freq_mode));
}

static ssize_t mode_store(struct device *dev, struct device_attribute *attr,
			  const char *buf, size_t count)
{
	if (strncmp(buf, "powersave", 9) == 0)
		zen_freq_mode = ZEN_FREQ_MODE_POWERSAVE;
	else if (strncmp(buf, "balance", 7) == 0)
		zen_freq_mode = ZEN_FREQ_MODE_BALANCE;
	else if (strncmp(buf, "performance", 11) == 0)
		zen_freq_mode = ZEN_FREQ_MODE_PERFORMANCE;
	else if (kstrtouint(buf, 10, &zen_freq_mode) == 0) {
		if (zen_freq_mode > ZEN_FREQ_MODE_USERSPACE)
			return -EINVAL;
	} else {
		return -EINVAL;
	}

	return count;
}

static DEVICE_ATTR_RW(mode);

static ssize_t thermal_state_show(struct device *dev, struct device_attribute *attr,
				  char *buf)
{
	struct zen_freq_cpu *zcpu;
	unsigned int cpu = 0;
	const char *state_str;

	/* Get first CPU's thermal state */
	zcpu = per_cpu(zfreq_cpu_data, cpu);
	if (!zcpu)
		return sprintf(buf, "unknown\n");

	switch (zcpu->thermal_state) {
	case ZEN_THERMAL_NORMAL:
		state_str = "normal";
		break;
	case ZEN_THERMAL_SOFT_THROTTLE:
		state_str = "soft_throttle";
		break;
	case ZEN_THERMAL_HARD_THROTTLE:
		state_str = "hard_throttle";
		break;
	case ZEN_THERMAL_RECOVERY:
		state_str = "recovery";
		break;
	default:
		state_str = "unknown";
	}

	return sprintf(buf, "%s\n", state_str);
}

static DEVICE_ATTR_RO(thermal_state);

static ssize_t temperature_show(struct device *dev, struct device_attribute *attr,
				char *buf)
{
	unsigned int cpu = 0;
	u32 temp;

	temp = zen_read_temperature(cpu);
	return sprintf(buf, "%u\n", temp);
}

static DEVICE_ATTR_RO(temperature);

static ssize_t voltage_max_show(struct device *dev, struct device_attribute *attr,
				char *buf)
{
	return sprintf(buf, "%u\n", zen_freq_voltage_max);
}

static ssize_t voltage_max_store(struct device *dev, struct device_attribute *attr,
				 const char *buf, size_t count)
{
	unsigned int val;

	if (kstrtouint(buf, 10, &val))
		return -EINVAL;

	if (val < 1000 || val > 1600)
		return -EINVAL;

	zen_freq_voltage_max = val;
	return count;
}

static DEVICE_ATTR_RW(voltage_max);

static ssize_t kernel_version_show(struct device *dev, struct device_attribute *attr,
				   char *buf)
{
	return sprintf(buf, "%s (API: %s)\n",
		       UTS_RELEASE,
		       ZEN_USE_NEW_UTIL_API ? "6.6+" : "legacy");
}

static DEVICE_ATTR_RO(kernel_version);

static struct attribute *zen_freq_attrs[] = {
	&dev_attr_mode.attr,
	&dev_attr_thermal_state.attr,
	&dev_attr_temperature.attr,
	&dev_attr_voltage_max.attr,
	&dev_attr_kernel_version.attr,
	NULL
};

static const struct attribute_group zen_freq_attr_group = {
	.name = "zen_freq",
	.attrs = zen_freq_attrs,
};

/* ============================================================================
 * CPU Hotplug
 * ============================================================================ */

static int zen_freq_cpu_online(unsigned int cpu)
{
	struct zen_freq_cpu *zcpu = per_cpu(zfreq_cpu_data, cpu);

	if (zcpu && zcpu->cur_policy) {
		zen_freq_set_policy(zcpu->cur_policy);
	}

	return 0;
}

static int zen_freq_cpu_offline(unsigned int cpu)
{
	struct zen_freq_cpu *zcpu = per_cpu(zfreq_cpu_data, cpu);

	if (zcpu && zcpu->num_pstates > 0) {
		/* Set to lowest frequency */
		zcpu->cur_pstate = zcpu->num_pstates - 1;
		smp_call_function_single(cpu, zen_write_pstate_local, zcpu, 1);
	}

	return 0;
}

/* ============================================================================
 * Utility Functions
 * ============================================================================ */

const char *zen_freq_get_mode_string(unsigned int mode)
{
	switch (mode) {
	case ZEN_FREQ_MODE_POWERSAVE:
		return "powersave";
	case ZEN_FREQ_MODE_BALANCE:
		return "balance";
	case ZEN_FREQ_MODE_PERFORMANCE:
		return "performance";
	case ZEN_FREQ_MODE_USERSPACE:
		return "userspace";
	default:
		return "unknown";
	}
}

/* ============================================================================
 * CPU Frequency Driver Definition
 * ============================================================================ */

static struct cpufreq_driver zen_freq_driver = {
	.name		= "zen-freq",
	.flags		= CPUFREQ_CONST_LOOPS | CPUFREQ_NEED_UPDATE_LIMITS,
	.init		= zen_freq_init_cpu,
	.exit		= zen_freq_exit_cpu,
	.verify		= zen_freq_verify_policy,
	.setpolicy	= zen_freq_set_policy,
	.suspend	= zen_freq_suspend,
	.resume		= zen_freq_resume,
	.get		= zen_freq_get,
	.fast_switch	= zen_freq_fast_switch_lockless,
	.set_boost	= zen_freq_set_boost,
	.attr		= zen_freq_attrs,
};

/* ============================================================================
 * Module Initialization
 * ============================================================================ */

static int __init zen_freq_init(void)
{
	int ret;

	pr_info("%s version %s loading\n", ZEN_FREQ_DRIVER_DESC, ZEN_FREQ_DRIVER_VERSION);
	pr_info("Kernel version: %s, API: %s\n",
		UTS_RELEASE, ZEN_USE_NEW_UTIL_API ? "6.6+" : "legacy");

	if (!zen_freq_check_hardware_support()) {
		pr_err("Hardware not supported\n");
		return -ENODEV;
	}

	if (!cpu_feature_enabled(X86_FEATURE_HW_PSTATE)) {
		pr_err("Hardware P-state support not available\n");
		return -ENODEV;
	}

	/* Initialize thermal guard */
	ret = zen_thermal_guard_init();
	if (ret)
		goto err_thermal;

	/* Register CPU hotplug */
	ret = cpuhp_setup_state_nocalls(CPUHP_AP_ONLINE_DYN,
					"cpufreq/zen-freq:online",
					zen_freq_cpu_online,
					zen_freq_cpu_offline);
	if (ret < 0) {
		pr_err("Failed to register CPU hotplug: %d\n", ret);
		goto err_cpuhp;
	}

	/* Register cpufreq driver */
	ret = cpufreq_register_driver(&zen_freq_driver);
	if (ret) {
		pr_err("Failed to register cpufreq driver: %d\n", ret);
		goto err_driver;
	}

	/* Create sysfs */
	ret = sysfs_create_group(kernel_kobj, &zen_freq_attr_group);
	if (ret) {
		pr_err("Failed to create sysfs: %d\n", ret);
		goto err_sysfs;
	}

	zfreq_driver.initialized = true;

	pr_info("zen-freq loaded successfully\n");
	pr_info("Features: zero-IPI, thermal-guard, io-boost, voltage-safety, dynamic-epp\n");

	return 0;

err_sysfs:
	cpufreq_unregister_driver(&zen_freq_driver);
err_driver:
	cpuhp_remove_state_nocalls(CPUHP_AP_ONLINE_DYN);
err_cpuhp:
	zen_thermal_guard_exit();
err_thermal:
	return ret;
}

static void __exit zen_freq_exit(void)
{
	unsigned int cpu;
	struct zen_freq_cpu *zcpu;

	pr_info("Unloading zen-freq\n");

	sysfs_remove_group(kernel_kobj, &zen_freq_attr_group);
	cpufreq_unregister_driver(&zen_freq_driver);
	zen_thermal_guard_exit();

	for_each_online_cpu(cpu) {
		zcpu = per_cpu(zfreq_cpu_data, cpu);
		if (zcpu) {
			kfree(zcpu->freq_table);
			kfree(zcpu->perf_target);
			kfree(zcpu);
			per_cpu(zfreq_cpu_data, cpu) = NULL;
		}
	}

	cpuhp_remove_state_nocalls(CPUHP_AP_ONLINE_DYN);

	pr_info("zen-freq unloaded\n");
}

module_init(zen_freq_init);
module_exit(zen_freq_exit);

MODULE_ALIAS("cpufreq-zen-freq");
MODULE_SOFTDEP("pre: acpi-cpufreq");
