# 贡献指南

欢迎为 ZLCoro 项目做出贡献！本文档提供了参与项目的指南。

## 开发环境

### 前置要求

- GCC 10+ 或 Clang 12+
- CMake 3.20+
- Git
- Google Test（用于运行测试）

### 从源码构建

```bash
git clone https://github.com/MyCodeHo/ZLCoro.git
cd ZLCoro
mkdir build && cd build
cmake ..
make -j$(nproc)
```

### 运行测试

```bash
ctest --output-on-failure
```

## 开发流程

### 1. Fork 和克隆

Fork 本仓库并克隆你的 fork：

```bash
git clone https://github.com/yourusername/ZLCoro.git
cd ZLCoro
git remote add upstream https://github.com/MyCodeHo/ZLCoro.git
```

### 2. 创建分支

为你的修改创建一个功能分支：

```bash
git checkout -b feature/your-feature-name
```

### 3. 进行修改

- 遵循现有代码风格
- 为新功能添加测试
- 根据需要更新文档
- 确保所有测试通过

### 4. 提交

编写清晰、简洁的提交信息：

```bash
git add .
git commit -m "feat: 简要描述"
```

**提交信息格式：**
```
<类型>: <主题>

<正文>

<页脚>
```

类型：
- `feat`: 新功能
- `fix`: Bug 修复
- `docs`: 文档变更
- `style`: 代码格式变更（不影响功能）
- `refactor`: 代码重构
- `test`: 添加或更新测试
- `perf`: 性能优化

### 5. 推送并创建 Pull Request

```bash
git push origin feature/your-feature-name
```

然后在 GitHub 上创建 pull request。

## 代码规范

### C++ 指南

- 遵循 [C++ Core Guidelines](https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines)
- 适当使用现代 C++20 特性
- 使用 RAII 进行资源管理
- 当所有权不明确时使用智能指针

### 代码格式化

使用项目的 `.clang-format` 文件进行格式化：

```bash
clang-format -i <源文件>
```

### 命名约定

- **类/结构体**：`PascalCase`
- **函数/方法**：`snake_case`
- **变量**：`snake_case`
- **常量**：`UPPER_SNAKE_CASE`
- **私有成员**：`snake_case_`（带下划线后缀）

### 示例

```cpp
class TaskScheduler {
public:
    void schedule_task(std::coroutine_handle<> handle);
    size_t pending_count() const;

private:
    std::queue<std::coroutine_handle<>> task_queue_;
    std::mutex mutex_;
};
```

## 测试

### 编写测试

- 将测试放在 `tests/` 目录
- 使用 Google Test 框架
- 追求高代码覆盖率
- 测试边界情况和错误条件

### 示例测试

```cpp
#include <gtest/gtest.h>
#include "zlcoro/core/task.h"

TEST(TaskTest, BasicExecution) {
    auto task = []() -> Task<int> {
        co_return 42;
    }();
    
    task.resume();
    EXPECT_TRUE(task.done());
}
```

### 运行特定测试

```bash
./build/tests/core_test/task_test
```

## 文档

### 代码文档

- 为公共 API 使用 Doxygen 风格注释
- 解释复杂算法
- 包含使用示例

### 示例

```cpp
/**
 * @brief 从 socket 异步读取数据
 * 
 * @param buffer 目标缓冲区
 * @param size 最大读取字节数
 * @return Task<size_t> 读取的字节数
 * 
 * @throws IOError socket 错误时抛出
 * 
 * 示例：
 * @code
 * char buf[1024];
 * size_t n = co_await socket.read(buf, sizeof(buf));
 * @endcode
 */
Task<size_t> read(char* buffer, size_t size);
```

### 更新文档

- 更新 `docs/` 目录中的相关 `.md` 文件
- 保持 README.md 更新
- 在 `examples/` 目录中添加示例

## Pull Request 流程

1. **确保 CI 通过**：所有测试和检查必须通过
2. **更新 CHANGELOG**：为你的更改添加条目
3. **请求审查**：标记合适的审查者
4. **处理反馈**：及时进行请求的修改
5. **压缩提交**：合并前清理提交历史

## 性能考虑

- 在优化前先进行性能分析
- 为性能关键代码添加基准测试
- 记录性能特征
- 考虑缓存局部性和内存访问模式

## Issue 报告

### Bug 报告

包含：
- 操作系统和编译器版本
- 复现步骤
- 期望行为 vs 实际行为
- 最小可复现示例
- 堆栈跟踪（如果适用）

### 功能请求

包含：
- 用例描述
- 建议的 API（如果适用）
- 实现思路
- 考虑的替代方案

## 代码审查

### 作为审查者

- 保持建设性和尊重
- 关注代码质量和正确性
- 提出改进建议，而非强制要求
- 准备好时批准

### 作为作者

- 回复所有评论
- 需要时寻求澄清
- 不要把批评当成针对个人
- 根据反馈更新 PR

## 开源协议

通过为 ZLCoro 做出贡献，你同意你的贡献将采用 MIT 协议授权。

## 问题？

- 为问题开启 issue
- 在现有 issue 中参与讨论
- 首先查看文档

感谢你为 ZLCoro 做出贡献！
