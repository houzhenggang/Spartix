/*----------------------------------------------------------------------
 * Copyright (C) 2016, 2017 Pedro Falcato
 *
 * This file is part of Spartix, and is made available under
 * the terms of the GNU General Public License version 2.
 *
 * You can redistribute it and/or modify it under the terms of the GNU
 * General Public License version 2 as published by the Free Software
 * Foundation.
 *----------------------------------------------------------------------*/
/**************************************************************************
 *
 *
 * File: cpu.c
 *
 * Description: Contains CPU identification routines on the x86 architecture
 *
 * Date: 6/4/2016
 *
 *
 **************************************************************************/
#include <stdlib.h>
#include <cpuid.h>
#include <string.h>
#include <stdio.h>
#include <acpi.h>

#include <kernel/cpu.h>
#include <kernel/panic.h>
#include <kernel/apic.h>
#include <kernel/pic.h>
#include <kernel/acpi.h>
#include <kernel/spinlock.h>
static cpu_t cpu;

char *cpu_get_name()
{
	uint32_t eax,ebx,edx,ecx = 0;
	__get_cpuid(0,&eax,&ebx,&ecx,&edx);
	
	uint32_t cpuid[4] = {0};
	cpuid[0] = ebx;
	cpuid[1] = edx;
	cpuid[2] = ecx;
	memcpy(&cpu.manuid,&cpuid,12);
	/* Zero terminate the string */
	cpu.manuid[12] = '\0';
	__get_cpuid(CPUID_MAXFUNCTIONSUPPORTED,&eax,&ebx,&ecx,&edx);
	cpu.max_function = eax;
	if( cpu.max_function >= 0x8000004 ) {
		__get_cpuid(CPUID_BRAND0,&eax,&ebx,&ecx,&edx);
		cpuid[0] = eax;
		cpuid[1] = ebx;
		cpuid[2] = ecx;
		cpuid[3] = edx;
		memcpy(&cpu.brandstr,&cpuid,16);
		__get_cpuid(CPUID_BRAND1,&eax,&ebx,&ecx,&edx);
		cpuid[0] = eax;
		cpuid[1] = ebx;
		cpuid[2] = ecx;
		cpuid[3] = edx;
		memcpy(&cpu.brandstr[16],&cpuid,16);
		__get_cpuid(CPUID_BRAND2,&eax,&ebx,&ecx,&edx);
		cpuid[0] = eax;
		cpuid[1] = ebx;
		cpuid[2] = ecx;
		cpuid[3] = edx;
		memcpy(&cpu.brandstr[32],&cpuid,16);
		cpu.brandstr[47] = '\0';
		// Get the address space sizes
		__get_cpuid(CPUID_ASS, &eax, &ebx, &ecx, &edx);
		cpu.physicalAddressSpace = eax & 0xFF;
		cpu.virtualAddressSpace = (eax >> 8) & 0xFF;
	}
	return &cpu.manuid[0];
}
void cpu_get_sign()
{
	uint32_t eax = 0,ebx,edx,ecx = 0;
	__get_cpuid(CPUID_SIGN,&eax,&ebx,&ecx,&edx);
	cpu.stepping = eax & 0xF;
	cpu.model = (eax >> 4) & 0xF;
	cpu.family = (eax >> 8) & 0xF;
}
void cpu_identify()
{
	printf("Detected x86_64 CPU\n");
	printf("Manufacturer ID: %s\n", cpu_get_name());
	if(cpu.brandstr[0] != '\0')
		printf("Name: %s\n", cpu.brandstr);
	cpu_get_sign();
	printf("Stepping %i, Model %i, Family %i\n", cpu.stepping, cpu.model, cpu.family);
}
extern void syscall_ENTRY64();
void cpu_init_interrupts()
{
	pic_remap();
	ioapic_init();
	lapic_init();
	apic_timer_init();

	wrmsr(IA32_MSR_STAR, 0, ((0x18 | 3) << 16) | 0x8);
	wrmsr(IA32_MSR_LSTAR, (unsigned long) syscall_ENTRY64 & 0xFFFFFFFF, (unsigned long) syscall_ENTRY64 >> 32);
	wrmsr(IA32_MSR_SFMASK, 0b11000000000, 0);
}
static struct processor *cpus = NULL;
static int booted_cpus = 0;
extern ACPI_TABLE_MADT *madt;
int cpu_num = 0;
static spinlock_t ap_entry_spinlock;
extern volatile uint32_t *bsp_lapic;
volatile int initialized_cpus = 0;
extern volatile uint64_t boot_ticks;
int cpu_init_mp()
{
	ACPI_SUBTABLE_HEADER *first = (ACPI_SUBTABLE_HEADER *) (madt+1);
	/* Lets parse through the MADT to get the number of cores.
	 * Each LAPIC = 1 core */
	
	/* APs can't access ´cpus´ before we've finished, as it's subject to memory address changes */
	acquire_spinlock(&ap_entry_spinlock);
	
	for(ACPI_SUBTABLE_HEADER *i = first; i < (ACPI_SUBTABLE_HEADER*)((char*)madt + madt->Header.Length); i = 
	(ACPI_SUBTABLE_HEADER*)((uint64_t)i + (uint64_t)i->Length))
	{
		if(i->Type == ACPI_MADT_TYPE_LOCAL_APIC)
		{
			ACPI_MADT_LOCAL_APIC *apic = (ACPI_MADT_LOCAL_APIC *) i;
			cpus = realloc(cpus, booted_cpus * sizeof(struct processor) + sizeof(struct processor));
			cpus[booted_cpus].lapic_id = apic->Id;
			cpus[booted_cpus].cpu_num = booted_cpus;
			cpus[booted_cpus].self = &cpus[booted_cpus];
			cpus[booted_cpus].sched_quantum = 10;
			booted_cpus++;
			if(booted_cpus != 1)
				apic_wake_up_processor(apic->Id);
			cpu_num = booted_cpus;
		}
	}
	/* Fill CPU0's data */
	cpus[0].lapic = (volatile char*) bsp_lapic;
	cpus[0].self = &cpus[0];
	cpus[0].apic_ticks = boot_ticks;
	cpus[0].sched_quantum = 10;
	wrmsr(GS_BASE_MSR, (uint64_t) &cpus[0] & 0xFFFFFFFF, (uint64_t) &cpus[0] >> 32);
	release_spinlock(&ap_entry_spinlock);
	
	while(initialized_cpus+1 != booted_cpus);
	return booted_cpus;
}
void cpu_ap_entry(int cpu_num)
{
	acquire_spinlock(&ap_entry_spinlock);

	uint32_t high, low;
	rdmsr(0x1b, &low, &high);
	uint64_t addr = low | ((uint64_t)high << 32);
	addr &= 0xFFFFF000;
	/* Map the BSP's LAPIC */
	uintptr_t _lapic = vmm_allocate_virt_address(VM_KERNEL, 1, VMM_TYPE_REGULAR, VMM_TYPE_HW);
	paging_map_phys_to_virt((uintptr_t)_lapic, addr, VMM_WRITE | VMM_NOEXEC | VMM_GLOBAL);
	
	/* Fill the processor struct with the LAPIC data */
	cpus[cpu_num].lapic = _lapic;
	cpus[cpu_num].lapic_phys = addr;

	/* Initialize the local apic + apic timer */
	apic_timer_smp_init((volatile uint32_t *) cpus[cpu_num].lapic);

	/* Fill this core's gs with &cpus[cpu_num] */
	wrmsr(GS_BASE_MSR, (uint64_t) &cpus[cpu_num] & 0xFFFFFFFF, (uint64_t) &cpus[cpu_num] >> 32);

	/* Enable interrupts */
	//asm volatile("sti");

	initialized_cpus++;
	release_spinlock(&ap_entry_spinlock);
	/* cpu_ap_entry() can't return, as there's no valid return address on the stack, so just hlt until the scheduler
	   preempts the AP
	*/
	while(1);
}