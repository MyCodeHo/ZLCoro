#pragma once

#include "io_uring_file.hpp"
#include "io_uring_poller.hpp"

#ifdef ZLCORO_HAS_IO_URING

#include "zlcoro/core/task.hpp"
#include <string>
#include <vector>
#include <memory>
#include <cstring>
#include <sys/uio.h>

namespace zlcoro {

// =============================================================================
// WalWriter - WAL (Write-Ahead Log) 写入器
// =============================================================================
// 
// 专为 KV 存储设计的高性能 WAL 写入器，特性：
// - 支持同步和异步写入
// - 批量写入优化
// - fdatasync 替代 fsync（更快）
// - 支持对齐写入（Direct I/O 友好）
// 
// 使用示例:
//   WalWriter wal(&poller, "/data/wal.log");
//   co_await wal.append_sync("record1");  // 同步写入并持久化
//   
//   std::vector<std::string> batch = {"r1", "r2", "r3"};
//   co_await wal.append_batch(batch);     // 批量写入
// =============================================================================

class WalWriter {
public:
    // 构造函数
    WalWriter() : fd_(-1), poller_(nullptr), write_offset_(0) {}

    explicit WalWriter(IoUringPoller* poller)
        : fd_(-1), poller_(poller), write_offset_(0) {}

    // 打开 WAL 文件
    WalWriter(IoUringPoller* poller, const std::string& path,
              bool direct_io = false)
        : fd_(-1), poller_(poller), write_offset_(0) {
        open(path, direct_io);
    }

    // 禁止拷贝
    WalWriter(const WalWriter&) = delete;
    WalWriter& operator=(const WalWriter&) = delete;

    // 移动构造
    WalWriter(WalWriter&& other) noexcept
        : fd_(std::exchange(other.fd_, -1))
        , poller_(std::exchange(other.poller_, nullptr))
        , write_offset_(other.write_offset_) {}

    WalWriter& operator=(WalWriter&& other) noexcept {
        if (this != &other) {
            close_sync();
            fd_ = std::exchange(other.fd_, -1);
            poller_ = std::exchange(other.poller_, nullptr);
            write_offset_ = other.write_offset_;
        }
        return *this;
    }

    // 析构函数
    ~WalWriter() {
        close_sync();
    }

    // 设置 poller
    void set_poller(IoUringPoller* poller) {
        poller_ = poller;
    }

    // 打开 WAL 文件
    // direct_io: 是否使用 Direct I/O（绕过页缓存）
    void open(const std::string& path, bool direct_io = false) {
        close_sync();

        int flags = O_WRONLY | O_CREAT | O_APPEND;
        if (direct_io) {
            flags |= O_DIRECT;
        }

        fd_ = ::open(path.c_str(), flags, 0644);
        if (fd_ == -1) {
            throw std::runtime_error(
                std::string("Failed to open WAL file: ") + strerror(errno));
        }

        // 获取当前文件大小作为写入偏移
        struct stat st;
        if (fstat(fd_, &st) == 0) {
            write_offset_ = st.st_size;
        }
    }

    // 同步关闭
    void close_sync() {
        if (fd_ != -1) {
            ::close(fd_);
            fd_ = -1;
        }
    }

    // 检查是否打开
    bool is_open() const noexcept {
        return fd_ != -1;
    }

    // 获取当前写入偏移
    off_t write_offset() const noexcept {
        return write_offset_;
    }

    // =========================================================================
    // 写入操作
    // =========================================================================

    // 同步写入并持久化（写入 + fdatasync）
    // 这是最安全的写入方式，保证数据持久化
    Task<void> append_sync(const void* data, size_t len) {
        if (!is_open() || !poller_) {
            throw std::runtime_error("WAL not ready");
        }

        // 写入数据
        auto write_req = make_safe_request();
        poller_->prep_write(fd_, data, len, write_offset_, write_req.get());
        poller_->submit();
        co_await SafeIoUringAwaiter(write_req);

        if (write_req->result < 0) {
            throw std::runtime_error(
                std::string("WAL write failed: ") + strerror(-write_req->result));
        }

        write_offset_ += write_req->result;

        // 持久化（fdatasync 比 fsync 更快）
        auto sync_req = make_safe_request();
        poller_->prep_fdatasync(fd_, sync_req.get());
        poller_->submit();
        co_await SafeIoUringAwaiter(sync_req);

        if (sync_req->result < 0) {
            throw std::runtime_error(
                std::string("WAL fdatasync failed: ") + strerror(-sync_req->result));
        }

        co_return;
    }

    // 字符串版本
    Task<void> append_sync(const std::string& data) {
        co_await append_sync(data.data(), data.size());
    }

    // 异步写入（不等待持久化）
    // 适合批量写入后统一 sync
    Task<ssize_t> append_async(const void* data, size_t len) {
        if (!is_open() || !poller_) {
            throw std::runtime_error("WAL not ready");
        }

        auto req = make_safe_request();
        poller_->prep_write(fd_, data, len, write_offset_, req.get());
        poller_->submit();
        co_await SafeIoUringAwaiter(req);

        if (req->result > 0) {
            write_offset_ += req->result;
        }

        co_return req->result;
    }

    // 批量写入（使用 writev）
    // 所有记录一次系统调用写入，然后一次 fdatasync
    Task<void> append_batch(const std::vector<std::pair<const void*, size_t>>& entries) {
        if (!is_open() || !poller_) {
            throw std::runtime_error("WAL not ready");
        }

        if (entries.empty()) {
            co_return;
        }

        // 构建 iovec 数组
        std::vector<struct iovec> iovs;
        iovs.reserve(entries.size());
        
        for (const auto& [data, len] : entries) {
            struct iovec iov;
            iov.iov_base = const_cast<void*>(data);
            iov.iov_len = len;
            iovs.push_back(iov);
        }

        // 写入所有数据
        auto write_req = make_safe_request();
        poller_->prep_writev(fd_, iovs.data(), iovs.size(), write_offset_, write_req.get());
        poller_->submit();
        co_await SafeIoUringAwaiter(write_req);

        if (write_req->result < 0) {
            throw std::runtime_error(
                std::string("WAL writev failed: ") + strerror(-write_req->result));
        }

        write_offset_ += write_req->result;

        // 持久化
        auto sync_req = make_safe_request();
        poller_->prep_fdatasync(fd_, sync_req.get());
        poller_->submit();
        co_await SafeIoUringAwaiter(sync_req);

        if (sync_req->result < 0) {
            throw std::runtime_error(
                std::string("WAL fdatasync failed: ") + strerror(-sync_req->result));
        }

        co_return;
    }

    // 字符串批量版本
    Task<void> append_batch(const std::vector<std::string>& entries) {
        std::vector<std::pair<const void*, size_t>> raw_entries;
        raw_entries.reserve(entries.size());
        
        for (const auto& entry : entries) {
            raw_entries.emplace_back(entry.data(), entry.size());
        }
        
        co_await append_batch(raw_entries);
    }

    // 手动触发持久化
    Task<int> sync() {
        if (!is_open() || !poller_) {
            throw std::runtime_error("WAL not ready");
        }

        auto req = make_safe_request();
        poller_->prep_fdatasync(fd_, req.get());
        poller_->submit();
        co_await SafeIoUringAwaiter(req);
        co_return req->result;
    }

    // 完整 fsync（包括元数据）
    Task<int> full_sync() {
        if (!is_open() || !poller_) {
            throw std::runtime_error("WAL not ready");
        }

        auto req = make_safe_request();
        poller_->prep_fsync(fd_, req.get());
        poller_->submit();
        co_await SafeIoUringAwaiter(req);
        co_return req->result;
    }

private:
    int fd_;                // 文件描述符
    IoUringPoller* poller_; // io_uring 轮询器
    off_t write_offset_;    // 当前写入偏移
};

} // namespace zlcoro

#endif // ZLCORO_HAS_IO_URING
