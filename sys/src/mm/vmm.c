#include <mm/vmm.h>
#include <mm/pmm.h>
#include <lib/assert.h>
#include <lib/module.h>
#include <lib/asm.h>
#include <lib/math.h>

#define VMM_DEBUG 0

MODULE("vmm");

static inline void __tlb_flush_single(uintptr_t virt) {
  ASMV("invlpg (%0)" :: "r" (virt) : "memory");
#if VMM_DEBUG
  PRINTK_SERIAL("[%s]: Invalidated %x\n", virt);
#endif
}

volatile struct limine_hhdm_request hhdm_request = {
  .id = LIMINE_HHDM_REQUEST,
  .revision = 0
};


static uintptr_t* get_next_level(uintptr_t* top_level, uint64_t index) {
  if ((top_level[index] & PTE_PRESENT)) {
    return ((uintptr_t*)PTE_GET_ADDR(top_level[index]));
  }

  /*
   *  Entry is not present so we
   *  will allocate our own entry.
   *
   */
  uintptr_t next_level = pmm_alloc();
  ASSERT(next_level != 0, "Could not allocate frame!\n");

  top_level[index] = (uintptr_t)next_level | PTE_PRESENT | PTE_WRITABLE;
  return (uintptr_t*)next_level;
}


void vmm_map_page(uintptr_t* pml4, uintptr_t vaddr, uintptr_t phys, uint64_t flags) {
  /*
   *  The logical address is like a key
   *  to a physical address.
   *
   *  Extract the indexes from the logical
   *  address for each paging structue.
   *
   */

  size_t pml4_index = (vaddr & ((size_t)0x1FF << 39)) >> 39;
  size_t pdpt_index = (vaddr & ((size_t)0x1FF << 30)) >> 30;
  size_t pd_index = (vaddr & ((size_t)0x1FF << 21)) >> 21;
  size_t pt_index = (vaddr & ((size_t)0x1FF << 12)) >> 12;

  /*
   *  Now we want to keep
   *  going down the structure 
   *  hirearchy until we get to
   *  the page table.
   *
   */

  uintptr_t* pdpt = get_next_level(pml4, pml4_index);
  uintptr_t* pd = get_next_level(pdpt, pdpt_index);
  uintptr_t* pt = get_next_level(pd, pd_index);
  pt[pt_index] = phys | flags;
  __tlb_flush_single(vaddr);

#if VMM_DEBUG
  PRINTK_SERIAL("[%s]: Logical address %x mapped to physical address %x with flags %x\n", MODULE_NAME, vaddr, phys, flags);
#endif
}


void vmm_unmap_page(uintptr_t* pml4, uintptr_t vaddr) {
  size_t pml4_index = (vaddr & ((size_t)0x1FF << 39)) >> 39;
  size_t pdpt_index = (vaddr & ((size_t)0x1FF << 30)) >> 30;
  size_t pd_index = (vaddr & ((size_t)0x1FF << 21)) >> 21;
  size_t pt_index = (vaddr & ((size_t)0x1FF << 12)) >> 12;

  uintptr_t* pdpt = get_next_level(pml4, pml4_index);
  uintptr_t* pd = get_next_level(pdpt, pdpt_index);
  uintptr_t* pt = get_next_level(pd, pd_index);
  pt[pt_index] = 0;
  __tlb_flush_single(vaddr);
}


uintptr_t vmm_get_phys(uintptr_t* pml4, uintptr_t vaddr) {
  size_t pml4_index = (vaddr & ((size_t)0x1FF << 39)) >> 39;
  size_t pdpt_index = (vaddr & ((size_t)0x1FF << 30)) >> 30;
  size_t pd_index = (vaddr & ((size_t)0x1FF << 21)) >> 21;
  size_t pt_index = (vaddr & ((size_t)0x1FF << 12)) >> 12;

  uintptr_t* pdpt = get_next_level(pml4, pml4_index);
  uintptr_t* pd = get_next_level(pdpt, pdpt_index);
  uintptr_t* pt = get_next_level(pd, pd_index);
  return PTE_GET_ADDR(pt[pt_index]);
}

uintptr_t get_cr3(void) {
  uintptr_t cr3_val = 0;
  ASMV("mov %%cr3, %0" : "=a" (cr3_val));
  return cr3_val;
}


uintptr_t mkpml4(void) {
  uint64_t* cr3_val = (uint64_t*)get_cr3();
  uint64_t* new = (uint64_t*)pmm_alloc();

  for (uint16_t i = 0; i < 512; ++i) {
    new[i] = cr3_val[i];
  }

  return (uintptr_t)new;
}

void* vmm_alloc_page(void) {
  uintptr_t ptr = pmm_alloc();
  if(ptr == 0) return 0;

  return (void*)ALIGN_DOWN((ptr + VMM_HIGHER_HALF), PAGE_SIZE);
}


void vmm_free_page(void* page) {
  uintptr_t addr = (uint64_t)page;
  pmm_free(addr - VMM_HIGHER_HALF);
}
