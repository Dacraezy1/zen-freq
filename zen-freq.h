/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * zen-freq-perfect.h - AMD Zen 2+ Perfect Potential CPU Frequency Driver Header
 *
 * An advanced frequency scaling driver for AMD Zen 2+ processors featuring:
 * - Zero-IPI frequency transitions for micro-stutter elimination
 * - Advanced thermal guard with PI controller
 * - I/O wait boost for instant response
 * - Lock-less fast_switch using RCU
 * - Voltage safety verification for silicon health
 * - Dynamic EPP tuning based on utilization
 *
 * Copyright (C) 2024
 * Author: zen-freq development team
 */

#ifndef _ZEN_FREQ_PERFECT_H
#define _ZEN_FREQ_PERFECT_H

#include <linux/types.h>
#include <linux/cpufreq.h>
#include <linux/spinlock.h>
#include <linux/mutex.h>
#include <linux/atomic.h>
#include <linux/rcupdate.h>
#include <linux/kthread.h>
#include <linux/delay.h>
#include <linux/workqueue.h>
#include <linux/cpumask.h>
#include <linux/jiffies.h>
#include <linux/timer.h>
#include <linux/bitops.h>
#include <linux/ktime.h>

/* ============================================================================
 * AMD Zen Architecture MSR Definitions
 * ============================================================================ */

/* P-state related MSRs */
#define MSR_AMD_PSTATE_DEF_BASE         0xC0010063
#define MSR_AMD_PSTATE_STATUS           0xC0010063
#define MSR_AMD_PSTATE_ENABLE           0xC0010064
#define MSR_AMD_PSTATE_CPPC_REQ         0xC0010068
#define MSR_AMD_PSTATE_ACTUAL_PERF      0xC0010083
#define MSR_AMD_PSTATE_HW_PSTATE        0xC0010015

/* Thermal MSRs */
#define MSR_IA32_THERM_STATUS           0x0000019C
#define MSR_IA32_TEMPERATURE_TARGET     0x000001A2
#define MSR_AMD_HW_THERMTRIP_STATUS     0xC0010064

/* Voltage/Frequency MSRs */
#define MSR_AMD_PSTATE_CUR_LIMIT        0xC0010061
#define MSR_AMD_CPPC_BOOST              0xC0010293
#define MSR_AMD_HW_CRBOOST_ON           0xC0011006

/* P-state definition MSR bit fields */
#define PSTATE_DEF_EN                   BIT_ULL(63)
#define PSTATE_DEF_PSTATE(val)          ((val) & 0x3F)
#define PSTATE_DEF_DID(val)             (((val) >> 6) & 0x1F)
#define PSTATE_DEF_FID(val)             ((val) & 0x3F)
#define PSTATE_DEF_VID(val)             (((val) >> 11) & 0xFF)
#define PSTATE_DEF_CUR_DIV(val)         (((val) >> 4) & 0x3)

/* Thermal status MSR bit fields */
#define THERM_STATUS_VALID              BIT_ULL(31)
#define THERM_STATUS_TEMP(val)          (((val) >> 16) & 0x7F)
#define THERM_STATUS_LOG                BIT_ULL(1)
#define THERM_STATUS_PROCHOT            BIT_ULL(0)

/* CPPC request MSR bit fields */
#define CPPC_MAX_PERF(val)              ((val) & 0xFF)
#define CPPC_MIN_PERF(val)              (((val) & 0xFF) << 8)
#define CPPC_DES_PERF(val)              (((val) & 0xFF) << 16)
#define CPPC_EPP(val)                   (((val) & 0xFF) << 24)

/* Extractors for CPPC */
#define CPPC_MAX_PERF_GET(val)          ((val) & 0xFF)
#define CPPC_MIN_PERF_GET(val)          (((val) >> 8) & 0xFF)
#define CPPC_DES_PERF_GET(val)          (((val) >> 16) & 0xFF)
#define CPPC_EPP_GET(val)               (((val) >> 24) & 0xFF)

/* Maximum hardware P-states */
#define ZEN_MAX_PSTATES                 8
#define ZEN_MAX_BOOST_STATES            4

/* ============================================================================
 * Thermal Guard Configuration
 * ============================================================================ */

/* Thermal limits in degrees Celsius */
#define ZEN_THERMAL_SOFT_LIMIT          80      /* Begin throttling */
#define ZEN_THERMAL_HARD_LIMIT          90      /* Emergency throttle */
#define ZEN_THERMAL_HYSTERESIS          3       /* Hysteresis for recovery */
#define ZEN_THERMAL_SAFE_LIMIT          75      /* Fully recover */

/* PI Controller coefficients (scaled by 1000) */
#define ZEN_THERMAL_KP                  50      /* Proportional gain */
#define ZEN_THERMAL_KI                  10      /* Integral gain */
#define ZEN_THERMAL_INTEGRAL_MAX        1000    /* Anti-windup limit */

/* Thermal polling interval in milliseconds */
#define ZEN_THERMAL_POLL_INTERVAL_MS    250

/* Thermal state machine states */
enum zen_thermal_state {
        ZEN_THERMAL_NORMAL,             /* Normal operation */
        ZEN_THERMAL_SOFT_THROTTLE,      /* Gradual throttling active */
        ZEN_THERMAL_HARD_THROTTLE,      /* Emergency throttling active */
        ZEN_THERMAL_RECOVERY,           /* Recovering from throttling */
};

/* ============================================================================
 * Voltage Safety Configuration
 * ============================================================================ */

/* Voltage limits in millivolts */
#define ZEN_VOLTAGE_MAX_SAFE            1450    /* 1.45V absolute max for Zen */
#define ZEN_VOLTAGE_WARN                1350    /* Warning threshold */
#define ZEN_VOLTAGE_BOOST_MAX           1500    /* Boost voltage allowed max */

/* VID to voltage conversion (simplified for Zen architecture)
 * Actual formula varies by CPU generation, these are conservative
 */
#define ZEN_VID_TO_MV(vid)              (1550 - ((vid) * 25))   /* Approximate */

/* ============================================================================
 * EPP Dynamic Tuning Configuration
 * ============================================================================ */

/* EPP values */
#define ZEN_EPP_POWERSAVE               0xFF
#define ZEN_EPP_BALANCE_POWERSAVE       0xBF
#define ZEN_EPP_BALANCE                 0x80
#define ZEN_EPP_BALANCE_PERFORMANCE     0x40
#define ZEN_EPP_PERFORMANCE             0x00

/* Utilization thresholds */
#define ZEN_UTIL_LOW_THRESHOLD          10      /* % utilization */
#define ZEN_UTIL_HIGH_THRESHOLD         80      /* % utilization */
#define ZEN_EPP_LOW_UTIL_DELAY_MS       500     /* Delay before EPP change */

/* ============================================================================
 * I/O Wait Boost Configuration
 * ============================================================================ */

#define ZEN_IO_BOOST_DURATION_MS        50      /* How long to hold boost */
#define ZEN_IO_BOOST_MIN_UTIL           5       /* Minimum I/O wait util */
#define ZEN_IO_BOOST_HOLD_MS            20      /* Minimum hold time */

/* ============================================================================
 * Performance Target Cache (RCU-protected)
 * ============================================================================ */

/**
 * struct zen_perf_target - RCU-protected performance target
 * @desired_perf:       Target performance (0-255)
 * @min_perf:           Minimum performance limit
 * @max_perf:           Maximum performance limit
 * @epp:                Energy Performance Preference
 * @timestamp:          Last update timestamp (ns)
 * @sequence:           Sequence number for consistency
 * @rcu:                RCU head for safe reclamation
 */
struct zen_perf_target {
        u8              desired_perf;
        u8              min_perf;
        u8              max_perf;
        u8              epp;
        u64             timestamp;
        unsigned int    sequence;
        struct rcu_head rcu;
};

/* ============================================================================
 * Hardware P-state Structure
 * ============================================================================ */

/**
 * struct zen_pstate - Hardware P-state representation
 * @pstate:     P-state number (0 is highest performance)
 * @freq:       Frequency in kHz
 * @voltage:    Voltage in mV (calculated from VID)
 * @vid:        Voltage ID from MSR
 * @fid:        Frequency ID
 * @did:        Divisor ID
 * @div:        Divider value
 * @en:         Whether this P-state is enabled
 * @boost:      Whether this is a boost state
 * @safe:       Whether voltage is within safe limits
 */
struct zen_pstate {
        u8              pstate;
        u32             freq;
        u32             voltage;
        u8              vid;
        u8              fid;
        u8              did;
        u8              div;
        bool            en;
        bool            boost;
        bool            safe;
};

/* ============================================================================
 * Per-CPU Driver Data
 * ============================================================================ */

/**
 * struct zen_freq_cpu - Per-CPU private data
 * @cpu:                CPU number
 * @pstates:            Array of hardware P-states
 * @num_pstates:        Number of valid P-states
 * @boost_states:       Array of boost P-states
 * @num_boost:          Number of boost states
 * @max_freq:           Maximum non-boost frequency
 * @min_freq:           Minimum frequency
 * @nominal_freq:       Nominal (guaranteed) frequency
 * @highest_perf:       Highest performance value
 * @lowest_perf:        Lowest performance value
 * @nominal_perf:       Nominal performance value
 *
 * @cur_pstate:         Current P-state index
 * @cur_freq:           Current frequency (atomic for fast access)
 * @cur_policy:         Current cpufreq policy
 *
 * @perf_target:        RCU-protected performance target
 * @update_lock:        Lock for frequency updates (rarely used)
 *
 * @freq_table:         CPU frequency table
 * @freq_table_rcu:     RCU pointer to frequency table
 *
 * @thermal_state:      Current thermal state
 * @thermal_integral:   PI controller integral term
 * @thermal_throttle_perf: Current thermal throttle limit
 * @last_temp:          Last measured temperature
 *
 * @io_boost_active:    Whether I/O boost is active
 * @io_boost_expire:    When I/O boost expires (jiffies)
 * @last_io_wait:       Last I/O wait time
 *
 * @util_low_since:     When utilization went low (jiffies)
 * @dynamic_epp:        Current dynamic EPP value
 * @epp_mode:           User-configured EPP mode
 *
 * @stats:              Performance statistics
 */
struct zen_freq_cpu {
        unsigned int            cpu;

        /* P-state information */
        struct zen_pstate       pstates[ZEN_MAX_PSTATES];
        unsigned int            num_pstates;
        struct zen_pstate       boost_states[ZEN_MAX_BOOST_STATES];
        unsigned int            num_boost;

        u32                     max_freq;
        u32                     min_freq;
        u32                     nominal_freq;

        u8                      highest_perf;
        u8                      lowest_perf;
        u8                      nominal_perf;

        /* Current state (fast path) */
        unsigned int            cur_pstate;
        atomic_t                cur_freq;
        struct cpufreq_policy   *cur_policy;

        /* RCU-protected performance target */
        struct zen_perf_target  __rcu *perf_target;

        /* Update lock (for slow path operations) */
        spinlock_t              update_lock;

        /* Frequency table with RCU protection */
        struct cpufreq_frequency_table *freq_table;
        struct cpufreq_frequency_table __rcu *freq_table_rcu;

        /* Thermal guard state */
        enum zen_thermal_state  thermal_state;
        s32                     thermal_integral;
        u8                      thermal_throttle_perf;
        u32                     last_temp;

        /* I/O wait boost state */
        bool                    io_boost_active;
        unsigned long           io_boost_expire;
        u64                     last_io_wait;

        /* Dynamic EPP state */
        unsigned long           util_low_since;
        u8                      dynamic_epp;
        u8                      epp_mode;

        /* Statistics */
        struct {
                u64             transitions;
                u64             io_boosts;
                u64             thermal_events;
                u64             voltage_clamps;
                u64             total_time_ns;
        } stats;
};

/* ============================================================================
 * Global Driver State
 * ============================================================================ */

/**
 * struct zen_freq_driver - Global driver state
 * @cpus:               Array of per-CPU data pointers
 * @num_cpus:           Number of CPUs under driver control
 * @driver_lock:        Global driver lock
 * @initialized:        Whether driver is initialized
 *
 * @thermal_thread:     Thermal guard kernel thread
 * @thermal_should_run: Flag to stop thermal thread
 * @thermal_wq:         Thermal workqueue
 *
 * @features:           Feature flags
 */
struct zen_freq_driver {
        struct zen_freq_cpu     **cpus;
        unsigned int            num_cpus;

        struct mutex            driver_lock;
        bool                    initialized;

        /* Thermal guard */
        struct task_struct      *thermal_thread;
        atomic_t                thermal_should_run;
        wait_queue_head_t       thermal_wq;

        /* Features */
        u32                     features;
};

/* Feature flags */
#define ZEN_FEAT_BOOST                  BIT(0)
#define ZEN_FEAT_EPP                    BIT(1)
#define ZEN_FEAT_PREFCORE               BIT(2)
#define ZEN_FEAT_FAST_CPPC              BIT(3)
#define ZEN_FEAT_MSR_ACCESS             BIT(4)
#define ZEN_FEAT_THERMAL_GUARD          BIT(5)
#define ZEN_FEAT_IO_BOOST               BIT(6)
#define ZEN_FEAT_VOLTAGE_GUARD          BIT(7)

/* ============================================================================
 * Module Parameters (extern declarations)
 * ============================================================================ */

extern unsigned int zen_freq_mode;
extern bool zen_freq_boost_enabled;
extern unsigned int zen_freq_min_perf;
extern unsigned int zen_freq_max_perf;
extern bool zen_freq_epp_enabled;
extern bool zen_freq_thermal_guard;
extern unsigned int zen_freq_soft_temp;
extern unsigned int zen_freq_hard_temp;
extern unsigned int zen_freq_voltage_max;

/* Mode definitions */
#define ZEN_FREQ_MODE_POWERSAVE         0
#define ZEN_FREQ_MODE_BALANCE           1
#define ZEN_FREQ_MODE_PERFORMANCE       2
#define ZEN_FREQ_MODE_USERSPACE         3

/* ============================================================================
 * Frequency Calculation Constants
 * ============================================================================ */

#define ZEN_FREQ_BASE                   25      /* MHz per FID increment */
#define ZEN_FREQ_MULTIPLIER             1000    /* Convert MHz to kHz */
#define ZEN_DID_BASE                    0x10    /* Base divider value */

/* ============================================================================
 * Function Prototypes
 * ============================================================================ */

/* Zero-IPI MSR access */
void zen_write_pstate_local(void *info);
int zen_freq_set_pstate_zero_ipi(struct zen_freq_cpu *zcpu, unsigned int pstate);

/* Thermal guard */
int zen_thermal_guard_init(void);
void zen_thermal_guard_exit(void);
int zen_thermal_thread(void *data);
void zen_thermal_check_cpu(struct zen_freq_cpu *zcpu);

/* I/O wait boost */
void zen_io_boost_init(struct zen_freq_cpu *zcpu);
void zen_io_boost_check(struct zen_freq_cpu *zcpu, u64 io_wait);
bool zen_io_boost_should_boost(u64 io_wait, u64 total);

/* Voltage safety */
bool zen_voltage_verify_pstate(struct zen_pstate *ps);
int zen_voltage_check_all_pstates(struct zen_freq_cpu *zcpu);

/* Dynamic EPP */
void zen_epp_update_dynamic(struct zen_freq_cpu *zcpu, u32 util);

/* RCU-protected operations */
struct zen_perf_target *zen_perf_target_alloc(gfp_t flags);
void zen_perf_target_update(struct zen_freq_cpu *zcpu,
                            u8 desired, u8 min, u8 max, u8 epp);

/* Fast switch (lock-less) */
unsigned int zen_freq_fast_switch_lockless(struct cpufreq_policy *policy,
                                           unsigned int target_freq);

/* Standard driver callbacks */
int zen_freq_get_pstate_info(struct zen_freq_cpu *zcpu);
int zen_freq_build_freq_table(struct zen_freq_cpu *zcpu);
unsigned int zen_freq_get_frequency(struct zen_freq_cpu *zcpu, unsigned int pstate);
bool zen_freq_check_hardware_support(void);

/* Utility functions */
u32 zen_freq_calc_freq_from_pstate(u64 pstate_val);
u32 zen_read_temperature(unsigned int cpu);
const char *zen_freq_get_mode_string(unsigned int mode);

/* ============================================================================
 * Helper Macros
 * ============================================================================ */

#define ZEN_IS_ZEN2_OR_NEWER(family, model) \
        (((family) == 0x17 && (model) >= 0x30) || \
         ((family) == 0x19) || \
         ((family) >= 0x1A))

#define ZEN_HAS_BOOST(edx) \
        ((edx) & BIT(9))

/* Performance conversion helpers */
#define ZEN_PERF_TO_FREQ(perf, min_perf, max_perf, min_freq, max_freq) \
        ((min_freq) + (((max_freq) - (min_freq)) * ((perf) - (min_perf))) / \
         ((max_perf) - (min_perf)))

#define ZEN_FREQ_TO_PERF(freq, min_freq, max_freq, min_perf, max_perf) \
        ((min_perf) + (((max_perf) - (min_perf)) * ((freq) - (min_freq))) / \
         ((max_freq) - (min_freq)))

/* Clamp helpers */
#define ZEN_CLAMP(val, min, max) \
        ((val) < (min) ? (min) : ((val) > (max) ? (max) : (val)))

#endif /* _ZEN_FREQ_PERFECT_H */
