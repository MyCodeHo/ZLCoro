# ZLCoro Bug 修复记录

## 核心 Bug 汇总

| # | 问题 | 修复方案 |
|---|------|---------|
| 1 | Awaiter 悬空指针 | 使用 `shared_ptr` 管理等待队列数据 |
| 2 | await_ready/await_suspend 竞态 | 获取逻辑移到 `await_suspend()`，返回 bool |
| 3 | Lambda 协程生命周期 | Lambda 协程必须赋值给命名变量 |
| 4 | async_run 重复 resume | 只 resume 一次，后续由 Scheduler 管理 |
| 5 | 协程递归帧堆叠 | 用 `while(true)` 循环代替递归 |
| 6 | 边缘触发事件丢失 | 改用"先尝试后等待"模式 |
| 7 | HTTP 头大小写敏感 | 实现 `CaseInsensitiveCompare` 比较器 |
| 8 | Content-Length 解析崩溃 | 添加 try-catch，无效值返回 0 |
| 9 | HTTP 版本未验证 | 添加 HTTP 版本和方法验证 |
| 10 | 请求体大小无限制 | 添加 `MAX_BODY_SIZE` (10MB) 限制 |
| 11 | TcpConnection 移动不完整 | 重置源对象状态，移动赋值时先关闭连接 |
| 12 | 同步原语资源泄漏 | 循环查找有效等待者，正确释放资源 |
| 13 | catch 块中 co_await | 用 `exception_ptr` 保存异常，在 catch 外处理 |

---

## 详细说明

### 1-6: 协程基础问题

```cpp
// ❌ 错误：临时 lambda
async_run([&]() -> Task<void> { co_await wg.wait(); }());

// ✅ 正确：命名变量
auto waiter = [&]() -> Task<void> { co_await wg.wait(); co_return; };
async_run(waiter());
```

```cpp
// ❌ 错误：先等待后尝试（边缘触发会丢事件）
co_await ReadAwaiter{...};
ssize_t n = ::read(...);

// ✅ 正确：先尝试后等待
ssize_t n = ::read(...);
if (n == -1 && errno == EAGAIN) {
    co_await ReadAwaiter{...};
}
```

### 7-10: HTTP 安全问题 (v0.9.0)

- **头部大小写**: RFC 7230 要求不敏感，用 `CaseInsensitiveCompare`
- **输入验证**: Content-Length 异常捕获、版本/方法验证、body 大小限制

### 11-12: 资源管理 (v0.9.0)

- **移动语义**: 重置源对象的 `read_pos_`/`read_end_`，移动赋值先 `close_sync()`
- **同步原语**: `unlock()`/`release()` 循环跳过无效句柄，确保资源释放

### 13: 协程语法限制 (v0.9.0)

```cpp
// ❌ 错误：catch 块中不能 co_await
catch (const std::exception& e) {
    resp = co_await error_handler_(req, e);  // 编译错误！
}

// ✅ 正确：保存异常，在 catch 外处理
catch (...) {
    handler_exception = std::current_exception();
}
if (handler_exception) {
    try { std::rethrow_exception(handler_exception); }
    catch (const std::exception& e) {
        resp = co_await error_handler_(req, e);  // OK
    }
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
7. **HTTP 头部大小写不敏感** - 遵循 RFC 7230
8. **输入验证** - 防止恶意请求
9. **移动语义要完整** - 重置源对象状态
10. **同步原语要健壮** - 处理无效句柄
11. **catch 块不能 co_await** - 用 exception_ptr 延迟处理
