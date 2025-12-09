# ZLCoro API 参考文档# API 参考文档



本文档描述 ZLCoro 框架的主要 API 接口。## 核心协程类型



## 目录### Task<T>



- [核心协程类型](#核心协程类型)表示返回类型为 T 的异步计算。

- [调度器](#调度器)

- [异步 I/O](#异步-io)```cpp

template<typename T = void>

---class Task;

```

## 核心协程类型

#### 示例

### Task<T>

```cpp

表示一个异步计算任务，支持惰性求值和协程链。Task<int> async_computation() {

    // 执行异步工作

**头文件**: `#include "zlcoro/core/task.hpp"`    co_return 42;

}

```cpp

template<typename T = void>Task<void> main_coroutine() {

class Task;    int result = co_await async_computation();

```    std::cout << "结果: " << result << "\n";

}

**特性**:```

- 惰性求值：创建时不立即执行

- 协程链：支持 `co_await` 另一个 Task#### 方法

- 异常传播：自动传播协程中的异常

- 移动语义：只能移动，不可复制- `void resume()` - 恢复协程执行

- `bool done() const` - 检查协程是否完成

**方法**:- 用于 `co_await` 的 Awaiter 接口

```cpp

T sync_wait();                          // 同步等待协程完成### Generator<T>

bool done() const noexcept;             // 检查是否完成

std::coroutine_handle<> handle();       // 获取协程句柄使用协程的惰性序列生成器。

```

```cpp

**示例**:template<typename T>

```cppclass Generator;

Task<int> compute() {```

    co_return 42;

}#### 示例



Task<std::string> process() {```cpp

    int value = co_await compute();Generator<int> range(int start, int end) {

    co_return "Result: " + std::to_string(value);    for (int i = start; i < end; ++i) {

}        co_yield i;

    }

int main() {}

    std::string result = process().sync_wait();

    return 0;// 使用方式

}for (int val : range(0, 10)) {

```    std::cout << val << " ";

}

---```



### Generator<T>## 调度器



惰性序列生成器，使用协程按需生成值。### ThreadPoolScheduler



**头文件**: `#include "zlcoro/core/generator.hpp"`用于 CPU 密集型任务的多线程工作窃取调度器。



```cpp```cpp

template<typename T>class ThreadPoolScheduler : public Scheduler {

class Generator;public:

```    explicit ThreadPoolScheduler(size_t num_threads);

    

**特性**:    void schedule(std::coroutine_handle<> handle) override;

- 惰性求值：按需生成值    void start() override;

- 范围支持：支持范围 for 循环    void stop() override;

- 迭代器接口：提供 begin/end};

```

**方法**:

```cpp#### 示例

Iterator begin();        // 获取开始迭代器

Iterator end();          // 获取结束迭代器```cpp

bool next();             // 生成下一个值ThreadPoolScheduler scheduler(4);  // 4 个工作线程

T& value();              // 获取当前值scheduler.start();

```

auto task = async_computation();

**示例**:scheduler.schedule(task.handle());

```cpp```

Generator<int> range(int start, int end) {

    for (int i = start; i < end; ++i) {### IOScheduler

        co_yield i;

    }基于 Epoll 的 I/O 密集型任务调度器。

}

```cpp

Generator<int> fibonacci(int n) {class IOScheduler : public Scheduler {

    int a = 0, b = 1;public:

    for (int i = 0; i < n; ++i) {    IOScheduler();

        co_yield a;    

        int next = a + b;    void register_read(int fd, std::coroutine_handle<>);

        a = b;    void register_write(int fd, std::coroutine_handle<>);

        b = next;    void run_loop();

    }};

}```



int main() {## 异步 I/O

    for (int val : range(0, 5)) {

        std::cout << val << " ";### AsyncSocket

    }

    return 0;非阻塞 Socket 操作。

}

``````cpp

class AsyncSocket {

---public:

    Task<size_t> read(char* buffer, size_t size);

## 调度器    Task<size_t> write(const char* data, size_t size);

    Task<void> connect(const Address& addr);

### ThreadPool    Task<AsyncSocket> accept();

    

固定大小的线程池。    void close();

    int fd() const;

**头文件**: `#include "zlcoro/scheduler/thread_pool.hpp"`};

```

```cpp

class ThreadPool {#### 示例

public:

    explicit ThreadPool(size_t num_threads = 16);```cpp

    Task<void> handle_connection(AsyncSocket socket) {

    template<typename Func>    char buffer[1024];

    void submit(Func&& func);    size_t n = co_await socket.read(buffer, sizeof(buffer));

};    co_await socket.write(buffer, n);  // 回显

```}



**示例**:Task<void> server() {

```cpp    AsyncSocket listener;

ThreadPool pool(4);    co_await listener.bind("0.0.0.0", 8080);

pool.submit([]() {    

    std::cout << "Task running\n";    while (true) {

});        auto client = co_await listener.accept();

```        schedule(handle_connection(std::move(client)));

    }

---}

```

### Scheduler

### Buffer

全局协程调度器（单例）。

用于 I/O 操作的动态缓冲区。

**头文件**: `#include "zlcoro/scheduler/scheduler.hpp"`

```cpp

```cppclass Buffer {

class Scheduler {public:

public:    void append(const char* data, size_t len);

    static Scheduler& instance();    void consume(size_t len);

    void schedule(std::coroutine_handle<> coro);    

    ThreadPool& thread_pool();    size_t readable_bytes() const;

};    size_t writable_bytes() const;

    

// 辅助函数    const char* peek() const;

ScheduleAwaiter schedule();              // 切换到线程池    char* write_ptr();

NewThreadAwaiter resume_on_new_thread(); // 在新线程恢复};

``````



**示例**:## 同步原语

```cpp

Task<void> background_work() {### Mutex

    co_await schedule();  // 切换到线程池

    // CPU 密集型工作协程感知的互斥锁。

}

``````cpp

class Mutex {

---public:

    Task<void> lock();

### async_run    void unlock();

    

异步执行协程，返回 future。    class ScopedLock {

    public:

**头文件**: `#include "zlcoro/scheduler/async.hpp"`        explicit ScopedLock(Mutex& mutex);

        ~ScopedLock();

```cpp    };

template<typename T>};

std::future<T> async_run(Task<T> task);```

```

#### 示例

**示例**:

```cpp```cpp

Task<int> compute() {Mutex mutex;

    co_return 42;int shared_counter = 0;

}

Task<void> increment() {

int main() {    co_await mutex.lock();

    auto future = async_run(compute());    ++shared_counter;

    int result = future.get();    mutex.unlock();

    return 0;}

}

```// 或使用 RAII

Task<void> increment_raii() {

---    auto lock = co_await Mutex::ScopedLock(mutex);

    ++shared_counter;

## 异步 I/O}

```

### AsyncFile

### Channel<T>

异步文件操作（使用线程池模拟）。

多生产者多消费者消息队列。

**头文件**: `#include "zlcoro/io/async_file.hpp"`

```cpp

```cpptemplate<typename T>

class AsyncFile {class Channel {

public:public:

    enum OpenMode {    explicit Channel(size_t capacity = 0);

        ReadOnly, WriteOnly, ReadWrite,    

        Create, Truncate, Append    Task<void> send(T value);

    };    Task<T> receive();

        

    AsyncFile(const std::string& path, int mode, int perms = 0644);    void close();

        bool is_closed() const;

    std::string read_all();};

    std::string read(size_t count);```

    size_t write(const std::string& data);

    void sync();#### 示例

    off_t seek(off_t offset, int whence = SEEK_SET);

};```cpp

Channel<int> chan(10);  // 有缓冲的通道

// 便捷函数

Task<std::string> read_file(const std::string& path);Task<void> producer() {

Task<void> write_file(const std::string& path, const std::string& content);    for (int i = 0; i < 100; ++i) {

Task<void> append_file(const std::string& path, const std::string& content);        co_await chan.send(i);

```    }

    chan.close();

**示例**:}

```cpp

Task<void> file_ops() {Task<void> consumer() {

    co_await write_file("/tmp/test.txt", "Hello!");    while (!chan.is_closed()) {

    std::string content = co_await read_file("/tmp/test.txt");        int val = co_await chan.receive();

    co_await append_file("/tmp/test.txt", "\nNew line");        process(val);

}    }

}

int main() {```

    async_run(file_ops()).get();

    return 0;### WaitGroup

}

```等待多个协程完成。



---```cpp

class WaitGroup {

### AsyncSocketpublic:

    void add(int count = 1);

异步网络 Socket（基于 epoll）。    void done();

    Task<void> wait();

**头文件**: `#include "zlcoro/io/async_socket.hpp"`};

```

```cpp

class AsyncSocket {#### 示例

public:

    void bind(const std::string& host, uint16_t port);```cpp

    void listen(int backlog = 128);WaitGroup wg;

    Task<AsyncSocket> accept();

    Task<void> connect(const std::string& host, uint16_t port);for (int i = 0; i < 10; ++i) {

    Task<std::string> read(size_t max_len = 4096);    wg.add();

    Task<size_t> write(const std::string& data);    schedule([&wg, i]() -> Task<void> {

};        co_await do_work(i);

```        wg.done();

    });

**注意**: AsyncSocket 需要 EventLoop 在独立线程运行。}



**示例**:co_await wg.wait();  // 等待所有任务

```cpp```

Task<void> echo_server() {

    AsyncSocket server;### Semaphore

    server.bind("127.0.0.1", 8080);

    server.listen();用于资源管理的计数信号量。

    

    while (true) {```cpp

        AsyncSocket client = co_await server.accept();class Semaphore {

        std::string data = co_await client.read();public:

        co_await client.write(data);  // Echo    explicit Semaphore(size_t initial_count);

    }    

}    Task<void> acquire();

```    void release();

};

---```



### EpollPoller## 定时器



Linux epoll 封装。### TimerWheel



**头文件**: `#include "zlcoro/io/epoll_poller.hpp"`使用分层时间轮的高效定时器实现。



```cpp```cpp

class EpollPoller {class TimerWheel {

public:public:

    void add(int fd, uint32_t events, std::coroutine_handle<> coro);    void add_timer(uint64_t delay_ms, std::coroutine_handle<>);

    void modify(int fd, uint32_t events);    void tick();  // 定期调用

    void remove(int fd);};

    std::vector<std::coroutine_handle<>> poll(int timeout_ms = -1);```

};

```#### 示例



**事件类型**: `EPOLLIN`, `EPOLLOUT`, `EPOLLERR`, `EPOLLHUP`, `EPOLLET````cpp

Task<void> sleep(uint64_t ms) {

---    struct SleepAwaiter {

        uint64_t delay;

### EventLoop        

        bool await_ready() { return false; }

Reactor 模式事件循环。        void await_suspend(std::coroutine_handle<> h) {

            timer_wheel.add_timer(delay, h);

**头文件**: `#include "zlcoro/io/event_loop.hpp"`        }

        void await_resume() {}

```cpp    };

class EventLoop {    

public:    co_await SleepAwaiter{ms};

    static EventLoop& instance();}

    ```

    void run();

    void stop();## 工具类

    void schedule(std::coroutine_handle<> coro);

    void register_read(int fd, std::coroutine_handle<> coro);### ObjectPool<T>

    void register_write(int fd, std::coroutine_handle<> coro);

};线程安全的对象池，用于减少分配开销。

```

```cpp

---template<typename T>

class ObjectPool {

## 使用建议public:

    T* allocate();

### 生命周期管理    void deallocate(T* obj);

1. 使用 `std::shared_ptr` 管理共享状态    

2. 避免捕获栈上的临时对象    size_t size() const;

3. 协程不要在内部再次调度自己    size_t capacity() const;

};

### 错误处理```

1. Task 自动传播异常

2. 使用标准 try-catch## 错误处理

3. 检查 I/O 操作返回值

### Result<T, E>

### 性能优化

1. 批量处理 I/O 操作不使用异常的类型安全错误处理。

2. 避免在协程中调用阻塞 API

3. ThreadPool 线程数不要过多```cpp

template<typename T, typename E = Error>

---class Result {

public:

## 更多示例    bool is_ok() const;

    bool is_err() const;

查看 [examples/](../examples/) 目录获取完整示例：    

- `01_basic_task.cpp` - Task 基础    T& unwrap();

- `02_generator_example.cpp` - Generator 使用    E& error();

- `03_scheduler_example.cpp` - 调度器使用    

- `04_async_io_example.cpp` - 异步 I/O    T value_or(T default_value) const;

};

## 参考资料```



- [架构设计](ARCHITECTURE.md)#### 示例

- [项目进度](../PROGRESS.md)

- [C++20 协程参考](https://en.cppreference.com/w/cpp/language/coroutines)```cpp

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
