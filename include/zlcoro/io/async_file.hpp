#pragma once

#include "zlcoro/core/task.hpp"
#include "zlcoro/scheduler/scheduler.hpp"
#include <string>
#include <fstream>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <cstring>
#include <memory>
#include <utility>

namespace zlcoro {

// =============================================================================
// AsyncFile - 异步文件操作
// =============================================================================
// 
// 提供异步文件读写接口。
// 注意：由于 Linux AIO 较复杂，这里使用线程池模拟异步操作。
// =============================================================================

class AsyncFile {
public:
    // 打开模式
    enum OpenMode {
        ReadOnly = O_RDONLY,
        WriteOnly = O_WRONLY,
        ReadWrite = O_RDWR,
        Create = O_CREAT,
        Truncate = O_TRUNC,
        Append = O_APPEND
    };

    // 构造函数
    AsyncFile() : fd_(-1) {}

    explicit AsyncFile(const std::string& path, int mode = ReadOnly, int perms = 0644)
        : fd_(-1) {
        open(path, mode, perms);
    }

    // 禁止拷贝
    AsyncFile(const AsyncFile&) = delete;
    AsyncFile& operator=(const AsyncFile&) = delete;

    // 移动构造
    AsyncFile(AsyncFile&& other) noexcept : fd_(std::exchange(other.fd_, -1)) {
    }

    AsyncFile& operator=(AsyncFile&& other) noexcept {
        if (this != &other) {
            close();
            fd_ = std::exchange(other.fd_, -1);
        }
        return *this;
    }

    // 析构函数
    ~AsyncFile() {
        close();
    }

    // 打开文件
    void open(const std::string& path, int mode = ReadOnly, int perms = 0644) {
        close();
        
        fd_ = ::open(path.c_str(), mode, perms);
        if (fd_ == -1) {
            throw std::runtime_error(
                std::string("Failed to open file: ") + strerror(errno));
        }
    }

    // 关闭文件
    void close() {
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

    // 同步读取所有内容（调用者决定是否需要调度）
    std::string read_all() {
        if (!is_open()) {
            throw std::runtime_error("File not open");
        }
        
        // 移动到文件开头
        lseek(fd_, 0, SEEK_SET);
        
        // 获取文件大小
        struct stat st;
        if (fstat(fd_, &st) == -1) {
            throw std::runtime_error(
                std::string("fstat failed: ") + strerror(errno));
        }
        
        size_t file_size = st.st_size;
        
        std::string content;
        if (file_size > 0) {
            content.resize(file_size);
            
            ssize_t n = ::read(fd_, content.data(), file_size);
            if (n == -1) {
                throw std::runtime_error(
                    std::string("read failed: ") + strerror(errno));
            }
            
            content.resize(n);
        }
        
        return content;
    }

    // 同步读取指定字节数（调用者决定是否需要调度）
    std::string read(size_t count) {
        if (!is_open()) {
            throw std::runtime_error("File not open");
        }
        
        std::string buffer;
        buffer.resize(count);
        
        ssize_t n = ::read(fd_, buffer.data(), count);
        if (n == -1) {
            throw std::runtime_error(
                std::string("read failed: ") + strerror(errno));
        }
        
        buffer.resize(n);
        return buffer;
    }

    // 同步写入数据（调用者决定是否需要调度）
    size_t write(const std::string& data) {
        if (!is_open()) {
            throw std::runtime_error("File not open");
        }
        
        ssize_t n = ::write(fd_, data.data(), data.size());
        if (n == -1) {
            throw std::runtime_error(
                std::string("write failed: ") + strerror(errno));
        }
        
        return static_cast<size_t>(n);
    }

    // 同步写入数据（原始指针版本，调用者决定是否需要调度）
    size_t write(const char* data, size_t len) {
        if (!is_open()) {
            throw std::runtime_error("File not open");
        }
        
        ssize_t n = ::write(fd_, data, len);
        if (n == -1) {
            throw std::runtime_error(
                std::string("write failed: ") + strerror(errno));
        }
        
        return static_cast<size_t>(n);
    }

    // 同步刷新到磁盘（调用者决定是否需要调度）
    void sync() {
        if (!is_open()) {
            throw std::runtime_error("File not open");
        }
        
        if (fsync(fd_) == -1) {
            throw std::runtime_error(
                std::string("fsync failed: ") + strerror(errno));
        }
    }

    // 移动文件指针（调用者决定是否需要调度）
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
    int fd_;  // 文件描述符
};

// =============================================================================
// 便捷函数
// =============================================================================

// 异步读取整个文件
// 注意：此函数必须通过 async_run() 调用，不要直接 co_await
inline Task<std::string> read_file(const std::string& path) {
    // 同步读取文件（在线程池中执行）
    AsyncFile file(path, AsyncFile::ReadOnly);
    co_return file.read_all();
}

// 异步写入整个文件
// 注意：此函数必须通过 async_run() 调用，不要直接 co_await
inline Task<void> write_file(const std::string& path, const std::string& content) {
    // 同步写入文件（在线程池中执行）
    AsyncFile file(path, AsyncFile::WriteOnly | AsyncFile::Create | AsyncFile::Truncate);
    file.write(content);
    file.sync();
    co_return;
}

// 异步追加到文件
// 注意：此函数必须通过 async_run() 调用，不要直接 co_await
inline Task<void> append_file(const std::string& path, const std::string& content) {
    // 同步追加到文件（在线程池中执行）
    AsyncFile file(path, AsyncFile::WriteOnly | AsyncFile::Create | AsyncFile::Append);
    file.write(content);
    file.sync();
    co_return;
}

} // namespace zlcoro
