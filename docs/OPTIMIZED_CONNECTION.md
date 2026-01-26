# 优化版高并发连接管理器

## 实现概述

本次实现为 ZLCoro 框架添加了优化版的连接管理器，主要文件包括：

1. **optimized_connection.hpp** - 优化的连接管理器
   - `OptimizedEpollManager` - epoll 优化版
   - `OptimizedIoUringManager` - io_uring 优化版  
   - `OptimizedKVConnection<Manager>` - 统一连接接口

2. **optimized_kv_server.hpp** - 优化版 KV 服务器
   - `OptimizedEpollKVServer` - epoll 多核心服务器
   - `OptimizedIoUringKVServer` - io_uring 多核心服务器

3. **multicore_echo_server.cpp** - 多核心 Echo 服务器示例

## 关键优化

### 1. FastRead/FastWrite Awaiter
```cpp
struct FastReadAwaiter {
    bool await_ready() noexcept {
        // 先尝试非阻塞读取，减少系统调用
        ssize_t n = ::recv(state->fd, buf, len, MSG_DONTWAIT);
        if (n >= 0) {
            state->last_read_result = n;
            return true;  // 立即返回，无需挂起
        }
        return false;  // 需要等待 epoll 事件
    }
};
```

### 2. ThreadLocalPool 内存池
使用框架内置的 `ThreadLocalPool` 减少内存分配开销：
- 每个线程本地缓存 32 个对象
- 无锁获取/释放

### 3. 多核心并行
- 每个 CPU 核心独立的事件循环
- 使用 `SO_REUSEPORT` 让多个线程共享同一监听端口
- 连接绑定到接受它的核心处理

## 性能测试结果

使用 `kv_benchmark` 测试：

```
========================================
        性能测试结果报告
========================================
测试时长:        10.00 秒
总请求数:        2222190
总连接数:        800
错误数:          0
----------------------------------------
QPS:             222196.78 req/s
吞吐量:          23.73 MB/s
平均延迟:        35.26 us
========================================
```

**配置:**
- 服务器核心数: 16
- 客户端线程数: 8
- 每线程连接数: 100
- 请求大小: 64 bytes

## 进一步优化建议

要达到百万 QPS，需要：

1. **更多核心** - 目前 16 核达到 22 万 QPS，线性扩展约需要 70+ 核心

2. **批量提交 (io_uring)**
   ```cpp
   static constexpr size_t BATCH_SIZE = 64;
   void maybe_submit() {
       if (pending_submits_ >= BATCH_SIZE) flush();
   }
   ```

3. **流水线客户端** - 批量发送请求不等待响应

4. **零拷贝** - 使用 `splice()` 或 `io_uring` 的 fixed buffers

5. **DPDK/kernel bypass** - 对于极端性能需求

## 使用示例

```cpp
#include "zlcoro/runtime/optimized_connection.hpp"

// Echo 服务器处理函数
Task<void> handle_connection(int fd, OptimizedEpollManager& mgr) {
    OptimizedEpollConnection conn(fd, mgr);
    
    char buf[4096];
    while (true) {
        ssize_t n = co_await conn.read(buf, sizeof(buf));
        if (n <= 0) break;
        
        ssize_t written = co_await conn.write(buf, n);
        if (written != n) break;
    }
}
```

## 文件列表

| 文件 | 描述 |
|------|------|
| include/zlcoro/runtime/optimized_connection.hpp | 优化的连接管理器 |
| include/zlcoro/runtime/optimized_kv_server.hpp | 优化的 KV 服务器 |
| examples/benchmark/multicore_echo_server.cpp | 多核心 Echo 服务器 |
| examples/benchmark/echo_benchmark.cpp | Echo 性能测试客户端 |
| examples/benchmark/optimized_kv_benchmark.cpp | KV 性能测试客户端 |
| examples/benchmark/optimized_kv_server.cpp | 优化的 KV 服务器程序 |
