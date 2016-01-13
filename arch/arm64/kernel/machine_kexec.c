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

#include <linux/kernel.h>
#include <linux/kexec.h>
#include <linux/of_fdt.h>
#include <linux/slab.h>
#include <linux/uaccess.h>

#include <asm/cacheflush.h>
#include <asm/system_misc.h>

#include "cpu-reset.h"

/* Global variables for the arm64_relocate_new_kernel routine. */
extern const unsigned char arm64_relocate_new_kernel[];
extern const unsigned long arm64_relocate_new_kernel_size;

bool in_crash_kexec;
static unsigned long kimage_start;

/**
 * kexec_is_dtb - Helper routine to check the device tree header signature.
 */
static bool kexec_is_dtb(const void *dtb)
{
	__be32 magic;

	return get_user(magic, (__be32 *)dtb) ? false :
		(be32_to_cpu(magic) == OF_DT_HEADER);
}

/**
 * kexec_image_info - For debugging output.
 */
#define kexec_image_info(_i) _kexec_image_info(__func__, __LINE__, _i)
static void _kexec_image_info(const char *func, int line,
	const struct kimage *kimage)
{
	unsigned long i;

#if !defined(DEBUG)
	return;
#endif
	pr_devel("%s:%d:\n", func, line);
	pr_devel("  kexec kimage info:\n");
	pr_devel("    type:        %d\n", kimage->type);
	pr_devel("    start:       %lx\n", kimage->start);
	pr_devel("    head:        %lx\n", kimage->head);
	pr_devel("    nr_segments: %lu\n", kimage->nr_segments);

	for (i = 0; i < kimage->nr_segments; i++) {
		pr_devel("      segment[%lu]: %016lx - %016lx, %lx bytes, %lu pages%s\n",
			i,
			kimage->segment[i].mem,
			kimage->segment[i].mem + kimage->segment[i].memsz,
			kimage->segment[i].memsz,
			kimage->segment[i].memsz /  PAGE_SIZE,
			(kexec_is_dtb(kimage->segment[i].buf) ?
				", dtb segment" : ""));
	}
}

void machine_kexec_cleanup(struct kimage *kimage)
{
	/* Empty routine needed to avoid build errors. */
}

/**
 * machine_kexec_prepare - Prepare for a kexec reboot.
 *
 * Called from the core kexec code when a kernel image is loaded.
 */
int machine_kexec_prepare(struct kimage *kimage)
{
	kimage_start = kimage->start;
	kexec_image_info(kimage);

	return 0;
}

/**
 * kexec_list_flush - Helper to flush the kimage list to PoC.
 */
static void kexec_list_flush(unsigned long kimage_head)
{
	unsigned long *entry;

	for (entry = &kimage_head; ; entry++) {
		unsigned int flag = *entry & IND_FLAGS;
		void *addr = phys_to_virt(*entry & PAGE_MASK);

		switch (flag) {
		case IND_INDIRECTION:
			entry = (unsigned long *)addr - 1;
			__flush_dcache_area(addr, PAGE_SIZE);
			break;
		case IND_DESTINATION:
			break;
		case IND_SOURCE:
			__flush_dcache_area(addr, PAGE_SIZE);
			break;
		case IND_DONE:
			return;
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

	pr_devel("%s:\n", __func__);

	for (i = 0; i < kimage->nr_segments; i++) {
		pr_devel("  segment[%lu]: %016lx - %016lx, %lx bytes, %lu pages\n",
			i,
			kimage->segment[i].mem,
			kimage->segment[i].mem + kimage->segment[i].memsz,
			kimage->segment[i].memsz,
			kimage->segment[i].memsz /  PAGE_SIZE);

		__flush_dcache_area(phys_to_virt(kimage->segment[i].mem),
			kimage->segment[i].memsz);
	}
}

/**
 * machine_kexec - Do the kexec reboot.
 *
 * Called from the core kexec code for a sys_reboot with LINUX_REBOOT_CMD_KEXEC.
 */
void machine_kexec(struct kimage *kimage)
{
	phys_addr_t reboot_code_buffer_phys;
	void *reboot_code_buffer;

	if (num_online_cpus() > 1) {
		if (in_crash_kexec)
			pr_warn("*\n* kdump might fail because %d cpus are still online\n*\n", num_online_cpus());
		else
			BUG();
	}

	reboot_code_buffer_phys = page_to_phys(kimage->control_code_page);
	reboot_code_buffer = phys_to_virt(reboot_code_buffer_phys);

	kexec_image_info(kimage);

	pr_devel("%s:%d: control_code_page:        %p\n", __func__, __LINE__,
		kimage->control_code_page);
	pr_devel("%s:%d: reboot_code_buffer_phys:  %pa\n", __func__, __LINE__,
		&reboot_code_buffer_phys);
	pr_devel("%s:%d: reboot_code_buffer:       %p\n", __func__, __LINE__,
		reboot_code_buffer);
	pr_devel("%s:%d: relocate_new_kernel:      %p\n", __func__, __LINE__,
		arm64_relocate_new_kernel);
	pr_devel("%s:%d: relocate_new_kernel_size: 0x%lx(%lu) bytes\n",
		__func__, __LINE__, arm64_relocate_new_kernel_size,
		arm64_relocate_new_kernel_size);

	pr_devel("%s:%d: kimage_head:              %lx\n", __func__, __LINE__,
		kimage->head);
	pr_devel("%s:%d: kimage_start:             %lx\n", __func__, __LINE__,
		kimage_start);

	/*
	 * Copy arm64_relocate_new_kernel to the reboot_code_buffer for use
	 * after the kernel is shut down.
	 */
	memcpy(reboot_code_buffer, arm64_relocate_new_kernel,
		arm64_relocate_new_kernel_size);

	/* Flush the reboot_code_buffer in preparation for its execution. */
	__flush_dcache_area(reboot_code_buffer, arm64_relocate_new_kernel_size);

	/* Flush the kimage data. */
	if (in_crash_kexec)
		kexec_segment_flush(kimage);
	else
		kexec_list_flush(kimage->head);

	pr_info("Bye!\n");

	/* Disable all DAIF exceptions. */
	asm volatile ("msr daifset, #0xf" : : : "memory");

	setup_mm_for_reboot();

	/*
	 * cpu_soft_restart will shutdown the MMU, disable data caches, then
	 * transfer control to the reboot_code_buffer which contains a copy of
	 * the arm64_relocate_new_kernel routine.  arm64_relocate_new_kernel
	 * uses physical addressing to relocate the new image to its final
	 * position and transfers control to the image entry point when the
	 * relocation is complete.
	 */

	cpu_soft_restart(virt_to_phys(cpu_reset),
		in_crash_kexec ? 0 : is_hyp_mode_available(),
		reboot_code_buffer_phys, kimage->head, kimage_start);

	BUG(); /* Should never get here. */
}

/**
 * machine_crash_shutdown - shutdown non-boot cpus and save registers
 */
void machine_crash_shutdown(struct pt_regs *regs)
{
	struct pt_regs dummy_regs;
	int cpu;

	local_irq_disable();

	in_crash_kexec = true;

	/*
	 * clear and initialize the per-cpu info. This is necessary
	 * because, otherwise, slots for offline cpus would never be
	 * filled up. See smp_send_stop().
	 */
	memset(&dummy_regs, 0, sizeof(dummy_regs));
	for_each_possible_cpu(cpu)
		crash_save_cpu(&dummy_regs, cpu);

	/* shutdown non-boot cpus */
	smp_send_stop();

	/* for boot cpu */
	crash_save_cpu(regs, smp_processor_id());

	pr_info("Starting crashdump kernel...\n");
}
