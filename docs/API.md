# API 参考文档

## 核心协程类型

### Task<T>

表示返回类型为 T 的异步计算。

```cpp
template<typename T = void>
class Task;
```

#### 示例

```cpp
Task<int> async_computation() {
    // 执行异步工作
    co_return 42;
}

Task<void> main_coroutine() {
    int result = co_await async_computation();
    std::cout << "结果: " << result << "\n";
}
```

#### 方法

- `void resume()` - 恢复协程执行
- `bool done() const` - 检查协程是否完成
- 用于 `co_await` 的 Awaiter 接口

### Generator<T>

使用协程的惰性序列生成器。

```cpp
template<typename T>
class Generator;
```

#### 示例

```cpp
Generator<int> range(int start, int end) {
    for (int i = start; i < end; ++i) {
        co_yield i;
    }
}

// 使用方式
for (int val : range(0, 10)) {
    std::cout << val << " ";
}
```

## 调度器

### ThreadPoolScheduler

用于 CPU 密集型任务的多线程工作窃取调度器。

```cpp
class ThreadPoolScheduler : public Scheduler {
public:
    explicit ThreadPoolScheduler(size_t num_threads);
    
    void schedule(std::coroutine_handle<> handle) override;
    void start() override;
    void stop() override;
};
```

#### 示例

```cpp
ThreadPoolScheduler scheduler(4);  // 4 个工作线程
scheduler.start();

auto task = async_computation();
scheduler.schedule(task.handle());
```

### IOScheduler

基于 Epoll 的 I/O 密集型任务调度器。

```cpp
class IOScheduler : public Scheduler {
public:
    IOScheduler();
    
    void register_read(int fd, std::coroutine_handle<>);
    void register_write(int fd, std::coroutine_handle<>);
    void run_loop();
};
```

## 异步 I/O

### AsyncSocket

非阻塞 Socket 操作。

```cpp
class AsyncSocket {
public:
    Task<size_t> read(char* buffer, size_t size);
    Task<size_t> write(const char* data, size_t size);
    Task<void> connect(const Address& addr);
    Task<AsyncSocket> accept();
    
    void close();
    int fd() const;
};
```

#### 示例

```cpp
Task<void> handle_connection(AsyncSocket socket) {
    char buffer[1024];
    size_t n = co_await socket.read(buffer, sizeof(buffer));
    co_await socket.write(buffer, n);  // 回显
}

Task<void> server() {
    AsyncSocket listener;
    co_await listener.bind("0.0.0.0", 8080);
    
    while (true) {
        auto client = co_await listener.accept();
        schedule(handle_connection(std::move(client)));
    }
}
```

### Buffer

用于 I/O 操作的动态缓冲区。

```cpp
class Buffer {
public:
    void append(const char* data, size_t len);
    void consume(size_t len);
    
    size_t readable_bytes() const;
    size_t writable_bytes() const;
    
    const char* peek() const;
    char* write_ptr();
};
```

## 同步原语

### Mutex

协程感知的互斥锁。

```cpp
class Mutex {
public:
    Task<void> lock();
    void unlock();
    
    class ScopedLock {
    public:
        explicit ScopedLock(Mutex& mutex);
        ~ScopedLock();
    };
};
```

#### 示例

```cpp
Mutex mutex;
int shared_counter = 0;

Task<void> increment() {
    co_await mutex.lock();
    ++shared_counter;
    mutex.unlock();
}

// 或使用 RAII
Task<void> increment_raii() {
    auto lock = co_await Mutex::ScopedLock(mutex);
    ++shared_counter;
}
```

### Channel<T>

多生产者多消费者消息队列。

```cpp
template<typename T>
class Channel {
public:
    explicit Channel(size_t capacity = 0);
    
    Task<void> send(T value);
    Task<T> receive();
    
    void close();
    bool is_closed() const;
};
```

#### 示例

```cpp
Channel<int> chan(10);  // 有缓冲的通道

Task<void> producer() {
    for (int i = 0; i < 100; ++i) {
        co_await chan.send(i);
    }
    chan.close();
}

Task<void> consumer() {
    while (!chan.is_closed()) {
        int val = co_await chan.receive();
        process(val);
    }
}
```

### WaitGroup

等待多个协程完成。

```cpp
class WaitGroup {
public:
    void add(int count = 1);
    void done();
    Task<void> wait();
};
```

#### 示例

```cpp
WaitGroup wg;

for (int i = 0; i < 10; ++i) {
    wg.add();
    schedule([&wg, i]() -> Task<void> {
        co_await do_work(i);
        wg.done();
    });
}

co_await wg.wait();  // 等待所有任务
```

### Semaphore

用于资源管理的计数信号量。

```cpp
class Semaphore {
public:
    explicit Semaphore(size_t initial_count);
    
    Task<void> acquire();
    void release();
};
```

## 定时器

### TimerWheel

使用分层时间轮的高效定时器实现。

```cpp
class TimerWheel {
public:
    void add_timer(uint64_t delay_ms, std::coroutine_handle<>);
    void tick();  // 定期调用
};
```

#### 示例

```cpp
Task<void> sleep(uint64_t ms) {
    struct SleepAwaiter {
        uint64_t delay;
        
        bool await_ready() { return false; }
        void await_suspend(std::coroutine_handle<> h) {
            timer_wheel.add_timer(delay, h);
        }
        void await_resume() {}
    };
    
    co_await SleepAwaiter{ms};
}
```

## 工具类

### ObjectPool<T>

线程安全的对象池，用于减少分配开销。

```cpp
template<typename T>
class ObjectPool {
public:
    T* allocate();
    void deallocate(T* obj);
    
    size_t size() const;
    size_t capacity() const;
};
```

## 错误处理

### Result<T, E>

不使用异常的类型安全错误处理。

```cpp
template<typename T, typename E = Error>
class Result {
public:
    bool is_ok() const;
    bool is_err() const;
    
    T& unwrap();
    E& error();
    
    T value_or(T default_value) const;
};
```

#### 示例

```cpp
Task<Result<int>> safe_divide(int a, int b) {
    if (b == 0) {
        co_return Error{ErrorCode::DIVISION_BY_ZERO};
    }
    co_return a / b;
}

auto result = co_await safe_divide(10, 2);
if (result.is_ok()) {
    std::cout << "结果: " << result.unwrap() << "\n";
} else {
    std::cerr << "错误: " << result.error() << "\n";
}
```

## 最佳实践

### 内存管理

- 使用 RAII 包装器进行自动资源清理
- 为频繁分配的类型使用对象池
- 避免协程句柄中的循环引用

### 性能

- 对于短操作优先使用内联任务
- 使用缓冲通道减少同步开销
- 尽可能批量处理 I/O 操作

### 错误处理

- 对异常情况使用异常机制
- 对预期错误使用 Result<T>
- 始终适当处理 Socket 错误

---

更多示例请参阅 [examples](../examples/) 目录。
