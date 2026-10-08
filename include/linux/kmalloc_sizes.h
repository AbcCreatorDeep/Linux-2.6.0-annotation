#if (PAGE_SIZE == 4096)
	CACHE(32)
#endif

	CACHE(64)


// 猜测： 考虑 CPU 高速缓存行的 利用率
// 假设 缓存行大小为 32 byte，那么 允许 定义 96 byte 的缓存碎片，此时 一个 96 byte 的页数据，需要放到 3 个缓存行中
// 假设 缓存行大小为 64 byte，那么 不允许 定义 96 byte => 96 - 64 = 32byte  => 浪费 32 byte 的缓存行空间
#if L1_CACHE_BYTES < 64
	CACHE(96)
#endif


	CACHE(128)

#if L1_CACHE_BYTES < 128
	CACHE(192)
#endif

	CACHE(256)
	CACHE(512)
	CACHE(1024)
	CACHE(2048)
	CACHE(4096)
	CACHE(8192)
	CACHE(16384)
	CACHE(32768)
	CACHE(65536)
	CACHE(131072)

// 猜测： 考虑的是 TLB 的占用时长
// 若 在 MMU 存在 的 CPU 上 不指定 如下内存的碎片，
// 那么等价于 获取 一个 4KB 的物理页直接分配，且不存在内存缓存功能，用完 释放后直接放回 伙伴算法

// MMU 对于 地址转换而言，只有 TLB 的 entry（虚拟地址 -> 物理地址）

#ifndef CONFIG_MMU
	CACHE(262144)  // 256KB
	CACHE(524288)  // 512KB
	CACHE(1048576) // 1024KB

// 不常用（风险：缓存 这些大页，会存在内存浪费 问题），使用 kmalloc 分配的时候 ，通常都是 小内存结构
#ifdef CONFIG_LARGE_ALLOCS 
	CACHE(2097152)
	CACHE(4194304)
	CACHE(8388608)
	CACHE(16777216) 
	CACHE(33554432) // 32MB
#endif /* CONFIG_LARGE_ALLOCS */

#endif /* CONFIG_MMU */
