#pragma once

#include "zlcoro/core/task.hpp"
#include "zlcoro/scheduler/scheduler.hpp"
#include <future>
#include <memory>
#include <functional>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <atomic>

namespace zlcoro {

// =============================================================================
// async_run - 异步执行协程
// =============================================================================
// 
// 将协程提交到调度器，返回 std::future 用于等待结果
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
    // 使用 shared_ptr 管理状态和 Task
    struct State {
        Task<T> task;
        std::promise<T> promise;
        std::mutex mutex;
        std::condition_variable cv;
        std::atomic<bool> started{false};
        std::atomic<bool> completed{false};
        
        explicit State(Task<T>&& t) : task(std::move(t)) {}
    };
    
    auto state = std::make_shared<State>(std::move(task));
    auto future = state->promise.get_future();
    
    // 提交启动任务到调度器
    Scheduler::instance().schedule([state]() {
        auto handle = state->task.handle();
        
        // 标记已启动
        state->started.store(true);
        state->cv.notify_all();
        
        // 只启动一次协程
        if (!handle.done()) {
            handle.resume();
        }
        
        // 检查是否立即完成
        if (handle.done()) {
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
            state->completed.store(true);
            state->cv.notify_all();
        }
    });
    
    // 启动一个监控线程，等待协程完成
    std::thread([state]() {
        // 等待协程启动
        {
            std::unique_lock<std::mutex> lock(state->mutex);
            state->cv.wait(lock, [&] { return state->started.load(); });
        }
        
        // 如果已经完成，直接返回
        if (state->completed.load()) {
            return;
        }
        
        auto handle = state->task.handle();
        
        // 轮询等待协程完成
        while (!handle.done()) {
            std::this_thread::sleep_for(std::chrono::microseconds(50));
        }
        
        // 如果还没设置结果，设置它
        if (!state->completed.exchange(true)) {
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
    }).detach();
    
    return future;
}

// =============================================================================
// fire_and_forget - 启动协程但不关心结果
// =============================================================================
// 
// 用于不需要返回值的异步操作
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
    auto task_ptr = std::make_shared<Task<void>>(std::move(task));
    
    // 只启动一次协程，之后协程会通过 Scheduler 自己继续执行
    Scheduler::instance().schedule([task_ptr]() {
        auto handle = task_ptr->handle();
        if (!handle.done()) {
            handle.resume();
        }
        // 协程会自己继续运行，task_ptr 保持它的生命周期
    });
}

} // namespace zlcoro
