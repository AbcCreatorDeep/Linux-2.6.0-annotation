/*
 *  linux/boot/head.S
 *
 *  Copyright (C) 1991, 1992, 1993  Linus Torvalds
 *
 *  $Id: head.S,v 1.3 2001/04/20 00:59:28 ak Exp $	 		
 */

/*
 *  head.S contains the 32-bit startup code.
 *
 * NOTE!!! Startup happens at absolute address 0x00001000, which is also where
 * the page directory will exist. The startup code will be overwritten by
 * the page directory. [According to comments etc elsewhere on a compressed
 * kernel it will end up at 0x1000 + 1Mb I hope so as I assume this. - AC]
 *
 * Page 0 is deliberately kept safe, since System Management Mode code in 
 * laptops may need to access the BIOS data stored there.  This is also
 * useful for future device drivers that either access the BIOS via VM86 
 * mode.
 */

/*
 * High loaded stuff by Hans Lermen & Werner Almesberger, Feb. 1996	
 */
.code32
.text

#include <linux/linkage.h>
#include <asm/segment.h>

	.code32 # setup.S 已经将CPU 切换到了 32 bit 的保护模式，所以一下代码都是 32 bit 的代码
	.globl startup_32
	
startup_32:
	cld
	cli # 再次确认 中断 是关闭的
	# 初始化 段寄存器的 选择子，因为这些段选择子 还是 16 bit 的，现在需要初始化 段选择子 指向内核数据段

	movl $(__KERNEL_DS),%eax
	movl %eax,%ds
	movl %eax,%es
	movl %eax,%fs
	movl %eax,%gs

	lss stack_start,%esp # 初始化栈

	xorl %eax,%eax 
1:	incl %eax		# check that A20 really IS enabled
	movl %eax,0x000000	# loop forever if it isn't
	cmpl %eax,0x100000
	je 1b

/*
 * Initialize eflags.  Some BIOS's leave bits like NT set.  This would
 * confuse the debugger if this code is traced.
 * XXX - best to initialize before switching to protected mode.
 */
	pushl $0
	popfl
/*
 * Clear BSS
 */
	xorl %eax,%eax
	movl $_edata,%edi
	movl $_end,%ecx
	subl %edi,%ecx
	cld
	rep
	stosb
/*
 * Do the decompression, and jump to the new kernel..
 */
	subl $16,%esp	# place for structure on the stack
	movl %esp,%eax

	pushl %esi	# real mode pointer as second arg
	pushl %eax	# address of structure as first arg

	call decompress_kernel    # 调用 C 语言的解压函数

	orl  %eax,%eax  # C语言函数的返回值 使用 eax 
	jnz  3f # 若返回值 不为0 ，那么向前跳转到标号为 3 处，这里就是大内核，所以 内核的一部分 就被 放到了低端内存处，所以要把它挪到 100000 处

    # 若返回值为0 ，那么解压的内核代码就已经放到了1M 处，那么跳转到  head.S 的代码处执行了（一定是 zimage 小内核）
	addl $8,%esp
	xorl %ebx,%ebx
	ljmp $(__KERNEL_CS), $0x100000 # 为什么这里不是 jmpi 0x100000, $(__KERNEL_CS)? 因为这里的代码都是 AT&T 的代码，所以 操作数 反过来了

/*
 * We come here, if we were loaded high.
 * We need to move the move-in-place routine down to 0x1000
 * and then start it with the buffer addresses in registers,
 * which we got from the stack.
 */
3:
	movl %esi,%ebx	
	movl $move_routine_start,%esi  # 这个 move_routine_start  就是 移动放置在 低内存处的内核一部分的移动代码
	movl $0x1000,%edi # 目的地址 1000 放到 edi 这个地方
	movl $move_routine_end,%ecx
	subl %esi,%ecx
	addl $3,%ecx
	shrl $2,%ecx
	cld
	rep
	movsl

	popl %esi	# discard the address
	addl $4,%esp	# real mode pointer
	popl %esi	# low_buffer_start
	popl %ecx	# lcount
	popl %edx	# high_buffer_start
	popl %eax	# hcount
	movl $0x100000,%edi
	cli		# make sure we don't get interrupted
	ljmp $(__KERNEL_CS), $0x1000 # and jump to the move routine

/*
 * Routine (template) for moving the decompressed kernel in place,
 * if we were high loaded. This _must_ PIC-code !
 */
move_routine_start:
	movl %ecx,%ebp
	shrl $2,%ecx
	rep
	movsl
	movl %ebp,%ecx
	andl $3,%ecx
	rep
	movsb
	movl %edx,%esi
	movl %eax,%ecx	# NOTE: rep movsb won't move if %ecx == 0
	addl $3,%ecx
	shrl $2,%ecx
	rep
	movsl
	movl %ebx,%esi	# Restore setup pointer
	xorl %ebx,%ebx
	ljmp $(__KERNEL_CS), $0x100000
move_routine_end:


/* Stack for uncompression */ 	
	.align 32
user_stack:	 	
	.fill 4096,4,0 // 开辟 4096 * 4 byte 的空间，然后初始化为 0

stack_start:	# 这里是 一个内存起始值 ，往后的 32 bit 放了 栈基址 ，栈基址的后面放了一个 16 位的 数据段选择子
	.long user_stack+4096 # 栈的分配必须由高地址 向低地址扩张
	.word __KERNEL_DS

