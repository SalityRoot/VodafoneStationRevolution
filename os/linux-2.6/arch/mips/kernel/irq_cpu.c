/*
 * Copyright 2001 MontaVista Software Inc.
 * Author: Jun Sun, jsun@mvista.com or jsun@junsun.net
 *
 * Copyright (C) 2001 Ralf Baechle
 * Copyright (C) 2005  MIPS Technologies, Inc.  All rights reserved.
 *      Author: Maciej W. Rozycki <macro@mips.com>
 *
 * This file define the irq handler for MIPS CPU interrupts.
 *
 * This program is free software; you can redistribute  it and/or modify it
 * under  the terms of  the GNU General  Public License as published by the
 * Free Software Foundation;  either version 2 of the  License, or (at your
 * option) any later version.
 */

/*
 * Almost all MIPS CPUs define 8 interrupt sources.  They are typically
 * level triggered (i.e., cannot be cleared from CPU; must be cleared from
 * device).  The first two are software interrupts which we don't really
 * use or support.  The last one is usually the CPU timer interrupt if
 * counter register is present or, for CPUs with an external FPU, by
 * convention it's the FPU exception interrupt.
 *
 * Don't even think about using this on SMP.  You have been warned.
 *
 * This file exports one global function:
 *	void mips_cpu_irq_init(void);
 */
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>

#include <asm/irq_cpu.h>
#include <asm/mipsregs.h>
#include <asm/mipsmtregs.h>
#include <asm/system.h>

static inline void unmask_mips_irq(unsigned int irq)
{
	set_c0_status(0x100 << (irq - MIPS_CPU_IRQ_BASE));
	irq_enable_hazard();
}

static inline void mask_mips_irq(unsigned int irq)
{
	clear_c0_status(0x100 << (irq - MIPS_CPU_IRQ_BASE));
	irq_disable_hazard();
}

#if !(defined(CONFIG_AR9) || defined(CONFIG_VR9))
static struct irq_chip mips_cpu_irq_controller = {
	.name		= "MIPS",
	.ack		= mask_mips_irq,
	.mask		= mask_mips_irq,
	.mask_ack	= mask_mips_irq,
	.unmask		= unmask_mips_irq,
	.eoi		= unmask_mips_irq,
};
#endif

/*
 * Basically the same as above but taking care of all the MT stuff
 */

#define unmask_mips_mt_irq	unmask_mips_irq
#define mask_mips_mt_irq	mask_mips_irq

static unsigned int mips_mt_cpu_irq_startup(unsigned int irq)
{
	unsigned int vpflags = dvpe();

	clear_c0_cause(0x100 << (irq - MIPS_CPU_IRQ_BASE));
#if !(defined(CONFIG_AR9) || defined(CONFIG_VR9))
	evpe(vpflags);
#endif
	unmask_mips_mt_irq(irq);
#if defined(CONFIG_AR9) || defined(CONFIG_VR9)
	evpe(vpflags);
#endif

	return 0;
}

/*
 * While we ack the interrupt interrupts are disabled and thus we don't need
 * to deal with concurrency issues.  Same for mips_cpu_irq_end.
 */
static void mips_mt_cpu_irq_ack(unsigned int irq)
{
	unsigned int vpflags = dvpe();
	clear_c0_cause(0x100 << (irq - MIPS_CPU_IRQ_BASE));
	evpe(vpflags);
	mask_mips_mt_irq(irq);
}

static struct irq_chip mips_mt_cpu_irq_controller = {
	.name		= "MIPS",
	.startup	= mips_mt_cpu_irq_startup,
	.ack		= mips_mt_cpu_irq_ack,
	.mask		= mask_mips_mt_irq,
	.mask_ack	= mips_mt_cpu_irq_ack,
	.unmask		= unmask_mips_mt_irq,
	.eoi		= unmask_mips_mt_irq,
};

void __init mips_cpu_irq_init(void)
{
	int irq_base = MIPS_CPU_IRQ_BASE;
	int i;

	/* Mask interrupts. */
	clear_c0_status(ST0_IM);
	clear_c0_cause(CAUSEF_IP);

	/*
	 * Only MT is using the software interrupts currently, so we just
	 * leave them uninitialized for other processors.
	 */
	if (cpu_has_mipsmt)
		for (i = irq_base; i < irq_base + 2; i++)
			set_irq_chip(i, &mips_mt_cpu_irq_controller);

#if !(defined(CONFIG_AR9) || defined(CONFIG_VR9))
	for (i = irq_base + 2; i < irq_base + 8; i++)
		set_irq_chip_and_handler(i, &mips_cpu_irq_controller,
					 handle_level_irq);
#endif
}
