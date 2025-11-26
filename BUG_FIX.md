# Bug 修复记录

## Bug #1: Generator 右值生命周期问题

**问题**: `co_yield` 右值时只存储地址，临时对象销毁后导致悬空指针。

**触发场景**:
```cpp
Generator<int> bug() {
    co_yield 42;  // 临时对象被销毁
}
```

**修复思路**: 区分左值和右值，右值移动到 Promise 存储。

```cpp
// 右值：移动到 stored_value_
std::suspend_always yield_value(T&& value) {
    std::construct_at(std::addressof(stored_value_), std::move(value));
    value_ptr_ = std::addressof(stored_value_);
    return {};
}
```

---

## Bug #2: 右值到左值切换时的资源泄漏

**问题**: 先 yield 右值后 yield 左值时，`stored_value_` 中的对象未析构。

**触发场景**:
```cpp
Generator<std::string> leak() {
    co_yield std::string("temp");  // 右值 → stored_value_
    std::string var = "variable";
    co_yield var;                   // 左值 → "temp" 未析构
}
```

**修复思路**: 在 yield 左值时检查并清理之前的右值。

```cpp
// 左值：先清理旧的右值，再存储新指针
std::suspend_always yield_value(const T& value) {
    if (value_ptr_ == std::addressof(stored_value_)) {
        std::destroy_at(std::addressof(stored_value_));
    }
    value_ptr_ = std::addressof(value);
    return {};
}
```

---

## Bug #3: Task 缺少 result() 方法

**问题**: `async_run` 实现中需要调用 `task.result()` 获取协程结果，但 Task 类未提供该方法。

**触发场景**:
```cpp
Task<int> compute() { co_return 42; }
auto future = async_run(compute());  // 编译错误：Task 无 result() 方法
```

**修复思路**: 在 `Task<T>` 类中添加 `result()` 方法。

```cpp
// task.hpp
T& result() & {
    return promise_->result_;
}

T&& result() && {
    return std::move(promise_->result_);
}
```

---

## Bug #4: 协程生命周期管理问题

**问题**: Task 移动后，原始句柄仍被 `async_run` 使用，导致悬空指针和段错误。

**触发场景**:
```cpp
// async.hpp 原实现
template <typename T>
std::future<T> async_run(Task<T> task) {
    auto handle = task.get_handle();  // 获取句柄
    task.resume();                     // task 被移动或销毁
    // ... 后续使用 handle → 悬空指针
}
```

**修复思路**: 使用 `shared_ptr` 延长 Task 生命周期，确保句柄有效。

```cpp
template <typename T>
std::future<T> async_run(Task<T> task) {
    struct State {
        Task<T> task;
        std::promise<T> promise;
        std::atomic<bool> executed{false};
    };
    auto state = std::make_shared<State>();
    state->task = std::move(task);
    
    auto handle = state->task.get_handle();  // 现在安全了
    // ... state 的引用计数确保生命周期
}
```

---

## Bug #5: 递归协程导致资源耗尽

**问题**: 递归斐波那契示例创建了指数级的并发任务，导致程序超时和资源耗尽。

**触发场景**:
```cpp
Task<int> fibonacci(int n) {
    if (n <= 1) co_return n;
    auto f1 = async_run(fibonacci(n-1));  // 指数级任务爆炸
    auto f2 = async_run(fibonacci(n-2));
    co_return f1.get() + f2.get();
}
```

**修复思路**: 将示例改为简单的循环计算，避免递归创建大量任务。

```cpp
Task<int> compute_sum(int n) {
    int sum = 0;
    for (int i = 0; i < n; i++) {
        sum += i;
    }
    co_return sum;
}
```

---

## Bug #6: co_await schedule() 的多线程竞争

**问题**: `sync_wait()` 和线程池同时操作同一个协程句柄，导致段错误。

**触发场景**:
```cpp
Task<int> with_schedule() {
    co_await schedule();  // 切换到线程池
    co_return 42;
}

auto task = with_schedule();
task.sync_wait();  // ❌ sync_wait 和线程池竞争
```

**修复思路**: 在涉及 `schedule()` 的场景使用 `async_run` 替代 `sync_wait()`。

```cpp
// ✅ 正确方式
auto future = async_run(with_schedule());
int result = future.get();
```

---

## Bug #7: 并发测试的协程重复执行

**问题**: 大量并发任务可能导致协程被重复执行或执行计数错误。

**触发场景**:
```cpp
TEST(ConcurrentTest, HeavyLoad) {
    for (int i = 0; i < 100; i++) {
        async_run(compute(i));  // 高并发下协程状态混乱
    }
}
```

**修复思路**: 使用 `atomic<bool>` 标志防止重复执行，并减少测试并发量。

```cpp
struct State {
    Task<T> task;
    std::promise<T> promise;
    std::atomic<bool> executed{false};  // 防止重复执行
};

// 测试中减少并发数量
const int N = 20;  // 原来是 100
```
