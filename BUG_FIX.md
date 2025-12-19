# ZLCoro Bug 修复记录

## 1. Awaiter 悬空指针

**问题**: Awaiter 是临时对象，将成员变量地址存入等待队列后，协程挂起时 Awaiter 已销毁。

**方案**: 使用 `shared_ptr` 管理等待队列中的数据。

---

## 2. await_ready/await_suspend 竞态

**问题**: `await_ready()` 返回 `false` 后、`await_suspend()` 调用前，锁可能被其他线程释放，导致死锁。

**方案**: 获取逻辑全部移到 `await_suspend()`，返回 `bool` 决定是否挂起。

---

## 3. Lambda 协程生命周期

**问题**: 临时 lambda 创建的协程帧会引用已销毁的捕获变量。

**方案**: Lambda 协程必须赋值给命名变量，或改用独立函数 + `shared_ptr` 传参。

```cpp
// 错误
async_run([&]() -> Task<void> { co_await wg.wait(); }());

// 正确
auto waiter = [&]() -> Task<void> { co_await wg.wait(); co_return; };
async_run(waiter());
```

---

## 4. async_run 重复 resume

**问题**: 循环调用 `resume()` 会与同步原语的唤醒冲突，导致同一协程被多次 resume。

**方案**: 只调用一次 `resume()` 启动协程，后续由 Scheduler 管理。

---

## 5. 协程递归导致帧堆叠

**问题**: `co_return co_await func()` 递归调用会堆叠协程帧。

**方案**: 使用 `while(true)` 循环代替递归。

---

## 6. 边缘触发模式事件丢失

**问题**: "先等待后尝试"模式下，数据在注册 epoll 前到达，边缘触发不会再次通知。

**方案**: 改用"先尝试后等待"，只有 EAGAIN 才等待事件。

```cpp
// 错误
co_await ReadAwaiter{...};
ssize_t n = ::read(...);

// 正确
ssize_t n = ::read(...);
if (n == -1 && errno == EAGAIN) {
    co_await ReadAwaiter{...};
}
```

---

## 核心原则

1. **Awaiter 数据用 shared_ptr** - 临时对象不能存地址
2. **获取逻辑在 await_suspend** - 避免竞态窗口
3. **Lambda 协程要命名** - 确保生命周期
4. **协程只 resume 一次** - 后续由调度器管理
5. **循环代替递归** - 避免帧堆叠
6. **先尝试后等待** - 边缘触发模式必须