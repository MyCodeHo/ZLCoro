#pragma once

#include <vector>
#include <mutex>
#include <memory>
#include <cstddef>
#include <atomic>
#include <new>

namespace zlcoro {

// =============================================================================
// ObjectPool - 对象池
// =============================================================================
// 
// 高性能对象池，减少频繁的内存分配：
// - 预分配对象，重用已释放的对象
// - 线程安全（使用自旋锁）
// - 支持自定义最大容量
// =============================================================================

template <typename T>
class ObjectPool {
public:
    // 构造函数：initial_size - 初始预分配数量，max_size - 池的最大容量
    explicit ObjectPool(size_t initial_size = 32, size_t max_size = 1024)
        : max_size_(max_size) {
        reserve(initial_size);  // 预分配内存块
    }

    // 析构函数：释放池中所有内存
    ~ObjectPool() {
        std::lock_guard<SpinLock> lock(lock_);
        for (void* ptr : pool_) {
            ::operator delete(ptr);  // 只释放内存，对象已在 release() 中析构
        }
    }

    // 禁止拷贝
    ObjectPool(const ObjectPool&) = delete;
    ObjectPool& operator=(const ObjectPool&) = delete;

    // 获取对象（如果池为空则创建新对象）
    // Args: 传递给 T 构造函数的参数
    // 返回: 构造好的 T* 对象指针
    template <typename... Args>
    T* acquire(Args&&... args) {
        void* ptr = nullptr;
        
        // 第 1 步：尝试从池中获取空闲内存块
        {
            std::lock_guard<SpinLock> lock(lock_);  // 加锁保护并发访问
            if (!pool_.empty()) {
                ptr = pool_.back();  // 取出最后一个（LIFO）
                pool_.pop_back();
            }
        }  // 锁在此处自动释放

        // 第 2 步：池为空，分配新内存
        if (!ptr) {
            ptr = ::operator new(sizeof(T));  // 只分配内存，不调用构造函数
        }

        // 第 3 步：在内存上构造对象（placement new）
        return new (ptr) T(std::forward<Args>(args)...);
    }

    // 释放对象到池
    // obj: 要释放的对象指针
    // 注意：会调用析构函数，但内存会被重用
    void release(T* obj) {
        if (!obj) return;

        // 第 1 步：显式调用析构函数，销毁对象但保留内存
        obj->~T();

        // 第 2 步：将内存块放回池中或释放
        std::lock_guard<SpinLock> lock(lock_);
        if (pool_.size() < max_size_) {
            pool_.push_back(obj);  // 放回池中，下次重用
        } else {
            ::operator delete(obj);  // 池满了，真正释放内存
        }
    }

    // 预分配内存
    // count: 预分配的内存块数量
    // 作用：减少后续 acquire 时的分配开销
    void reserve(size_t count) {
        std::lock_guard<SpinLock> lock(lock_);
        pool_.reserve(count);  // 预留 vector 容量，避免扩容
        // 预分配内存块（只分配，不构造对象）
        for (size_t i = pool_.size(); i < count && i < max_size_; ++i) {
            pool_.push_back(::operator new(sizeof(T)));
        }
    }

    // 清空池
    void clear() {
        std::lock_guard<SpinLock> lock(lock_);
        for (void* ptr : pool_) {
            ::operator delete(ptr);
        }
        pool_.clear();
    }

    size_t size() const {
        std::lock_guard<SpinLock> lock(lock_);
        return pool_.size();
    }

private:
    // 简单自旋锁，比 mutex 更轻量（适合短时间持有）
    class SpinLock {
    public:
        void lock() const {
            // test_and_set: 原子操作，返回旧值并设置为 true
            // 如果返回 true（已被占用），则循环等待（自旋）
            while (flag_.test_and_set(std::memory_order_acquire)) {
                // 自旋等待（busy-wait），不进入内核态
            }
        }

        void unlock() const {
            // 清除标志，释放锁
            flag_.clear(std::memory_order_release);
        }

    private:
        // atomic_flag: C++ 保证无锁的原子类型
        mutable std::atomic_flag flag_ = ATOMIC_FLAG_INIT;
    };

    std::vector<void*> pool_;    // 可用内存块列表（未构造对象）
    mutable SpinLock lock_;      // 保护 pool_ 的并发访问
    size_t max_size_;            // 池的最大容量
};

// =============================================================================
// PooledPtr - 池化智能指针
// =============================================================================
// 
// RAII 包装器，自动将对象返回池
// =============================================================================

template <typename T>
class PooledPtr {
public:
    // 默认构造：空指针
    PooledPtr() : ptr_(nullptr), pool_(nullptr) {}
    
    // 构造：接管对象所有权
    PooledPtr(T* ptr, ObjectPool<T>* pool) : ptr_(ptr), pool_(pool) {}

    // 析构：自动归还对象到池（RAII）
    ~PooledPtr() {
        reset();
    }

    // 移动构造：转移所有权
    PooledPtr(PooledPtr&& other) noexcept 
        : ptr_(other.ptr_), pool_(other.pool_) {
        other.ptr_ = nullptr;   // 窃取资源
        other.pool_ = nullptr;
    }

    // 移动赋值：转移所有权
    PooledPtr& operator=(PooledPtr&& other) noexcept {
        if (this != &other) {
            reset();  // 先释放当前持有的对象
            ptr_ = other.ptr_;    // 窃取资源
            pool_ = other.pool_;
            other.ptr_ = nullptr;
            other.pool_ = nullptr;
        }
        return *this;
    }

    // 禁止拷贝
    PooledPtr(const PooledPtr&) = delete;
    PooledPtr& operator=(const PooledPtr&) = delete;

    // 获取原始指针（不转移所有权）
    T* get() const noexcept { return ptr_; }
    
    // 箭头运算符：支持 ptr->member 语法
    T* operator->() const noexcept { return ptr_; }
    
    // 解引用运算符：支持 *ptr 语法
    T& operator*() const noexcept { return *ptr_; }
    
    // 转换为 bool：支持 if (ptr) 语法
    explicit operator bool() const noexcept { return ptr_ != nullptr; }

    // 重置：归还对象到池并清空指针
    void reset() {
        if (ptr_ && pool_) {
            pool_->release(ptr_);  // 自动归还
        }
        ptr_ = nullptr;
        pool_ = nullptr;
    }

    // 释放所有权：返回指针但不归还到池（调用者负责管理）
    T* release() {
        T* tmp = ptr_;
        ptr_ = nullptr;
        pool_ = nullptr;
        return tmp;
    }

private:
    T* ptr_;              // 管理的对象指针
    ObjectPool<T>* pool_; // 对象所属的池
};

// =============================================================================
// FixedSizeAllocator - 固定大小内存分配器
// =============================================================================
// 
// 用于分配固定大小的内存块（如协程帧）
// =============================================================================

class FixedSizeAllocator {
public:
    // 构造函数
    // block_size: 每个内存块的大小（会自动对齐）
    // initial_blocks: 初始预分配的块数量
    explicit FixedSizeAllocator(size_t block_size, size_t initial_blocks = 64)
        : block_size_(align_up(block_size, alignof(std::max_align_t))) {
        reserve(initial_blocks);  // 预分配内存块
    }

    // 析构函数：释放所有已分配的大块（chunks）
    ~FixedSizeAllocator() {
        std::lock_guard<std::mutex> lock(mutex_);
        for (void* chunk : chunks_) {
            ::operator delete(chunk);  // 释放整个 chunk
        }
    }

    // 分配一个固定大小的内存块
    // 返回: 对齐后的内存块指针
    void* allocate() {
        std::lock_guard<std::mutex> lock(mutex_);
        
        // 从空闲列表获取
        if (!free_list_.empty()) {
            void* ptr = free_list_.back();
            free_list_.pop_back();
            return ptr;
        }

        // 空闲列表为空，分配新的 chunk（一次分配 32 个块）
        allocate_chunk();
        void* ptr = free_list_.back();
        free_list_.pop_back();
        return ptr;
    }

    // 归还内存块到空闲列表
    // ptr: 要归还的内存块指针
    void deallocate(void* ptr) {
        if (!ptr) return;
        
        std::lock_guard<std::mutex> lock(mutex_);
        free_list_.push_back(ptr);  // 放回空闲列表，可重用
    }

    size_t block_size() const noexcept { return block_size_; }

private:
    // 向上对齐到指定边界
    // 例: align_up(10, 8) = 16
    static constexpr size_t align_up(size_t n, size_t alignment) {
        return (n + alignment - 1) & ~(alignment - 1);
    }

    // 预分配指定数量的内存块
    void reserve(size_t count) {
        while (free_list_.size() < count) {
            allocate_chunk();  // 每次分配一个 chunk（32 块）
        }
    }

    // 分配一个大块（chunk），并切分成多个小块
    void allocate_chunk() {
        constexpr size_t blocks_per_chunk = 32;  // 每个 chunk 包含 32 个块
        size_t chunk_size = block_size_ * blocks_per_chunk;
        
        // 分配一大块连续内存
        char* chunk = static_cast<char*>(::operator new(chunk_size));
        chunks_.push_back(chunk);  // 记录 chunk，用于析构时释放

        // 将 chunk 切分成 32 个小块，加入空闲列表
        for (size_t i = 0; i < blocks_per_chunk; ++i) {
            free_list_.push_back(chunk + i * block_size_);
        }
    }

    size_t block_size_;           // 每个块的大小（对齐后）
    std::vector<void*> free_list_; // 空闲块列表
    std::vector<void*> chunks_;    // 已分配的大块列表（用于析构）
    std::mutex mutex_;             // 保护并发访问
};

// =============================================================================
// ThreadLocalPool - 线程本地对象池
// =============================================================================
// 
// 每个线程有自己的小池，减少锁竞争
// =============================================================================

template <typename T>
class ThreadLocalPool {
public:
    static constexpr size_t local_cache_size = 32;  // 每个线程本地缓存的最大对象数

    // 构造函数
    explicit ThreadLocalPool() = default;

    // 获取对象（优先从线程本地缓存）
    // 性能优势：大多数情况下无需加锁
    template <typename... Args>
    T* acquire(Args&&... args) {
        auto& local = get_local_cache();  // 线程本地，无需加锁
        
        void* ptr = nullptr;
        // 第 1 步：从本地缓存获取（无锁，快）
        if (!local.empty()) {
            ptr = local.back();
            local.pop_back();
        } else {
            // 第 2 步：本地缓存空，分配新内存
            ptr = ::operator new(sizeof(T));
        }

        return new (ptr) T(std::forward<Args>(args)...);
    }

    // 释放对象（优先放入线程本地缓存）
    void release(T* obj) {
        if (!obj) return;
        
        obj->~T();  // 析构对象
        
        auto& local = get_local_cache();
        // 本地缓存未满，放入本地（无锁，快）
        if (local.size() < local_cache_size) {
            local.push_back(obj);
        } else {
            // 本地缓存满，直接释放内存
            ::operator delete(obj);
        }
    }

private:
    // 线程本地缓存结构
    struct LocalCache {
        std::vector<void*> objects;  // 缓存的对象指针
        
        // 析构时释放所有缓存的内存
        ~LocalCache() {
            for (void* ptr : objects) {
                ::operator delete(ptr);
            }
        }
        
        bool empty() const { return objects.empty(); }
        size_t size() const { return objects.size(); }
        void* back() const { return objects.back(); }
        void pop_back() { objects.pop_back(); }
        void push_back(void* p) { objects.push_back(p); }
    };

    // 获取线程本地缓存
    // 每个线程有自己独立的 cache 实例（thread_local）
    static LocalCache& get_local_cache() {
        thread_local LocalCache cache;  // 线程局部存储
        return cache;
    }
};

} // namespace zlcoro
