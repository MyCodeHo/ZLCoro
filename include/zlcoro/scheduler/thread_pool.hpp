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

namespace zlcoro {

// =============================================================================
// ThreadPool - 线程池
// =============================================================================
// 
// 基础线程池实现，管理多个工作线程。
// 特性：
// - 固定数量的工作线程
// - 任务队列管理
// - 优雅关闭
// =============================================================================

class ThreadPool {
public:
    // 任务类型：无参数、无返回值的可调用对象
    using Task = std::function<void()>;

    // 构造函数：创建指定数量的工作线程
    explicit ThreadPool(size_t num_threads = std::thread::hardware_concurrency())
        : stop_(false) {
        
        if (num_threads == 0) {
            num_threads = 1;
        }

        // 创建工作线程
        workers_.reserve(num_threads);
        for (size_t i = 0; i < num_threads; ++i) {
            workers_.emplace_back(&ThreadPool::worker_thread, this, i);
        }
    }

    // 禁止拷贝
    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    // 析构函数：停止所有线程
    ~ThreadPool() {
        shutdown();
    }

    // 提交任务到线程池
    void submit(Task task) {
        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            if (stop_) {
                return;  // 已关闭，拒绝新任务
            }
            task_queue_.push_back(std::move(task));
        }
        // 唤醒一个等待的线程
        condition_.notify_one();
    }

    // 获取线程数量
    size_t thread_count() const noexcept {
        return workers_.size();
    }

    // 获取当前队列中的任务数量
    size_t pending_tasks() const {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        return task_queue_.size();
    }

    // 停止线程池
    void shutdown() {
        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            if (stop_) {
                return;  // 已经停止
            }
            stop_ = true;
        }
        
        // 唤醒所有线程
        condition_.notify_all();
        
        // 等待所有线程结束
        for (auto& worker : workers_) {
            if (worker.joinable()) {
                worker.join();
            }
        }
        
        workers_.clear();
    }

private:
    // 工作线程函数
    void worker_thread(size_t thread_id) {
        while (true) {
            std::optional<Task> task;
            
            {
                std::unique_lock<std::mutex> lock(queue_mutex_);
                
                // 等待任务或停止信号
                condition_.wait(lock, [this] {
                    return stop_ || !task_queue_.empty();
                });
                
                // 如果停止且队列为空，退出
                if (stop_ && task_queue_.empty()) {
                    return;
                }
                
                // 取出任务
                if (!task_queue_.empty()) {
                    task = std::move(task_queue_.front());
                    task_queue_.pop_front();
                }
            }
            
            // 执行任务（在锁外执行）
            if (task) {
                try {
                    (*task)();
                } catch (...) {
                    // 捕获并忽略异常，防止线程崩溃
                    // 实际应用中应该记录日志
                }
            }
        }
    }

private:
    // 工作线程
    std::vector<std::thread> workers_;
    
    // 任务队列
    std::deque<Task> task_queue_;
    
    // 互斥锁保护任务队列
    mutable std::mutex queue_mutex_;
    
    // 条件变量用于线程唤醒
    std::condition_variable condition_;
    
    // 停止标志
    std::atomic<bool> stop_;
};

} // namespace zlcoro
