#pragma once

#include <coroutine>
#include <exception>
#include <memory>
#include <type_traits>
#include <utility>
#include <cassert>

namespace zlcoro {

// 前向声明
template <typename T>
class Task;

namespace detail {

// ============================================================================
// TaskPromiseBase - Promise 类型的基类，处理公共逻辑
// ============================================================================
// Promise 类型是协程的核心，它定义了协程的行为：
// - initial_suspend: 协程启动时是否挂起
// - final_suspend: 协程结束时是否挂起
// - return_value/return_void: 如何处理返回值
// - unhandled_exception: 如何处理异常
// ============================================================================
class TaskPromiseBase {
public:
    // 协程启动时立即挂起，等待调度器调度
    // suspend_always 表示总是挂起（惰性求值）
    std::suspend_always initial_suspend() noexcept {
        return {};
    }

    // final_awaiter 用于在协程结束时的特殊处理
    struct FinalAwaiter {
        // 协程结束时总是挂起，不自动销毁
        bool await_ready() noexcept { return false; }

        // 当协程结束时，如果有等待者（continuation），则恢复等待者
        // 这实现了协程的链式调用：Task1 co_await Task2
        template <typename Promise>
        std::coroutine_handle<> await_suspend(
            std::coroutine_handle<Promise> coro) noexcept {
            auto& promise = coro.promise();
            
            // 如果有协程在等待当前协程完成，恢复那个协程
            if (promise.continuation_) {
                return promise.continuation_;
            }
            
            // 否则返回 noop，表示不恢复任何协程
            return std::noop_coroutine();
        }

        void await_resume() noexcept {}
    };

    FinalAwaiter final_suspend() noexcept {
        return {};
    }

    // 设置延续协程（当前协程完成后要恢复的协程）
    void set_continuation(std::coroutine_handle<> continuation) noexcept {
        continuation_ = continuation;
    }

protected:
    // ========================================================================
    // 数据成员
    // ========================================================================
    
    /// @brief 延续协程句柄
    /// @details 当当前协程执行完毕后，需要恢复的协程句柄。
    ///          用于实现 co_await 链式调用：当 TaskA co_await TaskB 时，
    ///          TaskB 完成后会恢复 TaskA 的执行。
    ///          如果没有等待者，则为空句柄。
    /// @thread_safety 由单个协程独占，无需同步
    std::coroutine_handle<> continuation_;
};

// ============================================================================
// TaskPromise<T> - 有返回值的 Promise 实现
// ============================================================================
template <typename T>
class TaskPromise : public TaskPromiseBase {
public:
    using value_type = T;

    // 默认构造函数 - 协程框架需要能够构造 Promise 对象
    TaskPromise() : dummy_() {}

    // 创建 Task 对象（编译器会调用这个函数）
    Task<T> get_return_object() noexcept;

    // 处理 co_return value; 语句
    // 这里使用完美转发，支持移动语义
    template <typename U>
        requires std::convertible_to<U, T>
    void return_value(U&& value) noexcept(
        std::is_nothrow_constructible_v<T, U&&>) {
        // 在预留的内存中构造返回值
        std::construct_at(std::addressof(value_), std::forward<U>(value));
        has_value_ = true;
    }

    // 处理协程中未捕获的异常
    void unhandled_exception() noexcept {
        exception_ = std::current_exception();
    }

    // 获取结果（可能抛出异常）
    T& result() & {
        if (exception_) {
            std::rethrow_exception(exception_);
        }
        // 防御性检查：确保有值可返回
        // 正常情况下协程必须调用 return_value() 或抛出异常
        assert(has_value_ && "TaskPromise::result() called but no value was set");
        return value_;
    }

    T&& result() && {
        if (exception_) {
            std::rethrow_exception(exception_);
        }
        assert(has_value_ && "TaskPromise::result() called but no value was set");
        return std::move(value_);
    }

    ~TaskPromise() {
        // 如果有值，需要手动析构
        if (has_value_) {
            std::destroy_at(std::addressof(value_));
        }
    }

private:
    // ========================================================================
    // 数据成员
    // ========================================================================
    
    /// @brief 返回值存储（使用 union 延迟初始化）
    /// @details 使用 union 技巧避免 T 的默认构造要求。
    ///          只有在 return_value() 被调用后才会构造 value_。
    ///          这是一种常见的优化模式，称为"延迟初始化"。
    union {
        T value_;      ///< 协程的返回值（仅当 has_value_ 为 true 时有效）
        char dummy_;   ///< 占位符，用于默认构造 union
    };
    
    /// @brief 未捕获的异常
    /// @details 如果协程执行过程中抛出未捕获的异常，
    ///          会被存储在这里，在 result() 调用时重新抛出。
    std::exception_ptr exception_;
    
    /// @brief 是否已设置返回值
    /// @details 用于区分 value_ 是否已被构造。
    ///          在析构时需要检查此标志以决定是否析构 value_。
    bool has_value_ = false;
};

// ============================================================================
// TaskPromise<void> - void 返回值的特化版本
// ============================================================================
template <>
class TaskPromise<void> : public TaskPromiseBase {
public:
    using value_type = void;

    Task<void> get_return_object() noexcept;

    // void 类型使用 return_void
    void return_void() noexcept {}

    void unhandled_exception() noexcept {
        exception_ = std::current_exception();
    }

    void result() {
        if (exception_) {
            std::rethrow_exception(exception_);
        }
    }

private:
    // ========================================================================
    // 数据成员
    // ========================================================================
    
    /// @brief 未捕获的异常
    /// @details void 类型协程没有返回值，只需要存储可能的异常。
    std::exception_ptr exception_;
};

// ============================================================================
// TaskPromise<T&> - 引用返回值的特化版本
// ============================================================================
template <typename T>
class TaskPromise<T&> : public TaskPromiseBase {
public:
    using value_type = T&;

    Task<T&> get_return_object() noexcept;

    void return_value(T& value) noexcept {
        // 引用只需要存储指针
        value_ = std::addressof(value);
    }

    void unhandled_exception() noexcept {
        exception_ = std::current_exception();
    }

    T& result() {
        if (exception_) {
            std::rethrow_exception(exception_);
        }
        return *value_;
    }

private:
    // ========================================================================
    // 数据成员
    // ========================================================================
    
    /// @brief 引用目标的指针
    /// @details 引用类型不能直接存储，需要存储指向目标对象的指针。
    ///          在 return_value() 中设置，在 result() 中解引用返回。
    T* value_ = nullptr;
    
    /// @brief 未捕获的异常
    std::exception_ptr exception_;
};

} // namespace detail

// ============================================================================
// Task<T> - 协程任务类
// ============================================================================
// Task 是一个可等待的(awaitable)对象，代表一个异步计算
// 使用方式:
//   Task<int> compute() {
//       co_return 42;
//   }
//   
//   Task<void> caller() {
//       int result = co_await compute();  // 等待 compute 完成
//   }
// ============================================================================
template <typename T = void>
class Task {
public:
    using promise_type = detail::TaskPromise<T>;
    using value_type = T;

    // 从 Promise 构造 Task（编译器调用）
    explicit Task(std::coroutine_handle<promise_type> coro) noexcept
        : coro_(coro) {}

    // Task 不可复制，只能移动（因为持有协程句柄的所有权）
    Task(const Task&) = delete;
    Task& operator=(const Task&) = delete;

    Task(Task&& other) noexcept : coro_(std::exchange(other.coro_, {})) {}//

    Task& operator=(Task&& other) noexcept {
        if (this != &other) {
            if (coro_) {
                coro_.destroy();
            }
            coro_ = std::exchange(other.coro_, {});
        }
        return *this;
    }

    // 析构时销毁协程
    ~Task() {
        if (coro_) {
            coro_.destroy();
        }
    }

    // 检查协程是否有效
    bool valid() const noexcept {
        return coro_ != nullptr;
    }

    // ========================================================================
    // Awaiter - 使 Task 可以被 co_await
    // ========================================================================
    // Awaiter 定义了 co_await 的行为：
    // - await_ready: 是否需要挂起
    // - await_suspend: 挂起时的行为（通常是调度协程）
    // - await_resume: 恢复时的行为（通常是返回结果）
    // ========================================================================
    struct Awaiter {
        std::coroutine_handle<promise_type> coro_;

        // 如果任务已经完成，不需要挂起
        bool await_ready() const noexcept {
            return !coro_ || coro_.done();
        }

        // 当前协程挂起，启动被等待的协程
        // awaiting_coro: 正在执行 co_await 的协程（等待者）
        // coro_: 被等待的协程（Task 持有的协程）
        std::coroutine_handle<> await_suspend(
            std::coroutine_handle<> awaiting_coro) noexcept {
            // 设置延续：当 coro_ 完成后，恢复 awaiting_coro
            coro_.promise().set_continuation(awaiting_coro);
            // 返回 coro_，表示接下来要恢复被等待的协程
            return coro_;
        }

        // 被等待的协程完成后，返回结果
        decltype(auto) await_resume() {
            if constexpr (std::is_void_v<T>) {
                coro_.promise().result();
                return;
            } else if constexpr (std::is_reference_v<T>) {
                return coro_.promise().result();
            } else {
                return std::move(coro_.promise()).result();
            }
        }
    };

    // 实现 co_await 运算符
    auto operator co_await() const& noexcept {
        return Awaiter{coro_};
    }

    auto operator co_await() const&& noexcept {
        return Awaiter{coro_};
    }

    // ========================================================================
    // 同步等待接口（阻塞当前线程直到协程完成）
    // ========================================================================
    // 注意：这个接口会阻塞线程，主要用于：
    // 1. 测试代码
    // 2. main 函数中启动异步任务
    // 3. 从同步代码调用异步代码的边界
    // ========================================================================
    decltype(auto) sync_wait() {
        // 如果协程还没启动，启动它
        if (!coro_.done()) {
            coro_.resume();
        }

        // 持续恢复协程直到完成
        // 注意：这是一个简化版本，实际调度器会更复杂
        while (!coro_.done()) {
            coro_.resume();
        }

        // 返回结果
        if constexpr (std::is_void_v<T>) {
            coro_.promise().result();
            return;
        } else if constexpr (std::is_reference_v<T>) {
            return coro_.promise().result();
        } else {
            return std::move(coro_.promise()).result();
        }
    }

    // 获取底层协程句柄（高级用法）
    std::coroutine_handle<promise_type> handle() const noexcept {
        return coro_;
    }

    // 释放协程句柄的所有权（高级用法）
    // 调用后 Task 不再拥有协程，协程需要手动管理或自行销毁
    std::coroutine_handle<promise_type> release() noexcept {
        return std::exchange(coro_, {});
    }

    // 获取结果（需要协程已完成）
    decltype(auto) result() & {
        if constexpr (std::is_void_v<T>) {
            coro_.promise().result();
            return;
        } else if constexpr (std::is_reference_v<T>) {
            return coro_.promise().result();
        } else {
            return coro_.promise().result();
        }
    }

    decltype(auto) result() && {
        if constexpr (std::is_void_v<T>) {
            coro_.promise().result();
            return;
        } else if constexpr (std::is_reference_v<T>) {
            return coro_.promise().result();
        } else {
            return std::move(coro_.promise()).result();
        }
    }

private:
    // ========================================================================
    // 数据成员
    // ========================================================================
    
    /// @brief 底层协程句柄
    /// @details 持有协程帧的所有权。协程帧包含：
    ///          - Promise 对象（包含返回值/异常）
    ///          - 协程的局部变量
    ///          - 协程的执行状态（挂起点）
    ///          Task 移动时会转移此句柄的所有权，析构时会销毁协程帧。
    /// @ownership Task 独占所有权（RAII）
    std::coroutine_handle<promise_type> coro_;
};//end class Task

// ============================================================================
// Promise 的 get_return_object 实现
// ============================================================================
namespace detail {

template <typename T>
Task<T> TaskPromise<T>::get_return_object() noexcept {
    return Task<T>{
        std::coroutine_handle<TaskPromise<T>>::from_promise(*this)};
}

inline Task<void> TaskPromise<void>::get_return_object() noexcept {
    return Task<void>{
        std::coroutine_handle<TaskPromise<void>>::from_promise(*this)};
}

template <typename T>
Task<T&> TaskPromise<T&>::get_return_object() noexcept {
    return Task<T&>{
        std::coroutine_handle<TaskPromise<T&>>::from_promise(*this)};
}

} // namespace detail

} // namespace zlcoro
