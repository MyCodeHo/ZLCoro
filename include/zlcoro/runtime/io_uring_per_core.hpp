#pragma once

#include "per_core_event_loop.hpp"

#ifdef __linux__
#include <linux/version.h>
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 1, 0)
#define ZLCORO_HAS_IO_URING 1
#endif
#endif

#ifdef ZLCORO_HAS_IO_URING

#include <liburing.h>
#include <sys/eventfd.h>
#include <sys/poll.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdexcept>
#include <cstring>
#include <memory>
#include <atomic>

namespace zlcoro {

// =============================================================================
// IoUringPerCoreEventLoop - 基于 io_uring 的每核心事件循环
// =============================================================================
//
// 特点：
// - 使用 io_uring 作为 I/O 后端（真正的异步 I/O）
// - 支持文件和网络操作的统一处理
// - 批量提交 SQE，减少系统调用
// - 完全在单线程内运行，无锁
//
// 优势（相比 epoll）：
// - 文件 I/O 性能约 5x 提升
// - 零拷贝，内核直接 DMA
// - 统一的文件和网络 API
// =============================================================================

class IoUringPerCoreEventLoop : public PerCoreEventLoop {
public:
    // 最大事件数
    static constexpr unsigned MAX_CQE_BATCH = 128;
    static constexpr unsigned DEFAULT_QUEUE_DEPTH = 256;

    // 操作类型
    enum class OpType : uint8_t {
        None,
        Read,
        Write,
        ReadV,
        WriteV,
        Fsync,
        FdataSync,
        Accept,
        Connect,
        Send,
        Recv,
        Close,
        Poll,       // 等待 fd 就绪（用于兼容 epoll 语义）
        Wakeup      // 唤醒操作
    };

    // 操作请求
    struct Request {
        std::coroutine_handle<> coro;   // 完成时恢复的协程
        OpType op_type = OpType::None;  // 操作类型
        int result = 0;                 // 操作结果
        std::atomic<bool> completed{false};
        void* user_data = nullptr;
        
        Request() = default;
        Request(const Request&) = delete;
        Request& operator=(const Request&) = delete;
    };

    explicit IoUringPerCoreEventLoop(unsigned queue_depth = DEFAULT_QUEUE_DEPTH) 
        : queue_depth_(queue_depth)
        , pending_count_(0)
        , wakeup_fd_(-1) {
        
        // 初始化 io_uring
        int ret = io_uring_queue_init(queue_depth_, &ring_, 0);
        if (ret < 0) {
            throw std::runtime_error(
                std::string("io_uring_queue_init failed: ") + strerror(-ret));
        }
        
        // 创建 eventfd 用于唤醒
        wakeup_fd_ = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        if (wakeup_fd_ < 0) {
            io_uring_queue_exit(&ring_);
            throw std::runtime_error("Failed to create eventfd: " + 
                                    std::string(strerror(errno)));
        }
        
        // 提交一个持久的 poll 操作来监听 wakeup_fd
        submit_wakeup_poll();
    }

    ~IoUringPerCoreEventLoop() override {
        if (wakeup_fd_ >= 0) ::close(wakeup_fd_);
        io_uring_queue_exit(&ring_);
    }

    // =========================================================================
    // 后端类型
    // =========================================================================

    Backend backend() const override { return Backend::IoUring; }

    // =========================================================================
    // I/O 事件注册（使用 POLL 操作实现兼容 epoll 语义）
    // =========================================================================

    // 注册读事件
    void register_read(int fd, std::coroutine_handle<> coro) override {
        submit_poll(fd, POLLIN, coro);
    }

    // 注册写事件
    void register_write(int fd, std::coroutine_handle<> coro) override {
        submit_poll(fd, POLLOUT, coro);
    }

    // 注册读写事件
    void register_rw(int fd, std::coroutine_handle<> coro) override {
        submit_poll(fd, POLLIN | POLLOUT, coro);
    }

    // 取消注册
    void unregister(int fd) override {
        // io_uring 会自动清理已完成的请求
        // 这里只需要从本地映射中移除
        auto it = fd_requests_.find(fd);
        if (it != fd_requests_.end()) {
            // 取消 pending 的 poll 操作
            cancel_request(it->second.get());
            fd_requests_.erase(it);
        }
    }

    // 检查 fd 是否已注册
    bool has_fd(int fd) const override {
        return fd_requests_.find(fd) != fd_requests_.end();
    }

    // =========================================================================
    // 原生 io_uring 操作（高性能路径）
    // =========================================================================

    // 异步读取
    std::shared_ptr<Request> prep_read(int fd, void* buf, size_t len, off_t offset) {
        auto req = std::make_shared<Request>();
        req->op_type = OpType::Read;
        
        struct io_uring_sqe* sqe = get_sqe();
        io_uring_prep_read(sqe, fd, buf, len, offset);
        io_uring_sqe_set_data(sqe, req.get());
        
        pending_requests_.push_back(req);
        pending_count_++;
        return req;
    }

    // 异步写入
    std::shared_ptr<Request> prep_write(int fd, const void* buf, size_t len, off_t offset) {
        auto req = std::make_shared<Request>();
        req->op_type = OpType::Write;
        
        struct io_uring_sqe* sqe = get_sqe();
        io_uring_prep_write(sqe, fd, buf, len, offset);
        io_uring_sqe_set_data(sqe, req.get());
        
        pending_requests_.push_back(req);
        pending_count_++;
        return req;
    }

    // 向量读取 (readv)
    std::shared_ptr<Request> prep_readv(int fd, const struct iovec* iovs, 
                                        unsigned nr_vecs, off_t offset) {
        auto req = std::make_shared<Request>();
        req->op_type = OpType::ReadV;
        
        struct io_uring_sqe* sqe = get_sqe();
        io_uring_prep_readv(sqe, fd, iovs, nr_vecs, offset);
        io_uring_sqe_set_data(sqe, req.get());
        
        pending_requests_.push_back(req);
        pending_count_++;
        return req;
    }

    // 向量写入 (writev)
    std::shared_ptr<Request> prep_writev(int fd, const struct iovec* iovs, 
                                         unsigned nr_vecs, off_t offset) {
        auto req = std::make_shared<Request>();
        req->op_type = OpType::WriteV;
        
        struct io_uring_sqe* sqe = get_sqe();
        io_uring_prep_writev(sqe, fd, iovs, nr_vecs, offset);
        io_uring_sqe_set_data(sqe, req.get());
        
        pending_requests_.push_back(req);
        pending_count_++;
        return req;
    }

    // fsync
    std::shared_ptr<Request> prep_fsync(int fd) {
        auto req = std::make_shared<Request>();
        req->op_type = OpType::Fsync;
        
        struct io_uring_sqe* sqe = get_sqe();
        io_uring_prep_fsync(sqe, fd, 0);
        io_uring_sqe_set_data(sqe, req.get());
        
        pending_requests_.push_back(req);
        pending_count_++;
        return req;
    }

    // fdatasync（只同步数据，不同步元数据）
    std::shared_ptr<Request> prep_fdatasync(int fd) {
        auto req = std::make_shared<Request>();
        req->op_type = OpType::FdataSync;
        
        struct io_uring_sqe* sqe = get_sqe();
        io_uring_prep_fsync(sqe, fd, IORING_FSYNC_DATASYNC);
        io_uring_sqe_set_data(sqe, req.get());
        
        pending_requests_.push_back(req);
        pending_count_++;
        return req;
    }

    // 接受连接
    std::shared_ptr<Request> prep_accept(int fd, sockaddr* addr, socklen_t* addrlen) {
        auto req = std::make_shared<Request>();
        req->op_type = OpType::Accept;
        
        struct io_uring_sqe* sqe = get_sqe();
        io_uring_prep_accept(sqe, fd, addr, addrlen, 0);
        io_uring_sqe_set_data(sqe, req.get());
        
        pending_requests_.push_back(req);
        pending_count_++;
        return req;
    }

    // 发起连接
    std::shared_ptr<Request> prep_connect(int fd, const sockaddr* addr, socklen_t addrlen) {
        auto req = std::make_shared<Request>();
        req->op_type = OpType::Connect;
        
        struct io_uring_sqe* sqe = get_sqe();
        io_uring_prep_connect(sqe, fd, addr, addrlen);
        io_uring_sqe_set_data(sqe, req.get());
        
        pending_requests_.push_back(req);
        pending_count_++;
        return req;
    }

    // 发送数据
    std::shared_ptr<Request> prep_send(int fd, const void* buf, size_t len, int flags = 0) {
        auto req = std::make_shared<Request>();
        req->op_type = OpType::Send;
        
        struct io_uring_sqe* sqe = get_sqe();
        io_uring_prep_send(sqe, fd, buf, len, flags);
        io_uring_sqe_set_data(sqe, req.get());
        
        pending_requests_.push_back(req);
        pending_count_++;
        return req;
    }

    // 接收数据
    std::shared_ptr<Request> prep_recv(int fd, void* buf, size_t len, int flags = 0) {
        auto req = std::make_shared<Request>();
        req->op_type = OpType::Recv;
        
        struct io_uring_sqe* sqe = get_sqe();
        io_uring_prep_recv(sqe, fd, buf, len, flags);
        io_uring_sqe_set_data(sqe, req.get());
        
        pending_requests_.push_back(req);
        pending_count_++;
        return req;
    }

    // 关闭文件
    std::shared_ptr<Request> prep_close(int fd) {
        auto req = std::make_shared<Request>();
        req->op_type = OpType::Close;
        
        struct io_uring_sqe* sqe = get_sqe();
        io_uring_prep_close(sqe, fd);
        io_uring_sqe_set_data(sqe, req.get());
        
        pending_requests_.push_back(req);
        pending_count_++;
        return req;
    }

    // 提交所有准备的操作
    int submit() {
        int ret = io_uring_submit(&ring_);
        if (ret < 0 && ret != -EBUSY) {
            throw std::runtime_error(
                std::string("io_uring_submit failed: ") + strerror(-ret));
        }
        return ret;
    }

    // =========================================================================
    // 获取原生句柄
    // =========================================================================

    struct io_uring* ring() noexcept { return &ring_; }
    int ring_fd() const noexcept { return ring_.ring_fd; }
    unsigned queue_depth() const noexcept { return queue_depth_; }
    size_t pending_count() const noexcept { return pending_count_; }

protected:
    // 轮询 I/O 事件
    void poll_events(int timeout_ms) override {
        // 先提交所有准备的操作
        int submitted = io_uring_submit(&ring_);
        (void)submitted;
        
        struct io_uring_cqe* cqe;
        struct __kernel_timespec ts;
        
        if (timeout_ms == 0) {
            // 非阻塞 - 快速检查
            while (io_uring_peek_cqe(&ring_, &cqe) == 0) {
                process_cqe(cqe);
                io_uring_cqe_seen(&ring_, cqe);
            }
            return;
        } else if (timeout_ms < 0) {
            // 阻塞等待
            int ret = io_uring_wait_cqe(&ring_, &cqe);
            if (ret < 0) return;
        } else {
            // 带超时等待
            ts.tv_sec = timeout_ms / 1000;
            ts.tv_nsec = (timeout_ms % 1000) * 1000000LL;
            
            int ret = io_uring_wait_cqe_timeout(&ring_, &cqe, &ts);
            if (ret < 0) return;
        }
        
        // 处理第一个 CQE
        if (cqe) {
            process_cqe(cqe);
            io_uring_cqe_seen(&ring_, cqe);
        }
        
        // 非阻塞收集剩余的 CQE
        while (io_uring_peek_cqe(&ring_, &cqe) == 0) {
            process_cqe(cqe);
            io_uring_cqe_seen(&ring_, cqe);
        }
    }
    
    // 处理单个 CQE
    void process_cqe(struct io_uring_cqe* cqe) {
        Request* req = static_cast<Request*>(io_uring_cqe_get_data(cqe));
        if (!req) return;
        
        req->result = cqe->res;
        req->completed.store(true, std::memory_order_release);
        
        if (req->op_type == OpType::Wakeup) {
            // 处理 wakeup
            if (cqe->res >= 0) {
                uint64_t val;
                [[maybe_unused]] auto _ = ::read(wakeup_fd_, &val, sizeof(val));
            }
            submit_wakeup_poll();
        } else if (req->coro && !req->coro.done()) {
            // 立即唤醒协程
            req->coro.resume();
        }
        
        if (pending_count_ > 0) pending_count_--;
    }

    // 唤醒阻塞的 poll
    void wakeup() override {
        uint64_t val = 1;
        [[maybe_unused]] auto _ = ::write(wakeup_fd_, &val, sizeof(val));
    }

private:
    // 获取 SQE
    struct io_uring_sqe* get_sqe() {
        struct io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
        if (!sqe) {
            // SQ 已满，提交并重试
            submit();
            sqe = io_uring_get_sqe(&ring_);
            if (!sqe) {
                throw std::runtime_error("io_uring SQ ring is full");
            }
        }
        return sqe;
    }

    // 提交 poll 操作（兼容 epoll 语义）
    void submit_poll(int fd, short events, std::coroutine_handle<> coro) {
        auto req = std::make_shared<Request>();
        req->op_type = OpType::Poll;
        req->coro = coro;
        
        struct io_uring_sqe* sqe = get_sqe();
        io_uring_prep_poll_add(sqe, fd, events);
        io_uring_sqe_set_data(sqe, req.get());
        
        fd_requests_[fd] = req;
        pending_requests_.push_back(req);
        pending_count_++;
    }

    // 提交 wakeup poll
    void submit_wakeup_poll() {
        wakeup_request_.op_type = OpType::Wakeup;
        wakeup_request_.completed.store(false, std::memory_order_relaxed);
        
        struct io_uring_sqe* sqe = get_sqe();
        io_uring_prep_poll_add(sqe, wakeup_fd_, POLLIN);
        io_uring_sqe_set_data(sqe, &wakeup_request_);
        pending_count_++;
    }

    // 取消请求
    void cancel_request(Request* req) {
        struct io_uring_sqe* sqe = get_sqe();
        io_uring_prep_cancel(sqe, req, 0);
        io_uring_sqe_set_data(sqe, nullptr);
    }

    // 清理已完成的请求
    void cleanup_completed_requests() {
        pending_requests_.erase(
            std::remove_if(pending_requests_.begin(), pending_requests_.end(),
                [](const std::shared_ptr<Request>& req) {
                    return req->completed.load(std::memory_order_acquire);
                }),
            pending_requests_.end());
        
        for (auto it = fd_requests_.begin(); it != fd_requests_.end(); ) {
            if (it->second->completed.load(std::memory_order_acquire)) {
                it = fd_requests_.erase(it);
            } else {
                ++it;
            }
        }
    }

private:
    struct io_uring ring_;
    unsigned queue_depth_;
    size_t pending_count_;
    int wakeup_fd_;
    Request wakeup_request_;
    
    // 待处理的请求（保持 shared_ptr 活跃）
    std::vector<std::shared_ptr<Request>> pending_requests_;
    
    // fd -> Request 映射（用于取消和查询）
    std::map<int, std::shared_ptr<Request>> fd_requests_;
};

// =============================================================================
// io_uring Awaiter（用于 IoUringPerCoreEventLoop）
// =============================================================================

class IoUringRequestAwaiter {
public:
    explicit IoUringRequestAwaiter(std::shared_ptr<IoUringPerCoreEventLoop::Request> req)
        : req_(std::move(req)) {}

    bool await_ready() const noexcept {
        // 如果已完成，直接返回 true，不需要挂起
        return req_->completed.load(std::memory_order_acquire);
    }

    void await_suspend(std::coroutine_handle<> coro) noexcept {
        // 关键：先设置 coro，这样 poll_events 中可以唤醒它
        req_->coro = coro;
        // 使用 release 保证 coro 的写入对其他线程可见
        // 注意：由于是单线程事件循环，这里主要是保证顺序
    }

    int await_resume() const noexcept {
        return req_->result;
    }

private:
    std::shared_ptr<IoUringPerCoreEventLoop::Request> req_;
};

// 创建 awaiter 的便捷函数
inline IoUringRequestAwaiter make_awaiter(std::shared_ptr<IoUringPerCoreEventLoop::Request> req) {
    return IoUringRequestAwaiter(std::move(req));
}

} // namespace zlcoro

#endif // ZLCORO_HAS_IO_URING
