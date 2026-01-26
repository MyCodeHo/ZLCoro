#pragma once

#include "per_core_runtime.hpp"
#include "epoll_per_core.hpp"

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

namespace zlcoro {

// =============================================================================
// PerCoreConnection - Per-Core 架构的 TCP 连接（每连接一个协程）
// =============================================================================

class PerCoreConnection {
public:
    PerCoreConnection(int fd, PerCoreEventLoop& loop)
        : fd_(fd)
        , loop_(loop)
        , closed_(false) {}

    ~PerCoreConnection() {
        close();
    }

    // 禁止拷贝
    PerCoreConnection(const PerCoreConnection&) = delete;
    PerCoreConnection& operator=(const PerCoreConnection&) = delete;

    // 获取 fd
    int fd() const noexcept { return fd_; }

    // 检查是否关闭
    bool is_closed() const noexcept { return closed_; }

    // 关闭连接
    void close() {
        if (!closed_) {
            closed_ = true;
            loop_.unregister(fd_);
            ::close(fd_);
        }
    }

    // =========================================================================
    // epoll 模式的异步操作
    // =========================================================================

    // 异步读取（epoll 模式）
    Task<ssize_t> read_epoll(void* buf, size_t len) {
        while (true) {
            ssize_t n = ::recv(fd_, buf, len, 0);
            if (n >= 0) {
                co_return n;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // 等待可读
                co_await EpollReadAwaiter{
                    static_cast<EpollPerCoreEventLoop&>(loop_), fd_};
            } else {
                co_return -1;
            }
        }
    }

    // 异步写入（epoll 模式）
    Task<ssize_t> write_epoll(const void* buf, size_t len) {
        size_t written = 0;
        const char* ptr = static_cast<const char*>(buf);
        
        while (written < len) {
            ssize_t n = ::send(fd_, ptr + written, len - written, MSG_NOSIGNAL);
            if (n >= 0) {
                written += n;
            } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // 等待可写
                co_await EpollWriteAwaiter{
                    static_cast<EpollPerCoreEventLoop&>(loop_), fd_};
            } else {
                co_return -1;
            }
        }
        co_return static_cast<ssize_t>(written);
    }

    // =========================================================================
    // io_uring 模式的异步操作
    // =========================================================================

#ifdef ZLCORO_HAS_IO_URING
    // 异步读取（io_uring 模式）
    Task<ssize_t> read_io_uring(void* buf, size_t len) {
        auto& uring_loop = static_cast<IoUringPerCoreEventLoop&>(loop_);
        auto req = uring_loop.prep_recv(fd_, buf, len, 0);
        uring_loop.submit();
        int result = co_await make_awaiter(req);
        co_return result;
    }

    // 异步写入（io_uring 模式）
    Task<ssize_t> write_io_uring(const void* buf, size_t len) {
        auto& uring_loop = static_cast<IoUringPerCoreEventLoop&>(loop_);
        auto req = uring_loop.prep_send(fd_, buf, len, MSG_NOSIGNAL);
        uring_loop.submit();
        int result = co_await make_awaiter(req);
        co_return result;
    }
#endif

    // =========================================================================
    // 通用异步操作（自动选择后端）
    // =========================================================================

    // 异步读取
    Task<ssize_t> read(void* buf, size_t len) {
#ifdef ZLCORO_HAS_IO_URING
        if (loop_.backend() == PerCoreEventLoop::Backend::IoUring) {
            co_return co_await read_io_uring(buf, len);
        }
#endif
        co_return co_await read_epoll(buf, len);
    }

    // 异步写入
    Task<ssize_t> write(const void* buf, size_t len) {
#ifdef ZLCORO_HAS_IO_URING
        if (loop_.backend() == PerCoreEventLoop::Backend::IoUring) {
            co_return co_await write_io_uring(buf, len);
        }
#endif
        co_return co_await write_epoll(buf, len);
    }

    // 读取一行（以 \n 结尾）
    Task<std::string> read_line(size_t max_len = 4096) {
        std::string line;
        char c;
        
        while (line.size() < max_len) {
            ssize_t n = co_await read(&c, 1);
            if (n <= 0) break;
            if (c == '\n') break;
            line += c;
        }
        
        co_return line;
    }

    // 写入字符串
    Task<ssize_t> write_string(const std::string& str) {
        co_return co_await write(str.data(), str.size());
    }

    // 获取事件循环
    PerCoreEventLoop& loop() noexcept { return loop_; }

private:
    int fd_;
    PerCoreEventLoop& loop_;
    bool closed_;
};

// =============================================================================
// PerCoreTcpServer - TCP 服务器（每连接一个协程）
// =============================================================================

class PerCoreTcpServer {
public:
    // 连接处理器类型
    using Handler = std::function<Task<void>(PerCoreConnection&)>;

    PerCoreTcpServer(PerCoreRuntime& runtime)
        : runtime_(runtime)
        , listen_fd_(-1)
        , running_(false) {}

    ~PerCoreTcpServer() {
        stop();
    }

    // 监听指定端口
    bool listen(uint16_t port, const std::string& ip = "0.0.0.0", int backlog = 128) {
        // 创建 socket
        listen_fd_ = socket(AF_INET, SOCK_STREAM, 0);
        if (listen_fd_ < 0) {
            return false;
        }

        // 设置 SO_REUSEADDR 和 SO_REUSEPORT
        int opt = 1;
        setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
        setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));

        // 设置非阻塞
        int flags = fcntl(listen_fd_, F_GETFL, 0);
        fcntl(listen_fd_, F_SETFL, flags | O_NONBLOCK);

        // 绑定地址
        struct sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        inet_pton(AF_INET, ip.c_str(), &addr.sin_addr);

        if (bind(listen_fd_, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
            ::close(listen_fd_);
            listen_fd_ = -1;
            return false;
        }

        // 开始监听
        if (::listen(listen_fd_, backlog) < 0) {
            ::close(listen_fd_);
            listen_fd_ = -1;
            return false;
        }

        return true;
    }

    // 设置连接处理器
    void set_handler(Handler handler) {
        handler_ = std::move(handler);
    }

    // 开始接受连接（在指定核心上）
    void start_accept_on_core(size_t core_index) {
        if (listen_fd_ < 0 || !handler_) {
            return;
        }
        
        running_ = true;
        auto& loop = runtime_.get_loop(core_index);
        
        // 启动接受协程
        if (loop.backend() == PerCoreEventLoop::Backend::Epoll) {
            start_epoll_accept(core_index);
        }
#ifdef ZLCORO_HAS_IO_URING
        else if (loop.backend() == PerCoreEventLoop::Backend::IoUring) {
            start_io_uring_accept(core_index);
        }
#endif
    }

    // 在所有核心上接受连接（使用 SO_REUSEPORT）
    void start_accept_all_cores() {
        for (size_t i = 0; i < runtime_.num_cores(); ++i) {
            start_accept_on_core(i);
        }
    }

    // 停止服务器
    void stop() {
        running_ = false;
        if (listen_fd_ >= 0) {
            ::close(listen_fd_);
            listen_fd_ = -1;
        }
    }

    // 获取监听 fd
    int listen_fd() const noexcept { return listen_fd_; }

private:
    // 启动一个"fire and forget"协程（不等待结果）
    // 协程会在完成后自动销毁
    template<typename CoroFn>
    void spawn_coroutine(CoroFn&& fn) {
        auto task = fn();
        // 获取协程句柄并启动
        auto handle = task.handle();
        if (handle && !handle.done()) {
            handle.resume();
        }
        // 释放 Task 对句柄的所有权，让协程自管理
        task.release();
    }

    // epoll 模式接受连接
    void start_epoll_accept(size_t core_index) {
        auto& loop = static_cast<EpollPerCoreEventLoop&>(runtime_.get_loop(core_index));
        
        // 创建接受协程
        spawn_coroutine([this, &loop]() -> Task<void> {
            while (running_) {
                // 等待可读
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
                        continue;  // 其他错误，继续尝试
                    }
                    
                    // 选择处理该连接的核心
                    size_t target_core = runtime_.select_core_by_fd(client_fd);
                    auto& target_loop = runtime_.get_loop(target_core);
                    
                    // 在目标核心上启动处理协程
                    handle_connection_epoll(client_fd, 
                        static_cast<EpollPerCoreEventLoop&>(target_loop));
                }
            }
        });
    }

    // epoll 模式处理连接
    void handle_connection_epoll(int client_fd, EpollPerCoreEventLoop& loop) {
        // 设置 TCP_NODELAY
        int opt = 1;
        setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));
        
        // 创建连接对象并启动处理协程
        spawn_coroutine([this, client_fd, &loop]() -> Task<void> {
            PerCoreConnection conn(client_fd, loop);
            co_await handler_(conn);
        });
    }

#ifdef ZLCORO_HAS_IO_URING
    // io_uring 模式接受连接
    void start_io_uring_accept(size_t core_index) {
        auto& loop = static_cast<IoUringPerCoreEventLoop&>(runtime_.get_loop(core_index));
        
        spawn_coroutine([this, &loop]() -> Task<void> {
            while (running_) {
                struct sockaddr_in client_addr{};
                socklen_t addr_len = sizeof(client_addr);
                
                // 使用 io_uring accept
                auto req = loop.prep_accept(listen_fd_, 
                                           (struct sockaddr*)&client_addr, 
                                           &addr_len);
                loop.submit();
                
                int client_fd = co_await make_awaiter(req);
                
                if (client_fd < 0) {
                    continue;
                }
                
                // 设置非阻塞
                int flags = fcntl(client_fd, F_GETFL, 0);
                fcntl(client_fd, F_SETFL, flags | O_NONBLOCK);
                
                // 选择处理该连接的核心
                size_t target_core = runtime_.select_core_by_fd(client_fd);
                
                // 在目标核心上启动处理协程
                if (target_core == static_cast<size_t>(loop.core_id())) {
                    handle_connection_io_uring(client_fd, loop);
                } else {
                    auto& target_loop = static_cast<IoUringPerCoreEventLoop&>(
                        runtime_.get_loop(target_core));
                    handle_connection_io_uring(client_fd, target_loop);
                }
            }
        });
    }

    // io_uring 模式处理连接
    void handle_connection_io_uring(int client_fd, IoUringPerCoreEventLoop& loop) {
        // 设置 TCP_NODELAY
        int opt = 1;
        setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));
        
        spawn_coroutine([this, client_fd, &loop]() -> Task<void> {
            PerCoreConnection conn(client_fd, loop);
            co_await handler_(conn);
        });
    }
#endif

private:
    PerCoreRuntime& runtime_;
    int listen_fd_;
    Handler handler_;
    std::atomic<bool> running_;
};

// =============================================================================
// PerCoreTcpClient - TCP 客户端
// =============================================================================

class PerCoreTcpClient {
public:
    PerCoreTcpClient(PerCoreEventLoop& loop)
        : loop_(loop)
        , fd_(-1) {}

    ~PerCoreTcpClient() {
        close();
    }

    // 连接到服务器
    Task<bool> connect(const std::string& ip, uint16_t port) {
        // 创建 socket
        fd_ = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (fd_ < 0) {
            co_return false;
        }

        // 设置 TCP_NODELAY
        int opt = 1;
        setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));

        // 连接
        struct sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        inet_pton(AF_INET, ip.c_str(), &addr.sin_addr);

        int ret = ::connect(fd_, (struct sockaddr*)&addr, sizeof(addr));
        if (ret < 0 && errno != EINPROGRESS) {
            ::close(fd_);
            fd_ = -1;
            co_return false;
        }

        if (ret < 0) {
            // 等待连接完成
#ifdef ZLCORO_HAS_IO_URING
            if (loop_.backend() == PerCoreEventLoop::Backend::IoUring) {
                // io_uring 模式：使用 poll 等待可写
                auto& uring_loop = static_cast<IoUringPerCoreEventLoop&>(loop_);
                loop_.register_write(fd_, std::coroutine_handle<>::from_address(nullptr));
                // TODO: 需要更好的方式处理 connect
            } else
#endif
            {
                co_await EpollWriteAwaiter{
                    static_cast<EpollPerCoreEventLoop&>(loop_), fd_};
            }

            // 检查连接是否成功
            int error = 0;
            socklen_t len = sizeof(error);
            getsockopt(fd_, SOL_SOCKET, SO_ERROR, &error, &len);
            if (error != 0) {
                ::close(fd_);
                fd_ = -1;
                co_return false;
            }
        }

        co_return true;
    }

    // 获取连接
    std::unique_ptr<PerCoreConnection> get_connection() {
        if (fd_ >= 0) {
            auto conn = std::make_unique<PerCoreConnection>(fd_, loop_);
            fd_ = -1;  // 转移所有权
            return conn;
        }
        return nullptr;
    }

    // 关闭
    void close() {
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
    }

private:
    PerCoreEventLoop& loop_;
    int fd_;
};

} // namespace zlcoro
