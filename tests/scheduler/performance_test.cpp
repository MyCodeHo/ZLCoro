#include "zlcoro/scheduler/thread_pool.hpp"
#include "zlcoro/scheduler/work_stealing_scheduler.hpp"
#include "zlcoro/scheduler/scheduler.hpp"
#include "zlcoro/scheduler/async.hpp"
#include "zlcoro/core/task.hpp"
#include "zlcoro/utils/memory_pool.hpp"
#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <thread>
#include <vector>
#include <iostream>

using namespace zlcoro;

// =============================================================================
// 工作窃取调度器测试
// =============================================================================

TEST(WorkStealingSchedulerTest, BasicSubmit) {
    WorkStealingScheduler scheduler(4);
    
    std::atomic<int> counter{0};
    
    for (int i = 0; i < 100; ++i) {
        scheduler.submit([&counter] {
            counter++;
        });
    }
    
    // 等待任务完成
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    
    EXPECT_EQ(counter.load(), 100);
}

TEST(WorkStealingSchedulerTest, CoroutineSchedule) {
    WorkStealingScheduler scheduler(4);
    
    std::atomic<int> counter{0};
    std::atomic<bool> done{false};
    
    auto coro = [](std::atomic<int>& c, std::atomic<bool>& d) -> Task<void> {
        c++;
        d = true;
        co_return;
    };
    
    auto task = coro(counter, done);
    auto handle = task.handle();  // 保存 handle
    scheduler.schedule(handle);
    
    // 等待协程完成（使用超时避免无限等待）
    int wait_count = 0;
    while (!done.load() && wait_count < 1000) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        ++wait_count;
    }
    
    EXPECT_TRUE(done.load());
    EXPECT_EQ(counter.load(), 1);
}

TEST(WorkStealingSchedulerTest, HighConcurrency) {
    WorkStealingScheduler scheduler(8);
    
    constexpr int num_tasks = 10000;
    std::atomic<int> counter{0};
    
    for (int i = 0; i < num_tasks; ++i) {
        scheduler.submit([&counter] {
            counter++;
        });
    }
    
    // 等待所有任务完成（使用超时避免无限等待）
    int wait_count = 0;
    while (counter.load() < num_tasks && wait_count < 5000) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        ++wait_count;
    }
    
    EXPECT_EQ(counter.load(), num_tasks);
}

// =============================================================================
// 内存池测试
// =============================================================================

struct TestObject {
    int value;
    std::string name;
    
    TestObject() : value(0), name("default") {}
    TestObject(int v, const std::string& n) : value(v), name(n) {}
};

TEST(ObjectPoolTest, AcquireAndRelease) {
    ObjectPool<TestObject> pool;
    
    auto* obj1 = pool.acquire(42, "test1");
    EXPECT_EQ(obj1->value, 42);
    EXPECT_EQ(obj1->name, "test1");
    
    pool.release(obj1);
    
    // 获取的对象应该被重用
    auto* obj2 = pool.acquire(100, "test2");
    EXPECT_EQ(obj2->value, 100);
    EXPECT_EQ(obj2->name, "test2");
    
    pool.release(obj2);
}

TEST(ObjectPoolTest, MultipleObjects) {
    ObjectPool<TestObject> pool(10, 100);
    
    std::vector<TestObject*> objects;
    for (int i = 0; i < 50; ++i) {
        objects.push_back(pool.acquire(i, "obj" + std::to_string(i)));
    }
    
    for (auto* obj : objects) {
        pool.release(obj);
    }
    
    EXPECT_GE(pool.size(), 50u);
}

TEST(PooledPtrTest, RAII) {
    ObjectPool<TestObject> pool(0, 100);  // 初始大小为0
    
    {
        auto ptr = PooledPtr<TestObject>(pool.acquire(1, "raii"), &pool);
        EXPECT_EQ(ptr->value, 1);
    }
    // ptr 析构后对象应该返回池
    EXPECT_EQ(pool.size(), 1u);
}

TEST(FixedSizeAllocatorTest, AllocateAndDeallocate) {
    FixedSizeAllocator allocator(64);
    
    void* ptr1 = allocator.allocate();
    EXPECT_NE(ptr1, nullptr);
    
    void* ptr2 = allocator.allocate();
    EXPECT_NE(ptr2, nullptr);
    EXPECT_NE(ptr1, ptr2);
    
    allocator.deallocate(ptr1);
    
    // 下一次分配应该重用 ptr1
    void* ptr3 = allocator.allocate();
    EXPECT_EQ(ptr3, ptr1);
    
    allocator.deallocate(ptr2);
    allocator.deallocate(ptr3);
}

// =============================================================================
// 优化后的 async_run 测试
// =============================================================================

Task<int> compute_value(int x) {
    co_return x * 2;
}

Task<void> void_task(std::atomic<bool>& done) {
    done = true;
    co_return;
}

TEST(OptimizedAsyncRunTest, BasicInt) {
    auto future = async_run(compute_value(21));
    EXPECT_EQ(future.get(), 42);
}

TEST(OptimizedAsyncRunTest, VoidTask) {
    std::atomic<bool> done{false};
    
    auto task = [](std::atomic<bool>& d) -> Task<void> {
        d = true;
        co_return;
    };
    
    auto coro = task(done);
    auto future = async_run(std::move(coro));
    future.get();
    
    EXPECT_TRUE(done.load());
}

TEST(OptimizedAsyncRunTest, MultipleAsync) {
    std::vector<std::future<int>> futures;
    
    for (int i = 0; i < 10; ++i) {
        futures.push_back(async_run(compute_value(i)));
    }
    
    for (int i = 0; i < 10; ++i) {
        EXPECT_EQ(futures[i].get(), i * 2);
    }
}

TEST(FireAndForgetTest, Basic) {
    std::atomic<int> counter{0};
    
    auto task = [](std::atomic<int>& c) -> Task<void> {
        c++;
        co_return;
    };
    
    for (int i = 0; i < 5; ++i) {
        fire_and_forget(task(counter));
    }
    
    // 等待所有任务完成
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    
    EXPECT_EQ(counter.load(), 5);
}

// =============================================================================
// 性能基准测试（简单版本）
// =============================================================================

TEST(PerformanceTest, ThreadPoolVsWorkStealing) {
    constexpr int num_tasks = 10000;
    std::atomic<int> counter{0};
    
    // 测试标准 ThreadPool
    {
        ThreadPool pool(4);
        counter = 0;
        
        auto start = std::chrono::high_resolution_clock::now();
        
        for (int i = 0; i < num_tasks; ++i) {
            pool.submit([&counter] {
                counter++;
            });
        }
        
        while (counter.load() < num_tasks) {
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
        
        auto end = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);
        
        std::cout << "ThreadPool: " << duration.count() << " us for " << num_tasks << " tasks\n";
    }
    
    // 测试 WorkStealingScheduler
    {
        WorkStealingScheduler scheduler(4);
        counter = 0;
        
        auto start = std::chrono::high_resolution_clock::now();
        
        for (int i = 0; i < num_tasks; ++i) {
            scheduler.submit([&counter] {
                counter++;
            });
        }
        
        while (counter.load() < num_tasks) {
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
        
        auto end = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);
        
        std::cout << "WorkStealingScheduler: " << duration.count() << " us for " << num_tasks << " tasks\n";
    }
}

TEST(PerformanceTest, ObjectPoolVsNew) {
    constexpr int iterations = 100000;
    
    // 测试普通 new/delete
    {
        auto start = std::chrono::high_resolution_clock::now();
        
        for (int i = 0; i < iterations; ++i) {
            auto* obj = new TestObject(i, "test");
            delete obj;
        }
        
        auto end = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);
        
        std::cout << "new/delete: " << duration.count() << " us for " << iterations << " allocations\n";
    }
    
    // 测试 ObjectPool
    {
        ObjectPool<TestObject> pool;
        
        auto start = std::chrono::high_resolution_clock::now();
        
        for (int i = 0; i < iterations; ++i) {
            auto* obj = pool.acquire(i, "test");
            pool.release(obj);
        }
        
        auto end = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);
        
        std::cout << "ObjectPool: " << duration.count() << " us for " << iterations << " allocations\n";
    }
}
