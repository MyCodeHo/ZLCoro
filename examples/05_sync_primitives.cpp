#include "zlcoro/sync.hpp"
#include "zlcoro/core/task.hpp"
#include "zlcoro/scheduler/async.hpp"
#include <iostream>
#include <vector>
#include <chrono>
#include <thread>

using namespace zlcoro;

// =============================================================================
// 示例 1: Channel - 基础使用
// =============================================================================

Task<void> example1_channel_basic() {
    std::cout << "\n=== Example 1: Channel Basic ===\n";
    
    Channel<int> ch(3);
    
    auto producer = [&]() -> Task<void> {
        for (int i = 1; i <= 5; ++i) {
            std::cout << "Sending: " << i << "\n";
            co_await ch.send(i);
        }
        ch.close();
        co_return;
    };
    
    auto consumer = [&]() -> Task<void> {
        while (true) {
            auto value = co_await ch.receive();
            if (!value) {
                std::cout << "Channel closed\n";
                break;
            }
            std::cout << "Received: " << *value << "\n";
        }
        co_return;
    };
    
    auto f1 = async_run(producer());
    auto f2 = async_run(consumer());
    
    f1.get();
    f2.get();
    
    co_return;
}

// =============================================================================
// 示例 2: Channel - 多生产者多消费者
// =============================================================================

Task<void> example2_channel_multiple() {
    std::cout << "\n=== Example 2: Multiple Producers & Consumers ===\n";
    
    Channel<std::string> ch(5);
    
    auto producer = [&](int id) -> Task<void> {
        for (int i = 0; i < 3; ++i) {
            std::string msg = "Producer-" + std::to_string(id) + " msg-" + std::to_string(i);
            co_await ch.send(msg);
            std::cout << "Sent: " << msg << "\n";
        }
        co_return;
    };
    
    auto consumer = [&](int id) -> Task<void> {
        for (int i = 0; i < 3; ++i) {
            auto msg = co_await ch.receive();
            if (msg) {
                std::cout << "Consumer-" << id << " received: " << *msg << "\n";
            }
        }
        co_return;
    };
    
    std::vector<std::future<void>> futures;
    
    // 启动 2 个生产者
    futures.push_back(async_run(producer(1)));
    futures.push_back(async_run(producer(2)));
    
    // 启动 2 个消费者
    futures.push_back(async_run(consumer(1)));
    futures.push_back(async_run(consumer(2)));
    
    for (auto& f : futures) {
        f.get();
    }
    
    co_return;
}

// =============================================================================
// 示例 3: Mutex - 保护共享资源
// =============================================================================

Task<void> example3_mutex() {
    std::cout << "\n=== Example 3: Mutex ===\n";
    
    Mutex mtx;
    int counter = 0;
    
    auto increment = [&](int id, int times) -> Task<void> {
        for (int i = 0; i < times; ++i) {
            auto lock = co_await mtx.lock();
            counter++;
            std::cout << "Task-" << id << " incremented counter to " << counter << "\n";
        }
        co_return;
    };
    
    auto f1 = async_run(increment(1, 5));
    auto f2 = async_run(increment(2, 5));
    auto f3 = async_run(increment(3, 5));
    
    f1.get();
    f2.get();
    f3.get();
    
    std::cout << "Final counter: " << counter << "\n";
    
    co_return;
}

// =============================================================================
// 示例 4: WaitGroup - 等待多个任务完成
// =============================================================================

Task<void> example4_wait_group() {
    std::cout << "\n=== Example 4: WaitGroup ===\n";
    
    WaitGroup wg;
    
    auto worker = [&](int id) -> Task<void> {
        std::cout << "Worker-" << id << " starting\n";
        std::this_thread::sleep_for(std::chrono::milliseconds(100 * id));
        std::cout << "Worker-" << id << " done\n";
        wg.done();
        co_return;
    };
    
    wg.add(3);
    
    async_run(worker(1));
    async_run(worker(2));
    async_run(worker(3));
    
    std::cout << "Waiting for all workers...\n";
    co_await wg.wait();
    std::cout << "All workers completed!\n";
    
    co_return;
}

// =============================================================================
// 示例 5: Semaphore - 限制并发数
// =============================================================================

Task<void> example5_semaphore() {
    std::cout << "\n=== Example 5: Semaphore ===\n";
    
    Semaphore sem(2);  // 最多 2 个并发
    
    auto task = [&](int id) -> Task<void> {
        std::cout << "Task-" << id << " waiting for permit\n";
        auto guard = co_await sem.scoped_acquire();
        std::cout << "Task-" << id << " acquired permit (available: " << sem.available() << ")\n";
        
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        
        std::cout << "Task-" << id << " releasing permit\n";
        co_return;  // guard 自动释放
    };
    
    std::vector<std::future<void>> futures;
    for (int i = 1; i <= 5; ++i) {
        futures.push_back(async_run(task(i)));
    }
    
    for (auto& f : futures) {
        f.get();
    }
    
    std::cout << "Final available: " << sem.available() << "\n";
    
    co_return;
}

// =============================================================================
// 示例 6: 生产者-消费者模式（综合）
// =============================================================================

Task<void> example6_producer_consumer() {
    std::cout << "\n=== Example 6: Producer-Consumer Pattern ===\n";
    
    Channel<int> ch(5);
    Semaphore producer_sem(2);  // 最多 2 个生产者同时工作
    WaitGroup wg;
    Mutex result_mtx;
    std::vector<int> results;
    
    auto producer = [&](int id) -> Task<void> {
        auto guard = co_await producer_sem.scoped_acquire();
        
        for (int i = 0; i < 3; ++i) {
            int value = id * 10 + i;
            std::cout << "Producer-" << id << " sending: " << value << "\n";
            co_await ch.send(value);
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        
        wg.done();
        co_return;
    };
    
    auto consumer = [&](int id) -> Task<void> {
        while (true) {
            auto value = co_await ch.receive();
            if (!value) {
                std::cout << "Consumer-" << id << " done\n";
                break;
            }
            
            std::cout << "Consumer-" << id << " received: " << *value << "\n";
            
            {
                auto lock = co_await result_mtx.lock();
                results.push_back(*value);
            }
            
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
        }
        co_return;
    };
    
    wg.add(3);
    
    // 启动 3 个生产者
    async_run(producer(1));
    async_run(producer(2));
    async_run(producer(3));
    
    // 启动 2 个消费者
    auto c1 = async_run(consumer(1));
    auto c2 = async_run(consumer(2));
    
    // 等待所有生产者完成，然后关闭通道
    auto closer = async_run([&]() -> Task<void> {
        co_await wg.wait();
        std::cout << "All producers done, closing channel\n";
        ch.close();
        co_return;
    }());
    
    closer.get();
    c1.get();
    c2.get();
    
    std::cout << "Total items consumed: " << results.size() << "\n";
    
    co_return;
}

// =============================================================================
// 示例 7: 工作池模式
// =============================================================================

Task<void> example7_worker_pool() {
    std::cout << "\n=== Example 7: Worker Pool ===\n";
    
    Channel<int> jobs(10);
    Channel<int> results(10);
    Semaphore worker_sem(3);  // 3 个并发工作者
    WaitGroup wg;
    
    auto worker = [&](int id) -> Task<void> {
        auto guard = co_await worker_sem.scoped_acquire();
        
        while (true) {
            auto job = co_await jobs.receive();
            if (!job) {
                break;  // 没有更多任务
            }
            
            int job_id = *job;
            std::cout << "Worker-" << id << " processing job " << job_id << "\n";
            
            // 模拟工作
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            
            int result = job_id * 2;
            co_await results.send(result);
            std::cout << "Worker-" << id << " completed job " << job_id << " -> " << result << "\n";
        }
        
        wg.done();
        co_return;
    };
    
    // 启动 5 个工作者
    wg.add(5);
    for (int i = 1; i <= 5; ++i) {
        async_run(worker(i));
    }
    
    // 发送任务
    auto sender = async_run([&]() -> Task<void> {
        for (int i = 1; i <= 10; ++i) {
            co_await jobs.send(i);
        }
        jobs.close();
        co_return;
    }());
    
    // 收集结果
    auto collector = async_run([&]() -> Task<void> {
        co_await wg.wait();
        results.close();
        co_return;
    }());
    
    auto result_reader = async_run([&]() -> Task<void> {
        int sum = 0;
        while (true) {
            auto result = co_await results.receive();
            if (!result) {
                break;
            }
            sum += *result;
        }
        std::cout << "Sum of all results: " << sum << "\n";
        co_return;
    }());
    
    sender.get();
    collector.get();
    result_reader.get();
    
    co_return;
}

// =============================================================================
// Main
// =============================================================================

int main() {
    std::cout << "ZLCoro Sync Primitives Examples\n";
    std::cout << "================================\n";
    
    async_run(example1_channel_basic()).get();
    async_run(example2_channel_multiple()).get();
    async_run(example3_mutex()).get();
    async_run(example4_wait_group()).get();
    async_run(example5_semaphore()).get();
    async_run(example6_producer_consumer()).get();
    async_run(example7_worker_pool()).get();
    
    std::cout << "\nAll examples completed!\n";
    
    return 0;
}
