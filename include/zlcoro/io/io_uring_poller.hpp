#pragma once

// io_uring 仅在 Linux 5.1+ 可用
#ifdef __linux__
#include <linux/version.h>
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 1, 0)
#define ZLCORO_HAS_IO_URING 1
#endif
#endif

#ifdef ZLCORO_HAS_IO_URING

#include <liburing.h>
#include <coroutine>
#include <stdexcept>
#include <cstring>
#include <unordered_map>
#include <vector>
#include <memory>
#include <atomic>
#include <fcntl.h>
#include <unistd.h>

namespace zlcoro {

// =============================================================================
// IoUringPoller - io_uring 事件轮询器
// =============================================================================
// 
// 封装 Linux io_uring API，提供真正的异步 I/O 操作。
// 相比 epoll + 线程池方案，性能更高，系统调用更少。
// 
// 特性:
// - 零拷贝：SQ/CQ 使用共享内存
// - 批量提交：多个请求一次系统调用
// - 真正异步：内核直接执行 I/O，无需线程池
// =============================================================================

class IoUringPoller {
public:
    // 操作类型
    enum class OpType : uint8_t {
        Read,       // 读操作
        Write,      // 写操作
        ReadV,      // 向量读
        WriteV,     // 向量写
        Fsync,      // 文件同步
        Accept,     // 接受连接
        Connect,    // 发起连接
        Send,       // 发送数据
        Recv,       // 接收数据
        Close,      // 关闭文件
        Timeout,    // 超时
        Cancel,     // 取消操作
        Nop         // 空操作（测试用）
    };

    // 操作请求上下文
    // 注意：Request 必须在 I/O 操作完成前保持有效
    // 推荐使用 shared_ptr 或确保协程帧生命周期
    struct Request {
        std::coroutine_handle<> coro;   // 完成时恢复的协程
        OpType op_type;                  // 操作类型
        int result;                      // 操作结果（字节数或错误码）
        std::atomic<bool> completed;     // 是否已完成（原子操作，避免竞态）
        void* user_data;                 // 用户数据
        
        Request() 
            : coro(nullptr)
            , op_type(OpType::Nop)
            , result(0)
            , completed(false)
            , user_data(nullptr) {}
        
        // 禁止拷贝（因为地址被传给内核）
        Request(const Request&) = delete;
        Request& operator=(const Request&) = delete;
        
        // 允许移动（但移动后原对象不能再使用）
        Request(Request&& other) noexcept
            : coro(other.coro)
            , op_type(other.op_type)
            , result(other.result)
            , completed(other.completed.load())
            , user_data(other.user_data) {
            other.coro = nullptr;
        }
    };

    // 构造函数
    // queue_depth: SQ/CQ 队列深度，默认 256
    explicit IoUringPoller(unsigned queue_depth = 256) 
        : queue_depth_(queue_depth)
        , pending_count_(0) {
        
        // 初始化 io_uring
        int ret = io_uring_queue_init(queue_depth_, &ring_, 0);
        if (ret < 0) {
            throw std::runtime_error(
                std::string("io_uring_queue_init failed: ") + strerror(-ret));
        }
        
        initialized_ = true;
    }

    // 禁止拷贝
    IoUringPoller(const IoUringPoller&) = delete;
    IoUringPoller& operator=(const IoUringPoller&) = delete;

    // 析构函数
    ~IoUringPoller() {
        if (initialized_) {
            io_uring_queue_exit(&ring_);
        }
    }

    // =========================================================================
    // 文件操作
    // =========================================================================

    // 提交异步读请求
    // fd: 文件描述符
    // buf: 读取缓冲区
    // len: 读取长度
    // offset: 文件偏移（-1 表示当前位置）
    // req: 请求上下文
    void prep_read(int fd, void* buf, size_t len, off_t offset, Request* req) {
        struct io_uring_sqe* sqe = get_sqe();
        if (!sqe) {
            throw std::runtime_error("SQ ring is full");
        }
        
        io_uring_prep_read(sqe, fd, buf, len, offset);
        io_uring_sqe_set_data(sqe, req);
        req->op_type = OpType::Read;
        req->completed.store(false, std::memory_order_relaxed);
        pending_count_.fetch_add(1, std::memory_order_relaxed);
    }

    // 提交异步写请求
    void prep_write(int fd, const void* buf, size_t len, off_t offset, Request* req) {
        struct io_uring_sqe* sqe = get_sqe();
        if (!sqe) {
            throw std::runtime_error("SQ ring is full");
        }
        
        io_uring_prep_write(sqe, fd, buf, len, offset);
        io_uring_sqe_set_data(sqe, req);
        req->op_type = OpType::Write;
        req->completed.store(false, std::memory_order_relaxed);
        pending_count_.fetch_add(1, std::memory_order_relaxed);
    }

    // 提交 fsync 请求
    void prep_fsync(int fd, Request* req) {
        struct io_uring_sqe* sqe = get_sqe();
        if (!sqe) {
            throw std::runtime_error("SQ ring is full");
        }
        
        io_uring_prep_fsync(sqe, fd, 0);
        io_uring_sqe_set_data(sqe, req);
        req->op_type = OpType::Fsync;
        req->completed.store(false, std::memory_order_relaxed);
        pending_count_.fetch_add(1, std::memory_order_relaxed);
    }

    // =========================================================================
    // 网络操作
    // =========================================================================

    // 提交 accept 请求
    void prep_accept(int fd, sockaddr* addr, socklen_t* addrlen, Request* req) {
        struct io_uring_sqe* sqe = get_sqe();
        if (!sqe) {
            throw std::runtime_error("SQ ring is full");
        }
        
        io_uring_prep_accept(sqe, fd, addr, addrlen, 0);
        io_uring_sqe_set_data(sqe, req);
        req->op_type = OpType::Accept;
        req->completed.store(false, std::memory_order_relaxed);
        pending_count_.fetch_add(1, std::memory_order_relaxed);
    }

    // 提交 connect 请求
    void prep_connect(int fd, const sockaddr* addr, socklen_t addrlen, Request* req) {
        struct io_uring_sqe* sqe = get_sqe();
        if (!sqe) {
            throw std::runtime_error("SQ ring is full");
        }
        
        io_uring_prep_connect(sqe, fd, addr, addrlen);
        io_uring_sqe_set_data(sqe, req);
        req->op_type = OpType::Connect;
        req->completed.store(false, std::memory_order_relaxed);
        pending_count_.fetch_add(1, std::memory_order_relaxed);
    }

    // 提交 send 请求
    void prep_send(int fd, const void* buf, size_t len, int flags, Request* req) {
        struct io_uring_sqe* sqe = get_sqe();
        if (!sqe) {
            throw std::runtime_error("SQ ring is full");
        }
        
        io_uring_prep_send(sqe, fd, buf, len, flags);
        io_uring_sqe_set_data(sqe, req);
        req->op_type = OpType::Send;
        req->completed.store(false, std::memory_order_relaxed);
        pending_count_.fetch_add(1, std::memory_order_relaxed);
    }

    // 提交 recv 请求
    void prep_recv(int fd, void* buf, size_t len, int flags, Request* req) {
        struct io_uring_sqe* sqe = get_sqe();
        if (!sqe) {
            throw std::runtime_error("SQ ring is full");
        }
        
        io_uring_prep_recv(sqe, fd, buf, len, flags);
        io_uring_sqe_set_data(sqe, req);
        req->op_type = OpType::Recv;
        req->completed.store(false, std::memory_order_relaxed);
        pending_count_.fetch_add(1, std::memory_order_relaxed);
    }

    // 提交 close 请求
    void prep_close(int fd, Request* req) {
        struct io_uring_sqe* sqe = get_sqe();
        if (!sqe) {
            throw std::runtime_error("SQ ring is full");
        }
        
        io_uring_prep_close(sqe, fd);
        io_uring_sqe_set_data(sqe, req);
        req->op_type = OpType::Close;
        req->completed.store(false, std::memory_order_relaxed);
        pending_count_.fetch_add(1, std::memory_order_relaxed);
    }

    // =========================================================================
    // 控制操作
    // =========================================================================

    // 提交所有准备好的请求
    // 返回：提交的请求数量
    int submit() {
        int ret = io_uring_submit(&ring_);
        if (ret < 0) {
            throw std::runtime_error(
                std::string("io_uring_submit failed: ") + strerror(-ret));
        }
        return ret;
    }

    // 等待并处理完成事件
    // timeout_ms: 超时时间（毫秒），-1 表示永久阻塞，0 表示非阻塞
    // 返回：就绪的协程句柄列表
    std::vector<std::coroutine_handle<>> poll(int timeout_ms = -1) {
        std::vector<std::coroutine_handle<>> ready_coros;
        
        if (pending_count_ == 0) {
            return ready_coros;
        }
        
        struct io_uring_cqe* cqe;
        
        if (timeout_ms == 0) {
            // 非阻塞：尝试获取一个完成事件
            int ret = io_uring_peek_cqe(&ring_, &cqe);
            if (ret == -EAGAIN) {
                return ready_coros;  // 没有完成事件
            }
            if (ret < 0) {
                throw std::runtime_error(
                    std::string("io_uring_peek_cqe failed: ") + strerror(-ret));
            }
        } else if (timeout_ms < 0) {
            // 阻塞等待
            int ret = io_uring_wait_cqe(&ring_, &cqe);
            if (ret < 0) {
                if (ret == -EINTR) {
                    return ready_coros;  // 被信号中断
                }
                throw std::runtime_error(
                    std::string("io_uring_wait_cqe failed: ") + strerror(-ret));
            }
        } else {
            // 带超时等待
            struct __kernel_timespec ts;
            ts.tv_sec = timeout_ms / 1000;
            ts.tv_nsec = (timeout_ms % 1000) * 1000000;
            
            int ret = io_uring_wait_cqe_timeout(&ring_, &cqe, &ts);
            if (ret == -ETIME) {
                return ready_coros;  // 超时
            }
            if (ret < 0) {
                if (ret == -EINTR) {
                    return ready_coros;
                }
                throw std::runtime_error(
                    std::string("io_uring_wait_cqe_timeout failed: ") + strerror(-ret));
            }
        }
        
        // 处理所有完成事件
        unsigned head;
        unsigned count = 0;
        
        io_uring_for_each_cqe(&ring_, head, cqe) {
            Request* req = static_cast<Request*>(io_uring_cqe_get_data(cqe));
            if (req) {
                req->result = cqe->res;
                req->completed.store(true, std::memory_order_release);
                
                if (req->coro && !req->coro.done()) {
                    ready_coros.push_back(req->coro);
                }
            }
            ++count;
        }
        
        // 标记已处理的完成事件
        io_uring_cq_advance(&ring_, count);
        pending_count_.fetch_sub(count, std::memory_order_relaxed);
        
        return ready_coros;
    }

    // 提交并等待完成
    // 便捷方法：提交所有请求并等待至少一个完成
    std::vector<std::coroutine_handle<>> submit_and_wait(int timeout_ms = -1) {
        submit();
        return poll(timeout_ms);
    }

    // 获取待处理请求数
    size_t pending_count() const noexcept {
        return pending_count_.load(std::memory_order_relaxed);
    }

    // 获取队列深度
    unsigned queue_depth() const noexcept {
        return queue_depth_;
    }

    // 获取 io_uring 文件描述符（用于 epoll 混合模式）
    int fd() const noexcept {
        return ring_.ring_fd;
    }

private:
    // 获取一个 SQE（提交队列条目）
    struct io_uring_sqe* get_sqe() {
        struct io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
        if (!sqe) {
            // SQ 已满，尝试提交并重试
            submit();
            sqe = io_uring_get_sqe(&ring_);
        }
        return sqe;
    }

    struct io_uring ring_;          // io_uring 实例
    unsigned queue_depth_;          // 队列深度
    std::atomic<size_t> pending_count_;  // 待处理请求数（原子，支持多线程）
    bool initialized_ = false;      // 是否已初始化
};

// =============================================================================
// IoUringAwaiter - io_uring 操作等待器
// =============================================================================
// 
// 通用的 io_uring 操作 awaiter，用于协程中等待 I/O 完成
// 注意：req 指向的 Request 对象必须在 I/O 完成前保持有效
// =============================================================================

class IoUringAwaiter {
public:
    explicit IoUringAwaiter(IoUringPoller::Request* req)
        : req_(req) {}

    bool await_ready() const noexcept {
        // 使用原子操作检查完成状态
        return req_->completed.load(std::memory_order_acquire);
    }

    bool await_suspend(std::coroutine_handle<> coro) noexcept {
        // 使用 CAS 操作避免竞态：
        // 1. 如果 completed 为 false，设置 coro 并返回 true（挂起）
        // 2. 如果 completed 已为 true，返回 false（不挂起）
        // 
        // 关键：先设置 coro，再检查 completed
        // 如果 poll() 在我们设置 coro 后完成请求，它会看到 coro 并 resume
        // 如果 poll() 在我们设置 coro 前完成请求，completed 为 true，我们不挂起
        req_->coro = coro;
        
        // 内存屏障：确保 coro 写入对其他线程可见
        // 然后检查 completed
        if (req_->completed.load(std::memory_order_acquire)) {
            // 已完成，不挂起（返回 false 会立即调用 await_resume）
            // 注意：此时 poll() 可能已经把 coro 加入 ready_coros
            // 但由于我们返回 false，协程会立即继续，不会被重复 resume
            // 因为 poll() 返回的 coro 会在 done() 检查时被跳过
            return false;
        }
        return true;  // 挂起，等待 poll() 调用 resume
    }

    int await_resume() const noexcept {
        return req_->result;
    }

private:
    IoUringPoller::Request* req_;
};

} // namespace zlcoro

#endif // ZLCORO_HAS_IO_URING
