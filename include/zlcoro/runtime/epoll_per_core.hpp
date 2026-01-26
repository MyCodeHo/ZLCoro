#pragma once

#include "per_core_event_loop.hpp"
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdexcept>
#include <cstring>

namespace zlcoro {

// =============================================================================
// EpollPerCoreEventLoop - 基于 epoll 的每核心事件循环
// =============================================================================
//
// 特点：
// - 使用 epoll 作为 I/O 多路复用后端
// - 边缘触发（ET）模式，高性能
// - 每个 fd 关联一个协程句柄
// - 完全在单线程内运行，无锁
// =============================================================================

class EpollPerCoreEventLoop : public PerCoreEventLoop {
public:
    // 最大事件数
    static constexpr int MAX_EVENTS = 256;

    EpollPerCoreEventLoop() : epoll_fd_(-1), wakeup_fd_(-1) {
        // 创建 epoll 实例
        epoll_fd_ = epoll_create1(EPOLL_CLOEXEC);
        if (epoll_fd_ < 0) {
            throw std::runtime_error("Failed to create epoll: " + 
                                    std::string(strerror(errno)));
        }
        
        // 创建 eventfd 用于唤醒
        wakeup_fd_ = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        if (wakeup_fd_ < 0) {
            ::close(epoll_fd_);
            throw std::runtime_error("Failed to create eventfd: " + 
                                    std::string(strerror(errno)));
        }
        
        // 注册 wakeup fd
        struct epoll_event ev{};
        ev.events = EPOLLIN | EPOLLET;
        ev.data.fd = wakeup_fd_;
        if (epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, wakeup_fd_, &ev) < 0) {
            ::close(wakeup_fd_);
            ::close(epoll_fd_);
            throw std::runtime_error("Failed to add wakeup fd to epoll");
        }
    }

    ~EpollPerCoreEventLoop() override {
        if (wakeup_fd_ >= 0) ::close(wakeup_fd_);
        if (epoll_fd_ >= 0) ::close(epoll_fd_);
    }

    // =========================================================================
    // 后端类型
    // =========================================================================

    Backend backend() const override { return Backend::Epoll; }

    // =========================================================================
    // I/O 事件注册
    // =========================================================================

    // 注册读事件
    void register_read(int fd, std::coroutine_handle<> coro) override {
        register_fd(fd, EPOLLIN | EPOLLET, coro);
    }

    // 注册写事件
    void register_write(int fd, std::coroutine_handle<> coro) override {
        register_fd(fd, EPOLLOUT | EPOLLET, coro);
    }

    // 注册读写事件
    void register_rw(int fd, std::coroutine_handle<> coro) override {
        register_fd(fd, EPOLLIN | EPOLLOUT | EPOLLET, coro);
    }

    // 取消注册
    void unregister(int fd) override {
        epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr);
        handlers_.erase(fd);
    }

    // 检查 fd 是否已注册
    bool has_fd(int fd) const override {
        return handlers_.find(fd) != handlers_.end();
    }

    // 设置 fd 为非阻塞
    static bool set_nonblocking(int fd) {
        int flags = fcntl(fd, F_GETFL, 0);
        if (flags < 0) return false;
        return fcntl(fd, F_SETFL, flags | O_NONBLOCK) >= 0;
    }

    // =========================================================================
    // 获取原生句柄
    // =========================================================================

    int epoll_fd() const noexcept { return epoll_fd_; }

protected:
    // 轮询 I/O 事件
    void poll_events(int timeout_ms) override {
        struct epoll_event events[MAX_EVENTS];
        
        int n = epoll_wait(epoll_fd_, events, MAX_EVENTS, timeout_ms);
        
        for (int i = 0; i < n; ++i) {
            int fd = events[i].data.fd;
            
            // 处理 wakeup fd
            if (fd == wakeup_fd_) {
                uint64_t val;
                [[maybe_unused]] auto _ = ::read(wakeup_fd_, &val, sizeof(val));
                continue;
            }
            
            // 查找对应的协程
            auto it = handlers_.find(fd);
            if (it != handlers_.end()) {
                auto coro = it->second;
                
                // 检查错误和挂起
                if (events[i].events & (EPOLLERR | EPOLLHUP)) {
                    // 出错或挂起，仍然唤醒协程让其处理
                }
                
                // 唤醒协程
                if (coro && !coro.done()) {
                    coro.resume();
                }
            }
        }
    }

    // 唤醒阻塞的 poll
    void wakeup() override {
        uint64_t val = 1;
        [[maybe_unused]] auto _ = ::write(wakeup_fd_, &val, sizeof(val));
    }

private:
    // 注册 fd
    void register_fd(int fd, uint32_t events, std::coroutine_handle<> coro) {
        struct epoll_event ev{};
        ev.events = events;
        ev.data.fd = fd;
        
        auto it = handlers_.find(fd);
        if (it != handlers_.end()) {
            // 已存在，修改
            it->second = coro;
            epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, fd, &ev);
        } else {
            // 新增
            handlers_[fd] = coro;
            epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fd, &ev);
        }
    }

private:
    int epoll_fd_;    // epoll 文件描述符
    int wakeup_fd_;   // 用于唤醒的 eventfd
    
    // fd -> 协程句柄映射（单线程访问，无需加锁）
    std::map<int, std::coroutine_handle<>> handlers_;
};

// =============================================================================
// epoll 事件等待 Awaiter
// =============================================================================

// 等待读事件
struct EpollReadAwaiter {
    EpollPerCoreEventLoop& loop;
    int fd;

    bool await_ready() const noexcept { return false; }

    void await_suspend(std::coroutine_handle<> coro) {
        loop.register_read(fd, coro);
    }

    void await_resume() const noexcept {}
};

// 等待写事件
struct EpollWriteAwaiter {
    EpollPerCoreEventLoop& loop;
    int fd;

    bool await_ready() const noexcept { return false; }

    void await_suspend(std::coroutine_handle<> coro) {
        loop.register_write(fd, coro);
    }

    void await_resume() const noexcept {}
};

// =============================================================================
// 延迟等待 Awaiter
// =============================================================================

struct DelayAwaiter {
    PerCoreEventLoop& loop;
    int delay_ms;

    bool await_ready() const noexcept { return delay_ms <= 0; }

    void await_suspend(std::coroutine_handle<> coro) {
        loop.add_timer(delay_ms, [coro]() mutable {
            if (coro && !coro.done()) {
                coro.resume();
            }
        });
    }

    void await_resume() const noexcept {}
};

// 创建延迟等待
inline DelayAwaiter delay(PerCoreEventLoop& loop, int ms) {
    return DelayAwaiter{loop, ms};
}

} // namespace zlcoro
