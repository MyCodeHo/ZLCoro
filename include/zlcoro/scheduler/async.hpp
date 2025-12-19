#pragma once

#include "zlcoro/core/task.hpp"
#include "zlcoro/scheduler/scheduler.hpp"
#include <future>
#include <memory>
#include <functional>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <thread>

namespace zlcoro {

// =============================================================================
// async_run - 异步执行协程
// =============================================================================
// 
// 将协程提交到调度器，返回 std::future 用于等待结果
// 
// 实现说明：
// 使用条件变量进行协程完成通知，配合轻量级轮询检查协程状态。
// 相比纯轮询方案，大幅减少 CPU 占用。
// 
// 使用示例:
//   Task<int> compute() {
//       co_return 42;
//   }
//
//   auto future = async_run(compute());
//   int result = future.get();  // 阻塞等待结果
// =============================================================================

template <typename T>
std::future<T> async_run(Task<T> task) {
    // 使用 shared_ptr 管理状态，确保线程安全的生命周期
    struct State {
        Task<T> task;
        std::promise<T> promise;
        std::mutex mutex;
        std::condition_variable cv;
        std::atomic<bool> started{false};
        std::atomic<bool> result_set{false};
        
        explicit State(Task<T>&& t) : task(std::move(t)) {}
    };
    
    auto state = std::make_shared<State>(std::move(task));
    auto future = state->promise.get_future();
    
    // 启动监控线程（先启动监控，再提交任务）
    std::thread monitor([state]() {
        // 等待任务启动
        {
            std::unique_lock<std::mutex> lock(state->mutex);
            state->cv.wait(lock, [&] { return state->started.load(); });
        }
        
        auto handle = state->task.handle();
        
        // 使用条件变量等待，每 10ms 检查一次协程是否完成
        // 比纯轮询节省大量 CPU，10ms 延迟对大多数场景可接受
        while (!handle.done()) {
            std::unique_lock<std::mutex> lock(state->mutex);
            state->cv.wait_for(lock, std::chrono::milliseconds(10));
        }
        
        // 设置结果（确保只设置一次）
        bool expected = false;
        if (state->result_set.compare_exchange_strong(expected, true)) {
            try {
                if constexpr (std::is_void_v<T>) {
                    handle.promise().result();
                    state->promise.set_value();
                } else {
                    state->promise.set_value(std::move(handle.promise()).result());
                }
            } catch (...) {
                try {
                    state->promise.set_exception(std::current_exception());
                } catch (...) {}
            }
        }
    });
    monitor.detach();
    
    // 提交任务到调度器
    Scheduler::instance().schedule([state]() {
        auto handle = state->task.handle();
        
        // 标记已启动并通知监控线程
        state->started.store(true);
        state->cv.notify_all();
        
        // 恢复协程（只启动一次）
        if (!handle.done()) {
            handle.resume();
        }
        
        // 如果协程立即完成，设置结果
        if (handle.done()) {
            bool expected = false;
            if (state->result_set.compare_exchange_strong(expected, true)) {
                try {
                    if constexpr (std::is_void_v<T>) {
                        handle.promise().result();
                        state->promise.set_value();
                    } else {
                        state->promise.set_value(std::move(handle.promise()).result());
                    }
                } catch (...) {
                    try {
                        state->promise.set_exception(std::current_exception());
                    } catch (...) {}
                }
            }
            // 通知监控线程退出
            state->cv.notify_all();
        }
    });
    
    return future;
}

// =============================================================================
// fire_and_forget - 启动协程但不关心结果
// =============================================================================
// 
// 用于不需要返回值的异步操作。使用监控线程确保协程完成后资源被正确释放。
// 
// 使用示例:
//   Task<void> background_work() {
//       // 做一些后台工作
//       co_return;
//   }
//
//   fire_and_forget(background_work());  // 启动后忘记
// =============================================================================

inline void fire_and_forget(Task<void> task) {
    // 使用 shared_ptr 管理 Task 的生命周期
    struct LifetimeHolder {
        Task<void> task;
        std::mutex mutex;
        std::condition_variable cv;
        std::atomic<bool> started{false};
        
        explicit LifetimeHolder(Task<void>&& t) : task(std::move(t)) {}
    };
    
    auto holder = std::make_shared<LifetimeHolder>(std::move(task));
    
    // 启动监控线程（先启动监控，再提交任务）
    std::thread monitor([holder]() {
        // 等待任务启动
        {
            std::unique_lock<std::mutex> lock(holder->mutex);
            holder->cv.wait(lock, [&] { return holder->started.load(); });
        }
        
        auto handle = holder->task.handle();
        
        // 使用条件变量等待，每 10ms 检查一次
        while (!handle.done()) {
            std::unique_lock<std::mutex> lock(holder->mutex);
            holder->cv.wait_for(lock, std::chrono::milliseconds(10));
        }
        
        // 协程完成，holder 的引用计数在这里减少
        // 当最后一个引用释放后，Task 才会被正确销毁
    });
    monitor.detach();
    
    // 提交到调度器
    Scheduler::instance().schedule([holder]() {
        auto handle = holder->task.handle();
        
        // 标记已启动并通知监控线程
        holder->started.store(true);
        holder->cv.notify_all();
        
        // 恢复协程
        if (!handle.done()) {
            handle.resume();
        }
        
        // 通知监控线程（协程可能已完成）
        holder->cv.notify_all();
    });
}

} // namespace zlcoro
