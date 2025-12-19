#include <gtest/gtest.h>
#include "zlcoro/sync.hpp"
#include "zlcoro/core/task.hpp"
#include "zlcoro/scheduler/async.hpp"
#include <vector>
#include <chrono>
#include <thread>

using namespace zlcoro;

// =============================================================================
// Channel 测试
// =============================================================================

TEST(ChannelTest, BasicSendReceive) {
    Channel<int> ch(1);
    
    auto producer = [&]() -> Task<void> {
        co_await ch.send(42);
        co_return;
    };
    
    auto consumer = [&]() -> Task<int> {
        auto result = co_await ch.receive();
        co_return result.value();
    };
    
    auto f1 = async_run(producer());
    auto f2 = async_run(consumer());
    
    f1.get();
    int value = f2.get();
    
    EXPECT_EQ(value, 42);
}

TEST(ChannelTest, BufferedChannel) {
    Channel<int> ch(3);
    
    // 应该能够发送 3 个值而不阻塞
    EXPECT_TRUE(ch.try_send(1));
    EXPECT_TRUE(ch.try_send(2));
    EXPECT_TRUE(ch.try_send(3));
    EXPECT_FALSE(ch.try_send(4));  // 缓冲区满
    
    EXPECT_EQ(ch.size(), 3);
    
    // 接收值
    EXPECT_EQ(ch.try_receive().value(), 1);
    EXPECT_EQ(ch.try_receive().value(), 2);
    EXPECT_EQ(ch.try_receive().value(), 3);
    EXPECT_FALSE(ch.try_receive().has_value());
}

TEST(ChannelTest, MultipleProducersConsumers) {
    Channel<int> ch(10);
    std::atomic<int> sent{0};
    std::atomic<int> received{0};
    
    auto producer = [&](int id) -> Task<void> {
        for (int i = 0; i < 5; ++i) {
            co_await ch.send(id * 100 + i);
            sent++;
        }
        co_return;
    };
    
    auto consumer = [&]() -> Task<void> {
        for (int i = 0; i < 15; ++i) {
            auto value = co_await ch.receive();
            if (value) {
                received++;
            }
        }
        co_return;
    };
    
    auto f1 = async_run(producer(1));
    auto f2 = async_run(producer(2));
    auto f3 = async_run(producer(3));
    auto f4 = async_run(consumer());
    
    f1.get();
    f2.get();
    f3.get();
    f4.get();
    
    EXPECT_EQ(sent.load(), 15);
    EXPECT_EQ(received.load(), 15);
}

TEST(ChannelTest, CloseChannel) {
    Channel<int> ch(1);
    
    auto consumer = [&]() -> Task<bool> {
        auto value = co_await ch.receive();
        co_return !value.has_value();  // 应该收到 nullopt
    };
    
    auto f = async_run(consumer());
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    
    ch.close();
    
    bool received_close = f.get();
    EXPECT_TRUE(received_close);
    EXPECT_TRUE(ch.is_closed());
}

// =============================================================================
// Mutex 测试
// =============================================================================

TEST(MutexTest, BasicLock) {
    Mutex mtx;
    int counter = 0;
    
    auto task = [&]() -> Task<void> {
        auto lock = co_await mtx.lock();
        counter++;
        co_return;
    };
    
    auto f = async_run(task());
    f.get();
    
    EXPECT_EQ(counter, 1);
}

TEST(MutexTest, MultipleTasks) {
    Mutex mtx;
    int counter = 0;
    
    auto task = [&]() -> Task<void> {
        for (int i = 0; i < 100; ++i) {
            auto lock = co_await mtx.lock();
            counter++;
        }
        co_return;
    };
    
    std::vector<std::future<void>> futures;
    for (int i = 0; i < 10; ++i) {
        futures.push_back(async_run(task()));
    }
    
    for (auto& f : futures) {
        f.get();
    }
    
    EXPECT_EQ(counter, 1000);
}

TEST(MutexTest, TryLock) {
    Mutex mtx;
    
    EXPECT_TRUE(mtx.try_lock());
    EXPECT_FALSE(mtx.try_lock());  // 已被锁定
    
    mtx.unlock();
    EXPECT_TRUE(mtx.try_lock());
    mtx.unlock();
}

// =============================================================================
// WaitGroup 测试
// =============================================================================

TEST(WaitGroupTest, BasicWait) {
    WaitGroup wg;
    std::atomic<int> counter{0};
    
    auto worker = [&]() -> Task<void> {
        counter++;
        wg.done();
        co_return;
    };
    
    auto coordinator = [&]() -> Task<void> {
        wg.add(3);
        
        async_run(worker());
        async_run(worker());
        async_run(worker());
        
        co_await wg.wait();
        co_return;
    };
    
    auto f = async_run(coordinator());
    f.get();
    
    EXPECT_EQ(counter.load(), 3);
    EXPECT_EQ(wg.count(), 0);
}

TEST(WaitGroupTest, MultipleWaiters) {
    WaitGroup wg;
    std::atomic<int> done_count{0};
    
    auto worker = [&]() -> Task<void> {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        wg.done();
        co_return;
    };
    
    auto waiter = [&]() -> Task<void> {
        co_await wg.wait();
        done_count++;
        co_return;
    };
    
    wg.add(5);
    
    // 启动等待者
    std::vector<std::future<void>> waiter_futures;
    for (int i = 0; i < 3; ++i) {
        waiter_futures.push_back(async_run(waiter()));
    }
    
    // 启动工作者
    for (int i = 0; i < 5; ++i) {
        async_run(worker());
    }
    
    // 等待所有等待者完成
    for (auto& f : waiter_futures) {
        f.get();
    }
    
    EXPECT_EQ(done_count.load(), 3);
}

// =============================================================================
// Semaphore 测试
// =============================================================================

TEST(SemaphoreTest, BasicAcquireRelease) {
    Semaphore sem(1);
    
    auto task = [&]() -> Task<void> {
        co_await sem.acquire();
        // 临界区
        sem.release();
        co_return;
    };
    
    auto f = async_run(task());
    f.get();
    
    EXPECT_EQ(sem.available(), 1);
}

TEST(SemaphoreTest, LimitConcurrency) {
    Semaphore sem(3);
    std::atomic<int> concurrent{0};
    std::atomic<int> max_concurrent{0};
    
    auto task = [&]() -> Task<void> {
        co_await sem.acquire();
        
        int current = ++concurrent;
        if (current > max_concurrent) {
            max_concurrent.store(current);
        }
        
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        
        --concurrent;
        sem.release();
        co_return;
    };
    
    std::vector<std::future<void>> futures;
    for (int i = 0; i < 10; ++i) {
        futures.push_back(async_run(task()));
    }
    
    for (auto& f : futures) {
        f.get();
    }
    
    EXPECT_LE(max_concurrent.load(), 3);
    EXPECT_GT(max_concurrent.load(), 0);
}

TEST(SemaphoreTest, ScopedAcquire) {
    Semaphore sem(2);
    
    auto task = [&]() -> Task<void> {
        auto guard = co_await sem.scoped_acquire();
        // guard 析构时自动释放
        co_return;
    };
    
    auto f = async_run(task());
    f.get();
    
    EXPECT_EQ(sem.available(), 2);
}

TEST(SemaphoreTest, TryAcquire) {
    Semaphore sem(2);
    
    EXPECT_TRUE(sem.try_acquire());
    EXPECT_TRUE(sem.try_acquire());
    EXPECT_FALSE(sem.try_acquire());  // 已满
    
    sem.release();
    EXPECT_TRUE(sem.try_acquire());
    
    sem.release();
    sem.release();
    EXPECT_EQ(sem.available(), 2);
}

// =============================================================================
// 集成测试：生产者-消费者模式
// =============================================================================

TEST(IntegrationTest, ProducerConsumerWithSemaphore) {
    Channel<int> ch(5);
    Semaphore sem(3);  // 最多 3 个生产者同时工作
    WaitGroup wg;
    std::atomic<int> sum{0};
    
    auto producer = [&](int id) -> Task<void> {
        auto guard = co_await sem.scoped_acquire();
        
        for (int i = 0; i < 10; ++i) {
            co_await ch.send(id * 100 + i);
        }
        
        wg.done();
        co_return;
    };
    
    auto consumer = [&]() -> Task<void> {
        for (int i = 0; i < 50; ++i) {
            auto value = co_await ch.receive();
            if (value) {
                sum += *value;
            }
        }
        co_return;
    };
    
    // 定义等待协程，避免 lambda 生命周期问题
    auto waiter = [&]() -> Task<void> {
        co_await wg.wait();
        ch.close();
        co_return;
    };
    
    wg.add(5);
    
    // 启动生产者
    for (int i = 0; i < 5; ++i) {
        async_run(producer(i));
    }
    
    // 启动消费者
    auto consumer_future = async_run(consumer());
    
    // 等待所有生产者完成
    auto wait_future = async_run(waiter());
    
    wait_future.get();
    consumer_future.get();
    
    // 验证总和
    int expected_sum = 0;
    for (int id = 0; id < 5; ++id) {
        for (int i = 0; i < 10; ++i) {
            expected_sum += id * 100 + i;
        }
    }
    
    EXPECT_EQ(sum.load(), expected_sum);
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
