/*
 * Arch-generic compatibility layer for enabling kexec as loadable kernel
 * module.
 *
 * Copyright (C) 2021 Fabian Mastenbroek.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#define pr_fmt(fmt) "kexec_mod: " fmt

#include <linux/version.h>
#include <linux/mm_types.h>
#include <linux/kexec.h>
#include <linux/kallsyms.h>
#include <linux/slab.h>
#include <asm/uaccess.h>
#include <asm/virt.h>

#include "kexec_compat.h"

/* These kernel symbols need to be dynamically resolved at runtime
 * using kallsym due to them not being exposed to kernel modules */
static void (*machine_shutdown_ptr)(void);
static void (*kernel_restart_prepare_ptr)(char*);
static void (*migrate_to_reboot_cpu_ptr)(void);
static void (*cpu_hotplug_enable_ptr)(void);

void machine_shutdown(void)
{
	machine_shutdown_ptr();
}

void kernel_restart_prepare(char *cmd)
{
	kernel_restart_prepare_ptr(cmd);
}

void migrate_to_reboot_cpu(void)
{
	migrate_to_reboot_cpu_ptr();
}

void cpu_hotplug_enable(void)
{
	cpu_hotplug_enable_ptr();
}

static void *ksym(const char *name)
{
	return (void *)kallsyms_lookup_name(name);
}

struct wdog_data {
	unsigned int __iomem phys_base;
	size_t size;
	void __iomem *base;
};

extern void __iomem *watchdog_base;

int kexec_compat_load()
{
	void (*wdog_disable)(void *wdog_dd) = ksym("wdog_disable");
	struct wdog_data **wdog_data_ptr = ksym("wdog_data");

	if (wdog_disable != NULL && wdog_data_ptr != NULL) {
		pr_info("wdog phys_base=0x%x base=0x%p\n",
			(*wdog_data_ptr)->phys_base, (*wdog_data_ptr)->base);
		watchdog_base = (*wdog_data_ptr)->base;
		wdog_disable(*wdog_data_ptr);
	} else
		pr_info("could not resolve qcom wdog_disable\n");

	if (!(machine_shutdown_ptr = ksym("machine_shutdown"))
	    || !(migrate_to_reboot_cpu_ptr = ksym("migrate_to_reboot_cpu"))
	    || !(kernel_restart_prepare_ptr = ksym("kernel_restart_prepare"))
	    || !(cpu_hotplug_enable_ptr = ksym("cpu_hotplug_enable")))
		return -ENOENT;
	return 0;
}

void kexec_compat_unload(void)
{}
