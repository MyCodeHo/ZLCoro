#pragma once

#include "epoll_poller.hpp"
#include <coroutine>
#include <deque>
#include <mutex>
#include <atomic>
#include <chrono>
#include <functional>
#include <map>

namespace zlcoro {

// =============================================================================
// EventLoop - 事件循环
// =============================================================================
// 
// 管理 I/O 事件和协程调度的核心组件。
// 使用 Reactor 模式，在单线程中处理所有 I/O 事件和协程。
// =============================================================================

class EventLoop {
public:
    // 定时器 ID 类型
    using TimerId = uint64_t;
    
    // 定时器回调
    using TimerCallback = std::function<void()>;

    EventLoop() : running_(false), next_timer_id_(0) {}

    // 禁止拷贝
    EventLoop(const EventLoop&) = delete;
    EventLoop& operator=(const EventLoop&) = delete;

    // 获取全局事件循环实例（单例）
    static EventLoop& instance() {
        static EventLoop loop;
        return loop;
    }

    // 运行事件循环（阻塞）
    void run() {
        running_ = true;
        
        while (running_) {
            // 1. 执行所有待调度的协程
            process_ready_queue();
            
            // 2. 检查并执行到期的定时器
            auto next_timeout = process_timers();
            
            // 3. 等待 I/O 事件
            if (running_) {
                auto ready_coros = poller_.poll(next_timeout);
                
                // 将就绪的协程加入队列
                for (auto coro : ready_coros) {
                    schedule(coro);
                }
            }
        }
    }

    // 停止事件循环
    void stop() {
        running_ = false;
    }

    // 调度一个协程（加入就绪队列）
    void schedule(std::coroutine_handle<> coro) {
        std::lock_guard<std::mutex> lock(mutex_);
        ready_queue_.push_back(coro);
    }

    // 注册文件描述符的读事件
    void register_read(int fd, std::coroutine_handle<> coro) {
        if (poller_.has(fd)) {
            poller_.modify(fd, EpollPoller::Read | EpollPoller::EdgeTriggered, coro);
        } else {
            poller_.add(fd, EpollPoller::Read | EpollPoller::EdgeTriggered, coro);
        }
    }

    // 注册文件描述符的写事件
    void register_write(int fd, std::coroutine_handle<> coro) {
        if (poller_.has(fd)) {
            poller_.modify(fd, EpollPoller::Write | EpollPoller::EdgeTriggered, coro);
        } else {
            poller_.add(fd, EpollPoller::Write | EpollPoller::EdgeTriggered, coro);
        }
    }

    // 同时注册读写事件
    void register_rw(int fd, std::coroutine_handle<> coro) {
        uint32_t events = EpollPoller::Read | EpollPoller::Write | EpollPoller::EdgeTriggered;
        if (poller_.has(fd)) {
            poller_.modify(fd, events, coro);
        } else {
            poller_.add(fd, events, coro);
        }
    }

    // 取消注册
    void unregister(int fd) {
        poller_.remove(fd);
    }

    // 添加定时器（返回定时器 ID）
    TimerId add_timer(int delay_ms, TimerCallback callback) {
        auto expire_time = std::chrono::steady_clock::now() + 
                          std::chrono::milliseconds(delay_ms);
        
        std::lock_guard<std::mutex> lock(mutex_);
        TimerId id = next_timer_id_++;
        timers_[id] = Timer{expire_time, std::move(callback)};
        return id;
    }

    // 取消定时器
    void cancel_timer(TimerId id) {
        std::lock_guard<std::mutex> lock(mutex_);
        timers_.erase(id);
    }

    // 检查是否正在运行
    bool is_running() const {
        return running_;
    }

private:
    // 处理就绪队列中的协程
    void process_ready_queue() {
        std::deque<std::coroutine_handle<>> local_queue;
        
        {
            std::lock_guard<std::mutex> lock(mutex_);
            local_queue.swap(ready_queue_);
        }
        
        for (auto coro : local_queue) {
            if (coro && !coro.done()) {
                coro.resume();
            }
        }
    }

    // 处理定时器，返回下次超时时间（毫秒）
    int process_timers() {
        auto now = std::chrono::steady_clock::now();
        std::vector<TimerCallback> expired_callbacks;
        
        {
            std::lock_guard<std::mutex> lock(mutex_);
            
            auto it = timers_.begin();
            while (it != timers_.end()) {
                if (it->second.expire_time <= now) {
                    expired_callbacks.push_back(std::move(it->second.callback));
                    it = timers_.erase(it);
                } else {
                    ++it;
                }
            }
        }
        
        // 执行到期的定时器
        for (auto& callback : expired_callbacks) {
            callback();
        }
        
        // 计算下次超时时间
        std::lock_guard<std::mutex> lock(mutex_);
        if (timers_.empty()) {
            return 100;  // 默认 100ms
        }
        
        auto next_expire = timers_.begin()->second.expire_time;
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
            next_expire - now);
        
        return std::max(0, static_cast<int>(duration.count()));
    }

private:
    // 定时器结构
    struct Timer {
        std::chrono::steady_clock::time_point expire_time;
        TimerCallback callback;
    };

    EpollPoller poller_;                                    // Epoll 轮询器
    std::atomic<bool> running_;                             // 运行标志
    std::deque<std::coroutine_handle<>> ready_queue_;       // 就绪队列
    std::mutex mutex_;                                       // 保护共享数据
    std::map<TimerId, Timer> timers_;                       // 定时器
    TimerId next_timer_id_;                                 // 下一个定时器 ID
};

} // namespace zlcoro
