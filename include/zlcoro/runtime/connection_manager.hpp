#pragma once

#include "per_core_event_loop.hpp"
#include "epoll_per_core.hpp"

// 检测 io_uring 支持
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
// ConnectionState - 连接状态（协程与事件的关联）
// =============================================================================
//
// 核心设计理念：
// - 每个连接对应一个协程
// - 协程在需要 I/O 时挂起
// - 事件就绪时找到对应协程并唤醒
// - 支持 epoll 和 io_uring 两种模式
// =============================================================================

struct ConnectionState {
    int fd;                                    // 连接的文件描述符
    std::coroutine_handle<> waiting_coro;      // 当前等待的协程
    uint32_t interest_events;                  // 感兴趣的事件 (POLLIN, POLLOUT)
    bool active;                               // 连接是否活跃
    
    // 统计信息
    uint64_t bytes_read = 0;
    uint64_t bytes_written = 0;
    uint64_t ops_count = 0;
    
    ConnectionState(int fd_) 
        : fd(fd_)
        , waiting_coro(nullptr)
        , interest_events(0)
        , active(true) {}
};

// =============================================================================
// EpollConnectionManager - epoll 模式的连接管理器
// =============================================================================
//
// 工作流程：
// 1. 连接到来 -> 创建 ConnectionState -> 注册到 epoll -> 创建协程并挂起
// 2. 协程需要读/写 -> 注册事件到 epoll -> 协程挂起
// 3. epoll_wait 返回 -> 找到对应的 ConnectionState -> 唤醒协程
// 4. 协程完成操作 -> 继续执行或再次挂起
// =============================================================================

class EpollConnectionManager {
public:
    explicit EpollConnectionManager(EpollPerCoreEventLoop& loop)
        : loop_(loop) {}
    
    ~EpollConnectionManager() {
        // 关闭所有连接
        for (auto& [fd, state] : connections_) {
            if (state->active) {
                loop_.unregister(fd);
                ::close(fd);
            }
        }
    }
    
    // =========================================================================
    // 连接管理
    // =========================================================================
    
    // 注册新连接
    ConnectionState* register_connection(int fd) {
        // 设置非阻塞
        int flags = fcntl(fd, F_GETFL, 0);
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
        
        // 设置 TCP_NODELAY
        int opt = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));
        
        // 创建连接状态
        auto state = std::make_unique<ConnectionState>(fd);
        auto* ptr = state.get();
        connections_[fd] = std::move(state);
        
        return ptr;
    }
    
    // 取消注册连接
    void unregister_connection(int fd) {
        auto it = connections_.find(fd);
        if (it != connections_.end()) {
            it->second->active = false;
            loop_.unregister(fd);
            connections_.erase(it);
        }
    }
    
    // 获取连接状态
    ConnectionState* get_connection(int fd) {
        auto it = connections_.find(fd);
        return (it != connections_.end()) ? it->second.get() : nullptr;
    }
    
    // 获取活跃连接数
    size_t active_count() const {
        return connections_.size();
    }
    
    // =========================================================================
    // 等待事件的 Awaiter（核心：协程挂起/唤醒机制）
    // =========================================================================
    
    // 等待可读事件
    struct ReadAwaiter {
        EpollConnectionManager& mgr;
        ConnectionState* state;
        
        bool await_ready() const noexcept { return false; }
        
        void await_suspend(std::coroutine_handle<> coro) {
            // 保存等待的协程
            state->waiting_coro = coro;
            state->interest_events = EPOLLIN;
            
            // 注册到 epoll，当可读时 epoll_wait 会返回
            // 然后 poll_events 会找到这个 fd 对应的协程并唤醒
            mgr.loop_.register_read(state->fd, coro);
        }
        
        void await_resume() const noexcept {
            state->waiting_coro = nullptr;
        }
    };
    
    // 等待可写事件
    struct WriteAwaiter {
        EpollConnectionManager& mgr;
        ConnectionState* state;
        
        bool await_ready() const noexcept { return false; }
        
        void await_suspend(std::coroutine_handle<> coro) {
            state->waiting_coro = coro;
            state->interest_events = EPOLLOUT;
            mgr.loop_.register_write(state->fd, coro);
        }
        
        void await_resume() const noexcept {
            state->waiting_coro = nullptr;
        }
    };
    
    // 创建读等待器
    ReadAwaiter wait_readable(ConnectionState* state) {
        return ReadAwaiter{*this, state};
    }
    
    // 创建写等待器
    WriteAwaiter wait_writable(ConnectionState* state) {
        return WriteAwaiter{*this, state};
    }
    
    // =========================================================================
    // 异步 I/O 操作
    // =========================================================================
    
    // 异步读取
    Task<ssize_t> async_read(ConnectionState* state, void* buf, size_t len) {
        while (true) {
            ssize_t n = ::recv(state->fd, buf, len, 0);
            if (n >= 0) {
                state->bytes_read += n;
                state->ops_count++;
                co_return n;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // 数据未就绪，等待可读事件
                co_await wait_readable(state);
            } else {
                co_return -1;  // 错误
            }
        }
    }
    
    // 异步写入
    Task<ssize_t> async_write(ConnectionState* state, const void* buf, size_t len) {
        size_t written = 0;
        const char* ptr = static_cast<const char*>(buf);
        
        while (written < len) {
            ssize_t n = ::send(state->fd, ptr + written, len - written, MSG_NOSIGNAL);
            if (n >= 0) {
                written += n;
                state->bytes_written += n;
            } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // 缓冲区满，等待可写事件
                co_await wait_writable(state);
            } else {
                co_return -1;  // 错误
            }
        }
        state->ops_count++;
        co_return static_cast<ssize_t>(written);
    }
    
    // 异步读取精确字节数
    Task<ssize_t> async_read_exact(ConnectionState* state, void* buf, size_t len) {
        size_t total_read = 0;
        char* ptr = static_cast<char*>(buf);
        
        while (total_read < len) {
            ssize_t n = co_await async_read(state, ptr + total_read, len - total_read);
            if (n <= 0) {
                co_return (total_read > 0) ? static_cast<ssize_t>(total_read) : n;
            }
            total_read += n;
        }
        co_return static_cast<ssize_t>(total_read);
    }
    
    EpollPerCoreEventLoop& loop() { return loop_; }
    
private:
    EpollPerCoreEventLoop& loop_;
    std::unordered_map<int, std::unique_ptr<ConnectionState>> connections_;
};

// =============================================================================
// IoUringConnectionManager - io_uring 模式的连接管理器
// =============================================================================
//
// 工作流程：
// 1. 连接到来 -> 创建 ConnectionState -> 创建协程
// 2. 协程需要读/写 -> 创建 SQE 并注册到 io_uring -> 协程挂起
// 3. io_uring CQE 完成 -> 找到对应的请求 -> 唤醒协程
// 4. 协程获取结果 -> 继续执行或再次挂起
// =============================================================================

#ifdef ZLCORO_HAS_IO_URING

class IoUringConnectionManager {
public:
    explicit IoUringConnectionManager(IoUringPerCoreEventLoop& loop)
        : loop_(loop) {}
    
    ~IoUringConnectionManager() {
        for (auto& [fd, state] : connections_) {
            if (state->active) {
                ::close(fd);
            }
        }
    }
    
    // =========================================================================
    // 连接管理
    // =========================================================================
    
    ConnectionState* register_connection(int fd) {
        int flags = fcntl(fd, F_GETFL, 0);
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
        
        int opt = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));
        
        auto state = std::make_unique<ConnectionState>(fd);
        auto* ptr = state.get();
        connections_[fd] = std::move(state);
        
        return ptr;
    }
    
    void unregister_connection(int fd) {
        auto it = connections_.find(fd);
        if (it != connections_.end()) {
            it->second->active = false;
            connections_.erase(it);
        }
    }
    
    ConnectionState* get_connection(int fd) {
        auto it = connections_.find(fd);
        return (it != connections_.end()) ? it->second.get() : nullptr;
    }
    
    size_t active_count() const {
        return connections_.size();
    }
    
    // =========================================================================
    // 异步 I/O 操作（直接使用 IoUringPerCoreEventLoop 的方法）
    // =========================================================================
    
    // 异步读取：使用 io_uring 的 prep_recv
    Task<ssize_t> async_read(ConnectionState* state, void* buf, size_t len) {
        // 使用事件循环的 prep_recv，它会创建 Request 并设置 user_data
        auto req = loop_.prep_recv(state->fd, buf, len, 0);
        loop_.submit();
        
        // 使用 IoUringPerCoreEventLoop 的 awaiter
        int result = co_await make_awaiter(req);
        
        if (result > 0) {
            state->bytes_read += result;
            state->ops_count++;
        }
        
        co_return result;
    }
    
    // 异步写入：使用 io_uring 的 prep_send
    Task<ssize_t> async_write(ConnectionState* state, const void* buf, size_t len) {
        auto req = loop_.prep_send(state->fd, buf, len, MSG_NOSIGNAL);
        loop_.submit();
        
        int result = co_await make_awaiter(req);
        
        if (result > 0) {
            state->bytes_written += result;
            state->ops_count++;
        }
        
        co_return result;
    }
    
    // 异步读取精确字节数
    Task<ssize_t> async_read_exact(ConnectionState* state, void* buf, size_t len) {
        size_t total_read = 0;
        char* ptr = static_cast<char*>(buf);
        
        while (total_read < len) {
            ssize_t n = co_await async_read(state, ptr + total_read, len - total_read);
            if (n <= 0) {
                co_return (total_read > 0) ? static_cast<ssize_t>(total_read) : n;
            }
            total_read += n;
        }
        co_return static_cast<ssize_t>(total_read);
    }
    
    // 异步写入全部数据
    Task<ssize_t> async_write_all(ConnectionState* state, const void* buf, size_t len) {
        size_t total_written = 0;
        const char* ptr = static_cast<const char*>(buf);
        
        while (total_written < len) {
            ssize_t n = co_await async_write(state, ptr + total_written, len - total_written);
            if (n <= 0) {
                co_return (total_written > 0) ? static_cast<ssize_t>(total_written) : n;
            }
            total_written += n;
        }
        co_return static_cast<ssize_t>(total_written);
    }
    
    IoUringPerCoreEventLoop& loop() { return loop_; }
    
private:
    IoUringPerCoreEventLoop& loop_;
    std::unordered_map<int, std::unique_ptr<ConnectionState>> connections_;
};

#endif // ZLCORO_HAS_IO_URING

// =============================================================================
// KVConnection - KV 存储专用连接类
// =============================================================================
//
// 专为 KV 存储设计：
// - 支持请求/响应协议
// - 批量读写优化
// - 零拷贝支持
// =============================================================================

template<typename ConnectionManager>
class KVConnection {
public:
    KVConnection(int fd, ConnectionManager& mgr)
        : state_(mgr.register_connection(fd))
        , mgr_(mgr) {}
    
    ~KVConnection() {
        if (state_ && state_->active) {
            mgr_.unregister_connection(state_->fd);
            ::close(state_->fd);
        }
    }
    
    // 禁止拷贝
    KVConnection(const KVConnection&) = delete;
    KVConnection& operator=(const KVConnection&) = delete;
    
    // 移动
    KVConnection(KVConnection&& other) noexcept
        : state_(other.state_)
        , mgr_(other.mgr_) {
        other.state_ = nullptr;
    }
    
    // =========================================================================
    // KV 协议操作
    // =========================================================================
    
    // 读取 KV 请求头（固定大小）
    Task<bool> read_header(void* header, size_t header_size) {
        ssize_t n = co_await mgr_.async_read_exact(state_, header, header_size);
        co_return n == static_cast<ssize_t>(header_size);
    }
    
    // 读取 KV 值
    Task<ssize_t> read_value(void* buf, size_t len) {
        co_return co_await mgr_.async_read(state_, buf, len);
    }
    
    // 写入 KV 响应
    Task<bool> write_response(const void* data, size_t len) {
        ssize_t n = co_await mgr_.async_write(state_, data, len);
        co_return n == static_cast<ssize_t>(len);
    }
    
    // 获取统计信息
    uint64_t bytes_read() const { return state_ ? state_->bytes_read : 0; }
    uint64_t bytes_written() const { return state_ ? state_->bytes_written : 0; }
    uint64_t ops_count() const { return state_ ? state_->ops_count : 0; }
    
    bool is_valid() const { return state_ && state_->active; }
    int fd() const { return state_ ? state_->fd : -1; }
    
private:
    ConnectionState* state_;
    ConnectionManager& mgr_;
};

// 类型别名
using EpollKVConnection = KVConnection<EpollConnectionManager>;

#ifdef ZLCORO_HAS_IO_URING
using IoUringKVConnection = KVConnection<IoUringConnectionManager>;
#endif

} // namespace zlcoro
