#pragma once

#include <atomic>
#include <coroutine>
#include <memory>
#include <vector>
#include <mutex>
#include <functional>

namespace zlcoro {

// =============================================================================
// CancellationToken - 协程取消令牌
// =============================================================================
// 
// 用于协作式取消协程，支持：
// - 检查是否已取消
// - 注册取消回调
// - 在协程中等待取消信号
// 
// 使用示例:
//   Task<void> my_task(CancellationToken token) {
//       while (!token.is_cancelled()) {
//           auto result = co_await some_operation();
//           if (token.is_cancelled()) break;
//       }
//   }
// =============================================================================

class CancellationToken;

// CancellationSource - 取消源，用于触发取消
class CancellationSource {
public:
    CancellationSource() 
        : state_(std::make_shared<State>()) {}

    // 禁止拷贝，允许移动
    CancellationSource(const CancellationSource&) = delete;
    CancellationSource& operator=(const CancellationSource&) = delete;
    CancellationSource(CancellationSource&&) = default;
    CancellationSource& operator=(CancellationSource&&) = default;

    // 触发取消
    void cancel() {
        if (!state_) return;
        
        std::vector<std::function<void()>> callbacks;
        
        {
            std::lock_guard<std::mutex> lock(state_->mutex);
            if (state_->cancelled.exchange(true, std::memory_order_release)) {
                return;  // 已经取消过了
            }
            callbacks = std::move(state_->callbacks);
        }
        
        // 执行所有取消回调
        for (auto& cb : callbacks) {
            try {
                cb();
            } catch (...) {
                // 忽略回调中的异常
            }
        }
    }

    // 检查是否已取消
    bool is_cancelled() const noexcept {
        return state_ && state_->cancelled.load(std::memory_order_acquire);
    }

    // 获取关联的 Token
    CancellationToken token() const;

private:
    friend class CancellationToken;

    /// @brief 取消源的共享状态
    /// @details 包含取消标志、回调列表和同步原语。
    ///          使用 shared_ptr 共享，允许 Source 和 Token 独立生命周期。
    struct State {
        /// @brief 取消标志（原子）
        /// @details 一旦设为 true，不可逆转。
        ///          使用 atomic 保证多线程可见性。
        std::atomic<bool> cancelled{false};
        
        /// @brief 保护回调列表的互斥锁
        std::mutex mutex;
        
        /// @brief 取消回调列表
        /// @details cancel() 时依次执行所有回调。
        ///          回调会在 mutex 释放后执行，避免死锁。
        std::vector<std::function<void()>> callbacks;
    };

    /// @brief 共享状态指针
    /// @details 通过 shared_ptr 实现 Source 和 Token 之间的状态共享。
    ///          Source 移动后此指针可能为空。
    std::shared_ptr<State> state_;
};

// CancellationToken - 取消令牌，用于检查和响应取消
class CancellationToken {
public:
    // 默认构造：不可取消的令牌
    CancellationToken() : state_(nullptr) {}

    // 从 CancellationSource 构造
    explicit CancellationToken(std::shared_ptr<CancellationSource::State> state)
        : state_(std::move(state)) {}

    // 检查是否已取消
    bool is_cancelled() const noexcept {
        return state_ && state_->cancelled.load(std::memory_order_acquire);
    }

    // 转换为 bool：true 表示未取消，false 表示已取消
    explicit operator bool() const noexcept {
        return !is_cancelled();
    }

    // 注册取消回调
    // 如果已取消，立即执行回调
    // 返回是否成功注册（已取消返回 false）
    bool on_cancel(std::function<void()> callback) {
        if (!state_) return true;  // 不可取消的令牌
        
        std::lock_guard<std::mutex> lock(state_->mutex);
        
        if (state_->cancelled.load(std::memory_order_acquire)) {
            // 已取消，立即执行
            callback();
            return false;
        }
        
        state_->callbacks.push_back(std::move(callback));
        return true;
    }

    // 如果已取消则抛出异常
    void throw_if_cancelled() const {
        if (is_cancelled()) {
            throw CancelledException();
        }
    }

    // 取消异常
    class CancelledException : public std::exception {
    public:
        const char* what() const noexcept override {
            return "Operation was cancelled";
        }
    };

    // =========================================================================
    // Awaiter - 等待取消信号
    // =========================================================================
    
    struct CancelAwaiter {
        /// @brief 关联的取消令牌
        CancellationToken& token;
        
        /// @brief Awaiter 存活标志（共享指针包装的原子布尔）
        /// @details 用于防止悬垂指针问题：如果 awaiter 已销毁但回调尚未触发，
        ///          回调会检查此标志，避免恢复已失效的协程。
        std::shared_ptr<std::atomic<bool>> awaiter_alive;
        
        CancelAwaiter(CancellationToken& t) 
            : token(t)
            , awaiter_alive(std::make_shared<std::atomic<bool>>(true)) {}
        
        ~CancelAwaiter() {
            // 标记 awaiter 已销毁
            if (awaiter_alive) {
                awaiter_alive->store(false, std::memory_order_release);
            }
        }
        
        // 禁止拷贝
        CancelAwaiter(const CancelAwaiter&) = delete;
        CancelAwaiter& operator=(const CancelAwaiter&) = delete;
        
        // 允许移动
        CancelAwaiter(CancelAwaiter&& other) noexcept
            : token(other.token)
            , awaiter_alive(std::move(other.awaiter_alive)) {}
        
        bool await_ready() const noexcept {
            return token.is_cancelled();
        }
        
        void await_suspend(std::coroutine_handle<> coro) {
            auto alive = awaiter_alive;
            
            // 注册回调：取消时恢复协程
            // 使用弱引用检查 awaiter 是否还活着
            token.on_cancel([coro, alive]() {
                // 只有当 awaiter 还活着时才恢复协程
                if (alive && alive->load(std::memory_order_acquire)) {
                    if (coro && !coro.done()) {
                        coro.resume();
                    }
                }
            });
        }
        
        void await_resume() const {
            // 取消信号已收到
        }
    };

    // 等待取消信号（协程中使用）
    CancelAwaiter wait_for_cancel() {
        return CancelAwaiter{*this};
    }

    // 创建不可取消的令牌
    static CancellationToken none() {
        return CancellationToken{};
    }

private:
    // =========================================================================
    // 数据成员
    // =========================================================================
    
    /// @brief 共享状态指针
    /// @details 与 CancellationSource 共享的状态。
    ///          如果为 nullptr，表示这是一个"不可取消"的令牌。
    ///          多个 Token 可以共享同一个 State。
    std::shared_ptr<CancellationSource::State> state_;
};

// CancellationSource::token() 实现
inline CancellationToken CancellationSource::token() const {
    return CancellationToken(state_);
}

// =============================================================================
// StopToken - 更轻量级的停止令牌（仅检查，无回调）
// =============================================================================

class StopToken {
public:
    StopToken() : cancelled_(std::make_shared<std::atomic<bool>>(false)) {}

    // 检查是否已停止
    bool is_stopped() const noexcept {
        return cancelled_->load(std::memory_order_acquire);
    }

    // 请求停止
    void request_stop() {
        cancelled_->store(true, std::memory_order_release);
    }

    explicit operator bool() const noexcept {
        return !is_stopped();
    }

private:
    std::shared_ptr<std::atomic<bool>> cancelled_;
};

} // namespace zlcoro
