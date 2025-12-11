#pragma once

#include "zlcoro/core/task.hpp"
#include "event_loop.hpp"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstring>
#include <string>
#include <memory>
#include <stdexcept>
#include <utility>

namespace zlcoro {

// =============================================================================
// AsyncSocket - 异步网络 Socket
// =============================================================================
// 
// 提供异步的网络 socket 操作，基于 epoll 实现真正的异步 I/O。
// =============================================================================

class AsyncSocket {
public:
    // 构造函数
    AsyncSocket() : fd_(-1), event_loop_(EventLoop::instance()) {}

    explicit AsyncSocket(int fd) : fd_(fd), event_loop_(EventLoop::instance()) {
        make_nonblocking();
    }

    // 禁止拷贝
    AsyncSocket(const AsyncSocket&) = delete;
    AsyncSocket& operator=(const AsyncSocket&) = delete;

    // 移动构造
    AsyncSocket(AsyncSocket&& other) noexcept 
        : fd_(std::exchange(other.fd_, -1)), event_loop_(other.event_loop_) {
    }

    AsyncSocket& operator=(AsyncSocket&& other) noexcept {
        if (this != &other) {
            close();
            fd_ = std::exchange(other.fd_, -1);
            // event_loop_ 是引用，不需要赋值
        }
        return *this;
    }

    // 析构函数
    ~AsyncSocket() {
        close();
    }

    // 创建 socket
    void create(int domain = AF_INET, int type = SOCK_STREAM, int protocol = 0) {
        close();
        
        fd_ = socket(domain, type, protocol);
        if (fd_ == -1) {
            throw std::runtime_error(
                std::string("socket failed: ") + strerror(errno));
        }
        
        make_nonblocking();
    }

    // 关闭 socket
    void close() {
        if (fd_ != -1) {
            event_loop_.unregister(fd_);
            ::close(fd_);
            fd_ = -1;
        }
    }

    // 检查是否打开
    bool is_open() const noexcept {
        return fd_ != -1;
    }

    // 获取文件描述符
    int fd() const noexcept {
        return fd_;
    }

    // 设置地址重用
    void set_reuse_addr(bool reuse = true) {
        int opt = reuse ? 1 : 0;
        if (setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) == -1) {
            throw std::runtime_error(
                std::string("setsockopt SO_REUSEADDR failed: ") + strerror(errno));
        }
    }

    // 设置端口重用
    void set_reuse_port(bool reuse = true) {
        int opt = reuse ? 1 : 0;
        if (setsockopt(fd_, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt)) == -1) {
            throw std::runtime_error(
                std::string("setsockopt SO_REUSEPORT failed: ") + strerror(errno));
        }
    }

    // 绑定地址
    void bind(const std::string& host, int port) {
        if (!is_open()) {
            create();
        }
        
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        
        if (host.empty() || host == "0.0.0.0") {
            addr.sin_addr.s_addr = INADDR_ANY;
        } else {
            if (inet_pton(AF_INET, host.c_str(), &addr.sin_addr) <= 0) {
                throw std::runtime_error("Invalid address: " + host);
            }
        }
        
        if (::bind(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == -1) {
            throw std::runtime_error(
                std::string("bind failed: ") + strerror(errno));
        }
    }

    // 监听
    void listen(int backlog = 128) {
        if (::listen(fd_, backlog) == -1) {
            throw std::runtime_error(
                std::string("listen failed: ") + strerror(errno));
        }
    }

    // 异步连接
    Task<void> connect(const std::string& host, int port) {
        if (!is_open()) {
            create();
        }
        
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        
        if (inet_pton(AF_INET, host.c_str(), &addr.sin_addr) <= 0) {
            throw std::runtime_error("Invalid address: " + host);
        }
        
        int ret = ::connect(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
        
        if (ret == 0) {
            co_return;  // 立即连接成功
        }
        
        if (errno != EINPROGRESS) {
            throw std::runtime_error(
                std::string("connect failed: ") + strerror(errno));
        }
        
        // 等待可写（连接完成）
        co_await WriteAwaiter{fd_, event_loop_};
        
        // 检查连接错误
        int error = 0;
        socklen_t len = sizeof(error);
        if (getsockopt(fd_, SOL_SOCKET, SO_ERROR, &error, &len) == -1) {
            throw std::runtime_error(
                std::string("getsockopt failed: ") + strerror(errno));
        }
        
        if (error != 0) {
            throw std::runtime_error(
                std::string("connect failed: ") + strerror(error));
        }
        
        co_return;
    }

    // 异步接受连接
    Task<AsyncSocket> accept() {
        while (true) {
            co_await ReadAwaiter{fd_, event_loop_};
            
            sockaddr_in addr{};
            socklen_t len = sizeof(addr);
            
            int client_fd = ::accept(fd_, reinterpret_cast<sockaddr*>(&addr), &len);
            
            if (client_fd == -1) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    // 没有连接可接受，继续循环等待
                    continue;
                }
                throw std::runtime_error(
                    std::string("accept failed: ") + strerror(errno));
            }
            
            co_return AsyncSocket(client_fd);
        }
    }

    // 异步读取
    Task<std::string> read(size_t max_len = 4096) {
        std::string buffer;
        buffer.resize(max_len);
        
        while (true) {
            co_await ReadAwaiter{fd_, event_loop_};
            
            ssize_t n = ::read(fd_, buffer.data(), max_len);
            
            if (n == -1) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    // 没有数据，继续循环等待
                    continue;
                }
                throw std::runtime_error(
                    std::string("read failed: ") + strerror(errno));
            }
            
            if (n == 0) {
                // 连接关闭
                buffer.clear();
            } else {
                buffer.resize(n);
            }
            
            co_return buffer;
        }
    }

    // 异步写入（字符串版本）
    // 注意：直接返回重载版本，避免不必要的协程嵌套
    Task<size_t> write(const std::string& data) {
        return write(data.data(), data.size());
    }

    Task<size_t> write(const char* data, size_t len) {
        size_t total_written = 0;
        
        while (total_written < len) {
            ssize_t n = ::write(fd_, data + total_written, len - total_written);
            
            if (n == -1) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    // 缓冲区满，等待可写
                    co_await WriteAwaiter{fd_, event_loop_};
                    continue;
                }
                throw std::runtime_error(
                    std::string("write failed: ") + strerror(errno));
            }
            
            total_written += n;
        }
        
        co_return total_written;
    }

private:
    // 设置为非阻塞模式
    void make_nonblocking() {
        int flags = fcntl(fd_, F_GETFL, 0);
        if (flags == -1) {
            throw std::runtime_error(
                std::string("fcntl F_GETFL failed: ") + strerror(errno));
        }
        
        if (fcntl(fd_, F_SETFL, flags | O_NONBLOCK) == -1) {
            throw std::runtime_error(
                std::string("fcntl F_SETFL failed: ") + strerror(errno));
        }
    }

    // 读事件 Awaiter
    struct ReadAwaiter {
        int fd;
        EventLoop& event_loop;
        
        bool await_ready() const noexcept {
            return false;  // 总是挂起
        }
        
        void await_suspend(std::coroutine_handle<> coro) {
            event_loop.register_read(fd, coro);
        }
        
        void await_resume() const noexcept {}
    };

    // 写事件 Awaiter
    struct WriteAwaiter {
        int fd;
        EventLoop& event_loop;
        
        bool await_ready() const noexcept {
            return false;  // 总是挂起
        }
        
        void await_suspend(std::coroutine_handle<> coro) {
            event_loop.register_write(fd, coro);
        }
        
        void await_resume() const noexcept {}
    };

private:
    int fd_;                    // Socket 文件描述符
    EventLoop& event_loop_;     // 事件循环引用
};

} // namespace zlcoro
