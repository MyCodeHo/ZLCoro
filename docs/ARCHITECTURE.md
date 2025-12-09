# ZLCoro 架构设计# 架构设计



## 概述## 概述



ZLCoro 是一个基于 C++20 协程的高性能异步编程框架，采用分层架构设计，提供从底层协程原语到高层异步 I/O 的完整解决方案。ZLCoro 是一个高性能的 C++20 协程框架，专为构建异步服务器应用而设计。本文档描述了核心架构和设计决策。



## 系统架构## 系统架构



### 分层设计### 分层设计



``````

┌─────────────────────────────────────┐┌──────────────────────────────────────────────┐

│         应用层                       ││             应用层                            │

│    (用户业务逻辑)                    │├──────────────────────────────────────────────┤

├─────────────────────────────────────┤│          同步原语层                           │

│       异步 I/O 层                    ││  (Mutex, Channel, WaitGroup, Semaphore)      │

│  (AsyncFile, AsyncSocket)           │├──────────────────────────────────────────────┤

├─────────────────────────────────────┤│          异步 I/O 层                          │

│        事件循环层                    ││  (Epoll, Socket, Buffer, EventLoop)          │

│  (EventLoop, EpollPoller)           │├──────────────────────────────────────────────┤

├─────────────────────────────────────┤│          调度器层                             │

│        调度器层                      ││  (ThreadPool, IOScheduler, Timer)            │

│  (Scheduler, ThreadPool)            │├──────────────────────────────────────────────┤

├─────────────────────────────────────┤│        协程基础设施层                         │

│      协程基础设施层                  ││  (Task, Promise, Awaiter, Generator)         │

│  (Task, Generator)                  │├──────────────────────────────────────────────┤

└─────────────────────────────────────┘│          操作系统层                           │

```│      (Linux, Epoll, Threads)                 │

└──────────────────────────────────────────────┘

## 核心组件```



### 1. 协程基础设施## 核心组件



#### Task<T>### 1. 协程基础设施



异步计算的基础类型，支持惰性求值和协程链。#### Task<T>



**特性**:提供 RAII 语义和类型安全返回值的基础协程类型。

- 惰性求值：创建时不立即执行

- 协程链：支持 `co_await` 组合**核心特性：**

- 异常传播：自动传播异常- 显式恢复控制的惰性求值

- RAII 管理：自动清理资源- 通过模板参数提供类型安全的返回值

- 通过 RAII 自动管理资源

**实现要点**:- 支持异常传播

```cpp- 通过 co_await 可组合

template<typename T>

class Task {**设计：**

    struct promise_type {```cpp

        auto initial_suspend() { return std::suspend_always{}; }  // 惰性template<typename T>

        auto final_suspend() noexcept { return FinalAwaiter{}; }  // 链式class Task {

        void unhandled_exception() { exception_ = std::current_exception(); }    struct promise_type {

    };        Task get_return_object();

};        std::suspend_always initial_suspend() noexcept;

```        std::suspend_always final_suspend() noexcept;

        void unhandled_exception();

**使用场景**:        void return_value(T value);  // void 类型使用 return_void()

- 异步计算    };

- 协程组合    

- 异常处理    // Awaiter 接口

    bool await_ready() const noexcept;

---    std::coroutine_handle<> await_suspend(std::coroutine_handle<>);

    T await_resume();

#### Generator<T>};

```

惰性序列生成器。

#### Generator<T>

**特性**:

- 按需生成：只在迭代时生成值基于迭代器的协程，用于惰性值生成。

- 范围支持：兼容 C++20 ranges

- 内存高效：不需要提前生成所有值**使用场景：**

- 惰性序列生成

**实现要点**:- 流处理

```cpp- 迭代算法

template<typename T>

class Generator {### 2. 调度器

    struct promise_type {

        auto yield_value(T value) {#### 工作窃取线程池调度器

            current_value_ = std::move(value);

            return std::suspend_always{};**设计原则：**

        }- 每个工作线程使用无锁工作窃取队列

    };- 本地任务 LIFO 执行（缓存友好）

};- 远程队列 FIFO 窃取（减少竞争）

```- 动态负载均衡



**使用场景**:**实现：**

- 序列生成- 每线程工作队列最小化锁竞争

- 数据流处理- 随机化窃取减少惊群效应

- 惰性求值- 任务优先级支持延迟敏感操作



---**性能特征：**

- 任务提交：均摊 O(1)

### 2. 调度器层- 任务窃取：O(1)

- 可扩展性：与 CPU 核心数线性扩展

#### ThreadPool

#### I/O 调度器

固定大小的线程池，用于执行 CPU 密集型任务。

**设计：**

**设计**:- 基于 Epoll 的事件循环

- 固定线程数（默认 16）- 单次事件模式实现精确控制

- 任务队列 + 条件变量- 边缘触发实现高性能

- 工作线程自动从队列取任务- 与线程池集成处理 CPU 密集型任务



**实现要点**:**事件处理：**

```cpp```

class ThreadPool {1. epoll_wait() 阻塞直到事件就绪

    std::vector<std::thread> workers_;2. 获取就绪的文件描述符

    std::queue<std::function<void()>> tasks_;3. 恢复关联的协程

    std::mutex mutex_;4. 协程处理 I/O，可能再次挂起

    std::condition_variable cv_;```

};

```### 3. 异步 I/O



**使用场景**:#### Socket 抽象

- CPU 密集型任务

- 后台任务执行**非阻塞操作：**

- 阻塞操作隔离```cpp

class AsyncSocket {

---    Task<size_t> read(char* buf, size_t len);

    Task<size_t> write(const char* buf, size_t len);

#### Scheduler    Task<void> connect(const Address& addr);

    Task<AsyncSocket> accept();

全局协程调度器（单例模式）。};

```

**设计**:

- 单例模式：全局唯一实例**零拷贝优化：**

- 封装 ThreadPool- 文件传输使用 sendfile()

- 提供便捷的调度接口- 共享缓冲池

- 分散-聚集 I/O

**调度策略**:

1. 协程创建时不立即执行#### 缓冲区管理

2. 通过 `async_run()` 提交到线程池

3. 线程池工作线程执行协程**设计：**

4. 使用 `sync_wait()` 同步等待结果- 自动扩展的环形缓冲区

- 读写索引跟踪

**使用场景**:- 内存池集成进行分配

- 协程调度- 共享缓冲区的写时复制

- 异步任务执行

- 全局资源管理### 4. 同步原语



---#### 协程 Mutex



### 3. 事件循环层**无锁快速路径：**

- 无竞争获取使用原子 CAS

#### EpollPoller- 仅在竞争时使用等待队列

- 支持 RAII 锁保护

Linux epoll 的 C++ 封装。

**实现：**

**设计**:```cpp

- RAII 管理 epoll fdclass Mutex {

- 注册 fd + 事件 + 协程句柄    std::atomic<bool> locked_;

- poll() 返回就绪的协程列表    std::queue<std::coroutine_handle<>> waiters_;

    

**实现要点**:public:

```cpp    Task<void> lock();

class EpollPoller {    void unlock();

    int epoll_fd_;};

    std::unordered_map<int, std::coroutine_handle<>> fd_map_;```

    

    void add(int fd, uint32_t events, std::coroutine_handle<> coro);#### Channel<T>

    std::vector<std::coroutine_handle<>> poll(int timeout);

};**多生产者多消费者队列：**

```- 缓冲和无缓冲模式

- 队列满/空时协程挂起

**使用场景**:- 关闭语义用于优雅停机

- I/O 多路复用

- 网络事件监听**使用场景：**

- 高并发 I/O- 生产者-消费者模式

- 管道处理

---- 消息传递



#### EventLoop### 5. 定时器系统



Reactor 模式事件循环。#### 时间轮实现



**设计**:**分层时间轮：**

- 单例模式- 4 个时间轮：毫秒、秒、分钟、小时

- 集成 EpollPoller- O(1) 定时器插入和删除

- 就绪队列优先执行- 高效处理大量定时器

- 定时器支持

**精度：**

**事件处理流程**:- 毫秒级粒度

```- 惰性级联减少开销

1. 检查就绪队列 → 有任务 → 执行协程

                  ↓## 内存管理

2. 调用 epoll_wait → 有事件 → 恢复协程

                  ↓### 协程帧分配

3. 处理定时器 → 超时 → 恢复协程

                  ↓**堆分配：**

4. 返回步骤 1- 编译器在堆上分配协程帧

```- 通过重载 operator new/delete 可定制

- 与内存池集成提高效率

**使用场景**:

- 网络服务器### 对象池

- 异步 I/O

- 事件驱动架构**目的：**

- 减少分配开销

---- 提高缓存局部性

- 最小化内存碎片

### 4. 异步 I/O 层

**策略：**

#### AsyncFile- 线程本地自由列表

- 批量分配/释放

异步文件操作（线程池模拟）。- 按大小类别隔离



**设计选择**:## 错误处理

- 使用线程池而非 Linux AIO

- 原因：AIO 复杂且兼容性差### 异常支持

- 权衡：CPU 开销换取简单性

异常在协程边界自然传播：

**实现策略**:- 在 promise_type::unhandled_exception() 中捕获

- 同步方法：`read_all()`, `write()`, `sync()`- 存储在 promise 对象中

- 异步接口：`Task<std::string> read_file()`- 在 await_resume() 中重新抛出

- 调度分离：由 `async_run()` 统一调度

### 错误码

**使用场景**:

- 文件读写对于性能关键路径，Result<T, E> 类型提供：

- 配置加载- 零开销错误处理

- 日志写入- 显式错误检查

- 可组合的错误传播

---

## 性能优化

#### AsyncSocket

### 缓存优化

异步网络 Socket（真正的异步 I/O）。

1. **LIFO 任务执行**：热数据保留在缓存中

**设计**:2. **数据结构对齐**：防止伪共享

- 基于 epoll 的非阻塞 I/O3. **内存布局**：将频繁访问的数据放在一起

- ReadAwaiter/WriteAwaiter

- 需要 EventLoop 运行### 无锁算法



**Awaiter 实现**:1. **工作窃取队列**：单生产者无锁

```cpp2. **原子操作**：用于简单状态转换

struct ReadAwaiter {3. **RCU 模式**：用于读密集型工作负载

    bool await_ready() { return false; }  // 总是挂起

    ### 零拷贝 I/O

    void await_suspend(std::coroutine_handle<> coro) {

        EventLoop::instance().register_read(fd_, coro);  // 注册到 epoll1. **直接内核绕过**：适用场景

    }2. **缓冲区共享**：最小化数据拷贝

    3. **分散-聚集**：高效的向量 I/O

    std::string await_resume() { 

        return read_from_fd();  // 读取数据## 可扩展性

    }

};### 垂直扩展

```

- 高效使用所有 CPU 核心

**使用场景**:- NUMA 感知调度（未来）

- 网络服务器- 延迟敏感任务的 CPU 亲和性

- 网络客户端

- 高并发连接### 水平扩展



---- 为分布式部署而设计

- 状态可外部化

## 设计原则- RPC 网络协议支持



### 1. 单一职责## 测试策略



每个组件只负责一件事：### 单元测试

- Task：协程抽象

- Scheduler：任务调度- Google Test 框架

- EventLoop：事件分发- 组件级隔离

- AsyncFile：文件操作- 模拟依赖进行受控测试



### 2. 依赖倒置### 集成测试



高层不依赖低层，都依赖抽象：- 端到端场景

- EventLoop 不依赖具体的 Socket 实现- 并发执行压力测试

- Scheduler 不依赖具体的任务类型- 故障注入测试



### 3. 开闭原则### 性能测试



对扩展开放，对修改封闭：- Google Benchmark 框架

- 可以添加新的 Awaiter 类型- 延迟百分位跟踪（p50、p99、p999）

- 可以实现新的调度策略- 吞吐量测量

- 与行业标准对比

### 4. 最小惊讶原则

## 未来增强

API 行为符合直觉：

- `co_await` 等待异步操作1. **io_uring 支持**：Linux 下一代异步 I/O

- `async_run()` 异步执行2. **NUMA 感知**：更好的多插槽可扩展性

- `sync_wait()` 同步等待3. **分布式协调**：服务网格集成

4. **高级分析**：内置性能监控

---

---

## 关键设计决策

该架构为构建高性能异步应用提供了坚实基础，同时保持了代码清晰性和可维护性。

### 1. 为什么使用惰性求值？

**优点**:
- 显式控制：用户决定何时执行
- 可组合性：多个 Task 可以组合
- 资源管理：避免过早分配资源

**缺点**:
- 需要显式调用：忘记执行会导致不执行

**权衡**: 为了可组合性和控制性，选择惰性求值。

---

### 2. 为什么文件 I/O 用线程池？

**原因**:
- Linux AIO 复杂且兼容性差
- 线程池简单且可靠
- 文件 I/O 通常不是瓶颈

**权衡**: 牺牲一些性能换取简单性和可维护性。

---

### 3. 为什么 AsyncSocket 需要 EventLoop？

**原因**:
- 网络 I/O 是真正的异步
- epoll 需要事件循环
- 避免阻塞线程池

**设计**: EventLoop 在独立线程运行，不阻塞主线程。

---

### 4. 为什么不在协程内部再次调度？

**问题**: 
```cpp
Task<void> bad_example() {
    co_await schedule();  // 危险！
    // ...
}
```

**原因**:
- 生命周期问题：协程可能已被销毁
- 难以调试：调度链复杂
- use-after-free：协程句柄悬空

**解决方案**: 由 `async_run()` 统一调度。

---

## 性能考虑

### 协程开销

- **创建开销**: ~100ns（堆分配）
- **切换开销**: ~50ns（保存/恢复状态）
- **内存开销**: ~2KB/协程（栈帧 + 状态）

### 优化策略

1. **避免过度分配**: 使用对象池
2. **批量处理**: 减少系统调用
3. **零拷贝**: 使用 `std::string_view`
4. **内存对齐**: 优化缓存命中

---

## 未来改进

### 短期
1. 实现同步原语（Mutex, Channel）
2. 完善 AsyncSocket 集成测试
3. 添加更多示例

### 中期
4. 工作窃取调度器
5. io_uring 支持
6. 内存池优化

### 长期
7. HTTP 协议支持
8. 完整的网络框架
9. 生产级性能优化

---

## 参考资料

- [C++20 协程提案](https://en.cppreference.com/w/cpp/language/coroutines)
- [Lewis Baker 的协程教程](https://lewissbaker.github.io/)
- [cppcoro](https://github.com/lewissbaker/cppcoro) - 参考实现
- [Seastar](https://github.com/scylladb/seastar) - 高性能框架

---

## 相关文档

- [API 参考](API.md)
- [项目进度](../PROGRESS.md)
- [性能基准](BENCHMARKS.md)
