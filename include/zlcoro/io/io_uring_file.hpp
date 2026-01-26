#pragma once

#include "io_uring_poller.hpp"

#ifdef ZLCORO_HAS_IO_URING

#include "zlcoro/core/task.hpp"
#include <string>
#include <memory>
#include <vector>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <cstring>

namespace zlcoro {

// =============================================================================
// IoUringFile - 基于 io_uring 的异步文件操作
// =============================================================================
// 
// 使用 io_uring 实现真正的异步文件 I/O，相比线程池方案：
// - 无需线程切换开销
// - 内核直接执行 I/O
// - 支持批量提交
// =============================================================================

class IoUringFile {
public:
    // 打开模式
    enum OpenMode {
        ReadOnly = O_RDONLY,
        WriteOnly = O_WRONLY,
        ReadWrite = O_RDWR,
        Create = O_CREAT,
        Truncate = O_TRUNC,
        Append = O_APPEND,
        Direct = O_DIRECT  // 直接 I/O，绕过页缓存
    };

    // 构造函数
    IoUringFile() : fd_(-1), poller_(nullptr) {}

    // 带 poller 的构造函数
    explicit IoUringFile(IoUringPoller* poller) 
        : fd_(-1), poller_(poller) {}

    // 打开文件的构造函数
    IoUringFile(IoUringPoller* poller, const std::string& path, 
                int mode = ReadOnly, int perms = 0644)
        : fd_(-1), poller_(poller) {
        open(path, mode, perms);
    }

    // 禁止拷贝
    IoUringFile(const IoUringFile&) = delete;
    IoUringFile& operator=(const IoUringFile&) = delete;

    // 移动构造
    IoUringFile(IoUringFile&& other) noexcept 
        : fd_(std::exchange(other.fd_, -1))
        , poller_(std::exchange(other.poller_, nullptr)) {}

    IoUringFile& operator=(IoUringFile&& other) noexcept {
        if (this != &other) {
            close_sync();
            fd_ = std::exchange(other.fd_, -1);
            poller_ = std::exchange(other.poller_, nullptr);
        }
        return *this;
    }

    // 析构函数
    ~IoUringFile() {
        close_sync();  // 同步关闭，避免异步问题
    }

    // 设置 poller
    void set_poller(IoUringPoller* poller) {
        poller_ = poller;
    }

    // 同步打开文件
    void open(const std::string& path, int mode = ReadOnly, int perms = 0644) {
        close_sync();
        
        fd_ = ::open(path.c_str(), mode, perms);
        if (fd_ == -1) {
            throw std::runtime_error(
                std::string("Failed to open file: ") + strerror(errno));
        }
    }

    // 同步关闭文件
    void close_sync() {
        if (fd_ != -1) {
            ::close(fd_);
            fd_ = -1;
        }
    }

    // 检查文件是否打开
    bool is_open() const noexcept {
        return fd_ != -1;
    }

    // 获取文件描述符
    int fd() const noexcept {
        return fd_;
    }

    // =========================================================================
    // 异步操作
    // =========================================================================

    // 异步读取数据
    // buf: 读取缓冲区
    // len: 读取长度
    // offset: 文件偏移（-1 表示当前位置）
    // 返回: 实际读取的字节数，0 表示 EOF，负数表示错误
    Task<ssize_t> read(void* buf, size_t len, off_t offset = -1) {
        if (!is_open()) {
            throw std::runtime_error("File not open");
        }
        if (!poller_) {
            throw std::runtime_error("No IoUringPoller set");
        }

        // 使用 shared_ptr 管理 Request，确保生命周期安全
        auto req = make_safe_request();
        poller_->prep_read(fd_, buf, len, offset, req.get());
        poller_->submit();

        co_await SafeIoUringAwaiter(req);
        co_return req->result;
    }

    // 异步读取到字符串
    Task<std::string> read_string(size_t len, off_t offset = -1) {
        std::string buffer(len, '\0');
        
        ssize_t n = co_await read(buffer.data(), len, offset);
        if (n < 0) {
            throw std::runtime_error(
                std::string("read failed: ") + strerror(-n));
        }
        
        buffer.resize(n);
        co_return buffer;
    }

    // 异步读取整个文件
    Task<std::string> read_all() {
        if (!is_open()) {
            throw std::runtime_error("File not open");
        }

        // 获取文件大小
        struct stat st;
        if (fstat(fd_, &st) == -1) {
            throw std::runtime_error(
                std::string("fstat failed: ") + strerror(errno));
        }

        if (st.st_size == 0) {
            co_return std::string();
        }

        co_return co_await read_string(st.st_size, 0);
    }

    // 异步写入数据
    // buf: 写入缓冲区
    // len: 写入长度
    // offset: 文件偏移（-1 表示当前位置）
    // 返回: 实际写入的字节数，负数表示错误
    Task<ssize_t> write(const void* buf, size_t len, off_t offset = -1) {
        if (!is_open()) {
            throw std::runtime_error("File not open");
        }
        if (!poller_) {
            throw std::runtime_error("No IoUringPoller set");
        }

        // 使用 shared_ptr 管理 Request，确保生命周期安全
        auto req = make_safe_request();
        poller_->prep_write(fd_, buf, len, offset, req.get());
        poller_->submit();

        co_await SafeIoUringAwaiter(req);
        co_return req->result;
    }

    // 异步写入字符串
    Task<ssize_t> write_string(const std::string& data, off_t offset = -1) {
        co_return co_await write(data.data(), data.size(), offset);
    }

    // 异步写入所有数据（循环写入直到完成）
    Task<size_t> write_all(const void* buf, size_t len, off_t offset = -1) {
        const char* ptr = static_cast<const char*>(buf);
        size_t total = 0;
        off_t current_offset = offset;

        while (total < len) {
            ssize_t n = co_await write(ptr + total, len - total, current_offset);
            if (n < 0) {
                throw std::runtime_error(
                    std::string("write failed: ") + strerror(-n));
            }
            if (n == 0) {
                break;  // 无法继续写入
            }
            total += n;
            if (current_offset >= 0) {
                current_offset += n;
            }
        }

        co_return total;
    }

    // 异步写入整个字符串
    Task<size_t> write_all_string(const std::string& data, off_t offset = -1) {
        co_return co_await write_all(data.data(), data.size(), offset);
    }

    // 异步 fsync
    Task<int> fsync() {
        if (!is_open()) {
            throw std::runtime_error("File not open");
        }
        if (!poller_) {
            throw std::runtime_error("No IoUringPoller set");
        }

        // 使用 shared_ptr 管理 Request，确保生命周期安全
        auto req = make_safe_request();
        poller_->prep_fsync(fd_, req.get());
        poller_->submit();

        co_await SafeIoUringAwaiter(req);
        co_return req->result;
    }

    // 异步 fdatasync（只同步数据，不同步元数据）
    // 比 fsync 更快，适合 WAL 写入
    Task<int> fdatasync() {
        if (!is_open()) {
            throw std::runtime_error("File not open");
        }
        if (!poller_) {
            throw std::runtime_error("No IoUringPoller set");
        }

        auto req = make_safe_request();
        poller_->prep_fdatasync(fd_, req.get());
        poller_->submit();

        co_await SafeIoUringAwaiter(req);
        co_return req->result;
    }

    // =========================================================================
    // Vectored I/O (readv/writev)
    // =========================================================================
    // 用于批量读写多个不连续的缓冲区，减少系统调用次数
    // 对于 KV 存储的 SST 文件读写非常有用

    // 异步向量读
    // iovs: iovec 数组（必须在操作完成前保持有效）
    // nr_vecs: iovec 数量
    // offset: 文件偏移
    Task<ssize_t> readv(const struct iovec* iovs, unsigned nr_vecs, off_t offset) {
        if (!is_open()) {
            throw std::runtime_error("File not open");
        }
        if (!poller_) {
            throw std::runtime_error("No IoUringPoller set");
        }

        auto req = make_safe_request();
        poller_->prep_readv(fd_, iovs, nr_vecs, offset, req.get());
        poller_->submit();

        co_await SafeIoUringAwaiter(req);
        co_return req->result;
    }

    // 异步向量写
    Task<ssize_t> writev(const struct iovec* iovs, unsigned nr_vecs, off_t offset) {
        if (!is_open()) {
            throw std::runtime_error("File not open");
        }
        if (!poller_) {
            throw std::runtime_error("No IoUringPoller set");
        }

        auto req = make_safe_request();
        poller_->prep_writev(fd_, iovs, nr_vecs, offset, req.get());
        poller_->submit();

        co_await SafeIoUringAwaiter(req);
        co_return req->result;
    }

    // 便捷版本：使用 vector<iovec>
    Task<ssize_t> readv(std::vector<struct iovec>& iovs, off_t offset) {
        co_return co_await readv(iovs.data(), iovs.size(), offset);
    }

    Task<ssize_t> writev(std::vector<struct iovec>& iovs, off_t offset) {
        co_return co_await writev(iovs.data(), iovs.size(), offset);
    }

    // 同步移动文件指针
    off_t seek(off_t offset, int whence = SEEK_SET) {
        if (!is_open()) {
            throw std::runtime_error("File not open");
        }

        off_t pos = lseek(fd_, offset, whence);
        if (pos == -1) {
            throw std::runtime_error(
                std::string("lseek failed: ") + strerror(errno));
        }

        return pos;
    }

private:
    int fd_;                // 文件描述符
    IoUringPoller* poller_; // io_uring 轮询器
};

// =============================================================================
// IoUringEventLoop - 简单的 io_uring 事件循环
// =============================================================================
// 
// 单线程事件循环，用于驱动 io_uring 协程
// =============================================================================

class IoUringEventLoop {
public:
    explicit IoUringEventLoop(unsigned queue_depth = 256)
        : poller_(queue_depth), running_(false) {}

    // 获取 poller
    IoUringPoller& poller() { return poller_; }
    const IoUringPoller& poller() const { return poller_; }

    // 运行事件循环直到没有待处理事件
    void run() {
        running_ = true;
        
        while (running_ && poller_.pending_count() > 0) {
            auto ready = poller_.poll(100);  // 100ms 超时
            
            for (auto& coro : ready) {
                if (coro && !coro.done()) {
                    coro.resume();
                }
            }
        }
        
        running_ = false;
    }

    // 运行一次（非阻塞）
    void run_once() {
        auto ready = poller_.poll(0);
        
        for (auto& coro : ready) {
            if (coro && !coro.done()) {
                coro.resume();
            }
        }
    }

    // 停止事件循环
    void stop() {
        running_ = false;
    }

    // 是否正在运行
    bool is_running() const noexcept {
        return running_;
    }

private:
    IoUringPoller poller_;
    bool running_;
};

// =============================================================================
// 便捷函数
// =============================================================================

// 使用 io_uring 异步读取整个文件
inline Task<std::string> uring_read_file(IoUringPoller* poller, const std::string& path) {
    IoUringFile file(poller, path, IoUringFile::ReadOnly);
    co_return co_await file.read_all();
}

// 使用 io_uring 异步写入整个文件
inline Task<void> uring_write_file(IoUringPoller* poller, const std::string& path, 
                                   const std::string& content) {
    IoUringFile file(poller, path, 
                     IoUringFile::WriteOnly | IoUringFile::Create | IoUringFile::Truncate);
    co_await file.write_all_string(content);
    co_await file.fsync();
    co_return;
}

} // namespace zlcoro

#endif // ZLCORO_HAS_IO_URING
