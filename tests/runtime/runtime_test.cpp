#include <gtest/gtest.h>
#include "zlcoro/runtime/runtime.hpp"
#include "zlcoro/core/task.hpp"
#include "zlcoro/sync/timer.hpp"
#include "zlcoro/sync/cancellation.hpp"
#include "zlcoro/sync/rwlock.hpp"

#include <atomic>
#include <chrono>
#include <vector>
#include <thread>

using namespace zlcoro;

// =============================================================================
// Runtime 基础测试
// =============================================================================

TEST(RuntimeTest, Construction) {
    Runtime runtime(2);
    EXPECT_EQ(runtime.thread_count(), 2);
    EXPECT_FALSE(runtime.is_shutdown());
}

TEST(RuntimeTest, DefaultConstruction) {
    Runtime runtime;
    EXPECT_GE(runtime.thread_count(), 1);
    EXPECT_FALSE(runtime.is_shutdown());
}

TEST(RuntimeTest, Shutdown) {
    Runtime runtime(2);
    EXPECT_FALSE(runtime.is_shutdown());
    runtime.shutdown();
    EXPECT_TRUE(runtime.is_shutdown());
}

TEST(RuntimeTest, DoubleShutdown) {
    Runtime runtime(2);
    runtime.shutdown();
    runtime.shutdown();  // 不应崩溃
    EXPECT_TRUE(runtime.is_shutdown());
}

// =============================================================================
// Runtime spawn 测试
// =============================================================================

TEST(RuntimeTest, SpawnSimpleTask) {
    std::atomic<int> counter{0};
    
    {
        Runtime runtime(2);
        
        auto task = [&]() -> Task<void> {
            counter.fetch_add(1);
            co_return;
        };
        
        runtime.spawn(task());
        runtime.run_for(std::chrono::milliseconds(100));
    }
    
    EXPECT_EQ(counter.load(), 1);
}

TEST(RuntimeTest, SpawnMultipleTasks) {
    std::atomic<int> counter{0};
    const int num_tasks = 10;
    
    {
        Runtime runtime(4);
        
        for (int i = 0; i < num_tasks; ++i) {
            runtime.spawn([&]() {
                counter.fetch_add(1);
            });
        }
        
        runtime.run_for(std::chrono::milliseconds(200));
    }
    
    EXPECT_EQ(counter.load(), num_tasks);
}

// =============================================================================
// Runtime block_on 测试
// =============================================================================

TEST(RuntimeTest, BlockOnVoidTask) {
    Runtime runtime(2);
    
    std::atomic<bool> executed{false};
    
    auto task = [&]() -> Task<void> {
        executed = true;
        co_return;
    };
    
    runtime.block_on(task());
    EXPECT_TRUE(executed.load());
}

TEST(RuntimeTest, BlockOnIntTask) {
    Runtime runtime(2);
    
    auto task = []() -> Task<int> {
        co_return 42;
    };
    
    int result = runtime.block_on(task());
    EXPECT_EQ(result, 42);
}

TEST(RuntimeTest, BlockOnStringTask) {
    Runtime runtime(2);
    
    auto task = []() -> Task<std::string> {
        co_return "Hello, ZLCoro!";
    };
    
    std::string result = runtime.block_on(task());
    EXPECT_EQ(result, "Hello, ZLCoro!");
}

// =============================================================================
// CancellationToken 测试
// =============================================================================

TEST(CancellationTest, InitialState) {
    CancellationSource source;
    auto token = source.token();
    
    EXPECT_FALSE(source.is_cancelled());
    EXPECT_FALSE(token.is_cancelled());
}

TEST(CancellationTest, Cancel) {
    CancellationSource source;
    auto token = source.token();
    
    source.cancel();
    
    EXPECT_TRUE(source.is_cancelled());
    EXPECT_TRUE(token.is_cancelled());
}

TEST(CancellationTest, MultipleTokens) {
    CancellationSource source;
    auto token1 = source.token();
    auto token2 = source.token();
    
    source.cancel();
    
    EXPECT_TRUE(token1.is_cancelled());
    EXPECT_TRUE(token2.is_cancelled());
}

TEST(CancellationTest, OnCancelCallback) {
    CancellationSource source;
    auto token = source.token();
    
    std::atomic<bool> called{false};
    token.on_cancel([&]() {
        called = true;
    });
    
    EXPECT_FALSE(called.load());
    source.cancel();
    EXPECT_TRUE(called.load());
}

TEST(CancellationTest, CallbackOnAlreadyCancelled) {
    CancellationSource source;
    source.cancel();
    
    auto token = source.token();
    
    std::atomic<bool> called{false};
    token.on_cancel([&]() {
        called = true;
    });
    
    // 应该立即执行
    EXPECT_TRUE(called.load());
}

TEST(CancellationTest, BoolConversion) {
    CancellationSource source;
    auto token = source.token();
    
    EXPECT_TRUE(static_cast<bool>(token));  // 未取消
    source.cancel();
    EXPECT_FALSE(static_cast<bool>(token));  // 已取消
}

TEST(CancellationTest, ThrowIfCancelled) {
    CancellationSource source;
    auto token = source.token();
    
    // 未取消时不抛异常
    EXPECT_NO_THROW(token.throw_if_cancelled());
    
    source.cancel();
    
    // 取消后抛异常
    EXPECT_THROW(token.throw_if_cancelled(), CancellationToken::CancelledException);
}

// =============================================================================
// Timer 测试
// =============================================================================

TEST(TimerTest, SleepBasic) {
    // 简单测试：使用同步方式测试 sleep
    auto start = std::chrono::steady_clock::now();
    
    // 直接使用 std::this_thread::sleep_for 测试基准
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    
    auto elapsed = std::chrono::steady_clock::now() - start;
    EXPECT_GE(elapsed, std::chrono::milliseconds(40));
}

TEST(TimerTest, SleepZero) {
    // 零延迟应该立即返回
    Timer::SleepAwaiter awaiter(std::chrono::milliseconds(0));
    EXPECT_TRUE(awaiter.await_ready());
}

TEST(TimerTest, DeadlineCalculation) {
    auto now = std::chrono::steady_clock::now();
    auto deadline = Timer::deadline_from_now(std::chrono::milliseconds(100));
    
    EXPECT_GT(deadline, now);
    EXPECT_FALSE(Timer::is_past_deadline(deadline));
    
    auto remaining = Timer::time_until(deadline);
    EXPECT_GT(remaining.count(), 0);
}

// =============================================================================
// RWLock 测试
// =============================================================================

TEST(RWLockTest, ReadLock) {
    Runtime runtime(2);
    
    RWLock lock;
    
    auto task = [&]() -> Task<void> {
        auto guard = co_await lock.read_lock();
        // 持有读锁
        EXPECT_EQ(lock.reader_count(), 1);
    };
    
    runtime.block_on(task());
}

TEST(RWLockTest, WriteLock) {
    Runtime runtime(2);
    
    RWLock lock;
    
    auto task = [&]() -> Task<void> {
        auto guard = co_await lock.write_lock();
        // 持有写锁
        EXPECT_TRUE(lock.is_write_locked());
    };
    
    runtime.block_on(task());
}

TEST(RWLockTest, TryReadLock) {
    RWLock lock;
    
    // 尝试获取读锁
    auto guard = lock.try_read_lock();
    EXPECT_TRUE(guard.has_value());
    EXPECT_EQ(lock.reader_count(), 1);
}

TEST(RWLockTest, TryWriteLock) {
    RWLock lock;
    
    // 尝试获取写锁
    auto guard = lock.try_write_lock();
    EXPECT_TRUE(guard.has_value());
    EXPECT_TRUE(lock.is_write_locked());
}

TEST(RWLockTest, TryWriteLockFailsWithReaders) {
    RWLock lock;
    
    // 先获取读锁
    auto read_guard = lock.try_read_lock();
    EXPECT_TRUE(read_guard.has_value());
    
    // 尝试获取写锁应该失败
    auto write_guard = lock.try_write_lock();
    EXPECT_FALSE(write_guard.has_value());
}

TEST(RWLockTest, MultipleReaders) {
    RWLock lock;
    
    auto guard1 = lock.try_read_lock();
    auto guard2 = lock.try_read_lock();
    auto guard3 = lock.try_read_lock();
    
    EXPECT_TRUE(guard1.has_value());
    EXPECT_TRUE(guard2.has_value());
    EXPECT_TRUE(guard3.has_value());
    EXPECT_EQ(lock.reader_count(), 3);
}

// =============================================================================
// StopToken 测试
// =============================================================================

TEST(StopTokenTest, InitialState) {
    StopToken token;
    EXPECT_FALSE(token.is_stopped());
    EXPECT_TRUE(static_cast<bool>(token));
}

TEST(StopTokenTest, RequestStop) {
    StopToken token;
    token.request_stop();
    EXPECT_TRUE(token.is_stopped());
    EXPECT_FALSE(static_cast<bool>(token));
}

// =============================================================================
// Interval 测试
// =============================================================================

TEST(IntervalTest, Cancel) {
    Interval interval(std::chrono::milliseconds(100));
    EXPECT_FALSE(interval.is_cancelled());
    
    interval.cancel();
    EXPECT_TRUE(interval.is_cancelled());
}

TEST(IntervalTest, Period) {
    Interval interval(std::chrono::milliseconds(50));
    EXPECT_EQ(interval.period(), std::chrono::milliseconds(50));
}

// =============================================================================
// 集成测试
// =============================================================================

TEST(IntegrationTest, ConcurrentTasks) {
    std::atomic<int> counter{0};
    
    {
        Runtime runtime(4);
        
        for (int i = 0; i < 100; ++i) {
            runtime.spawn([&counter]() {
                // 模拟一些工作
                std::this_thread::sleep_for(std::chrono::microseconds(100));
                counter.fetch_add(1);
            });
        }
        
        runtime.run_for(std::chrono::milliseconds(500));
    }
    
    EXPECT_EQ(counter.load(), 100);
}

int main(int argc, char** argv) {
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
