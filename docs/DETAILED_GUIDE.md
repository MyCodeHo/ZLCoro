# ZLCoro 协程框架详细入门指南

> 本文档面向零基础的 C++ 学习者，详细介绍 ZLCoro 项目的每个组成部分。

## 目录

- [第一章：项目概述](#第一章项目概述)
- [第二章：核心概念——什么是协程](#第二章核心概念什么是协程)
- [第三章：项目架构](#第三章项目架构)
- [第四章：文件详解](#第四章文件详解)
- [第五章：文件依赖关系](#第五章文件依赖关系)
- [第六章：使用示例](#第六章使用示例)
- [附录：快速参考](#附录快速参考)

---

## 第一章：项目概述

### 1.1 ZLCoro 是什么？

ZLCoro 是一个基于 **C++20 协程**（Coroutine）的高性能异步编程框架。它的目标是让 C++ 程序员能够像写同步代码一样编写异步程序，同时获得接近系统编程的性能。

**版本：** 0.8.0  
**编译要求：** GCC 10+ 或 Clang 12+，CMake 3.20+，Linux 系统

### 1.2 为什么需要 ZLCoro？

传统的 C++ 异步编程通常需要使用回调函数，代码会变得很复杂：

```cpp
// 传统回调方式（回调地狱）
connect(server, [](auto conn) {
    read(conn, [](auto data) {
        process(data, [](auto result) {
            write(conn, result, [](auto) {
                close(conn);
            });
        });
    });
});
```

使用 ZLCoro，同样的逻辑可以写成：

```cpp
// ZLCoro 协程方式（清晰简洁）
Task<void> handle() {
    auto conn = co_await connect(server);
    auto data = co_await read(conn);
    auto result = co_await process(data);
    co_await write(conn, result);
    co_await close(conn);
}
```

### 1.3 主要功能

| 功能模块 | 描述 |
|---------|------|
| **核心协程** | Task<T> 异步任务、Generator<T> 生成器 |
| **调度器** | 工作窃取调度器、线程池 |
| **网络** | TCP 服务器/客户端、HTTP 服务器 |
| **同步原语** | 互斥锁、读写锁、通道、信号量、等待组 |
| **定时器** | 休眠、超时、周期性执行 |
| **I/O** | 基于 epoll 的事件循环 |

---

## 第二章：核心概念——什么是协程

### 2.1 协程 vs 线程

在理解 ZLCoro 之前，你需要先理解协程的基本概念。

**线程**是操作系统级别的并发单元：
- 由操作系统调度
- 切换开销大（需要保存/恢复寄存器、栈等）
- 每个线程需要独立的栈空间（通常 1-8MB）

**协程**是用户级别的并发单元：
- 由程序员控制切换点（使用 `co_await`）
- 切换开销极小（只需保存几个指针）
- 多个协程可以共享一个线程

```
┌─────────────────────────────────────────────────────────────┐
│                      操作系统                                │
│  ┌─────────┐  ┌─────────┐  ┌─────────┐  ┌─────────┐        │
│  │ 线程 1  │  │ 线程 2  │  │ 线程 3  │  │ 线程 4  │        │
│  │         │  │         │  │         │  │         │        │
│  │ ┌─────┐ │  │ ┌─────┐ │  │ ┌─────┐ │  │ ┌─────┐ │        │
│  │ │协程1│ │  │ │协程3│ │  │ │协程5│ │  │ │协程7│ │        │
│  │ ├─────┤ │  │ ├─────┤ │  │ ├─────┤ │  │ ├─────┤ │        │
│  │ │协程2│ │  │ │协程4│ │  │ │协程6│ │  │ │协程8│ │        │
│  │ └─────┘ │  │ └─────┘ │  │ └─────┘ │  │ └─────┘ │        │
│  └─────────┘  └─────────┘  └─────────┘  └─────────┘        │
└─────────────────────────────────────────────────────────────┘
```

### 2.2 C++20 协程关键字

C++20 引入了三个协程关键字：

| 关键字 | 作用 | 示例 |
|--------|------|------|
| `co_await` | 挂起协程，等待某个操作完成 | `co_await some_task();` |
| `co_return` | 从协程返回值 | `co_return 42;` |
| `co_yield` | 产生一个值并挂起（用于生成器） | `co_yield value;` |

### 2.3 协程的生命周期

```
创建协程 ──→ 首次恢复 ──→ 执行代码 ──→ co_await 挂起
                                         │
                                         ▼
销毁协程 ←── 协程完成 ←── 继续执行 ←── 等待完成后恢复
```

---

## 第三章：项目架构

### 3.1 五层架构设计

ZLCoro 采用分层架构，从底层到高层依次是：

```
┌─────────────────────────────────────────────────────────────┐
│                    应用层 Application                        │
│        examples/、用户代码、HttpServer                        │
├─────────────────────────────────────────────────────────────┤
│                  同步原语层 Synchronization                   │
│     Mutex、RWLock、Channel、Semaphore、WaitGroup、Timer      │
├─────────────────────────────────────────────────────────────┤
│                   异步 I/O 层 Async I/O                      │
│       AsyncSocket、EventLoop、TcpServer、TcpConnection       │
├─────────────────────────────────────────────────────────────┤
│                    调度器层 Scheduler                        │
│    WorkStealingScheduler、ThreadPool、Scheduler、Runtime     │
├─────────────────────────────────────────────────────────────┤
│                  协程基础设施层 Core                          │
│              Task<T>、Generator<T>、Awaiter                  │
└─────────────────────────────────────────────────────────────┘
```

### 3.2 目录结构

```
ZLCoro/
├── include/zlcoro/           # 所有头文件
│   ├── zlcoro.hpp           # 主头文件（包含所有模块）
│   ├── core/                # 核心协程类型
│   │   ├── task.hpp         # Task<T> 异步任务
│   │   └── generator.hpp    # Generator<T> 生成器
│   ├── scheduler/           # 调度器
│   │   ├── scheduler.hpp    # 基础调度器
│   │   ├── work_stealing_scheduler.hpp  # 工作窃取调度器
│   │   ├── thread_pool.hpp  # 线程池
│   │   └── async.hpp        # async_run 工具函数
│   ├── runtime/             # 运行时
│   │   └── runtime.hpp      # 统一运行时入口
│   ├── io/                  # I/O 层
│   │   ├── epoll_poller.hpp # epoll 轮询器
│   │   ├── event_loop.hpp   # 事件循环
│   │   └── async_socket.hpp # 异步 Socket
│   ├── net/                 # 网络层
│   │   ├── tcp.hpp          # TCP 相关类
│   │   └── http.hpp         # HTTP 服务器
│   ├── sync/                # 同步原语
│   │   ├── mutex.hpp        # 互斥锁
│   │   ├── rwlock.hpp       # 读写锁
│   │   ├── channel.hpp      # 通道
│   │   ├── semaphore.hpp    # 信号量
│   │   ├── wait_group.hpp   # 等待组
│   │   ├── timer.hpp        # 定时器
│   │   └── cancellation.hpp # 取消令牌
│   └── utils/               # 工具类
│       └── memory_pool.hpp  # 对象池
├── examples/                # 示例代码
├── tests/                   # 单元测试
├── benchmarks/              # 性能测试
└── docs/                    # 文档
```

---

## 第四章：文件详解

### 4.1 核心协程层 (core/)

#### 4.1.1 task.hpp - 异步任务
//promise是给编译器提供的函数，编译器会根据协程函数的返回类型自动生成promise_type类型，自动调用相应的promise方法来管理协程的生命周期和状态。决定协程的构建，挂起，返回值等行为。
//task类负责提供返回对象，负责规定co_await的行为。

**功能：** 定义 `Task<T>` 类型，这是 ZLCoro 最基本的协程类型。

**核心组件：**

| 类名 | 功能 |
|------|------|
| `TaskPromiseBase` | 协程 Promise 的基类，处理续体（continuation）和挂起逻辑 |
| `TaskPromise<T>` | 有返回值协程的 Promise，存储返回值或异常 |
| `TaskPromise<void>` | void 特化版本，不需要存储返回值 |
| `Task<T>` | 协程任务的句柄包装，支持 co_await |
| `Task<T>::Awaiter` | 实现 co_await 协议的等待器 |

**关键代码解析：**

```cpp
// 协程返回类型，包含一个协程句柄
template <typename T>
class Task {
public:
    using promise_type = TaskPromise<T>;  // C++20 要求的类型别名
    using handle_type = std::coroutine_handle<promise_type>;
    
private:
    handle_type handle_;  // 协程句柄，用于控制协程
};
```

**Promise 的工作原理：**

```cpp
// Promise 控制协程的行为
struct TaskPromise {
    // 协程开始时的行为：挂起，等待被调度
    auto initial_suspend() noexcept { return std::suspend_always{}; }
    
    // 协程结束时的行为：恢复等待者（如果有的话）
    auto final_suspend() noexcept { return FinalAwaiter{}; }
    
    // 存储返回值
    void return_value(T value) { result_ = std::move(value); }
    
    // 存储异常
    void unhandled_exception() { exception_ = std::current_exception(); }
};
```

**依赖关系：**
- 仅依赖 C++ 标准库 `<coroutine>`
- 被几乎所有其他文件依赖

---

#### 4.1.2 generator.hpp - 生成器

**功能：** 定义 `Generator<T>` 类型，用于惰性生成值序列。

**使用场景：**
- 遍历大量数据但不想一次性加载到内存
- 实现无限序列（如斐波那契数列）
- 流式处理数据

**示例：**

```cpp
// 生成斐波那契数列
Generator<int> fibonacci() {
    int a = 0, b = 1;
    while (true) {
        co_yield a;      // 产生当前值并挂起
        int next = a + b;
        a = b;
        b = next;
    }
}

// 使用生成器
auto gen = fibonacci();
for (int i = 0; i < 10; ++i) {
    std::cout << *gen.begin() << " ";  // 0 1 1 2 3 5 8 13 21 34
    ++gen.begin();
}
```

**核心组件：**

| 类名 | 功能 |
|------|------|
| `Generator<T>::promise_type` | 处理 co_yield，存储产生的值 |
| `Generator<T>::Iterator` | 迭代器，支持 range-based for |
| `Generator<T>` | 生成器句柄包装 |

**依赖关系：**
- 仅依赖 C++ 标准库
- 独立模块，不依赖 task.hpp

---

### 4.2 调度器层 (scheduler/)

#### 4.2.1 thread_pool.hpp - 线程池

**功能：** 基础的线程池实现，管理多个工作线程。

**工作原理：**

```
┌──────────────────────────────────────────────────────────┐
│                      ThreadPool                           │
│  ┌────────────────────────────────────────────────────┐  │
│  │              任务队列 (task_queue_)                 │  │
│  │  ┌──────┐ ┌──────┐ ┌──────┐ ┌──────┐ ┌──────┐     │  │
│  │  │任务1│ │任务2│ │任务3│ │任务4│ │任务5│     │  │
│  │  └──────┘ └──────┘ └──────┘ └──────┘ └──────┘     │  │
│  └────────────────────────────────────────────────────┘  │
│                         │ 获取任务                        │
│        ┌────────────────┼────────────────┐               │
│        ▼                ▼                ▼               │
│  ┌──────────┐    ┌──────────┐    ┌──────────┐           │
│  │ Worker 1 │    │ Worker 2 │    │ Worker 3 │  ...      │
│  │  (线程)   │    │  (线程)   │    │  (线程)   │           │
│  └──────────┘    └──────────┘    └──────────┘           │
└──────────────────────────────────────────────────────────┘
```

**核心数据成员：**

```cpp
class ThreadPool {
    std::vector<std::thread> workers_;      // 工作线程数组
    std::deque<Task> task_queue_;           // 任务队列（FIFO）
    std::mutex queue_mutex_;                // 保护任务队列的锁
    std::condition_variable condition_;     // 线程等待/唤醒机制
    std::atomic<bool> stop_;                // 停止标志
};
```

**依赖关系：**
- 仅依赖 C++ 标准库
- 被 scheduler.hpp 和 work_stealing_scheduler.hpp 使用

---

#### 4.2.2 work_stealing_scheduler.hpp - 工作窃取调度器

**功能：** 高性能调度器，每个线程有自己的本地队列，空闲时从其他线程"窃取"任务。

**优势：**
- 减少全局锁竞争
- 提高缓存局部性
- 更好的负载均衡

**工作原理：**

```
┌─────────────────────────────────────────────────────────────────┐
│                    WorkStealingScheduler                         │
│                                                                  │
│  ┌─────────────┐  ┌─────────────┐  ┌─────────────┐              │
│  │   线程 0    │  │   线程 1    │  │   线程 2    │   ...        │
│  │  ┌───────┐  │  │  ┌───────┐  │  │  ┌───────┐  │              │
│  │  │本地队列│  │  │  │本地队列│  │  │  │本地队列│  │              │
│  │  │┌─────┐│  │  │  │┌─────┐│  │  │  │ (空)  │  │              │
│  │  ││任务1││  │  │  ││任务3││  │  │  │       │  │              │
│  │  │├─────┤│  │  │  │├─────┤│  │  │  │       │  │              │
│  │  ││任务2││  │  │  ││任务4││  │  │  │       │  │              │
│  │  │└─────┘│  │  │  │└─────┘│  │  │  │       │  │              │
│  │  └───────┘  │  │  └───────┘  │  │  └───────┘  │              │
│  └─────────────┘  └─────────────┘  └──────┬──────┘              │
│                                           │                      │
│                                    窃取任务 │                      │
│                                           ▼                      │
│                                    从线程0或1偷取                  │
└─────────────────────────────────────────────────────────────────┘
```

**关键策略：**
- **本地操作：** LIFO（后进先出），缓存友好
- **窃取操作：** FIFO（先进先出），保证公平性

**核心代码：**

```cpp
// 从本地队列取任务（后进先出）
std::optional<Task> pop_local(size_t thread_id) {
    auto& queue = local_queues_[thread_id];
    if (queue.empty()) return std::nullopt;
    Task task = queue.back();   // 取最后一个
    queue.pop_back();
    return task;
}

// 从其他队列窃取任务（先进先出）
std::optional<Task> steal_from(size_t victim_id) {
    auto& queue = local_queues_[victim_id];
    if (queue.empty()) return std::nullopt;
    Task task = queue.front();  // 取第一个
    queue.pop_front();
    return task;
}
```

**依赖关系：**
- 仅依赖 C++ 标准库
- 被 runtime.hpp 使用

---

#### 4.2.3 scheduler.hpp - 基础调度器

**功能：** 简单的调度器封装，提供单例访问和协程调度接口。
//给调度器类封装awaiter，协程可以主动调用co_await scheduler来把自己挂起等待工作线程调度。

**核心功能：**

```cpp
class Scheduler {
public:
    static Scheduler& instance();  // 单例访问
    
    void schedule(std::coroutine_handle<> coro);  // 调度协程
    
private:
    ThreadPool thread_pool_;  // 底层线程池
};
```

**辅助 Awaiter：**

```cpp
// 用于切换到调度器线程
struct ScheduleAwaiter {
    void await_suspend(std::coroutine_handle<> coro) {
        Scheduler::instance().schedule(coro);
    }
};

// 使用方式：co_await schedule();
```

**依赖关系：**
- 依赖 thread_pool.hpp
- 被 mutex.hpp、channel.hpp 等同步原语使用

---

#### 4.2.4 async.hpp - 异步运行工具

**功能：** 提供 `async_run()` 函数，将协程提交到调度器并返回 `std::future`。

**使用场景：**
- 从普通函数（如 main）启动协程
- 需要等待协程结果

**示例：**

```cpp
Task<int> compute() {
    co_return 42;
}

int main() {
    auto future = async_run(compute());
    int result = future.get();  // 阻塞等待结果
    std::cout << result << std::endl;  // 42
}
```

**依赖关系：**
- 依赖 task.hpp、scheduler.hpp
- 被用户代码使用

---

#### 4.2.5 runtime.hpp - 统一运行时

**功能：** 整合调度器和 I/O 轮询器，提供统一的运行时入口。

**核心接口：**

```cpp
class Runtime {
public:
    void spawn(Task<void> task);     // 提交协程任务
    void block_on(Task<void> task);  // 阻塞等待任务完成
    void shutdown();                  // 优雅关闭
};
```

**使用示例：**

```cpp
int main() {
    Runtime runtime;
    
    runtime.spawn(background_task());  // 后台运行
    runtime.block_on(main_task());     // 阻塞等待主任务
    
    return 0;
}
```

**依赖关系：**
- 依赖 task.hpp、work_stealing_scheduler.hpp、epoll_poller.hpp
- 是用户使用的主要入口

---

### 4.3 I/O 层 (io/)

#### 4.3.1 epoll_poller.hpp - Epoll 轮询器

**功能：** 封装 Linux epoll API，用于高效的 I/O 事件监听。

**什么是 epoll？**

epoll 是 Linux 提供的 I/O 多路复用机制，可以同时监听多个文件描述符（socket、文件等）的事件。

```
┌──────────────────────────────────────────────────────────────────┐
│                         EpollPoller                               │
│                                                                   │
│  ┌──────────────────────────────────────────────────────────┐    │
│  │                    epoll 实例 (epfd_)                     │    │
│  │   监听的文件描述符:                                        │    │
│  │   fd=5 (socket1) → 等待可读 → 协程 coro1                  │    │
│  │   fd=6 (socket2) → 等待可写 → 协程 coro2                  │    │
│  │   fd=7 (socket3) → 等待可读 → 协程 coro3                  │    │
│  │   fd=wakeup_fd  → 用于中断 epoll_wait                    │    │
│  └──────────────────────────────────────────────────────────┘    │
│                              │                                    │
│                              ▼                                    │
│                      epoll_wait() 阻塞等待                        │
│                              │                                    │
│                              ▼                                    │
│                    返回就绪的文件描述符                            │
│                    恢复对应的协程                                  │
└──────────────────────────────────────────────────────────────────┘
```

**核心功能：**

```cpp
class EpollPoller {
public:
    void add(int fd, uint32_t events, std::coroutine_handle<> coro);
    void modify(int fd, uint32_t events, std::coroutine_handle<> coro);
    void remove(int fd);
    std::vector<std::coroutine_handle<>> poll(int timeout_ms);
    void wakeup();  // 中断 epoll_wait（用于优雅关闭）
    
private:
    int epfd_;                                    // epoll 实例 fd
    int wakeup_fd_;                               // eventfd，用于唤醒
    std::map<int, EventHandler> handlers_;        // fd → 协程映射
};
```

**依赖关系：**
- 依赖 Linux 系统调用（epoll、eventfd）
- 被 event_loop.hpp、async_socket.hpp 使用

---

#### 4.3.2 event_loop.hpp - 事件循环

**功能：** 管理 I/O 事件和协程调度的核心组件，使用 Reactor 模式。

**Reactor 模式工作流程：**

```
┌───────────────────────────────────────────────────────────────┐
│                        EventLoop                               │
│                                                                │
│   while (running_) {                                           │
│       ┌─────────────────────────────────────────────────┐     │
│       │ 1. 处理就绪队列中的协程                          │     │
│       │    for (coro : ready_queue_) coro.resume();     │     │
│       └─────────────────────────────────────────────────┘     │
│                           │                                    │
│                           ▼                                    │
│       ┌─────────────────────────────────────────────────┐     │
│       │ 2. 检查并执行到期的定时器                        │     │
│       └─────────────────────────────────────────────────┘     │
│                           │                                    │
│                           ▼                                    │
│       ┌─────────────────────────────────────────────────┐     │
│       │ 3. 调用 epoll_wait 等待 I/O 事件                │     │
│       │    将就绪的协程加入 ready_queue_                 │     │
│       └─────────────────────────────────────────────────┘     │
│   }                                                            │
└───────────────────────────────────────────────────────────────┘
```

**核心接口：**

```cpp
class EventLoop {
public:
    static EventLoop& instance();  // 单例
    
    void run();                    // 运行事件循环（阻塞）
    void stop();                   // 停止事件循环
    
    void schedule(std::coroutine_handle<> coro);  // 调度协程
    void register_read(int fd, std::coroutine_handle<> coro);   // 注册读事件
    void register_write(int fd, std::coroutine_handle<> coro);  // 注册写事件
    
    TimerId add_timer(int delay_ms, TimerCallback callback);    // 添加定时器
};
```

**依赖关系：**
- 依赖 epoll_poller.hpp
- 被 async_socket.hpp、示例代码使用

---

#### 4.3.3 async_socket.hpp - 异步 Socket

**功能：** 封装非阻塞 socket 操作，提供异步 I/O 接口。

**核心接口：**

```cpp
class AsyncSocket {
public:
    void create();                                    // 创建 socket
    void bind(const std::string& host, int port);    // 绑定地址
    void listen(int backlog);                        // 开始监听
    
    Task<void> connect(const std::string& host, int port);  // 异步连接
    Task<AsyncSocket> accept();                             // 异步接受连接
    Task<std::string> read(size_t max_len);                // 异步读取
    Task<size_t> write(const std::string& data);           // 异步写入
    
private:
    int fd_;                    // socket 文件描述符
    EventLoop& event_loop_;    // 关联的事件循环
};
```

**异步读取的工作原理：**

```cpp
Task<std::string> AsyncSocket::read(size_t max_len) {
    while (true) {
        // 1. 尝试读取
        ssize_t n = ::read(fd_, buffer, max_len);
        
        if (n == -1 && errno == EAGAIN) {
            // 2. 没有数据，挂起协程，等待可读事件
            co_await ReadAwaiter{fd_, event_loop_};
            continue;  // 被唤醒后重试
        }
        
        // 3. 读取成功，返回数据
        co_return std::string(buffer, n);
    }
}
```

**依赖关系：**
- 依赖 task.hpp、event_loop.hpp
- 被 tcp.hpp 使用

---

### 4.4 网络层 (net/)

#### 4.4.1 tcp.hpp - TCP 网络组件

**功能：** 提供完整的 TCP 网络编程支持。
//用户层维护一个读写缓冲区，根据需要进行读写操作，可在未准备就绪的时候主动挂起协程等待。
//封装服务器和客户端的逻辑，也就是调用了 async_socket.hpp 提供的异步 socket 功能。


**包含的类：**

| 类名 | 功能 |
|------|------|
| `TcpConnection` | TCP 连接抽象，提供缓冲读写 |
| `TcpListener` | TCP 监听器，接受新连接 |
| `TcpServer` | TCP 服务器框架 |  //封装调用逻辑，用户只需要设置回调函数就能完成功能
| `TcpClient` | TCP 客户端 |

**TcpConnection 的缓冲读取：**

```
┌─────────────────────────────────────────────────────────────┐
│                      TcpConnection                           │
│                                                              │
│  ┌──────────────────────────────────────────────────────┐   │
│  │              读缓冲区 (read_buffer_)                  │   │
│  │  ┌───┬───┬───┬───┬───┬───┬───┬───┬───┬───┬───┬───┐  │   │
│  │  │ H │ e │ l │ l │ o │ \n │ W │ o │ r │ l │ d │   │  │   │
│  │  └───┴───┴───┴───┴───┴───┴───┴───┴───┴───┴───┴───┘  │   │
│  │        ↑                       ↑                     │   │
│  │    read_pos_                read_end_                │   │
│  │    (已读位置)              (数据结束位置)              │   │
│  └──────────────────────────────────────────────────────┘   │
│                                                              │
│  read_line() 会查找 '\n'，返回 "Hello"                       │
│  再次调用返回 "World"                                        │
└─────────────────────────────────────────────────────────────┘
```

**TcpServer 使用示例：**

```cpp
TcpServer server;

// 设置连接处理器
server.on_connection([](TcpConnection conn) -> Task<void> {
    auto line = co_await conn.read_line();
    co_await conn.write("Echo: " + line);
});

// 启动服务器
co_await server.serve("0.0.0.0", 8080);
```

**依赖关系：**
- 依赖 task.hpp、async_socket.hpp、cancellation.hpp
- 被 http.hpp 和用户代码使用

---

#### 4.4.2 http.hpp - HTTP 服务器

**功能：** 简单的 HTTP/1.1 服务器实现。

//当用户使用该文件的HttpServer类的时候，需要先手动注册路由，也就是指定当用户想要对某个路径进行什么操作的时候，客户端应该怎么办。当服务器获取到客户端发来的消息的时候，直接进行用户设置的操作。
//HttpRequest类封装了解析逻辑，可以解析出发来的请求。
//HttpResponse类封装了回复逻辑，使用工厂方法构造出需要的回复情况。

**包含的类：**

| 类名 | 功能 |
|------|------|
| `HttpRequest` | HTTP 请求解析 |
| `HttpResponse` | HTTP 响应构建 |
| `HttpServer` | HTTP 服务器，支持路由 |

**使用示例：**

```cpp
HttpServer server;

server.get("/", [](const HttpRequest& req) -> Task<HttpResponse> {
    co_return HttpResponse::ok("Hello, World!");
});

server.get("/json", [](const HttpRequest& req) -> Task<HttpResponse> {
    co_return HttpResponse::json(R"({"message": "Hello"})");
});

co_await server.serve("0.0.0.0", 8080);
```

**依赖关系：**
- 依赖 task.hpp、tcp.hpp
- 被用户代码使用

---

### 4.5 同步原语层 (sync/)

#### 4.5.1 mutex.hpp - 协程互斥锁

**功能：** 保护协程之间的共享资源，与 `std::mutex` 类似但专为协程设计。

**与 std::mutex 的区别：**

| 特性 | std::mutex | zlcoro::Mutex |
|------|-----------|---------------|
| 获取锁 | lock()（阻塞线程） | co_await lock()（挂起协程） |
| 等待时 | 线程睡眠 | 协程挂起，线程可执行其他协程 |
| 适用场景 | 多线程 | 多协程 |

**使用示例：**

```cpp
Mutex mtx;

Task<void> critical_section() {
    auto guard = co_await mtx.lock();  // 获取锁
    // 临界区代码
    // guard 析构时自动解锁
}
```

**工作原理：**

```
┌───────────────────────────────────────────────────────────────┐
│                          Mutex                                 │
│                                                                │
│  locked_ = true  (锁被持有)                                    │
│                                                                │
│  等待队列 (waiters_):                                          │
│  ┌──────────┬──────────┬──────────┐                           │
│  │ coro1    │ coro2    │ coro3    │  → 先到先得（FIFO）        │
│  └──────────┴──────────┴──────────┘                           │
│                                                                │
│  unlock() 时：                                                 │
│  1. 从队首取出 coro1                                          │
│  2. 通过 Scheduler 恢复 coro1                                 │
│  3. 锁继续被持有（由 coro1 持有）                              │
└───────────────────────────────────────────────────────────────┘
```

**依赖关系：**
- 依赖 scheduler.hpp
- 独立使用

---

#### 4.5.2 rwlock.hpp - 读写锁

**功能：** 支持多读单写的协程锁。

**特点：**
- 允许多个读者同时读取
- 写者独占访问
- 支持写锁降级为读锁

**使用示例：**

```cpp
RWLock lock;

// 读取（多个协程可以同时读）
Task<void> reader() {
    auto guard = co_await lock.read_lock();
    // 读取共享数据
}

// 写入（独占访问）
Task<void> writer() {
    auto guard = co_await lock.write_lock();
    // 修改共享数据
}
```

**依赖关系：**
- 依赖 C++ 标准库
- 独立使用

---

#### 4.5.3 channel.hpp - 协程通道

**功能：** 协程间通信通道，类似 Go 语言的 channel。

**特点：**
- 支持有缓冲和无缓冲通道
- 支持关闭通道
- 发送/接收都可以挂起

**使用示例：**

```cpp
Channel<int> ch(5);  // 容量为 5 的缓冲通道

// 生产者
Task<void> producer() {
    for (int i = 0; i < 10; ++i) {
        co_await ch.send(i);  // 发送数据
    }
    ch.close();  // 关闭通道
}

// 消费者
Task<void> consumer() {
    while (true) {
        auto value = co_await ch.receive();
        if (!value) break;  // 通道已关闭
        std::cout << *value << std::endl;
    }
}
```

**工作原理：**

```
无缓冲通道 (capacity = 0):
  发送者挂起，直到有接收者 → 直接传递数据

有缓冲通道 (capacity > 0):
  ┌────────────────────────────────────┐
  │          缓冲区 (buffer_)          │
  │  ┌───┬───┬───┬───┬───┐            │
  │  │ 1 │ 2 │ 3 │   │   │  capacity=5│
  │  └───┴───┴───┴───┴───┘            │
  │  发送：缓冲区满则挂起              │
  │  接收：缓冲区空则挂起              │
  └────────────────────────────────────┘
```

**依赖关系：**
- 依赖 task.hpp、scheduler.hpp
- 独立使用

---

#### 4.5.4 semaphore.hpp - 信号量

**功能：** 限制同时访问资源的协程数量。

**使用场景：**
- 限制并发连接数
- 控制资源池大小
- 实现限流

**使用示例：**

```cpp
Semaphore sem(3);  // 最多 3 个并发

Task<void> limited_task() {
    auto guard = co_await sem.scoped_acquire();  // 获取许可（RAII）
    // 访问资源
    // guard 析构时自动释放许可
}
```

**依赖关系：**
- 依赖 scheduler.hpp
- 独立使用

---

#### 4.5.5 wait_group.hpp - 等待组

**功能：** 等待一组协程完成，类似 Go 的 `sync.WaitGroup`。

**使用示例：**

```cpp
WaitGroup wg;
wg.add(3);  // 有 3 个任务

// 启动 3 个任务
for (int i = 0; i < 3; ++i) {
    spawn([&wg]() -> Task<void> {
        // 做一些工作
        wg.done();  // 完成一个
        co_return;
    }());
}

co_await wg.wait();  // 等待所有任务完成
```

**工作原理：**

```
count_ = 3          count_ = 2          count_ = 1          count_ = 0
   │                   │                   │                   │
   ▼                   ▼                   ▼                   ▼
 add(3)              done()              done()              done()
                                                               │
                                                               ▼
                                                        唤醒所有等待者
```

**依赖关系：**
- 依赖 scheduler.hpp
- 独立使用

---

#### 4.5.6 timer.hpp - 定时器

**功能：** 提供协程友好的定时功能。

**核心功能：**

```cpp
// 休眠
co_await Timer::sleep(std::chrono::seconds(1));

// 带超时的操作
auto result = co_await Timer::timeout(
    some_operation(),
    std::chrono::seconds(5)
);

// 周期性执行
auto timer = Timer::interval(std::chrono::seconds(1), []() {
    std::cout << "Tick!" << std::endl;
});
```

**内部实现 - TimerWheel：**

```
┌───────────────────────────────────────────────────────────────┐
│                        TimerWheel                              │
│                                                                │
│  定时器优先队列（按到期时间排序）:                               │
│  ┌────────────────────────────────────────────────────────┐   │
│  │  时间 10:00:01 → coro1                                 │   │
│  │  时间 10:00:02 → coro2                                 │   │
│  │  时间 10:00:05 → coro3                                 │   │
│  │  时间 10:00:10 → callback                              │   │
│  └────────────────────────────────────────────────────────┘   │
│                                                                │
│  工作线程循环:                                                 │
│  1. 等待最早的到期时间                                        │
│  2. 到期后执行回调或恢复协程                                   │
└───────────────────────────────────────────────────────────────┘
```

**依赖关系：**
- 依赖 task.hpp
- 独立使用

---

#### 4.5.7 cancellation.hpp - 取消令牌

**功能：** 实现协作式取消机制。

**组件：**

| 类名 | 功能 |
|------|------|
| `CancellationSource` | 取消源，用于触发取消 |
| `CancellationToken` | 取消令牌，用于检查和响应取消 |

**使用示例：**

```cpp
Task<void> long_running_task(CancellationToken token) {
    while (!token.is_cancelled()) {
        co_await do_some_work();
        
        // 定期检查取消状态
        if (token.is_cancelled()) {
            co_return;  // 提前退出
        }
    }
}

// 使用
CancellationSource source;
spawn(long_running_task(source.token()));

// 稍后取消
source.cancel();
```

**依赖关系：**
- 仅依赖 C++ 标准库
- 被 tcp.hpp、用户代码使用

---

### 4.6 工具层 (utils/)

#### 4.6.1 memory_pool.hpp - 对象池

**功能：** 高性能对象池，减少频繁的内存分配。

**工作原理：**

```
┌───────────────────────────────────────────────────────────────┐
│                        ObjectPool<T>                           │
│                                                                │
│  池中的空闲对象:                                               │
│  ┌──────┐ ┌──────┐ ┌──────┐ ┌──────┐                         │
│  │ obj1 │ │ obj2 │ │ obj3 │ │ obj4 │                         │
│  └──────┘ └──────┘ └──────┘ └──────┘                         │
│                                                                │
│  acquire():                                                    │
│  1. 从池中取出空闲对象（如果有）                               │
│  2. 否则 new 一个新对象                                       │
│  3. 调用构造函数初始化                                        │
│                                                                │
│  release(obj):                                                 │
│  1. 调用析构函数清理                                          │
│  2. 将内存块放回池中（下次重用）                               │
│  3. 如果池满，则真正释放内存                                  │
└───────────────────────────────────────────────────────────────┘
```

**使用示例：**

```cpp
ObjectPool<Connection> pool(32, 1024);  // 初始 32 个，最大 1024 个

// 获取对象
Connection* conn = pool.acquire(host, port);

// 使用对象
conn->send("Hello");

// 释放回池
pool.release(conn);
```

**依赖关系：**
- 仅依赖 C++ 标准库
- 可被任何需要高性能分配的地方使用

---

### 4.7 聚合头文件

#### 4.7.1 zlcoro.hpp - 主头文件

**功能：** 包含所有模块的头文件，方便用户使用。

```cpp
#include <zlcoro/zlcoro.hpp>  // 一行包含所有功能
```

**包含的模块：**
- 核心：task.hpp, generator.hpp
- 运行时：runtime.hpp
- 调度器：scheduler.hpp, work_stealing_scheduler.hpp, async.hpp
- 网络：tcp.hpp
- 同步原语：mutex.hpp, rwlock.hpp, channel.hpp, semaphore.hpp, wait_group.hpp, cancellation.hpp, timer.hpp
- I/O：io.hpp（聚合）
- 工具：memory_pool.hpp

#### 4.7.2 sync.hpp - 同步原语聚合

**功能：** 包含所有同步原语。

```cpp
#include <zlcoro/sync.hpp>  // 包含所有同步原语
```

#### 4.7.3 io.hpp - I/O 聚合

**功能：** 包含所有 I/O 相关头文件。

---

### 4.8 异步文件 I/O (io/)

#### 4.8.1 async_file.hpp - 异步文件操作

**功能：** 提供异步文件读写接口。

**注意：** 由于 Linux 原生 AIO 较复杂，这里使用线程池模拟异步操作。

**核心接口：**

```cpp
class AsyncFile {
public:
    void open(const std::string& path, int mode, int perms);
    void close();
    
    Task<std::string> read_all_async();     // 异步读取全部内容
    Task<size_t> write_async(const std::string& data);  // 异步写入
    Task<std::string> read_async(size_t count);         // 异步读取指定字节
};
```

**使用示例：**

```cpp
AsyncFile file("data.txt", AsyncFile::ReadOnly);
std::string content = co_await file.read_all_async();
```

**依赖关系：**
- 依赖 task.hpp、scheduler.hpp
- 独立使用

---

## 第五章：文件依赖关系

### 5.1 依赖关系图

```
                              ┌──────────────┐
                              │   应用代码    │
                              └──────┬───────┘
                                     │
              ┌──────────────────────┼──────────────────────┐
              ▼                      ▼                      ▼
       ┌─────────────┐        ┌─────────────┐        ┌─────────────┐
       │  zlcoro.hpp │        │  http.hpp   │        │  examples/  │
       │  (聚合)     │        └──────┬──────┘        └──────┬──────┘
       └─────────────┘               │                      │
                                     ▼                      ▼
                              ┌─────────────┐        ┌─────────────┐
                              │   tcp.hpp   │ ←────── │ 网络示例    │
                              └──────┬──────┘        └─────────────┘
                                     │
              ┌──────────────────────┼──────────────────────┐
              ▼                      ▼                      ▼
       ┌─────────────┐        ┌─────────────┐        ┌─────────────┐
       │async_socket │        │cancellation │        │  task.hpp   │
       │   .hpp      │        │   .hpp      │        └──────┬──────┘
       └──────┬──────┘        └─────────────┘               │
              │                                              │
              ▼                                              │
       ┌─────────────┐                                      │
       │ event_loop  │                                      │
       │   .hpp      │                                      │
       └──────┬──────┘                                      │
              │                                              │
              ▼                                              │
       ┌─────────────┐                                      │
       │epoll_poller │                                      │
       │   .hpp      │                                      │
       └──────┬──────┘                                      │
              │                                              │
              └──────────────────────┬──────────────────────┘
                                     │
                                     ▼
                              ┌─────────────┐
                              │  C++ 标准库  │
                              │ <coroutine> │
                              └─────────────┘
```

### 5.2 依赖关系表

| 文件 | 依赖的文件 | 被依赖的文件 |
|------|-----------|-------------|
| task.hpp | 标准库 | 几乎所有文件 |
| generator.hpp | 标准库 | zlcoro.hpp |
| thread_pool.hpp | 标准库 | scheduler.hpp |
| scheduler.hpp | thread_pool.hpp | mutex.hpp, channel.hpp 等 |
| work_stealing_scheduler.hpp | 标准库 | runtime.hpp |
| runtime.hpp | task.hpp, work_stealing_scheduler.hpp, epoll_poller.hpp | 用户代码 |
| epoll_poller.hpp | Linux API | event_loop.hpp |
| event_loop.hpp | epoll_poller.hpp | async_socket.hpp |
| async_socket.hpp | task.hpp, event_loop.hpp | tcp.hpp |
| tcp.hpp | task.hpp, async_socket.hpp, cancellation.hpp | http.hpp, 用户代码 |
| http.hpp | task.hpp, tcp.hpp | 用户代码 |
| mutex.hpp | scheduler.hpp | 用户代码 |
| rwlock.hpp | 标准库 | 用户代码 |
| channel.hpp | task.hpp, scheduler.hpp | 用户代码 |
| semaphore.hpp | scheduler.hpp | 用户代码 |
| wait_group.hpp | scheduler.hpp | 用户代码 |
| timer.hpp | task.hpp | 用户代码 |
| cancellation.hpp | 标准库 | tcp.hpp, 用户代码 |
| memory_pool.hpp | 标准库 | 需要高性能分配的地方 |

### 5.3 模块间关系总结

```
┌─────────────────────────────────────────────────────────────────┐
│                        用户代码/应用层                           │
├─────────────────────────────────────────────────────────────────┤
│  ┌──────────┐  ┌──────────┐  ┌──────────┐  ┌──────────┐        │
│  │HttpServer│  │TcpServer │  │ Channel  │  │  Timer   │        │
│  └────┬─────┘  └────┬─────┘  └────┬─────┘  └────┬─────┘        │
├───────┼─────────────┼─────────────┼─────────────┼───────────────┤
│       │             │             │             │                │
│       ▼             ▼             │             │                │
│  ┌──────────────────────┐        │             │                │
│  │   TcpConnection      │        │             │                │
│  │   TcpListener        │        │             │                │
│  └──────────┬───────────┘        │             │                │
│             │                     │             │                │
│             ▼                     ▼             ▼                │
│  ┌──────────────────┐      ┌──────────────────────┐             │
│  │   AsyncSocket    │      │     Scheduler        │             │
│  └────────┬─────────┘      └──────────┬───────────┘             │
│           │                           │                          │
│           ▼                           ▼                          │
│  ┌──────────────────┐      ┌──────────────────────┐             │
│  │   EventLoop      │      │     ThreadPool       │             │
│  └────────┬─────────┘      └─────────────────────┬┘             │
│           │                                       │              │
│           ▼                                       │              │
│  ┌──────────────────┐                            │              │
│  │   EpollPoller    │                            │              │
│  └────────┬─────────┘                            │              │
├───────────┼──────────────────────────────────────┼──────────────┤
│           │                                       │              │
│           ▼                                       ▼              │
│  ┌──────────────────────────────────────────────────────────┐  │
│  │                      Task<T> / Generator<T>              │  │
│  │                      (核心协程基础设施)                    │  │
│  └──────────────────────────────────────────────────────────┘  │
├─────────────────────────────────────────────────────────────────┤
│                      C++ 标准库 / Linux API                      │
└─────────────────────────────────────────────────────────────────┘
```

---

## 第六章：使用示例

### 6.1 最简单的协程

```cpp
#include <zlcoro/zlcoro.hpp>
#include <iostream>

using namespace zlcoro;

Task<int> simple_task() {
    co_return 42;  // 直接返回值
}

int main() {
    auto task = simple_task();
    auto handle = task.handle();
    handle.resume();  // 恢复协程执行
    
    std::cout << "Result: " << handle.promise().result() << std::endl;
    return 0;
}
```

### 6.2 使用 Runtime

```cpp
#include <zlcoro/zlcoro.hpp>
#include <iostream>

using namespace zlcoro;

Task<void> hello() {
    co_await Timer::sleep(std::chrono::seconds(1));
    std::cout << "Hello, ZLCoro!" << std::endl;
}

int main() {
    Runtime runtime;
    runtime.block_on(hello());
    return 0;
}
```

### 6.3 Echo 服务器

```cpp
#include <zlcoro/zlcoro.hpp>
#include <iostream>

using namespace zlcoro;

Task<void> handle_client(TcpConnection conn) {
    while (conn.is_open()) {
        std::string line = co_await conn.read_line();
        if (line.empty()) break;
        co_await conn.write("Echo: " + line + "\n");
    }
}

Task<void> server() {
    TcpListener listener;
    listener.listen("0.0.0.0", 8888);
    
    std::cout << "Server listening on port 8888" << std::endl;
    
    while (true) {
        auto conn = co_await listener.accept();
        // 注意：这里需要正确管理协程生命周期
        auto task = handle_client(std::move(conn));
        task.handle().resume();
    }
}

int main() {
    EventLoop::instance().schedule(server().handle());
    EventLoop::instance().run();
    return 0;
}
```

### 6.4 生产者-消费者模式

```cpp
#include <zlcoro/zlcoro.hpp>
#include <iostream>

using namespace zlcoro;

Channel<int> ch(10);  // 缓冲区大小为 10

Task<void> producer() {
    for (int i = 0; i < 100; ++i) {
        co_await ch.send(i);
        std::cout << "Produced: " << i << std::endl;
    }
    ch.close();
}

Task<void> consumer() {
    while (true) {
        auto value = co_await ch.receive();
        if (!value) break;  // 通道关闭
        std::cout << "Consumed: " << *value << std::endl;
    }
}

int main() {
    Runtime runtime;
    runtime.spawn(producer());
    runtime.spawn(consumer());
    runtime.block_on([]() -> Task<void> {
        co_await Timer::sleep(std::chrono::seconds(5));
    }());
    return 0;
}
```

---

## 附录：快速参考

### A. 常用类型

| 类型 | 用途 |
|------|------|
| `Task<T>` | 异步任务，返回类型为 T |
| `Task<void>` | 无返回值的异步任务 |
| `Generator<T>` | 惰性生成器 |
| `TcpConnection` | TCP 连接 |
| `TcpListener` | TCP 监听器 |
| `TcpServer` | TCP 服务器 |
| `HttpServer` | HTTP 服务器 |
| `Channel<T>` | 通道 |
| `Mutex` | 互斥锁 |
| `RWLock` | 读写锁 |
| `Semaphore` | 信号量 |
| `WaitGroup` | 等待组 |
| `Timer` | 定时器 |
| `CancellationToken` | 取消令牌 |

### B. 常用函数

| 函数 | 用途 |
|------|------|
| `co_await` | 等待异步操作 |
| `co_return` | 从协程返回 |
| `co_yield` | 在生成器中产生值 |
| `async_run(task)` | 异步运行任务 |
| `Timer::sleep(duration)` | 休眠 |
| `Timer::timeout(task, duration)` | 带超时的操作 |
| `schedule()` | 切换到调度器 |

### C. 编译选项

```cmake
# 启用 io_uring 支持
add_definitions(-DZLCORO_HAS_IO_URING)

# 编译选项
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fcoroutines")
```

### D. 常见问题

**Q: 为什么协程没有执行？**
A: 协程创建后需要 `resume()` 或提交到调度器才会执行。

**Q: 为什么程序崩溃？**
A: 检查协程生命周期，确保 `Task` 对象在协程执行期间存活。

**Q: 如何调试协程？**
A: 使用日志、断点，注意协程可能在不同线程恢复。

---

*文档版本：1.0*  
*最后更新：2025年*  
*适用于 ZLCoro v0.8.0+*
