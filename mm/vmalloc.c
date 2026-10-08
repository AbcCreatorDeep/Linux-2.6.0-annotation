/*
 *  linux/mm/vmalloc.c
 *
 *  Copyright (C) 1993  Linus Torvalds
 *  Support of BIGMEM added by Gerhard Wichert, Siemens AG, July 1999
 *  SMP-safe vmalloc/vfree/ioremap, Tigran Aivazian <tigran@veritas.com>, May 2000
 *  Major rework to support vmap/vunmap, Christoph Hellwig, SGI, August 2002
 */

#include <linux/mm.h>
#include <linux/module.h>
#include <linux/highmem.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/interrupt.h>

#include <linux/vmalloc.h>

#include <asm/uaccess.h>
#include <asm/pgalloc.h>
#include <asm/tlbflush.h>


rwlock_t vmlist_lock = RW_LOCK_UNLOCKED;
struct vm_struct *vmlist;

static void unmap_area_pte(pmd_t *pmd, unsigned long address,
				  unsigned long size)
{
	unsigned long end;
	pte_t *pte;

	if (pmd_none(*pmd))
		return;
	if (pmd_bad(*pmd)) {
		pmd_ERROR(*pmd);
		pmd_clear(pmd);
		return;
	}

	pte = pte_offset_kernel(pmd, address);
	address &= ~PMD_MASK;
	end = address + size;
	if (end > PMD_SIZE)
		end = PMD_SIZE;

	do {
		pte_t page;
		page = ptep_get_and_clear(pte);
		address += PAGE_SIZE;
		pte++;
		if (pte_none(page))
			continue;
		if (pte_present(page))
			continue;
		printk(KERN_CRIT "Whee.. Swapped out page in kernel page table\n");
	} while (address < end);
}

static void unmap_area_pmd(pgd_t *dir, unsigned long address,
				  unsigned long size)
{
	unsigned long end;
	pmd_t *pmd;

	if (pgd_none(*dir))
		return;
	if (pgd_bad(*dir)) {
		pgd_ERROR(*dir);
		pgd_clear(dir);
		return;
	}

	pmd = pmd_offset(dir, address);
	address &= ~PGDIR_MASK;
	end = address + size;
	if (end > PGDIR_SIZE)
		end = PGDIR_SIZE;

	do {
		unmap_area_pte(pmd, address, end - address);
		address = (address + PMD_SIZE) & PMD_MASK;
		pmd++;
	} while (address < end);
}

static int map_area_pte(pte_t *pte, unsigned long address,
			       unsigned long size, pgprot_t prot,
			       struct page ***pages)
{
	unsigned long end;

	address &= ~PMD_MASK;
	end = address + size;
	if (end > PMD_SIZE)
		end = PMD_SIZE;

	do {
		struct page *page = **pages;

		WARN_ON(!pte_none(*pte));
		if (!page)
			return -ENOMEM;
		// 根据 x86 手册 中  pte 的表项，填进去就行了
		set_pte(pte, mk_pte(page, prot));
		address += PAGE_SIZE;
		pte++;
		(*pages)++;
	} while (address < end);
	return 0;
}

static int map_area_pmd(pmd_t *pmd, unsigned long address,
			       unsigned long size, pgprot_t prot,
			       struct page ***pages)
{
	unsigned long end;

	address &= ~PGDIR_MASK;
	end = address + size;
	if (end > PGDIR_SIZE)
		end = PGDIR_SIZE;
	// 将 alloc_page 函数返回的 物理页 的 地址 填入代表 虚拟地址的 PTE 中
	do {
		pte_t * pte = pte_alloc_kernel(&init_mm, pmd, address);
		if (!pte)
			return -ENOMEM;
		if (map_area_pte(pte, address, end - address, prot, pages))
			return -ENOMEM;
		address = (address + PMD_SIZE) & PMD_MASK;
		pmd++;
	} while (address < end);

	return 0;
}

void unmap_vm_area(struct vm_struct *area)
{
	unsigned long address = (unsigned long) area->addr;
	unsigned long end = (address + area->size);
	pgd_t *dir;

	dir = pgd_offset_k(address);
	flush_cache_vunmap(address, end);
	do {
		unmap_area_pmd(dir, address, end - address);
		address = (address + PGDIR_SIZE) & PGDIR_MASK;
		dir++;
	} while (address && (address < end));
	flush_tlb_kernel_range((unsigned long) area->addr, end);
}

int map_vm_area(struct vm_struct *area, pgprot_t prot, struct page ***pages)
{
	unsigned long address = (unsigned long) area->addr;
	unsigned long end = address + (area->size-PAGE_SIZE);
	pgd_t *dir;
	int err = 0;

	dir = pgd_offset_k(address);
	spin_lock(&init_mm.page_table_lock);
	do {
		pmd_t *pmd = pmd_alloc(&init_mm, dir, address);
		if (!pmd) {
			err = -ENOMEM;
			break;
		}
		if (map_area_pmd(pmd, address, end - address, prot, pages)) {
			err = -ENOMEM;
			break;
		}

		address = (address + PGDIR_SIZE) & PGDIR_MASK;
		dir++;
	} while (address && (address < end));

	spin_unlock(&init_mm.page_table_lock);
	flush_cache_vmap((unsigned long) area->addr, end);
	return err;
}

struct vm_struct *__get_vm_area(unsigned long size, unsigned long flags,
				unsigned long start, unsigned long end)
{
	struct vm_struct **p, *tmp, *area;
	unsigned long addr = start;

	area = kmalloc(sizeof(*area), GFP_KERNEL); // slab 分配 元数据结构
	if (unlikely(!area))
		return NULL;

	/*
	 * We always allocate a guard page.
	 */
	// 分配 警戒页，防止越界访问。基本预防，而不是 完全杜绝 内存溢出攻击
	// （完整防御，使用 gcc 编译器的 选项  -fstack-clash-protection 在分配大内存时 按页循环分配，而不是一次性分配多个页）
	size += PAGE_SIZE;
	if (unlikely(!size)) {
		kfree (area);
		return NULL;
	}

	// 因为 vmalloc  分配后，映射完成 ，会频繁的 CRUD  这些物理页帧，而不是 经常操作 vmlist 这个 虚拟内存的元数据，所以
	// 场景属于：读多写少，选择 读写自旋锁（操作时间很短的场景）
	write_lock(&vmlist_lock);
	// 找一个 非线性映射区中 vmalloc  中 可以用的虚拟地址空间
	for (p = &vmlist; (tmp = *p) ;p = &tmp->next) {
		if ((unsigned long)tmp->addr < addr)
			continue;
		if ((size + addr) < addr)
			goto out;
		if (size + addr <= (unsigned long)tmp->addr)
			goto found;
		addr = tmp->size + (unsigned long)tmp->addr;
		if (addr > end - size)
			goto out;
	}

found:
	area->next = *p;
	*p = area;

	area->flags = flags;
	area->addr = (void *)addr;
	area->size = size;
	area->pages = NULL;
	area->nr_pages = 0;
	area->phys_addr = 0;
	 
	write_unlock(&vmlist_lock);

	return area;

out:
	write_unlock(&vmlist_lock);
	kfree(area);
	return NULL;
}

/**
 *	get_vm_area  -  reserve a contingous kernel virtual area
 *
 *	@size:		size of the area
 *	@flags:		%VM_IOREMAP for I/O mappings or VM_ALLOC
 *
 *	Search an area of @size in the kernel virtual mapping area,
 *	and reserved it for out purposes.  Returns the area descriptor
 *	on success or %NULL on failure.
 */
struct vm_struct *get_vm_area(unsigned long size, unsigned long flags)
{

	/**
	 * 32位机上 非线性映射区 只有 128M， 64bit 没有限制，所以  vmalloc 可以分配的 连续虚拟内存 就需要有 地址 限制
	 */
	return __get_vm_area(size, flags, VMALLOC_START, VMALLOC_END);
}

/**
 *	remove_vm_area  -  find and remove a contingous kernel virtual area
 *
 *	@addr:		base address
 *
 *	Search for the kernel VM area starting at @addr, and remove it.
 *	This function returns the found VM area, but using it is NOT safe
 *	on SMP machines.
 */
struct vm_struct *remove_vm_area(void *addr)
{
	struct vm_struct **p, *tmp;

	write_lock(&vmlist_lock);
	for (p = &vmlist ; (tmp = *p) ;p = &tmp->next) {
		 if (tmp->addr == addr)
			 goto found;
	}
	write_unlock(&vmlist_lock);
	return NULL;

found:
	unmap_vm_area(tmp);
	*p = tmp->next;
	write_unlock(&vmlist_lock);
	return tmp;
}

void __vunmap(void *addr, int deallocate_pages)
{
	struct vm_struct *area;

	if (!addr)
		return;

	if ((PAGE_SIZE-1) & (unsigned long)addr) {
		printk(KERN_ERR "Trying to vfree() bad address (%p)\n", addr);
		return;
	}

	area = remove_vm_area(addr);
	if (unlikely(!area)) {
		printk(KERN_ERR "Trying to vfree() nonexistent vm area (%p)\n",
				addr);
		return;
	}
	
	if (deallocate_pages) {
		int i;

		for (i = 0; i < area->nr_pages; i++) {
			if (unlikely(!area->pages[i]))
				BUG();
			__free_page(area->pages[i]);
		}

		kfree(area->pages);
	}

	kfree(area);
	return;
}

/**
 *	vfree  -  release memory allocated by vmalloc()
 *
 *	@addr:		memory base address
 *
 *	Free the virtually contiguous memory area starting at @addr, as
 *	obtained from vmalloc(), vmalloc_32() or __vmalloc().
 *
 *	May not be called in interrupt context.
 */
void vfree(void *addr)
{
	BUG_ON(in_interrupt());
	__vunmap(addr, 1);
}

EXPORT_SYMBOL(vfree);

/**
 *	vunmap  -  release virtual mapping obtained by vmap()
 *
 *	@addr:		memory base address
 *
 *	Free the virtually contiguous memory area starting at @addr,
 *	which was created from the page array passed to vmap().
 *
 *	May not be called in interrupt context.
 */
void vunmap(void *addr)
{
	BUG_ON(in_interrupt());
	__vunmap(addr, 0);
}

EXPORT_SYMBOL(vunmap);

/**
 *	vmap  -  map an array of pages into virtually contiguous space
 *
 *	@pages:		array of page pointers
 *	@count:		number of pages to map
 *	@flags:		vm_area->flags
 *	@prot:		page protection for the mapping
 *
 *	Maps @count pages from @pages into contiguous kernel virtual
 *	space.
 */
void *vmap(struct page **pages, unsigned int count,
		unsigned long flags, pgprot_t prot)
{
	struct vm_struct *area;

	if (count > num_physpages)
		return NULL;

	area = get_vm_area((count << PAGE_SHIFT), flags);
	if (!area)
		return NULL;
	if (map_vm_area(area, prot, &pages)) {
		vunmap(area->addr);
		return NULL;
	}

	return area->addr;
}

EXPORT_SYMBOL(vmap);

/**
 *	__vmalloc  -  allocate virtually contiguous memory
 *
 *	@size:		allocation size
 *	@gfp_mask:	flags for the page level allocator
 *	@prot:		protection mask for the allocated pages
 *
 *	Allocate enough pages to cover @size from the page level
 *	allocator with @gfp_mask flags.  Map them into contiguous
 *	kernel virtual space, using a pagetable protection of @prot.
 */
void *__vmalloc(unsigned long size, int gfp_mask, pgprot_t prot)
{
	struct vm_struct *area;
	struct page **pages;
	unsigned int nr_pages, array_size, i;

	size = PAGE_ALIGN(size);
	if (!size || (size >> PAGE_SHIFT) > num_physpages)
		return NULL;

	area = get_vm_area(size, VM_ALLOC); // 分配表示 连续虚拟地址空间的 元数据结构 vm_struct，然后从 vmalloc 的分配区中找到可以使用的 连续虚拟地址空间
	if (!area)
		return NULL;

	nr_pages = size >> PAGE_SHIFT;
	array_size = (nr_pages * sizeof(struct page *)); // 分配的数组是存放 page 的指针

	area->nr_pages = nr_pages;
	area->pages = pages = kmalloc(array_size, (gfp_mask & ~__GFP_HIGHMEM));// 分配存放 持有的 page 页帧的数组

	if (!area->pages) {
		remove_vm_area(area->addr);
		kfree(area);
		return NULL;
	}

	// 这里是重点：为什么要 清 0？新分配的数据页，不干净，可能存在之前使用该页的 旧数据
	memset(area->pages, 0, array_size);

	// 前面已经计算了 vmalloc 需要分配的size 和 size/page size 的页数（这些页可以不连续）

	// 考虑下为什么 不这么写：page *pages= alloc_pages(area->nr_pages，order); 但是！ 我们需要的可以连续 可以不连续的页
	for (i = 0; i < area->nr_pages; i++) {
		area->pages[i] = alloc_page(gfp_mask); // 向伙伴算法 分配 一个物理页
		/**
		 * 该分支代码 几乎不执行，那么将 该 if 内的代码块 挪到 BPU 不会预测的代码路径中
		 * eg1:  静态分支预测器的行为: 默认 预测 jcc 后的代码
		 * jcc xxxx;
		 * i < area->nr_pages; i++
		 * ret;
		 * 
		 * xxxx:
		 * area->nr_pages = i;
		 * goto fail;
		 * ret;
		 * 
		 * eg2: 静态分支预测器的行为: 默认 预测 jcc 跳转的代码
		 * jcc xxxx;
		 * area->nr_pages = i;
		 * goto fail;
		 * xxxx:
		 * i < area->nr_pages; i++
		 */
		if (unlikely(!area->pages[i])) { 
			/* Successfully allocated i pages, free them in __vunmap() */
			area->nr_pages = i;
			goto fail;
		}
	}
	
	if (map_vm_area(area, prot, &pages))
		goto fail;
	return area->addr;

fail:
	vfree(area->addr);
	return NULL;
}

EXPORT_SYMBOL(__vmalloc);

/**
 *	vmalloc  -  allocate virtually contiguous memory
 *
 *	@size:		allocation size
 *
 *	Allocate enough pages to cover @size from the page level
 *	allocator and map them into contiguous kernel virtual space.
 *	从伙伴算法中找到 N 个 Page 的大小 满足 你的 size，然后 将他们的物理页地址 填入 连续虚拟地址空间的页表项中
 *	For tight cotrol over page level allocator and protection flags
 *	use __vmalloc() instead.
 */
void *vmalloc(unsigned long size)
{		// 因为 vmalloc 本身就很宽松，仅仅只是临时使用的内存而已，所以 标志位 也很宽松
       return __vmalloc(size, GFP_KERNEL | __GFP_HIGHMEM, PAGE_KERNEL);
}

EXPORT_SYMBOL(vmalloc);

/**
 *	vmalloc_32  -  allocate virtually contiguous memory (32bit addressable)
 *
 *	@size:		allocation size
 *
 *	Allocate enough 32bit PA addressable pages to cover @size from the
 *	page level allocator and map them into contiguous kernel virtual space.
 */
void *vmalloc_32(unsigned long size)
{
	return __vmalloc(size, GFP_KERNEL, PAGE_KERNEL);
}

EXPORT_SYMBOL(vmalloc_32);

long vread(char *buf, char *addr, unsigned long count)
{
	struct vm_struct *tmp;
	char *vaddr, *buf_start = buf;
	unsigned long n;

	/* Don't allow overflow */
	if ((unsigned long) addr + count < count)
		count = -(unsigned long) addr;

	read_lock(&vmlist_lock);
	for (tmp = vmlist; tmp; tmp = tmp->next) {
		vaddr = (char *) tmp->addr;
		if (addr >= vaddr + tmp->size - PAGE_SIZE)
			continue;
		while (addr < vaddr) {
			if (count == 0)
				goto finished;
			*buf = '\0';
			buf++;
			addr++;
			count--;
		}
		n = vaddr + tmp->size - PAGE_SIZE - addr;
		do {
			if (count == 0)
				goto finished;
			*buf = *addr;
			buf++;
			addr++;
			count--;
		} while (--n > 0);
	}
finished:
	read_unlock(&vmlist_lock);
	return buf - buf_start;
}

long vwrite(char *buf, char *addr, unsigned long count)
{
	struct vm_struct *tmp;
	char *vaddr, *buf_start = buf;
	unsigned long n;

	/* Don't allow overflow */
	if ((unsigned long) addr + count < count)
		count = -(unsigned long) addr;

	read_lock(&vmlist_lock);
	for (tmp = vmlist; tmp; tmp = tmp->next) {
		vaddr = (char *) tmp->addr;
		if (addr >= vaddr + tmp->size - PAGE_SIZE)
			continue;
		while (addr < vaddr) {
			if (count == 0)
				goto finished;
			buf++;
			addr++;
			count--;
		}
		n = vaddr + tmp->size - PAGE_SIZE - addr;
		do {
			if (count == 0)
				goto finished;
			*addr = *buf;
			buf++;
			addr++;
			count--;
		} while (--n > 0);
	}
finished:
	read_unlock(&vmlist_lock);
	return buf - buf_start;
}
