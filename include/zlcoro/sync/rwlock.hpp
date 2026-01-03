#pragma once

#include <coroutine>
#include <atomic>
#include <mutex>
#include <queue>
#include <memory>
#include <optional>
#include <functional>

namespace zlcoro {

// 前向声明，避免循环依赖
class Scheduler;

// =============================================================================
// RWLock - 协程读写锁
// =============================================================================
// 
// 支持多个读者同时访问，但写者独占访问。
// 
// 特性:
// - 多读单写：允许多个协程同时读，写时独占
// - 协程友好：使用 co_await 而非阻塞
// - 公平调度：读写者按到达顺序排队
// - RAII 支持：自动释放锁
// - 可配置调度器：支持自定义恢复策略
// 
// 使用示例:
//   RWLock lock;
//   
//   // 读取
//   {
//       auto guard = co_await lock.read_lock();
//       // 可以安全读取共享数据
//   }
//   
//   // 写入
//   {
//       auto guard = co_await lock.write_lock();
//       // 可以安全修改共享数据
//   }
// =============================================================================

class RWLock {
public:
    // 协程恢复策略
    using ResumeCallback = std::function<void(std::coroutine_handle<>)>;
    
    // 默认恢复策略：直接恢复
    static void default_resume(std::coroutine_handle<> coro) {
        if (coro && !coro.done()) {
            coro.resume();
        }
    }

    explicit RWLock(ResumeCallback resume_fn = default_resume) 
        : readers_(0)
        , writer_(false) 
        , resume_fn_(std::move(resume_fn)) {}

    ~RWLock() = default;

    // 禁止拷贝和移动
    RWLock(const RWLock&) = delete;
    RWLock& operator=(const RWLock&) = delete;
    RWLock(RWLock&&) = delete;
    RWLock& operator=(RWLock&&) = delete;

    // =========================================================================
    // 锁守卫
    // =========================================================================

    // 读锁守卫
    class ReadGuard {
    public:
        explicit ReadGuard(RWLock* lock) : lock_(lock) {}

        ~ReadGuard() {
            if (lock_) {
                lock_->read_unlock();
            }
        }

        // 禁止拷贝
        ReadGuard(const ReadGuard&) = delete;
        ReadGuard& operator=(const ReadGuard&) = delete;

        // 支持移动
        ReadGuard(ReadGuard&& other) noexcept
            : lock_(std::exchange(other.lock_, nullptr)) {}

        ReadGuard& operator=(ReadGuard&& other) noexcept {
            if (this != &other) {
                if (lock_) {
                    lock_->read_unlock();
                }
                lock_ = std::exchange(other.lock_, nullptr);
            }
            return *this;
        }

    private:
        RWLock* lock_;
    };

    // 写锁守卫
    class WriteGuard {
    public:
        explicit WriteGuard(RWLock* lock) : lock_(lock) {}

        ~WriteGuard() {
            if (lock_) {
                lock_->write_unlock();
            }
        }

        // 禁止拷贝
        WriteGuard(const WriteGuard&) = delete;
        WriteGuard& operator=(const WriteGuard&) = delete;

        // 支持移动
        WriteGuard(WriteGuard&& other) noexcept
            : lock_(std::exchange(other.lock_, nullptr)) {}

        WriteGuard& operator=(WriteGuard&& other) noexcept {
            if (this != &other) {
                if (lock_) {
                    lock_->write_unlock();
                }
                lock_ = std::exchange(other.lock_, nullptr);
            }
            return *this;
        }

        // 降级为读锁
        ReadGuard downgrade() {
            if (!lock_) {
                throw std::runtime_error("Cannot downgrade released lock");
            }
            
            auto* lock = lock_;
            lock_ = nullptr;  // 防止析构时 write_unlock
            
            lock->downgrade_to_read();
            return ReadGuard(lock);
        }

    private:
        RWLock* lock_;
    };

    // =========================================================================
    // 等待者类型
    // =========================================================================
private:
    enum class WaiterType { Reader, Writer };
    
    struct Waiter {
        WaiterType type;
        std::coroutine_handle<> coro;
    };

public:
    // =========================================================================
    // Read Lock Awaiter
    // =========================================================================

    struct ReadLockAwaiter {
        RWLock* lock_;

        bool await_ready() const noexcept {
            return false;  // 总是检查 await_suspend
        }

        bool await_suspend(std::coroutine_handle<> coro) {
            std::lock_guard guard(lock_->mutex_);

            // 如果没有写者且没有等待的写者，可以直接获取读锁
            if (!lock_->writer_ && lock_->waiting_writers_ == 0) {
                ++lock_->readers_;
                return false;  // 不挂起
            }

            // 否则加入等待队列
            lock_->waiters_.push({WaiterType::Reader, coro});
            return true;  // 挂起
        }

        ReadGuard await_resume() {
            return ReadGuard(lock_);
        }
    };

    // =========================================================================
    // Write Lock Awaiter
    // =========================================================================

    struct WriteLockAwaiter {
        RWLock* lock_;

        bool await_ready() const noexcept {
            return false;
        }

        bool await_suspend(std::coroutine_handle<> coro) {
            std::lock_guard guard(lock_->mutex_);

            // 如果没有读者也没有写者，可以直接获取写锁
            if (lock_->readers_ == 0 && !lock_->writer_) {
                lock_->writer_ = true;
                return false;  // 不挂起
            }

            // 否则加入等待队列
            ++lock_->waiting_writers_;
            lock_->waiters_.push({WaiterType::Writer, coro});
            return true;  // 挂起
        }

        WriteGuard await_resume() {
            return WriteGuard(lock_);
        }
    };

    // =========================================================================
    // 公共接口
    // =========================================================================

    // 获取读锁
    ReadLockAwaiter read_lock() {
        return ReadLockAwaiter{this};
    }

    // 获取写锁
    WriteLockAwaiter write_lock() {
        return WriteLockAwaiter{this};
    }

    // 尝试获取读锁（非阻塞）
    std::optional<ReadGuard> try_read_lock() {
        std::lock_guard guard(mutex_);
        
        if (!writer_ && waiting_writers_ == 0) {
            ++readers_;
            return ReadGuard(this);
        }
        
        return std::nullopt;
    }

    // 尝试获取写锁（非阻塞）
    std::optional<WriteGuard> try_write_lock() {
        std::lock_guard guard(mutex_);
        
        if (readers_ == 0 && !writer_) {
            writer_ = true;
            return WriteGuard(this);
        }
        
        return std::nullopt;
    }

    // 获取当前读者数量
    size_t reader_count() const {
        std::lock_guard guard(mutex_);
        return readers_;
    }

    // 检查是否有写者
    bool is_write_locked() const {
        std::lock_guard guard(mutex_);
        return writer_;
    }

private:
    // 释放读锁
    void read_unlock() {
        std::vector<std::coroutine_handle<>> to_resume;
        
        {
            std::lock_guard guard(mutex_);
            --readers_;

            // 如果没有读者了，唤醒等待的写者
            if (readers_ == 0 && !waiters_.empty()) {
                wake_up_waiters(to_resume);
            }
        }

        // 在锁外恢复协程
        for (auto& coro : to_resume) {
            resume_fn_(coro);
        }
    }

    // 释放写锁
    void write_unlock() {
        std::vector<std::coroutine_handle<>> to_resume;
        
        {
            std::lock_guard guard(mutex_);
            writer_ = false;

            // 唤醒等待者
            if (!waiters_.empty()) {
                wake_up_waiters(to_resume);
            }
        }

        // 在锁外恢复协程
        for (auto& coro : to_resume) {
            resume_fn_(coro);
        }
    }

    // 从写锁降级为读锁
    void downgrade_to_read() {
        std::vector<std::coroutine_handle<>> to_resume;
        
        {
            std::lock_guard guard(mutex_);
            writer_ = false;
            ++readers_;  // 变成读者

            // 唤醒其他等待的读者
            while (!waiters_.empty()) {
                auto& waiter = waiters_.front();
                if (waiter.type == WaiterType::Reader) {
                    ++readers_;
                    to_resume.push_back(waiter.coro);
                    waiters_.pop();
                } else {
                    // 遇到写者就停止
                    break;
                }
            }
        }

        // 在锁外恢复协程
        for (auto& coro : to_resume) {
            resume_fn_(coro);
        }
    }

    // 唤醒等待者
    void wake_up_waiters(std::vector<std::coroutine_handle<>>& to_resume) {
        if (waiters_.empty()) return;

        auto& first = waiters_.front();
        
        if (first.type == WaiterType::Writer) {
            // 唤醒一个写者
            writer_ = true;
            --waiting_writers_;
            to_resume.push_back(first.coro);
            waiters_.pop();
        } else {
            // 唤醒所有连续的读者
            while (!waiters_.empty()) {
                auto& waiter = waiters_.front();
                if (waiter.type == WaiterType::Reader) {
                    ++readers_;
                    to_resume.push_back(waiter.coro);
                    waiters_.pop();
                } else {
                    // 遇到写者就停止
                    break;
                }
            }
        }
    }

private:
    // =========================================================================
    // 数据成员
    // =========================================================================
    
    /// @brief 互斥锁
    /// @details 保护所有其他数据成员的并发访问。
    ///          标记为 mutable 以便 const 方法也能加锁（如果有的话）。
    mutable std::mutex mutex_;
    
    /// @brief 当前读者数量
    /// @details 正在持有读锁的协程数量。
    ///          读锁可以被多个协程同时持有（共享）。
    ///          当 readers_ > 0 时，写者必须等待。
    /// @thread_safety 由 mutex_ 保护
    size_t readers_ = 0;
    
    /// @brief 写者标志
    /// @details true 表示当前有一个协程持有写锁。
    ///          写锁是独占的，同时只能有一个写者。
    ///          当 writer_ == true 时，新的读者和写者都必须等待。
    /// @thread_safety 由 mutex_ 保护
    bool writer_ = false;
    
    /// @brief 等待中的写者数量
    /// @details 用于实现写者优先策略。
    ///          当 waiting_writers_ > 0 时，新的读者需要等待，
    ///          避免写者饥饿（持续有读者导致写者永远拿不到锁）。
    /// @thread_safety 由 mutex_ 保护
    size_t waiting_writers_ = 0;
    
    /// @brief 等待者队列
    /// @details 存储等待获取锁的协程及其类型（读者/写者）。
    ///          按 FIFO 顺序唤醒，但会批量唤醒连续的读者。
    /// @thread_safety 由 mutex_ 保护
    std::queue<Waiter> waiters_;
    
    /// @brief 协程恢复回调函数
    /// @details 可自定义如何恢复协程（如提交到调度器）。
    ///          默认使用 Scheduler::instance().schedule()。
    ///          通过 set_resume_callback() 设置。
    ResumeCallback resume_fn_;
};

} // namespace zlcoro
