#pragma once
#include "Common.h"

namespace gzq_memoryPool
{

    // 线程本地缓存
    class ThreadCache
    {
    public://单例模式
        static ThreadCache* getInstance()
        {
            static thread_local ThreadCache instance;//局部线程
            return &instance;
        }

        void* allocate(size_t size);
        void deallocate(void* ptr, size_t size);
    private:
        ThreadCache()
        {
            // 初始化自由链表和大小统计
            freeList_.fill(nullptr);
            freeListSize_.fill(0);
            // 动态批量：申请量从最小值起步，随使用情况自适应增减
            batchHint_.fill(BATCH_MIN);
        }

        // 从中心缓存获取内存
        void* fetchFromCentralCache(size_t index);
        // 归还内存到中心缓存
        void returnToCentralCache(void* start, size_t size);

        bool shouldReturnToCentralCache(size_t index);
    private:
        // 动态批量的上下限：每次向 CentralCache 申请的块数在 [BATCH_MIN, BATCH_MAX] 间自适应
        static const size_t BATCH_MIN = 4;
        static const size_t BATCH_MAX = 48;

        // 每个线程的自由链表数组
        std::array<void*, FREE_LIST_SIZE>  freeList_;
        std::array<size_t, FREE_LIST_SIZE> freeListSize_; // 自由链表大小统计
        // 动态批量：每个大小类下次向 CentralCache 申请的块数
        std::array<size_t, FREE_LIST_SIZE> batchHint_;
    };

} // namespace memoryPool
