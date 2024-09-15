/*
 * kexec for arm64
 *
 * Copyright (C) Linaro.
 * Copyright (C) Huawei Futurewei Technologies.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/kernel.h>
#include <linux/page-flags.h>
#include <linux/smp.h>
#include <linux/io.h>
#include <linux/delay.h>

#include <asm/cacheflush.h>
#include <asm/cpu_ops.h>
#include <asm/daifflags.h>
#include <asm/memory.h>
#include <asm/mmu.h>
#include <asm/mmu_context.h>
#include <asm/page.h>

#include "../../kexec.h"
#include "cpu-reset.h"

/* Global variables for the arm64_relocate_new_kernel routine. */
extern const unsigned char arm64_relocate_new_kernel[];
extern const unsigned long arm64_relocate_new_kernel_size;

/**
 * kexec_image_info - For debugging output.
 */
#define kexec_image_info(_i) _kexec_image_info(__func__, __LINE__, _i)
static void _kexec_image_info(const char *func, int line,
			     const struct kimage *kimage)
{
       unsigned long i;

       pr_info("%s:%d:\n", func, line);
       pr_info("  kexec kimage info:\n");
       pr_info("    start:       %lx\n", kimage->start);
       pr_info("    head:        %lx\n", kimage->head);
       pr_info("    nr_segments: %lu\n", kimage->nr_segments);

       for (i = 0; i < kimage->nr_segments; i++) {
	       pr_info("      segment[%lu]: %016lx - %016lx, 0x%lx bytes, %lu pages\n",
			i,
			kimage->segment[i].mem,
			kimage->segment[i].mem + kimage->segment[i].memsz,
			kimage->segment[i].memsz,
			kimage->segment[i].memsz /  PAGE_SIZE);
       }
}

void machine_kexec_cleanup(struct kimage *kimage)
{
       /* Empty routine needed to avoid build errors. */
}
EXPORT_SYMBOL_GPL(machine_kexec_cleanup);

/**
 * machine_kexec_prepare - Prepare for a kexec reboot.
 *
 * Called from the core kexec code when a kernel image is loaded.
 * Forbid loading a kexec kernel if we have no way of hotplugging cpus or cpus
 * are stuck in the kernel. This avoids a panic once we hit machine_kexec().
 */
int machine_kexec_prepare(struct kimage *kimage)
{
       kexec_image_info(kimage);

       if (cpus_are_stuck_in_kernel()) {
	       pr_err("Can't kexec: CPUs are stuck in the kernel.\n");
	       return -EBUSY;
       }

       return 0;
}
EXPORT_SYMBOL_GPL(machine_kexec_prepare);

/**
 * kexec_list_flush - Helper to flush the kimage list and source pages to PoC.
 */
static void kexec_list_flush(struct kimage *kimage)
{
       kimage_entry_t *entry;

       for (entry = &kimage->head; ; entry++) {
	       unsigned int flag;
	       void *addr;

	       /* flush the list entries. */
	       __flush_dcache_area(entry, sizeof(kimage_entry_t));

	       flag = *entry & IND_FLAGS;
	       if (flag == IND_DONE)
		       break;

	       addr = phys_to_virt(*entry & PAGE_MASK);

	       switch (flag) {
	       case IND_INDIRECTION:
		       /* Set entry point just before the new list page. */
		       entry = (kimage_entry_t *)addr - 1;
		       break;
	       case IND_SOURCE:
		       /* flush the source pages. */
		       __flush_dcache_area(addr, PAGE_SIZE);
		       break;
	       case IND_DESTINATION:
		       break;
	       default:
		       BUG();
	       }
       }
}

/**
 * kexec_segment_flush - Helper to flush the kimage segments to PoC.
 */
static void kexec_segment_flush(const struct kimage *kimage)
{
       unsigned long i;

       pr_info("%s:\n", __func__);

       for (i = 0; i < kimage->nr_segments; i++) {
	       pr_info("  segment[%lu]: %016lx - %016lx, 0x%lx bytes, %lu pages\n",
			i,
			kimage->segment[i].mem,
			kimage->segment[i].mem + kimage->segment[i].memsz,
			kimage->segment[i].memsz,
			kimage->segment[i].memsz /  PAGE_SIZE);

	       __flush_dcache_area(phys_to_virt(kimage->segment[i].mem),
				   kimage->segment[i].memsz);
       }
}

void __iomem *watchdog_base;
EXPORT_SYMBOL(watchdog_base);

#define WDT0_ACCSCSSNBARK_INT 0
#define TCSR_WDT_CFG	0x30
#define WDT0_RST	0x04
#define WDT0_EN		0x08
#define WDT0_STS	0x0C
#define WDT0_BARK_TIME	0x10
#define WDT0_BITE_TIME	0x14

static long WDT_HZ = 32765;

void watchdog_bite(int when)
{
	u64 timeout = when == 0 ? 1 : (12000 * WDT_HZ)/1000 + 3*WDT_HZ;

	if (watchdog_base == NULL) {
		pr_info("watchdog_bite: watchdog_base not set up!");
		return;
	}
	__raw_writel(1, watchdog_base + WDT0_EN);
	__raw_writel(1, watchdog_base + WDT0_RST);
	mb();
	__raw_writel(timeout, watchdog_base + WDT0_BITE_TIME);
	mb();
	__raw_writel(1, watchdog_base + WDT0_RST);
	mb();
	if (when == 0) {
		pr_info("watchdog_bite: waiting 10s for bite to occur!\n");
		mdelay(10000);
	} else
		pr_info("watchdog_bite: watchdog enabled, continuing!\n");
}
EXPORT_SYMBOL(watchdog_bite);

/**
 * machine_kexec - Do the kexec reboot.
 *
 * Called from the core kexec code for a sys_reboot with LINUX_REBOOT_CMD_KEXEC.
 */
void machine_kexec(struct kimage *kimage)
{
       phys_addr_t reboot_code_buffer_phys;
       void *reboot_code_buffer;
       bool stuck_cpus = cpus_are_stuck_in_kernel();

       BUG_ON(stuck_cpus || (num_online_cpus() > 1));

       /*
	* New cpus may have become stuck_in_kernel after we loaded the image.
	*/
       reboot_code_buffer_phys = page_to_phys(kimage->control_code_page);
       reboot_code_buffer = phys_to_virt(reboot_code_buffer_phys);

       kexec_image_info(kimage);

       pr_info("%s:%d: control_code_page:        %p\n", __func__, __LINE__,
		kimage->control_code_page);
       pr_info("%s:%d: reboot_code_buffer_phys:  %pa\n", __func__, __LINE__,
		&reboot_code_buffer_phys);
       pr_info("%s:%d: reboot_code_buffer:       %p\n", __func__, __LINE__,
		reboot_code_buffer);
       pr_info("%s:%d: relocate_new_kernel:      %p\n", __func__, __LINE__,
		arm64_relocate_new_kernel);
       pr_info("%s:%d: relocate_new_kernel_size: 0x%lx(%lu) bytes\n",
		__func__, __LINE__, arm64_relocate_new_kernel_size,
		arm64_relocate_new_kernel_size);

       /*
	* Copy arm64_relocate_new_kernel to the reboot_code_buffer for use
	* after the kernel is shut down.
	*/
       memcpy(reboot_code_buffer, arm64_relocate_new_kernel,
	      arm64_relocate_new_kernel_size);

       /* Flush the reboot_code_buffer in preparation for its execution. */
       __flush_dcache_area(reboot_code_buffer, arm64_relocate_new_kernel_size);

       flush_icache_range((uintptr_t)reboot_code_buffer,
			    arm64_relocate_new_kernel_size);

       /* Flush the kimage list and its buffers. */
       kexec_list_flush(kimage);

       /* Flush the new image if already in place. */
       if (kimage->head & IND_DONE)
	       kexec_segment_flush(kimage);

       pr_info("Bye!\n");
	watchdog_bite(1);

       local_daif_mask();

       /*
	* cpu_soft_restart will shutdown the MMU, disable data caches, then
	* transfer control to the reboot_code_buffer which contains a copy of
	* the arm64_relocate_new_kernel routine.  arm64_relocate_new_kernel
	* uses physical addressing to relocate the new image to its final
	* position and transfers control to the image entry point when the
	* relocation is complete.
	*/

       km_cpu_soft_restart(reboot_code_buffer_phys, kimage->head, kimage->start, 0);

       BUG(); /* Should never get here. */
}
EXPORT_SYMBOL_GPL(machine_kexec);
