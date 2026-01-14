# ZLCoro 项目进度

> 基于 C++20 协程的高性能异步编程框架开发进度  
> **最后更新**: 2025-01-06 | **版本**: 0.9.0

## 📊 总体进度

### 🎯 里程碑状态
- ✅ **Phase 1**: Task 协程类型
- ✅ **Phase 2**: Generator 生成器
- ✅ **Phase 3**: Scheduler 调度器
- ✅ **Phase 4**: Async I/O 异步 I/O
- ✅ **Phase 5**: 同步原语 (Channel/Mutex/Semaphore/WaitGroup)
- ✅ **Phase 6**: 性能优化 (工作窃取调度器/内存池/io_uring)
- ✅ **Phase 7**: 应用层框架 (Runtime/TCP/Timer/RWLock)
- ✅ **Phase 8**: 生产就绪 (HTTP服务器/网络示例/基准测试)

### 🧪 测试状态
**118/118 tests passing (100%)** 🎉

| 模块 | 测试数 | 状态 |
|------|--------|------|
| Task | 15 | ✅ |
| Generator | 16 | ✅ |
| Scheduler | 13 | ✅ |
| I/O | 7 | ✅ |
| Sync | 14 | ✅ |
| Performance | 13 | ✅ |
| io_uring | 10 | ✅ |
| Runtime | 30 | ✅ |

### 📦 代码统计
- **头文件**: 27 个（Header-Only）
- **测试文件**: 8 个
- **示例程序**: 8 个
- **基准测试**: 3 个
- **代码行数**: ~12000 行

---

## 🏗️ 架构概览

```
┌─────────────────────────────────────┐
│         应用层                       │
│    Runtime | TcpServer | Timer      │
├─────────────────────────────────────┤
│    网络层 (net/)                     │
│    TcpServer | TcpConnection        │
│    TcpListener | TcpClient          │
├─────────────────────────────────────┤
│    同步原语 (sync/)                  │
│    Mutex | RWLock | Channel         │
│    Semaphore | WaitGroup | Timer    │
│    CancellationToken                │
├─────────────────────────────────────┤
│    异步 I/O (io/)                    │
│    EpollPoller | EventLoop          │
│    IoUringPoller | IoUringFile      │
│    AsyncFile | AsyncSocket          │
├─────────────────────────────────────┤
│    调度器 (scheduler/)               │
│    ThreadPool | WorkStealingScheduler │
├─────────────────────────────────────┤
│    工具 (utils/)                     │
│    ObjectPool | MemoryPool          │
├─────────────────────────────────────┤
│    协程核心 (core/)                  │
│    Task<T> | Generator<T>           │
└─────────────────────────────────────┘
```

---

## 📚 Phase 1: Task<T> 协程类型

### 核心实现 (`include/zlcoro/core/task.hpp`)

**Promise 类型体系**:
- `TaskPromiseBase` - 基类，管理 continuation
- `TaskPromise<T>` - 值返回
- `TaskPromise<void>` - 无返回值
- `TaskPromise<T&>` - 引用返回

**核心特性**:
- ✅ 惰性求值 (`initial_suspend` 返回 `suspend_always`)
- ✅ 协程链 (`co_await` 支持，自动管理 continuation)
- ✅ 异常传播（完整的异常捕获和重新抛出）
- ✅ 移动语义（禁止拷贝，正确管理句柄所有权）

**API**:
```cpp
Task<int> compute() { co_return 42; }
Task<void> caller() { 
    int result = co_await compute();  // 协程链
}
```

### 测试覆盖 (15 个测试)
- 基本功能: int/void/string/引用返回
- 协程链: 单层/多层/多个 await
- 异常处理: 抛出/传播/捕获
- 移动语义: 移动构造/移动赋值
- 复杂场景: 条件分支/递归协程

**示例**: `examples/01_basic_task.cpp` (7 个示例)

---

## 📚 Phase 2: Generator<T> 生成器

### 核心实现 (`include/zlcoro/core/generator.hpp`)

**核心机制**:
- ✅ `co_yield` 支持（左值/右值区分处理）
- ✅ 惰性求值（`begin()` 时才开始生成）
- ✅ 迭代器接口（支持 range-based for）
- ✅ 提前终止（break 时正确清理）

**API**:
```cpp
Generator<int> range(int n) {
    for (int i = 0; i < n; ++i) {
        co_yield i;
    }
}

for (int x : range(5)) {  // 惰性生成
    std::cout << x << "\n";
}
```

### Task vs Generator

| 特性 | Task<T> | Generator<T> |
|------|---------|--------------|
| 返回方式 | `co_return` 一次 | `co_yield` 多次 |
| 执行模式 | 一次性执行 | 惰性生成 |
| 迭代支持 | ❌ | ✅ range-based for |
| 挂起点 | `co_await` | `co_yield` |

### 测试覆盖 (16 个测试)
- 基础功能: 简单序列/range/空生成器
- 不同类型: 字符串/自定义类型
- 惰性求值: 按需生成/提前退出
- 复杂场景: 斐波那契/过滤/嵌套循环

**示例**: `examples/02_generator_example.cpp` (10 个示例)

---

## 📚 Phase 3: Scheduler 调度器

### 核心实现

#### ThreadPool (`include/zlcoro/scheduler/thread_pool.hpp`)
- 固定线程数
- 线程安全任务队列
- 优雅关闭

#### Scheduler (`include/zlcoro/scheduler/scheduler.hpp`)
- 单例模式
- 调度协程句柄和可调用对象
- `ScheduleAwaiter` 支持

#### Async (`include/zlcoro/scheduler/async.hpp`)
- `async_run<T>()` - 异步执行，返回 `std::future`
- `fire_and_forget()` - 后台执行

**API**:
```cpp
Task<int> compute() { co_return 42; }

// 异步执行
auto future = async_run(compute());
int result = future.get();

// 后台执行
fire_and_forget(background_task());
```

### 测试覆盖 (13 个测试)
- ThreadPool: 基础提交/多线程/关闭
- Scheduler: 协程调度
- async_run: 多种返回类型/异常处理/协程链

**示例**: `examples/03_scheduler_example.cpp` (7 个示例)

---

## 📚 Phase 4: 异步 I/O

### 核心实现

#### EpollPoller (`include/zlcoro/io/epoll_poller.hpp`)
- 封装 Linux epoll API
- 边缘触发模式
- 事件轮询和协程恢复

#### EventLoop (`include/zlcoro/io/event_loop.hpp`)
- 单例事件循环
- 就绪队列调度
- 定时器支持（multimap 按时间排序）

#### AsyncFile (`include/zlcoro/io/async_file.hpp`)
- 异步文件读写
- 基于线程池模拟

#### AsyncSocket (`include/zlcoro/io/async_socket.hpp`)
- 异步 connect/accept
- 非阻塞 send/recv
- 避免递归协程（使用循环）

**API**:
```cpp
// 文件操作
AsyncFile file("/tmp/test.txt", AsyncFile::WriteOnly | AsyncFile::Create);
file.write("Hello, ZLCoro!");

// Socket 操作
AsyncSocket socket;
socket.create();
socket.bind("127.0.0.1", 8080);
socket.listen(128);
auto client = co_await socket.accept();
```

### 测试覆盖 (7 个测试)
- AsyncFile: 读/写/追加
- AsyncSocket: connect/accept/send/recv

**示例**: `examples/04_async_io_example.cpp`

---

## 📚 Phase 5: 同步原语

### 核心实现

#### Channel<T> (`include/zlcoro/sync/channel.hpp`)
- 有缓冲/无缓冲通道
- `send()`/`receive()` awaiter
- `try_send()`/`try_receive()` 非阻塞
- `close()` 关闭通道
- **使用 `shared_ptr` 管理等待者数据**

#### Mutex (`include/zlcoro/sync/mutex.hpp`)
- 协程互斥锁
- RAII `LockGuard`
- `try_lock()` 非阻塞
- 公平调度（FIFO）

#### Semaphore (`include/zlcoro/sync/semaphore.hpp`)
- 限制并发数量
- `acquire()`/`scoped_acquire()`
- `try_acquire()` 非阻塞

#### WaitGroup (`include/zlcoro/sync/wait_group.hpp`)
- 等待多个协程完成
- `add()`/`done()`/`wait()`
- 计数归零时批量唤醒

**API**:
```cpp
// Channel
Channel<int> ch(10);
co_await ch.send(42);
auto value = co_await ch.receive();

// Mutex
Mutex mtx;
auto lock = co_await mtx.lock();  // RAII

// Semaphore
Semaphore sem(3);  // 最多 3 个并发
auto guard = co_await sem.scoped_acquire();

// WaitGroup
WaitGroup wg;
wg.add(5);
// ... 启动 5 个任务
co_await wg.wait();
```

---

## 🎯 当前项目状态

```
ZLCoro/
├── include/zlcoro/
│   ├── core/                 ✅ Task + Generator
│   ├── scheduler/            ✅ ThreadPool + Scheduler + async
│   ├── io/                   ✅ Epoll + EventLoop + AsyncFile + AsyncSocket
│   └── sync/                 ✅ Channel + Mutex + Semaphore + WaitGroup
├── tests/                    ✅ 65/65 通过
├── examples/                 ✅ 5 个示例正常运行
└── docs/                     ✅ 架构/API/基准测试文档
```

---

## 💡 最佳实践

### ✅ 推荐用法

```cpp
// 协程执行
Task<int> compute() { co_return 42; }
auto future = async_run(compute());

// 同步原语
Channel<int> ch(10);
co_await ch.send(42);

Mutex mtx;
auto lock = co_await mtx.lock();  // RAII

// 等待多个任务
WaitGroup wg;
wg.add(3);
// ... 启动任务
co_await wg.wait();
```

### ❌ 避免的模式

```cpp
// ❌ 不要循环 resume
while (!handle.done()) {
    handle.resume();  // 可能被其他地方 resume
}

// ❌ 不要存储栈变量地址
struct Awaiter {
    int value_;
    bool await_suspend(handle) {
        queue.push(&value_);  // 悬空指针！
    }
};

// ❌ 不要在协程内部调度自己
Task<void> bad() {
    co_await schedule();  // 避免嵌套调度
}
```

---

## 📖 核心设计原则

从 11 个 Bug 修复中总结的经验：

1. **协程不应在内部重新调度** - 避免嵌套协程和生命周期问题
2. **避免循环中使用 lambda 协程** - 使用独立函数确保生命周期明确
3. **使用 shared_ptr 管理异步对象** - 确保异步执行时对象有效
4. **数据结构要匹配使用场景** - 如定时器按时间排序而非 ID
5. **使用循环而非递归** - 避免协程帧堆叠和栈溢出
6. **添加边界检查** - 防御性编程，检查句柄有效性
7. **Awaiter 数据生命周期** - 等待队列中的数据必须使用 shared_ptr
8. **协程只 resume 一次启动** - 之后由 Scheduler 管理

---

## 📚 Phase 6: 性能优化

### WorkStealingScheduler (`include/zlcoro/scheduler/work_stealing_scheduler.hpp`)

高性能工作窃取调度器：
- ✅ 每个线程有独立的本地任务队列
- ✅ 空闲时从其他线程窃取任务
- ✅ 减少全局锁竞争，提高缓存局部性

**性能对比**:
```
ThreadPool:           15952 us (10000 tasks)
WorkStealingScheduler: 6833 us (10000 tasks)
性能提升: ~2.3x
```

**API**:
```cpp
WorkStealingScheduler scheduler(4);  // 4 线程

scheduler.submit([]() {
    // 任务代码
});

scheduler.schedule(coroutine_handle);  // 调度协程
```

### ObjectPool (`include/zlcoro/utils/memory_pool.hpp`)

对象池，减少频繁内存分配：
- ✅ 预分配对象池
- ✅ 线程安全（自旋锁）
- ✅ RAII 包装器 (PooledPtr)

**API**:
```cpp
ObjectPool<MyClass> pool(32, 1024);  // 初始32个，最大1024个

auto* obj = pool.acquire(arg1, arg2);  // 获取对象
pool.release(obj);                      // 释放对象

// RAII 版本
{
    PooledPtr<MyClass> ptr(pool.acquire(args...), &pool);
    ptr->method();
}  // 自动归还
```

### FixedSizeAllocator

固定大小内存块分配器：
- ✅ 批量分配内存块
- ✅ 适用于协程帧等固定大小对象

**API**:
```cpp
FixedSizeAllocator allocator(64);  // 64 字节块

void* ptr = allocator.allocate();
allocator.deallocate(ptr);
```

### io_uring 支持 (`include/zlcoro/io/io_uring*.hpp`)

Linux 5.1+ 高性能异步 I/O 接口：
- ✅ IoUringPoller: io_uring 封装，支持批量提交/完成
- ✅ IoUringFile: 真正的异步文件读写
- ✅ IoUringSocket: 异步网络 I/O (accept/connect/send/recv)
- ✅ IoUringEventLoop: 简单事件循环

**相比 epoll + 线程池方案的优势**:
- 真正的异步：内核直接执行 I/O，无需线程池模拟
- 零拷贝：使用共享内存的 SQ/CQ 环形缓冲区
- 批量提交：多个请求可以一次系统调用提交
- 统一接口：支持文件、网络、定时器等多种操作

**性能对比**:
```
io_uring 读取 1MB 文件: 439 us
io_uring 100 次小读取: 570 us (5 us/read)
```

**API**:
```cpp
#include "zlcoro/io/io_uring.hpp"

#ifdef ZLCORO_HAS_IO_URING

IoUringEventLoop loop;

// 文件操作
IoUringFile file(&loop.poller(), "test.txt", IoUringFile::ReadOnly);
auto content = co_await file.read_all();

// Socket 操作
IoUringSocket server(&loop.poller());
server.create();
server.set_reuse_addr();
server.bind("0.0.0.0", 8080);
server.listen();
auto client = co_await server.accept();
auto data = co_await client.recv_string(1024);
co_await client.send_string("Hello!");

#endif
```

**编译要求**:
- Linux 5.1+ 内核
- liburing 库 (`apt install liburing-dev`)
- 链接时添加 `-luring`

**测试覆盖 (10 个测试)**:
- IoUringPoller: 构造/队列深度
- IoUringFile: 读取/写入/部分读取
- IoUringSocket: 创建绑定/Echo 服务器
- 性能测试: 文件读取基准/多次小读取

**示例**: `tests/io/io_uring_test.cpp`

---

## 📚 Phase 7: 应用层框架

### Runtime (`include/zlcoro/runtime/runtime.hpp`)

**统一的运行时入口**，整合调度器和 I/O 轮询器：
- ✅ `spawn()`: 提交协程任务
- ✅ `block_on()`: 阻塞等待任务完成
- ✅ `shutdown()`: 优雅关闭
- ✅ 自动管理工作线程和 I/O 轮询

**API**:
```cpp
Runtime runtime(4);  // 4 个工作线程

// 提交任务
runtime.spawn(my_task());

// 阻塞等待
int result = runtime.block_on(compute());

// 优雅关闭
runtime.shutdown();
```

### TcpServer/TcpConnection (`include/zlcoro/net/tcp.hpp`)

**TCP 网络框架**：
- ✅ `TcpListener`: 监听和接受连接
- ✅ `TcpConnection`: 连接读写抽象（缓冲读取）
- ✅ `TcpServer`: 高层服务器框架
- ✅ `TcpClient`: 客户端连接

**API**:
```cpp
TcpServer server;
server.on_connection([](TcpConnection conn) -> Task<void> {
    auto line = co_await conn.read_line();
    co_await conn.write("Echo: " + line);
});
co_await server.serve("0.0.0.0", 8080);
```

### Timer (`include/zlcoro/sync/timer.hpp`)

**协程定时器**：
- ✅ `Timer::sleep()`: 休眠指定时间
- ✅ `Timer::timeout()`: 带超时的操作
- ✅ `Interval`: 周期性定时器

**API**:
```cpp
// 休眠
co_await Timer::sleep(std::chrono::seconds(1));

// 超时
auto result = co_await Timer::timeout(
    some_operation(),
    std::chrono::seconds(5)
);
```

### CancellationToken (`include/zlcoro/sync/cancellation.hpp`)

**协程取消机制**：
- ✅ `CancellationSource`: 取消源
- ✅ `CancellationToken`: 取消令牌
- ✅ 取消回调支持
- ✅ `throw_if_cancelled()` 异常模式

**API**:
```cpp
CancellationSource source;
auto token = source.token();

Task<void> worker(CancellationToken token) {
    while (!token.is_cancelled()) {
        co_await do_work();
    }
}

// 取消任务
source.cancel();
```

### RWLock (`include/zlcoro/sync/rwlock.hpp`)

**协程读写锁**：
- ✅ 多读单写语义
- ✅ `read_lock()` / `write_lock()`
- ✅ `try_read_lock()` / `try_write_lock()`
- ✅ 写锁降级为读锁

**API**:
```cpp
RWLock lock;

// 读取
{
    auto guard = co_await lock.read_lock();
    // 多个协程可以同时读
}

// 写入
{
    auto guard = co_await lock.write_lock();
    // 独占访问
}
```

**测试覆盖 (30 个测试)**:
- Runtime: 构造/关闭/spawn/block_on
- CancellationToken: 状态/回调/异常
- Timer: sleep/deadline
- RWLock: 读锁/写锁/多读者
- 集成测试: 并发任务

---

## 🚀 Phase 8: 生产就绪（已完成）

### HTTP 服务器 (`include/zlcoro/net/http.hpp`)

**轻量级 HTTP/1.1 服务器框架**：
- ✅ `HttpRequest`: HTTP 请求解析（方法、路径、头部、请求体）
- ✅ `HttpResponse`: HTTP 响应构建（状态码、头部、响应体）
- ✅ `HttpServer`: 路由注册和请求分发
- ✅ 支持 GET/POST/PUT/DELETE 等常用方法
- ✅ 支持自定义 404 和错误处理器

**API**:
```cpp
HttpServer server;

server.get("/", [](const HttpRequest& req) -> Task<HttpResponse> {
    co_return HttpResponse::ok("Hello, World!");
});

server.get("/json", [](const HttpRequest& req) -> Task<HttpResponse> {
    co_return HttpResponse::json(R"({"message": "Hello"})");
});

server.post("/echo", [](const HttpRequest& req) -> Task<HttpResponse> {
    co_return HttpResponse::json(req.body());
});

co_await server.serve("0.0.0.0", 8080);
```

### 网络示例

#### Echo 服务器 (`examples/network/echo_server.cpp`)
- ✅ 完整的 TCP Echo 服务器
- ✅ 支持并发连接处理
- ✅ 优雅关闭（信号处理）
- ✅ 连接统计

#### Echo 客户端 (`examples/network/echo_client.cpp`)
- ✅ 交互式 Echo 客户端
- ✅ 自动重连支持

#### HTTP 服务器 (`examples/network/http_server.cpp`)
- ✅ 多路由支持（/, /hello, /json, /time, /echo, /status）
- ✅ HTML/JSON 响应
- ✅ 自定义 404 页面

### 性能基准测试 (`benchmarks/`)

#### 协程基准测试 (`coroutine_bench.cpp`)
- ✅ 协程创建/销毁开销
- ✅ 协程切换延迟
- ✅ Generator 性能
- ✅ 大量协程并发
- ✅ 内存占用估算
- ✅ 协程 vs 函数调用对比

#### 调度器基准测试 (`scheduler_bench.cpp`)
- ✅ 任务提交性能
- ✅ 任务调度延迟
- ✅ 线程池扩展性
- ✅ 任务窃取效率
- ✅ 并发任务吞吐量
- ✅ 协程链式调用性能
- ✅ Runtime 配置对比

#### I/O 基准测试 (`io_bench.cpp`)
- ✅ Epoll 事件处理性能
- ✅ 文件异步读写性能
- ✅ TCP 连接性能
- ✅ 网络吞吐量
- ✅ Timer 精度测试
- ✅ Event Loop 性能

---

## 📖 核心设计原则

从 11 个 Bug 修复中总结的经验：

1. **协程不应在内部重新调度** - 避免嵌套协程和生命周期问题
2. **避免循环中使用 lambda 协程** - 使用独立函数确保生命周期明确
3. **使用 shared_ptr 管理异步对象** - 确保异步执行时对象有效
4. **数据结构要匹配使用场景** - 如定时器按时间排序而非 ID
5. **使用循环而非递归** - 避免协程帧堆叠和栈溢出
6. **添加边界检查** - 防御性编程，检查句柄有效性
7. **Awaiter 数据生命周期** - 等待队列中的数据必须使用 shared_ptr
8. **协程只 resume 一次启动** - 之后由 Scheduler 管理

---

## 📖 参考资料

- `docs/ARCHITECTURE.md` - 架构设计文档
- `docs/API.md` - API 参考手册
- `docs/BENCHMARKS.md` - 性能基准测试报告
- `BUG_FIX.md` - Bug 修复记录和设计原则
- `DEVELOPMENT.md` - 开发指南和最佳实践

---

**项目状态**: 生产就绪，可用于实际项目开发  
**最后更新**: 2025-01-06 | **版本**: 0.9.0  
**贡献者**: 欢迎提交 Issue 和 Pull Request  
**开源协议**: MIT License
