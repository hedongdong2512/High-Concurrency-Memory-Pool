#include "ThreadCache.h"
#include "CentralCache.h"
#include <algorithm>

#if defined(_WIN32)
#include <intrin.h>
// 预取：把目标地址所在的缓存行提前加载进 L1 缓存，缩短下次访问的访存延迟
#define MP_PREFETCH(addr) _mm_prefetch(reinterpret_cast<const char*>(addr), _MM_HINT_T0)
#else
#define MP_PREFETCH(addr) __builtin_prefetch(addr)
#endif

namespace gzq_memoryPool
{

    void* ThreadCache::allocate(size_t size)//内存的分配实现主要判断freeListSize_[index]来分配内存
    {
        // 处理0大小的分配请求
        if (size == 0)
        {
            size = ALIGNMENT; // 至少分配一个对齐大小
        }

        if (size > MAX_BYTES)
        {
            // 大对象直接从系统分配
            return malloc(size);
        }

        size_t index = SizeClass::getIndex(size);//获取一个与size对应的索引值

        // 检查线程本地自由链表
        // 如果 freeList_[index] 不为空，表示该链表中有可用内存块
        if (void* ptr = freeList_[index])
        {
            // 将链表头后移一位
            freeList_[index] = *reinterpret_cast<void**>(ptr);
            // 更新对应自由链表的长度计数
            freeListSize_[index]--;

            // 预取下一个空闲块：同一线程大概率很快再次分配同类内存，
            // 提前把下一块所在缓存行加载进 L1，降低下次分配的访存延迟
            if (freeList_[index] != nullptr)
            {
                MP_PREFETCH(freeList_[index]);
            }

            return ptr;
        }

        // 如果线程本地自由链表为空，则按动态批量从中心缓存获取一批内存
        return fetchFromCentralCache(index);
    }

    void ThreadCache::deallocate(void* ptr, size_t size)//内存释放
    {
        if (size > MAX_BYTES)
        {
            free(ptr);
            return;
        }

        size_t index = SizeClass::getIndex(size);

        // 插入到线程本地自由链表
        *reinterpret_cast<void**>(ptr) = freeList_[index];
        freeList_[index] = ptr;

        // 更新对应自由链表的长度计数
        freeListSize_[index]++;

        // 判断是否需要将部分内存回收给中心缓存
        if (shouldReturnToCentralCache(index))
        {
            returnToCentralCache(freeList_[index], size);
        }
    }

    // 判断是否需要将内存回收给中心缓存
    bool ThreadCache::shouldReturnToCentralCache(size_t index)
    {
        // 设定阈值，例如：当自由链表的大小超过一定数量时
        size_t threshold = 256;
        return (freeListSize_[index] > threshold);
    }

    void* ThreadCache::fetchFromCentralCache(size_t index)
    {
        // 动态批量：按该大小类的历史使用情况决定本次申请的块数，
        // 而不是固定值——繁忙的大小类申请更多，闲置的大小类申请更少
        size_t want = batchHint_[index];

        // 从中心缓存批量获取内存（返回块链表头）
        void* start = CentralCache::getInstance().fetchRange(index, want);
        if (!start) return nullptr;

        // 统计本次实际拿到的块数，并把整条链表接到本地自由链表
        size_t got = 0;
        void* cur = start;
        void* tail = nullptr;
        while (cur != nullptr)
        {
            tail = cur;
            got++;
            cur = *reinterpret_cast<void**>(cur); // 遍历下一个内存块
        }
        if (tail != nullptr)
        {
            *reinterpret_cast<void**>(tail) = freeList_[index];
        }
        freeList_[index] = start;
        freeListSize_[index] += got;

        // 取第一块返回，其余留在本地自由链表
        void* result = start;
        freeList_[index] = *reinterpret_cast<void**>(start);
        freeListSize_[index]--;

        // 需求驱动增长：本地链表耗尽才触发本次补充，说明该大小类分配频繁，
        // 下次申请量翻倍（封顶 BATCH_MAX），减少来回找 CentralCache 的频率
        batchHint_[index] = std::min(batchHint_[index] * 2, BATCH_MAX);

        return result;
    }

    void ThreadCache::returnToCentralCache(void* start, size_t size)
    {
        // 根据大小计算对应的索引
        size_t index = SizeClass::getIndex(size);

        // 获取对齐后的实际块大小
        size_t alignedSize = SizeClass::roundUp(size);

        // 计算要归还内存块数量
        size_t batchNum = freeListSize_[index];
        if (batchNum <= 1) return; // 如果只有一个块，则不归还

        // 保留一部分在ThreadCache中（比如保留1/4）
        size_t keepNum = std::max(batchNum / 4, size_t(1));
        size_t returnNum = batchNum - keepNum;

        // 将内存块串成链表
        char* current = static_cast<char*>(start);
        // 使用对齐后的大小计算分割点
        char* splitNode = current;
        for (size_t i = 0; i < keepNum - 1; ++i)
        {
            splitNode = reinterpret_cast<char*>(*reinterpret_cast<void**>(splitNode));
            if (splitNode == nullptr)
            {
                // 如果链表提前结束，更新实际的返回数量
                returnNum = batchNum - (i + 1);
                break;
            }
        }

        if (splitNode != nullptr)
        {
            // 将要返回的部分和要保留的部分断开
            void* nextNode = *reinterpret_cast<void**>(splitNode);
            *reinterpret_cast<void**>(splitNode) = nullptr; // 断开连接

            // 更新ThreadCache的空闲链表
            freeList_[index] = start;

            // 更新自由链表大小
            freeListSize_[index] = keepNum;

            // 将剩余部分返回给CentralCache
            if (returnNum > 0 && nextNode != nullptr)
            {
                CentralCache::getInstance().returnRange(nextNode, returnNum * alignedSize, index);

                // 闲置驱动收缩：本地块大量闲置到触发回收，说明批量偏大，
                // 下次申请量减半（保底 BATCH_MIN），避免内存长期滞留在本地缓存
                batchHint_[index] = std::max(batchHint_[index] / 2, BATCH_MIN);
            }
        }
    }


} // namespace memoryPool
