# Bug 修复记录

## Generator<T> 生命周期问题

**日期**: 2025年11月18日  
**文件**: `include/zlcoro/core/generator.hpp`  
**严重性**: 🔴 严重  
**状态**: ✅ 已修复

---

## 问题描述

原实现中，`yield_value()` 只存储了参数的地址。当 `co_yield` 右值（临时对象）时，临时对象在 `yield_value()` 返回后被销毁，导致悬空指针。

### 有问题的代码

```cpp
std::suspend_always yield_value(T&& value) noexcept {
    value_ptr_ = std::addressof(value);  // ⚠️ 临时对象会被销毁
    return {};
}
```

### 触发场景

```cpp
Generator<int> example() {
    co_yield 42;  // 42 是临时对象，会被销毁
}

Generator<std::string> example2() {
    co_yield std::string("hello");  // 临时 string 会被销毁
}
```

---

## 修复方案

**核心思路**: 区分左值和右值，对右值进行移动存储。

```cpp
class promise_type {
public:
    // 左值：直接存储指针（协程帧中的变量是安全的）
    std::suspend_always yield_value(const T& value) 
        noexcept(std::is_nothrow_copy_constructible_v<T>) 
        requires std::copy_constructible<T>
    {
        value_ptr_ = std::addressof(value);
        return {};
    }
    
    // 右值：移动到 Promise 的存储中
    std::suspend_always yield_value(T&& value)
        noexcept(std::is_nothrow_move_constructible_v<T>)
        requires std::move_constructible<T>
    {
        // 销毁旧值（如果存在）
        if (value_ptr_ == std::addressof(stored_value_)) {
            std::destroy_at(std::addressof(stored_value_));
        }
        
        // 移动构造新值
        std::construct_at(std::addressof(stored_value_), std::move(value));
        value_ptr_ = std::addressof(stored_value_);
        return {};
    }
    
    ~promise_type() {
        if (value_ptr_ == std::addressof(stored_value_)) {
            std::destroy_at(std::addressof(stored_value_));
        }
    }
    
private:
    union {
        T stored_value_;  // 存储右值的副本
    };
    const T* value_ptr_;
    std::exception_ptr exception_;
};
```

---

## 验证

- ✅ 所有单元测试通过 (31/31)
- ✅ AddressSanitizer 验证通过
- ✅ UBSanitizer 验证通过

---

## 技术要点

1. **区分左值和右值**: 使用函数重载，根据值类别采用不同策略
2. **延迟初始化**: 使用 union 避免不必要的默认构造
3. **手动生命周期管理**: 使用 `std::construct_at` 和 `std::destroy_at`
4. **noexcept 规范**: 根据 T 的特性条件性 noexcept
