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
#include <stdio.h>
#include <stdbool.h>
#include <cpuid.h>

#include <kernel/paging.h>
#include <kernel/process.h>
#include <kernel/vmm.h>
#include <kernel/panic.h>
static _Bool is_spawning = 0;
PML4 *spawning_pml = NULL;
#define PML_EXTRACT_ADDRESS(n) (n & 0x0FFFFFFFFFFFF000)
inline void __native_tlb_invalidate_page(void *addr)
{
	__asm__ __volatile__("invlpg %0"::"m"(addr));
}
inline uint64_t make_pml4e(uint64_t base,uint64_t avl,uint64_t pcd,uint64_t pwt,uint64_t us,uint64_t rw,uint64_t p)
{
	return (uint64_t)( \
  		(base) | \
  		(avl << 9) | \
  		(pcd << 4) | \
  		(pwt << 3) | \
  		(us << 2) | \
  		(rw << 1) | \
  		p);
}
inline uint64_t make_pml3e(uint64_t base,uint64_t nx, uint64_t avl,uint64_t glbl, uint64_t pcd,uint64_t pwt,uint64_t us,uint64_t rw,uint64_t p)
{
	return (uint64_t)( \
  		(base) | \
  		(nx << 63) | \
  		(avl << 9) | \
  		(glbl << 8) | \
  		(pcd << 4) | \
  		(pwt << 3) | \
  		(us << 2) | \
  		(rw << 1) | \
  		p);
}
inline uint64_t make_pml2e(uint64_t base,uint64_t nx, uint64_t avl,uint64_t glbl,uint64_t pcd,uint64_t pwt,uint64_t us,uint64_t rw,uint64_t p)
{
	return (uint64_t)( \
  		(base) | \
  		(nx << 63) | \
  		(avl << 9) | \
  		(glbl << 8) | \
  		(pcd << 4) | \
  		(pwt << 3) | \
  		(us << 2) | \
  		(rw << 1) | \
  		p);
}
inline uint64_t make_pml1e(uint64_t base,uint64_t nx, uint64_t avl,uint64_t glbl,uint64_t pcd,uint64_t pwt,uint64_t us,uint64_t rw,uint64_t p)
{
	return (uint64_t)( \
  		(base) | \
  		(nx << 63) | \
  		(avl << 9) | \
  		(glbl << 8) | \
  		(pcd << 4) | \
  		(pwt << 3) | \
  		(us << 2) | \
  		(rw << 1) | \
  		p);
}
typedef struct 
{
	uint64_t offsetFromPage :12;
	uint64_t pt :9;
	uint64_t pd :9;
	uint64_t pdpt :9;
	uint64_t pml4 :9;
	uint64_t rest :16;
} decomposed_addr_t;
PML4 *current_pml4 = NULL;
void *virtual2phys(void *ptr)
{
	decomposed_addr_t dec;
	memcpy(&dec, &ptr, sizeof(decomposed_addr_t));
	PML4 *pml4;
	if(!is_spawning)
		pml4 = (PML4*)((uint64_t)current_pml4 + PHYS_BASE);
	else
		pml4 = (PML4*)((uint64_t)spawning_pml + PHYS_BASE);
	PML3 *pml3 = (PML3*)((pml4->entries[dec.pml4] & 0x0FFFFFFFFFFFF000) + PHYS_BASE);
	PML2 *pml2 = (PML2*)((pml3->entries[dec.pdpt] & 0x0FFFFFFFFFFFF000)+ PHYS_BASE);
	PML1 *pml1 = (PML1*)((pml2->entries[dec.pd] & 0x0FFFFFFFFFFFF000)+ PHYS_BASE);
	return (void *)((pml1->entries[dec.pt] & 0x0FFFFFFFFFFFF000) + dec.offsetFromPage);
}
extern PML3 pdptphysical_map;
static PML2 pdphysical_map __attribute__((aligned(PAGE_SIZE)));
void paging_init()
{
	/* Get the current PML4 and store it */
	asm volatile("movq %%cr3, %%rax\t\nmovq %%rax, %0":"=r"(current_pml4));
	/* Bootstrap the first 1GB */
	uintptr_t virt = 0xffffea0000000000;
	decomposed_addr_t decAddr;
	memcpy(&decAddr, &virt, sizeof(decomposed_addr_t));
	uint64_t* entry = &current_pml4->entries[decAddr.pml4];
	PML3* pml3 = (PML3*)&pdptphysical_map;
	
	memset(pml3, 0, sizeof(PML3));
	*entry = make_pml4e((uint64_t)pml3, 0, 0, 0, 0, 1, 1);
	entry = &pml3->entries[decAddr.pdpt];
	*entry = make_pml3e(((uint64_t) &pdphysical_map - KERNEL_VIRTUAL_BASE), 0, 0, 1, 0, 0, 0, 1, 1);
	
	for(size_t j = 0; j < 512; j++)
	{
		if(!paging_map_phys_to_virt_large_early(virt + j * 0x200000, 
		j * 0x200000, VM_NOEXEC | VM_GLOBAL | VM_WRITE))
			while(1);
	}

}
void paging_map_all_phys()
{
	_Bool is_1gb_supported = false;
	uint32_t eax = 0,ebx = 0,edx = 0,ecx = 0;
	__get_cpuid(0x80000001, &eax, &ebx, &ecx, &edx);

	if(edx & (1 << 26))
		is_1gb_supported = true;
	uintptr_t virt = 0xffffea0000000000;
	decomposed_addr_t decAddr;
	memcpy(&decAddr, &virt, sizeof(decomposed_addr_t));
	uint64_t* entry = &current_pml4->entries[decAddr.pml4];
	PML3* pml3 = (PML3*)&pdptphysical_map;
	
	memset(pml3, 0, sizeof(PML3));
	*entry = make_pml4e((uint64_t)pml3, 0, 0, 0, 0, 1, 1);
	if(is_1gb_supported)
	{
		for(size_t i = 0; i < 512; i++)
		{
			entry = &pml3->entries[i];
			*entry = make_pml3e(i * 0x40000000, 0, 0, 1, 0, 0, 0, 1, 1);
			*entry |= (1 << 7);
			__native_tlb_invalidate_page((void*)(virt + i * 0x40000000));
		}
	}
	else
	{
		/* Use 2MiB pages instead */
		entry = &pml3->entries[decAddr.pdpt];
		*entry = make_pml3e(((uint64_t) &pdphysical_map - KERNEL_VIRTUAL_BASE), 0, 0, 1, 0, 0, 0, 1, 1);
		for(size_t i = 0; i < 512; i++)
		{
			for(size_t j = 0; j < 512; j++)
			{
				if(!paging_map_phys_to_virt_large(virt + i * 0x40000000 + j * 0x200000, 
				i * 0x40000000 + j * 0x200000, VM_NOEXEC | VM_GLOBAL | VM_WRITE))
					return;
			}
		}
	}
}
void *paging_map_phys_to_virt_huge(uint64_t virt, uint64_t phys, uint64_t prot)
{
	_Bool user = 0;
	if (virt < 0x00007fffffffffff)
		user = 1;
	if(!current_pml4)
		return NULL;
	decomposed_addr_t decAddr;
	memcpy(&decAddr, &virt, sizeof(decomposed_addr_t));
	PML4 *pml4 = (PML4*)((uint64_t)current_pml4 + KERNEL_VIRTUAL_BASE);
	uint64_t* entry = &pml4->entries[decAddr.pml4];
	PML3* pml3 = NULL;
	if(*entry & 1)
	{
		pml3 = (PML3*)(*entry & 0x0FFFFFFFFFFFF000);
	}
	else
	{
		pml3 = (PML3*)bootmem_alloc(1);
		if(!pml3)
			return NULL;
		memset((void*)((uint64_t)pml3 + KERNEL_VIRTUAL_BASE), 0, sizeof(PML3));
		*entry = make_pml4e((uint64_t)pml3, 0, 0, 0, user ? 1 : 0, 1, 1);
	}
	pml3 = (PML3*)((char*) pml3 + KERNEL_VIRTUAL_BASE);
	pml3->entries[decAddr.pdpt] = make_pml3e((phys & 0x000fffffc0000000UL), (prot & VM_NOEXEC) ? 1 : 0, 0, 0, 0, 0, user ? 1 : 0, (prot & VM_WRITE) ? 1 : 0, 1);
	pml3->entries[decAddr.pdpt] |= (1 << 7);

	__native_tlb_invalidate_page((void*) virt);
	return (void*) virt;
}
void* paging_map_phys_to_virt_large_early(uint64_t virt, uint64_t phys, uint64_t prot)
{
	_Bool user = 0;
	if (virt < 0x00007fffffffffff)
		user = 1;
	if(!current_pml4)
		return NULL;
	decomposed_addr_t decAddr;
	memcpy(&decAddr, &virt, sizeof(decomposed_addr_t));
	PML4 *pml4 = (PML4*)((uint64_t)current_pml4 + KERNEL_VIRTUAL_BASE);
	
	uint64_t* entry = &pml4->entries[decAddr.pml4];
	PML3* pml3 = NULL;
	PML2* pml2 = NULL;
	/* If its present, use that pml3 */
	if(*entry & 1) {
		pml3 = (PML3*)(*entry & 0x0FFFFFFFFFFFF000);
	}
	else { /* Else create one */
		pml3 = (PML3*)bootmem_alloc(1);
		if(!pml3)
			return NULL;
		memset((void*)((uint64_t)pml3 + KERNEL_VIRTUAL_BASE), 0, sizeof(PML3));
		*entry = make_pml4e((uint64_t)pml3, 0, 0, 0, user ? 1 : 0, 1, 1);
	}
	pml3 = (PML3*)((uint64_t)pml3 + KERNEL_VIRTUAL_BASE);
	entry = &pml3->entries[decAddr.pdpt];
	if(*entry & 1) {
		pml2 = (PML2*)(*entry & 0x0FFFFFFFFFFFF000);
	}
	else {
		pml2 = (PML2*)bootmem_alloc(1);
		if(!pml2 )
			return NULL;
		memset((void*)((uint64_t)pml2 + KERNEL_VIRTUAL_BASE), 0, sizeof(PML2));
		*entry = make_pml3e( (uint64_t)pml2, 0, 0, 0, 0, 0, user ? 1 : 0, 1, 1);
	}
	pml2 = (PML2*)((uint64_t)pml2 + KERNEL_VIRTUAL_BASE);
	entry = &pml2->entries[decAddr.pd];
	
	*entry = make_pml2e(phys, (prot & 4), 0, (prot & 2)? 1 : 0, 0, 0, (prot & 0x80) ? 1 : 0, (prot & 1)? 1 : 0, 1);
	*entry |= (1 << 7);
	return (void*) virt;
}
void* paging_map_phys_to_virt_large(uint64_t virt, uint64_t phys, uint64_t prot)
{
	_Bool user = 0;
	if (virt < 0x00007fffffffffff)
		user = 1;
	if(!current_pml4)
		return NULL;
	decomposed_addr_t decAddr;
	memcpy(&decAddr, &virt, sizeof(decomposed_addr_t));
	PML4 *pml4 = (PML4*)((uint64_t)current_pml4 + PHYS_BASE);
	
	uint64_t* entry = &pml4->entries[decAddr.pml4];
	PML3* pml3 = NULL;
	PML2* pml2 = NULL;
	/* If its present, use that pml3 */
	if(*entry & 1) {
		pml3 = (PML3*)(*entry & 0x0FFFFFFFFFFFF000);
	}
	else { /* Else create one */
		pml3 = (PML3*)bootmem_alloc(1);
		if(!pml3)
			return NULL;
		memset((void*)((uint64_t)pml3 + PHYS_BASE), 0, sizeof(PML3));
		*entry = make_pml4e((uint64_t)pml3, 0, 0, 0, user ? 1 : 0, 1, 1);
	}
	pml3 = (PML3*)((uint64_t)pml3 + PHYS_BASE);
	entry = &pml3->entries[decAddr.pdpt];
	if(*entry & 1) {
		pml2 = (PML2*)(*entry & 0x0FFFFFFFFFFFF000);
	}
	else {
		pml2 = (PML2*)bootmem_alloc(1);
		if(!pml2 )
			return NULL;
		memset((void*)((uint64_t)pml2 + PHYS_BASE), 0, sizeof(PML2));
		*entry = make_pml3e( (uint64_t)pml2, 0, 0, 0, 0, 0, user ? 1 : 0, 1, 1);
	}
	pml2 = (PML2*)((uint64_t)pml2 + PHYS_BASE);
	entry = &pml2->entries[decAddr.pd];
	
	*entry = make_pml2e(phys, (prot & 4), 0, (prot & 2)? 1 : 0, 0, 0, (prot & 0x80) ? 1 : 0, (prot & 1)? 1 : 0, 1);
	*entry |= (1 << 7);
	return (void*) virt;
}
void* paging_map_phys_to_virt(uint64_t virt, uint64_t phys, uint64_t prot)
{
	_Bool user = 0;
	if (virt < 0x00007fffffffffff)
		user = 1;
	if(!current_pml4)
		return NULL;
	decomposed_addr_t decAddr;
	memcpy(&decAddr, &virt, sizeof(decomposed_addr_t));
	PML4 *pml4;
	if(!is_spawning)
		pml4 = (PML4*)((uint64_t)current_pml4 + PHYS_BASE);
	else
		{
		pml4 = (PML4*)((uint64_t)spawning_pml + PHYS_BASE);}
	
	uint64_t* entry = &pml4->entries[decAddr.pml4];
	PML3* pml3 = NULL;
	PML2* pml2 = NULL;
	PML1* pml1 = NULL;
	/* If its present, use that pml3 */
	if(*entry & 1) {
		pml3 = (PML3*)(*entry & 0x0FFFFFFFFFFFF000);
	}
	else { /* Else create one */
		pml3 = (PML3*)bootmem_alloc(1);
		if(!pml3)
			return NULL;
		memset((void*)((uint64_t)pml3 + PHYS_BASE), 0, sizeof(PML3));
		*entry = make_pml4e((uint64_t)pml3, 0, 0, 0, user ? 1 : 0, 1, 1);
	}
	pml3 = (PML3*)((uint64_t)pml3 + PHYS_BASE);
	entry = &pml3->entries[decAddr.pdpt];
	if(*entry & 1) {
		pml2 = (PML2*)(*entry & 0x0FFFFFFFFFFFF000);
	}
	else {
		pml2 = (PML2*)bootmem_alloc(1);
		if(!pml2 )
			return NULL;
		memset((void*)((uint64_t)pml2 + PHYS_BASE), 0, sizeof(PML2));
		*entry = make_pml3e( (uint64_t)pml2, 0, 0, 0, 0, 0, user ? 1 : 0, 1, 1);
	}
	pml2 = (PML2*)((uint64_t)pml2 + PHYS_BASE);
	entry = &pml2->entries[decAddr.pd];
	if(*entry & 1) {
		pml1 = (PML1*)(*entry & 0x0FFFFFFFFFFFF000);
	}
	else {
		pml1 = (PML1*)bootmem_alloc(1);
		if(!pml1)
			return NULL;
		memset((void*)((uint64_t)pml1 + PHYS_BASE), 0, sizeof(PML1));
		*entry = make_pml2e( (uint64_t)pml1, (prot & 4), 0, (prot & 2)? 1 : 0, 0, 0, (prot & 0x80) ? 1 : 0, (prot & 1)? 1 : 0, 1);
	}
	pml1 = (PML1*)((uint64_t)pml1 + PHYS_BASE);
	entry = &pml1->entries[decAddr.pt];
	*entry = make_pml1e( phys, (prot & 4) ? 1 : 0, 0, (prot & 0x2) ? 1 : 0, 0, 0, (prot & 0x80) ? 1 : 0, (prot & 1) ? 1 : 0, 1);
	return (void*)virt;
}
void paging_unmap(void* memory)
{
	decomposed_addr_t dec;
	memcpy(&dec, &memory, sizeof(decomposed_addr_t));
	PML4 *pml4;
	if(!is_spawning)
		pml4 = (PML4*)((uint64_t)current_pml4 + PHYS_BASE);
	else
		pml4 = (PML4*)((uint64_t)spawning_pml + PHYS_BASE);
	uint64_t* entry = &pml4->entries[dec.pml4];
	PML3 *pml3 = (PML3*)((*entry & 0x0FFFFFFFFFFFF000) + PHYS_BASE);
	entry = &pml3->entries[dec.pdpt];
	PML2 *pml2 = (PML2*)((*entry & 0x0FFFFFFFFFFFF000) + PHYS_BASE);
	entry = &pml2->entries[dec.pd];
	PML1 *pml1 = (PML1*)((*entry & 0x0FFFFFFFFFFFF000) + PHYS_BASE);
	entry = &pml1->entries[dec.pt];
	//pfree(1, entry);
	*entry = 0;
}
PML4 *paging_clone_as()
{
	PML4 *new_pml = bootmem_alloc(1);
	if(!new_pml)
		panic("OOM while cloning address space!");
	PML4 *p = (PML4*)((uint64_t)new_pml + PHYS_BASE);
	memset(p, 0, sizeof(PML4));
	PML4 *curr = (PML4*)((uint64_t)current_pml4 + PHYS_BASE);
	// Clone the kernel-space memory
	memcpy(&p->entries[256], &curr->entries[256], 256 * sizeof(uint64_t));
	is_spawning = 1;
	spawning_pml = new_pml;
	return new_pml;
}
inline PML4 *paging_fork_pml(PML4 *pml, int entry)
{
	uint64_t old_address = PML_EXTRACT_ADDRESS(pml->entries[entry]);
	uint64_t perms = pml->entries[entry] & 0xF000000000000FFF;
	pml->entries[entry] = PML_EXTRACT_ADDRESS((uint64_t)bootmem_alloc(1)) | perms;
	PML4 *new_pml = (PML4*)((PML_EXTRACT_ADDRESS(pml->entries[entry])) + PHYS_BASE);
	PML4 *old_pml = (PML4*)(old_address + PHYS_BASE);
	memcpy(new_pml, old_pml, sizeof(PML4));
	return new_pml;
}
PML4 *paging_fork_as()
{
	PML4 *new_pml = bootmem_alloc(1);
	if(!new_pml)
		panic("OOM while cloning address space!");
	PML4 *p = (PML4*)((uint64_t)new_pml + PHYS_BASE);
	PML4 *curr = (PML4*)((uint64_t)current_pml4 + PHYS_BASE);
	memcpy(p, curr, sizeof(PML4));
	PML4 *mod_pml = (PML4*)((char*)new_pml + PHYS_BASE);
	for(int i = 0; i < 256; i++)
	{
		if(mod_pml->entries[i] & 1)
		{
			PML3 *pml3 = (PML3*)paging_fork_pml(mod_pml, i);
			for(int j = 0; j < PAGE_TABLE_ENTRIES; j++)
			{
				if(pml3->entries[j] & 1)
				{
					PML2 *pml2 = (PML2*)paging_fork_pml((PML4*) pml3, j);
					for(int k = 0; k < PAGE_TABLE_ENTRIES; k++)
					{
						if(pml2->entries[k] & 1 && !(pml2->entries[k] & (1<<7)))
						{
							PML1 *pml1 = (PML1*)paging_fork_pml((PML4*)pml2, k);
							for(int l = 0; l < PAGE_TABLE_ENTRIES; l++)
							{
								if(pml1->entries[l] & 1)
								{
									paging_fork_pml((PML4*)pml1, l);
								}
							}
						}
					}
				}
			}
		}
	}
	return new_pml;
}
void paging_stop_spawning()
{
	is_spawning = 0;
	spawning_pml = NULL;
}
extern process_t *first_process;
void paging_load_cr3(PML4 *pml)
{
	if(!pml)
	{
		printf("Trace: %p\n", __builtin_return_address(0));
		process_t *p = first_process;
		while(p)
		{
			printf("%p: ", p);
			printf("CR3: %p\n", p->cr3);
			p = p->next;
		}
		printf("current process: %p\n Current CR3: %p\n", current_process, current_process->cr3);
		abort();
	}
	asm volatile("movq %0, %%cr3"::"r"(pml));
	current_pml4 = pml;
}
void paging_change_perms(void *addr, int prot)
{
	decomposed_addr_t dec;
	memcpy(&dec, &addr, sizeof(decomposed_addr_t));
	PML4 *pml4;
	if(!is_spawning)
		pml4 = (PML4*)((uint64_t)current_pml4 + PHYS_BASE);
	else
		pml4 = (PML4*)((uint64_t)spawning_pml + PHYS_BASE);
	uint64_t* entry = &pml4->entries[dec.pml4];
	PML3 *pml3 = (PML3*)((*entry & 0x0FFFFFFFFFFFF000) + PHYS_BASE);
	entry = &pml3->entries[dec.pdpt];
	PML2 *pml2 = (PML2*)((*entry & 0x0FFFFFFFFFFFF000) + PHYS_BASE);
	entry = &pml2->entries[dec.pd];
	PML1 *pml1 = (PML1*)((*entry & 0x0FFFFFFFFFFFF000) + PHYS_BASE);
	entry = &pml1->entries[dec.pt];
	uint32_t perms = *entry & 0xF00000000000FFF;
	uint64_t page = PML_EXTRACT_ADDRESS(*entry);
	if(prot & VMM_NOEXEC)
		perms |= 0xF00000000000000;
	if(prot & VMM_WRITE)
		perms |= (1 << 1);
	*entry = perms | page;
}
