#pragma once

#include <sys/epoll.h>
#include <unistd.h>
#include <coroutine>
#include <stdexcept>
#include <map>
#include <vector>
#include <cstring>

namespace zlcoro {

// =============================================================================
// EpollPoller - Epoll 事件轮询器
// =============================================================================
// 
// 封装 Linux epoll API，用于高效的 I/O 事件监听。
// 支持注册文件描述符，等待事件，并在事件就绪时恢复协程。
// =============================================================================

class EpollPoller {
public:
    // 事件类型
    enum Event : uint32_t {
        Read = EPOLLIN,       // 可读事件
        Write = EPOLLOUT,     // 可写事件
        Error = EPOLLERR,     // 错误事件
        HangUp = EPOLLHUP,    // 挂断事件
        EdgeTriggered = EPOLLET  // 边缘触发模式
    };

    // 事件回调信息
    struct EventHandler {
        std::coroutine_handle<> coro;  // 要恢复的协程
        uint32_t events;                // 监听的事件
    };

    // 构造函数：创建 epoll 实例
    EpollPoller() {
        epfd_ = epoll_create1(0);
        if (epfd_ == -1) {
            throw std::runtime_error(
                std::string("epoll_create1 failed: ") + strerror(errno));
        }
    }

    // 禁止拷贝
    EpollPoller(const EpollPoller&) = delete;
    EpollPoller& operator=(const EpollPoller&) = delete;

    // 析构函数
    ~EpollPoller() {
        if (epfd_ != -1) {
            close(epfd_);
        }
    }

    // 添加文件描述符监听
    void add(int fd, uint32_t events, std::coroutine_handle<> coro) {
        epoll_event ev;
        ev.events = events;
        ev.data.fd = fd;
        
        if (epoll_ctl(epfd_, EPOLL_CTL_ADD, fd, &ev) == -1) {
            throw std::runtime_error(
                std::string("epoll_ctl ADD failed: ") + strerror(errno));
        }
        
        // 记录事件处理器
        handlers_[fd] = EventHandler{coro, events};
    }

    // 修改文件描述符监听的事件
    void modify(int fd, uint32_t events, std::coroutine_handle<> coro) {
        epoll_event ev;
        ev.events = events;
        ev.data.fd = fd;
        
        if (epoll_ctl(epfd_, EPOLL_CTL_MOD, fd, &ev) == -1) {
            throw std::runtime_error(
                std::string("epoll_ctl MOD failed: ") + strerror(errno));
        }
        
        handlers_[fd] = EventHandler{coro, events};
    }

    // 移除文件描述符
    void remove(int fd) {
        if (epoll_ctl(epfd_, EPOLL_CTL_DEL, fd, nullptr) == -1) {
            // 忽略 ENOENT 错误（文件描述符不存在）
            if (errno != ENOENT) {
                throw std::runtime_error(
                    std::string("epoll_ctl DEL failed: ") + strerror(errno));
            }
        }
        
        handlers_.erase(fd);
    }

    // 轮询事件（阻塞指定时间）
    // timeout_ms: 超时时间（毫秒），-1 表示永久阻塞
    // 返回：就绪的协程句柄列表
    std::vector<std::coroutine_handle<>> poll(int timeout_ms = -1) {
        epoll_event events[max_events_];
        
        int n = epoll_wait(epfd_, events, max_events_, timeout_ms);
        
        if (n == -1) {
            if (errno == EINTR) {
                // 被信号中断，返回空列表
                return {};
            }
            throw std::runtime_error(
                std::string("epoll_wait failed: ") + strerror(errno));
        }
        
        // 收集就绪的协程（使用 set 去重）
        std::vector<std::coroutine_handle<>> ready_coros;
        ready_coros.reserve(n);
        
        for (int i = 0; i < n; ++i) {
            int fd = events[i].data.fd;
            uint32_t revents = events[i].events;
            
            auto it = handlers_.find(fd);
            if (it != handlers_.end()) {
                // 检查是否触发了我们关心的事件或错误事件
                // 错误事件（EPOLLERR | EPOLLHUP）总是需要处理
                if ((revents & it->second.events) || (revents & (EPOLLERR | EPOLLHUP))) {
                    ready_coros.push_back(it->second.coro);
                }
            }
        }
        
        return ready_coros;
    }

    // 检查文件描述符是否已注册
    bool has(int fd) const {
        return handlers_.find(fd) != handlers_.end();
    }

    // 获取 epoll 文件描述符
    int fd() const noexcept {
        return epfd_;
    }

private:
    // =========================================================================
    // 数据成员
    // =========================================================================
    
    /// @brief epoll 实例的文件描述符
    /// @details 由 epoll_create1() 创建，用于 epoll_ctl() 和 epoll_wait()。
    ///          -1 表示未初始化或已关闭。
    /// @ownership EpollPoller 独占所有权
    int epfd_ = -1;
    
    /// @brief 文件描述符到事件处理器的映射
    /// @details 记录每个 fd 关联的协程句柄和监听的事件类型。
    ///          当 epoll_wait() 返回时，通过此 map 找到要恢复的协程。
    ///          使用 map 保证 O(log n) 查找，适合中等规模的 fd 数量。
    /// @thread_safety 目前非线程安全，需外部同步
    std::map<int, EventHandler> handlers_;
    
    /// @brief 单次 epoll_wait 最多返回的事件数
    /// @details 这个值影响内存使用和轮询效率的平衡。
    ///          128 是一个常见的折中值，适合大多数场景。
    static constexpr int max_events_ = 128;
};

} // namespace zlcoro
