#include <type.h>
#include <memory.h>
#include <linkage.h>
#include <interrupt.h>
#include "mem_map.h"
#include "alloc.h"
#include <mm.h>
#include <printk.h>

unsigned long mm_pgd;

extern bool boot_alloc_ready;
extern bool page_alloc_ready;
extern bool slab_alloc_ready;


#define dsb() __asm__ __volatile__ ("mcr p15, 0, %0, c7, c10, 4" \
				    : : "r" (0) : "memory")

static inline void flush_pgd_entry(pgd_t *pgd)
{
  asm("mcr	p15, 0, %0, c7, c10, 1	@ flush_pmd"
    : : "r" (pgd) : "cc");

  /* not enabled on s3c6410 by default
  asm("mcr	p15, 1, %0, c15, c9, 1  @ L2 flush_pmd"
      : : "r" (pgd) : "cc");
  */

  dsb();
}

static void create_mapping_section (unsigned long physical, unsigned long virtual) {
  pgd_t *pgd = pgd_offset(((pgd_t *)mm_pgd), virtual);
  unsigned long property = (PMD_TYPE_SECT | PMD_SECT_AP_WRITE | PMD_SECT_AP_READ | PMD_FLAGS);

  //  unsigned long pgd_index = pgd_index(virtual);
  //  printk(PR_SS_MM, PR_LVL_DBG7, "%s: physical = %x, virtual = %x, pgd_index = %x\n", __func__, physical, virtual, pgd_index);  

  printk(PR_SS_MM, PR_LVL_DBG7, "%s: pgd = %x, *pgd = %x\n", __func__, pgd, (physical | property));  
  //  *pgd = (pgd_t)(physical | 0x031);
  *pgd = (pgd_t)(physical | property);
  flush_pgd_entry(pgd);
}


static pte_t *pte_offset(pte_t *pt, unsigned long virtual) {
  int index = (virtual & ~SECTION_MASK) >> PAGE_SHIFT;
  return (pt + index);
}

/* Each PT contains 256 items, and take 1K memory. But the size of each page is 4K, so each page hold 4 page table.
 * Here we're different from Linux, in which a page contains 2 linux tables and 2 hardware tables. As we don't support page swap in/out, so we don't need any extra information besides the hardware pagetable.
 */
static void create_mapping_page (unsigned long physical, unsigned long virtual) {
  pgd_t *pgd = pgd_offset(((pgd_t *)mm_pgd), virtual);
  pte_t *pte, pte_value, *pt = NULL;
  /* 4 continuous page table fall in the same page. */
  pgd_t *aligned_pgd = (pgd_t *)((unsigned long)pgd & 0xfffffff0);

  printk(PR_SS_MM, PR_LVL_DBG7, "%s: pgd = %x, virtual = %x\n", __func__, pgd, virtual);  
  printk(PR_SS_MM, PR_LVL_DBG7, "%s: aligned_pgd = %x\n", __func__, aligned_pgd);  

  if (NULL == *pgd) {
    /* populate pgd */
    pte = kmalloc(PAGE_SIZE);
	printk(PR_SS_MM, PR_LVL_DBG7, "%s: populating pgd: allocated page table: virt = %x, phys = %x\n", __func__, pte, __virt_to_phys((unsigned long)pte));  

	int i = 0;
	for (; i < 4; i++) {
	  pgd_t pgd_value = ((unsigned long)(__virt_to_phys((unsigned long)pte) + i * 256 * sizeof(pte_t))) | 0x01;
	  printk(PR_SS_MM, PR_LVL_DBG7, "%s: populating pgd: pgd = %x, *pgd = %x\n", __func__, &aligned_pgd[i], pgd_value);  
	  aligned_pgd[i] = pgd_value;
	  flush_pgd_entry(&aligned_pgd[i]);
	}
  } 

  pt = (pte_t *)(__phys_to_virt((unsigned long)*pgd) & KILOBYTES_MASK);
  /* populate pte */
  if (NULL == pt) {
	printk(PR_SS_MM, PR_LVL_ERR, "%s: page table not found: %x\n", __func__, pt);
	while(1);
  }

  pte = pte_offset(pt, virtual);
  pte_value = (physical & PAGE_MASK) | 0x032;
  //pte_value = (physical & PAGE_MASK) | 0x02a;
  printk(PR_SS_MM, PR_LVL_DBG7, "%s: pt = %x, pte = %x, *pte = %x\n", __func__, pt, pte, pte_value);  
  *pte = pte_value;
}

static void print_map_desc(struct map_desc *map) {
  printk(PR_SS_MM, PR_LVL_DBG7, "print_map_desc(): md = %x\n", map);  
  printk(PR_SS_MM, PR_LVL_DBG7, "physical = %x, virtual = %x, length = %x, type = %x\n", map->physical, map->virtual, map->length, map->type);  
}

static void create_mapping(struct map_desc *md) {
  printk(PR_SS_MM, PR_LVL_DBG7, "create_mapping():\n");
  print_map_desc(md);
  switch(md->type) {
  case MAP_DESC_TYPE_SECTION:
    /* Physical/virtual address and length have to be aligned to 1M. */
    if ((md->physical & (~SECTION_MASK)) || (md->virtual & (~SECTION_MASK)) || (md->length & (~SECTION_MASK))) {
      /* error */
	  printk(PR_SS_MM, PR_LVL_ERROR, "create_mapping(): section map_desc data not aligned \n");
      return;
    } else {
      unsigned long physical = md->physical; 
      unsigned long virtual = md->virtual;
	  /*
	  if (virtual == (EXCEPTION_BASE & SECTION_MASK)) {
		create_mapping_section(physical, virtual);
		return;
	  }
	  */
      while (virtual < (md->virtual + md->length)) {
		create_mapping_section(physical, virtual);
		physical += SECTION_SIZE;
		virtual += SECTION_SIZE;
      }
    }
    break;
  case MAP_DESC_TYPE_PAGE:
    /* Physical/virtual address and length have to be aligned to 4K. */
    if ((md->physical & (~PAGE_MASK)) || (md->virtual & (~PAGE_MASK)) || (md->length & (~PAGE_MASK))) {
      /* error */
	  printk(PR_SS_MM, PR_LVL_ERROR, "create_mapping(): page map_desc data not aligned \n");
      return;
    } else {
      unsigned long physical = md->physical; 
      unsigned long virtual = md->virtual;
      while (virtual < (md->virtual + md->length)) {
		create_mapping_page(physical, virtual);
		physical += PAGE_SIZE;
		virtual += PAGE_SIZE;
      }
    }
    break;
  default:
    /* error */
    break;
  }
}

static void map_low_memory() {
  struct map_desc map;
  map.physical = PHYS_OFFSET;
  map.virtual = PAGE_OFFSET;
  map.length = MEMORY_SIZE - EXCEPTION_SIZE;
  map.type = MAP_DESC_TYPE_SECTION;
  create_mapping(&map);
}

/*
static void map_vector_memory() {
  struct map_desc map;
  map.physical = PHYS_OFFSET + 63*SECTION_SIZE;
  map.virtual = EXCEPTION_BASE & SECTION_MASK;
  map.length = SECTION_SIZE;
  map.type = MAP_DESC_TYPE_SECTION;
  create_mapping(&map);
}
*/

static void map_vector_memory() {
  struct map_desc map;
  map.physical = __virt_to_phys((unsigned long)kmalloc(PAGE_SIZE));
  map.virtual = EXCEPTION_BASE;
  map.length = PAGE_SIZE;
  map.type = MAP_DESC_TYPE_PAGE;
  create_mapping(&map);
}

extern void * _debug_output_io;

static void map_debug_memory() {
  struct map_desc map;
  map.physical = 0x7f005020 & PAGE_MASK;
  map.virtual = 0xef005020 & PAGE_MASK;//(unsigned long)kmalloc(PAGE_SIZE);
  map.length = PAGE_SIZE;
  map.type = MAP_DESC_TYPE_PAGE;

  /* Have to disable printk, otherwise any printing before mapping finish will lead to mapping fault. */
  printk_disable();
  /* Remove existing section mapping: */
  pgd_t *pgd = pgd_offset(((pgd_t *)mm_pgd), map.virtual);
  *pgd = 0;
  /* Create new mapping. */
  create_mapping(&map);
  _debug_output_io = (void *)((map.virtual & PAGE_MASK) | (0x7f005020 & ~PAGE_MASK));
  printk_enable();
  printk(PR_SS_MM, PR_LVL_DBG7, "%s: debug io is mapped to %x\n", __func__, _debug_output_io);
}

static void map_vic_memory() {
  struct map_desc map;
  /* VIC0 */
  map.physical = 0x71200000;
  map.virtual = 0xe1200000;
  map.length = SECTION_SIZE;
  map.type = MAP_DESC_TYPE_SECTION;
  create_mapping(&map);
  /* VIC1 */
  map.physical = 0x71300000;
  map.virtual = 0xe1300000;
  map.length = SECTION_SIZE;
  map.type = MAP_DESC_TYPE_SECTION;
  create_mapping(&map);

}

static void map_timer_memory() {
  struct map_desc map;
  map.physical = 0x7f006000;
  map.virtual = S3C6410_TIMER_BASE;
  map.length = PAGE_SIZE;
  map.type = MAP_DESC_TYPE_PAGE;
  create_mapping(&map);
}


//////////////////////////////////////////////////////////////

struct membank {
	addr start;
	addr size;
};
#define bank_pfn_start(bank)	__phys_to_pfn((bank)->start)
#define bank_pfn_end(bank)	__phys_to_pfn((bank)->start + (bank)->size)
#define bank_pfn_size(bank)	((bank)->size >> PAGE_SHIFT)
#define bank_phys_start(bank)	(bank)->start
#define bank_phys_end(bank)	((bank)->start + (bank)->size)
#define bank_phys_size(bank)	(bank)->size


static void create_section_map_item(addr physical, addr virtual) {
  unsigned long table_item;
  addr *table_item_addr;

  /* site proterty */
  table_item = PMD_TYPE_SECT | PMD_SECT_AP_WRITE | PMD_SECT_AP_READ | PMD_FLAGS;

  /* ensure the physical address is aligned to 1M */
  physical = physical & 0xfff00000;

  /* construct the table item */
  table_item = table_item | physical;

  /* calculate the table item address */
  table_item_addr = (addr *)(PAGE_TABLE_ADDRESS + ((virtual - PAGE_OFFSET) >> MAP_ITEM_SHIFT) * 4);

  printk(PR_SS_MM, PR_LVL_DBG7, "create_mapping_section: pgd = %x, *pgd = %x\n", table_item_addr, table_item);  
  printk(PR_SS_MM, PR_LVL_DBG6, "create_section_map_item() : table_item = %x, table_item_addr = %x\n", table_item, table_item_addr);

  /* write mapping table */
  *table_item_addr = table_item;
}


static void flush_mmu() {
  addr page_table_addr = PAGE_TABLE_ADDRESS;
  asm("mcr	p15, 0, %0, c7, c10, 1	@ flush_pmd"
	  : : "r" (page_table_addr) : "cc");
  asm("mcr p15, 0, %0, c7, c10, 4"
	  : : "r" (0) : "memory");
}

void map_memory_bank(struct membank *bank)
{
	int items_count = map_length_to_count(bank->size);
	int i;
	addr physical, virtual;

	for (i = 0; i < items_count; i++) {
	  physical = bank->start + map_count_to_length(i);
	  virtual = __phys_to_virt(bank_phys_start(bank)) + i * MEGABYTES_SIZE;
	  printk(PR_SS_MM, PR_LVL_DBG6, "map_memory_bank() : physical = %x, virtual = %x\n", physical, virtual);
	  create_section_map_item(physical, virtual);
	}

	flush_mmu();
}




//////////////////////////////////////////////////////////////


void mm_init() {
  boot_alloc_ready = false;
  page_alloc_ready = false;
  slab_alloc_ready = false;

  mm_pgd = PAGE_OFFSET + PAGE_TABLE_OFFSET;

  if (0) {
	struct membank membank;
	membank.start = PHYS_OFFSET;
	membank.size = MEMORY_SIZE - EXCEPTION_SIZE;
	map_memory_bank(&membank);
  }

  if (1) {
	struct membank membank;
	membank.start = PHYS_OFFSET;
	membank.size = MEGABYTES_SIZE;
	map_memory_bank(&membank);
  }

  /* clear the page table at first*/
  // prepare_page_table();
  // printk(PR_SS_MM, PR_LVL_DBG7, "mm_init(): page table prepared\n");

  /* map main memory, lowmem in linux */
  if (0) map_low_memory();
  //printk(PR_SS_MM, PR_LVL_DBG7, "mm_init(): low memory mapped\n");

  bootmem_initialize();
  boot_alloc_ready = true;
  printk(PR_SS_MM, PR_LVL_DBG7, "mm_init(): boot memory allocator initialized\n");

  /* map vector page */
  if (0) map_vector_memory();
  /* map debug page */
  if (0) map_debug_memory();
  /* map VIC page */
  if (0) map_vic_memory();
  /* map timer IRQ page */
  if (0) map_timer_memory();

  //  bootmem_test();
  init_pages_map();

  page_alloc_init();
  //  pages_alloc_test();
  page_alloc_ready = true;

  bootmem_finalize();
  boot_alloc_ready = false;

  slab_alloc_init();
  slab_alloc_ready = true;
  //  slab_alloc_test();

}

/*
 * Map 64M memory for ListFS archived file:
 * Physical memory between 0x5800 0000 ~ 0x5C00 0000
 * Virtual memory between  0xC800 0000 ~ 0xCC00 0000
 * The mapping section index is between 128 ~ 191
 */
void map_fs_to_ram() {
  struct map_desc map;
  map.physical = 0x58000000;
  map.virtual = 0xC8000000;
  map.length = SECTION_SIZE*64;
  map.type = MAP_DESC_TYPE_SECTION;
  create_mapping(&map);
}