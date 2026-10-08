/*
 *  linux/arch/x86_64/mm/init.c
 *
 *  Copyright (C) 1995  Linus Torvalds
 *  Copyright (C) 2000  Pavel Machek <pavel@suse.cz>
 *  Copyright (C) 2002,2003 Andi Kleen <ak@suse.de>
 */

#include <linux/config.h>
#include <linux/signal.h>
#include <linux/sched.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/string.h>
#include <linux/types.h>
#include <linux/ptrace.h>
#include <linux/mman.h>
#include <linux/mm.h>
#include <linux/swap.h>
#include <linux/smp.h>
#include <linux/init.h>
#include <linux/pagemap.h>
#include <linux/bootmem.h>
#include <linux/proc_fs.h>

#include <asm/processor.h>
#include <asm/system.h>
#include <asm/uaccess.h>
#include <asm/pgtable.h>
#include <asm/pgalloc.h>
#include <asm/dma.h>
#include <asm/fixmap.h>
#include <asm/e820.h>
#include <asm/apic.h>
#include <asm/tlb.h>
#include <asm/mmu_context.h>
#include <asm/proto.h>
#include <asm/smp.h>

#define Dprintk(x...)

extern char _stext;

DEFINE_PER_CPU(struct mmu_gather, mmu_gathers);

/*
 * NOTE: pagetable_init alloc all the fixmap pagetables contiguous on the
 * physical space so we can cache the place of the first one and move
 * around without checking the pgd every time.
 */

void show_mem(void)
{
	int i, total = 0, reserved = 0;
	int shared = 0, cached = 0;
	pg_data_t *pgdat;
	struct page *page;

	printk("Mem-info:\n");
	show_free_areas();
	printk("Free swap:       %6dkB\n",nr_swap_pages<<(PAGE_SHIFT-10));

	for_each_pgdat(pgdat) {
               for (i = 0; i < pgdat->node_spanned_pages; ++i) {
                       page = pgdat->node_mem_map + i;
		total++;
                       if (PageReserved(page))
			reserved++;
                       else if (PageSwapCache(page))
			cached++;
                       else if (page_count(page))
                               shared += page_count(page) - 1;
               }
	}
	printk("%d pages of RAM\n", total);
	printk("%d reserved pages\n",reserved);
	printk("%d pages shared\n",shared);
	printk("%d pages swap cached\n",cached);
}

/* References to section boundaries */

extern char _text, _etext, _edata, __bss_start, _end;
extern char __init_begin, __init_end;

int after_bootmem;

static void *spp_getpage(void)
{ 
	void *ptr;
	if (after_bootmem)
		ptr = (void *) get_zeroed_page(GFP_ATOMIC); 
	else
		ptr = alloc_bootmem_pages(PAGE_SIZE);
	if (!ptr || ((unsigned long)ptr & ~PAGE_MASK))
		panic("set_pte_phys: cannot allocate page data %s\n", after_bootmem?"after bootmem":"");

	Dprintk("spp_getpage %p\n", ptr);
	return ptr;
} 

static void set_pte_phys(unsigned long vaddr,
			 unsigned long phys, pgprot_t prot)
{
	pml4_t *level4;
	pgd_t *pgd;
	pmd_t *pmd;
	pte_t *pte, new_pte;

	Dprintk("set_pte_phys %lx to %lx\n", vaddr, phys);

	level4 = pml4_offset_k(vaddr);
	if (pml4_none(*level4)) {
		printk("PML4 FIXMAP MISSING, it should be setup in head.S!\n");
		return;
	}
	pgd = level3_offset_k(level4, vaddr);
	if (pgd_none(*pgd)) {
		pmd = (pmd_t *) spp_getpage(); 
		set_pgd(pgd, __pgd(__pa(pmd) | _KERNPG_TABLE | _PAGE_USER));
		if (pmd != pmd_offset(pgd, 0)) {
			printk("PAGETABLE BUG #01! %p <-> %p\n", pmd, pmd_offset(pgd,0));
			return;
		}
	}
	pmd = pmd_offset(pgd, vaddr);
	if (pmd_none(*pmd)) {
		pte = (pte_t *) spp_getpage();
		set_pmd(pmd, __pmd(__pa(pte) | _KERNPG_TABLE | _PAGE_USER));
		if (pte != pte_offset_kernel(pmd, 0)) {
			printk("PAGETABLE BUG #02!\n");
			return;
		}
	}
	new_pte = pfn_pte(phys >> PAGE_SHIFT, prot);

	pte = pte_offset_kernel(pmd, vaddr);
	if (!pte_none(*pte) &&
	    pte_val(*pte) != (pte_val(new_pte) & __supported_pte_mask))
		pte_ERROR(*pte);
	set_pte(pte, new_pte);

	/*
	 * It's enough to flush this one mapping.
	 * (PGE mappings get flushed as well)
	 */
	__flush_tlb_one(vaddr);
}

/* NOTE: this is meant to be run only at boot */
void __set_fixmap (enum fixed_addresses idx, unsigned long phys, pgprot_t prot)
{
	unsigned long address = __fix_to_virt(idx);

	if (idx >= __end_of_fixed_addresses) {
		printk("Invalid __set_fixmap\n");
		return;
	}
	set_pte_phys(address, phys, prot);
}

unsigned long __initdata table_start, table_end; 

extern pmd_t temp_boot_pmds[]; 

static  struct c { 
	pmd_t *pmd;
	void  *address; 
	int    allocated; 
} temp_mappings[] __initdata = { 
	{ &temp_boot_pmds[0], (void *)(40UL * 1024 * 1024) },
	{ &temp_boot_pmds[1], (void *)(42UL * 1024 * 1024) }, 
	{}
}; // head.S 的临时映射是： 0 - 40M 

static __init void *alloc_low_page(int *index, unsigned long *phys) 
{ 
	struct temp_map *ti;
	int i; 
	// table_end 从低内存 8000 -> 10000 可以分配页的 地址，前面已经转为了页帧，所以 这里的 table_end位页帧号
	unsigned long pfn = table_end++, paddr; 
	void *adr;

	if (pfn >= end_pfn) 
		panic("alloc_low_page: ran out of memory"); 

	// 我现在只有 物理页帧号，后续想要往这个数据页 去写数据，CPU 就需要 虚拟地址
	// 而 现在 的 PAGE_OFFSET 的 虚拟地址的映射 还没有起来 ，那怎么访问呢？

	// 找两个在 head.S 中预留的2个 虚拟地址 的映射区，临时映射 4KB 的页
	for (i = 0; temp_mappings[i].allocated; i++) {
		if (!temp_mappings[i].pmd) 
			panic("alloc_low_page: ran out of temp mappings"); 
	} 
	ti = &temp_mappings[i];
	paddr = (pfn << PAGE_SHIFT) & PMD_MASK;   // 该 4kb 页的物理地址（该物理地址一定对齐到 2MB 处）
	set_pmd(ti->pmd, __pmd(paddr | _KERNPG_TABLE | _PAGE_PSE)); // _PAGE_PSE 置1 ，手册说了这就是代表 2MB 的页
	// 这里只要一填到临时虚拟内存映射区后，就可以直接通过 虚拟地址 访问了
	ti->allocated = 1; 
	// 记住： 只要修改了页表，那就刷 TLB。怎么刷？重置 CR3 寄存器
	__flush_tlb(); 	// 必须刷新 TLB ，为什么？TLB 不会感知 页表发生了变化，所以必须强制刷新       
	adr = ti->address + ((pfn << PAGE_SHIFT) & ~PMD_MASK); 
	*index = i; 
	*phys  = pfn * PAGE_SIZE;  
	return adr; 
} 

static __init void unmap_low_page(int i)
{ 
	struct temp_map *ti = &temp_mappings[i];
	set_pmd(ti->pmd, __pmd(0));
	ti->allocated = 0; 
} 

// pgd 就是 要初始化的 level 3 
static void __init phys_pgd_init(pgd_t *pgd, unsigned long address, unsigned long end)
{ 
	long i, j; 

	i = pgd_index(address);
	pgd = pgd + i;
	for (; i < PTRS_PER_PGD; pgd++, i++) {
		int map; 
		unsigned long paddr, pmd_phys;
		pmd_t *pmd;

		paddr = (address & PML4_MASK) + i*PGDIR_SIZE;
		if (paddr >= end) { 
			for (; i < PTRS_PER_PGD; i++, pgd++) 
				set_pgd(pgd, __pgd(0)); 
			break;
		} 

		if (!e820_mapped(paddr, paddr+PGDIR_SIZE, 0)) { 
			set_pgd(pgd, __pgd(0)); 
			continue;
		} 
		// 临时映射

		// 分配 PMD 的4KB 页
		pmd = alloc_low_page(&map, &pmd_phys);

		// 将 PMD 的 4kb 的页 填写到 PGD 页中
		set_pgd(pgd, __pgd(pmd_phys | _KERNPG_TABLE));

		/**
		 * 填写 PMD 页表中的 512个 2MB 的映射页的 PMD ENTRY #define PMD_SIZE	(1UL << 21) => 2M
		 *  PTRS_PER_PMD = 512 项
		 * */ 
		for (j = 0; j < PTRS_PER_PMD; pmd++, j++, paddr += PMD_SIZE) {
			unsigned long pe;

			if (paddr >= end) {  // 超出 物理内存了
				for (; j < PTRS_PER_PMD; j++, pmd++)
					set_pmd(pmd,  __pmd(0)); 
				break;
			}
			// 构建 PMD 映射到物理内存2M 的 PMD entry 项 ，_PAGE_PSE 置 1 ，所以 映射 2M
			// _PAGE_GLOBAL 置 1 ，设置为 全局页后，一直驻留在 TLB 中，不刷新 paddr 为 物理内存物理地址 2MB 对齐
			pe = _PAGE_NX|_PAGE_PSE | _KERNPG_TABLE | _PAGE_GLOBAL | paddr;
			pe &= __supported_pte_mask;
			set_pmd(pmd, __pmd(pe));
		}

		// 解除临时映射
		unmap_low_page(map);
	}
	__flush_tlb();
} 

/* Setup the direct mapping of the physical memory at PAGE_OFFSET.
   This runs before bootmem is initialized and gets pages directly from the 
   physical memory. To access them they are temporarily mapped. */
void __init init_memory_mapping(void) 
{ 
	unsigned long adr;	       
	unsigned long end;
	unsigned long next; 
	unsigned long pgds, pmds, tables; 

	Dprintk("init_memory_mapping\n");

	
	end = end_pfn_map << PAGE_SHIFT;

	/* 
	 * Find space for the kernel direct mapping tables.
	 * Later we should allocate these tables in the local node of the memory
	 * mapped.  Unfortunately this is done currently before the nodes are 
	 * discovered.
	 */

	pgds = (end + PGDIR_SIZE - 1) >> PGDIR_SHIFT;
	pmds = (end + PMD_SIZE - 1) >> PMD_SHIFT; 
	tables = round_up(pgds * 8, PAGE_SIZE) + round_up(pmds * 8, PAGE_SIZE); 
    // 要构建 虚拟内存映射，必须要找到存放 页表的页 ，找到一个 4KB 页 ，放 根目录了就行
	// 但是，原来我们不是在 head.S 中 就已经构建了一个 全局页目录么？
	// CR3 指向该根页
	// ENTRY(init_level4_pgt) // 总共有 512 项 页表项，使得 我们可以使用 48bit 虚拟内存中的高 9 位 作为索引找到页表项
	// 这里只有 48 bit  可用，不是 64 bit 
	// .quad	0x0000000000102007		/* -> level3_ident_pgt */  
	 // 第 0 项， 对应的虚拟地址二进制: 0000 00000 未知数 低端虚拟内存映射表项
	// .fill	255,8,0
	// .quad	0x000000000010a007 // 中间的虚拟内存段映射表项
	// .fill	254,8,0
	/* (2^48-(2*1024*1024*1024))/(2^39) = 511  一个 4KB 的页中能存放 512 个 表项 */ 
	// .quad	0x0000000000103007		/* -> level3_kernel_pgt */   0xffffffff80000000 的 40M 的映射
	// 最后 一 项， 对应的虚拟地址二进制: 1111 11111  未知数 高端虚拟内存映射表项

	// 充分利用 1M 以下的碎片地址，充分利用内存，减少 1M 以内的碎片问题
	// 以 物理内存为 8GB 来举例，这里需要 9个 4kb 的页，一个用来存 PGD ，8个用来存 PMD ，总共 就需要 36KB 的空间
	table_start = find_e820_area(0x8000, __pa_symbol(&_text), tables); 
	if (table_start == -1UL) 
		panic("Cannot find space for the kernel page tables"); 

	table_start >>= PAGE_SHIFT; 
	table_end = table_start;
       
	// 将 物理内存的 8GB 的顶点 结束地址 ，转为 虚拟地址
	// __PAGE_OFFSET           0x0000010000000000   0000 00010
	// 0x0000010000000000[物理地址 0GB] -  0x0000010200000000[物理地址 8GB]
	end += __PAGE_OFFSET; /* turn virtual */  	


	// 循环 分配 页表
	/**
	 * 1、在 init_level4_pgt 中 找一项 存 PGD : 一项 能覆盖 512GB 的物理地址
	 * 2、在 PGD 中 保存 PMD 地址信息: 一项 能覆盖 1GB 的物理地址
	 * 3、在 PMD 中 保存 2MB 的物理页帧信息 ： 一项能 覆盖 2MB 的物理地址
	 * 
	 */
	for (adr = PAGE_OFFSET; adr < end; adr = next) { 
		int map;
		unsigned long pgd_phys; 
		pgd_t *pgd = alloc_low_page(&map, &pgd_phys); // 从刚才 table_start 中分配 一页 来作为 PGD 的 4KB 页
		next = adr + PML4_SIZE; // 512 GB
		if (next > end) 
			next = end; 

		phys_pgd_init(pgd, adr-PAGE_OFFSET, next-PAGE_OFFSET);  // 初始化 PGD -> PMD

		set_pml4(init_level4_pgt + pml4_index(adr), mk_kernel_pml4(pgd_phys));

		unmap_low_page(map);   
	} 

	asm volatile("movq %%cr4,%0" : "=r" (mmu_cr4_features));
	__flush_tlb_all();
	early_printk("kernel direct mapping tables upto %lx @ %lx-%lx\n", end, 
	       table_start<<PAGE_SHIFT, 
	       table_end<<PAGE_SHIFT);
}

extern struct x8664_pda cpu_pda[NR_CPUS];

static unsigned long low_pml4[NR_CPUS];

void swap_low_mappings(void)
{
	int i;
	for (i = 0; i < NR_CPUS; i++) {
	        unsigned long t;
		if (!cpu_pda[i].level4_pgt) 
			continue;
		t = cpu_pda[i].level4_pgt[0];
		cpu_pda[i].level4_pgt[0] = low_pml4[i];
		low_pml4[i] = t;
	}
	flush_tlb_all();
}

void zap_low_mappings(void)
{
	swap_low_mappings();
}

#ifndef CONFIG_DISCONTIGMEM
void __init paging_init(void)
{
	{
		unsigned long zones_size[MAX_NR_ZONES] = {0, 0, 0};
		unsigned int max_dma;

		max_dma = virt_to_phys((char *)MAX_DMA_ADDRESS) >> PAGE_SHIFT;

		if (end_pfn < max_dma)
			zones_size[ZONE_DMA] = end_pfn;
		else {
			zones_size[ZONE_DMA] = max_dma;
			zones_size[ZONE_NORMAL] = end_pfn - max_dma;
		}
		free_area_init(zones_size);
	}
	return;
}
#endif

/* Unmap a kernel mapping if it exists. This is useful to avoid prefetches
   from the CPU leading to inconsistent cache lines. address and size
   must be aligned to 2MB boundaries. 
   Does nothing when the mapping doesn't exist. */
void __init clear_kernel_mapping(unsigned long address, unsigned long size) 
{
	unsigned long end = address + size;

	BUG_ON(address & ~LARGE_PAGE_MASK);
	BUG_ON(size & ~LARGE_PAGE_MASK); 
	
	for (; address < end; address += LARGE_PAGE_SIZE) { 
		pgd_t *pgd = pgd_offset_k(address);
               pmd_t *pmd;
		if (!pgd || pgd_none(*pgd))
			continue; 
               pmd = pmd_offset(pgd, address);
		if (!pmd || pmd_none(*pmd))
			continue; 
		if (0 == (pmd_val(*pmd) & _PAGE_PSE)) { 
			/* Could handle this, but it should not happen currently. */
			printk(KERN_ERR 
	       "clear_kernel_mapping: mapping has been split. will leak memory\n"); 
			pmd_ERROR(*pmd); 
		}
		set_pmd(pmd, __pmd(0)); 		
	}
	__flush_tlb_all();
} 

static inline int page_is_ram (unsigned long pagenr)
{
	int i;

	for (i = 0; i < e820.nr_map; i++) {
		unsigned long addr, end;

		if (e820.map[i].type != E820_RAM)	/* not usable memory */
			continue;
		/*
		 *	!!!FIXME!!! Some BIOSen report areas as RAM that
		 *	are not. Notably the 640->1Mb area. We need a sanity
		 *	check here.
		 */
		addr = (e820.map[i].addr+PAGE_SIZE-1) >> PAGE_SHIFT;
		end = (e820.map[i].addr+e820.map[i].size) >> PAGE_SHIFT;
		if  ((pagenr >= addr) && (pagenr < end))
			return 1;
	}
	return 0;
}

static struct kcore_list kcore_mem, kcore_vmalloc, kcore_kernel, kcore_modules,
			 kcore_vsyscall;

void __init mem_init(void)
{
	int codesize, reservedpages, datasize, initsize;
	int tmp;

	/* How many end-of-memory variables you have, grandma! */
	max_low_pfn = end_pfn;
	max_pfn = end_pfn;
	num_physpages = end_pfn;
	high_memory = (void *) __va(end_pfn * PAGE_SIZE);

	/* clear the zero-page */
	memset(empty_zero_page, 0, PAGE_SIZE);

	reservedpages = 0;

	/* this will put all low memory onto the freelists */
#ifdef CONFIG_DISCONTIGMEM
	totalram_pages += numa_free_all_bootmem();
	tmp = 0;
	/* should count reserved pages here for all nodes */ 
#else
	max_mapnr = end_pfn;
	if (!mem_map) BUG();

	totalram_pages += free_all_bootmem();

	for (tmp = 0; tmp < end_pfn; tmp++)
		/*
		 * Only count reserved RAM pages
		 */
		if (page_is_ram(tmp) && PageReserved(mem_map+tmp))
			reservedpages++;
#endif

	after_bootmem = 1;

	codesize =  (unsigned long) &_etext - (unsigned long) &_text;
	datasize =  (unsigned long) &_edata - (unsigned long) &_etext;
	initsize =  (unsigned long) &__init_end - (unsigned long) &__init_begin;

	/* Register memory areas for /proc/kcore */
	kclist_add(&kcore_mem, __va(0), max_low_pfn << PAGE_SHIFT); 
	kclist_add(&kcore_vmalloc, (void *)VMALLOC_START, 
		   VMALLOC_END-VMALLOC_START);
	kclist_add(&kcore_kernel, &_stext, &_end - &_stext); 
	kclist_add(&kcore_modules, (void *)MODULES_VADDR, MODULES_LEN);
	kclist_add(&kcore_vsyscall, (void *)VSYSCALL_START, 
				 VSYSCALL_END - VSYSCALL_START);

	printk("Memory: %luk/%luk available (%dk kernel code, %dk reserved, %dk data, %dk init)\n",
		(unsigned long) nr_free_pages() << (PAGE_SHIFT-10),
		end_pfn << (PAGE_SHIFT-10),
		codesize >> 10,
		reservedpages << (PAGE_SHIFT-10),
		datasize >> 10,
		initsize >> 10);

	/*
	 * Subtle. SMP is doing its boot stuff late (because it has to
	 * fork idle threads) - but it also needs low mappings for the
	 * protected-mode entry to work. We zap these entries only after
	 * the WP-bit has been tested.
	 */
#ifndef CONFIG_SMP
	zap_low_mappings();
#endif
}

void free_initmem(void)
{
	unsigned long addr;

	addr = (unsigned long)(&__init_begin);
	for (; addr < (unsigned long)(&__init_end); addr += PAGE_SIZE) {
		ClearPageReserved(virt_to_page(addr));
		set_page_count(virt_to_page(addr), 1);
#ifdef CONFIG_INIT_DEBUG
		memset((void *)(addr & ~(PAGE_SIZE-1)), 0xcc, PAGE_SIZE); 
#endif
		free_page(addr);
		totalram_pages++;
	}
	printk ("Freeing unused kernel memory: %luk freed\n", (&__init_end - &__init_begin) >> 10);
}

#ifdef CONFIG_BLK_DEV_INITRD
void free_initrd_mem(unsigned long start, unsigned long end)
{
	if (start < (unsigned long)&_end)
		return;
	printk ("Freeing initrd memory: %ldk freed\n", (end - start) >> 10);
	for (; start < end; start += PAGE_SIZE) {
		ClearPageReserved(virt_to_page(start));
		set_page_count(virt_to_page(start), 1);
		free_page(start);
		totalram_pages++;
	}
}
#endif

void __init reserve_bootmem_generic(unsigned long phys, unsigned len) 
{ 
	/* Should check here against the e820 map to avoid double free */ 
#ifdef CONFIG_DISCONTIGMEM
	int nid = phys_to_nid(phys);
	if (phys < HIGH_MEMORY && nid) 
		panic("reserve of %lx at node %d", phys, nid);
	reserve_bootmem_node(NODE_DATA(nid), phys, len);
#else       		
	reserve_bootmem(phys, len);    
#endif
}

int kern_addr_valid(unsigned long addr) 
{ 
	unsigned long above = ((long)addr) >> __VIRTUAL_MASK_SHIFT;
       pml4_t *pml4;
       pgd_t *pgd;
       pmd_t *pmd;
       pte_t *pte;

	if (above != 0 && above != -1UL)
		return 0; 
	
       pml4 = pml4_offset_k(addr);
	if (pml4_none(*pml4))
		return 0;

       pgd = pgd_offset_k(addr);
	if (pgd_none(*pgd))
		return 0; 

       pmd = pmd_offset(pgd, addr);
	if (pmd_none(*pmd))
		return 0;
	if (pmd_large(*pmd))
		return pfn_valid(pmd_pfn(*pmd));

       pte = pte_offset_kernel(pmd, addr);
	if (pte_none(*pte))
		return 0;
	return pfn_valid(pte_pfn(*pte));
}
