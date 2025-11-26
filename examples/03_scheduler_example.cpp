#include "zlcoro/core/task.hpp"
#include "zlcoro/scheduler/scheduler.hpp"
#include "zlcoro/scheduler/async.hpp"
#include <iostream>
#include <thread>
#include <chrono>

using namespace zlcoro;

// =============================================================================
// 示例 1: 基础异步任务
// =============================================================================

Task<int> simple_async_task() {
    std::cout << "  [线程 " << std::this_thread::get_id() << "] 开始计算\n";
    co_return 42;
}

void example1_basic() {
    std::cout << "\n=== 示例 1: 基础异步任务 ===\n";
    std::cout << "[主线程 " << std::this_thread::get_id() << "]\n";
    
    auto future = async_run(simple_async_task());
    int result = future.get();
    
    std::cout << "结果: " << result << "\n";
}

// =============================================================================
// 示例 2: 获取线程信息
// =============================================================================

Task<std::thread::id> get_thread_id() {
    co_return std::this_thread::get_id();
}

void example2_thread_info() {
    std::cout << "\n=== 示例 2: 线程信息 ===\n";
    std::cout << "主线程 ID: " << std::this_thread::get_id() << "\n";
    
    auto future = async_run(get_thread_id());
    auto worker_id = future.get();
    
    std::cout << "工作线程 ID: " << worker_id << "\n";
}

// =============================================================================
// 示例 3: 协程链
// =============================================================================

Task<int> compute_value(int x) {
    std::cout << "  计算 " << x << " * 2\n";
    co_return x * 2;
}

Task<int> chain_example() {
    std::cout << "  开始协程链\n";
    
    int a = co_await compute_value(5);
    int b = co_await compute_value(a);
    int c = co_await compute_value(b);
    
    std::cout << "  最终结果: " << c << "\n";
    co_return c;
}

void example3_chain() {
    std::cout << "\n=== 示例 3: 协程链 ===\n";
    
    auto future = async_run(chain_example());
    int result = future.get();
    
    std::cout << "返回值: " << result << "\n";
}

// =============================================================================
// 示例 4: 并发执行多个任务
// =============================================================================

Task<int> slow_task(int id, int duration_ms) {
    std::cout << "  任务 " << id << " 开始 [线程 " 
              << std::this_thread::get_id() << "]\n";
    
    std::this_thread::sleep_for(std::chrono::milliseconds(duration_ms));
    
    std::cout << "  任务 " << id << " 完成\n";
    co_return id * 10;
}

void example4_concurrent() {
    std::cout << "\n=== 示例 4: 并发执行 ===\n";
    
    // 启动多个任务
    auto future1 = async_run(slow_task(1, 100));
    auto future2 = async_run(slow_task(2, 150));
    auto future3 = async_run(slow_task(3, 80));
    
    // 等待所有任务完成
    int r1 = future1.get();
    int r2 = future2.get();
    int r3 = future3.get();
    
    std::cout << "结果: " << r1 << ", " << r2 << ", " << r3 << "\n";
    std::cout << "总和: " << (r1 + r2 + r3) << "\n";
}

// =============================================================================
// 示例 5: 异常处理
// =============================================================================

Task<int> may_throw(bool should_throw) {
    if (should_throw) {
        throw std::runtime_error("协程中的错误");
    }
    co_return 100;
}

void example5_exception() {
    std::cout << "\n=== 示例 5: 异常处理 ===\n";
    
    // 正常情况
    try {
        auto future1 = async_run(may_throw(false));
        int result = future1.get();
        std::cout << "  正常结果: " << result << "\n";
    } catch (const std::exception& e) {
        std::cout << "  捕获异常: " << e.what() << "\n";
    }
    
    // 抛出异常
    try {
        auto future2 = async_run(may_throw(true));
        int result = future2.get();
        std::cout << "  结果: " << result << "\n";
    } catch (const std::exception& e) {
        std::cout << "  ✓ 成功捕获异常: " << e.what() << "\n";
    }
}

// =============================================================================
// 示例 6: 协程中使用循环计算
// =============================================================================

Task<int> compute_sum(int n) {
    int sum = 0;
    for (int i = 0; i <= n; ++i) {
        sum += i;
    }
    co_return sum;
}

void example6_loop() {
    std::cout << "\n=== 示例 6: 循环计算 ===\n";
    
    auto future = async_run(compute_sum(100));
    int result = future.get();
    
    std::cout << "sum(0..100) = " << result << "\n";
    std::cout << "期望值: " << (100 * 101 / 2) << "\n";
}

// =============================================================================
// 示例 7: 生产者-消费者模式
// =============================================================================

Task<int> produce_value(int id) {
    std::cout << "  [生产] 生产值: " << id * 100 << "\n";
    co_return id * 100;
}

Task<void> consume_value(int id) {
    std::cout << "  [消费 " << id << "] 开始\n";
    auto prod = produce_value(id);
    int data = co_await prod;
    std::cout << "  [消费 " << id << "] 处理数据: " << data << "\n";
}

void example7_producer_consumer() {
    std::cout << "\n=== 示例 7: 简单的生产-消费 ===\n";
    
    // 异步执行消费任务
    auto f1 = async_run(consume_value(1));
    auto f2 = async_run(consume_value(2));
    
    f1.get();
    f2.get();
    
    std::cout << "  所有任务完成\n";
}

// =============================================================================
// 主函数
// =============================================================================

int main() {
    std::cout << "==============================================\n";
    std::cout << "      调度器使用示例\n";
    std::cout << "==============================================\n";
    std::cout << "CPU 核心数: " << std::thread::hardware_concurrency() << "\n";
    std::cout << "调度器线程数: " << Scheduler::instance().thread_count() << "\n";

    example1_basic();
    example2_thread_info();
    example3_chain();
    example4_concurrent();
    example5_exception();
    example6_loop();
    example7_producer_consumer();

    std::cout << "\n==============================================\n";
    std::cout << "所有示例运行完成!\n";
    std::cout << "==============================================\n";

    // 等待一下，确保所有后台任务完成
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    return 0;
}
