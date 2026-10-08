/* 
 * X86-64 specific CPU setup.
 * Copyright (C) 1995  Linus Torvalds
 * Copyright 2001, 2002, 2003 SuSE Labs / Andi Kleen.
 * See setup.c for older changelog.
 * $Id: setup64.c,v 1.12 2002/03/21 10:09:17 ak Exp $
 */ 
#include <linux/config.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/string.h>
#include <linux/bootmem.h>
#include <asm/pda.h>
#include <asm/pgtable.h>
#include <asm/processor.h>
#include <asm/desc.h>
#include <asm/bitops.h>
#include <asm/atomic.h>
#include <asm/mmu_context.h>
#include <asm/smp.h>
#include <asm/i387.h>
#include <asm/percpu.h>
#include <asm/mtrr.h>
#include <asm/proto.h>
#include <asm/mman.h>

char x86_boot_params[2048] __initdata = {0,};

unsigned long cpu_initialized __initdata = 0;

// 该变量放在 内核 ELF 中的 数据段中，bss 段
// 这里的 bss 段的记录大小： NR_CPUS *  sizeof(struct x8664_pda)
struct x8664_pda cpu_pda[NR_CPUS] __cacheline_aligned; 

extern struct task_struct init_task;

extern unsigned char __per_cpu_start[], __per_cpu_end[]; 

extern struct desc_ptr cpu_gdt_descr[];
struct desc_ptr idt_descr = { 256 * 16, (unsigned long) idt_table }; 

char boot_cpu_stack[IRQSTACKSIZE] __cacheline_aligned;

unsigned long __supported_pte_mask = ~0UL;
static int do_not_nx __initdata = 0;
unsigned long vm_stack_flags = __VM_STACK_FLAGS; 
unsigned long vm_stack_flags32 = __VM_STACK_FLAGS; 
unsigned long vm_data_default_flags = __VM_DATA_DEFAULT_FLAGS; 
unsigned long vm_data_default_flags32 = __VM_DATA_DEFAULT_FLAGS; 
unsigned long vm_force_exec32 = PROT_EXEC; 

/* noexec=on|off
Control non executable mappings for 64bit processes.

on	Enable
off	Disable
noforce (default) Don't enable by default for heap/stack/data, 
	but allow PROT_EXEC to be effective

*/ 
static int __init nonx_setup(char *str)
{
	if (!strncmp(str, "on",3)) { 
                __supported_pte_mask |= _PAGE_NX; 
 		do_not_nx = 0; 
 		vm_data_default_flags &= ~VM_EXEC; 
 		vm_stack_flags &= ~VM_EXEC;  
	} else if (!strncmp(str, "noforce",7) || !strncmp(str,"off",3)) { 
		do_not_nx = (str[0] == 'o');
		if (do_not_nx)
			__supported_pte_mask &= ~_PAGE_NX; 
		vm_data_default_flags |= VM_EXEC; 
		vm_stack_flags |= VM_EXEC;
        } 
        return 1;
} 

__setup("noexec=", nonx_setup); 

/* noexec32=opt{,opt} 

Control the no exec default for 32bit processes. Can be also overwritten
per executable using ELF header flags (e.g. needed for the X server)
Requires noexec=on or noexec=noforce to be effective.

Valid options: 
   all,on    Heap,stack,data is non executable. 	
   off       (default) Heap,stack,data is executable
   stack     Stack is non executable, heap/data is.
   force     Don't imply PROT_EXEC for PROT_READ 
   compat    (default) Imply PROT_EXEC for PROT_READ

*/
 static int __init nonx32_setup(char *str)
 {
	char *s;
	while ((s = strsep(&str, ",")) != NULL) { 
		if (!strcmp(s, "all") || !strcmp(s,"on")) {
			vm_data_default_flags32 &= ~VM_EXEC; 
			vm_stack_flags32 &= ~VM_EXEC;  
		} else if (!strcmp(s, "off")) { 
			vm_data_default_flags32 |= VM_EXEC; 
			vm_stack_flags32 |= VM_EXEC;  
		} else if (!strcmp(s, "stack")) { 
			vm_data_default_flags32 |= VM_EXEC; 
			vm_stack_flags32 &= ~VM_EXEC;  		
		} else if (!strcmp(s, "force")) { 
			vm_force_exec32 = 0; 
		} else if (!strcmp(s, "compat")) { 
			vm_force_exec32 = PROT_EXEC;
		} 
	} 
 	return 1;
} 

__setup("noexec32=", nonx32_setup); 

/*
 * Great future plan:
 * Declare PDA itself and support (irqstack,tss,pml4) as per cpu data.
 * Always point %gs to its beginning
 */
void __init setup_per_cpu_areas(void)
{ 
	int i;
	unsigned long size;

	/* Copy section for each CPU (we discard the original) */
	size = ALIGN(__per_cpu_end - __per_cpu_start, SMP_CACHE_BYTES);
#ifdef CONFIG_MODULES
	if (size < PERCPU_ENOUGH_ROOM)
		size = PERCPU_ENOUGH_ROOM;
#endif

	for (i = 0; i < NR_CPUS; i++) { 
		unsigned char *ptr;
		/* If possible allocate on the node of the CPU.
		   In case it doesn't exist round-robin nodes. */
		if (!NODE_DATA(i % numnodes)) { 
			printk("cpu with no node %d, numnodes %d\n", i, numnodes);
			ptr = alloc_bootmem(size);
		} else { 
			ptr = alloc_bootmem_node(NODE_DATA(i % numnodes), size);
		}
		if (!ptr)
			panic("Cannot allocate cpu data for CPU %d\n", i);
		cpu_pda[i].data_offset = ptr - __per_cpu_start;
		memcpy(ptr, __per_cpu_start, __per_cpu_end - __per_cpu_start);
	}
} 

// 初始化每个 CPU 自己的 pda 区域 ，初始化后 地址固定不变，由 schedule 函数 动态填入 当前切换的进程的数据即可
void pda_init(int cpu)
{ 
    pml4_t *level4;
	struct x8664_pda *pda = &cpu_pda[cpu];  // 内核中的 TLS 数组

	/* Setup up data that may be needed in __get_free_pages early */
	asm volatile("movl %0,%%fs ; movl %0,%%gs" :: "r" (0)); 
	wrmsrl(MSR_GS_BASE, cpu_pda + cpu);

	pda->me = pda;
	pda->cpunumber = cpu; 
	pda->irqcount = -1;
	pda->kernelstack = 
		(unsigned long)stack_thread_info() - PDA_STACKOFFSET + THREAD_SIZE; 
	// 前置知识： 所有进程的 高位 页表 全部相同（内核只有一个）
	// 加入当前内核上 正在运行一个 R3 / R0 态的 task，然后发生了中断
	// 切换中断栈，然后保存 R3 / R0 态的 task 的数据，放在内核栈中，然后执行 内核的 中断处理函数
	pda->active_mm = &init_mm;  
	pda->mmu_state = 0;

	if (cpu == 0) { // 当前CPU 是不是 BSP CPU ，因为 Linux 内核中 CPU 的编号 是从 0 开始的
		/* others are initialized in smpboot.c */
		pda->pcurrent = &init_task;
		pda->irqstackptr = boot_cpu_stack; 
		level4 = init_level4_pgt;  // 获取 head.S 中的临时页表
	} else { // BSP 负责初始化唤醒 AP，AP 也会执行这段代码
		level4 = (pml4_t *)__get_free_pages(GFP_ATOMIC, 0);  // 分配一个页的数据
		if (!level4) 
			panic("Cannot allocate top level page for cpu %d", cpu); 
		pda->irqstackptr = (char *)
			__get_free_pages(GFP_ATOMIC, IRQSTACK_ORDER);
		if (!pda->irqstackptr)
			panic("cannot allocate irqstack for cpu %d", cpu); 
	}

	// 继续临时使用，后续再变。 分配页表空间需要 内存分配器，而现在伙伴分配机制
	pda->level4_pgt = (unsigned long *)level4; 
	if (level4 != init_level4_pgt)
		memcpy(level4, &init_level4_pgt, PAGE_SIZE); 


	// 在内核页表项中 填入能够访问  boot_vmalloc_pgt level3 的地址，使得 内核后续可以直接访问 level3 所指向的内存
	// 这里的做法 就等价于 do_page_fault 中的做法。 因为 当前OS 还没有完全初始化，所以没有处理 缺页中断的 IDT 
	// 所以这里得手动填写指向需要访问的物理内存的 L3 的页表项
	set_pml4(level4 + 510, mk_kernel_pml4(__pa_symbol(boot_vmalloc_pgt)));
	asm volatile("movq %0,%%cr3" :: "r" (__pa(level4))); // 刷新 TLB

	pda->irqstackptr += IRQSTACKSIZE-64;
} 

char boot_exception_stacks[N_EXCEPTION_STACKS * EXCEPTION_STKSZ];

void syscall_init(void)
{
	/* 
	 * LSTAR and STAR live in a bit strange symbiosis.
	 * They both write to the same internal register. STAR allows to set CS/DS
	 * but only a 32bit target. LSTAR sets the 64bit rip. 	 
	 */ 
	wrmsrl(MSR_STAR,  ((u64)__USER32_CS)<<48  | ((u64)__KERNEL_CS)<<32); 
	wrmsrl(MSR_LSTAR, system_call); 

#ifdef CONFIG_IA32_EMULATION   		
	wrmsrl(MSR_CSTAR, ia32_cstar_target); 
#endif

	/* Flags to clear on syscall */
	wrmsrl(MSR_SYSCALL_MASK, EF_TF|EF_DF|EF_IE|0x3000); 
}

/*
 * cpu_init() initializes state that is per-CPU. Some data is already
 * initialized (naturally) in the bootstrap process, such as the GDT
 * and IDT. We reload them nevertheless, this function acts as a
 * 'CPU state barrier', nothing should get across.
 * A lot of state is already set up in PDA init.
 */
void __init cpu_init (void)
{
#ifdef CONFIG_SMP
	int cpu = stack_smp_processor_id();
#else
	int cpu = smp_processor_id();
#endif
	struct tss_struct * t = &init_tss[cpu];
	unsigned long v, efer; 
	char *estacks = NULL; 
	struct task_struct *me;

	/* CPU 0 is initialised in head64.c */
	if (cpu != 0) {
		(cpu);pda_init
	} else 
		estacks = boot_exception_stacks; 

	me = current;

	if (test_and_set_bit(cpu, &cpu_initialized))
		panic("CPU#%d already initialized!\n", cpu);

	printk("Initializing CPU#%d\n", cpu);

		clear_in_cr4(X86_CR4_VME|X86_CR4_PVI|X86_CR4_TSD|X86_CR4_DE);

	/*
	 * Initialize the per-CPU GDT with the boot GDT,
	 * and set up the GDT descriptor:
	 */
	if (cpu) {
		memcpy(cpu_gdt_table[cpu], cpu_gdt_table[0], GDT_SIZE);
	}	

	cpu_gdt_descr[cpu].size = GDT_SIZE;
	cpu_gdt_descr[cpu].address = (unsigned long)cpu_gdt_table[cpu];
	__asm__ __volatile__("lgdt %0": "=m" (cpu_gdt_descr[cpu]));
	__asm__ __volatile__("lidt %0": "=m" (idt_descr));

	memcpy(me->thread.tls_array, cpu_gdt_table[cpu], GDT_ENTRY_TLS_ENTRIES * 8);

	/*
	 * Delete NT
	 */

	asm volatile("pushfq ; popq %%rax ; btr $14,%%rax ; pushq %%rax ; popfq" ::: "eax");

	syscall_init();

	wrmsrl(MSR_FS_BASE, 0);
	wrmsrl(MSR_KERNEL_GS_BASE, 0);
	barrier(); 

	rdmsrl(MSR_EFER, efer); 
        if (!(efer & EFER_NX) || do_not_nx) { 
                __supported_pte_mask &= ~_PAGE_NX; 
        }       

	/*
	 * set up and load the per-CPU TSS
	 */
	for (v = 0; v < N_EXCEPTION_STACKS; v++) {
		if (cpu) {
			estacks = (char *)__get_free_pages(GFP_ATOMIC, 0);
			if (!estacks)
				panic("Cannot allocate exception stack %ld %d\n",
				      v, cpu); 
		}
		estacks += EXCEPTION_STKSZ;
		t->ist[v] = (unsigned long)estacks;
	}

	t->io_bitmap_base = INVALID_IO_BITMAP_OFFSET;
	/*
	 * This is required because the CPU will access up to
	 * 8 bits beyond the end of the IO permission bitmap.
	 */
	t->io_bitmap[IO_BITMAP_LONGS] = ~0UL;

	atomic_inc(&init_mm.mm_count);
	me->active_mm = &init_mm;
	if (me->mm)
		BUG();
	enter_lazy_tlb(&init_mm, me);

	set_tss_desc(cpu, t);
	load_TR_desc();
	load_LDT(&init_mm.context);

	/*
	 * Clear all 6 debug registers:
	 */

	set_debug(0UL, 0);
	set_debug(0UL, 1);
	set_debug(0UL, 2);
	set_debug(0UL, 3);
	set_debug(0UL, 6);
	set_debug(0UL, 7);

	fpu_init(); 
}
