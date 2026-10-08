/*
 *  linux/arch/x86_64/kernel/head.S -- start in 32bit and switch to 64bit
 *
 *  Copyright (C) 2000 Andrea Arcangeli <andrea@suse.de> SuSE
 *  Copyright (C) 2000 Pavel Machek <pavel@suse.cz>
 *  Copyright (C) 2000 Karsten Keil <kkeil@suse.de>
 *  Copyright (C) 2001,2002 Andi Kleen <ak@suse.de>
 *
 *  $Id: head.S,v 1.49 2002/03/19 17:39:25 ak Exp $
 */


#include <linux/linkage.h>
#include <linux/threads.h>
#include <asm/desc.h>
#include <asm/segment.h>
#include <asm/page.h>
#include <asm/msr.h>
	
/* we are not able to switch in one step to the final KERNEL ADRESS SPACE
 * because we need identity-mapped pages on setup so define __START_KERNEL to
 * 0x100000 for this stage
 * 
 */

	.text
	.code32
/* %bx:	 1 if coming from smp trampoline on secondary cpu  从这里开始，就是  100000 处的 解压后的 内核代码*/ 
startup_32: 
	
	/*
	 * At this point the CPU runs in 32bit protected mode (CS.D = 1) with
	 * paging disabled and the point of this file is to switch to 64bit
	 * long mode with a kernel mapping for kerneland to jump into the
	 * kernel virtual addresses.
 	 * There is no stack until we set one up.
	 */

	// 内核 当前没有栈，无所谓，因为 没有C语言代码，都是汇编
	movl %ebx,%ebp	/* Save trampoline flag */
	
	/* If the CPU doesn't support CPUID this will double fault.
	 * Unfortunately it is hard to check for CPUID without a stack. 
	 */
	
	// 检测当前CPU 是否支持 64 bit 模式
	/* Check if extended functions are implemented */		
	movl	$0x80000000, %eax
	cpuid
	cmpl	$0x80000000, %eax
	jbe	no_long_mode
	/* Check if long mode is implemented */
	mov	$0x80000001, %eax
	cpuid
	btl	$29, %edx
	jnc	no_long_mode

	movl	%edx,%edi
	
	/* 
	 * Prepare for entering 64bits mode  当前CPU 支持 64 bit 长模式
	 */

	// 对着 INTEL 开发手册 写就行了，无需 研究 为什么这么写，因为 Intel 就要你这样设置 CRX 寄存器 才能开启分页
	/* Enable PAE mode and PGE */
	xorl	%eax, %eax
	btsl	$5, %eax
	btsl	$7, %eax
	movl	%eax, %cr4

	/* Setup early boot stage 4 level pagetables */
	// 一旦开 PGE，立马 CPU 就把 指令缓存 和 RIP 当成 虚拟地址对待了，所以 必须立马来一个页表
	// 细节： 控制寄存器 CR X  的 操作： 必须由一个 寄存器中转
	// CR 3 寄存器 存放的是 顶级页表的物理地址

	movl	$(init_level4_pgt - __START_KERNEL_map), %eax
	movl	%eax, %cr3 


	// INTEL 手册写了，要开启长模式 就写 MSR  寄存器中的 特定位
	/* Setup EFER (Extended Feature Enable Register) */
	movl	$MSR_EFER, %ecx
	rdmsr

	/* Enable Long Mode */
	btsl	$_EFER_LME, %eax
	/* Enable System Call */
	btsl	$_EFER_SCE, %eax

	/* No Execute supported? */	
	btl	$20,%edi
	jnc     1f
	btsl	$_EFER_NX, %eax
1:	
				
	/* Make changes effective */
	wrmsr // 将intel手册写的 长模式开启的位  写入到 MSR 中

    // 此时 进入到了 64 bit 中兼容  32 bit 的模式
	xorl	%eax, %eax
	btsl	$31, %eax			/* Enable paging and in turn activate Long Mode */
	btsl	$0, %eax			/* Enable protected mode */
	btsl	$1, %eax			/* Enable MP */
	btsl	$4, %eax			/* Enable ET */
	btsl	$5, %eax			/* Enable NE */
	btsl	$16, %eax			/* Enable WP */
	btsl	$18, %eax			/* Enable AM */
	/* Make changes effective */
	movl	%eax, %cr0
	jmp	reach_compatibility_mode
reach_compatibility_mode:
	
	/*
	 * At this point we're in long mode but in 32bit compatibility mode
	 * with EFER.LME = 1, CS.L = 0, CS.D = 1 (and in turn
	 * EFER.LMA = 1). Now we want to jump in 64bit mode, to do that we load
	 * the new gdt/idt that has __KERNEL_CS with CS.L = 1.
	 */
	// 这里是一个临界区：虽然硬件已经完全进入了 64 bit 模式，但是我们的GDT和 IDT 还是用的是 32bit 的，所以在
	// 没有修改 GDT 和 IDT 为 64 bit 之前，仍然处于 32 bit 编程模式，但是，CPU 的硬件 已经 完全开启了 64 bit
	 
	testw %bp,%bp	/* secondary CPU? */  这里先不用管，这里是给 SMP 多处理器 BSP IPI 引导 AP的从 CPU 时用的  
	jnz   second	
	

	// 加载 64 bit 的 GDT，intel 中 只要进入长模式， 所有段寄存器中的选择子 拿到 GDT 的表项中的CS.L = 1那么 
	// 以后 直接执行 RIP 就等于 最终 线性地址，不分段，检测都不检测了 ==> 恭喜各位，后续代码 再也没有GDT 的事情了，
	// 段寄存器 你可以理解为 消失了, 跟 ARM 一样了. 特权级 CPL RPL DPL 仍然使用，不然无法区分

	/* Load new GDT with the 64bit segment using 32bit descriptor */

	// 第一个问题： 这里为什么用 减法 获取 相对地址（物理地址）
	movl	$(pGDT32 - __START_KERNEL_map), %eax
	lgdt	(%eax)

	movl    $(ljumpvector - __START_KERNEL_map), %eax
	/* Finally jump in 64bit mode */
	ljmp	*(%eax) 
    // 因为前面已经 建立过 低端虚拟内存映射表项，所以这里 的 ljmp 没问题，直接跳转到  reach_long64 的代码处执行

second:
	/* abuse syscall to get into 64bit mode. this way we don't need
	   a working low identity mapping just for the short 32bit roundtrip. 
	   XXX kludge. this should not be needed. */
	movl  $MSR_STAR,%ecx
	xorl  %eax,%eax
	movl  $(__USER32_CS<<16)|__KERNEL_CS,%edx
	wrmsr

	movl  $MSR_CSTAR,%ecx
	movl  $0xffffffff,%edx
	movl  $0x80100100,%eax	# reach_long64 absolute
	wrmsr
	syscall

	.code64 // 此时 正式进入 64bit
	.org 0x100	

reach_long64:  // 此时 head.S 的功能差不多完了，要进入到内核的 C 语言代码了
	// rip 此时 是什么？ .long	reach_long64-__START_KERNEL_map 虚拟内存依然 指向低端内存
	movq init_rsp(%rip),%rsp // 执行 C 语言，必须要有栈，所以临时 搭建一个栈内存

	/* zero EFLAGS after setting rsp */

	// 清空 EFLAGS 为后面进入内核时，保持寄存器为 初始状态
	pushq $0
	popfq

	/*
	 * We must switch to a new descriptor in kernel space for the GDT
	 * because soon the kernel won't have access anymore to the userspace
	 * addresses where we're currently running on. We have to do that here
	 * because in 32bit we couldn't load a 64bit linear address.
	 */
	// 之前在 32bit 兼容模式的时候，用的是 32bit 的GDT，现在完全开启了 64bit ，那么必然要把GDT 修改为 64bit 的GDT
	// 而且： 在Intel 中 只要开启了 长模式（64bit模式）那么 GDT 相当于不用，直接用 RIP 就可以了
	lgdt	cpu_gdt_descr

	/* 
	 * Setup up a dummy PDA. this is just for some early bootup code
	 * that does in_interrupt() 
	   swapgs  // 将用户态的 gs 指向 和  内核的 PDA 所在的地址进行交换  。 MSR_GS_BASE -> GS GS->MSR_GS_BASE
	 */ 
	movl	$MSR_GS_BASE,%ecx 
	movq	$empty_zero_page,%rax
	movq    %rax,%rdx
	shrq	$32,%rdx
	wrmsr	

	// 设置其他段寄存器，因为现在完全开启了 64bit ，且在Intel 中 
	// 只要开启了 长模式（64bit模式）那么 GDT 相当于不用（其他段寄存器也不用），直接用 RIP 就可以了，所以这里
	// 直接 设置到 内核 DS 的 GDT 表项索引就行了，实际表项索引 除了 DPL 为 0 以外，其他的都不用
	/* set up data segments. actually 0 would do too */
	movl $__KERNEL_DS,%eax
	movl %eax,%ds	
	movl %eax,%ss
	movl %eax,%es
			
	/* esi is pointer to real mode structure with interesting info.
	   pass it to C */

	// 将 16 bit 实模式 读取的数据指针 通过 edi 寄存器 传递给 x86_64_start_kernel 的 C 语言函数
	// （因为 C语言函数的前两个参数传递，使用 EDI，ESI），x86_64_start_kernel 只有一个参数，所以 EDI
	// 里面放了什么： 从 BIOS 中提取出来的数据，在 后续高版本中，名字叫：boot_params page
	movl	%esi, %edi
	
	/* Finally jump to run C code and to be on real kernel address
	 * Since we are running on identity-mapped space we have to jump
	 * to the full 64bit address , this is only possible as indirect
	 * jump
	 */
	movq	initial_code(%rip),%rax
	jmp	*%rax // 由于是 64 bit 模式，所以 不需要 cs，直接无条件跳转即可

	/* SMP bootup changes these two */	
	.globl	initial_code
initial_code:
	.quad	x86_64_start_kernel
	.globl init_rsp
init_rsp:
	.quad  init_thread_union+THREAD_SIZE-8


.code32
ENTRY(no_long_mode)
	/* This isn't an x86-64 CPU so hang */
1:
	jmp	1b

.org 0xf00
	.globl pGDT32
pGDT32:
	.word	gdt32_end-gdt_table32
	.long	gdt_table32-__START_KERNEL_map

.org 0xf10	
ljumpvector:
	.long	reach_long64-__START_KERNEL_map // 得到 reach_long64 物理地址
	.word	__KERNEL_CS  段选择子

ENTRY(stext)
ENTRY(_stext)

	/*
	 * This default setting generates an ident mapping at address 0x100000
	 * and a mapping for the kernel that precisely maps virtual address
	 * 0xffffffff80000000 to physical address 0x000000. (always using
	 * 2Mbyte large pages provided by PAE mode)
	 */
.org 0x1000
ENTRY(init_level4_pgt) // 总共有 512 项 页表项，使得 我们可以使用 48bit 虚拟内存中的高 9 位 作为索引找到页表项
	// 这里只有 48 bit  可用，不是 64 bit 
	.quad	0x0000000000102007		/* -> level3_ident_pgt */ 
	 // 第 0 项， 对应的虚拟地址二进制: 0000 00000 未知数 低端虚拟内存映射表项
	.fill	255,8,0
	.quad	0x000000000010a007 // 中间的虚拟内存段映射表项
	.fill	254,8,0
	/* (2^48-(2*1024*1024*1024))/(2^39) = 511 */ 一个 4KB 的页中能存放 512 个 表项
	.quad	0x0000000000103007		/* -> level3_kernel_pgt */
	// 最后 一 项， 对应的虚拟地址二进制: 1111 11111  未知数 高端虚拟内存映射表项

.org 0x2000
/* Kernel does not "know" about 4-th level of page tables. */

// 如果查到了这个页中目录的页表的第一项，那么虚拟地址必定为：0000 0000 0000 0000 00 => 0x 0000 位置数
// 所以如果开启了 分页，那么 访问 ： 0x 0000 A FFFF FFFF
ENTRY(level3_ident_pgt)  // 第二级页表  0000 00000  映射过来的，0000 00000 
	.quad	0x0000000000104007
	.fill	511,8,0

.org 0x3000
ENTRY(level3_kernel_pgt) // 第二级页表
	.fill	510,8,0
	/* (2^48-(2*1024*1024*1024)-((2^39)*511))/(2^30) = 510 */
	.quad	0x0000000000105007		/* -> level2_kernel_pgt */
	.fill	1,8,0

.org 0x4000
ENTRY(level2_ident_pgt)
	/* 40MB for bootup. 	*/
	.quad	0x0000000000000283
	.quad	0x0000000000200183
	.quad	0x0000000000400183
	.quad	0x0000000000600183
	.quad	0x0000000000800183
	.quad	0x0000000000A00183
	.quad	0x0000000000C00183
	.quad	0x0000000000E00183
	.quad	0x0000000001000183
	.quad	0x0000000001200183
	.quad	0x0000000001400183
	.quad	0x0000000001600183
	.quad	0x0000000001800183
	.quad	0x0000000001A00183
	.quad	0x0000000001C00183
	.quad	0x0000000001E00183
	.quad	0x0000000002000183
	.quad	0x0000000002200183
	.quad	0x0000000002400183
	.quad	0x0000000002600183
	/* Temporary mappings for the super early allocator in arch/x86_64/mm/init.c */
	.globl temp_boot_pmds
temp_boot_pmds:
	.fill	492,8,0
	
.org 0x5000
ENTRY(level2_kernel_pgt)
	/* 40MB kernel mapping. The kernel code cannot be bigger than that.
	   When you change this change KERNEL_TEXT_SIZE in page.h too. */
	/* (2^48-(2*1024*1024*1024)-((2^39)*511)-((2^30)*510)) = 0 */
	.quad	0x0000000000000183
	.quad	0x0000000000200183
	.quad	0x0000000000400183
	.quad	0x0000000000600183
	.quad	0x0000000000800183
	.quad	0x0000000000A00183
	.quad	0x0000000000C00183
	.quad	0x0000000000E00183
	.quad	0x0000000001000183
	.quad	0x0000000001200183
	.quad	0x0000000001400183
	.quad	0x0000000001600183
	.quad	0x0000000001800183
	.quad	0x0000000001A00183
	.quad	0x0000000001C00183
	.quad	0x0000000001E00183
	.quad	0x0000000002000183
	.quad	0x0000000002200183
	.quad	0x0000000002400183
	.quad	0x0000000002600183
	/* Module mapping starts here */
	.fill	492,8,0

.org 0x6000
ENTRY(empty_zero_page)

.org 0x7000
ENTRY(empty_bad_page)

.org 0x8000
ENTRY(empty_bad_pte_table)

.org 0x9000
ENTRY(empty_bad_pmd_table)

.org 0xa000
ENTRY(level3_physmem_pgt)
	.quad	0x0000000000105007		/* -> level2_kernel_pgt (so that __va works even before pagetable_init) */

	.org 0xb000
#ifdef CONFIG_ACPI_SLEEP
ENTRY(wakeup_level4_pgt)
	.quad	0x0000000000102007		/* -> level3_ident_pgt */
	.fill	255,8,0
	.quad	0x000000000010a007
	.fill	254,8,0
	/* (2^48-(2*1024*1024*1024))/(2^39) = 511 */
	.quad	0x0000000000103007		/* -> level3_kernel_pgt */
#endif

	.data

	.align 16
	.globl cpu_gdt_descr
cpu_gdt_descr:
	.word	gdt_end-cpu_gdt_table
gdt:
	.quad	cpu_gdt_table
#ifdef CONFIG_SMP
	.rept	NR_CPUS-1
	.word	0
	.quad	0
	.endr
#endif

.align 64 /* cacheline aligned */
ENTRY(gdt_table32)
	.quad	0x0000000000000000	/* This one is magic */
	.quad	0x0000000000000000	/* unused */
	.quad	0x00af9a000000ffff	/* __KERNEL_CS */
gdt32_end:	
	
/* We need valid kernel segments for data and code in long mode too
 * IRET will check the segment types  kkeil 2000/10/28
 * Also sysret mandates a special GDT layout 
 */
		 		
.align 64 /* cacheline aligned, keep this synchronized with asm/desc.h */

/* The TLS descriptors are currently at a different place compared to i386.
   Hopefully nobody expects them at a fixed place (Wine?) */
	
ENTRY(cpu_gdt_table)
	.quad	0x0000000000000000	/* NULL descriptor */
	.quad	0x00af9a000000ffff ^ (1<<21)	/* __KERNEL_COMPAT32_CS */	
	.quad	0x00af9a000000ffff	/* __KERNEL_CS */
	.quad	0x00cf92000000ffff	/* __KERNEL_DS */
	.quad	0x00cffe000000ffff	/* __USER32_CS */
	.quad	0x00cff2000000ffff	/* __USER_DS, __USER32_DS  */		
	.quad	0x00affa000000ffff	/* __USER_CS */
	.quad	0x00cf9a000000ffff	/* __KERNEL32_CS */
	.quad	0,0			/* TSS */
	.quad	0			/* LDT */
	.quad   0,0,0			/* three TLS descriptors */ 
	.quad	0			/* unused now */
	.quad	0x00009a000000ffff	/* __KERNEL16_CS - 16bit PM for S3 wakeup. */
					/* base must be patched for real base address. */
	/* This should be a multiple of the cache line size */
gdt_end:	
	.globl gdt_end

	/* GDTs of other CPUs */	
#ifdef CONFIG_SMP
	.rept NR_CPUS-1
	.quad 0,0,0,0,0,0,0,0,0,0,0
	.endr	
#endif

	.align  64
ENTRY(idt_table)	
	.rept   256
	.quad   0
	.quad 	0
	.endr

