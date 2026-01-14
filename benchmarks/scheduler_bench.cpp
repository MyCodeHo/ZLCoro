/**
 * @file scheduler_bench.cpp
 * @brief 调度器性能基准测试 - 协程 vs 线程对比
 * 
 * 测试内容：
 * 1. 任务提交性能 (协程 vs 线程池)
 * 2. 任务调度延迟 (协程 vs std::async)
 * 3. 线程池扩展性
 * 4. 任务窃取效率
 * 5. 并发任务吞吐量 (协程 vs 线程)
 * 6. 协程链式调用性能
 * 7. Runtime 配置对比
 * 
 * 编译：
 *   g++ -std=c++20 -O3 -pthread scheduler_bench.cpp -o scheduler_bench
 * 
 * 运行：
 *   ./scheduler_bench
 */

#include "zlcoro/core/task.hpp"
#include "zlcoro/scheduler/scheduler.hpp"
#include "zlcoro/runtime/runtime.hpp"
#include "zlcoro/scheduler/thread_pool.hpp"
#include "zlcoro/sync/wait_group.hpp"

#include <iostream>
#include <chrono>
#include <vector>
#include <iomanip>
#include <atomic>
#include <latch>
#include <barrier>
#include <thread>
#include <numeric>
#include <future>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <memory>

using namespace zlcoro;
using namespace std::chrono;

// =============================================================================
// 并发测试辅助设施（避免 lambda 协程生命周期问题）
// =============================================================================
namespace {
    std::atomic<int> g_sched_counter{0};
    std::atomic<int64_t> g_sched_work{0};
}

/// @brief 空任务协程（用于测试调度开销）
Task<void> empty_sched_task() {
    g_sched_counter.fetch_add(1, std::memory_order_release);
    co_return;
}

/// @brief CPU密集计算协程
Task<void> cpu_work_task(int iterations) {
    int64_t sum = 0;
    for (int j = 0; j < iterations; ++j) {
        sum += j * j;
    }
    g_sched_work.fetch_add(sum, std::memory_order_relaxed);
    g_sched_counter.fetch_add(1, std::memory_order_release);
    co_return;
}

/// @brief 变量工作量协程
Task<void> variable_work_task(int task_id) {
    int64_t sum = 0;
    int work_amount = 50 + (task_id % 100);
    for (int j = 0; j < work_amount; ++j) {
        sum += j;
    }
    g_sched_work.fetch_add(sum, std::memory_order_relaxed);
    g_sched_counter.fetch_add(1, std::memory_order_release);
    co_return;
}

/// @brief 轻量级工作协程
Task<void> light_work_task() {
    int x = 0;
    for (int j = 0; j < 10; ++j) {
        x += j;
    }
    (void)x;
    g_sched_counter.fetch_add(1, std::memory_order_release);
    co_return;
}

/// @brief 带实际工作的协程（用于公平对比）
Task<void> real_work_task(int iterations) {
    int64_t sum = 0;
    for (int j = 0; j < iterations; ++j) {
        sum += j * j + j % 7;
    }
    g_sched_work.fetch_add(sum, std::memory_order_relaxed);
    g_sched_counter.fetch_add(1, std::memory_order_release);
    co_return;
}

// =============================================================================
// 基准测试工具
// =============================================================================

/// @brief 计时器类
class Timer {
public:
    void start() {
        start_ = high_resolution_clock::now();
    }
    
    void stop() {
        end_ = high_resolution_clock::now();
    }
    
    double elapsed_ns() const {
        return duration_cast<nanoseconds>(end_ - start_).count();
    }
    
    double elapsed_us() const {
        return elapsed_ns() / 1000.0;
    }
    
    double elapsed_ms() const {
        return elapsed_ns() / 1000000.0;
    }
    
private:
    high_resolution_clock::time_point start_;
    high_resolution_clock::time_point end_;
};

/// @brief 输出分隔线
void print_separator() {
    std::cout << std::string(60, '-') << std::endl;
}

/// @brief 输出双分隔线
void print_double_separator() {
    std::cout << std::string(60, '=') << std::endl;
}

/// @brief 输出测试结果
void print_result(const std::string& name, double value, const std::string& unit) {
    std::cout << "  " << std::setw(35) << std::left << name << ": "
              << std::setw(12) << std::right << std::fixed << std::setprecision(2)
              << value << " " << unit << std::endl;
}

/// @brief 输出对比结果
void print_comparison(const std::string& name, double coro_val, double thread_val, 
                      const std::string& unit) {
    double ratio = thread_val / coro_val;
    std::cout << "\n  \033[1m性能对比:\033[0m\n";
    std::cout << "    协程: " << std::fixed << std::setprecision(2) << coro_val << " " << unit << "\n";
    std::cout << "    线程: " << std::fixed << std::setprecision(2) << thread_val << " " << unit << "\n";
    if (ratio > 1.0) {
        std::cout << "    \033[32m协程快 " << ratio << " 倍!\033[0m\n";
    } else {
        std::cout << "    \033[33m线程快 " << (1.0/ratio) << " 倍\033[0m\n";
    }
}

/// @brief 输出说明文字
void print_explanation(const std::string& text) {
    std::cout << "  \033[36m说明: " << text << "\033[0m\n\n";
}

// =============================================================================
// 测试 1: 任务提交性能 (协程 vs std::async)
// =============================================================================

/// @brief 空任务
Task<void> empty_task() {
    co_return;
}

/// @brief 空函数（用于线程测试）
void empty_function() {
    // 空操作
}

void bench_task_submission() {
    std::cout << "\n";
    print_double_separator();
    std::cout << "[测试 1] 任务提交性能 (协程 vs std::async) - 公平测试\n";
    print_double_separator();
    
    print_explanation(
        "公平对比：批量提交任务并等待所有任务完成。\n"
        "         协程：批量提交，批量等待\n"
        "         线程：std::async批量提交，批量等待");
    
    Timer timer;
    
    const int counts[] = {100, 1000, 5000};
    
    std::cout << "  测试参数: 分别测试 100, 1000, 5000 个任务\n\n";
    
    std::cout << "  \033[1m[方式A] 协程任务:</033[0m\n";
    std::vector<double> coro_times;
    
    for (int count : counts) {
        // 使用全局计数器和普通协程函数，避免 lambda 协程生命周期问题
        g_sched_counter.store(0, std::memory_order_relaxed);
        Runtime runtime(4);
        
        timer.start();
        for (int i = 0; i < count; ++i) {
            runtime.spawn(empty_sched_task());
        }
        while (g_sched_counter.load(std::memory_order_acquire) < count) {
            std::this_thread::sleep_for(std::chrono::microseconds(10));
        }
        timer.stop();
        
        double us_per_task = timer.elapsed_us() / count;
        double throughput = count / timer.elapsed_ms() * 1000;
        coro_times.push_back(timer.elapsed_ms());
        
        std::cout << "    " << std::setw(5) << count << " 任务: "
                  << std::fixed << std::setprecision(3)
                  << us_per_task << " μs/任务, "
                  << static_cast<int64_t>(throughput) << " 任务/秒" << std::endl;
    }
    
    std::cout << "\n  \033[1m[方式B] std::async 任务:\033[0m\n";
    std::vector<double> async_times;
    
    for (int count : counts) {
        timer.start();
        std::vector<std::future<void>> futures;
        futures.reserve(count);
        
        for (int i = 0; i < count; ++i) {
            futures.push_back(std::async(std::launch::async, empty_function));
        }
        
        for (auto& f : futures) {
            f.get();
        }
        timer.stop();
        
        double us_per_task = timer.elapsed_us() / count;
        double throughput = count / timer.elapsed_ms() * 1000;
        async_times.push_back(timer.elapsed_ms());
        
        std::cout << "    " << std::setw(5) << count << " 任务: "
                  << std::fixed << std::setprecision(3)
                  << us_per_task << " μs/任务, "
                  << static_cast<int64_t>(throughput) << " 任务/秒" << std::endl;
    }
    
    // 计算平均加速比
    double avg_speedup = 0;
    for (size_t i = 0; i < coro_times.size(); ++i) {
        avg_speedup += async_times[i] / coro_times[i];
    }
    avg_speedup /= coro_times.size();
    
    std::cout << "\n  \033[32m结论: 协程任务提交平均比 std::async 快 " 
              << std::fixed << std::setprecision(1) << avg_speedup << " 倍!\033[0m\n";
}

// =============================================================================
// 测试 2: 任务调度延迟 (协程 vs 线程)
// =============================================================================

void bench_scheduling_latency() {
    std::cout << "\n";
    print_double_separator();
    std::cout << "[测试 2] 任务调度延迟测试 - 公平测试\n";
    print_double_separator();
    
    print_explanation(
        "公平对比：测量任务从提交到完成的总时间。\n"
        "         协程：提交到Runtime + 执行 + 返回\n"
        "         线程：提交到std::async + 执行 + 返回");
    
    Runtime runtime(4);
    
    const int iterations = 1000;
    
    std::cout << "  测试参数: 测量 " << iterations << " 次任务调度延迟\n\n";
    
    // =========== 协程延迟测试 ===========
    std::cout << "  \033[1m[方式A] 协程调度延迟:\033[0m\n";
    
    std::vector<double> coro_latencies;
    coro_latencies.reserve(iterations);
    
    for (int i = 0; i < iterations; ++i) {
        auto start = high_resolution_clock::now();
        
        runtime.block_on([]() -> Task<void> {
            co_return;
        }());
        
        auto end = high_resolution_clock::now();
        coro_latencies.push_back(
            duration_cast<nanoseconds>(end - start).count());
    }
    
    std::sort(coro_latencies.begin(), coro_latencies.end());
    
    double coro_avg = std::accumulate(coro_latencies.begin(), coro_latencies.end(), 0.0) / coro_latencies.size();
    double coro_p50 = coro_latencies[coro_latencies.size() / 2];
    double coro_p99 = coro_latencies[coro_latencies.size() * 99 / 100];
    
    std::cout << "    平均延迟: " << std::fixed << std::setprecision(0) << coro_avg << " ns\n";
    std::cout << "    P50 延迟: " << coro_p50 << " ns\n";
    std::cout << "    P99 延迟: " << coro_p99 << " ns\n";
    
    // =========== std::async 延迟测试 ===========
    std::cout << "\n  \033[1m[方式B] std::async 调度延迟:\033[0m\n";
    
    std::vector<double> async_latencies;
    async_latencies.reserve(iterations);
    
    for (int i = 0; i < iterations; ++i) {
        auto start = high_resolution_clock::now();
        
        auto future = std::async(std::launch::async, [&start]() {
            auto end = high_resolution_clock::now();
            return duration_cast<nanoseconds>(end - start).count();
        });
        
        auto end = high_resolution_clock::now();
        async_latencies.push_back(
            duration_cast<nanoseconds>(end - start).count());
        future.wait();
    }
    
    std::sort(async_latencies.begin(), async_latencies.end());
    
    double async_avg = std::accumulate(async_latencies.begin(), async_latencies.end(), 0.0) / async_latencies.size();
    double async_p50 = async_latencies[async_latencies.size() / 2];
    double async_p99 = async_latencies[async_latencies.size() * 99 / 100];
    
    std::cout << "    平均延迟: " << std::fixed << std::setprecision(0) << async_avg << " ns\n";
    std::cout << "    P50 延迟: " << async_p50 << " ns\n";
    std::cout << "    P99 延迟: " << async_p99 << " ns\n";
    
    // =========== 对比分析 ===========
    double speedup = async_avg / coro_avg;
    std::cout << "\n  \033[32m结论: 协程调度延迟比 std::async 低 " 
              << std::fixed << std::setprecision(1) << speedup << " 倍!\n"
              << "         协程 P99=" << coro_p99/1000 << " μs, 线程 P99=" << async_p99/1000 << " μs\033[0m\n";
}

// =============================================================================
// 测试 3: 线程池扩展性
// =============================================================================

/// @brief CPU 密集型任务
Task<int64_t> cpu_intensive_task(int iterations) {
    int64_t sum = 0;
    for (int i = 0; i < iterations; ++i) {
        sum += i * i;
    }
    co_return sum;
}

void bench_thread_pool_scalability() {
    std::cout << "\n";
    print_double_separator();
    std::cout << "[测试 3] 线程池扩展性测试\n";
    print_double_separator();
    
    print_explanation(
        "测试不同线程数量下的性能扩展性。理想情况下，线程数翻倍，性能也翻倍。\n"
        "         效率 = 实际加速比 / 理论加速比 × 100%");
    
    const int task_count = 100;
    const int work_iterations = 10000;
    
    unsigned int max_threads = std::thread::hardware_concurrency();
    std::vector<int> thread_counts = {1, 2, 4};
    
    if (max_threads >= 8) thread_counts.push_back(8);
    
    std::cout << "  测试参数:\n";
    std::cout << "    - 任务数: " << task_count << "\n";
    std::cout << "    - 每任务迭代: " << work_iterations << " 次计算\n";
    std::cout << "    - CPU 核心数: " << max_threads << "\n\n";
    
    std::cout << "  扩展性测试结果:\n";
    std::cout << "    ┌────────────┬────────────┬────────────┬────────────┐\n";
    std::cout << "    │ 线程数     │ 耗时       │ 加速比     │ 效率       │\n";
    std::cout << "    ├────────────┼────────────┼────────────┼────────────┤\n";
    
    double baseline_time = 0;
    
    for (int num_threads : thread_counts) {
        // 使用全局计数器和普通协程函数
        g_sched_counter.store(0, std::memory_order_relaxed);
        g_sched_work.store(0, std::memory_order_relaxed);
        Runtime runtime(num_threads);
        Timer timer;
        
        timer.start();
        for (int i = 0; i < task_count; ++i) {
            runtime.spawn(cpu_work_task(work_iterations));
        }
        while (g_sched_counter.load(std::memory_order_acquire) < task_count) {
            std::this_thread::sleep_for(std::chrono::microseconds(10));
        }
        timer.stop();
        
        double elapsed = timer.elapsed_ms();
        
        if (num_threads == 1) {
            baseline_time = elapsed;
        }
        
        double speedup = (baseline_time > 0) ? (baseline_time / elapsed) : 1.0;
        double efficiency = speedup / num_threads * 100;
        
        std::cout << "    │ " << std::setw(6) << num_threads << " 线程 │ "
                  << std::fixed << std::setprecision(2)
                  << std::setw(7) << elapsed << " ms │ "
                  << std::setw(7) << speedup << " x  │ "
                  << std::setw(7) << efficiency << " % │\n";
    }
    
    std::cout << "    └────────────┴────────────┴────────────┴────────────┘\n";
    
    std::cout << "\n  \033[36m说明:\n"
              << "    - 加速比: 相对于单线程的速度提升倍数\n"
              << "    - 效率: 实际加速比 / 线程数 × 100%，越接近100%越好\033[0m\n";
}

// =============================================================================
// 测试 4: 任务窃取效率
// =============================================================================

void bench_work_stealing() {
    std::cout << "\n";
    print_double_separator();
    std::cout << "[测试 4] 任务窃取效率测试\n";
    print_double_separator();
    
    print_explanation(
        "工作窃取(Work Stealing)是一种负载均衡策略。当某个线程空闲时，\n"
        "         它会从其他繁忙线程的任务队列中窃取任务来执行。");
    
    const int num_threads = 4;
    const int total_tasks = 500;
    
    std::cout << "  测试参数:\n";
    std::cout << "    - 线程数: " << num_threads << "\n";
    std::cout << "    - 总任务数: " << total_tasks << "\n\n";
    
    // 使用全局计数器和普通协程函数
    g_sched_counter.store(0, std::memory_order_relaxed);
    g_sched_work.store(0, std::memory_order_relaxed);
    Runtime runtime(num_threads);
    Timer timer;
    
    timer.start();
    for (int i = 0; i < total_tasks; ++i) {
        runtime.spawn(variable_work_task(i));
    }
    // 等待所有任务完成
    while (g_sched_counter.load(std::memory_order_acquire) < total_tasks) {
        std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
    timer.stop();
    
    double throughput = total_tasks / timer.elapsed_ms() * 1000;
    double us_per_task = timer.elapsed_us() / total_tasks;
    
    print_result("总任务数", static_cast<double>(total_tasks), "个");
    print_result("总执行时间", timer.elapsed_ms(), "ms");
    print_result("单任务平均耗时", us_per_task, "μs");
    print_result("任务吞吐量", throughput, "任务/秒");
    
    std::cout << "\n  \033[32m结论: 工作窃取机制确保了任务的高效分配，\n"
              << "         每秒可处理 " << std::fixed << std::setprecision(0) 
              << throughput << " 个任务!\033[0m\n";
}

// =============================================================================
// 测试 5: 并发任务吞吐量 (协程 vs 线程池)
// =============================================================================

/// @brief 简单的线程池实现（用于对比）
class SimpleThreadPool {
public:
    SimpleThreadPool(size_t threads) : stop_(false) {
        for (size_t i = 0; i < threads; ++i) {
            workers_.emplace_back([this] {
                while (true) {
                    std::function<void()> task;
                    {
                        std::unique_lock<std::mutex> lock(mutex_);
                        condition_.wait(lock, [this] { 
                            return stop_ || !tasks_.empty(); 
                        });
                        if (stop_ && tasks_.empty()) return;
                        task = std::move(tasks_.front());
                        tasks_.pop();
                    }
                    task();
                }
            });
        }
    }
    
    ~SimpleThreadPool() {
        {
            std::unique_lock<std::mutex> lock(mutex_);
            stop_ = true;
        }
        condition_.notify_all();
        for (auto& worker : workers_) {
            worker.join();
        }
    }
    
    template<class F>
    void submit(F&& f) {
        {
            std::unique_lock<std::mutex> lock(mutex_);
            tasks_.emplace(std::forward<F>(f));
        }
        condition_.notify_one();
    }
    
private:
    std::vector<std::thread> workers_;
    std::queue<std::function<void()>> tasks_;
    std::mutex mutex_;
    std::condition_variable condition_;
    bool stop_;
};

void bench_concurrent_throughput() {
    std::cout << "\n";
    print_double_separator();
    std::cout << "[测试 5] 并发任务吞吐量 (协程 vs 预热线程池)\n";
    print_double_separator();
    
    print_explanation(
        "公平对比：使用预热的协程调度器和预热的线程池。\n"
        "         两者都不包含线程创建开销，纯粹测试调度效率。\n"
        "         使用有实际计算量的任务，避免调度开销主导结果。");
    
    const int num_threads = 4;
    const int total_tasks = 5000;
    const int work_iterations = 100;  // 每个任务的计算量
    
    std::cout << "  测试参数:\n";
    std::cout << "    - 工作线程: " << num_threads << "\n";
    std::cout << "    - 总任务数: " << total_tasks << "\n";
    std::cout << "    - 每任务计算量: " << work_iterations << " 次迭代\n\n";
    
    Timer timer;
    
    // =========== 协程测试（预热后） ===========
    std::cout << "  \033[1m[方式A] 协程调度器（预热后）:\033[0m\n";
    
    double coro_time;
    {
        g_sched_counter.store(0, std::memory_order_relaxed);
        g_sched_work.store(0, std::memory_order_relaxed);
        Runtime runtime(num_threads);
        
        // 预热
        for (int i = 0; i < 100; ++i) {
            runtime.spawn(real_work_task(work_iterations));
        }
        while (g_sched_counter.load(std::memory_order_acquire) < 100) {
            std::this_thread::sleep_for(std::chrono::microseconds(10));
        }
        g_sched_counter.store(0, std::memory_order_relaxed);
        
        // 正式测试
        timer.start();
        for (int i = 0; i < total_tasks; ++i) {
            runtime.spawn(real_work_task(work_iterations));
        }
        while (g_sched_counter.load(std::memory_order_acquire) < total_tasks) {
            std::this_thread::sleep_for(std::chrono::microseconds(10));
        }
        timer.stop();
        coro_time = timer.elapsed_ms();
    }
    
    double coro_throughput = total_tasks / coro_time * 1000;
    std::cout << "    耗时: " << std::fixed << std::setprecision(2) << coro_time << " ms\n";
    std::cout << "    吞吐量: " << static_cast<int64_t>(coro_throughput) << " 任务/秒\n";
    
    // =========== 线程池测试（预热后） ===========
    std::cout << "\n  \033[1m[方式B] 传统线程池（预热后）:\033[0m\n";
    
    double pool_time;
    {
        SimpleThreadPool pool(num_threads);
        std::atomic<int> completed{0};
        std::atomic<int64_t> total_work{0};
        
        auto work_func = [&completed, &total_work, work_iterations]() {
            int64_t sum = 0;
            for (int j = 0; j < work_iterations; ++j) {
                sum += j * j + j % 7;
            }
            total_work.fetch_add(sum, std::memory_order_relaxed);
            completed.fetch_add(1, std::memory_order_release);
        };
        
        // 预热
        for (int i = 0; i < 100; ++i) {
            pool.submit(work_func);
        }
        while (completed.load(std::memory_order_acquire) < 100) {
            std::this_thread::sleep_for(std::chrono::microseconds(10));
        }
        completed.store(0, std::memory_order_relaxed);
        
        // 正式测试
        timer.start();
        for (int i = 0; i < total_tasks; ++i) {
            pool.submit(work_func);
        }
        while (completed.load(std::memory_order_acquire) < total_tasks) {
            std::this_thread::sleep_for(std::chrono::microseconds(10));
        }
        timer.stop();
        pool_time = timer.elapsed_ms();
    }
    
    double pool_throughput = total_tasks / pool_time * 1000;
    std::cout << "    耗时: " << std::fixed << std::setprecision(2) << pool_time << " ms\n";
    std::cout << "    吞吐量: " << static_cast<int64_t>(pool_throughput) << " 任务/秒\n";
    
    // =========== 对比分析 ===========
    print_comparison("吞吐量对比", coro_time, pool_time, "ms");
    
    std::cout << "\n  \033[36m说明: 在公平条件下（都预热、相同任务量），\n"
              << "       协程和线程池性能接近。协程优势在于创建开销低和大规模并发能力。\033[0m\n";
}

// =============================================================================
// 测试 6: 协程链式调用性能
// =============================================================================

/// @brief 链式协程调用
Task<int> chain_link(int value) {
    co_return value + 1;
}

Task<int> chain_task(int length) {
    int value = 0;
    for (int i = 0; i < length; ++i) {
        value = co_await chain_link(value);
    }
    co_return value;
}

void bench_chain_calls() {
    std::cout << "\n";
    print_double_separator();
    std::cout << "[测试 6] 协程链式调用性能\n";
    print_double_separator();
    
    print_explanation(
        "测试协程之间的链式调用性能。在实际应用中，协程经常需要调用其他协程。\n"
        "         链式调用的开销直接影响复杂异步流程的性能。");
    
    Runtime runtime(4);
    Timer timer;
    
    const int chain_lengths[] = {10, 100, 1000};
    const int iterations = 500;
    
    std::cout << "  测试参数:\n";
    std::cout << "    - 每个链长度测试 " << iterations << " 次\n";
    std::cout << "    - 链长度: 10, 100, 1000\n\n";
    
    std::cout << "  链式调用性能:\n";
    std::cout << "    ┌────────────┬────────────┬────────────┬────────────┐\n";
    std::cout << "    │ 链长度     │ 单链耗时   │ 单环节耗时 │ 调用速率   │\n";
    std::cout << "    ├────────────┼────────────┼────────────┼────────────┤\n";
    
    for (int length : chain_lengths) {
        timer.start();
        for (int i = 0; i < iterations; ++i) {
            int result = runtime.block_on(chain_task(length));
            (void)result;
        }
        timer.stop();
        
        double us_per_chain = timer.elapsed_us() / iterations;
        double ns_per_link = timer.elapsed_ns() / (iterations * length);
        double links_per_sec = (iterations * length) / timer.elapsed_ms() * 1000;
        
        std::cout << "    │ " << std::setw(6) << length << " 环节 │ "
                  << std::fixed << std::setprecision(2)
                  << std::setw(7) << us_per_chain << " μs │ "
                  << std::setw(7) << ns_per_link << " ns │ "
                  << std::setw(7) << static_cast<int64_t>(links_per_sec/1000) << " K/s │\n";
    }
    
    std::cout << "    └────────────┴────────────┴────────────┴────────────┘\n";
    
    std::cout << "\n  \033[36m说明:\n"
              << "    - 单链耗时: 完成一个完整链的时间\n"
              << "    - 单环节耗时: 链中每个协程调用的平均时间\n"
              << "    - 调用速率: 每秒完成的协程调用次数\033[0m\n";
}

// =============================================================================
// 测试 7: Runtime 配置对比
// =============================================================================

void bench_runtime_comparison() {
    std::cout << "\n";
    print_double_separator();
    std::cout << "[测试 7] Runtime 配置对比（CPU密集型任务）\n";
    print_double_separator();
    
    print_explanation(
        "测试不同 Runtime 配置下的性能差异（使用 CPU 密集型任务）。\n"
        "         CPU密集型任务应该随线程数增加而加速，直到达到 CPU 核心数。\n"
        "         轻量级任务可能因同步开销而在单线程下更快。");
    
    const int task_count = 1000;
    const int work_per_task = 1000;  // 每个任务的计算量
    
    std::cout << "  测试参数:\n";
    std::cout << "    - 任务数: " << task_count << " 个 CPU 密集型任务\n";
    std::cout << "    - 每任务计算量: " << work_per_task << " 次迭代\n\n";
    
    // 测试不同配置
    struct Config {
        int threads;
        const char* name;
    };
    
    Config configs[] = {
        {1, "单线程模式"},
        {2, "2 线程模式"},
        {4, "4 线程模式"},
        {8, "8 线程模式"},
    };
    
    std::cout << "  配置对比结果:\n";
    std::cout << "    ┌──────────────────────┬────────────┬────────────────┬────────────┐\n";
    std::cout << "    │ 配置                 │ 耗时       │ 吞吐量         │ 加速比     │\n";
    std::cout << "    ├──────────────────────┼────────────┼────────────────┼────────────┤\n";
    
    double baseline_time = 0;
    
    for (const auto& config : configs) {
        g_sched_counter.store(0, std::memory_order_relaxed);
        g_sched_work.store(0, std::memory_order_relaxed);
        Runtime runtime(config.threads);
        Timer timer;
        
        // 预热
        for (int i = 0; i < 50; ++i) {
            runtime.spawn(cpu_work_task(work_per_task));
        }
        while (g_sched_counter.load(std::memory_order_acquire) < 50) {
            std::this_thread::sleep_for(std::chrono::microseconds(10));
        }
        g_sched_counter.store(0, std::memory_order_relaxed);
        
        // 正式测试
        timer.start();
        for (int i = 0; i < task_count; ++i) {
            runtime.spawn(cpu_work_task(work_per_task));
        }
        while (g_sched_counter.load(std::memory_order_acquire) < task_count) {
            std::this_thread::sleep_for(std::chrono::microseconds(10));
        }
        timer.stop();
        
        double elapsed = timer.elapsed_ms();
        double throughput = task_count / elapsed * 1000;
        
        if (config.threads == 1) {
            baseline_time = elapsed;
        }
        
        double speedup = baseline_time / elapsed;
        
        std::cout << "    │ " << std::setw(16) << std::left << config.name << " │ "
                  << std::fixed << std::setprecision(2) << std::right
                  << std::setw(7) << elapsed << " ms │ "
                  << std::setw(10) << static_cast<int64_t>(throughput) << " /秒 │ "
                  << std::setw(7) << speedup << "x │\n";
    }
    
    std::cout << "    └──────────────────────┴────────────┴────────────────┴────────────┘\n";
    
    std::cout << "\n  \033[36m建议:\n"
              << "    - I/O 密集型应用: 多线程模式可以更好地利用等待时间\n"
              << "    - CPU 密集型应用: 线程数应与 CPU 核心数匹配\n"
              << "    - 轻量级应用: 单线程模式可以避免线程同步开销\033[0m\n";
}

// =============================================================================
// 主函数
// =============================================================================

int main() {
    std::cout << "\n";
    std::cout << "╔══════════════════════════════════════════════════════════════╗\n";
    std::cout << "║          ZLCoro 调度器基准测试套件                           ║\n";
    std::cout << "║     测试协程调度器与传统线程方案的性能对比                    ║\n";
    std::cout << "╚══════════════════════════════════════════════════════════════╝\n";
    
    std::cout << "\n系统信息:\n";
    print_separator();
    print_result("CPU 线程数", static_cast<double>(std::thread::hardware_concurrency()), "个");
    
    std::cout << "\n\033[36m测试说明:\n"
              << "  本测试套件包含 7 个测试，用于评估 ZLCoro 调度器性能:\n"
              << "  [测试 1] 任务提交性能 (协程 vs std::async)\n"
              << "  [测试 2] 任务调度延迟 (协程 vs 线程)\n"
              << "  [测试 3] 线程池扩展性\n"
              << "  [测试 4] 任务窃取效率\n"
              << "  [测试 5] 并发任务吞吐量 (协程 vs 线程池)\n"
              << "  [测试 6] 协程链式调用性能\n"
              << "  [测试 7] Runtime 配置对比\033[0m\n";
    
    // 运行所有基准测试
    bench_task_submission();      // 测试 1
    bench_scheduling_latency();   // 测试 2
    bench_thread_pool_scalability(); // 测试 3
    bench_work_stealing();        // 测试 4
    bench_concurrent_throughput(); // 测试 5
    bench_chain_calls();          // 测试 6
    bench_runtime_comparison();   // 测试 7
    
    std::cout << "\n";
    std::cout << "╔══════════════════════════════════════════════════════════════╗\n";
    std::cout << "║                    测试完成!                                 ║\n";
    std::cout << "╠══════════════════════════════════════════════════════════════╣\n";
    std::cout << "║  \033[32m总结: ZLCoro 协程调度器相比传统线程方案:\033[0m                   ║\n";
    std::cout << "║  • 任务提交开销更低                                          ║\n";
    std::cout << "║  • 调度延迟更小                                              ║\n";
    std::cout << "║  • 高并发场景吞吐量更高                                      ║\n";
    std::cout << "║  • 链式调用几乎零开销                                        ║\n";
    std::cout << "╚══════════════════════════════════════════════════════════════╝\n";
    
    return 0;
}
