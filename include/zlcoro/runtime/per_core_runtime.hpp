#pragma once

#include "per_core_event_loop.hpp"
#include "epoll_per_core.hpp"

#ifdef ZLCORO_HAS_IO_URING
#include "io_uring_per_core.hpp"
#endif

#include <vector>
#include <thread>
#include <memory>
#include <functional>
#include <atomic>
#include <mutex>
#include <condition_variable>

namespace zlcoro {

// =============================================================================
// PerCoreRuntime - 多核运行时管理器
// =============================================================================
//
// 设计理念：
// - 管理多个 PerCoreEventLoop，每个核心一个
// - 支持 epoll 和 io_uring 两种后端
// - 提供连接分发和负载均衡
// - 完全无锁的核心内操作
//
// 使用场景：
// - 高性能网络服务器
// - KV 存储系统
// - 需要极低延迟的应用
// =============================================================================

class PerCoreRuntime {
public:
    // 事件循环后端类型
    using Backend = PerCoreEventLoop::Backend;
    
    // 连接处理函数类型
    using ConnectionHandler = std::function<void(int client_fd, PerCoreEventLoop& loop)>;

    // 配置选项
    struct Config {
        Backend backend;
        size_t num_cores;
        std::vector<int> core_ids;
        unsigned io_uring_queue_depth;
        bool bind_to_core;
        
        Config() 
            : backend(Backend::Epoll)
            , num_cores(0)
            , core_ids()
            , io_uring_queue_depth(256)
            , bind_to_core(true) {}
    };

    // 构造函数
    explicit PerCoreRuntime(const Config& config = Config{}) 
        : config_(config)
        , running_(false)
        , next_core_(0) {
        
        // 确定核心数
        size_t num_cores = config_.num_cores;
        if (num_cores == 0) {
            num_cores = std::thread::hardware_concurrency();
            if (num_cores == 0) num_cores = 1;
        }
        
        // 确定核心 ID
        std::vector<int> core_ids = config_.core_ids;
        if (core_ids.empty()) {
            for (size_t i = 0; i < num_cores; ++i) {
                core_ids.push_back(static_cast<int>(i));
            }
        }
        
        // 创建事件循环
        loops_.reserve(num_cores);
        for (size_t i = 0; i < num_cores; ++i) {
            int core_id = core_ids[i % core_ids.size()];
            loops_.push_back(create_event_loop(core_id));
        }
    }

    ~PerCoreRuntime() {
        stop();
    }

    // 禁止拷贝
    PerCoreRuntime(const PerCoreRuntime&) = delete;
    PerCoreRuntime& operator=(const PerCoreRuntime&) = delete;

    // =========================================================================
    // 生命周期管理
    // =========================================================================

    // 启动所有事件循环
    void start() {
        if (running_.exchange(true)) {
            return;  // 已经在运行
        }
        
        threads_.reserve(loops_.size());
        for (size_t i = 0; i < loops_.size(); ++i) {
            threads_.emplace_back([this, i]() {
                run_loop(i);
            });
        }
    }

    // 停止所有事件循环
    void stop() {
        if (!running_.exchange(false)) {
            return;  // 已经停止
        }
        
        // 停止所有循环
        for (auto& loop : loops_) {
            loop->stop();
        }
        
        // 等待所有线程结束
        for (auto& thread : threads_) {
            if (thread.joinable()) {
                thread.join();
            }
        }
        threads_.clear();
    }

    // 等待所有线程结束
    void wait() {
        for (auto& thread : threads_) {
            if (thread.joinable()) {
                thread.join();
            }
        }
    }

    // 检查是否运行中
    bool is_running() const noexcept { return running_; }

    // =========================================================================
    // 核心访问
    // =========================================================================

    // 获取核心数量
    size_t num_cores() const noexcept { return loops_.size(); }

    // 获取指定核心的事件循环
    PerCoreEventLoop& get_loop(size_t index) {
        return *loops_.at(index);
    }

    // 获取指定核心的事件循环（const 版本）
    const PerCoreEventLoop& get_loop(size_t index) const {
        return *loops_.at(index);
    }

    // =========================================================================
    // 负载均衡
    // =========================================================================

    // 轮询选择下一个核心
    size_t next_core_round_robin() {
        return next_core_.fetch_add(1, std::memory_order_relaxed) % loops_.size();
    }

    // 基于 fd 哈希选择核心（同一连接总是分配到同一核心）
    size_t select_core_by_fd(int fd) const {
        return static_cast<size_t>(fd) % loops_.size();
    }

    // 基于客户端地址哈希选择核心
    size_t select_core_by_addr(uint32_t ip, uint16_t port) const {
        size_t hash = static_cast<size_t>(ip) ^ (static_cast<size_t>(port) << 16);
        return hash % loops_.size();
    }

    // =========================================================================
    // 任务分发
    // =========================================================================

    // 在指定核心上调度协程
    void schedule_on(size_t core_index, std::coroutine_handle<> coro) {
        if (core_index < loops_.size()) {
            loops_[core_index]->schedule(coro);
        }
    }

    // 在指定核心上执行任务
    template<typename Func>
    void post_to(size_t core_index, Func&& func) {
        if (core_index < loops_.size()) {
            loops_[core_index]->post(std::forward<Func>(func));
        }
    }

    // 在轮询选择的核心上执行任务
    template<typename Func>
    void post(Func&& func) {
        post_to(next_core_round_robin(), std::forward<Func>(func));
    }

    // =========================================================================
    // 类型转换（用于访问特定后端功能）
    // =========================================================================

#ifdef ZLCORO_HAS_IO_URING
    // 获取 io_uring 事件循环（如果后端是 io_uring）
    IoUringPerCoreEventLoop* get_io_uring_loop(size_t index) {
        if (config_.backend == Backend::IoUring && index < loops_.size()) {
            return static_cast<IoUringPerCoreEventLoop*>(loops_[index].get());
        }
        return nullptr;
    }
#endif

    // 获取 epoll 事件循环（如果后端是 epoll）
    EpollPerCoreEventLoop* get_epoll_loop(size_t index) {
        if (config_.backend == Backend::Epoll && index < loops_.size()) {
            return static_cast<EpollPerCoreEventLoop*>(loops_[index].get());
        }
        return nullptr;
    }

    // =========================================================================
    // 后端信息
    // =========================================================================

    Backend backend() const noexcept { return config_.backend; }

private:
    // 创建事件循环
    std::unique_ptr<PerCoreEventLoop> create_event_loop(int core_id) {
        std::unique_ptr<PerCoreEventLoop> loop;
        
#ifdef ZLCORO_HAS_IO_URING
        if (config_.backend == Backend::IoUring) {
            loop = std::make_unique<IoUringPerCoreEventLoop>(
                config_.io_uring_queue_depth);
        } else
#endif
        {
            loop = std::make_unique<EpollPerCoreEventLoop>();
        }
        
        if (config_.bind_to_core) {
            loop->bind_to_core(core_id);
        }
        
        return loop;
    }

    // 运行事件循环
    void run_loop(size_t index) {
        auto& loop = loops_[index];
        
        // 设置当前线程的事件循环
        set_current_event_loop(loop.get());
        
        // 运行事件循环
        loop->run();
        
        // 清除当前线程的事件循环
        set_current_event_loop(nullptr);
    }

private:
    Config config_;
    std::vector<std::unique_ptr<PerCoreEventLoop>> loops_;
    std::vector<std::thread> threads_;
    std::atomic<bool> running_;
    std::atomic<size_t> next_core_;
};

// =============================================================================
// 便捷函数：创建运行时
// =============================================================================

// 创建 epoll 运行时
inline std::unique_ptr<PerCoreRuntime> make_epoll_runtime(size_t num_cores = 0) {
    PerCoreRuntime::Config config;
    config.backend = PerCoreEventLoop::Backend::Epoll;
    config.num_cores = num_cores;
    return std::make_unique<PerCoreRuntime>(config);
}

#ifdef ZLCORO_HAS_IO_URING
// 创建 io_uring 运行时
inline std::unique_ptr<PerCoreRuntime> make_io_uring_runtime(
    size_t num_cores = 0, 
    unsigned queue_depth = 256) {
    
    PerCoreRuntime::Config config;
    config.backend = PerCoreEventLoop::Backend::IoUring;
    config.num_cores = num_cores;
    config.io_uring_queue_depth = queue_depth;
    return std::make_unique<PerCoreRuntime>(config);
}
#endif

} // namespace zlcoro
