/*
 * This file is subject to the terms and conditions of the GNU General
 * Public License.  See the file "COPYING" in the main directory of this
 * archive for more details.
 *
 * Copyright (C) 2000 - 2001 by Kanoj Sarcar (kanoj@sgi.com)
 * Copyright (C) 2000 - 2001 by Silicon Graphics, Inc.
 * Copyright (C) 2000, 2001, 2002 Ralf Baechle
 * Copyright (C) 2000, 2001 Broadcom Corporation
 */
#ifndef __ASM_SMP_H
#define __ASM_SMP_H


#ifdef CONFIG_SMP

#include <linux/bitops.h>
#include <linux/linkage.h>
#include <linux/threads.h>
#include <linux/cpumask.h>
#include <asm/atomic.h>

#define raw_smp_processor_id() (current_thread_info()->cpu)

/* Map from cpu id to sequential logical cpu number.  This will only
   not be idempotent when cpus failed to come on-line.  */
extern int __cpu_number_map[NR_CPUS];
#define cpu_number_map(cpu)  __cpu_number_map[cpu]

/* The reverse map from sequential logical cpu number to cpu id.  */
extern int __cpu_logical_map[NR_CPUS];
#define cpu_logical_map(cpu)  __cpu_logical_map[cpu]

#define NO_PROC_ID	(-1)

struct call_data_struct {
	void		(*func)(void *);
	void		*info;
	atomic_t	started;
	atomic_t	finished;
	int		wait;
};

extern struct call_data_struct *call_data;

#define SMP_RESCHEDULE_YOURSELF	0x1	/* XXX braindead */
#define SMP_CALL_FUNCTION	0x2

#if defined(CONFIG_BROADCOM_9636X) && defined(CONFIG_BCM_HOSTMIPS_PWRSAVE_TIMERS)
#define SMP_BCM_PWRSAVE_TIMER   0x3
#endif

#define SMP_ICACHE_FLUSH	0x4     /* Octeon - Tell another core to flush its icache */

extern cpumask_t phys_cpu_present_map;
#define cpu_possible_map	phys_cpu_present_map

extern cpumask_t cpu_callout_map;
/* We don't mark CPUs online until __cpu_up(), so we need another measure */
static inline int num_booting_cpus(void)
{
	return cpus_weight(cpu_callout_map);
}

/*
 * These are defined by the board-specific code.
 */

/*
 * Cause the function described by call_data to be executed on the passed
 * cpu.  When the function has finished, increment the finished field of
 * call_data.
 */
extern void core_send_ipi(int cpu, unsigned int action);

/*
 * Firmware CPU startup hook
 */
extern void prom_boot_secondary(int cpu, struct task_struct *idle);

/*
 *  After we've done initial boot, this function is called to allow the
 *  board code to clean up state, if needed
 */
extern void prom_init_secondary(void);

/*
 * Populate cpu_possible_map before smp_init, called from setup_arch.
 */
extern void plat_smp_setup(void);

/*
 * Called in smp_prepare_cpus.
 */
extern void plat_prepare_cpus(unsigned int max_cpus);

/*
 * Last chance for the board code to finish SMP initialization before
 * the CPU is "online".
 */
extern void prom_smp_finish(void);

/* Hook for after all CPUs are online */
extern void prom_cpus_done(void);

extern void asmlinkage smp_bootstrap(void);

/*
 * this function sends a 'reschedule' IPI to another CPU.
 * it goes straight through and wastes no time serializing
 * anything. Worst case is that we lose a reschedule ...
 */
static inline void smp_send_reschedule(int cpu)
{
	core_send_ipi(cpu, SMP_RESCHEDULE_YOURSELF);
}

extern asmlinkage void smp_call_function_interrupt(void);

int smp_call_function(void(*func)(void *info), void *info, int retry, int wait);

/*
 * Special Variant of smp_call_function for use by cache functions:
 *
 *  o No return value
 *  o collapses to normal function call on UP kernels
 *  o collapses to normal function call on systems with a single shared
 *    primary cache.
 *  o Both CONFIG_MIPS_MT_SMP and CONFIG_MIPS_MT_SMTC currently imply there
 *    is only one physical core.
 */
static inline void __on_other_cores(void (*func) (void *info), void *info)
{
#if !defined(CONFIG_MIPS_MT_SMP) && !defined(CONFIG_MIPS_MT_SMTC)
	smp_call_function(func, info, 1, 1);
#endif
}

static inline void __on_each_core(void (*func) (void *info), void *info)
{
	preempt_disable();

	__on_other_cores(func, info);
	func(info);

	preempt_enable();
}

#endif /* CONFIG_SMP */

#endif /* __ASM_SMP_H */
