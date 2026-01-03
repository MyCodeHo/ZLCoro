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
        timers_.emplace(expire_time, std::make_pair(id, std::move(callback)));
        return id;
    }

    // 取消定时器
    void cancel_timer(TimerId id) {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto it = timers_.begin(); it != timers_.end(); ++it) {
            if (it->second.first == id) {
                timers_.erase(it);
                break;
            }
        }
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
            
            // multimap 按 expire_time 排序，从头开始处理到期的定时器
            auto it = timers_.begin();
            while (it != timers_.end() && it->first <= now) {
                expired_callbacks.push_back(std::move(it->second.second));
                it = timers_.erase(it);
            }
        }
        
        // 执行到期的定时器
        for (auto& callback : expired_callbacks) {
            callback();
        }
        
        // 重新获取当前时间（考虑回调执行耗时）
        now = std::chrono::steady_clock::now();
        
        // 计算下次超时时间
        std::lock_guard<std::mutex> lock(mutex_);
        if (timers_.empty()) {
            return 100;  // 默认 100ms
        }
        
        // multimap 按 expire_time 排序，第一个就是最早到期的
        auto next_expire = timers_.begin()->first;
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
            next_expire - now);
        
        return std::max(0, static_cast<int>(duration.count()));
    }

private:
    // =========================================================================
    // 数据成员
    // =========================================================================
    
    /// @brief Epoll 轮询器
    /// @details 封装 Linux epoll，负责监听 I/O 事件。
    ///          当文件描述符就绪时，返回关联的协程句柄。
    EpollPoller poller_;
    
    /// @brief 事件循环运行标志（原子）
    /// @details true 表示循环正在运行，false 表示已停止或将要停止。
    ///          在 run() 开始时设为 true，调用 stop() 后设为 false。
    std::atomic<bool> running_;
    
    /// @brief 就绪协程队列
    /// @details 等待执行的协程句柄队列。
    ///          I/O 事件就绪或通过 schedule() 添加的协程都会进入此队列。
    ///          使用 deque 支持高效的头部弹出。
    /// @thread_safety 由 mutex_ 保护
    std::deque<std::coroutine_handle<>> ready_queue_;
    
    /// @brief 互斥锁
    /// @details 保护 ready_queue_ 和 timers_ 的并发访问。
    std::mutex mutex_;
    
    /// @brief 定时器存储（按到期时间排序）
    /// @details 使用 multimap 以到期时间为 key，支持：
    ///          1. 自动按时间排序（便于找到最早到期的定时器）
    ///          2. 多个定时器同时到期（multimap 允许重复 key）
    ///          Value 是 (TimerId, Callback) 对。
    /// @thread_safety 由 mutex_ 保护
    std::multimap<std::chrono::steady_clock::time_point, 
                  std::pair<TimerId, TimerCallback>> timers_;
    
    /// @brief 定时器 ID 计数器
    /// @details 递增分配，保证每个定时器有唯一 ID。
    ///          用于 cancel_timer() 时识别要取消的定时器。
    TimerId next_timer_id_;
};

} // namespace zlcoro
