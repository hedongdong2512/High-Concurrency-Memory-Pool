# High-Concurrency-Memory-Pool

基于 C++11 实现的线程安全高并发内存池，参考 tcmalloc 的三层缓存设计思想，针对多线程环境下 `malloc/new` 的锁竞争与内存碎片问题进行优化。

## 架构

```
ThreadCache  ──线程本地缓存，分配/释放全程无锁
     │  批量申请 / 按比例归还
CentralCache ──中心缓存，按大小类分桶自旋锁，跨线程内存复用
     │  以 span（页）为单位申请
PageCache    ──页缓存，向系统申请内存，负责 span 的分割与合并
```

- **ThreadCache**：每线程独立实例（`thread_local` 单例），按 8 字节对齐划分大小类，命中时直接从自由链表取块，绝大多数分配不经过任何锁。
- **CentralCache**：全局单例，自由链表数组 + 按大小类分桶的原子自旋锁（`std::atomic_flag`），不同大小类互不阻塞；用 `SpanTracker` 记录每个 span 的块数与空闲数，当 span 内所有块空闲时整体归还 PageCache。
- **PageCache**：按页（4KB）管理，`std::map` 按页数维护空闲 span 链表，支持大 span 分割与相邻 span 合并；`MAX_BYTES` 以上（>256KB）的大对象直接走系统分配。

## 主要特性

- 8 字节对齐的大小类划分（Size Class），自由链表管理小块内存
- 线程本地缓存无锁分配，中心缓存分桶自旋锁降低锁竞争
- 动态批量分配：ThreadCache 按各大小类的使用情况自适应调整向 CentralCache 的申请量（耗尽触发则翻倍增长、闲置回收则减半收缩），繁忙路径减少跨层请求频率，闲置路径减少内存滞留
- 缓存行填充：`SpanTracker` 与每个大小类的锁/延迟归还状态均按 `alignas(64)` 独占缓存行，消除多线程更新相邻桶计数时的伪共享
- 预取优化：`allocate` 命中本地自由链表时预取下一空闲块（`_mm_prefetch`/`__builtin_prefetch`），降低连续分配的访存延迟
- 批量归还 + 按比例回收（ThreadCache 超过阈值时保留 1/4，其余归还 CentralCache）
- 延迟归还机制：CentralCache 按计数/时间窗口批量检查 span 空闲情况
- 原子操作配合 `memory_order_relaxed / acquire / release` 控制同步开销

## 构建

当前 `PageCache::systemAlloc` 使用 Win32 API（`VirtualAlloc`），Windows / MSVC 下可直接编译；Linux 下需启用 `PageCache.cpp` 中注释的 `mmap` 分支。

```cpp
// 使用方式
#include "MemoryPool.h"
void* p = gzq_memoryPool::MemoryPool::allocate(64);
gzq_memoryPool::MemoryPool::deallocate(p, 64);
```

## 性能测试

`Test/源.cpp` 内置性能测试，对比内存池与 `new/delete` 在以下场景的耗时：

- 单线程小对象分配（5 万次）
- 4 线程并发分配 + 批量释放（每线程 2.5 万次）
- 混合大小分配（10 万次，小/中/大对象按 60/30/10 比例）

参考数据（i5 笔记本 / Windows / MSVC 19.42 /O2，仅供量级参考）：

| 场景 | 内存池 | new/delete |
|---|---|---|
| 单线程小对象（5 万次） | ~4.1 ms | ~7.5 ms |
| 混合大小（10 万次） | ~4.2 ms | ~8.7 ms |
| 4 线程并发（4×2.5 万次） | ~8.6 ms | ~4.2 ms |

> 多线程场景在本机暂慢于 `new/delete`：现代 MSVC 堆本身做了 per-thread 缓存，而本实现的 CentralCache 在少数几个大小类上竞争同一把自旋锁、`getSpanTracker` 为 O(n) 线性查找。已知改进方向：按 CPU/NUMA 分片 CentralCache、用页号哈希把 span 查找降到 O(1)——这是项目的下一步计划。
