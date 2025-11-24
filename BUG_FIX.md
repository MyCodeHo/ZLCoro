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
