#pragma once

#include "zlcoro/scheduler/scheduler.hpp"
#include <coroutine>
#include <atomic>
#include <mutex>
#include <queue>

namespace zlcoro {

// =============================================================================
// Semaphore - 信号量
// =============================================================================
//
// 用于限制同时访问资源的协程数量
// 
// 特性:
// - 可配置的最大计数
// - 协程友好的获取/释放
// - 公平调度
// - 线程安全
//
// 使用示例:
//   Semaphore sem(3);  // 最多 3 个并发
//   
//   co_await sem.acquire();  // 获取许可
//   // 访问资源
//   sem.release();  // 释放许可
// =============================================================================

class Semaphore {
public:
    explicit Semaphore(int max_count) : count_(max_count), max_count_(max_count) {
        if (max_count <= 0) {
            throw std::invalid_argument("Semaphore max_count must be positive");
        }
    }

    ~Semaphore() = default;

    // 禁止拷贝和移动
    Semaphore(const Semaphore&) = delete;
    Semaphore& operator=(const Semaphore&) = delete;
    Semaphore(Semaphore&&) = delete;
    Semaphore& operator=(Semaphore&&) = delete;

    // RAII 守卫
    class Guard {
    public:
        explicit Guard(Semaphore* sem) : sem_(sem) {}

        ~Guard() {
            if (sem_) {
                sem_->release();
            }
        }

        // 禁止拷贝
        Guard(const Guard&) = delete;
        Guard& operator=(const Guard&) = delete;

        // 支持移动
        Guard(Guard&& other) noexcept
            : sem_(std::exchange(other.sem_, nullptr)) {}

        Guard& operator=(Guard&& other) noexcept {
            if (this != &other) {
                if (sem_) {
                    sem_->release();
                }
                sem_ = std::exchange(other.sem_, nullptr);
            }
            return *this;
        }

    private:
        Semaphore* sem_;
    };

    // Acquire Awaiter
    struct AcquireAwaiter {
        Semaphore* sem_;

        explicit AcquireAwaiter(Semaphore* sem) : sem_(sem) {}

        bool await_ready() const noexcept {
            // 快速路径优化，真正的获取逻辑在 await_suspend 中
            return false;
        }

        bool await_suspend(std::coroutine_handle<> handle) {
            std::lock_guard lock(sem_->mutex_);
            
            // 在持有锁的情况下尝试获取许可
            if (sem_->count_ > 0) {
                sem_->count_--;
                return false;  // 成功获取，不挂起
            }
            
            // 获取失败，加入等待队列
            sem_->waiters_.push(handle);
            return true;  // 挂起
        }

        void await_resume() {}
    };

    // Scoped Acquire Awaiter (返回 RAII 守卫)
    struct ScopedAcquireAwaiter {
        Semaphore* sem_;

        explicit ScopedAcquireAwaiter(Semaphore* sem) : sem_(sem) {}

        bool await_ready() const noexcept {
            // 快速路径优化
            return false;
        }

        bool await_suspend(std::coroutine_handle<> handle) {
            std::lock_guard lock(sem_->mutex_);
            
            if (sem_->count_ > 0) {
                sem_->count_--;
                return false;  // 成功获取，不挂起
            }
            
            sem_->waiters_.push(handle);
            return true;  // 挂起
        }

        Guard await_resume() {
            return Guard(sem_);
        }
    };

    // 获取许可
    AcquireAwaiter acquire() {
        return AcquireAwaiter(this);
    }

    // 获取许可（RAII）
    ScopedAcquireAwaiter scoped_acquire() {
        return ScopedAcquireAwaiter(this);
    }

    // 尝试获取许可（非阻塞）
    bool try_acquire() {
        std::lock_guard lock(mutex_);
        
        if (count_ > 0) {
            count_--;
            return true;
        }
        
        return false;
    }

    // 释放许可
    void release() {
        std::coroutine_handle<> to_resume;
        
        {
            std::lock_guard lock(mutex_);
            
            // 如果有等待者，找到第一个有效的等待者
            while (!waiters_.empty()) {
                to_resume = waiters_.front();
                waiters_.pop();
                
                // 检查协程句柄是否有效且未完成
                if (to_resume && !to_resume.done()) {
                    // 许可直接转移给等待者，不增加 count_
                    break;
                }
                // 协程无效或已完成，继续查找下一个
                to_resume = nullptr;
            }
            
            // 如果没有有效的等待者，增加计数
            if (!to_resume && count_ < max_count_) {
                count_++;
            }
        }
        
        // 在锁外使用调度器恢复协程
        if (to_resume) {
            Scheduler::instance().schedule(to_resume);
        }
    }

    // 获取当前可用许可数
    int available() const {
        std::lock_guard lock(mutex_);
        return count_;
    }

    // 获取最大许可数
    int max_count() const {
        return max_count_;
    }

private:
    // =========================================================================
    // 数据成员
    // =========================================================================
    
    /// @brief 当前可用许可数
    /// @details 表示可以立即获取的许可数量。
    ///          acquire() 会减少此计数，release() 会增加（不超过 max_count_）。
    ///          当 count_ 为 0 时，acquire() 的协程会挂起等待。
    /// @thread_safety 由 mutex_ 保护
    int count_;
    
    /// @brief 最大许可数
    /// @details 限制信号量的上界，防止 release() 调用次数超过 acquire()。
    ///          这是一个常量，在构造后不会改变。
    int max_count_;
    
    /// @brief 等待者队列
    /// @details 当 count_ 为 0 时，试图获取许可的协程会加入此队列。
    ///          release() 会从队首取出协程恢复执行（FIFO）。
    /// @thread_safety 由 mutex_ 保护
    std::queue<std::coroutine_handle<>> waiters_;
    
    /// @brief 互斥锁
    /// @details 保护 count_ 和 waiters_ 的并发访问。
    ///          标记为 mutable 以便 const 方法（如 available()）也能加锁。
    mutable std::mutex mutex_;
};

}  // namespace zlcoro
