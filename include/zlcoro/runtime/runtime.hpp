#pragma once

#include "zlcoro/core/task.hpp"
#include "zlcoro/scheduler/work_stealing_scheduler.hpp"

#ifdef ZLCORO_HAS_IO_URING
#include "zlcoro/io/io_uring_poller.hpp"
#else
#include "zlcoro/io/epoll_poller.hpp"
#endif

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace zlcoro {

// =============================================================================
// Runtime - 统一的协程运行时
// =============================================================================
// 
// 整合调度器和 I/O 轮询器，提供统一的运行时入口。
// 
// 特性:
// - spawn(): 提交协程任务
// - block_on(): 阻塞等待任务完成
// - shutdown(): 优雅关闭
// - 自动处理 I/O 事件和任务调度
// 
// 使用示例:
//   Runtime runtime;
//   runtime.spawn(my_server_task());
//   runtime.block_on(main_task());
// =============================================================================

class Runtime {
public:
    // 配置选项
    struct Config {
        size_t num_threads = 0;           // 工作线程数，0 表示自动检测
        size_t io_queue_depth = 256;      // I/O 队列深度
        std::chrono::milliseconds io_poll_timeout{1};  // I/O 轮询超时
        
        static Config default_config() {
            return Config{};
        }
    };

    // 构造函数
    explicit Runtime(const Config& config = Config::default_config())
        : config_(config)
        , shutdown_(false)
        , scheduler_(config.num_threads == 0 
                     ? std::thread::hardware_concurrency() 
                     : config.num_threads)
#ifdef ZLCORO_HAS_IO_URING
        , io_poller_(config.io_queue_depth)
#endif
    {
        // 启动 I/O 轮询线程
        io_thread_ = std::thread([this]() {
            io_thread_func();
        });
    }

    // 简化构造函数
    explicit Runtime(size_t num_threads)
        : Runtime(Config{num_threads, 256, std::chrono::milliseconds(1)}) {}

    // 禁止拷贝
    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;

    // 析构函数
    ~Runtime() {
        shutdown();
    }

    // =========================================================================
    // 任务提交接口
    // =========================================================================

    // 提交一个 Task<void> 协程
    void spawn(Task<void> task) {
        if (shutdown_.load(std::memory_order_relaxed)) {
            return;
        }

        auto handle = task.handle();
        if (!handle || handle.done()) {
            return;
        }

        // 将 Task 移动到堆上，防止生命周期问题
        auto task_ptr = std::make_shared<Task<void>>(std::move(task));
        
        scheduler_.submit([task_ptr]() {
            auto handle = task_ptr->handle();
            if (handle && !handle.done()) {
                handle.resume();
            }
        });
    }

    // 提交一个普通函数或 lambda
    template<typename Func>
    void spawn(Func&& func) {
        if (shutdown_.load(std::memory_order_relaxed)) {
            return;
        }
        scheduler_.submit(std::forward<Func>(func));
    }

    // 提交协程句柄
    void schedule(std::coroutine_handle<> coro) {
        if (shutdown_.load(std::memory_order_relaxed)) {
            return;
        }
        scheduler_.schedule(coro);
    }

    // =========================================================================
    // 阻塞等待接口
    // =========================================================================

    // 阻塞等待 Task<void> 完成
    void block_on(Task<void> task) {
        std::promise<void> promise;
        auto future = promise.get_future();

        auto task_ptr = std::make_shared<Task<void>>(std::move(task));
        auto promise_ptr = std::make_shared<std::promise<void>>(std::move(promise));
        
        // 创建一个完成通知机制
        auto done_flag = std::make_shared<std::atomic<bool>>(false);
        auto done_cv = std::make_shared<std::condition_variable>();
        auto done_mutex = std::make_shared<std::mutex>();

        scheduler_.submit([this, task_ptr, promise_ptr, done_flag, done_cv, done_mutex]() {
            try {
                auto handle = task_ptr->handle();
                if (handle && !handle.done()) {
                    // 只恢复一次，让协程自行调度
                    handle.resume();
                    
                    // 如果协程挂起（未完成），我们需要等待它被调度完成
                    // 这里使用轮询但加入休眠，避免忙等待
                    while (!handle.done()) {
                        // 短暂让出 CPU，让其他任务有机会执行
                        std::this_thread::sleep_for(std::chrono::microseconds(100));
                        
                        // 检查是否已经完成
                        if (handle.done()) break;
                        
                        // 尝试恢复（如果协程还在等待可能不会做什么）
                        // 注意：真正的异步协程会通过 I/O 完成或定时器触发来恢复
                    }
                    
                    // 检查异常
                    task_ptr->result();
                }
                promise_ptr->set_value();
            } catch (...) {
                promise_ptr->set_exception(std::current_exception());
            }
            
            // 通知完成
            {
                std::lock_guard<std::mutex> lock(*done_mutex);
                done_flag->store(true, std::memory_order_release);
            }
            done_cv->notify_all();
        });

        // 等待完成，使用条件变量避免忙等待
        future.get();
    }

    // 阻塞等待 Task<T> 完成并返回结果
    template<typename T>
    T block_on(Task<T> task) {
        std::promise<T> promise;
        auto future = promise.get_future();

        auto task_ptr = std::make_shared<Task<T>>(std::move(task));
        auto promise_ptr = std::make_shared<std::promise<T>>(std::move(promise));

        scheduler_.submit([task_ptr, promise_ptr]() {
            try {
                auto handle = task_ptr->handle();
                if (handle && !handle.done()) {
                    handle.resume();
                    
                    // 等待协程完成，加入休眠避免忙等待
                    while (!handle.done()) {
                        std::this_thread::sleep_for(std::chrono::microseconds(100));
                    }
                    
                    if constexpr (std::is_void_v<T>) {
                        task_ptr->result();
                        promise_ptr->set_value();
                    } else {
                        promise_ptr->set_value(std::move(*task_ptr).result());
                    }
                }
            } catch (...) {
                promise_ptr->set_exception(std::current_exception());
            }
        });

        return future.get();
    }

    // =========================================================================
    // 运行和关闭
    // =========================================================================

    // 运行直到所有任务完成
    void run() {
        while (!shutdown_.load(std::memory_order_acquire)) {
            if (scheduler_.pending_tasks() == 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                
                // 再次检查是否有新任务
                if (scheduler_.pending_tasks() == 0) {
                    break;
                }
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
    }

    // 运行指定时间
    void run_for(std::chrono::milliseconds duration) {
        auto end_time = std::chrono::steady_clock::now() + duration;
        
        while (!shutdown_.load(std::memory_order_acquire) &&
               std::chrono::steady_clock::now() < end_time) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    // 优雅关闭
    void shutdown() {
        if (shutdown_.exchange(true, std::memory_order_acq_rel)) {
            return;  // 已经关闭
        }

        // 关闭调度器
        scheduler_.shutdown();

        // 等待 I/O 线程
        if (io_thread_.joinable()) {
            io_thread_.join();
        }
    }

    // 检查是否已关闭
    bool is_shutdown() const noexcept {
        return shutdown_.load(std::memory_order_acquire);
    }

    // =========================================================================
    // 访问器
    // =========================================================================

    WorkStealingScheduler& scheduler() noexcept {
        return scheduler_;
    }

    const WorkStealingScheduler& scheduler() const noexcept {
        return scheduler_;
    }

#ifdef ZLCORO_HAS_IO_URING
    IoUringPoller& io_poller() noexcept {
        return io_poller_;
    }

    const IoUringPoller& io_poller() const noexcept {
        return io_poller_;
    }
#endif

    // 获取工作线程数
    size_t thread_count() const noexcept {
        return scheduler_.thread_count();
    }

    // =========================================================================
    // 全局 Runtime 访问（可选）
    // =========================================================================
    
    // 设置当前线程的 Runtime（用于协程内部访问）
    static void set_current(Runtime* runtime) {
        current_runtime_ = runtime;
    }

    // 获取当前线程的 Runtime
    static Runtime* current() {
        return current_runtime_;
    }

private:
    void io_thread_func() {
#ifdef ZLCORO_HAS_IO_URING
        while (!shutdown_.load(std::memory_order_acquire)) {
            // 轮询 I/O 事件
            auto ready_coros = io_poller_.poll(
                static_cast<int>(config_.io_poll_timeout.count()));
            
            // 将就绪的协程提交到调度器
            for (auto& coro : ready_coros) {
                if (coro && !coro.done()) {
                    scheduler_.schedule(coro);
                }
            }
            
            // 如果没有 I/O 事件，短暂休眠避免 CPU 空转
            if (ready_coros.empty() && io_poller_.pending_count() == 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
#else
        // epoll 版本
        while (!shutdown_.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(config_.io_poll_timeout);
        }
#endif
    }

private:
    // =========================================================================
    // 数据成员
    // =========================================================================
    
    /// @brief 运行时配置
    /// @details 包含线程数、I/O 队列深度、轮询超时等参数。
    ///          在构造时设置，之后不可修改。
    Config config_;
    
    /// @brief 关闭标志（原子）
    /// @details true 表示 Runtime 正在或已经关闭。
    ///          shutdown() 会设置此标志，所有后台线程会检查并退出。
    std::atomic<bool> shutdown_;
    
    /// @brief 工作窃取调度器
    /// @details 负责调度和执行协程任务。
    ///          使用工作窃取算法实现高效的多线程调度。
    WorkStealingScheduler scheduler_;
    
#ifdef ZLCORO_HAS_IO_URING
    /// @brief io_uring 轮询器
    /// @details 负责异步 I/O 操作的提交和完成事件收集。
    ///          只在定义了 ZLCORO_HAS_IO_URING 时启用。
    IoUringPoller io_poller_;
#endif

    /// @brief I/O 轮询线程
    /// @details 独立线程运行 io_thread_func()，持续轮询 I/O 事件，
    ///          将就绪的协程提交到调度器执行。
    std::thread io_thread_;

    /// @brief 当前线程的 Runtime 指针（线程局部存储）
    /// @details 允许协程内部通过 Runtime::current() 访问当前 Runtime。
    ///          每个线程可以有不同的关联 Runtime（或为 nullptr）。
    static inline thread_local Runtime* current_runtime_ = nullptr;
};

// =============================================================================
// 便捷函数
// =============================================================================

// 在当前 Runtime 中 spawn 任务
inline void spawn(Task<void> task) {
    if (auto* rt = Runtime::current()) {
        rt->spawn(std::move(task));
    }
}

// 在当前 Runtime 中调度协程
inline void schedule(std::coroutine_handle<> coro) {
    if (auto* rt = Runtime::current()) {
        rt->schedule(coro);
    }
}

} // namespace zlcoro
