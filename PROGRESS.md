# ZLCoro 项目进度

> 基于 C++20 协程的高性能异步编程框架开发进度  
> **最后更新**: 2025-12-18 | **版本**: 0.5.0

## 📊 总体进度

### 🎯 里程碑状态
- ✅ **Phase 1**: Task 协程类型
- ✅ **Phase 2**: Generator 生成器
- ✅ **Phase 3**: Scheduler 调度器
- ✅ **Phase 4**: Async I/O 异步 I/O
- ✅ **Phase 5**: 同步原语 (Channel/Mutex/Semaphore/WaitGroup)
- 🚧 **Phase 6**: 性能优化 (工作窃取/内存池/io_uring)
- 🚧 **Phase 7**: 高级功能 (HTTP/WebSocket/DNS)
- 🚧 **Phase 8**: 生产就绪 (基准测试/文档/部署)

### 🧪 测试状态
**65/65 tests passing (100%)** 🎉

| 模块 | 测试数 | 状态 |
|------|--------|------|
| Task | 15 | ✅ |
| Generator | 16 | ✅ |
| Scheduler | 13 | ✅ |
| I/O | 7 | ✅ |
| Sync | 14 | ✅ |

### 📦 代码统计
- **头文件**: 16 个（Header-Only）
- **测试文件**: 5 个
- **示例程序**: 5 个
- **代码行数**: ~6500 行

---

## 🏗️ 架构概览

```
┌─────────────────────────────────────┐
│         应用层                       │
├─────────────────────────────────────┤
│    同步原语 (sync/)                  │
│    Channel | Mutex | WaitGroup      │
├─────────────────────────────────────┤
│    异步 I/O (io/)                    │
│    EpollPoller | EventLoop          │
│    AsyncFile | AsyncSocket          │
├─────────────────────────────────────┤
│    调度器 (scheduler/)               │
│    ThreadPool | Scheduler           │
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

### 关键 Bug 修复

**Bug #10: Channel Awaiter 悬空指针**
- 问题: Awaiter 是临时对象，存储栈变量地址导致悬空指针
- 修复: 使用 `shared_ptr<T>` 和 `shared_ptr<optional<T>>` 管理数据

**Bug #11: async_run 重复 resume**
- 问题: `sync_wait()` 循环 resume，协程被 Scheduler 和 sync_wait 同时 resume
- 修复: 只 resume 一次启动，使用监控线程轮询 `done()`

### 测试覆盖 (14 个测试)
- Channel: 基础发送接收/缓冲/多生产者消费者/关闭
- Mutex: 基础锁/多任务/try_lock
- Semaphore: 获取释放/并发限制/RAII/try_acquire
- WaitGroup: 基础等待/多等待者
- 集成测试: 生产者消费者模式

**示例**: `examples/05_sync_primitives.cpp` (6 个示例)

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

## 🚀 下一步计划

### Phase 6: 性能优化
- [ ] 工作窃取调度器 (Work-Stealing Scheduler)
- [ ] 协程池和内存池
- [ ] io_uring 支持（真正的异步文件 I/O）
- [ ] 优化 async_run（回调机制代替轮询）

### Phase 7: 高级功能
- [ ] HTTP 客户端/服务器
- [ ] Echo 服务器示例
- [ ] DNS 解析器
- [ ] WebSocket 支持

### Phase 8: 生产就绪
- [ ] 完善性能基准测试
- [ ] 压力测试和稳定性验证
- [ ] 完整的 API 文档
- [ ] 生产环境部署指南

---

## 📖 参考资料

- `docs/ARCHITECTURE.md` - 架构设计文档
- `docs/API.md` - API 参考手册
- `BUG_FIX.md` - Bug 修复记录和设计原则

---

**项目状态**: 基础功能完成，进入优化和高级功能阶段  
**贡献者**: 欢迎提交 Issue 和 Pull Request  
**开源协议**: MIT License
