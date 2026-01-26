# ZLCoro 项目结构

## 📁 目录结构

```
ZLCoro/
├── README.md                 # 项目简介
├── PROGRESS.md               # 开发进度
├── BUG_FIX.md               # Bug 修复记录
├── DEVELOPMENT.md           # 开发指南
├── CMakeLists.txt           # 主构建配置
│
├── include/zlcoro/          # 📦 头文件（Header-Only 库）
│   ├── zlcoro.hpp           # 主入口文件
│   ├── core/                # 协程核心
│   │   ├── task.hpp         # Task<T> 协程类型
│   │   └── generator.hpp    # Generator<T> 生成器
│   ├── scheduler/           # 调度器
│   │   ├── scheduler.hpp    # 协程调度器
│   │   ├── thread_pool.hpp  # 线程池
│   │   ├── work_stealing_scheduler.hpp  # 工作窃取
│   │   └── async.hpp        # async_run / fire_and_forget
│   ├── io/                  # I/O 组件
│   │   ├── epoll_poller.hpp # epoll 封装
│   │   ├── event_loop.hpp   # 事件循环
│   │   ├── async_file.hpp   # 异步文件
│   │   ├── async_socket.hpp # 异步 socket
│   │   ├── io_uring*.hpp    # io_uring 支持
│   │   └── wal_writer.hpp   # WAL 写入器
│   ├── runtime/             # ⭐ Per-Core 运行时（高性能核心）
│   │   ├── per_core.hpp     # 架构总览
│   │   ├── per_core_event_loop.hpp      # 事件循环基类
│   │   ├── epoll_per_core.hpp           # epoll 后端
│   │   ├── io_uring_per_core.hpp        # io_uring 后端
│   │   ├── connection_manager.hpp       # 连接管理（基础版）
│   │   ├── optimized_connection.hpp     # 连接管理（优化版）
│   │   ├── tcp_server.hpp               # TCP 服务器
│   │   ├── kv_server.hpp                # KV 服务器（基础版）
│   │   └── optimized_kv_server.hpp      # KV 服务器（优化版）
│   ├── sync/                # 同步原语
│   │   ├── mutex.hpp        # 协程互斥锁
│   │   ├── rwlock.hpp       # 读写锁
│   │   ├── channel.hpp      # 通道
│   │   ├── semaphore.hpp    # 信号量
│   │   ├── wait_group.hpp   # 等待组
│   │   └── timer.hpp        # 定时器
│   └── utils/               # 工具类
│       ├── memory_pool.hpp  # 内存池
│       └── aligned_buffer.hpp # 对齐缓冲区
│
├── examples/                # 📖 示例代码
│   ├── 01_basic_task.cpp    # Task 基础用法
│   ├── 02_generator_example.cpp  # Generator 用法
│   ├── 03_scheduler_example.cpp  # 调度器用法
│   ├── 04_async_io_example.cpp   # 异步 I/O
│   ├── 05_sync_primitives.cpp    # 同步原语
│   ├── 06_per_core_server.cpp    # Per-Core 服务器
│   ├── network/             # 网络示例
│   │   ├── echo_server.cpp
│   │   ├── echo_client.cpp
│   │   └── http_server.cpp
│   └── benchmark/           # ⚡ 性能测试示例（推荐）
│       ├── README.md        # 使用说明
│       ├── http_server_v2.cpp     # 高性能 HTTP 服务器
│       ├── optimized_kv_server.cpp    # KV 服务器
│       └── ...
│
├── benchmarks/              # 📊 基准测试
│   ├── README.md            # 测试说明
│   ├── coroutine_bench.cpp  # 协程性能
│   ├── scheduler_bench.cpp  # 调度器性能
│   ├── io_bench.cpp         # I/O 基础性能
│   ├── io_all_bench.cpp     # I/O 综合测试
│   ├── io_uring_bench.cpp   # io_uring vs epoll
│   └── io_comparison_bench.cpp  # 协程 vs 阻塞 I/O
│
├── tests/                   # 🧪 单元测试
│   ├── core/                # 协程测试
│   ├── scheduler/           # 调度器测试
│   ├── io/                  # I/O 测试
│   ├── sync/                # 同步原语测试
│   └── runtime/             # 运行时测试
│
└── docs/                    # 📚 文档
    ├── ARCHITECTURE.md      # 架构设计
    ├── API.md               # API 参考
    ├── PER_CORE_ARCHITECTURE.md  # Per-Core 架构详解
    └── BENCHMARKS.md        # 性能测试说明
```

## 🎯 核心组件说明

### 1. 协程核心 (core/)
- `Task<T>` - 通用协程类型，支持 co_await/co_return
- `Generator<T>` - 惰性生成器，支持 co_yield

### 2. Per-Core 运行时 (runtime/) ⭐ 推荐
- **每核独立架构**：每个 CPU 核心运行独立事件循环
- **SO_REUSEPORT**：内核级负载均衡，无惊群效应
- **双后端支持**：epoll（兼容）和 io_uring（高性能）

### 3. 同步原语 (sync/)
- 完整的协程版同步原语，无阻塞线程

### 4. 工具类 (utils/)
- `MemoryPool` - 对象池，减少内存分配
- `AlignedBuffer` - 对齐缓冲区（Direct I/O）

## 🚀 快速开始

```bash
# 编译
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)

# 运行测试
ctest --output-on-failure

# 运行 HTTP 服务器
./examples/http_server_v2 epoll 8080 8

# 压测
wrk -t8 -c1000 -d15s http://127.0.0.1:8080/
```

## 📊 性能参考

| 测试场景 | 性能 |
|----------|------|
| HTTP Hello World | ~98 万 QPS |
| 协程创建 | ~50ns/个 |
| 协程切换 | ~20ns/次 |
| io_uring 文件读 | 12 GB/s |
