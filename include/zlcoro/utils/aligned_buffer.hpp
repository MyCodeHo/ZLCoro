#pragma once

#include <cstdlib>
#include <cstddef>
#include <memory>
#include <stdexcept>
#include <cstring>

namespace zlcoro {

// =============================================================================
// AlignedBuffer - 对齐内存缓冲区
// =============================================================================
// 
// 用于 Direct I/O 的对齐内存分配器。
// Direct I/O 要求缓冲区地址和大小都按照扇区大小（通常 512 或 4096）对齐。
// 
// 使用示例:
//   AlignedBuffer buf(4096, 4096);  // 4KB 缓冲区，4KB 对齐
//   memcpy(buf.data(), "hello", 5);
//   write(fd, buf.data(), buf.size());
// =============================================================================

class AlignedBuffer {
public:
    // 默认对齐大小（页大小，通常适用于大多数系统）
    static constexpr size_t DEFAULT_ALIGNMENT = 4096;

    // 默认构造（空缓冲区）
    AlignedBuffer() noexcept : data_(nullptr), size_(0), capacity_(0), alignment_(0) {}

    // 分配指定大小和对齐的缓冲区
    explicit AlignedBuffer(size_t size, size_t alignment = DEFAULT_ALIGNMENT)
        : data_(nullptr), size_(size), capacity_(0), alignment_(alignment) {
        if (size > 0) {
            allocate(size, alignment);
        }
    }

    // 禁止拷贝
    AlignedBuffer(const AlignedBuffer&) = delete;
    AlignedBuffer& operator=(const AlignedBuffer&) = delete;

    // 移动构造
    AlignedBuffer(AlignedBuffer&& other) noexcept
        : data_(std::exchange(other.data_, nullptr))
        , size_(std::exchange(other.size_, 0))
        , capacity_(std::exchange(other.capacity_, 0))
        , alignment_(std::exchange(other.alignment_, 0)) {}

    AlignedBuffer& operator=(AlignedBuffer&& other) noexcept {
        if (this != &other) {
            deallocate();
            data_ = std::exchange(other.data_, nullptr);
            size_ = std::exchange(other.size_, 0);
            capacity_ = std::exchange(other.capacity_, 0);
            alignment_ = std::exchange(other.alignment_, 0);
        }
        return *this;
    }

    // 析构
    ~AlignedBuffer() {
        deallocate();
    }

    // 获取数据指针
    void* data() noexcept { return data_; }
    const void* data() const noexcept { return data_; }

    // 获取 char* 指针（便于字符串操作）
    char* chars() noexcept { return static_cast<char*>(data_); }
    const char* chars() const noexcept { return static_cast<const char*>(data_); }

    // 获取大小
    size_t size() const noexcept { return size_; }
    size_t capacity() const noexcept { return capacity_; }
    size_t alignment() const noexcept { return alignment_; }

    // 检查是否为空
    bool empty() const noexcept { return size_ == 0; }

    // 检查是否有效
    explicit operator bool() const noexcept { return data_ != nullptr; }

    // 重新分配
    void resize(size_t new_size) {
        if (new_size <= capacity_) {
            size_ = new_size;
            return;
        }
        
        // 需要重新分配
        void* new_data = nullptr;
        if (posix_memalign(&new_data, alignment_, new_size) != 0) {
            throw std::bad_alloc();
        }
        
        // 复制旧数据
        if (data_ && size_ > 0) {
            std::memcpy(new_data, data_, size_);
        }
        
        deallocate();
        data_ = new_data;
        size_ = new_size;
        capacity_ = new_size;
    }

    // 清空（不释放内存）
    void clear() noexcept {
        size_ = 0;
    }

    // 释放内存
    void reset() noexcept {
        deallocate();
        size_ = 0;
        capacity_ = 0;
    }

    // 追加数据
    void append(const void* src, size_t len) {
        if (size_ + len > capacity_) {
            // 扩容
            size_t new_capacity = std::max(capacity_ * 2, size_ + len);
            new_capacity = align_up(new_capacity, alignment_);
            resize(new_capacity);
        }
        std::memcpy(static_cast<char*>(data_) + size_, src, len);
        size_ += len;
    }

    // 填充零
    void zero() noexcept {
        if (data_ && size_ > 0) {
            std::memset(data_, 0, size_);
        }
    }

    // =========================================================================
    // 静态工具函数
    // =========================================================================

    // 向上对齐
    static size_t align_up(size_t value, size_t alignment) noexcept {
        return (value + alignment - 1) & ~(alignment - 1);
    }

    // 向下对齐
    static size_t align_down(size_t value, size_t alignment) noexcept {
        return value & ~(alignment - 1);
    }

    // 检查是否对齐
    static bool is_aligned(const void* ptr, size_t alignment) noexcept {
        return (reinterpret_cast<uintptr_t>(ptr) & (alignment - 1)) == 0;
    }

    static bool is_aligned(size_t value, size_t alignment) noexcept {
        return (value & (alignment - 1)) == 0;
    }

private:
    void allocate(size_t size, size_t alignment) {
        // 确保对齐是 2 的幂
        if ((alignment & (alignment - 1)) != 0 || alignment == 0) {
            throw std::invalid_argument("Alignment must be a power of 2");
        }
        
        // 对齐大小
        size_t aligned_size = align_up(size, alignment);
        
        if (posix_memalign(&data_, alignment, aligned_size) != 0) {
            throw std::bad_alloc();
        }
        
        capacity_ = aligned_size;
    }

    void deallocate() noexcept {
        if (data_) {
            std::free(data_);
            data_ = nullptr;
        }
    }

private:
    void* data_;        // 对齐的内存指针
    size_t size_;       // 当前使用大小
    size_t capacity_;   // 实际分配大小
    size_t alignment_;  // 对齐字节数
};

// =============================================================================
// make_aligned_buffer - 创建对齐缓冲区的辅助函数
// =============================================================================

inline AlignedBuffer make_aligned_buffer(size_t size, size_t alignment = AlignedBuffer::DEFAULT_ALIGNMENT) {
    return AlignedBuffer(size, alignment);
}

// 创建并初始化为零的缓冲区
inline AlignedBuffer make_zeroed_buffer(size_t size, size_t alignment = AlignedBuffer::DEFAULT_ALIGNMENT) {
    AlignedBuffer buf(size, alignment);
    buf.zero();
    return buf;
}

} // namespace zlcoro
