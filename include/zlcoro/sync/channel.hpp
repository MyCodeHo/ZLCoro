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

        SendAwaiter(Channel* ch, T val) 
            : ch_(ch), value_(std::make_shared<T>(std::move(val))) {}

        bool await_ready() { return false; }

        bool await_suspend(std::coroutine_handle<> handle) {
            std::unique_lock lock(ch_->mutex_);
            
            if (ch_->closed_) {
                return false;  // 不挂起，抛异常
            }

            // 1. 检查是否有接收者在等待
            if (!ch_->recv_waiters_.empty()) {
                RecvWaiter waiter = ch_->recv_waiters_.front();
                ch_->recv_waiters_.pop();
                *(waiter.result) = std::move(*value_);
                
                lock.unlock();
                if (waiter.handle && !waiter.handle.done()) {
                    Scheduler::instance().schedule(waiter.handle);
                }
                return false;  // 不挂起当前协程
            }

            // 2. 尝试放入缓冲区
            if (ch_->buffer_.size() < ch_->capacity_) {
                ch_->buffer_.push(std::move(*value_));
                return false;  // 不挂起
            }

            // 3. 挂起并加入等待队列（使用 shared_ptr，生命周期安全）
            ch_->send_waiters_.push({handle, value_});
            return true;  // 挂起
        }

        void await_resume() {
            if (ch_->closed_) {
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
        
        {
            std::lock_guard lock(mutex_);

            if (!buffer_.empty()) {
                result = std::move(buffer_.front());
                buffer_.pop();

                if (!send_waiters_.empty()) {
                    SendWaiter waiter = send_waiters_.front();
                    send_waiters_.pop();
                    buffer_.push(std::move(*waiter.value));
                    to_resume = waiter.handle;
                }
            } else if (!send_waiters_.empty()) {
                SendWaiter waiter = send_waiters_.front();
                send_waiters_.pop();
                result = std::move(*waiter.value);
                to_resume = waiter.handle;
            }
        }
        
        if (to_resume && !to_resume.done()) {
            Scheduler::instance().schedule(to_resume);
        }
        return result;
    }

    void close() {
        std::queue<RecvWaiter> waiters_copy;
        
        {
            std::lock_guard lock(mutex_);
            
            if (closed_) {
                return;
            }
            
            closed_ = true;
            waiters_copy = recv_waiters_;
            while (!recv_waiters_.empty()) {
                recv_waiters_.pop();
            }
        }
        
        // 设置所有等待接收者的结果为 nullopt，然后调度
        while (!waiters_copy.empty()) {
            auto waiter = waiters_copy.front();
            waiters_copy.pop();
            *(waiter.result) = std::nullopt;  // 通道关闭，返回空值
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
    size_t capacity_;
    std::atomic<bool> closed_;
    std::queue<T> buffer_;
    std::queue<SendWaiter> send_waiters_;
    std::queue<RecvWaiter> recv_waiters_;
    mutable std::mutex mutex_;
};

}  // namespace zlcoro
