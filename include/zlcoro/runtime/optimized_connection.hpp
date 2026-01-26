#pragma once

#include "per_core_event_loop.hpp"
#include "epoll_per_core.hpp"

#ifdef __linux__
#include <linux/version.h>
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 1, 0)
#ifndef ZLCORO_HAS_IO_URING
#define ZLCORO_HAS_IO_URING 1
#endif
#endif
#endif

#ifdef ZLCORO_HAS_IO_URING
#include "io_uring_per_core.hpp"
#endif

#include "zlcoro/core/task.hpp"
#include "zlcoro/utils/memory_pool.hpp"

#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <cstring>
#include <string>
#include <functional>
#include <memory>
#include <unordered_map>
#include <atomic>
#include <optional>

namespace zlcoro {

// =============================================================================
// 优化版连接管理器 - 为百万 QPS 设计
// =============================================================================
//
// 优化点：
// 1. 使用 ThreadLocalPool 减少内存分配开销
// 2. 批量处理 I/O 操作
// 3. 减少系统调用次数
// 4. 优化协程调度
// =============================================================================

// 连接状态（轻量级，无动态分配）
struct OptimizedConnectionState {
    int fd;
    std::coroutine_handle<> waiting_coro;
    bool active;
    
    // 批量 I/O 支持
    void* pending_read_buf = nullptr;
    size_t pending_read_len = 0;
    ssize_t last_read_result = 0;
    
    void* pending_write_buf = nullptr;
    size_t pending_write_len = 0;
    ssize_t last_write_result = 0;
    
    // 统计
    uint64_t total_bytes_read = 0;
    uint64_t total_bytes_written = 0;
    uint64_t total_ops = 0;
    
    OptimizedConnectionState(int fd_) 
        : fd(fd_), waiting_coro(nullptr), active(true) {}
    
    void reset() {
        waiting_coro = nullptr;
        pending_read_buf = nullptr;
        pending_read_len = 0;
        pending_write_buf = nullptr;
        pending_write_len = 0;
        last_read_result = 0;
        last_write_result = 0;
    }
};

// =============================================================================
// OptimizedEpollManager - 优化的 epoll 连接管理器
// =============================================================================

class OptimizedEpollManager {
public:
    explicit OptimizedEpollManager(EpollPerCoreEventLoop& loop)
        : loop_(loop) {}  // ThreadLocalPool 默认构造
    
    ~OptimizedEpollManager() {
        for (auto& [fd, state] : connections_) {
            if (state->active) {
                loop_.unregister(fd);
                ::close(fd);
            }
            state_pool_.release(state);
        }
    }
    
    // 注册连接（使用对象池）
    OptimizedConnectionState* register_connection(int fd) {
        // 设置非阻塞 + TCP_NODELAY
        int flags = fcntl(fd, F_GETFL, 0);
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
        
        int opt = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));
        
        // 从池中获取状态对象
        auto* state = state_pool_.acquire(fd);
        connections_[fd] = state;
        
        return state;
    }
    
    void unregister_connection(int fd) {
        auto it = connections_.find(fd);
        if (it != connections_.end()) {
            it->second->active = false;
            state_pool_.release(it->second);
            connections_.erase(it);
        }
    }
    
    size_t active_count() const { return connections_.size(); }
    
    // =========================================================================
    // 优化的异步 I/O - 减少系统调用
    // =========================================================================
    
    // 读等待器
    struct FastReadAwaiter {
        OptimizedEpollManager& mgr;
        OptimizedConnectionState* state;
        void* buf;
        size_t len;
        
        bool await_ready() noexcept {
            // 先尝试非阻塞读取
            ssize_t n = ::recv(state->fd, buf, len, MSG_DONTWAIT);
            if (n >= 0) {
                state->last_read_result = n;
                state->total_bytes_read += n;
                state->total_ops++;
                return true;  // 立即返回，无需挂起
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return false;  // 需要等待
            }
            state->last_read_result = -1;
            return true;  // 错误，立即返回
        }
        
        void await_suspend(std::coroutine_handle<> coro) noexcept {
            state->waiting_coro = coro;
            state->pending_read_buf = buf;
            state->pending_read_len = len;
            mgr.loop_.register_read(state->fd, coro);
        }
        
        ssize_t await_resume() noexcept {
            if (state->last_read_result != 0) {
                return state->last_read_result;
            }
            
            // 事件就绪后再次读取
            ssize_t n = ::recv(state->fd, buf, len, MSG_DONTWAIT);
            state->waiting_coro = nullptr;
            
            if (n >= 0) {
                state->total_bytes_read += n;
                state->total_ops++;
            }
            return n;
        }
    };
    
    // 写等待器
    struct FastWriteAwaiter {
        OptimizedEpollManager& mgr;
        OptimizedConnectionState* state;
        const void* buf;
        size_t len;
        
        bool await_ready() noexcept {
            // 先尝试非阻塞写入
            ssize_t n = ::send(state->fd, buf, len, MSG_DONTWAIT | MSG_NOSIGNAL);
            if (n >= 0) {
                state->last_write_result = n;
                state->total_bytes_written += n;
                state->total_ops++;
                return true;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return false;
            }
            state->last_write_result = -1;
            return true;
        }
        
        void await_suspend(std::coroutine_handle<> coro) noexcept {
            state->waiting_coro = coro;
            mgr.loop_.register_write(state->fd, coro);
        }
        
        ssize_t await_resume() noexcept {
            if (state->last_write_result != 0) {
                return state->last_write_result;
            }
            
            ssize_t n = ::send(state->fd, buf, len, MSG_DONTWAIT | MSG_NOSIGNAL);
            state->waiting_coro = nullptr;
            
            if (n >= 0) {
                state->total_bytes_written += n;
                state->total_ops++;
            }
            return n;
        }
    };
    
    // 异步读取
    Task<ssize_t> async_read(OptimizedConnectionState* state, void* buf, size_t len) {
        state->last_read_result = 0;
        co_return co_await FastReadAwaiter{*this, state, buf, len};
    }
    
    // 异步写入
    Task<ssize_t> async_write(OptimizedConnectionState* state, const void* buf, size_t len) {
        state->last_write_result = 0;
        co_return co_await FastWriteAwaiter{*this, state, buf, len};
    }
    
    // 完整读取
    Task<ssize_t> async_read_exact(OptimizedConnectionState* state, void* buf, size_t len) {
        size_t total = 0;
        char* ptr = static_cast<char*>(buf);
        
        while (total < len) {
            ssize_t n = co_await async_read(state, ptr + total, len - total);
            if (n <= 0) {
                co_return (total > 0) ? static_cast<ssize_t>(total) : n;
            }
            total += n;
        }
        co_return static_cast<ssize_t>(total);
    }
    
    // 完整写入
    Task<ssize_t> async_write_all(OptimizedConnectionState* state, const void* buf, size_t len) {
        size_t total = 0;
        const char* ptr = static_cast<const char*>(buf);
        
        while (total < len) {
            ssize_t n = co_await async_write(state, ptr + total, len - total);
            if (n <= 0) {
                co_return (total > 0) ? static_cast<ssize_t>(total) : n;
            }
            total += n;
        }
        co_return static_cast<ssize_t>(total);
    }
    
    EpollPerCoreEventLoop& loop() { return loop_; }
    
private:
    EpollPerCoreEventLoop& loop_;
    ThreadLocalPool<OptimizedConnectionState> state_pool_;
    std::unordered_map<int, OptimizedConnectionState*> connections_;
};

// =============================================================================
// OptimizedIoUringManager - 优化的 io_uring 连接管理器
// =============================================================================

#ifdef ZLCORO_HAS_IO_URING

class OptimizedIoUringManager {
public:
    // 批量大小
    static constexpr size_t BATCH_SIZE = 64;
    
    explicit OptimizedIoUringManager(IoUringPerCoreEventLoop& loop)
        : loop_(loop)
        , pending_submits_(0) {}  // ThreadLocalPool 默认构造
    
    ~OptimizedIoUringManager() {
        for (auto& [fd, state] : connections_) {
            if (state->active) {
                ::close(fd);
            }
            state_pool_.release(state);
        }
    }
    
    OptimizedConnectionState* register_connection(int fd) {
        int flags = fcntl(fd, F_GETFL, 0);
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
        
        int opt = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));
        
        auto* state = state_pool_.acquire(fd);
        connections_[fd] = state;
        
        return state;
    }
    
    void unregister_connection(int fd) {
        auto it = connections_.find(fd);
        if (it != connections_.end()) {
            it->second->active = false;
            state_pool_.release(it->second);
            connections_.erase(it);
        }
    }
    
    size_t active_count() const { return connections_.size(); }
    
    // =========================================================================
    // 批量提交优化
    // =========================================================================
    
    // 强制提交所有待处理操作
    void flush() {
        if (pending_submits_ > 0) {
            loop_.submit();
            pending_submits_ = 0;
        }
    }
    
    // 检查是否需要自动提交
    void maybe_submit() {
        if (pending_submits_ >= BATCH_SIZE) {
            flush();
        }
    }
    
    // =========================================================================
    // 优化的异步 I/O
    // =========================================================================
    
    Task<ssize_t> async_read(OptimizedConnectionState* state, void* buf, size_t len) {
        // 使用 io_uring 的 prep_recv
        auto req = loop_.prep_recv(state->fd, buf, len, 0);
        loop_.submit();  // 立即提交，避免批量导致的延迟
        
        int result = co_await make_awaiter(req);
        
        if (result > 0) {
            state->total_bytes_read += result;
            state->total_ops++;
        }
        
        co_return result;
    }
    
    Task<ssize_t> async_write(OptimizedConnectionState* state, const void* buf, size_t len) {
        auto req = loop_.prep_send(state->fd, buf, len, MSG_NOSIGNAL);
        loop_.submit();  // 立即提交，避免批量导致的延迟
        
        int result = co_await make_awaiter(req);
        
        if (result > 0) {
            state->total_bytes_written += result;
            state->total_ops++;
        }
        
        co_return result;
    }
    
    Task<ssize_t> async_read_exact(OptimizedConnectionState* state, void* buf, size_t len) {
        size_t total = 0;
        char* ptr = static_cast<char*>(buf);
        
        while (total < len) {
            ssize_t n = co_await async_read(state, ptr + total, len - total);
            if (n <= 0) {
                co_return (total > 0) ? static_cast<ssize_t>(total) : n;
            }
            total += n;
        }
        co_return static_cast<ssize_t>(total);
    }
    
    Task<ssize_t> async_write_all(OptimizedConnectionState* state, const void* buf, size_t len) {
        size_t total = 0;
        const char* ptr = static_cast<const char*>(buf);
        
        while (total < len) {
            ssize_t n = co_await async_write(state, ptr + total, len - total);
            if (n <= 0) {
                co_return (total > 0) ? static_cast<ssize_t>(total) : n;
            }
            total += n;
        }
        co_return static_cast<ssize_t>(total);
    }
    
    IoUringPerCoreEventLoop& loop() { return loop_; }
    
private:
    IoUringPerCoreEventLoop& loop_;
    ThreadLocalPool<OptimizedConnectionState> state_pool_;
    std::unordered_map<int, OptimizedConnectionState*> connections_;
    size_t pending_submits_;
};

#endif // ZLCORO_HAS_IO_URING

// =============================================================================
// OptimizedKVConnection - 优化的 KV 连接
// =============================================================================

template<typename Manager>
class OptimizedKVConnection {
public:
    OptimizedKVConnection(int fd, Manager& mgr)
        : state_(mgr.register_connection(fd))
        , mgr_(mgr) {}
    
    ~OptimizedKVConnection() {
        if (state_ && state_->active) {
            mgr_.unregister_connection(state_->fd);
            ::close(state_->fd);
        }
    }
    
    // 禁止拷贝
    OptimizedKVConnection(const OptimizedKVConnection&) = delete;
    OptimizedKVConnection& operator=(const OptimizedKVConnection&) = delete;
    
    // 移动构造
    OptimizedKVConnection(OptimizedKVConnection&& other) noexcept
        : state_(other.state_), mgr_(other.mgr_) {
        other.state_ = nullptr;
    }
    
    // 单次异步读取（读取可用数据，不等待填满）
    Task<ssize_t> read(void* buf, size_t len) {
        co_return co_await mgr_.async_read(state_, buf, len);
    }
    
    // 完整读取（读取直到填满 len 字节）
    Task<ssize_t> read_exact(void* buf, size_t len) {
        co_return co_await mgr_.async_read_exact(state_, buf, len);
    }
    
    // 异步写入（保证写完所有数据）
    Task<ssize_t> write(const void* buf, size_t len) {
        co_return co_await mgr_.async_write_all(state_, buf, len);
    }
    
    bool is_valid() const { return state_ && state_->active; }
    int fd() const { return state_ ? state_->fd : -1; }
    uint64_t total_ops() const { return state_ ? state_->total_ops : 0; }
    
private:
    OptimizedConnectionState* state_;
    Manager& mgr_;
};

using OptimizedEpollConnection = OptimizedKVConnection<OptimizedEpollManager>;

#ifdef ZLCORO_HAS_IO_URING
using OptimizedIoUringConnection = OptimizedKVConnection<OptimizedIoUringManager>;
#endif

} // namespace zlcoro
