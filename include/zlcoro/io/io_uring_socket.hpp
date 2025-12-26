#pragma once

#include "io_uring_poller.hpp"

#ifdef ZLCORO_HAS_IO_URING

#include "zlcoro/core/task.hpp"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <cstring>
#include <string>
#include <memory>
#include <utility>

namespace zlcoro {

// =============================================================================
// IoUringSocket - 基于 io_uring 的异步 Socket
// =============================================================================
// 
// 使用 io_uring 实现真正的异步网络 I/O：
// - 异步 accept/connect/send/recv
// - 无需 epoll 边缘触发 + 非阻塞的复杂组合
// - 内核直接完成 I/O 操作
// =============================================================================

class IoUringSocket {
public:
    // 构造函数
    IoUringSocket() : fd_(-1), poller_(nullptr) {}

    explicit IoUringSocket(IoUringPoller* poller) 
        : fd_(-1), poller_(poller) {}

    IoUringSocket(int fd, IoUringPoller* poller)
        : fd_(fd), poller_(poller) {}

    // 禁止拷贝
    IoUringSocket(const IoUringSocket&) = delete;
    IoUringSocket& operator=(const IoUringSocket&) = delete;

    // 移动构造
    IoUringSocket(IoUringSocket&& other) noexcept
        : fd_(std::exchange(other.fd_, -1))
        , poller_(std::exchange(other.poller_, nullptr)) {}

    IoUringSocket& operator=(IoUringSocket&& other) noexcept {
        if (this != &other) {
            close_sync();
            fd_ = std::exchange(other.fd_, -1);
            poller_ = std::exchange(other.poller_, nullptr);
        }
        return *this;
    }

    // 析构函数
    ~IoUringSocket() {
        close_sync();
    }

    // 设置 poller
    void set_poller(IoUringPoller* poller) {
        poller_ = poller;
    }

    // =========================================================================
    // Socket 创建和配置
    // =========================================================================

    // 创建 TCP socket
    void create(int domain = AF_INET, int type = SOCK_STREAM) {
        close_sync();
        
        fd_ = socket(domain, type, 0);
        if (fd_ == -1) {
            throw std::runtime_error(
                std::string("socket failed: ") + strerror(errno));
        }
    }

    // 设置 socket 选项
    void set_option(int level, int optname, int value) {
        if (setsockopt(fd_, level, optname, &value, sizeof(value)) == -1) {
            throw std::runtime_error(
                std::string("setsockopt failed: ") + strerror(errno));
        }
    }

    // 设置地址重用
    void set_reuse_addr(bool enable = true) {
        set_option(SOL_SOCKET, SO_REUSEADDR, enable ? 1 : 0);
    }

    // 设置端口重用
    void set_reuse_port(bool enable = true) {
        set_option(SOL_SOCKET, SO_REUSEPORT, enable ? 1 : 0);
    }

    // 设置非阻塞模式
    void set_nonblocking(bool enable = true) {
        int flags = fcntl(fd_, F_GETFL, 0);
        if (flags == -1) {
            throw std::runtime_error(
                std::string("fcntl F_GETFL failed: ") + strerror(errno));
        }
        
        if (enable) {
            flags |= O_NONBLOCK;
        } else {
            flags &= ~O_NONBLOCK;
        }
        
        if (fcntl(fd_, F_SETFL, flags) == -1) {
            throw std::runtime_error(
                std::string("fcntl F_SETFL failed: ") + strerror(errno));
        }
    }

    // 绑定地址
    void bind(const std::string& ip, uint16_t port) {
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        
        if (ip.empty() || ip == "0.0.0.0") {
            addr.sin_addr.s_addr = INADDR_ANY;
        } else {
            if (inet_pton(AF_INET, ip.c_str(), &addr.sin_addr) != 1) {
                throw std::runtime_error("Invalid IP address: " + ip);
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

    // 同步关闭
    void close_sync() {
        if (fd_ != -1) {
            ::close(fd_);
            fd_ = -1;
        }
    }

    // 检查是否有效
    bool is_valid() const noexcept {
        return fd_ != -1;
    }

    // 获取文件描述符
    int fd() const noexcept {
        return fd_;
    }

    // 释放所有权
    int release() {
        return std::exchange(fd_, -1);
    }

    // =========================================================================
    // 异步操作
    // =========================================================================

    // 异步 accept
    // 返回: 新连接的 socket 封装
    // 注意：使用 shared_ptr 管理地址信息，确保内核写入时内存有效
    Task<IoUringSocket> accept() {
        if (!is_valid()) {
            throw std::runtime_error("Socket not valid");
        }
        if (!poller_) {
            throw std::runtime_error("No IoUringPoller set");
        }

        // 使用 shared_ptr 确保地址信息在 io_uring 完成前有效
        // 即使协程被取消，内存也不会泄漏
        struct AcceptData {
            sockaddr_in client_addr{};
            socklen_t addr_len = sizeof(sockaddr_in);
        };
        auto accept_data = std::make_shared<AcceptData>();

        IoUringPoller::Request req;
        poller_->prep_accept(fd_, reinterpret_cast<sockaddr*>(&accept_data->client_addr), 
                            &accept_data->addr_len, &req);
        poller_->submit();

        co_await IoUringAwaiter(&req);
        
        if (req.result < 0) {
            throw std::runtime_error(
                std::string("accept failed: ") + strerror(-req.result));
        }

        co_return IoUringSocket(req.result, poller_);
    }

    // 异步 connect
    // 注意：使用 shared_ptr 管理地址信息，确保内核访问时内存有效
    Task<int> connect(const std::string& ip, uint16_t port) {
        if (!is_valid()) {
            throw std::runtime_error("Socket not valid");
        }
        if (!poller_) {
            throw std::runtime_error("No IoUringPoller set");
        }

        // 使用 shared_ptr 确保地址信息在 io_uring 完成前有效
        auto addr = std::make_shared<sockaddr_in>();
        addr->sin_family = AF_INET;
        addr->sin_port = htons(port);
        
        if (inet_pton(AF_INET, ip.c_str(), &addr->sin_addr) != 1) {
            throw std::runtime_error("Invalid IP address: " + ip);
        }

        IoUringPoller::Request req;
        poller_->prep_connect(fd_, reinterpret_cast<const sockaddr*>(addr.get()),
                             sizeof(sockaddr_in), &req);
        poller_->submit();

        co_await IoUringAwaiter(&req);
        co_return req.result;
    }

    // 异步 send
    // 返回: 发送的字节数，负数表示错误
    Task<ssize_t> send(const void* buf, size_t len, int flags = 0) {
        if (!is_valid()) {
            throw std::runtime_error("Socket not valid");
        }
        if (!poller_) {
            throw std::runtime_error("No IoUringPoller set");
        }

        IoUringPoller::Request req;
        poller_->prep_send(fd_, buf, len, flags, &req);
        poller_->submit();

        co_await IoUringAwaiter(&req);
        co_return req.result;
    }

    // 异步发送字符串
    Task<ssize_t> send_string(const std::string& data, int flags = 0) {
        co_return co_await send(data.data(), data.size(), flags);
    }

    // 异步发送所有数据
    Task<size_t> send_all(const void* buf, size_t len, int flags = 0) {
        const char* ptr = static_cast<const char*>(buf);
        size_t total = 0;

        while (total < len) {
            ssize_t n = co_await send(ptr + total, len - total, flags);
            if (n < 0) {
                throw std::runtime_error(
                    std::string("send failed: ") + strerror(-n));
            }
            if (n == 0) {
                break;  // 连接关闭
            }
            total += n;
        }

        co_return total;
    }

    // 异步 recv
    // 返回: 接收的字节数，0 表示连接关闭，负数表示错误
    Task<ssize_t> recv(void* buf, size_t len, int flags = 0) {
        if (!is_valid()) {
            throw std::runtime_error("Socket not valid");
        }
        if (!poller_) {
            throw std::runtime_error("No IoUringPoller set");
        }

        IoUringPoller::Request req;
        poller_->prep_recv(fd_, buf, len, flags, &req);
        poller_->submit();

        co_await IoUringAwaiter(&req);
        co_return req.result;
    }

    // 异步接收到字符串
    Task<std::string> recv_string(size_t max_len, int flags = 0) {
        std::string buffer(max_len, '\0');
        
        ssize_t n = co_await recv(buffer.data(), max_len, flags);
        if (n < 0) {
            throw std::runtime_error(
                std::string("recv failed: ") + strerror(-n));
        }
        
        buffer.resize(n);
        co_return buffer;
    }

    // 异步关闭（通过 io_uring）
    Task<int> close_async() {
        if (!is_valid()) {
            co_return 0;
        }
        if (!poller_) {
            close_sync();
            co_return 0;
        }

        IoUringPoller::Request req;
        poller_->prep_close(fd_, &req);
        poller_->submit();

        co_await IoUringAwaiter(&req);
        
        fd_ = -1;  // 标记为已关闭
        co_return req.result;
    }

    // 获取对端地址信息
    std::pair<std::string, uint16_t> get_peer_address() const {
        sockaddr_in addr{};
        socklen_t len = sizeof(addr);
        
        if (getpeername(fd_, reinterpret_cast<sockaddr*>(&addr), &len) == -1) {
            throw std::runtime_error(
                std::string("getpeername failed: ") + strerror(errno));
        }
        
        char ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &addr.sin_addr, ip, sizeof(ip));
        
        return {ip, ntohs(addr.sin_port)};
    }

private:
    int fd_;                // socket 文件描述符
    IoUringPoller* poller_; // io_uring 轮询器
};

} // namespace zlcoro

#endif // ZLCORO_HAS_IO_URING
