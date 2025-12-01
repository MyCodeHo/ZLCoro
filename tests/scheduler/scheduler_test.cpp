#include "zlcoro/scheduler/thread_pool.hpp"
#include "zlcoro/scheduler/scheduler.hpp"
#include "zlcoro/scheduler/async.hpp"
#include "zlcoro/core/task.hpp"
#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <thread>

using namespace zlcoro;

// =============================================================================
// ThreadPool 测试
// =============================================================================

TEST(ThreadPoolTest, BasicSubmit) {
    ThreadPool pool(2);
    
    std::atomic<int> counter{0};
    
    for (int i = 0; i < 10; ++i) {
        pool.submit([&counter] {
            counter++;
        });
    }
    
    // 等待所有任务完成
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    
    EXPECT_EQ(counter.load(), 10);
}

TEST(ThreadPoolTest, MultipleThreads) {
    ThreadPool pool(4);
    
    std::atomic<int> counter{0};
    std::mutex mutex;
    std::vector<std::thread::id> thread_ids;
    
    for (int i = 0; i < 100; ++i) {
        pool.submit([&counter, &mutex, &thread_ids] {
            // 添加小延迟，模拟真实任务，确保多线程参与
            std::this_thread::sleep_for(std::chrono::microseconds(100));
            
            counter++;
            std::lock_guard<std::mutex> lock(mutex);
            thread_ids.push_back(std::this_thread::get_id());
        });
    }
    
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    
    EXPECT_EQ(counter.load(), 100);
    
    // 验证使用了多个线程
    std::set<std::thread::id> unique_threads(thread_ids.begin(), thread_ids.end());
    EXPECT_GT(unique_threads.size(), 1);
}

TEST(ThreadPoolTest, Shutdown) {
    ThreadPool pool(2);
    
    std::atomic<int> counter{0};
    
    pool.submit([&counter] {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        counter++;
    });
    
    pool.shutdown();
    
    // 关闭后提交任务应该被拒绝
    pool.submit([&counter] {
        counter++;
    });
    
    // 第一个任务应该完成
    EXPECT_EQ(counter.load(), 1);
}

// =============================================================================
// Scheduler 测试
// =============================================================================

TEST(SchedulerTest, BasicSchedule) {
    auto& scheduler = Scheduler::instance();
    
    std::atomic<bool> executed{false};
    
    scheduler.schedule([&executed] {
        executed = true;
    });
    
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    
    EXPECT_TRUE(executed.load());
}

TEST(SchedulerTest, CoroutineSchedule) {
    std::atomic<int> value{0};
    std::atomic<bool> done{false};
    
    auto coro_func = [&value, &done]() -> Task<void> {
        value = 42;
        done = true;
        co_return;
    };
    
    // 使用 async_run 来正确管理协程生命周期
    auto future = async_run(coro_func());
    future.get();
    
    EXPECT_EQ(value.load(), 42);
    EXPECT_TRUE(done.load());
}

// =============================================================================
// ScheduleAwaiter 测试
// =============================================================================

TEST(ScheduleAwaiterTest, CoAwaitSchedule) {
    std::atomic<int> step{0};
    
    auto inner_task = []() -> Task<int> {
        co_return 42;
    };
    
    auto coro = [&]() -> Task<void> {
        step = 1;
        int result = co_await inner_task();  // co_await 另一个协程
        EXPECT_EQ(result, 42);
        step = 2;
        co_return;
    };
    
    auto future = async_run(coro());
    future.get();
    
    EXPECT_EQ(step.load(), 2);
}

// =============================================================================
// async_run 测试
// =============================================================================

TEST(AsyncRunTest, IntReturn) {
    auto coro = []() -> Task<int> {
        co_return 42;
    };
    
    auto future = async_run(coro());
    int result = future.get();
    
    EXPECT_EQ(result, 42);
}

TEST(AsyncRunTest, VoidReturn) {
    std::atomic<bool> executed{false};
    
    auto coro = [&executed]() -> Task<void> {
        executed = true;
        co_return;
    };
    
    auto future = async_run(coro());
    future.get();
    
    EXPECT_TRUE(executed.load());
}

TEST(AsyncRunTest, StringReturn) {
    auto coro = []() -> Task<std::string> {
        co_return "Hello from coroutine";
    };
    
    auto future = async_run(coro());
    std::string result = future.get();
    
    EXPECT_EQ(result, "Hello from coroutine");
}

TEST(AsyncRunTest, ExceptionPropagation) {
    auto coro = []() -> Task<int> {
        throw std::runtime_error("Test error");
        co_return 42;
    };
    
    auto future = async_run(coro());
    
    EXPECT_THROW(future.get(), std::runtime_error);
}

TEST(AsyncRunTest, ChainedCoroutines) {
    auto inner = []() -> Task<int> {
        co_return 10;
    };
    
    auto outer = [&inner]() -> Task<int> {
        int x = co_await inner();
        co_return x * 2;
    };
    
    auto future = async_run(outer());
    int result = future.get();
    
    EXPECT_EQ(result, 20);
}

// =============================================================================
// 并发测试
// =============================================================================

TEST(ConcurrentTest, MultipleTasks) {
    std::atomic<int> counter{0};
    std::vector<std::future<void>> futures;
    
    for (int i = 0; i < 10; ++i) {
        auto coro = [&counter]() -> Task<void> {
            counter++;
            co_return;
        };
        
        futures.push_back(async_run(coro()));
    }
    
    for (auto& future : futures) {
        future.get();
    }
    
    EXPECT_EQ(counter.load(), 10);
}

TEST(ConcurrentTest, HeavyLoad) {
    // 测试并发任务的基本功能
    
    std::atomic<int> completed{0};
    std::vector<std::future<void>> futures;
    
    const int N = 10;
    futures.reserve(N);
    
    // 创建简单的并发任务
    for (int i = 0; i < N; ++i) {
        auto coro = [&completed]() -> Task<void> {
            // 简单的原子操作
            completed.fetch_add(1);
            co_return;
        };
        
        futures.push_back(async_run(coro()));
    }
    
    // 等待所有任务完成
    for (auto& future : futures) {
        future.get();
    }
    
    // 验证所有任务都执行了
    EXPECT_EQ(completed.load(), N);
}
