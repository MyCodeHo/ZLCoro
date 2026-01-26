#pragma once

#include "connection_manager.hpp"
#include "per_core_runtime.hpp"

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
#include <atomic>
#include <thread>
#include <vector>

namespace zlcoro {

// =============================================================================
// KVServer - 高性能 KV 存储服务器
// =============================================================================
//
// 设计特点：
// - 每个 CPU 核心运行一个事件循环
// - 每个连接对应一个协程
// - epoll 模式：事件就绪时唤醒协程
// - io_uring 模式：CQE 完成时唤醒协程
// - 支持高并发、高压力场景
// =============================================================================

// KV 请求头
struct KVRequestHeader {
    uint8_t op;           // 操作类型: 0=GET, 1=SET, 2=DEL
    uint8_t reserved;
    uint16_t key_len;     // key 长度
    uint32_t value_len;   // value 长度（仅 SET 有效）
} __attribute__((packed));

// KV 响应头
struct KVResponseHeader {
    uint8_t status;       // 0=OK, 1=NOT_FOUND, 2=ERROR
    uint8_t reserved;
    uint16_t reserved2;
    uint32_t value_len;   // value 长度
} __attribute__((packed));

// 操作类型
enum class KVOp : uint8_t {
    GET = 0,
    SET = 1,
    DEL = 2,
    PING = 3,
    QUIT = 4
};

// 响应状态
enum class KVStatus : uint8_t {
    OK = 0,
    NOT_FOUND = 1,
    ERROR = 2
};

// =============================================================================
// epoll 模式的 KV 服务器
// =============================================================================

class EpollKVServer {
public:
    // 连接处理器类型
    using ConnectionHandler = std::function<Task<void>(EpollKVConnection&)>;
    
    explicit EpollKVServer(size_t num_cores = 0)
        : num_cores_(num_cores > 0 ? num_cores : std::thread::hardware_concurrency())
        , listen_fd_(-1)
        , running_(false)
        , total_connections_(0)
        , total_requests_(0) {
        
        // 为每个核心创建事件循环和连接管理器
        for (size_t i = 0; i < num_cores_; ++i) {
            loops_.push_back(std::make_unique<EpollPerCoreEventLoop>());
            managers_.push_back(std::make_unique<EpollConnectionManager>(*loops_.back()));
        }
    }
    
    ~EpollKVServer() {
        stop();
    }
    
    // =========================================================================
    // 服务器生命周期
    // =========================================================================
    
    bool listen(uint16_t port, const std::string& ip = "0.0.0.0", int backlog = 1024) {
        listen_fd_ = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (listen_fd_ < 0) return false;
        
        int opt = 1;
        setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
        setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));
        
        struct sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        inet_pton(AF_INET, ip.c_str(), &addr.sin_addr);
        
        if (bind(listen_fd_, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
            ::close(listen_fd_);
            listen_fd_ = -1;
            return false;
        }
        
        if (::listen(listen_fd_, backlog) < 0) {
            ::close(listen_fd_);
            listen_fd_ = -1;
            return false;
        }
        
        return true;
    }
    
    void set_handler(ConnectionHandler handler) {
        handler_ = std::move(handler);
    }
    
    // 启动服务器（多线程）
    void start() {
        if (listen_fd_ < 0 || !handler_) return;
        
        running_ = true;
        
        // 为每个核心启动一个线程
        for (size_t i = 0; i < num_cores_; ++i) {
            threads_.emplace_back([this, i]() {
                run_core(i);
            });
        }
    }
    
    // 等待所有线程结束
    void join() {
        for (auto& t : threads_) {
            if (t.joinable()) {
                t.join();
            }
        }
    }
    
    void stop() {
        running_ = false;
        
        // 先关闭监听 socket，触发所有 accept 协程退出
        if (listen_fd_ >= 0) {
            ::shutdown(listen_fd_, SHUT_RDWR);
            ::close(listen_fd_);
            listen_fd_ = -1;
        }
        
        // 唤醒所有事件循环
        for (auto& loop : loops_) {
            loop->stop();
        }
        
        // 等待线程结束（带超时）
        for (auto& t : threads_) {
            if (t.joinable()) {
                t.join();
            }
        }
        threads_.clear();
    }
    
    // =========================================================================
    // 统计信息
    // =========================================================================
    
    uint64_t total_connections() const { return total_connections_.load(); }
    uint64_t total_requests() const { return total_requests_.load(); }
    size_t num_cores() const { return num_cores_; }
    
    size_t active_connections(size_t core) const {
        if (core < managers_.size()) {
            return managers_[core]->active_count();
        }
        return 0;
    }
    
private:
    // 运行单个核心的事件循环
    void run_core(size_t core_index) {
        auto& loop = *loops_[core_index];
        auto& mgr = *managers_[core_index];
        
        // 绑定到指定核心
        loop.bind_to_core(static_cast<int>(core_index));
        
        // 设置当前线程的事件循环
        set_current_event_loop(&loop);
        
        // 启动接受协程
        start_accept_coroutine(core_index);
        
        // 运行事件循环
        loop.run();
    }
    
    // 启动接受连接的协程
    void start_accept_coroutine(size_t core_index) {
        auto& loop = *loops_[core_index];
        
        // 创建并启动接受协程
        auto accept_task = accept_coroutine(core_index);
        auto handle = accept_task.handle();
        if (handle && !handle.done()) {
            handle.resume();
        }
        accept_task.release();
    }
    
    // 接受连接的协程
    Task<void> accept_coroutine(size_t core_index) {
        auto& loop = static_cast<EpollPerCoreEventLoop&>(*loops_[core_index]);
        
        while (running_) {
            // 等待监听 socket 可读
            co_await EpollReadAwaiter{loop, listen_fd_};
            
            // 接受所有等待的连接
            while (running_) {
                struct sockaddr_in client_addr{};
                socklen_t addr_len = sizeof(client_addr);
                
                int client_fd = accept4(listen_fd_,
                                       (struct sockaddr*)&client_addr,
                                       &addr_len,
                                       SOCK_NONBLOCK | SOCK_CLOEXEC);
                
                if (client_fd < 0) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK) {
                        break;  // 没有更多连接
                    }
                    continue;
                }
                
                total_connections_++;
                
                // 根据 fd 选择处理核心
                size_t target_core = client_fd % num_cores_;
                
                // 在目标核心上启动处理协程
                spawn_connection_coroutine(client_fd, target_core);
            }
        }
    }
    
    // 启动连接处理协程
    void spawn_connection_coroutine(int client_fd, size_t core_index) {
        auto& mgr = *managers_[core_index];
        
        auto conn_task = connection_coroutine(client_fd, mgr);
        auto handle = conn_task.handle();
        if (handle && !handle.done()) {
            handle.resume();
        }
        conn_task.release();
    }
    
    // 连接处理协程
    Task<void> connection_coroutine(int client_fd, EpollConnectionManager& mgr) {
        EpollKVConnection conn(client_fd, mgr);
        co_await handler_(conn);
        total_requests_.fetch_add(conn.ops_count());
    }
    
private:
    size_t num_cores_;
    int listen_fd_;
    std::atomic<bool> running_;
    std::atomic<uint64_t> total_connections_;
    std::atomic<uint64_t> total_requests_;
    
    std::vector<std::unique_ptr<EpollPerCoreEventLoop>> loops_;
    std::vector<std::unique_ptr<EpollConnectionManager>> managers_;
    std::vector<std::thread> threads_;
    ConnectionHandler handler_;
};

// =============================================================================
// io_uring 模式的 KV 服务器
// =============================================================================

#ifdef ZLCORO_HAS_IO_URING

class IoUringKVServer {
public:
    using ConnectionHandler = std::function<Task<void>(IoUringKVConnection&)>;
    
    explicit IoUringKVServer(size_t num_cores = 0, unsigned queue_depth = 256)
        : num_cores_(num_cores > 0 ? num_cores : std::thread::hardware_concurrency())
        , queue_depth_(queue_depth)
        , listen_fd_(-1)
        , running_(false)
        , total_connections_(0)
        , total_requests_(0) {
        
        for (size_t i = 0; i < num_cores_; ++i) {
            loops_.push_back(std::make_unique<IoUringPerCoreEventLoop>(queue_depth));
            managers_.push_back(std::make_unique<IoUringConnectionManager>(*loops_.back()));
        }
    }
    
    ~IoUringKVServer() {
        stop();
    }
    
    bool listen(uint16_t port, const std::string& ip = "0.0.0.0", int backlog = 1024) {
        listen_fd_ = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (listen_fd_ < 0) return false;
        
        int opt = 1;
        setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
        setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));
        
        struct sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        inet_pton(AF_INET, ip.c_str(), &addr.sin_addr);
        
        if (bind(listen_fd_, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
            ::close(listen_fd_);
            listen_fd_ = -1;
            return false;
        }
        
        if (::listen(listen_fd_, backlog) < 0) {
            ::close(listen_fd_);
            listen_fd_ = -1;
            return false;
        }
        
        return true;
    }
    
    void set_handler(ConnectionHandler handler) {
        handler_ = std::move(handler);
    }
    
    void start() {
        if (listen_fd_ < 0 || !handler_) return;
        
        running_ = true;
        
        for (size_t i = 0; i < num_cores_; ++i) {
            threads_.emplace_back([this, i]() {
                run_core(i);
            });
        }
    }
    
    void join() {
        for (auto& t : threads_) {
            if (t.joinable()) {
                t.join();
            }
        }
    }
    
    void stop() {
        running_ = false;
        
        // 先关闭监听 socket
        if (listen_fd_ >= 0) {
            ::shutdown(listen_fd_, SHUT_RDWR);
            ::close(listen_fd_);
            listen_fd_ = -1;
        }
        
        for (auto& loop : loops_) {
            loop->stop();
        }
        
        for (auto& t : threads_) {
            if (t.joinable()) {
                t.join();
            }
        }
        threads_.clear();
    }
    
    uint64_t total_connections() const { return total_connections_.load(); }
    uint64_t total_requests() const { return total_requests_.load(); }
    size_t num_cores() const { return num_cores_; }
    
private:
    void run_core(size_t core_index) {
        auto& loop = *loops_[core_index];
        auto& mgr = *managers_[core_index];
        
        loop.bind_to_core(static_cast<int>(core_index));
        set_current_event_loop(&loop);
        
        start_accept_coroutine(core_index);
        loop.run();
    }
    
    void start_accept_coroutine(size_t core_index) {
        auto accept_task = accept_coroutine(core_index);
        auto handle = accept_task.handle();
        if (handle && !handle.done()) {
            handle.resume();
        }
        accept_task.release();
    }
    
    // io_uring 模式的接受协程
    Task<void> accept_coroutine(size_t core_index) {
        auto& loop = *loops_[core_index];
        
        while (running_) {
            struct sockaddr_in client_addr{};
            socklen_t addr_len = sizeof(client_addr);
            
            // 使用 io_uring 的 accept 操作
            auto req = loop.prep_accept(listen_fd_,
                                       (struct sockaddr*)&client_addr,
                                       &addr_len);
            loop.submit();
            
            int client_fd = co_await make_awaiter(req);
            
            if (client_fd < 0) {
                if (!running_) break;
                continue;
            }
            
            total_connections_++;
            
            // 设置非阻塞
            int flags = fcntl(client_fd, F_GETFL, 0);
            fcntl(client_fd, F_SETFL, flags | O_NONBLOCK);
            
            size_t target_core = client_fd % num_cores_;
            spawn_connection_coroutine(client_fd, target_core);
        }
    }
    
    void spawn_connection_coroutine(int client_fd, size_t core_index) {
        auto& mgr = *managers_[core_index];
        
        auto conn_task = connection_coroutine(client_fd, mgr);
        auto handle = conn_task.handle();
        if (handle && !handle.done()) {
            handle.resume();
        }
        conn_task.release();
    }
    
    Task<void> connection_coroutine(int client_fd, IoUringConnectionManager& mgr) {
        IoUringKVConnection conn(client_fd, mgr);
        co_await handler_(conn);
        total_requests_.fetch_add(conn.ops_count());
    }
    
private:
    size_t num_cores_;
    unsigned queue_depth_;
    int listen_fd_;
    std::atomic<bool> running_;
    std::atomic<uint64_t> total_connections_;
    std::atomic<uint64_t> total_requests_;
    
    std::vector<std::unique_ptr<IoUringPerCoreEventLoop>> loops_;
    std::vector<std::unique_ptr<IoUringConnectionManager>> managers_;
    std::vector<std::thread> threads_;
    ConnectionHandler handler_;
};

#endif // ZLCORO_HAS_IO_URING

} // namespace zlcoro
