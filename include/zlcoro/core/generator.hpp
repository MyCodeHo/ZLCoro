#pragma once

#include <coroutine>
#include <exception>
#include <iterator>
#include <memory>

namespace zlcoro {

// =============================================================================
// Generator<T> - 生成器协程类型
// =============================================================================
// 
// Generator 是一种特殊的协程,用于惰性生成值序列。
// 与 Task<T> 不同,Generator 可以多次产生值(co_yield),而不是只返回一次。
//
// 使用示例:
//   Generator<int> range(int n) {
//       for (int i = 0; i < n; ++i) {
//           co_yield i;  // 生成一个值,然后挂起
//       }
//   }
//
//   for (int x : range(5)) {
//       std::cout << x << "\n";  // 输出 0 1 2 3 4
//   }
//
// 核心特性:
// - 惰性求值: 只在需要时才生成下一个值
// - 迭代器接口: 支持 range-based for 循环
// - 单向迭代: 只能向前,不能回退
// =============================================================================

template <typename T>
class Generator {
public:
    // =========================================================================
    // Promise Type - 定义 Generator 协程的行为
    // =========================================================================
    class promise_type {
    public:
        // 构造时不需要存储值
        promise_type() noexcept : value_ptr_(nullptr) {}

        // 1. 创建 Generator 对象 (编译器调用)
        Generator get_return_object() {
            return Generator{std::coroutine_handle<promise_type>::from_promise(*this)};
        }

        // 2. 初始挂起: 返回 suspend_always
        // Generator 是惰性的,创建时不执行,等调用 begin() 时才开始
        std::suspend_always initial_suspend() noexcept { 
            return {}; 
        }

        // 3. 最终挂起: 返回 suspend_always
        // Generator 结束后保持挂起状态,等待析构
        std::suspend_always final_suspend() noexcept { 
            return {}; 
        }

        // 4. co_yield 的实现
        // 当协程执行 co_yield value 时:
        // - 编译器会调用 yield_value(value)
        // - 我们需要处理两种情况:
        //   a) 左值引用: 直接存储地址 (协程帧中的变量是安全的)
        //   b) 右值引用: 需要移动到 Promise 中存储 (临时对象会被销毁)
        
        // 情况 1: co_yield lvalue (例如: int x = 42; co_yield x;)
        // 左值是协程帧的一部分,生命周期由协程管理,可以安全地存储指针
        std::suspend_always yield_value(const T& value) 
            noexcept(std::is_nothrow_copy_constructible_v<T>) 
            requires std::copy_constructible<T>
        {
            // ⚠️ 重要：如果之前存储了右值，需要先清理
            if (value_ptr_ == std::addressof(stored_value_)) {
                std::destroy_at(std::addressof(stored_value_));
            }
            
            // 对于左值，直接存储指针（协程帧中的变量生命周期安全）
            value_ptr_ = std::addressof(value);
            return {};
        }

        // 情况 2: co_yield rvalue (例如: co_yield 42; 或 co_yield std::string("hello");)
        // 右值可能是临时对象,会在 yield_value 返回后被销毁
        // 必须移动到 Promise 的存储中
        std::suspend_always yield_value(T&& value)
            noexcept(std::is_nothrow_move_constructible_v<T>)
            requires std::move_constructible<T>
        {
            // 销毁旧值(如果有)
            if (value_ptr_ == std::addressof(stored_value_)) {
                std::destroy_at(std::addressof(stored_value_));
            }
            
            // 移动构造新值到 Promise 的存储中
            std::construct_at(std::addressof(stored_value_), std::move(value));
            value_ptr_ = std::addressof(stored_value_);
            return {};
        }

        // 5. Generator 不应该使用 co_return value
        // 如果用了,编译器会报错
        void return_void() noexcept {}

        // 6. 异常处理: 存储异常指针
        void unhandled_exception() {
            exception_ = std::current_exception();
        }

        // 获取当前生成的值
        const T& value() const {
            return *value_ptr_;
        }

        // 如果有异常,重新抛出
        void rethrow_if_exception() {
            if (exception_) {
                std::rethrow_exception(exception_);
            }
        }

        // 析构: 清理存储的值
        ~promise_type() {
            // 如果 value_ptr_ 指向 stored_value_,需要析构
            if (value_ptr_ == std::addressof(stored_value_)) {
                std::destroy_at(std::addressof(stored_value_));
            }
        }

    private:
        // 使用 union 来延迟初始化,避免不必要的构造
        union {
            T stored_value_;        // 存储右值的副本
        };
        const T* value_ptr_;        // 指向当前值(可能指向协程帧或 stored_value_)
        std::exception_ptr exception_;
    };

    // =========================================================================
    // Iterator - 迭代器实现
    // =========================================================================
    // 
    // 这个迭代器允许我们使用 range-based for 循环:
    //   for (auto x : generator) { ... }
    //
    // 工作原理:
    // - operator++: 调用 handle_.resume() 恢复协程,生成下一个值
    // - operator*: 返回当前 yield 的值
    // - operator==: 检查协程是否完成
    // =========================================================================
    class Iterator {
    public:
        // 迭代器的类型定义 (C++ 标准要求)
        using iterator_category = std::input_iterator_tag;  // 单向迭代器
        using value_type = T;
        using difference_type = std::ptrdiff_t;
        using pointer = const T*;
        using reference = const T&;

        // 默认构造: 表示 end() 迭代器
        Iterator() noexcept = default;

        // 从协程句柄构造
        explicit Iterator(std::coroutine_handle<promise_type> handle) noexcept
            : handle_(handle) {}

        // 前置++: 恢复协程,生成下一个值
        Iterator& operator++() {
            if (handle_ && !handle_.done()) {
                handle_.resume();  // 恢复协程执行
                if (handle_.done()) {
                    // 协程完成,检查是否有异常
                    handle_.promise().rethrow_if_exception();
                }
            }
            return *this;
        }

        // 后置++
        void operator++(int) {
            ++(*this);
        }

        // 解引用: 返回当前 yield 的值
        reference operator*() const {
            // 确保句柄有效且未完成
            if (!handle_ || handle_.done()) {
                throw std::runtime_error("Generator iterator out of range");
            }
            return handle_.promise().value();
        }

        pointer operator->() const {
            return std::addressof(operator*());
        }

        // 比较: 两个迭代器相等当且仅当都是 end() 或指向同一个协程
        friend bool operator==(const Iterator& lhs, const Iterator& rhs) noexcept {
            // 如果都是默认构造的(end 迭代器),相等
            if (!lhs.handle_ && !rhs.handle_) {
                return true;
            }
            // 如果一个是 end,另一个不是
            if (!lhs.handle_ || !rhs.handle_) {
                // 检查非 end 的那个是否完成
                auto h = lhs.handle_ ? lhs.handle_ : rhs.handle_;
                return h.done();
            }
            // 都不是 end,比较地址
            return lhs.handle_.address() == rhs.handle_.address();
        }

        friend bool operator!=(const Iterator& lhs, const Iterator& rhs) noexcept {
            return !(lhs == rhs);
        }

    private:
        std::coroutine_handle<promise_type> handle_;
    };

    // =========================================================================
    // Generator 构造和析构
    // =========================================================================

    // 从协程句柄构造
    explicit Generator(std::coroutine_handle<promise_type> handle) noexcept
        : handle_(handle) {}

    // 禁止拷贝
    Generator(const Generator&) = delete;
    Generator& operator=(const Generator&) = delete;

    // 支持移动
    Generator(Generator&& other) noexcept
        : handle_(std::exchange(other.handle_, nullptr)) {
    }

    Generator& operator=(Generator&& other) noexcept {
        if (this != &other) {
            if (handle_) {
                handle_.destroy();
            }
            handle_ = std::exchange(other.handle_, {});
        }
        return *this;
    }

    // 析构: 销毁协程
    ~Generator() {
        if (handle_) {
            handle_.destroy();
        }
    }

    // =========================================================================
    // 迭代器接口 (支持 range-based for 循环)
    // =========================================================================

    // begin(): 开始迭代
    // 第一次调用时,恢复协程执行,直到第一个 co_yield
    Iterator begin() {
        if (handle_) {
            handle_.resume();  // 启动协程,执行到第一个 co_yield
            if (handle_.done()) {
                // 如果立即完成(没有 yield 任何值),检查异常
                // 注意：迭代过程中的异常会在 Iterator::operator++ 中检查
                handle_.promise().rethrow_if_exception();
            }
        }
        return Iterator{handle_};
    }

    // end(): 结束迭代器 (默认构造的 Iterator)
    Iterator end() noexcept {
        return Iterator{};
    }

private:
    std::coroutine_handle<promise_type> handle_;
};

} // namespace zlcoro
