#pragma once

#include "zlcoro/core/task.hpp"
#include <coroutine>
#include <functional>
#include <vector>
#include <map>
#include <chrono>
#include <atomic>
#include <thread>
#include <memory>

#ifdef __linux__
#include <sched.h>
#include <pthread.h>
#endif

namespace zlcoro {

// =============================================================================
// PerCoreEventLoop - 每核心事件循环基类
// =============================================================================
// 
// 设计理念：
// - 每个 CPU 核心运行一个独立的事件循环
// - 协程绑定到特定核心，无跨线程切换
// - 完全无锁设计，每个核心独立运行
// - 支持 epoll 和 io_uring 两种后端
//
// 优势：
// - 极低延迟：无锁、无队列等待
// - 高缓存命中率：数据和协程都在本核心
// - 线性扩展：性能随核心数增长
// =============================================================================

class PerCoreEventLoop {
public:
    // 定时器 ID
    using TimerId = uint64_t;
    using TimerCallback = std::function<void()>;
    
    // 定时器条目
    struct TimerEntry {
        TimerId id;
        TimerCallback callback;
    };

    PerCoreEventLoop() 
        : core_id_(-1)
        , running_(false)
        , next_timer_id_(0) {}

    virtual ~PerCoreEventLoop() = default;

    // 禁止拷贝和移动
    PerCoreEventLoop(const PerCoreEventLoop&) = delete;
    PerCoreEventLoop& operator=(const PerCoreEventLoop&) = delete;

    // =========================================================================
    // 核心生命周期
    // =========================================================================

    // 绑定到指定 CPU 核心
    void bind_to_core(int core_id) {
        core_id_ = core_id;
        
#ifdef __linux__
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        CPU_SET(core_id, &cpuset);
        
        int rc = pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
        if (rc != 0) {
            // 绑定失败，继续运行但可能性能下降
        }
#endif
    }

    // 获取核心 ID
    int core_id() const noexcept { return core_id_; }

    // 运行事件循环（阻塞）
    void run() {
        running_ = true;
        
        while (running_) {
            run_once();
        }
    }

    // 运行一次事件循环迭代
    void run_once() {
        // 1. 处理就绪队列中的协程
        process_ready_queue();
        
        // 2. 处理到期的定时器
        int timeout = process_timers();
        
        // 3. 等待 I/O 事件（子类实现）
        poll_events(timeout);
    }

    // 停止事件循环
    void stop() {
        running_ = false;
        wakeup();
    }

    // 检查是否运行中
    bool is_running() const noexcept { return running_; }

    // =========================================================================
    // 协程调度（本核心内）
    // =========================================================================

    // 调度协程在本核心执行
    void schedule(std::coroutine_handle<> coro) {
        if (coro && !coro.done()) {
            ready_queue_.push_back(coro);
        }
    }

    // 调度任务在本核心执行
    template<typename Func>
    void post(Func&& func) {
        pending_tasks_.push_back(std::forward<Func>(func));
    }

    // =========================================================================
    // 定时器
    // =========================================================================

    // 添加定时器
    TimerId add_timer(int delay_ms, TimerCallback callback) {
        auto expire_time = std::chrono::steady_clock::now() + 
                          std::chrono::milliseconds(delay_ms);
        
        TimerId id = next_timer_id_++;
        timers_.emplace(expire_time, TimerEntry{id, std::move(callback)});
        return id;
    }

    // 取消定时器
    void cancel_timer(TimerId id) {
        for (auto it = timers_.begin(); it != timers_.end(); ) {
            if (it->second.id == id) {
                it = timers_.erase(it);
            } else {
                ++it;
            }
        }
    }

    // =========================================================================
    // I/O 事件注册（子类实现）
    // =========================================================================

    // 注册读事件
    virtual void register_read(int fd, std::coroutine_handle<> coro) = 0;
    
    // 注册写事件
    virtual void register_write(int fd, std::coroutine_handle<> coro) = 0;
    
    // 注册读写事件
    virtual void register_rw(int fd, std::coroutine_handle<> coro) = 0;
    
    // 取消注册
    virtual void unregister(int fd) = 0;
    
    // 检查 fd 是否已注册
    virtual bool has_fd(int fd) const = 0;

    // =========================================================================
    // 后端类型
    // =========================================================================

    enum class Backend {
        Epoll,
        IoUring
    };
    
    virtual Backend backend() const = 0;

protected:
    // 轮询 I/O 事件（子类实现）
    virtual void poll_events(int timeout_ms) = 0;
    
    // 唤醒阻塞的 poll（子类实现）
    virtual void wakeup() = 0;

    // 处理就绪队列
    void process_ready_queue() {
        // 处理协程
        for (auto& coro : ready_queue_) {
            if (coro && !coro.done()) {
                coro.resume();
            }
        }
        ready_queue_.clear();
        
        // 处理普通任务
        for (auto& task : pending_tasks_) {
            task();
        }
        pending_tasks_.clear();
    }

    // 处理定时器，返回下次超时时间（毫秒）
    int process_timers() {
        auto now = std::chrono::steady_clock::now();
        
        // 执行到期的定时器
        while (!timers_.empty() && timers_.begin()->first <= now) {
            auto it = timers_.begin();
            auto callback = std::move(it->second.callback);
            timers_.erase(it);
            callback();
        }
        
        // 计算下次超时
        if (timers_.empty()) {
            return 100;  // 默认 100ms
        }
        
        auto next = timers_.begin()->first;
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(next - now);
        return std::max(0, static_cast<int>(duration.count()));
    }

protected:
    int core_id_;                  // 绑定的 CPU 核心
    std::atomic<bool> running_;   // 运行状态
    
    // 就绪队列（本核心独占，无需加锁）
    std::vector<std::coroutine_handle<>> ready_queue_;
    std::vector<std::function<void()>> pending_tasks_;
    
    // 定时器
    std::multimap<std::chrono::steady_clock::time_point, TimerEntry> timers_;
    TimerId next_timer_id_;
};

// =============================================================================
// 获取当前核心的 EventLoop（thread_local）
// =============================================================================

inline PerCoreEventLoop*& current_event_loop() {
    thread_local PerCoreEventLoop* loop = nullptr;
    return loop;
}

// 设置当前线程的 EventLoop
inline void set_current_event_loop(PerCoreEventLoop* loop) {
    current_event_loop() = loop;
}

// 获取当前线程的 EventLoop
inline PerCoreEventLoop* get_current_event_loop() {
    return current_event_loop();
}

} // namespace zlcoro
