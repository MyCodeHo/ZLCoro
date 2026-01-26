# Per-Core 事件循环架构

## 概述

Per-Core 架构是一种高性能网络编程模型，核心思想是**每个 CPU 核心运行一个独立的事件循环**，完全消除核心间的锁竞争。

## 设计理念

### 传统架构 vs Per-Core 架构

**传统架构（单 EventLoop + 线程池）：**
```
                    ┌──────────────┐
                    │  EventLoop   │ ◄─── 单点瓶颈
                    └──────┬───────┘
                           │
              ┌────────────┼────────────┐
              ▼            ▼            ▼
         ┌────────┐   ┌────────┐   ┌────────┐
         │ Worker │   │ Worker │   │ Worker │
         └────────┘   └────────┘   └────────┘
              │            │            │
              └────────────┼────────────┘
                           │
                    ┌──────▼───────┐
                    │ 共享任务队列 │ ◄─── 锁竞争
                    └──────────────┘
```

**Per-Core 架构：**
```
   ┌────────────────┐ ┌────────────────┐ ┌────────────────┐
   │   Core 0       │ │   Core 1       │ │   Core N       │
   │ ┌────────────┐ │ │ ┌────────────┐ │ │ ┌────────────┐ │
   │ │ EventLoop  │ │ │ │ EventLoop  │ │ │ │ EventLoop  │ │
   │ ├────────────┤ │ │ ├────────────┤ │ │ ├────────────┤ │
   │ │ Coroutines │ │ │ │ Coroutines │ │ │ │ Coroutines │ │
   │ ├────────────┤ │ │ ├────────────┤ │ │ ├────────────┤ │
   │ │ Connections│ │ │ │ Connections│ │ │ │ Connections│ │
   │ └────────────┘ │ │ └────────────┘ │ │ └────────────┘ │
   └────────────────┘ └────────────────┘ └────────────────┘
        独立运行           独立运行           独立运行
        无共享状态         无共享状态         无共享状态
```

### 核心优势

| 优势 | 说明 |
|------|------|
| **零锁竞争** | 每个核心完全独立，无需任何同步原语 |
| **极低延迟** | 无队列等待，事件就绪立即处理 |
| **缓存友好** | 连接数据、协程状态都在本核心缓存中 |
| **线性扩展** | 性能随核心数近乎线性增长 |
| **确定性** | 同一连接总是在同一核心处理 |

## 架构组件

### 1. PerCoreEventLoop（事件循环基类）

```cpp
class PerCoreEventLoop {
public:
    // 绑定到指定 CPU 核心
    void bind_to_core(int core_id);
    
    // 运行事件循环
    void run();
    void run_once();
    void stop();
    
    // 协程调度
    void schedule(std::coroutine_handle<> coro);
    
    // 任务投递
    template<typename Func>
    void post(Func&& func);
    
    // 定时器
    TimerId add_timer(int delay_ms, TimerCallback callback);
    void cancel_timer(TimerId id);
    
    // I/O 事件注册（子类实现）
    virtual void register_read(int fd, std::coroutine_handle<> coro) = 0;
    virtual void register_write(int fd, std::coroutine_handle<> coro) = 0;
};
```

### 2. EpollPerCoreEventLoop（epoll 后端）

基于 epoll 的实现，适用于：
- 网络 I/O 密集型应用
- 需要兼容旧内核的场景
- 不需要文件异步 I/O 的场景

```cpp
class EpollPerCoreEventLoop : public PerCoreEventLoop {
    // 使用 epoll_wait 等待事件
    // 边缘触发模式
    // eventfd 用于唤醒
};
```

### 3. IoUringPerCoreEventLoop（io_uring 后端）

基于 io_uring 的实现，适用于：
- 高性能存储系统
- 需要文件异步 I/O 的场景
- Linux 5.1+ 系统

```cpp
class IoUringPerCoreEventLoop : public PerCoreEventLoop {
    // 支持所有 io_uring 操作
    // 批量提交 SQE
    // 零拷贝
    
    // 原生 io_uring 操作
    std::shared_ptr<Request> prep_read(int fd, void* buf, size_t len, off_t offset);
    std::shared_ptr<Request> prep_write(int fd, const void* buf, size_t len, off_t offset);
    std::shared_ptr<Request> prep_accept(int fd, sockaddr* addr, socklen_t* addrlen);
    std::shared_ptr<Request> prep_send(int fd, const void* buf, size_t len, int flags);
    std::shared_ptr<Request> prep_recv(int fd, void* buf, size_t len, int flags);
    // ...
};
```

### 4. PerCoreRuntime（运行时管理器）

管理多个事件循环的生命周期：

```cpp
class PerCoreRuntime {
public:
    // 创建运行时
    explicit PerCoreRuntime(const Config& config);
    
    // 生命周期
    void start();   // 启动所有核心
    void stop();    // 停止所有核心
    void wait();    // 等待结束
    
    // 核心访问
    PerCoreEventLoop& get_loop(size_t index);
    size_t num_cores() const;
    
    // 负载均衡
    size_t next_core_round_robin();           // 轮询
    size_t select_core_by_fd(int fd) const;   // 按 fd 哈希
    
    // 任务分发
    void schedule_on(size_t core_index, std::coroutine_handle<> coro);
    void post_to(size_t core_index, Func&& func);
};
```

### 5. PerCoreTcpServer（TCP 服务器）

高性能 TCP 服务器实现：

```cpp
class PerCoreTcpServer {
public:
    using Handler = std::function<Task<void>(PerCoreConnection&)>;
    
    // 监听
    bool listen(uint16_t port, const std::string& ip = "0.0.0.0");
    
    // 设置处理器
    void set_handler(Handler handler);
    
    // 启动接受连接
    void start_accept_on_core(size_t core_index);
    void start_accept_all_cores();  // 使用 SO_REUSEPORT
};
```

## 连接分发策略

### 基于 fd 哈希

```cpp
size_t target_core = fd % num_cores;
```

**优点**：
- 确定性：同一连接总是在同一核心
- 简单高效：无需维护映射表
- 均匀分布：fd 通常是递增的

### 基于地址哈希

```cpp
size_t target_core = (ip ^ (port << 16)) % num_cores;
```

**优点**：
- 来自同一客户端的连接在同一核心
- 适合有状态的服务

## 使用场景

### KV 存储系统

```cpp
auto runtime = make_io_uring_runtime(num_cores);
PerCoreTcpServer server(*runtime);

server.set_handler([&kv_store](PerCoreConnection& conn) -> Task<void> {
    while (true) {
        auto cmd = co_await conn.read_command();
        
        switch (cmd.type) {
            case GET:
                // 读取不需要跨核心
                auto value = kv_store.get(cmd.key);
                co_await conn.write_response(value);
                break;
                
            case PUT:
                // 写入提交到 WAL（本核心）
                co_await wal_writer.append_async(cmd.serialize());
                kv_store.put(cmd.key, cmd.value);
                co_await conn.write_response("OK");
                break;
        }
    }
});
```

### 高性能 HTTP 服务器

```cpp
server.set_handler([](PerCoreConnection& conn) -> Task<void> {
    HttpRequest req = co_await parse_request(conn);
    HttpResponse resp = handle_request(req);
    co_await conn.write(resp.serialize());
});
```

## 性能考虑

### CPU 亲和性

```cpp
// 绑定到指定核心
loop.bind_to_core(core_id);
```

使用 `pthread_setaffinity_np` 将线程绑定到指定 CPU 核心，减少上下文切换和缓存失效。

### 批量处理

```cpp
// io_uring 批量提交
loop.prep_read(...);
loop.prep_read(...);
loop.prep_read(...);
loop.submit();  // 一次系统调用
```

### 避免跨核心通信

如果必须跨核心通信，使用 eventfd 或 lockless queue：

```cpp
// 唤醒其他核心
other_loop.post([data]() {
    // 在目标核心执行
});
```

## 与 Seastar 的对比

| 特性 | ZLCoro Per-Core | Seastar |
|------|-----------------|---------|
| 语言标准 | C++20 协程 | C++17 Future |
| 用户态调度 | 简单实现 | 完整实现 |
| 内存管理 | 标准分配器 | Per-Core 分配器 |
| 网络栈 | 内核网络栈 | 可选用户态网络栈 |
| 复杂度 | 低 | 高 |
| 适用场景 | 通用 | 极限性能 |

ZLCoro Per-Core 架构提供了 Seastar 核心思想的简化实现，适合大多数高性能应用。

## 最佳实践

1. **核心数选择**：通常使用物理核心数（排除超线程）
2. **连接分发**：优先使用 fd 哈希，简单且均匀
3. **避免跨核心**：尽量在本核心完成所有操作
4. **使用 io_uring**：文件 I/O 性能约 5x 提升
5. **批量提交**：减少系统调用次数
