#pragma once

#include "optimized_connection.hpp"
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
// 优化版 KV 服务器 - 目标百万 QPS
// =============================================================================

// 请求/响应头
struct alignas(8) OptimizedKVRequestHeader {
    uint8_t op;
    uint8_t reserved;
    uint16_t key_len;
    uint32_t value_len;
};

struct alignas(8) OptimizedKVResponseHeader {
    uint8_t status;
    uint8_t reserved;
    uint16_t reserved2;
    uint32_t value_len;
};

// =============================================================================
// OptimizedEpollKVServer
// =============================================================================

class OptimizedEpollKVServer {
public:
    using ConnectionHandler = std::function<Task<void>(OptimizedEpollConnection&)>;
    
    explicit OptimizedEpollKVServer(size_t num_cores = 0)
        : num_cores_(num_cores > 0 ? num_cores : std::thread::hardware_concurrency())
        , listen_fd_(-1)
        , running_(false)
        , total_connections_(0)
        , total_requests_(0) {
        
        for (size_t i = 0; i < num_cores_; ++i) {
            loops_.push_back(std::make_unique<EpollPerCoreEventLoop>());
            managers_.push_back(std::make_unique<OptimizedEpollManager>(*loops_.back()));
        }
    }
    
    ~OptimizedEpollKVServer() {
        stop();
    }
    
    bool listen(uint16_t port, const std::string& ip = "0.0.0.0", int backlog = 65535) {
        listen_fd_ = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (listen_fd_ < 0) return false;
        
        int opt = 1;
        setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
        setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));
        
        // TCP Fast Open
        int qlen = 128;
        setsockopt(listen_fd_, SOL_TCP, TCP_FASTOPEN, &qlen, sizeof(qlen));
        
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
    
    Task<void> accept_coroutine(size_t core_index) {
        auto& loop = static_cast<EpollPerCoreEventLoop&>(*loops_[core_index]);
        auto& mgr = *managers_[core_index];  // 使用本 core 的 manager
        
        while (running_) {
            co_await EpollReadAwaiter{loop, listen_fd_};
            
            while (running_) {
                struct sockaddr_in client_addr{};
                socklen_t addr_len = sizeof(client_addr);
                
                int client_fd = accept4(listen_fd_,
                                       (struct sockaddr*)&client_addr,
                                       &addr_len,
                                       SOCK_NONBLOCK | SOCK_CLOEXEC);
                
                if (client_fd < 0) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK) {
                        break;
                    }
                    continue;
                }
                
                total_connections_++;
                
                // 直接在本 core 处理连接
                spawn_connection_coroutine(client_fd, loop, mgr);
            }
        }
    }
    
    void spawn_connection_coroutine(int client_fd, EpollPerCoreEventLoop& loop, OptimizedEpollManager& mgr) {
        auto conn_task = connection_coroutine(client_fd, mgr);
        auto handle = conn_task.handle();
        if (handle && !handle.done()) {
            handle.resume();
        }
        conn_task.release();
    }
    
    Task<void> connection_coroutine(int client_fd, OptimizedEpollManager& mgr) {
        OptimizedEpollConnection conn(client_fd, mgr);
        co_await handler_(conn);
        total_requests_.fetch_add(conn.total_ops());
    }
    
private:
    size_t num_cores_;
    int listen_fd_;
    std::atomic<bool> running_;
    std::atomic<uint64_t> total_connections_;
    std::atomic<uint64_t> total_requests_;
    
    std::vector<std::unique_ptr<EpollPerCoreEventLoop>> loops_;
    std::vector<std::unique_ptr<OptimizedEpollManager>> managers_;
    std::vector<std::thread> threads_;
    ConnectionHandler handler_;
};

// =============================================================================
// OptimizedIoUringKVServer
// =============================================================================

#ifdef ZLCORO_HAS_IO_URING

class OptimizedIoUringKVServer {
public:
    using ConnectionHandler = std::function<Task<void>(OptimizedIoUringConnection&)>;
    
    explicit OptimizedIoUringKVServer(size_t num_cores = 0, unsigned queue_depth = 512)
        : num_cores_(num_cores > 0 ? num_cores : std::thread::hardware_concurrency())
        , queue_depth_(queue_depth)
        , listen_fd_(-1)
        , running_(false)
        , total_connections_(0)
        , total_requests_(0) {
        
        for (size_t i = 0; i < num_cores_; ++i) {
            loops_.push_back(std::make_unique<IoUringPerCoreEventLoop>(queue_depth));
            managers_.push_back(std::make_unique<OptimizedIoUringManager>(*loops_.back()));
        }
    }
    
    ~OptimizedIoUringKVServer() {
        stop();
    }
    
    bool listen(uint16_t port, const std::string& ip = "0.0.0.0", int backlog = 65535) {
        listen_fd_ = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (listen_fd_ < 0) return false;
        
        int opt = 1;
        setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
        setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));
        
        int qlen = 128;
        setsockopt(listen_fd_, SOL_TCP, TCP_FASTOPEN, &qlen, sizeof(qlen));
        
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
    
    Task<void> accept_coroutine(size_t core_index) {
        auto& loop = *loops_[core_index];
        auto& mgr = *managers_[core_index];  // 使用本 core 的 manager
        
        while (running_) {
            struct sockaddr_in client_addr{};
            socklen_t addr_len = sizeof(client_addr);
            
            auto req = loop.prep_accept(listen_fd_,
                                       (struct sockaddr*)&client_addr,
                                       &addr_len);
            loop.submit();
            
            int client_fd = co_await make_awaiter(req);
            
            if (client_fd < 0) {
                if (!running_) break;
                continue;
            }
            
            int flags = fcntl(client_fd, F_GETFL, 0);
            fcntl(client_fd, F_SETFL, flags | O_NONBLOCK);
            
            total_connections_++;
            
            // 直接在本 core 处理连接
            spawn_connection_coroutine(client_fd, loop, mgr);
        }
    }
    
    void spawn_connection_coroutine(int client_fd, IoUringPerCoreEventLoop& loop, OptimizedIoUringManager& mgr) {
        auto conn_task = connection_coroutine(client_fd, mgr);
        auto handle = conn_task.handle();
        if (handle && !handle.done()) {
            handle.resume();
        }
        conn_task.release();
    }
    
    Task<void> connection_coroutine(int client_fd, OptimizedIoUringManager& mgr) {
        OptimizedIoUringConnection conn(client_fd, mgr);
        co_await handler_(conn);
        total_requests_.fetch_add(conn.total_ops());
    }
    
private:
    size_t num_cores_;
    unsigned queue_depth_;
    int listen_fd_;
    std::atomic<bool> running_;
    std::atomic<uint64_t> total_connections_;
    std::atomic<uint64_t> total_requests_;
    
    std::vector<std::unique_ptr<IoUringPerCoreEventLoop>> loops_;
    std::vector<std::unique_ptr<OptimizedIoUringManager>> managers_;
    std::vector<std::thread> threads_;
    ConnectionHandler handler_;
};

#endif // ZLCORO_HAS_IO_URING

} // namespace zlcoro
