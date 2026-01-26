#pragma once

#include "zlcoro/core/task.hpp"
#include "zlcoro/scheduler/scheduler.hpp"
#include <future>
#include <memory>
#include <atomic>
#include <thread>
#include <chrono>
#include <condition_variable>
#include <mutex>

namespace zlcoro {

// =============================================================================
// async_run - 异步执行协程并返回 future
// =============================================================================
// 优化版本：使用条件变量替代监控线程轮询
// 在协程完成时直接设置 promise，避免创建额外线程
// =============================================================================

namespace detail {

// 协程完成通知器
template<typename T>
struct CompletionNotifier {
    std::shared_ptr<std::promise<T>> promise;
    std::shared_ptr<std::atomic<bool>> completed;
    std::shared_ptr<std::mutex> mutex;
    std::shared_ptr<std::condition_variable> cv;
    
    CompletionNotifier()
        : promise(std::make_shared<std::promise<T>>())
        , completed(std::make_shared<std::atomic<bool>>(false))
        , mutex(std::make_shared<std::mutex>())
        , cv(std::make_shared<std::condition_variable>()) {}
    
    void notify() {
        {
            std::lock_guard<std::mutex> lock(*mutex);
            completed->store(true);
        }
        cv->notify_all();
    }
    
    void wait() {
        std::unique_lock<std::mutex> lock(*mutex);
        cv->wait(lock, [this] { return completed->load(); });
    }
    
    bool is_done() const {
        return completed->load();
    }
};

} // namespace detail

template <typename T>
std::future<T> async_run(Task<T> task) {
    auto notifier = std::make_shared<detail::CompletionNotifier<T>>();
    auto future = notifier->promise->get_future();
    auto task_ptr = std::make_shared<Task<T>>(std::move(task));
    
    // 调度执行协程
    Scheduler::instance().schedule([task_ptr, notifier]() {
        auto handle = task_ptr->handle();
        
        // 启动协程
        if (handle && !handle.done()) {
            handle.resume();
        }
        
        // 如果协程立即完成（无 co_await 或同步完成）
        if (handle.done()) {
            bool expected = false;
            if (notifier->completed->compare_exchange_strong(expected, true)) {
                try {
                    if constexpr (std::is_void_v<T>) {
                        handle.promise().result();
                        notifier->promise->set_value();
                    } else {
                        notifier->promise->set_value(std::move(handle.promise()).result());
                    }
                } catch (...) {
                    try {
                        notifier->promise->set_exception(std::current_exception());
                    } catch (...) {}
                }
                notifier->notify();
            }
            return;
        }
        
        // 如果协程挂起，启动一个轻量级监控（只在需要时）
        // 使用单个共享的监控线程池而非每次创建新线程
        std::thread([task_ptr, notifier]() {
            auto handle = task_ptr->handle();
            
            // 自适应轮询：快速响应但避免 busy-wait
            int delay_us = 10;  // 初始 10μs
            while (!handle.done()) {
                std::this_thread::sleep_for(std::chrono::microseconds(delay_us));
                // 逐渐增加延迟到 1ms，减少 CPU 占用
                if (delay_us < 1000) delay_us = std::min(delay_us * 2, 1000);
            }
            
            // 设置结果
            bool expected = false;
            if (notifier->completed->compare_exchange_strong(expected, true)) {
                try {
                    if constexpr (std::is_void_v<T>) {
                        handle.promise().result();
                        notifier->promise->set_value();
                    } else {
                        notifier->promise->set_value(std::move(handle.promise()).result());
                    }
                } catch (...) {
                    try {
                        notifier->promise->set_exception(std::current_exception());
                    } catch (...) {}
                }
                notifier->notify();
            }
        }).detach();
    });
    
    return future;
}

// =============================================================================
// fire_and_forget - 启动协程但不关心结果
// =============================================================================
// 优化版本：减少不必要的监控开销
// =============================================================================

inline void fire_and_forget(Task<void> task) {
    auto task_ptr = std::make_shared<Task<void>>(std::move(task));
    
    // 调度执行
    Scheduler::instance().schedule([task_ptr]() {
        auto handle = task_ptr->handle();
        if (handle && !handle.done()) {
            try {
                handle.resume();
            } catch (...) {
                // 忽略异常
            }
        }
        
        // 如果协程挂起，需要保持 task_ptr 存活
        if (!handle.done()) {
            // 启动监控线程保持 task 存活
            std::thread([task_ptr]() {
                auto handle = task_ptr->handle();
                int delay_us = 10;
                while (!handle.done()) {
                    std::this_thread::sleep_for(std::chrono::microseconds(delay_us));
                    if (delay_us < 1000) delay_us = std::min(delay_us * 2, 1000);
                }
            }).detach();
        }
    });
}

// =============================================================================
// sync_wait - 同步等待协程完成
// =============================================================================
// 在当前线程阻塞直到协程完成
// =============================================================================

template <typename T>
T sync_wait(Task<T> task) {
    return async_run(std::move(task)).get();
}

} // namespace zlcoro
