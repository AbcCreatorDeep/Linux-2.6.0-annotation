/*
 * Read-Copy Update mechanism for mutual exclusion
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 *
 * Copyright (C) IBM Corporation, 2001
 *
 * Author: Dipankar Sarma <dipankar@in.ibm.com>
 * 
 * Based on the original work by Paul McKenney <paul.mckenney@us.ibm.com>
 * and inputs from Rusty Russell, Andrea Arcangeli and Andi Kleen.
 * Papers:
 * http://www.rdrop.com/users/paulmck/paper/rclockpdcsproof.pdf
 * http://lse.sourceforge.net/locking/rclock_OLS.2001.05.01c.sc.pdf (OLS2001)
 *
 * For detailed explanation of Read-Copy Update mechanism see -
 * 		http://lse.sourceforge.net/locking/rcupdate.html
 *
 */
#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/spinlock.h>
#include <linux/smp.h>
#include <linux/interrupt.h>
#include <linux/sched.h>
#include <asm/atomic.h>
#include <asm/bitops.h>
#include <linux/module.h>
#include <linux/completion.h>
#include <linux/percpu.h>
#include <linux/notifier.h>
#include <linux/rcupdate.h>
#include <linux/cpu.h>

/* Definition for rcupdate control block. */
struct rcu_ctrlblk rcu_ctrlblk = 
	{ .mutex = SPIN_LOCK_UNLOCKED, .curbatch = 1, 
	  .maxbatch = 1, .rcu_cpu_mask = CPU_MASK_NONE };
DEFINE_PER_CPU(struct rcu_data, rcu_data) = { 0L };

/* Fake initialization required by compiler */
static DEFINE_PER_CPU(struct tasklet_struct, rcu_tasklet) = {NULL};
#define RCU_tasklet(cpu) (per_cpu(rcu_tasklet, cpu))

 // struct tasklet_struct per_cpu__rcu_tasklet;放置在 内核的 数据节 中
/**
 * call_rcu - Queue an RCU update request.
 * @head: structure to be used for queueing the RCU updates.
 * @func: actual update function to be invoked after the grace period
 * @arg: argument to be passed to the update function
 *
 * The update function will be invoked as soon as all CPUs have performed 
 * a context switch or been seen in the idle loop or in a user process. 
 * The read-side of critical section that use call_rcu() for updation must 
 * be protected by rcu_read_lock()/rcu_read_unlock().
 */
void call_rcu(struct rcu_head *head, void (*func)(void *arg), void *arg)
{
	int cpu;
	unsigned long flags;
	// 将回调函数 和 入参 绑定到 head 中
	head->func = func;
	head->arg = arg;
	// 关闭硬中断的场景:要求执行的时间 比 自旋锁 还要短
	local_irq_save(flags); // 关闭硬中断

	cpu = smp_processor_id();
	// 而又由于这里是 per cpu 结构，TLS 技术保证每个 CPU 操作自己的 RCU 链表,所以这里不需要自旋锁
	list_add_tail(&head->list, &RCU_nxtlist(cpu)); 

	local_irq_restore(flags); // 打开硬中断
}

/*
 * Invoke the completed RCU callbacks. They are expected to be in
 * a per-cpu list.
 */
static void rcu_do_batch(struct list_head *list)
{
	struct list_head *entry;
	struct rcu_head *head;

	while (!list_empty(list)) { // 循环遍历 RCU 链表 执行回调函数
		entry = list->next;
		list_del(entry);
		head = list_entry(entry, struct rcu_head, list);
		head->func(head->arg);
	}
}

/*
 * Register a new batch of callbacks, and start it up if there is currently no
 * active batch and the batch to be registered has not already occurred.
 * Caller must hold the rcu_ctrlblk lock.
 */
static void rcu_start_batch(long newbatch)
{
	// 这里就是：rcu_ctrlblk.maxbatch < newbatch
	if (rcu_batch_before(rcu_ctrlblk.maxbatch, newbatch)) {
		rcu_ctrlblk.maxbatch = newbatch; // rcu_ctrlblk.maxbatch = 2
	}

	// rcu_ctrlblk.maxbatch: 2， rcu_ctrlblk.curbatch: 2
	if (rcu_batch_before(rcu_ctrlblk.maxbatch, rcu_ctrlblk.curbatch) ||
	    !cpus_empty(rcu_ctrlblk.rcu_cpu_mask)) { // 初始时：CPU_MASK_NONE 所以为 空，所以这个条件不成立
		return; // 最后一次进来后这里设置为 0000
	}

	// 启动新一轮的宽限期检测
	rcu_ctrlblk.rcu_cpu_mask = cpu_online_map; 
}

/*
 * Check if the cpu has gone through a quiescent state (say context
 * switch). If so and if it already hasn't done so in this RCU
 * quiescent cycle, then indicate that it has done so.
 */
static void rcu_check_quiescent_state(void)
{
	int cpu = smp_processor_id();

	if (!cpu_isset(cpu, rcu_ctrlblk.rcu_cpu_mask)) // 当前 CPU 编号下的 位图项有没有被置 1
		return;

	/* 
	 * Races with local timer interrupt - in the worst case
	 * we may miss one quiescent state of that CPU. That is
	 * tolerable. So no need to disable interrupts.
	 */
	if (RCU_last_qsctr(cpu) == RCU_QSCTR_INVALID) { // 参与者 CPU 只会在进来第一次 初始化 该代码
		RCU_last_qsctr(cpu) = RCU_qsctr(cpu);
		return;
	}
	// 所有 CPU 都会 第一次 last_qsctr ：1 qsctr ：1
	// 所有 CPU 都会 第二次 last_qsctr ：1 qsctr ：2
	// 所有 CPU 都会 第二次 执行完毕 last_qsctr ：0  qsctr ：2
	// 参与者 CPU 第二次进来（被时钟中断满足 RCU_PENDING 也即是参与者的身份）
	// if (user ||  // 如果当前 工作在用户态，那么说明正在执行用户代码（RCU 的 read lock 只会出现在内核代码中），那么就增加 qsctr 计数器
	// (idle_cpu(cpu) && // 当前CPU是 空闲的，啥事都没干
	// !in_softirq() &&  // 是否处于软中断上下文
	// 		hardirq_count() <= (1 << HARDIRQ_SHIFT))) // 当前 CPU 没有在处理 硬中断
	// RCU_qsctr(cpu)++; // 递增一次度过宽限期计数器
	
	// 只有当前 参与者 身份的 CPU  经历过一次 宽限期后，才能往下继续执行，解除自己参与者的身份
	if (RCU_qsctr(cpu) == RCU_last_qsctr(cpu))
		return;
	// 只有当前 参与者 身份的 CPU  已经经过一次上下文切换，也就是 本身自己度过了宽限期了
	spin_lock(&rcu_ctrlblk.mutex);
	if (!cpu_isset(cpu, rcu_ctrlblk.rcu_cpu_mask))
		goto out_unlock;

    // 清理位图：1 -> 0，表示当前 CPU 已经度过了宽限期，不再是参与者了，也即告诉 全局 说：我已经度过了宽限期
	// 大白话：每个CPU 通过自己的 last_qsctr 和 qsctr 的计数器来判定自己是不是经过了宽限期，若是那么把全局的 
	// rcu_cpu_mask 对应的 CPU 位 置 0 ，通知全局，我已经度过宽限期了
	cpu_clear(cpu, rcu_ctrlblk.rcu_cpu_mask); // 当前 其他参与者CPU 都把自己设置为0后，位图变为：1000，
	// 发起 call_rcu 的CPU 再次进来将自己也是 0 后，位图变为：0000
	RCU_last_qsctr(cpu) = RCU_QSCTR_INVALID; //  初始化为 无效值，等待下一轮  宽限期检测
	if (!cpus_empty(rcu_ctrlblk.rcu_cpu_mask)) // 问：还没有 CPU 没有经过宽限期？
		goto out_unlock; // 若还有其他CPU 没有经过宽限期，那么 直接释放自旋锁 该干嘛干嘛去，下一次时钟中断时，就不再进来了！
		// 为什么？因为 rcu_pending 的条件不成立了，三个条件都不成立

		// 当前的假设是：4 个 CPU ，其中一个 call_rcu ，其他的三个都是参与者
		// 其他的三个 CPU  都会进入该函数两次：第一次 初始化 RCU_last_qsctr(cpu) = RCU_qsctr(cpu); 
		// 第二次：cpu_clear(cpu, rcu_ctrlblk.rcu_cpu_mask); 

		// 那么：那个发起 call_rcu 的CPU 会干啥？此时位图：1000

	// 该代码就是最后一个 经过宽限期 的 CPU  执行的代码，位图中最后一个 CPU 来执行该代码（此时为：0000）
	rcu_ctrlblk.curbatch++;
// 	struct rcu_ctrlblk rcu_ctrlblk =
// { .mutex = SPIN_LOCK_UNLOCKED, .curbatch = 2,
//   .maxbatch = 2, .rcu_cpu_mask = 0000 （假定当前为 4核心 ）};
	rcu_start_batch(rcu_ctrlblk.maxbatch);

out_unlock:
	spin_unlock(&rcu_ctrlblk.mutex);
}


/*
 * This does the RCU processing work from tasklet context. 
 */
static void rcu_process_callbacks(unsigned long unused)
{
	//  记得 视角转换一下：当前调用进来的 CPU  没有调用 call_rcu，也即他只是一个参与者
	int cpu = smp_processor_id(); // 获取当前 CPU 的 ID
	LIST_HEAD(list); // 初始化局部链表
	// 你的大脑目前要记住：call_rcu 函数只是添加 callback 放到 nxt 链表, 其他的属性没有数据
	if (!list_empty(&RCU_curlist(cpu)) && // 视角：发起者CPU  进来，该条件成立
	    rcu_batch_after(rcu_ctrlblk.curbatch, RCU_batch(cpu))) { // 该条件不成立
		list_splice(&RCU_curlist(cpu), &list);
		INIT_LIST_HEAD(&RCU_curlist(cpu));
	}

	local_irq_disable();
	if (!list_empty(&RCU_nxtlist(cpu)) && list_empty(&RCU_curlist(cpu))) {
		// 写线程 将 该条件成立
		// 将待处理的callback 从 nxt 链表 移动到 curlist 链表，启动新一轮的宽限期检测
		list_splice(&RCU_nxtlist(cpu), &RCU_curlist(cpu));
		INIT_LIST_HEAD(&RCU_nxtlist(cpu));
		local_irq_enable();

		/*
		 * start the next batch of callbacks
		 */
		spin_lock(&rcu_ctrlblk.mutex); // 自旋锁：保护短时操作的 全局（所有 CPU 共享）结构

		RCU_batch(cpu) = rcu_ctrlblk.curbatch + 1; // 递增计数器：此时假定为 2，因为 初始时 为 1
		
		rcu_start_batch(RCU_batch(cpu));

		spin_unlock(&rcu_ctrlblk.mutex);
	} else {
		local_irq_enable();
	}
	rcu_check_quiescent_state(); //   参与者 CPU 只会执行该代码

	if (!list_empty(&list)) // 回调  callback 函数
		rcu_do_batch(&list);
}

void rcu_check_callbacks(int cpu, int user)
{
	if (user ||  // 如果当前 工作在用户态，那么说明正在执行用户代码（RCU 的 read lock 只会出现在内核代码中），那么就增加 qsctr 计数器
	    (idle_cpu(cpu) && // 当前CPU是 空闲的，啥事都没干
		!in_softirq() &&  // 是否处于软中断上下文
				hardirq_count() <= (1 << HARDIRQ_SHIFT))) // 当前 CPU 没有在处理 硬中断
		RCU_qsctr(cpu)++; // 递增一次度过宽限期计数器
	// 暂且把它当成软中断处理，也就是调用 do_softirq() 
	tasklet_schedule(&RCU_tasklet(cpu)); // 只要 在调用该方法前 发现 rcu_pending 成立，那么一定会调用该函数
}

static void __devinit rcu_online_cpu(int cpu)
{
	memset(&per_cpu(rcu_data, cpu), 0, sizeof(struct rcu_data));
	tasklet_init(&RCU_tasklet(cpu), rcu_process_callbacks, 0UL);
	INIT_LIST_HEAD(&RCU_nxtlist(cpu));
	INIT_LIST_HEAD(&RCU_curlist(cpu));
}

static int __devinit rcu_cpu_notify(struct notifier_block *self, 
				unsigned long action, void *hcpu)
{
	long cpu = (long)hcpu;
	switch (action) {
	case CPU_UP_PREPARE:
		rcu_online_cpu(cpu);
		break;
	/* Space reserved for CPU_OFFLINE :) */
	default:
		break;
	}
	return NOTIFY_OK;
}

static struct notifier_block __devinitdata rcu_nb = {
	.notifier_call	= rcu_cpu_notify,
};

/*
 * Initializes rcu mechanism.  Assumed to be called early.
 * That is before local timer(SMP) or jiffie timer (uniproc) is setup.
 * Note that rcu_qsctr and friends are implicitly
 * initialized due to the choice of ``0'' for RCU_CTR_INVALID.
 */
void __init rcu_init(void)
{
	rcu_cpu_notify(&rcu_nb, CPU_UP_PREPARE,
			(void *)(long)smp_processor_id());
	/* Register notifier for non-boot CPUs */
	register_cpu_notifier(&rcu_nb);
}


/* Because of FASTCALL declaration of complete, we use this wrapper */
static void wakeme_after_rcu(void *completion)
{
	complete(completion);
}

/**
 * synchronize-kernel - wait until all the CPUs have gone
 * through a "quiescent" state. It may sleep.
 */
void synchronize_kernel(void)
{
	struct rcu_head rcu;
	DECLARE_COMPLETION(completion);

	/* Will wake me after RCU finished */
	call_rcu(&rcu, wakeme_after_rcu, &completion);

	/* Wait for it */
	wait_for_completion(&completion);
}


EXPORT_SYMBOL(call_rcu);
EXPORT_SYMBOL(synchronize_kernel);
