#pragma once

#include "zlcoro/core/task.hpp"
#include <chrono>
#include <thread>
#include <atomic>
#include <memory>
#include <functional>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <map>

namespace zlcoro {

// =============================================================================
// TimerWheel - 定时器轮（内部使用）
// =============================================================================
// 
// 单线程管理所有定时器，避免为每个 sleep 创建线程。
// 使用优先队列按到期时间排序。
// =============================================================================

class TimerWheel {
public:
    using TimePoint = std::chrono::steady_clock::time_point;
    using Duration = std::chrono::milliseconds;
    using Callback = std::function<void()>;
    
    struct TimerEntry {
        TimePoint deadline;
        std::coroutine_handle<> coro;           // 协程句柄（可选）
        Callback callback;                       // 回调函数（可选）
        std::shared_ptr<std::atomic<bool>> cancelled;
        
        bool operator>(const TimerEntry& other) const {
            return deadline > other.deadline;
        }
    };
    
    static TimerWheel& instance() {
        static TimerWheel wheel;
        return wheel;
    }
    
    // 添加定时器（协程版本），返回取消令牌
    std::shared_ptr<std::atomic<bool>> add_timer(
        Duration duration, 
        std::coroutine_handle<> coro) 
    {
        auto cancelled = std::make_shared<std::atomic<bool>>(false);
        auto deadline = std::chrono::steady_clock::now() + duration;
        
        {
            std::lock_guard<std::mutex> lock(mutex_);
            timers_.push({deadline, coro, nullptr, cancelled});
        }
        cv_.notify_one();
        
        return cancelled;
    }
    
    // 添加定时器（回调版本），返回取消令牌
    std::shared_ptr<std::atomic<bool>> add_timer_callback(
        Duration duration, 
        Callback callback) 
    {
        auto cancelled = std::make_shared<std::atomic<bool>>(false);
        auto deadline = std::chrono::steady_clock::now() + duration;
        
        {
            std::lock_guard<std::mutex> lock(mutex_);
            timers_.push({deadline, nullptr, std::move(callback), cancelled});
        }
        cv_.notify_one();
        
        return cancelled;
    }
    
    void shutdown() {
        bool expected = true;
        if (!running_.compare_exchange_strong(expected, false, std::memory_order_acq_rel)) {
            return;  // 已经关闭了
        }
        
        cv_.notify_all();
        
        if (worker_.joinable()) {
            worker_.join();
        }
    }

private:
    TimerWheel() : running_(true) {
        worker_ = std::thread([this]() { run(); });
    }
    
    ~TimerWheel() {
        shutdown();
    }
    
    // 禁止拷贝和移动
    TimerWheel(const TimerWheel&) = delete;
    TimerWheel& operator=(const TimerWheel&) = delete;
    
    void run() {
        while (running_.load(std::memory_order_acquire)) {
            std::unique_lock<std::mutex> lock(mutex_);
            
            if (timers_.empty()) {
                // 等待新定时器或关闭信号
                cv_.wait_for(lock, std::chrono::milliseconds(100), [this]() {
                    return !timers_.empty() || !running_.load(std::memory_order_acquire);
                });
                continue;
            }
            
            auto now = std::chrono::steady_clock::now();
            auto& top = timers_.top();
            
            if (top.deadline <= now) {
                // 定时器到期
                auto entry = timers_.top();
                timers_.pop();
                lock.unlock();
                
                // 检查是否已取消
                if (!entry.cancelled->load(std::memory_order_acquire)) {
                    // 优先执行回调
                    if (entry.callback) {
                        try {
                            entry.callback();
                        } catch (...) {
                            // 忽略回调中的异常
                        }
                    } else if (entry.coro && !entry.coro.done()) {
                        entry.coro.resume();
                    }
                }
            } else {
                // 等待到最近的到期时间
                auto wait_time = std::chrono::duration_cast<std::chrono::milliseconds>(
                    top.deadline - now);
                cv_.wait_for(lock, wait_time);
            }
        }
    }

private:
    // =========================================================================
    // 数据成员
    // =========================================================================
    
    /// @brief 定时器优先队列（按到期时间排序）
    /// @details 使用最小堆，堆顶始终是最早到期的定时器。
    ///          TimerEntry::operator> 定义了比较规则（greater 实现最小堆）。
    /// @thread_safety 由 mutex_ 保护
    std::priority_queue<TimerEntry, std::vector<TimerEntry>, std::greater<TimerEntry>> timers_;
    
    /// @brief 互斥锁
    /// @details 保护 timers_ 的并发访问。
    std::mutex mutex_;
    
    /// @brief 条件变量
    /// @details 用于工作线程的等待/唤醒：
    ///          - 队列为空时，工作线程 wait
    ///          - 添加新定时器时，notify_one
    ///          - 关闭时，notify_all
    std::condition_variable cv_;
    
    /// @brief 工作线程
    /// @details 单线程运行 run() 函数，循环检查到期定时器并执行回调/恢复协程。
    ///          相比为每个 sleep 创建线程，这种方式大大减少了线程开销。
    std::thread worker_;
    
    /// @brief 运行标志（原子）
    /// @details true 表示正在运行，shutdown() 设为 false 后工作线程退出。
    std::atomic<bool> running_{true};
};

// =============================================================================
// Timer - 协程定时器
// =============================================================================
// 
// 提供协程友好的定时器功能：
// - sleep(): 休眠指定时间
// - timeout(): 带超时的操作
// - interval(): 周期性执行
// 
// 使用示例:
//   co_await Timer::sleep(std::chrono::seconds(1));
//   
//   auto result = co_await Timer::timeout(
//       some_operation(),
//       std::chrono::seconds(5)
//   );
// =============================================================================

class Timer {
public:
    // =========================================================================
    // Sleep Awaiter - 使用 TimerWheel 而不是创建新线程
    // =========================================================================
    
    class SleepAwaiter {
    public:
        explicit SleepAwaiter(std::chrono::milliseconds duration)
            : duration_(duration) {}

        bool await_ready() const noexcept {
            return duration_.count() <= 0;
        }

        bool await_suspend(std::coroutine_handle<> coro) {
            if (duration_.count() <= 0) {
                return false;  // 不挂起
            }
            
            // 使用 TimerWheel 管理定时器
            cancel_token_ = TimerWheel::instance().add_timer(duration_, coro);
            return true;  // 挂起
        }

        void await_resume() const noexcept {
            // 休眠完成
        }
        
        // 允许取消
        void cancel() {
            if (cancel_token_) {
                cancel_token_->store(true, std::memory_order_release);
            }
        }

    private:
        // =====================================================================
        // 数据成员
        // =====================================================================
        
        /// @brief 休眠时长
        /// @details 协程挂起的目标时间长度。
        ///          如果 duration <= 0，await_ready() 返回 true，不会挂起。
        std::chrono::milliseconds duration_;
        
        /// @brief 取消令牌（共享指针）
        /// @details 注册到 TimerWheel 时返回，用于取消定时器。
        ///          设为 true 后，TimerWheel 不会恢复对应的协程。
        ///          使用 shared_ptr 是因为 TimerWheel 也持有一份引用。
        mutable std::shared_ptr<std::atomic<bool>> cancel_token_;
    };

    // 休眠指定时间
    static SleepAwaiter sleep(std::chrono::milliseconds duration) {
        return SleepAwaiter(duration);
    }

    // 重载：支持其他时间单位
    template<typename Rep, typename Period>
    static SleepAwaiter sleep(std::chrono::duration<Rep, Period> duration) {
        return SleepAwaiter(std::chrono::duration_cast<std::chrono::milliseconds>(duration));
    }

    // 便捷方法
    static SleepAwaiter sleep_ms(int64_t ms) {
        return sleep(std::chrono::milliseconds(ms));
    }

    static SleepAwaiter sleep_sec(int64_t sec) {
        return sleep(std::chrono::seconds(sec));
    }

    // =========================================================================
    // Timeout 支持
    // =========================================================================

    // 超时异常
    class TimeoutException : public std::exception {
    public:
        const char* what() const noexcept override {
            return "Operation timed out";
        }
    };

    // =========================================================================
    // TimeoutAwaiter - 带超时的等待器
    // =========================================================================
    // 
    // 使用竞态方式：任务完成或超时，谁先到谁生效。
    // 通过 TimerWheel 回调机制处理超时，避免创建额外线程。
    // =========================================================================
    
    template<typename T>
    class TimeoutAwaiter {
    public:
        TimeoutAwaiter(Task<T> task, std::chrono::milliseconds duration)
            : task_(std::move(task))
            , duration_(duration)
            , state_(std::make_shared<TimeoutState>()) {}
        
        bool await_ready() {
            // 如果任务已完成，直接返回
            auto handle = task_.handle();
            if (!handle || handle.done()) {
                return true;
            }
            // 先恢复一次，看是否能立即完成
            handle.resume();
            return handle.done();
        }
        
        bool await_suspend(std::coroutine_handle<> coro) {
            state_->waiting_coro = coro;
            
            auto handle = task_.handle();
            if (!handle || handle.done()) {
                return false;  // 任务已完成
            }
            
            auto state = state_;
            
            // 使用 TimerWheel 设置超时回调
            state_->timer_token = TimerWheel::instance().add_timer_callback(
                duration_,
                [state]() {
                    bool expected = false;
                    if (state->completed.compare_exchange_strong(expected, true,
                            std::memory_order_acq_rel)) {
                        state->timed_out = true;
                        if (state->waiting_coro && !state->waiting_coro.done()) {
                            state->waiting_coro.resume();
                        }
                    }
                }
            );
            
            // 在单独线程中运行任务
            // 注意：这是简化实现，理想情况下应使用调度器
            std::thread([state, handle]() mutable {
                while (!handle.done()) {
                    // 检查是否已超时
                    if (state->completed.load(std::memory_order_acquire)) {
                        return;  // 已超时，不再继续
                    }
                    handle.resume();
                    
                    // 如果还没完成且没超时，短暂休眠
                    if (!handle.done() && !state->completed.load(std::memory_order_acquire)) {
                        std::this_thread::sleep_for(std::chrono::microseconds(100));
                    }
                }
                
                bool expected = false;
                if (state->completed.compare_exchange_strong(expected, true,
                        std::memory_order_acq_rel)) {
                    state->timed_out = false;
                    // 取消超时定时器
                    if (state->timer_token) {
                        state->timer_token->store(true, std::memory_order_release);
                    }
                    if (state->waiting_coro && !state->waiting_coro.done()) {
                        state->waiting_coro.resume();
                    }
                }
            }).detach();
            
            return true;
        }
        
        T await_resume() {
            if (state_->timed_out) {
                throw TimeoutException();
            }
            
            if constexpr (std::is_void_v<T>) {
                task_.result();
            } else {
                return std::move(task_).result();
            }
        }
        
    private:
        struct TimeoutState {
            std::atomic<bool> completed{false};
            bool timed_out{false};
            std::coroutine_handle<> waiting_coro;
            std::shared_ptr<std::atomic<bool>> timer_token;
        };
        
        Task<T> task_;
        std::chrono::milliseconds duration_;
        std::shared_ptr<TimeoutState> state_;
    };

    // 带超时的 Task
    template<typename T>
    static TimeoutAwaiter<T> timeout(Task<T> task, std::chrono::milliseconds duration) {
        return TimeoutAwaiter<T>(std::move(task), duration);
    }

    // =========================================================================
    // Deadline 支持
    // =========================================================================

    using TimePoint = std::chrono::steady_clock::time_point;

    // 检查是否已超过截止时间
    static bool is_past_deadline(TimePoint deadline) {
        return std::chrono::steady_clock::now() >= deadline;
    }

    // 获取距离截止时间的剩余时间
    static std::chrono::milliseconds time_until(TimePoint deadline) {
        auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            return std::chrono::milliseconds(0);
        }
        return std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
    }

    // 从当前时间计算截止时间
    static TimePoint deadline_from_now(std::chrono::milliseconds duration) {
        return std::chrono::steady_clock::now() + duration;
    }
};

// =============================================================================
// Interval - 周期性定时器
// =============================================================================

class Interval {
public:
    explicit Interval(std::chrono::milliseconds period)
        : period_(period)
        , cancelled_(std::make_shared<std::atomic<bool>>(false)) {}

    // 取消定时器
    void cancel() {
        cancelled_->store(true, std::memory_order_release);
    }

    // 是否已取消
    bool is_cancelled() const {
        return cancelled_->load(std::memory_order_acquire);
    }

    // 等待下一次触发
    Task<bool> wait() {
        if (is_cancelled()) {
            co_return false;
        }

        co_await Timer::sleep(period_);

        co_return !is_cancelled();
    }

    // 获取周期
    std::chrono::milliseconds period() const {
        return period_;
    }

private:
    // =========================================================================
    // 数据成员
    // =========================================================================
    
    /// @brief 定时器周期
    /// @details 每次 wait() 调用之间的时间间隔。
    ///          这是一个常量，在构造后不会改变。
    std::chrono::milliseconds period_;
    
    /// @brief 取消标志（共享指针包装的原子布尔）
    /// @details 使用 shared_ptr 是为了支持在协程等待期间安全取消。
    ///          调用 cancel() 设置为 true，wait() 返回前会检查此标志。
    /// @thread_safety 原子操作保证线程安全
    std::shared_ptr<std::atomic<bool>> cancelled_;
};

// =============================================================================
// 便捷函数
// =============================================================================

// 休眠
inline Timer::SleepAwaiter sleep(std::chrono::milliseconds duration) {
    return Timer::sleep(duration);
}

template<typename Rep, typename Period>
inline Timer::SleepAwaiter sleep(std::chrono::duration<Rep, Period> duration) {
    return Timer::sleep(duration);
}

// 带超时执行
template<typename T>
inline Task<T> timeout(Task<T> task, std::chrono::milliseconds duration) {
    return Timer::timeout(std::move(task), duration);
}

} // namespace zlcoro
