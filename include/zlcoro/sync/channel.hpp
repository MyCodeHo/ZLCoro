#pragma once

#include "zlcoro/core/task.hpp"
#include "zlcoro/scheduler/scheduler.hpp"
#include <queue>
#include <mutex>
#include <optional>
#include <coroutine>
#include <stdexcept>
#include <atomic>
#include <memory>

namespace zlcoro {

// =============================================================================
// Channel - 协程间通信通道
// =============================================================================

template <typename T>
class Channel {
public:
    explicit Channel(size_t capacity = 0) : capacity_(capacity), closed_(false) {}
    
    ~Channel() {
        close();
    }

    Channel(const Channel&) = delete;
    Channel& operator=(const Channel&) = delete;
    Channel(Channel&&) = delete;
    Channel& operator=(Channel&&) = delete;

    struct SendAwaiter;
    struct ReceiveAwaiter;

private:
    // 使用 shared_ptr 管理等待者的数据，确保生命周期安全
    struct SendWaiter {
        std::coroutine_handle<> handle;
        std::shared_ptr<T> value;  // 使用 shared_ptr 存储值
        std::shared_ptr<bool> success;  // 标记发送是否成功
    };
    
    struct RecvWaiter {
        std::coroutine_handle<> handle;
        std::shared_ptr<std::optional<T>> result;  // 使用 shared_ptr 存储结果
    };

public:
    // Send Awaiter
    struct SendAwaiter {
        Channel* ch_;
        std::shared_ptr<T> value_;  // 使用 shared_ptr 确保生命周期
        std::shared_ptr<bool> success_;  // 标记发送是否成功

        SendAwaiter(Channel* ch, T val) 
            : ch_(ch), 
              value_(std::make_shared<T>(std::move(val))),
              success_(std::make_shared<bool>(false)) {}

        bool await_ready() { return false; }

        bool await_suspend(std::coroutine_handle<> handle) {
            std::unique_lock lock(ch_->mutex_);
            
            if (ch_->closed_) {
                return false;  // 不挂起，在 await_resume 中抛异常
            }

            // 1. 检查是否有接收者在等待
            if (!ch_->recv_waiters_.empty()) {
                RecvWaiter waiter = ch_->recv_waiters_.front();
                ch_->recv_waiters_.pop();
                *(waiter.result) = std::move(*value_);
                *success_ = true;  // 标记发送成功
                
                lock.unlock();
                if (waiter.handle && !waiter.handle.done()) {
                    Scheduler::instance().schedule(waiter.handle);
                }
                return false;  // 不挂起当前协程
            }

            // 2. 尝试放入缓冲区
            if (ch_->buffer_.size() < ch_->capacity_) {
                ch_->buffer_.push(std::move(*value_));
                *success_ = true;  // 标记发送成功
                return false;  // 不挂起
            }

            // 3. 挂起并加入等待队列（使用 shared_ptr，生命周期安全）
            ch_->send_waiters_.push({handle, value_, success_});
            return true;  // 挂起
        }

        void await_resume() {
            // 只有在发送失败（被 close 唤醒）时才抛异常
            if (!*success_ && ch_->closed_) {
                throw std::runtime_error("Channel is closed");
            }
        }
    };

    // Receive Awaiter
    struct ReceiveAwaiter {
        Channel* ch_;
        std::shared_ptr<std::optional<T>> result_;  // 使用 shared_ptr 确保生命周期

        explicit ReceiveAwaiter(Channel* ch) 
            : ch_(ch), result_(std::make_shared<std::optional<T>>()) {}

        bool await_ready() { return false; }

        bool await_suspend(std::coroutine_handle<> handle) {
            std::unique_lock lock(ch_->mutex_);

            // 1. 检查缓冲区
            if (!ch_->buffer_.empty()) {
                *result_ = std::move(ch_->buffer_.front());
                ch_->buffer_.pop();

                // 唤醒一个发送者
                if (!ch_->send_waiters_.empty()) {
                    SendWaiter waiter = ch_->send_waiters_.front();
                    ch_->send_waiters_.pop();
                    ch_->buffer_.push(std::move(*waiter.value));
                    *(waiter.success) = true;  // 标记发送成功
                    
                    lock.unlock();
                    if (waiter.handle && !waiter.handle.done()) {
                        Scheduler::instance().schedule(waiter.handle);
                    }
                }
                return false;  // 不挂起
            }

            // 2. 检查是否有发送者在等待
            if (!ch_->send_waiters_.empty()) {
                SendWaiter waiter = ch_->send_waiters_.front();
                ch_->send_waiters_.pop();
                *result_ = std::move(*waiter.value);
                *(waiter.success) = true;  // 标记发送成功
                
                lock.unlock();
                if (waiter.handle && !waiter.handle.done()) {
                    Scheduler::instance().schedule(waiter.handle);
                }
                return false;  // 不挂起
            }

            // 3. 如果通道已关闭
            if (ch_->closed_) {
                *result_ = std::nullopt;
                return false;  // 不挂起，返回 nullopt
            }

            // 4. 挂起并加入等待队列（使用 shared_ptr，生命周期安全）
            ch_->recv_waiters_.push({handle, result_});
            return true;  // 挂起
        }

        std::optional<T> await_resume() {
            return std::move(*result_);
        }
    };

    SendAwaiter send(T value) {
        return SendAwaiter(this, std::move(value));
    }

    ReceiveAwaiter receive() {
        return ReceiveAwaiter(this);
    }

    bool try_send(T value) {
        std::coroutine_handle<> to_resume;
        bool success = false;
        
        {
            std::lock_guard lock(mutex_);
            
            if (closed_) {
                return false;
            }

            if (!recv_waiters_.empty()) {
                RecvWaiter waiter = recv_waiters_.front();
                recv_waiters_.pop();
                *(waiter.result) = std::move(value);
                to_resume = waiter.handle;
                success = true;
            } else if (buffer_.size() < capacity_) {
                buffer_.push(std::move(value));
                success = true;
            }
        }
        
        if (to_resume && !to_resume.done()) {
            Scheduler::instance().schedule(to_resume);
        }
        return success;
    }

    std::optional<T> try_receive() {
        std::coroutine_handle<> to_resume;
        std::optional<T> result;
        std::shared_ptr<bool> sender_success;
        
        {
            std::lock_guard lock(mutex_);

            if (!buffer_.empty()) {
                result = std::move(buffer_.front());
                buffer_.pop();

                if (!send_waiters_.empty()) {
                    SendWaiter waiter = send_waiters_.front();
                    send_waiters_.pop();
                    buffer_.push(std::move(*waiter.value));
                    *(waiter.success) = true;  // 标记发送成功
                    to_resume = waiter.handle;
                }
            } else if (!send_waiters_.empty()) {
                SendWaiter waiter = send_waiters_.front();
                send_waiters_.pop();
                result = std::move(*waiter.value);
                *(waiter.success) = true;  // 标记发送成功
                to_resume = waiter.handle;
            }
        }
        
        if (to_resume && !to_resume.done()) {
            Scheduler::instance().schedule(to_resume);
        }
        return result;
    }

    void close() {
        std::queue<RecvWaiter> recv_waiters_copy;
        std::queue<SendWaiter> send_waiters_copy;
        
        {
            std::lock_guard lock(mutex_);
            
            if (closed_) {
                return;
            }
            
            closed_ = true;
            
            // 复制等待者队列
            recv_waiters_copy = std::move(recv_waiters_);
            send_waiters_copy = std::move(send_waiters_);
            
            // 清空原队列
            while (!recv_waiters_.empty()) recv_waiters_.pop();
            while (!send_waiters_.empty()) send_waiters_.pop();
        }
        
        // 唤醒所有等待接收者，返回 nullopt
        while (!recv_waiters_copy.empty()) {
            auto waiter = recv_waiters_copy.front();
            recv_waiters_copy.pop();
            *(waiter.result) = std::nullopt;
            if (waiter.handle && !waiter.handle.done()) {
                Scheduler::instance().schedule(waiter.handle);
            }
        }
        
        // 唤醒所有等待发送者（它们会在 await_resume 中抛出异常）
        while (!send_waiters_copy.empty()) {
            auto waiter = send_waiters_copy.front();
            send_waiters_copy.pop();
            if (waiter.handle && !waiter.handle.done()) {
                Scheduler::instance().schedule(waiter.handle);
            }
        }
    }

    bool is_closed() const {
        return closed_.load();
    }

    size_t size() const {
        std::lock_guard lock(mutex_);
        return buffer_.size();
    }

    size_t capacity() const {
        return capacity_;
    }

private:
    // =========================================================================
    // 数据成员
    // =========================================================================
    
    /// @brief 通道容量
    /// @details 缓冲区最大存储的元素数量。
    ///          0 表示无缓冲通道（同步通道），每次发送必须等待接收者。
    ///          正数表示有缓冲通道，发送者在缓冲区未满时可立即返回。
    size_t capacity_;
    
    /// @brief 通道关闭标志（原子）
    /// @details 一旦设为 true，后续的发送操作会失败，
    ///          接收操作会在缓冲区清空后返回 nullopt。
    /// @thread_safety 原子操作保证线程安全
    std::atomic<bool> closed_;
    
    /// @brief 数据缓冲区
    /// @details 存储已发送但未被接收的数据。
    ///          使用 std::queue 实现 FIFO 语义。
    /// @thread_safety 由 mutex_ 保护
    std::queue<T> buffer_;
    
    /// @brief 发送者等待队列
    /// @details 当缓冲区满时，发送者协程挂起并加入此队列。
    ///          当有空间时，从队首取出发送者恢复执行。
    /// @thread_safety 由 mutex_ 保护
    std::queue<SendWaiter> send_waiters_;
    
    /// @brief 接收者等待队列
    /// @details 当缓冲区空时，接收者协程挂起并加入此队列。
    ///          当有数据时，从队首取出接收者恢复执行。
    /// @thread_safety 由 mutex_ 保护
    std::queue<RecvWaiter> recv_waiters_;
    
    /// @brief 互斥锁
    /// @details 保护 buffer_、send_waiters_、recv_waiters_ 的并发访问。
    ///          标记为 mutable 以便 const 方法（如 size()）也能加锁。
    mutable std::mutex mutex_;
};

}  // namespace zlcoro
