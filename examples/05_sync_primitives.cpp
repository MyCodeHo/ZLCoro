#include "zlcoro/sync.hpp"
#include "zlcoro/core/task.hpp"
#include "zlcoro/scheduler/async.hpp"
#include <iostream>
#include <vector>
#include <chrono>
#include <thread>
#include <memory>

using namespace zlcoro;

// =============================================================================
// 示例 1: Channel - 基础使用
// =============================================================================

// 使用独立函数代替 lambda，避免生命周期问题
Task<void> channel_producer(std::shared_ptr<Channel<int>> ch) {
    for (int i = 1; i <= 5; ++i) {
        std::cout << "Sending: " << i << "\n";
        co_await ch->send(i);
    }
    ch->close();
    co_return;
}

Task<void> channel_consumer(std::shared_ptr<Channel<int>> ch) {
    while (true) {
        auto value = co_await ch->receive();
        if (!value) {
            std::cout << "Channel closed\n";
            break;
        }
        std::cout << "Received: " << *value << "\n";
    }
    co_return;
}

Task<void> example1_channel_basic() {
    std::cout << "\n=== Example 1: Channel Basic ===\n";
    
    auto ch = std::make_shared<Channel<int>>(3);
    
    auto f1 = async_run(channel_producer(ch));
    auto f2 = async_run(channel_consumer(ch));
    
    f1.get();
    f2.get();
    
    co_return;
}

// =============================================================================
// 示例 2: Channel - 多生产者多消费者
// =============================================================================

Task<void> string_producer(std::shared_ptr<Channel<std::string>> ch, int id) {
    for (int i = 0; i < 3; ++i) {
        std::string msg = "Producer-" + std::to_string(id) + " msg-" + std::to_string(i);
        co_await ch->send(msg);
        std::cout << "Sent: " << msg << "\n";
    }
    co_return;
}

Task<void> string_consumer(std::shared_ptr<Channel<std::string>> ch, int id) {
    for (int i = 0; i < 3; ++i) {
        auto msg = co_await ch->receive();
        if (msg) {
            std::cout << "Consumer-" << id << " received: " << *msg << "\n";
        }
    }
    co_return;
}

Task<void> example2_channel_multiple() {
    std::cout << "\n=== Example 2: Multiple Producers & Consumers ===\n";
    
    auto ch = std::make_shared<Channel<std::string>>(5);
    
    std::vector<std::future<void>> futures;
    
    // 启动 2 个生产者
    futures.push_back(async_run(string_producer(ch, 1)));
    futures.push_back(async_run(string_producer(ch, 2)));
    
    // 启动 2 个消费者
    futures.push_back(async_run(string_consumer(ch, 1)));
    futures.push_back(async_run(string_consumer(ch, 2)));
    
    for (auto& f : futures) {
        f.get();
    }
    
    co_return;
}

// =============================================================================
// 示例 3: Mutex - 保护共享资源
// =============================================================================

Task<void> mutex_incrementer(std::shared_ptr<Mutex> mtx, std::shared_ptr<int> counter, int id, int times) {
    for (int i = 0; i < times; ++i) {
        auto lock = co_await mtx->lock();
        (*counter)++;
        std::cout << "Task-" << id << " incremented counter to " << *counter << "\n";
    }
    co_return;
}

Task<void> example3_mutex() {
    std::cout << "\n=== Example 3: Mutex ===\n";
    
    auto mtx = std::make_shared<Mutex>();
    auto counter = std::make_shared<int>(0);
    
    auto f1 = async_run(mutex_incrementer(mtx, counter, 1, 5));
    auto f2 = async_run(mutex_incrementer(mtx, counter, 2, 5));
    auto f3 = async_run(mutex_incrementer(mtx, counter, 3, 5));
    
    f1.get();
    f2.get();
    f3.get();
    
    std::cout << "Final counter: " << *counter << "\n";
    
    co_return;
}

// =============================================================================
// 示例 4: WaitGroup - 等待多个任务完成
// =============================================================================

Task<void> waitgroup_worker(std::shared_ptr<WaitGroup> wg, int id) {
    std::cout << "Worker-" << id << " starting\n";
    co_await schedule();  // 切换到线程池
    std::this_thread::sleep_for(std::chrono::milliseconds(100 * id));
    std::cout << "Worker-" << id << " done\n";
    wg->done();
    co_return;
}

Task<void> example4_wait_group() {
    std::cout << "\n=== Example 4: WaitGroup ===\n";
    
    auto wg = std::make_shared<WaitGroup>();
    
    wg->add(3);
    
    fire_and_forget(waitgroup_worker(wg, 1));
    fire_and_forget(waitgroup_worker(wg, 2));
    fire_and_forget(waitgroup_worker(wg, 3));
    
    std::cout << "Waiting for all workers...\n";
    co_await wg->wait();
    std::cout << "All workers completed!\n";
    
    co_return;
}

// =============================================================================
// 示例 5: Semaphore - 限制并发数
// =============================================================================

Task<void> semaphore_task(std::shared_ptr<Semaphore> sem, int id) {
    std::cout << "Task-" << id << " waiting for permit\n";
    auto guard = co_await sem->scoped_acquire();
    std::cout << "Task-" << id << " acquired permit (available: " << sem->available() << ")\n";
    
    co_await schedule();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    
    std::cout << "Task-" << id << " releasing permit\n";
    co_return;  // guard 自动释放
}

Task<void> example5_semaphore() {
    std::cout << "\n=== Example 5: Semaphore ===\n";
    
    auto sem = std::make_shared<Semaphore>(2);  // 最多 2 个并发
    
    std::vector<std::future<void>> futures;
    for (int i = 1; i <= 5; ++i) {
        futures.push_back(async_run(semaphore_task(sem, i)));
    }
    
    for (auto& f : futures) {
        f.get();
    }
    
    std::cout << "Final available: " << sem->available() << "\n";
    
    co_return;
}

// =============================================================================
// 示例 6: 生产者-消费者模式（综合）
// =============================================================================

// 共享状态结构
struct ProducerConsumerState {
    std::shared_ptr<Channel<int>> ch;
    std::shared_ptr<Semaphore> producer_sem;
    std::shared_ptr<WaitGroup> wg;
    std::shared_ptr<Mutex> result_mtx;
    std::shared_ptr<std::vector<int>> results;
};

Task<void> pc_producer(std::shared_ptr<ProducerConsumerState> state, int id) {
    auto guard = co_await state->producer_sem->scoped_acquire();
    
    for (int i = 0; i < 3; ++i) {
        int value = id * 10 + i;
        std::cout << "Producer-" << id << " sending: " << value << "\n";
        co_await state->ch->send(value);
        co_await schedule();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    
    state->wg->done();
    co_return;
}

Task<void> pc_consumer(std::shared_ptr<ProducerConsumerState> state, int id) {
    while (true) {
        auto value = co_await state->ch->receive();
        if (!value) {
            std::cout << "Consumer-" << id << " done\n";
            break;
        }
        
        std::cout << "Consumer-" << id << " received: " << *value << "\n";
        
        {
            auto lock = co_await state->result_mtx->lock();
            state->results->push_back(*value);
        }
        
        co_await schedule();
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
    }
    co_return;
}

Task<void> pc_closer(std::shared_ptr<ProducerConsumerState> state) {
    co_await state->wg->wait();
    std::cout << "All producers done, closing channel\n";
    state->ch->close();
    co_return;
}

Task<void> example6_producer_consumer() {
    std::cout << "\n=== Example 6: Producer-Consumer Pattern ===\n";
    
    auto state = std::make_shared<ProducerConsumerState>();
    state->ch = std::make_shared<Channel<int>>(5);
    state->producer_sem = std::make_shared<Semaphore>(2);
    state->wg = std::make_shared<WaitGroup>();
    state->result_mtx = std::make_shared<Mutex>();
    state->results = std::make_shared<std::vector<int>>();
    
    state->wg->add(3);
    
    // 启动 3 个生产者
    fire_and_forget(pc_producer(state, 1));
    fire_and_forget(pc_producer(state, 2));
    fire_and_forget(pc_producer(state, 3));
    
    // 启动 2 个消费者
    auto c1 = async_run(pc_consumer(state, 1));
    auto c2 = async_run(pc_consumer(state, 2));
    
    // 等待所有生产者完成，然后关闭通道
    auto closer = async_run(pc_closer(state));
    
    closer.get();
    c1.get();
    c2.get();
    
    std::cout << "Total items consumed: " << state->results->size() << "\n";
    
    co_return;
}

// =============================================================================
// 示例 7: 工作池模式
// =============================================================================

struct WorkerPoolState {
    std::shared_ptr<Channel<int>> jobs;
    std::shared_ptr<Channel<int>> results;
    std::shared_ptr<Semaphore> worker_sem;
    std::shared_ptr<WaitGroup> wg;
};

Task<void> wp_worker(std::shared_ptr<WorkerPoolState> state, int id) {
    auto guard = co_await state->worker_sem->scoped_acquire();
    
    while (true) {
        auto job = co_await state->jobs->receive();
        if (!job) {
            break;  // 没有更多任务
        }
        
        int job_id = *job;
        std::cout << "Worker-" << id << " processing job " << job_id << "\n";
        
        // 模拟工作
        co_await schedule();
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        
        int result = job_id * 2;
        co_await state->results->send(result);
        std::cout << "Worker-" << id << " completed job " << job_id << " -> " << result << "\n";
    }
    
    state->wg->done();
    co_return;
}

Task<void> wp_sender(std::shared_ptr<WorkerPoolState> state) {
    for (int i = 1; i <= 10; ++i) {
        co_await state->jobs->send(i);
    }
    state->jobs->close();
    co_return;
}

Task<void> wp_collector(std::shared_ptr<WorkerPoolState> state) {
    co_await state->wg->wait();
    state->results->close();
    co_return;
}

Task<void> wp_reader(std::shared_ptr<WorkerPoolState> state) {
    int sum = 0;
    while (true) {
        auto result = co_await state->results->receive();
        if (!result) {
            break;
        }
        sum += *result;
    }
    std::cout << "Sum of all results: " << sum << "\n";
    co_return;
}

Task<void> example7_worker_pool() {
    std::cout << "\n=== Example 7: Worker Pool ===\n";
    
    auto state = std::make_shared<WorkerPoolState>();
    state->jobs = std::make_shared<Channel<int>>(10);
    state->results = std::make_shared<Channel<int>>(10);
    state->worker_sem = std::make_shared<Semaphore>(3);
    state->wg = std::make_shared<WaitGroup>();
    
    // 启动 5 个工作者
    state->wg->add(5);
    for (int i = 1; i <= 5; ++i) {
        fire_and_forget(wp_worker(state, i));
    }
    
    // 发送任务
    auto sender = async_run(wp_sender(state));
    
    // 收集结果
    auto collector = async_run(wp_collector(state));
    
    auto result_reader = async_run(wp_reader(state));
    
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
    
    // 等待一下确保所有后台任务完成
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    
    return 0;
}
