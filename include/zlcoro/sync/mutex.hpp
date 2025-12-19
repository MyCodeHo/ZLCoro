#pragma once

#include "zlcoro/scheduler/scheduler.hpp"
#include <coroutine>
#include <atomic>
#include <mutex>
#include <queue>

namespace zlcoro {

// =============================================================================
// Mutex - 协程互斥锁
// =============================================================================
//
// 用于保护协程之间的共享资源，与 std::mutex 类似但专为协程设计
// 
// 特性:
// - 协程友好（使用 co_await 而不是阻塞）
// - 公平调度（先到先得）
// - 自动解锁（RAII）
//
// 使用示例:
//   Mutex mtx;
//   auto lock = co_await mtx.lock();
//   // 临界区
//   // lock 析构时自动解锁
// =============================================================================

class Mutex {
public:
    Mutex() : locked_(false) {}

    ~Mutex() = default;

    // 禁止拷贝和移动
    Mutex(const Mutex&) = delete;
    Mutex& operator=(const Mutex&) = delete;
    Mutex(Mutex&&) = delete;
    Mutex& operator=(Mutex&&) = delete;

    // RAII 锁守卫
    class LockGuard {
    public:
        explicit LockGuard(Mutex* mtx) : mutex_(mtx) {}

        ~LockGuard() {
            if (mutex_) {
                mutex_->unlock();
            }
        }

        // 禁止拷贝
        LockGuard(const LockGuard&) = delete;
        LockGuard& operator=(const LockGuard&) = delete;

        // 支持移动
        LockGuard(LockGuard&& other) noexcept
            : mutex_(std::exchange(other.mutex_, nullptr)) {}

        LockGuard& operator=(LockGuard&& other) noexcept {
            if (this != &other) {
                if (mutex_) {
                    mutex_->unlock();
                }
                mutex_ = std::exchange(other.mutex_, nullptr);
            }
            return *this;
        }

    private:
        Mutex* mutex_;
    };

    // Lock Awaiter
    struct LockAwaiter {
        Mutex* mutex_;

        explicit LockAwaiter(Mutex* mtx) : mutex_(mtx) {}

        bool await_ready() const noexcept {
            // 快速路径：如果锁明显被持有，直接返回 false
            // 注意：这只是优化，真正的获取逻辑在 await_suspend 中
            return false;
        }

        bool await_suspend(std::coroutine_handle<> handle) {
            std::lock_guard lock(mutex_->queue_mutex_);
            
            // 在持有 queue_mutex_ 的情况下尝试获取锁
            // 这确保了 await_ready 和 await_suspend 之间没有竞态
            bool expected = false;
            if (mutex_->locked_.compare_exchange_strong(expected, true)) {
                // 成功获取锁，不需要挂起
                return false;
            }
            
            // 获取失败，加入等待队列
            mutex_->waiters_.push(handle);
            return true;  // 挂起
        }

        LockGuard await_resume() {
            return LockGuard(mutex_);
        }
    };

    // 获取锁
    LockAwaiter lock() {
        return LockAwaiter(this);
    }

    // 尝试获取锁（非阻塞）
    bool try_lock() {
        bool expected = false;
        return locked_.compare_exchange_strong(expected, true);
    }

    // 释放锁
    void unlock() {
        std::coroutine_handle<> to_resume;
        
        {
            std::lock_guard lock(queue_mutex_);
            
            // 如果有等待者，唤醒第一个
            if (!waiters_.empty()) {
                to_resume = waiters_.front();
                waiters_.pop();
                // 锁继续被持有，由下一个等待者持有
            } else {
                // 没有等待者，释放锁
                locked_.store(false);
            }
        }
        
        // 在锁外使用调度器恢复协程
        if (to_resume && !to_resume.done()) {
            Scheduler::instance().schedule(to_resume);
        }
    }

private:
    std::atomic<bool> locked_;
    std::queue<std::coroutine_handle<>> waiters_;
    std::mutex queue_mutex_;  // 保护等待队列
};

}  // namespace zlcoro
