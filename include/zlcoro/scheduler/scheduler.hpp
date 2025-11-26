#pragma once

#include "thread_pool.hpp"
#include "zlcoro/core/task.hpp"
#include <coroutine>
#include <memory>

namespace zlcoro {

// =============================================================================
// Scheduler - 协程调度器
// =============================================================================
// 
// 负责调度协程的执行，将协程任务分配到线程池。
// 
// 使用方式:
//   auto& scheduler = Scheduler::instance();
//   scheduler.schedule(some_coroutine_handle);
// =============================================================================

class Scheduler {
public:
    // 获取全局调度器实例（单例模式）
    static Scheduler& instance() {
        static Scheduler scheduler;
        return scheduler;
    }

    // 禁止拷贝和移动
    Scheduler(const Scheduler&) = delete;
    Scheduler& operator=(const Scheduler&) = delete;

    // 调度一个协程句柄
    void schedule(std::coroutine_handle<> coro) {
        if (!coro || coro.done()) {
            return;
        }
        
        thread_pool_.submit([coro]() mutable {
            if (!coro.done()) {
                coro.resume();
            }
        });
    }

    // 调度一个可调用对象
    template <typename Func>
    void schedule(Func&& func) {
        thread_pool_.submit(std::forward<Func>(func));
    }

    // 获取线程池
    ThreadPool& thread_pool() {
        return thread_pool_;
    }

    // 获取线程数量
    size_t thread_count() const noexcept {
        return thread_pool_.thread_count();
    }

private:
    // 私有构造函数（单例模式）
    Scheduler() 
        : thread_pool_(std::thread::hardware_concurrency()) {
    }

    ~Scheduler() {
        thread_pool_.shutdown();
    }

private:
    ThreadPool thread_pool_;
};

// =============================================================================
// ScheduleAwaiter - 用于 co_await 切换到调度器
// =============================================================================
// 
// 使用方式:
//   co_await schedule();  // 当前协程会被重新调度到线程池
// =============================================================================

struct ScheduleAwaiter {
    bool await_ready() const noexcept {
        return false;  // 总是挂起
    }

    void await_suspend(std::coroutine_handle<> coro) const {
        // 将协程提交到调度器
        Scheduler::instance().schedule(coro);
    }

    void await_resume() const noexcept {}
};

// 辅助函数：创建 ScheduleAwaiter
inline ScheduleAwaiter schedule() {
    return {};
}

// =============================================================================
// NewThreadAwaiter - 在新线程中恢复协程
// =============================================================================
// 
// 使用方式:
//   co_await resume_on_new_thread();  // 协程在新线程中继续执行
// =============================================================================

struct NewThreadAwaiter {
    bool await_ready() const noexcept {
        return false;
    }

    void await_suspend(std::coroutine_handle<> coro) const {
        std::thread([coro]() mutable {
            coro.resume();
        }).detach();
    }

    void await_resume() const noexcept {}
};

inline NewThreadAwaiter resume_on_new_thread() {
    return {};
}

} // namespace zlcoro
