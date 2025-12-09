# Bug 修复记录

## Bug #1: Generator 右值生命周期问题 ✅

**状态**: 已修复  
**修复日期**: 2025-11-08

**问题**: `co_yield` 右值时只存储地址，临时对象销毁后导致悬空指针。

**触发场景**:
```cpp
Generator<int> bug() {
    co_yield 42;  // 临时对象被销毁
}
```

**修复方案**: 区分左值和右值，右值移动到 Promise 存储。

```cpp
// 右值：移动到 stored_value_
std::suspend_always yield_value(T&& value) {
    std::construct_at(std::addressof(stored_value_), std::move(value));
    value_ptr_ = std::addressof(stored_value_);
    return {};
}
```

**测试验证**: GeneratorTest - 16/16 tests passing

---

## Bug #2: 右值到左值切换时的资源泄漏 ✅

**状态**: 已修复  
**修复日期**: 2025-11-12

**问题**: 先 yield 右值后 yield 左值时，`stored_value_` 中的对象未析构。

**触发场景**:
```cpp
Generator<std::string> leak() {
    co_yield std::string("temp");  // 右值 → stored_value_
    std::string var = "variable";
    co_yield var;                   // 左值 → "temp" 未析构
}
```

**修复方案**: 在 yield 左值时检查并清理之前的右值。

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

**测试验证**: GeneratorTest - 16/16 tests passing

---

## Bug #3: Task 缺少 result() 方法 ✅

**状态**: 已修复  
**修复日期**: 2025-11-17

**问题**: `async_run` 实现中需要调用 `task.result()` 获取协程结果，但 Task 类未提供该方法。

**触发场景**:
```cpp
Task<int> compute() { co_return 42; }
auto future = async_run(compute());  // 编译错误：Task 无 result() 方法
```

**修复方案**: 在 `Task<T>` 类中添加 `result()` 方法。

```cpp
// task.hpp
T& result() & {
    return promise_->result_;
}

T&& result() && {
    return std::move(promise_->result_);
}
```

**测试验证**: TaskTest - 15/15 tests passing

---

## Bug #4: 协程生命周期管理问题 ✅

**状态**: 已修复  
**修复日期**: 2025-11-17

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

**修复方案**: 使用 `shared_ptr` 延长 Task 生命周期，确保句柄有效。

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

**测试验证**: SchedulerTest - 13/13 tests passing

---

## Bug #5: co_await schedule() 的多线程竞争 ✅

**状态**: 已修复  
**修复日期**: 2025-11-20

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

**修复方案**: 在涉及 `schedule()` 的场景使用 `async_run` 替代 `sync_wait()`。

```cpp
// ✅ 正确方式
auto future = async_run(with_schedule());
int result = future.get();
```

**测试验证**: SchedulerTest - 13/13 tests passing

---

## Bug #6: IOTest heap-use-after-free ✅

**状态**: 已修复  
**修复日期**: 2025-12-24

**问题**: AsyncFile 方法返回 Task<T> 并在内部使用 `co_await schedule()`，导致协程在线程池执行时父协程已经销毁。

**触发场景**:
```cpp
Task<std::string> AsyncFile::read_all() {
    co_await schedule();  // 切换到线程池
    // ... 此时父协程可能已销毁
    co_return content;
}

// 使用方式
auto future = async_run(file.read_all());  // ❌ 段错误
```

**根本原因**: 
- AsyncFile 的方法内部调用 `co_await schedule()` 会创建嵌套协程
- 外层协程（async_run 创建）销毁后，内层协程仍在线程池中执行
- 访问已销毁的协程句柄导致 heap-use-after-free

**修复方案**: 
1. 将 AsyncFile 的所有方法从 Task<T> 改为同步函数
2. 移除内部的 `co_await schedule()`
3. 让调用者通过 async_run() 统一管理协程调度

```cpp
// 修改前
Task<std::string> read_all() {
    co_await schedule();
    // ...
    co_return content;
}

// 修改后
std::string read_all() {
    // ... 同步执行
    return content;
}

// 使用方式
auto future = async_run([&]() -> Task<std::string> {
    co_return file.read_all();  // ✅ 安全
}());
```

**设计原则**: 
- **协程不应在内部重新调度自己**
- 调度职责应由调用者（async_run）统一管理
- 避免嵌套协程导致的生命周期问题

**测试验证**: IOTest - 7/7 tests passing

---

## Bug #7: SchedulerTest stack-use-after-scope ✅

**状态**: 已修复  
**修复日期**: 2025-12-02

**问题**: Lambda 协程在 for 循环中创建，lambda 对象在栈上，生命周期结束后协程仍在执行。

**触发场景**:
```cpp
TEST(ConcurrentTest, MultipleTasks) {
    auto counter = std::make_shared<std::atomic<int>>(0);
    
    for (int i = 0; i < 100; ++i) {
        async_run([counter]() -> Task<void> {  // ❌ lambda 在栈上
            counter->fetch_add(1);
            co_return;
        }());
    }
    // for 循环结束，lambda 对象销毁
    // 但协程可能仍在线程池中执行 → stack-use-after-scope
}
```

**根本原因**:
- Lambda 表达式在 for 循环的栈上创建
- 即使捕获的是 shared_ptr，lambda 对象本身也有生命周期问题
- 协程在线程池中执行时，lambda 对象可能已被销毁

**修复方案**: 使用独立的协程函数，而非 lambda

```cpp
// 添加独立协程函数
Task<void> increment_counter(std::shared_ptr<std::atomic<int>> counter) {
    counter->fetch_add(1);
    co_return;
}

// 测试代码
TEST(ConcurrentTest, MultipleTasks) {
    auto counter = std::make_shared<std::atomic<int>>(0);
    
    for (int i = 0; i < 100; ++i) {
        async_run(increment_counter(counter));  // ✅ 安全
    }
}
```

**设计原则**:
- **避免在协程中使用 lambda**，尤其是在循环中
- 使用独立的协程函数，确保函数对象有明确的生命周期
- Lambda 的生命周期难以控制，容易导致悬空引用

**测试验证**: SchedulerTest - 13/13 tests passing

---

## Bug #8: EventLoop 定时器排序错误 ✅

**状态**: 已修复  
**修复日期**: 2025-12-09

**问题**: 定时器使用 `std::map<TimerId, Timer>` 按 TimerId 排序，但 `process_timers()` 错误地假设第一个元素是最早到期的。

**触发场景**:
```cpp
loop.add_timer(1000, callback1);  // TimerId=0, expire in 1000ms
loop.add_timer(100, callback2);   // TimerId=1, expire in 100ms

// map 按 TimerId 排序：
// [0] -> 1000ms  <- begin()
// [1] -> 100ms

// process_timers() 使用 begin() 计算超时 → 1000ms
// callback2 应该 100ms 执行，却被延迟到 1000ms！
```

**修复方案**: 使用 `std::multimap<time_point, pair<TimerId, Callback>>` 按到期时间排序。

```cpp
// 修改前
std::map<TimerId, Timer> timers_;  // 按 TimerId 排序

// 修改后
std::multimap<std::chrono::steady_clock::time_point,
              std::pair<TimerId, TimerCallback>> timers_;  // 按到期时间排序

// 现在 begin() 就是最早到期的定时器
auto next_expire = timers_.begin()->first;
```

**测试验证**: 51/51 tests passing

---

## 总结

### 已修复的 Bug
1. ✅ Generator 右值生命周期问题
2. ✅ 右值到左值切换时的资源泄漏
3. ✅ Task 缺少 result() 方法
4. ✅ 协程生命周期管理问题
5. ✅ co_await schedule() 的多线程竞争
6. ✅ IOTest heap-use-after-free
7. ✅ SchedulerTest stack-use-after-scope
8. ✅ EventLoop 定时器排序错误

### 核心设计原则
1. **协程不应在内部重新调度** - 避免嵌套协程和生命周期问题
2. **避免协程中使用 lambda** - 使用独立函数确保生命周期明确
3. **使用 shared_ptr 管理生命周期** - 确保异步执行时对象有效
4. **调用者负责调度** - async_run() 统一管理协程调度
5. **数据结构要匹配使用场景** - 排序依据要和查找逻辑一致

### 测试状态
- **总计**: 51/51 tests passing (100%)
- **TaskTest**: 15/15 passing
- **GeneratorTest**: 16/16 passing
- **SchedulerTest**: 13/13 passing
- **IOTest**: 7/7 passing

---

**最后更新**: 2025-12-09
