/*
 *  linux/arch/x86_64/kernel/head64.c -- prepare to run common code
 *
 *  Copyright (C) 2000 Andrea Arcangeli <andrea@suse.de> SuSE
 *
 *  $Id: head64.c,v 1.22 2001/07/06 14:28:20 ak Exp $
 */

#include <linux/init.h>
#include <linux/linkage.h>
#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/string.h>

#include <asm/processor.h>
#include <asm/proto.h>
#include <asm/smp.h>
#include <asm/bootsetup.h>

/* Don't add a printk in there. printk relies on the PDA which is not initialized 
   yet. */
static void __init clear_bss(void)
{
	extern char __bss_start[], __bss_end[];
	memset(__bss_start, 0,
	       (unsigned long) __bss_end - (unsigned long) __bss_start);
}

extern char x86_boot_params[2048];

#define NEW_CL_POINTER		0x228	/* Relative to real mode data */
#define OLD_CL_MAGIC_ADDR	0x90020
#define OLD_CL_MAGIC            0xA33F
#define OLD_CL_BASE_ADDR        0x90000
#define OLD_CL_OFFSET           0x90022

extern char saved_command_line[];

// 存放在 ESI 指向的低端 内存处，所以  再一次强调了前面的 低端 虚拟内存 线性映射到 0 -40M 的物理内存重要性
static void __init copy_bootdata(char *real_mode_data)
{
	int new_data;
	char * command_line;

	memcpy(x86_boot_params, real_mode_data, 2048); // 在该版本中 2KB 的数据已经够用
	new_data = *(int *) (x86_boot_params + NEW_CL_POINTER);
	if (!new_data) {
		if (OLD_CL_MAGIC != * (u16 *) OLD_CL_MAGIC_ADDR) {
			printk("so old bootloader that it does not support commandline?!\n");
			return;
		}
		new_data = OLD_CL_BASE_ADDR + * (u16 *) OLD_CL_OFFSET;
		printk("old bootloader convention, maybe loadlin?\n");
	}
	command_line = (char *) ((u64)(new_data));
	memcpy(saved_command_line, command_line, COMMAND_LINE_SIZE);
	printk("Bootdata ok (command line is %s)\n", saved_command_line);	
}

static void __init setup_boot_cpu_data(void)
{
	int dummy, eax;

	/* get vendor info */
	cpuid(0, &boot_cpu_data.cpuid_level,
	      (int *)&boot_cpu_data.x86_vendor_id[0],
	      (int *)&boot_cpu_data.x86_vendor_id[8],
	      (int *)&boot_cpu_data.x86_vendor_id[4]);

	/* get cpu type */
	cpuid(1, &eax, &dummy, &dummy, (int *) &boot_cpu_data.x86_capability);
	boot_cpu_data.x86 = (eax >> 8) & 0xf;
	boot_cpu_data.x86_model = (eax >> 4) & 0xf;
	boot_cpu_data.x86_mask = eax & 0xf;
}


// 以后的代码 全部属于：0xffffffff80100000 以上的虚拟内存地址了

// 当前：执行该代码的 CPU 是什么？ BSP（Bootstrap Processor） ，
// 因为 其他 AP 还没有初始化，后续需要 BSP 发送 IPI  信息 启动
void __init x86_64_start_kernel(char * real_mode_data)
{
	char *s;

	clear_bss();
	pda_init(0); // 刚才构建的是  dummy 假的 PDA ,没有数据，linux 内核的  CPU  序号 从 0 开始
	copy_bootdata(real_mode_data); // 将 BIOS 中读取到的数据 复制到 char x86_boot_params[2048] __initdata = {0,}; 中
	/* default console: */
	if (!strstr(saved_command_line, "console="))
		strcat(saved_command_line, " console=tty0"); 
	s = strstr(saved_command_line, "earlyprintk="); 
	if (s != NULL)
		setup_early_printk(s+12); 
#ifdef CONFIG_DISCONTIGMEM
	s = strstr(saved_command_line, "numa=");
	if (s != NULL)
		numa_setup(s+5);
#endif
#ifdef CONFIG_X86_IO_APIC
	if (strstr(saved_command_line, "disableapic"))
		disable_apic = 1;
#endif
	setup_boot_cpu_data();
	start_kernel();
}
