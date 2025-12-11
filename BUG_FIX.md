# ZLCoro Bug 修复记录

## Bug #1: Generator 右值生命周期问题

**问题**: `co_yield` 右值时只存储地址，临时对象销毁后导致悬空指针。

**修复方案**: 区分左值和右值，右值移动到 Promise 存储。

---

## Bug #2: 右值到左值切换时的资源泄漏

**问题**: 先 yield 右值后 yield 左值时，`stored_value_` 中的对象未析构。

**修复方案**: 在 yield 左值时检查并清理之前的右值。

---

## Bug #3: 协程生命周期管理问题

**问题**: Task 移动后，原始句柄仍被 `async_run` 使用，导致悬空指针。

**修复方案**: 使用 `shared_ptr` 延长 Task 生命周期。

---

## Bug #4: IOTest heap-use-after-free

**问题**: AsyncFile 方法返回 Task 并在内部使用 `co_await schedule()`，导致嵌套协程生命周期问题。

**修复方案**: 将 AsyncFile 方法改为同步函数，由调用者统一管理协程调度。

---

## Bug #5: SchedulerTest stack-use-after-scope

**问题**: Lambda 协程在循环中创建，lambda 对象在栈上，生命周期结束后协程仍在执行。

**修复方案**: 使用独立的协程函数，避免循环中使用 lambda。

---

## Bug #6: EventLoop 定时器排序错误

**问题**: 定时器按 TimerId 排序，但 `process_timers()` 错误地假设第一个元素是最早到期的。

**修复方案**: 使用 `std::multimap<time_point, ...>` 按到期时间排序。

---

## Bug #7: AsyncSocket 递归协程导致栈帧堆叠

**问题**: `accept()` 遇到 `EAGAIN` 时使用 `co_return co_await accept()` 递归调用，导致协程帧堆叠。

**修复方案**: 使用 `while(true)` 循环代替递归，复用同一协程帧。

---

## Bug #8: EpollPoller 重复添加协程

**问题**: 文件描述符同时触发多个事件时，同一个协程被添加两次导致崩溃。

**修复方案**: 使用标志位确保每个 fd 只添加一次协程。

---

## Bug #9: Generator 迭代器安全性问题

**问题**: Generator 迭代器缺少边界检查，可能导致访问无效句柄。

**修复方案**: 在 `operator++` 和 `operator*` 中添加有效性检查。

---

## 核心设计原则

1. **协程不应在内部重新调度** - 避免嵌套协程和生命周期问题
2. **避免循环中使用 lambda** - 使用独立函数确保生命周期明确
3. **使用 shared_ptr 管理异步对象** - 确保异步执行时对象有效
4. **数据结构要匹配使用场景** - 排序依据要和查找逻辑一致
5. **使用循环而非递归** - 避免协程帧堆叠和栈溢出
6. **添加边界检查** - 防御性编程