#pragma once

#include "zlcoro/core/task.hpp"
#include "zlcoro/scheduler/scheduler.hpp"
#include <future>
#include <memory>
#include <functional>

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
    // 使用 shared_ptr 包装状态
    struct State {
        Task<T> task;
        std::promise<T> promise;
        std::atomic<bool> executed{false};
        
        explicit State(Task<T>&& t) : task(std::move(t)) {}
    };
    
    auto state = std::make_shared<State>(std::move(task));
    auto future = state->promise.get_future();
    
    // 创建runner - 使用lambda按值捕获shared_ptr
    auto runner = [state]() {
        // 确保只执行一次，额外的保护机制
        bool expected = false;
        if (!state->executed.compare_exchange_strong(expected, true)) {
            return;  // 已经被执行过了
        }
        
        try {
            // 使用 sync_wait 执行协程
            if constexpr (std::is_void_v<T>) {
                state->task.sync_wait();
                state->promise.set_value();
            } else {
                T result = state->task.sync_wait();
                state->promise.set_value(std::move(result));
            }
        } catch (...) {
            try {
                state->promise.set_exception(std::current_exception());
            } catch (...) {
                // 忽略
            }
        }
    };
    
    // 提交到调度器
    Scheduler::instance().schedule(std::move(runner));
    
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
    
    std::function<void()> runner = [task_ptr]() {
        try {
            auto handle = task_ptr->handle();
            while (!handle.done()) {
                handle.resume();
            }
        } catch (...) {
            // 忽略异常
        }
    };
    
    Scheduler::instance().schedule(std::move(runner));
}

} // namespace zlcoro
