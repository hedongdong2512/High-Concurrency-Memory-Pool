#pragma once
#include "Common.h"
#include <mutex>
#include <unordered_map>
#include <array>
#include <atomic>
#include <chrono>

namespace gzq_memoryPool
{

    // 缓存行填充：让每个 span 的跟踪信息独占一个缓存行（64 字节对齐）。
    // 多线程同时获取/归还相邻 span 的内存块时会各自更新 freeCount；
    // 若两个 SpanTracker 落在同一缓存行，两个核会互相把对方的缓存行打失效（伪共享），
    // alignas(64) 强制每个 SpanTracker 起始于一行的开头，彼此不再共享缓存行。
    struct alignas(64) SpanTracker {
        std::atomic<void*> spanAddr{ nullptr };
        std::atomic<size_t> numPages{ 0 };
        std::atomic<size_t> blockCount{ 0 };
        std::atomic<size_t> freeCount{ 0 }; // 用于追踪span中还有多少块是空闲的，如果所有块都空闲，则归还span给PageCache
    };

    // 缓存行填充：把每个大小类的热点数据（自旋锁、延迟归还计数、上次归还时间）
    // 打包到一个 64 字节对齐的结构体中。不同线程操作不同大小类时，
    // 锁与计数值不再与相邻大小类挤在同一缓存行，消除锁竞争路径上的伪共享。
    struct alignas(64) BucketCtl {
        std::atomic_flag lock{};
        std::atomic<size_t> delayCount{ 0 };
        std::chrono::steady_clock::time_point lastReturnTime{};

        BucketCtl()
        {
            lock.clear();
            lastReturnTime = std::chrono::steady_clock::now();
        }
    };

    class CentralCache
    {
    public:
        static CentralCache& getInstance()
        {
            static CentralCache instance;
            return instance;
        }

        // 从中心缓存获取内存：一次最多给 batchNum 块（动态批量，返回块链表头）
        void* fetchRange(size_t index, size_t batchNum);
        void returnRange(void* start, size_t size, size_t index);

    private:
        // 初始化所有原子指针为nullptr
        CentralCache();
        // 从页缓存获取内存
        void* fetchFromPageCache(size_t size);

        // 获取span信息
        SpanTracker* getSpanTracker(void* blockAddr);

        // 更新span的空闲计数并检查是否可以归还
        void updateSpanFreeCount(SpanTracker* tracker, size_t newFreeBlocks, size_t index);

    private:
        // 中心缓存的自由链表
        std::array<std::atomic<void*>, FREE_LIST_SIZE> centralFreeList_;

        // 每个大小类的锁与延迟归还状态（独占缓存行，消除伪共享）
        std::array<BucketCtl, FREE_LIST_SIZE> bucketCtl_;

        // 使用数组存储span信息，避免map的开销
        std::array<SpanTracker, 1024> spanTrackers_;
        std::atomic<size_t> spanCount_{ 0 };

        // 延迟归还相关的常量
        static const size_t MAX_DELAY_COUNT = 48;  // 最大延迟计数
        static const std::chrono::milliseconds DELAY_INTERVAL;  // 延迟间隔

        bool shouldPerformDelayedReturn(size_t index, size_t currentCount, std::chrono::steady_clock::time_point currentTime);
        void performDelayedReturn(size_t index);
    };

} // namespace memoryPool
