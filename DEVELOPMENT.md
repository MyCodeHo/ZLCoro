# ZLCoro 开发指南

这是一个精简版的 C++20 协程框架项目，专注于核心开发。

## 📁 项目结构

```
ZLCoro/
├── README.md              # 项目简介和使用说明
├── LICENSE                # MIT 开源协议
├── CMakeLists.txt         # 构建配置（核心）
│
├── include/zlcoro/        # 头文件（公共 API）
│   ├── core/             # 协程核心类型
│   ├── scheduler/        # 调度器
│   ├── io/              # 异步 I/O
│   ├── sync/            # 同步原语
│   └── utils/           # 工具类
│
├── src/                  # 源文件（实现）
│   ├── core/
│   ├── scheduler/
│   ├── io/
│   ├── sync/
│   └── utils/
│
├── tests/                # 测试代码
│   ├── core_test/
│   ├── scheduler_test/
│   └── io_test/
│
├── examples/             # 示例代码
│   ├── basic/
│   ├── network/
│   └── benchmark/
│
├── benchmarks/           # 性能测试
│
└── docs/                 # 技术文档
    ├── ARCHITECTURE.md   # 架构设计
    ├── API.md           # API 参考
    └── BENCHMARKS.md    # 性能测试计划
```

## 🚀 快速开始

### 构建项目

```bash
# 创建构建目录
mkdir build && cd build

# 配置 CMake（Debug 版本）
cmake -DCMAKE_BUILD_TYPE=Debug ..

# 编译
make -j$(nproc)

# 运行测试
ctest --output-on-failure
```

### Release 版本

```bash
# Release 版本（O3 优化）
cmake -DCMAKE_BUILD_TYPE=Release ..
make -j$(nproc)
```

## 📝 开发工作流

### 1. 添加新功能

```bash
# 1. 在 include/zlcoro/ 中添加头文件
# 2. 在 src/ 中实现功能
# 3. 在 tests/ 中添加测试
# 4. 在 examples/ 中添加使用示例
```

### 2. 测试代码

```bash
cd build
ctest --output-on-failure
```

### 3. 运行示例

```bash
cd build
./examples/basic/example_name
```

### 4. 性能测试

```bash
cd build
./benchmarks/benchmark_name
```

## 🔧 CMake 选项

```bash
# 构建测试（默认 ON）
cmake -DZLCORO_BUILD_TESTS=OFF ..

# 构建示例（默认 ON）
cmake -DZLCORO_BUILD_EXAMPLES=OFF ..

# 构建基准测试（默认 ON）
cmake -DZLCORO_BUILD_BENCHMARKS=OFF ..

# 启用 AddressSanitizer（Debug 模式自动启用）
cmake -DCMAKE_BUILD_TYPE=Debug ..

# 禁用 Sanitizer
cmake -DCMAKE_CXX_FLAGS="-O0 -g" ..
```

## 📚 参考文档

- **docs/ARCHITECTURE.md** - 系统架构和设计思路
- **docs/API.md** - API 使用手册和示例
- **docs/BENCHMARKS.md** - 性能测试计划和结果

## 🐛 调试技巧

### 使用 GDB 调试

```bash
cd build
gdb ./tests/core_test/test_name

(gdb) break main
(gdb) run
(gdb) next
```

### 内存检测

```bash
# AddressSanitizer（自动启用在 Debug 模式）
cmake -DCMAKE_BUILD_TYPE=Debug ..
make
./tests/core_test/test_name

# Valgrind
valgrind --leak-check=full ./tests/core_test/test_name
```

### 查看编译命令

```bash
# 生成 compile_commands.json（用于 IDE）
cmake -DCMAKE_EXPORT_COMPILE_COMMANDS=ON ..
```

## 💡 开发建议

### 代码风格
- 使用 4 空格缩进
- 类名使用 PascalCase
- 函数和变量使用 snake_case
- 遵循 C++20 现代特性

### 协程开发注意事项
1. **避免协程内部重新调度** - 不要在协程方法内使用 `co_await schedule()`
2. **避免 lambda 协程** - 尤其在循环中，使用独立函数
3. **生命周期管理** - 使用 `shared_ptr` 管理异步对象
4. **调用者负责调度** - 让 `async_run()` 统一管理协程调度

### 提交代码
```bash
git add .
git commit -m "feat: 添加新功能描述"
git push origin main
```

### 常用命令

```bash
# 清理构建
rm -rf build/*

# 重新构建
cd build && cmake .. && make -j$(nproc)

# 只编译特定目标
make test_target_name

# 运行特定测试
ctest -R test_name -V

# 运行所有测试（推荐）
ctest --output-on-failure
```

## 🎯 开发路线图

### Phase 1-4: 已完成 ✅
- ✅ 核心协程类型（Task, Generator）
- ✅ 线程池调度器（ThreadPool + Scheduler）
- ✅ 异步 I/O（AsyncFile + AsyncSocket）
- ✅ Epoll 事件循环（EpollPoller + EventLoop）

### Phase 5: 同步原语（计划中）
- [ ] Channel - 协程间通信
- [ ] Mutex - 协程互斥锁
- [ ] WaitGroup - 协程等待组
- [ ] Semaphore - 信号量

### Phase 6: 性能优化（计划中）
- [ ] 工作窃取调度器
- [ ] 协程池和内存池
- [ ] io_uring 支持

### Phase 7: 高级特性（计划中）
- [ ] HTTP 客户端/服务器
- [ ] Echo 服务器示例
- [ ] DNS 解析器

### Phase 8: 生产就绪（计划中）
- [ ] 完整的性能基准测试
- [ ] 压力测试和稳定性验证
- [ ] 完善的文档和示例

## 📊 当前状态

**版本**: 0.4.0  
**测试状态**: 51/51 tests passing (100%)  
**代码质量**: AddressSanitizer clean  
**文档状态**: 核心文档完整

---

**最后更新**: 2025-12-02
