#pragma once

#include "zlcoro/scheduler/scheduler.hpp"
#include <coroutine>
#include <atomic>
#include <mutex>
#include <vector>

namespace zlcoro {

// =============================================================================
// WaitGroup - 协程等待组，当任务完成时，主动唤醒所有等待任务完成的协程
// =============================================================================
//
// 用于等待一组协程完成，类似 Go 的 sync.WaitGroup
// 
// 特性:
// - 计数器机制
// - 协程友好的等待
// - 线程安全
//
// 使用示例:
//   WaitGroup wg;
//   wg.add(3);
//   
//   // 启动 3 个协程
//   for (int i = 0; i < 3; ++i) {
//       async_run([&wg]() -> Task<void> {
//           // 做一些工作
//           wg.done();
//           co_return;
//       }());
//   }
//   
//   co_await wg.wait();  // 等待所有协程完成
// =============================================================================

class WaitGroup {
public:
    WaitGroup() : count_(0) {}

    ~WaitGroup() = default;

    // 禁止拷贝和移动
    WaitGroup(const WaitGroup&) = delete;
    WaitGroup& operator=(const WaitGroup&) = delete;
    WaitGroup(WaitGroup&&) = delete;
    WaitGroup& operator=(WaitGroup&&) = delete;

    // Wait Awaiter
    struct WaitAwaiter {
        WaitGroup* wg_;

        explicit WaitAwaiter(WaitGroup* wg) : wg_(wg) {}

        bool await_ready() const noexcept {
            // 快速路径：如果计数已经为 0，无需挂起
            return wg_->count_.load(std::memory_order_acquire) == 0;
        }

        bool await_suspend(std::coroutine_handle<> handle) {
            std::lock_guard lock(wg_->mutex_);
            
            // 在持有锁的情况下再次检查
            // 这里使用 relaxed 因为我们已经在锁内
            if (wg_->count_.load(std::memory_order_relaxed) == 0) {
                return false;  // 不挂起
            }
            
            wg_->waiters_.push_back(handle);
            return true;  // 挂起
        }

        void await_resume() {}
    };

    // 增加计数器
    void add(int delta = 1) {
        if (delta <= 0) {
            return;
        }
        count_.fetch_add(delta);
    }

    // 减少计数器
    void done() {
        // 使用 CAS 循环来防止计数变为负数
        int old_count = count_.load(std::memory_order_relaxed);
        while (old_count > 0) {
            if (count_.compare_exchange_weak(old_count, old_count - 1,
                                             std::memory_order_release,
                                             std::memory_order_relaxed)) {
                // 成功减少计数
                if (old_count == 1) {
                    // 计数器归零，唤醒所有等待者
                    std::vector<std::coroutine_handle<>> to_resume;
                    
                    {
                        std::lock_guard lock(mutex_);
                        to_resume = std::move(waiters_);
                        waiters_.clear();
                    }
                    
                    // 在锁外使用调度器批量恢复所有等待者
                    for (auto handle : to_resume) {
                        if (handle && !handle.done()) {  // 检查句柄有效性
                            Scheduler::instance().schedule(handle);
                        }
                    }
                }
                return;
            }
            // compare_exchange_weak 失败，old_count 已被更新，继续循环
        }
        // 如果 old_count <= 0，忽略这次 done() 调用（防御性编程）
    }

    // 等待计数器归零
    WaitAwaiter wait() {
        return WaitAwaiter(this);
    }

    // 获取当前计数
    int count() const {
        return count_.load();
    }

private:
    // =========================================================================
    // 数据成员
    // =========================================================================
    
    /// @brief 任务计数器（原子）
    /// @details 表示剩余需要完成的任务数量。
    ///          add() 增加计数，done() 减少计数。
    ///          当计数归零时，所有等待的协程会被唤醒。
    /// @thread_safety 原子操作保证线程安全
    std::atomic<int> count_;
    
    /// @brief 等待者列表
    /// @details 调用 wait() 且计数非零的协程会加入此列表。
    ///          当计数归零时，列表中的所有协程会被批量唤醒。
    ///          使用 vector 而非 queue 是因为需要一次性唤醒所有等待者。
    /// @thread_safety 由 mutex_ 保护
    std::vector<std::coroutine_handle<>> waiters_;
    
    /// @brief 互斥锁
    /// @details 保护 waiters_ 的并发访问。
    ///          注意：count_ 使用原子操作，不需要此锁保护。
    std::mutex mutex_;
};

}  // namespace zlcoro
