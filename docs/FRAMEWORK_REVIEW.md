# ZLCoro 框架审查报告

## 📊 审查总结

本次审查针对 ZLCoro 协程框架进行全面评估，重点关注 **高性能 KV 存储系统** 场景下的适用性。

**审查日期**: 2026-01-16  
**状态**: ✅ 关键问题已修复，Per-Core 架构已实现

---

## 🆕 新增：Per-Core 架构

### 架构概述

专为高性能 KV 存储设计的每核心事件循环架构：

```
   ┌─────────────────────────────────────────────────────────────────┐
   │                    PerCoreTcpServer                             │
   │                   (Accept Dispatcher)                           │
   └──────────────────────────┬──────────────────────────────────────┘
                              │ 连接分发 (hash by fd)
            ┌─────────────────┼─────────────────┐
            ▼                 ▼                 ▼
   ┌────────────────┐ ┌────────────────┐ ┌────────────────┐
   │   Core 0       │ │   Core 1       │ │   Core N       │
   │ ┌────────────┐ │ │ ┌────────────┐ │ │ ┌────────────┐ │
   │ │ EventLoop  │ │ │ │ EventLoop  │ │ │ │ EventLoop  │ │
   │ │ (epoll/    │ │ │ │ (epoll/    │ │ │ │ (epoll/    │ │
   │ │  io_uring) │ │ │ │  io_uring) │ │ │ │  io_uring) │ │
   │ └────────────┘ │ │ └────────────┘ │ │ └────────────┘ │
   │ ┌────────────┐ │ │ ┌────────────┐ │ │ ┌────────────┐ │
   │ │ Coroutines │ │ │ │ Coroutines │ │ │ │ Coroutines │ │
   │ │ (per-conn) │ │ │ │ (per-conn) │ │ │ │ (per-conn) │ │
   │ └────────────┘ │ │ └────────────┘ │ │ └────────────┘ │
   └────────────────┘ └────────────────┘ └────────────────┘
        独立运行           独立运行           独立运行
        无共享状态         无共享状态         无共享状态
```

### 核心组件

| 组件 | 文件 | 说明 |
|------|------|------|
| `PerCoreEventLoop` | [per_core_event_loop.hpp](../include/zlcoro/runtime/per_core_event_loop.hpp) | 事件循环基类 |
| `EpollPerCoreEventLoop` | [epoll_per_core.hpp](../include/zlcoro/runtime/epoll_per_core.hpp) | epoll 后端 |
| `IoUringPerCoreEventLoop` | [io_uring_per_core.hpp](../include/zlcoro/runtime/io_uring_per_core.hpp) | io_uring 后端 |
| `PerCoreRuntime` | [per_core_runtime.hpp](../include/zlcoro/runtime/per_core_runtime.hpp) | 多核运行时管理器 |
| `PerCoreTcpServer` | [tcp_server.hpp](../include/zlcoro/runtime/tcp_server.hpp) | TCP 服务器 |

### 使用示例

```cpp
#include "zlcoro/runtime/per_core.hpp"

// 创建 io_uring 运行时（4 核心）
auto runtime = zlcoro::make_io_uring_runtime(4);

// 创建 TCP 服务器
zlcoro::PerCoreTcpServer server(*runtime);
server.listen(8080);

// 设置连接处理器（每个连接一个协程）
server.set_handler([](zlcoro::PerCoreConnection& conn) -> zlcoro::Task<void> {
    char buf[1024];
    while (true) {
        ssize_t n = co_await conn.read(buf, sizeof(buf));
        if (n <= 0) break;
        co_await conn.write(buf, n);  // echo
    }
});

// 启动
runtime->start();
server.start_accept_all_cores();
runtime->wait();
```

### 架构优势

| 特性 | 说明 |
|------|------|
| **完全无锁** | 每个核心独立运行，无共享状态 |
| **缓存友好** | 连接数据和协程都在本核心 |
| **线性扩展** | 性能随核心数线性增长 |
| **双后端支持** | epoll 和 io_uring 两种后端 |
| **每连接一协程** | 简化编程模型 |

---

## 🟢 已修复问题

### 1. **io_uring Request 生命周期问题** ✅ 已修复
**文件**: [io_uring_poller.hpp](../include/zlcoro/io/io_uring_poller.hpp)

**问题**: `Request` 对象作为栈变量在协程中使用，当协程被取消或过早销毁时，内核仍可能写入已释放的内存。

**修复方案**: 
- 添加 `SafeIoUringAwaiter` 使用 `shared_ptr<Request>` 管理生命周期
- 添加 `make_safe_request()` 辅助函数
- 更新 `IoUringFile` 和 `IoUringSocket` 使用安全版本

### 2. **async_run 监控线程优化** ✅ 已优化
**文件**: [async.hpp](../include/zlcoro/scheduler/async.hpp)

**问题**: 每次调用 `async_run()` 都会创建一个监控线程。

**修复方案**: 
- 立即完成的协程不再创建监控线程
- 只有挂起的协程才创建监控线程
- 添加条件变量支持

### 3. **Vectored I/O 支持** ✅ 已添加
**文件**: [io_uring_poller.hpp](../include/zlcoro/io/io_uring_poller.hpp), [io_uring_file.hpp](../include/zlcoro/io/io_uring_file.hpp)

新增 `readv()`/`writev()` 支持，用于批量读写不连续缓冲区。

### 4. **fdatasync 支持** ✅ 已添加
**文件**: [io_uring_poller.hpp](../include/zlcoro/io/io_uring_poller.hpp), [io_uring_file.hpp](../include/zlcoro/io/io_uring_file.hpp)

新增 `fdatasync()` 支持，比 `fsync()` 更快（只同步数据，不同步元数据）。

---

## 📦 新增功能 (KV 存储支持)

### 1. **WalWriter - WAL 写入器**
**文件**: [wal_writer.hpp](../include/zlcoro/io/wal_writer.hpp)

专为 KV 存储设计的高性能 WAL 写入器：
- `append_sync()` - 同步写入并持久化
- `append_async()` - 异步写入
- `append_batch()` - 批量写入（writev + fdatasync）
- 支持 Direct I/O 模式

### 2. **AlignedBuffer - 对齐内存分配器**
**文件**: [aligned_buffer.hpp](../include/zlcoro/utils/aligned_buffer.hpp)

用于 Direct I/O 的对齐内存分配：
- 页对齐（4096 字节）
- 支持 resize/append/clear
- 静态工具函数 `align_up()`/`align_down()`

---

## 🟡 仍需关注的问题

### 1. **EpollPoller handlers_ 线程安全**
`handlers_` 使用 `std::map`，在多线程场景下需要外部同步。

**建议**: 如果需要多线程访问，添加 `std::mutex` 保护或改用无锁数据结构。

### 2. **io_uring 网络测试崩溃**
benchmark 中的网络测试因 lambda 协程生命周期问题暂时禁用。

**建议**: 使用统一的 Runtime 管理协程生命周期。

---

## 📈 性能测试结果

| 测试项 | io_uring | epoll + 线程池 | 性能比 |
|--------|----------|----------------|--------|
| **文件读取** | 13550 MB/s (74μs) | 2762 MB/s (362μs) | **4.9x 更快** ⚡ |
| **文件写入** (fsync) | 22.30 MB/s | 26.59 MB/s | 略慢 (受 fsync 影响) |

---

## 📋 KV 存储系统建议

基于本次审查，以下是构建 KV 存储系统的建议：

### 优先使用 io_uring
- 文件读取性能是 epoll 的 **4.9 倍**
- 支持 vectored I/O 和批量提交
- 统一的文件和网络 I/O 接口

### 使用 WalWriter 实现 WAL
```cpp
WalWriter wal(&poller, "/data/wal.log");
co_await wal.append_sync("commit record");  // 同步持久化
co_await wal.append_batch(records);         // 批量写入
```

### 使用 AlignedBuffer 实现 SST 读写
```cpp
AlignedBuffer buf(4096);  // 页对齐
co_await file.read(buf.data(), buf.size(), offset);
```

### 后续完善建议
1. **Fixed File Descriptors** - 减少内核 fd 查找开销
2. **超时取消机制** - 生产环境必备
3. **统一 Runtime** - 简化协程生命周期管理
4. **批量提交优化** - 减少 `io_uring_submit()` 调用

---

**测试状态**: 14/14 tests passing ✅ (含 Per-Core 测试)

**新增文件**:
- `include/zlcoro/runtime/per_core_event_loop.hpp` - 事件循环基类
- `include/zlcoro/runtime/epoll_per_core.hpp` - epoll 后端
- `include/zlcoro/runtime/io_uring_per_core.hpp` - io_uring 后端
- `include/zlcoro/runtime/per_core_runtime.hpp` - 运行时管理器
- `include/zlcoro/runtime/tcp_server.hpp` - TCP 服务器/客户端
- `include/zlcoro/runtime/per_core.hpp` - 统一头文件
- `examples/06_per_core_server.cpp` - 示例服务器
- `tests/runtime/per_core_test.cpp` - 单元测试
