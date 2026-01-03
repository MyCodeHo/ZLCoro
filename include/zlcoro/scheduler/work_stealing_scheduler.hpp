#pragma once

#include <atomic>
#include <thread>
#include <vector>
#include <deque>
#include <mutex>
#include <condition_variable>
#include <coroutine>
#include <functional>
#include <optional>
#include <random>
#include <memory>
#include <algorithm>

namespace zlcoro {

// =============================================================================
// WorkStealingScheduler - 工作窃取调度器
// =============================================================================
// 
// 高性能调度器实现：
// - 每个线程有本地任务队列（带锁保护）
// - 空闲时从其他线程窃取任务
// - 减少全局锁竞争，提高缓存局部性
// =============================================================================

class WorkStealingScheduler {
public:
    using Task = std::function<void()>;

    explicit WorkStealingScheduler(size_t num_threads = std::thread::hardware_concurrency())
        : num_threads_(num_threads > 0 ? num_threads : 1)
        , stop_(false) {
        
        // 创建每个线程的本地队列
        local_queues_.reserve(num_threads_);
        for (size_t i = 0; i < num_threads_; ++i) {
            local_queues_.push_back(std::make_unique<LocalQueue>());
        }

        // 创建工作线程
        workers_.reserve(num_threads_);
        for (size_t i = 0; i < num_threads_; ++i) {
            workers_.emplace_back(&WorkStealingScheduler::worker_thread, this, i);
        }
    }

    ~WorkStealingScheduler() {
        shutdown();
    }

    // 禁止拷贝
    WorkStealingScheduler(const WorkStealingScheduler&) = delete;
    WorkStealingScheduler& operator=(const WorkStealingScheduler&) = delete;

    // 获取单例
    static WorkStealingScheduler& instance() {
        static WorkStealingScheduler scheduler;
        return scheduler;
    }

    // 提交任务（分配到指定队列，轮询方式）
    void submit(Task task) {
        if (stop_.load(std::memory_order_relaxed)) return;

        // 轮询选择目标队列
        size_t target = task_counter_.fetch_add(1, std::memory_order_relaxed) % num_threads_;
        
        {
            std::lock_guard<std::mutex> lock(local_queues_[target]->mutex);
            local_queues_[target]->tasks.push_back(std::move(task));
        }

        // 唤醒等待的线程
        cv_.notify_one();
    }

    // 调度协程
    void schedule(std::coroutine_handle<> coro) {
        if (!coro || coro.done()) return;
        
        submit([coro]() mutable {
            if (!coro.done()) {
                coro.resume();
            }
        });
    }

    // 调度可调用对象
    template <typename Func>
    void schedule(Func&& func) {
        submit(std::forward<Func>(func));
    }

    size_t thread_count() const noexcept {
        return num_threads_;
    }

    // 获取总待处理任务数（近似值）
    size_t pending_tasks() const {
        size_t total = 0;
        for (const auto& q : local_queues_) {
            std::lock_guard<std::mutex> lock(q->mutex);
            total += q->tasks.size();
        }
        return total;
    }

    void shutdown() {
        {
            std::lock_guard<std::mutex> lock(global_mutex_);
            if (stop_.load()) return;
            stop_.store(true, std::memory_order_release);
        }

        cv_.notify_all();

        for (auto& worker : workers_) {
            if (worker.joinable()) {
                worker.join();
            }
        }
        workers_.clear();
    }

private:
    // =========================================================================
    // 内部类型
    // =========================================================================
    
    /// @brief 线程本地任务队列
    /// @details 每个工作线程拥有独立的本地队列，减少锁竞争。
    ///          本地操作使用后进先出（LIFO，缓存友好），
    ///          窃取操作使用先进先出（FIFO，公平性）。
    struct LocalQueue {
        mutable std::mutex mutex;  ///< 保护任务队列的互斥锁
        std::deque<Task> tasks;    ///< 任务队列（双端，支持 LIFO 和 FIFO）
    };

    // 从本地队列取任务（后进先出）
    std::optional<Task> pop_local(size_t thread_id) {
        std::lock_guard<std::mutex> lock(local_queues_[thread_id]->mutex);
        if (local_queues_[thread_id]->tasks.empty()) {
            return std::nullopt;
        }
        Task task = std::move(local_queues_[thread_id]->tasks.back());
        local_queues_[thread_id]->tasks.pop_back();
        return task;
    }

    // 从其他队列窃取任务（先进先出，从队首取）
    std::optional<Task> steal_from(size_t victim_id) {
        std::lock_guard<std::mutex> lock(local_queues_[victim_id]->mutex);
        if (local_queues_[victim_id]->tasks.empty()) {
            return std::nullopt;
        }
        Task task = std::move(local_queues_[victim_id]->tasks.front());
        local_queues_[victim_id]->tasks.pop_front();
        return task;
    }

    void worker_thread(size_t thread_id) {
        // 线程本地随机数生成器用于选择窃取目标
        std::mt19937 rng(static_cast<unsigned int>(thread_id + 
            std::chrono::steady_clock::now().time_since_epoch().count()));

        // 预先构建其他线程的索引列表（避免每次循环都构建）
        std::vector<size_t> other_threads;
        other_threads.reserve(num_threads_ - 1);
        for (size_t i = 0; i < num_threads_; ++i) {
            if (i != thread_id) {
                other_threads.push_back(i);
            }
        }

        size_t steal_attempts = 0;  // 连续窃取失败次数

        while (true) {
            std::optional<Task> task;

            // 1. 先从本地队列获取
            task = pop_local(thread_id);
            
            if (!task && !other_threads.empty()) {
                // 2. 本地队列空，尝试从其他线程窃取
                // 只在连续失败多次后才重新打乱顺序（减少 shuffle 开销）
                if (steal_attempts % 8 == 0) {
                    std::shuffle(other_threads.begin(), other_threads.end(), rng);
                }
                
                for (size_t victim : other_threads) {
                    task = steal_from(victim);
                    if (task) {
                        steal_attempts = 0;  // 成功窃取，重置计数
                        break;
                    }
                }
                
                if (!task) {
                    ++steal_attempts;
                }
            }

            if (task) {
                // 执行任务
                try {
                    (*task)();
                } catch (...) {
                    // 忽略异常
                }
            } else {
                // 没有任务，等待
                std::unique_lock<std::mutex> lock(global_mutex_);
                
                // 检查是否应该退出
                if (stop_.load(std::memory_order_acquire)) {
                    // 退出前检查队列是否还有任务
                    bool has_work = false;
                    for (const auto& q : local_queues_) {
                        std::lock_guard<std::mutex> qlock(q->mutex);
                        if (!q->tasks.empty()) {
                            has_work = true;
                            break;
                        }
                    }
                    if (!has_work) {
                        return;  // 退出
                    }
                    continue;  // 还有任务，继续处理
                }

                // 等待新任务，增加等待时间避免 CPU 空转
                cv_.wait_for(lock, std::chrono::milliseconds(10));
            }
        }
    }

private:
    // =========================================================================
    // 数据成员
    // =========================================================================
    
    /// @brief 工作线程数量
    /// @details 在构造时确定，通常等于 CPU 核心数。
    ///          决定了 local_queues_ 的大小和 workers_ 的数量。
    size_t num_threads_;
    
    /// @brief 工作线程数组
    /// @details 每个线程运行 worker_thread() 函数：
    ///          1. 优先从本地队列取任务（LIFO，缓存友好）
    ///          2. 本地为空时，随机窃取其他线程的任务（FIFO，公平）
    ///          3. 都为空时，条件变量等待
    /// @lifetime 构造时创建，shutdown() 时 join
    std::vector<std::thread> workers_;
    
    /// @brief 线程本地任务队列数组
    /// @details 每个线程对应一个独立的 LocalQueue。
    ///          使用 unique_ptr 存储以保证指针稳定性（避免 vector 扩容导致失效）。
    ///          索引与 thread_id 一一对应。
    std::vector<std::unique_ptr<LocalQueue>> local_queues_;
    
    /// @brief 停止标志（原子）
    /// @details 设置为 true 后，工作线程在处理完剩余任务后退出。
    std::atomic<bool> stop_;
    
    /// @brief 任务计数器（原子，用于轮询分配）
    /// @details submit() 时递增，取模后得到目标队列索引。
    ///          这种轮询策略实现简单的负载均衡。
    std::atomic<size_t> task_counter_{0};
    
    /// @brief 全局互斥锁
    /// @details 用于保护 shutdown 操作和条件变量等待。
    ///          注意：任务队列使用各自的 LocalQueue::mutex。
    std::mutex global_mutex_;
    
    /// @brief 条件变量（线程等待/唤醒）
    /// @details 当队列为空时，工作线程 wait_for() 等待。
    ///          submit() 后 notify_one()，shutdown() 后 notify_all()。
    std::condition_variable cv_;
};

} // namespace zlcoro
