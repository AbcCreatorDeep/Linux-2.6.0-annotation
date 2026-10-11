#ifndef _X8664_TLBFLUSH_H
#define _X8664_TLBFLUSH_H

#include <linux/config.h>
#include <linux/mm.h>
#include <asm/processor.h>

#define __flush_tlb()							\
	do {								\
		unsigned long tmpreg;					\
									\
		__asm__ __volatile__(					\
			"movq %%cr3, %0;  # flush TLB \n"		\
			"movq %0, %%cr3;              \n"		\
			: "=r" (tmpreg)					\
			:: "memory");					\
	} while (0)

/*
 * Global pages have to be flushed a bit differently. Not a real
 * performance problem because this does not happen often.
 */
#define __flush_tlb_global()						\
	do {								\
		unsigned long tmpreg;					\
		// 	保证所有情况下 都会刷新 TLB 高版本已经纠正
		// 在 支持 CR4 寄存器的 CPU 上直接 重置 CR4 的 PGE 位来刷新 TLB 全局条目
	    //   if (cr4 & X86_CR4_PGE) {
		//		clear PGE and flush TLB of all entries */
		//		native_write_cr4(cr4 & ~X86_CR4_PGE);
				/* restore PGE as it was before */
		//		ative_write_cr4(cr4);
		//	} else {	 // 在 不支持 CR4 寄存器的 CPU 上直接 重置 CR3 来刷新 TLB 条目
				/* do it with cr3, letting kaiser flush user PCID */
		//			__native_flush_tlb();
		//		}
	
		__asm__ __volatile__(					\
			"movq %1, %%cr4;  # turn off PGE     \n"	\
			// 目的： 刷新 CR3 寄存器（ 重置 CR3 寄存器 ），从而刷新TLB 使得上面修改的页表生效
			"movq %%cr3, %0;  # flush TLB        \n"	\
			"movq %0, %%cr3;                     \n"	\
			// 刷新 CR4
			"movq %2, %%cr4;  # turn PGE back on \n"	\
			: "=&r" (tmpreg)				\
			: "r" (mmu_cr4_features & ~X86_CR4_PGE),	\
			  "r" (mmu_cr4_features)			\
			: "memory");					\
	} while (0)

extern unsigned long pgkern_mask;

#define __flush_tlb_all() __flush_tlb_global()

#define __flush_tlb_one(addr) \
	__asm__ __volatile__("invlpg %0": :"m" (*(char *) addr))


/*
 * TLB flushing:
 *
 *  - flush_tlb() flushes the current mm struct TLBs
 *  - flush_tlb_all() flushes all processes TLBs
 *  - flush_tlb_mm(mm) flushes the specified mm context TLB's
 *  - flush_tlb_page(vma, vmaddr) flushes one page
 *  - flush_tlb_range(vma, start, end) flushes a range of pages
 *  - flush_tlb_kernel_range(start, end) flushes a range of kernel pages
 *  - flush_tlb_pgtables(mm, start, end) flushes a range of page tables
 *
 * ..but the x86_64 has somewhat limited tlb flushing capabilities,
 * and page-granular flushes are available only on i486 and up.
 */

#ifndef CONFIG_SMP

#define flush_tlb() __flush_tlb()
#define flush_tlb_all() __flush_tlb_all()
#define local_flush_tlb() __flush_tlb()

static inline void flush_tlb_mm(struct mm_struct *mm)
{
	if (mm == current->active_mm)
		__flush_tlb();
}

static inline void flush_tlb_page(struct vm_area_struct *vma,
	unsigned long addr)
{
	if (vma->vm_mm == current->active_mm)
		__flush_tlb_one(addr);
}

static inline void flush_tlb_range(struct vm_area_struct *vma,
	unsigned long start, unsigned long end)
{
	if (vma->vm_mm == current->active_mm)
		__flush_tlb();
}

#else

#include <asm/smp.h>

#define local_flush_tlb() \
	__flush_tlb()

extern void flush_tlb_all(void);
extern void flush_tlb_current_task(void);
extern void flush_tlb_mm(struct mm_struct *);
extern void flush_tlb_page(struct vm_area_struct *, unsigned long);

#define flush_tlb()	flush_tlb_current_task()

static inline void flush_tlb_range(struct vm_area_struct * vma, unsigned long start, unsigned long end)
{
	flush_tlb_mm(vma->vm_mm);
}

#define TLBSTATE_OK	1
#define TLBSTATE_LAZY	2

#endif

#define flush_tlb_kernel_range(start, end) flush_tlb_all()

static inline void flush_tlb_pgtables(struct mm_struct *mm,
				      unsigned long start, unsigned long end)
{
	/* x86_64 does not keep any page table caches in TLB */
}

#endif /* _X8664_TLBFLUSH_H */
