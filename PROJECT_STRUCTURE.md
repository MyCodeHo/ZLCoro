# ZLCoro 项目结构说明

本文档介绍 ZLCoro 项目的文件组织结构和各组件功能，帮助开发者快速了解项目架构。

## 📋 根目录文件

### 项目文档

#### README.md
- **功能**：项目主页文档
- **内容**：
  - 项目简介和核心特性
  - 快速开始指南
  - 系统架构图
  - 性能目标和使用示例
  - 贡献指南链接
- **说明**：新用户了解项目的首选文档

#### CONTRIBUTING.md
- **功能**：贡献者指南
- **内容**：
  - 开发环境搭建步骤
  - Git 工作流程（Fork → Branch → PR）
  - 代码风格规范（命名约定、格式化）
  - 测试要求和提交信息规范
- **说明**：规范化开发流程，方便社区贡献

#### CHANGELOG.md
- **功能**：版本更新日志
- **内容**：
  - 各版本的新增功能
  - Bug 修复记录
  - 破坏性变更说明
  - 遵循 [Keep a Changelog](https://keepachangelog.com/) 标准
- **说明**：记录项目演进历史

#### ROADMAP.md
- **功能**：项目发展路线图
- **内容**：
  - 项目愿景和当前状态
  - 详细的版本发布计划（v0.2.0 - v1.0.0）
  - 性能指标和质量目标
  - 未来增强方向（io_uring、NUMA 感知等）
  - 风险缓解策略
- **说明**：明确项目发展方向和技术演进路径

#### LICENSE
- **功能**：开源许可证
- **内容**：MIT 许可证全文
- **说明**：采用 MIT 许可，允许商业和个人自由使用

### 构建配置

#### CMakeLists.txt
- **功能**：CMake 构建系统配置文件
- **内容**：
  - 项目元信息（名称、版本、描述）
  - C++20 标准要求
  - 编译器选项（GCC/Clang 优化、警告、sanitizers）
  - 构建目标定义（库、测试、示例、基准测试）
  - 安装规则和导出配置
- **关键配置**：
  ```cmake
  # C++20 协程支持
  set(CMAKE_CXX_STANDARD 20)
  
  # Debug 模式开启 Address/Undefined Sanitizer
  add_compile_options(-fsanitize=address -fsanitize=undefined)
  
  # Release 模式 O3 优化
  add_compile_options(-O3 -DNDEBUG)
  ```

### 代码质量工具配置

#### .clang-format
- **功能**：代码格式化规则
- **配置**：
  - 基于 Google C++ Style Guide
  - 缩进宽度 4 空格
  - 每行最大 100 字符
  - 大括号、空格、对齐规则
- **使用**：`clang-format -i *.cpp *.h`

#### .editorconfig
- **功能**：跨编辑器代码风格配置
- **配置**：
  - UTF-8 字符编码
  - LF 换行符（Unix 风格）
  - C++ 文件 4 空格缩进
  - Markdown 2 空格缩进
  - YAML/JSON 2 空格缩进
- **支持**：VS Code, IntelliJ, Vim, Emacs 等主流编辑器

#### .gitignore
- **功能**：Git 版本控制忽略规则
- **配置**：
  - 构建目录（build/）
  - 编译产物（*.o, *.a, *.so）
  - IDE 配置（.vscode/, .idea/）
  - 测试覆盖率文件（*.gcov, coverage/）
  - 临时文件（*~, *.swp）

## 📁 docs/ 技术文档

### docs/ARCHITECTURE.md
- **功能**：系统架构设计文档
- **内容**：
  - 分层架构设计（应用层 → 同步原语 → 异步 I/O → 调度器 → 协程基础设施 → OS）
  - 核心组件详解：
    - Task<T> 和 Generator<T> 协程类型
    - 工作窃取调度器设计
    - Epoll I/O 调度器
    - 同步原语（Mutex, Channel, WaitGroup）
  - 内存管理策略（协程帧分配、对象池）
  - 性能优化技术（缓存优化、无锁算法、零拷贝 I/O）
  - 测试策略
- **用途**：帮助开发者理解系统设计思路和技术选型

### docs/API.md
- **功能**：API 参考手册
- **内容**：
  - 协程类型（Task, Generator）
  - 调度器（ThreadPoolScheduler, IOScheduler）
  - 异步 I/O（AsyncSocket, Buffer）
  - 同步原语（Mutex, Channel, WaitGroup, Semaphore）
  - 定时器（TimerWheel）
  - 错误处理（Result<T, E>）
  - 每个 API 都附带完整代码示例
- **用途**：API 使用参考和示例代码库

### docs/BENCHMARKS.md
- **功能**：性能基准测试报告
- **内容**：
  - 测试环境详细配置
  - 协程性能（创建 87ns，切换 45ns，比 pthread 快 33 倍）
  - 调度器性能（8 核 6.8M tasks/sec）
  - I/O 性能（10K 连接 520K QPS）
  - 内存使用（每协程 1.1KB）
  - 与业界框架对比（Go、Rust Tokio、libuv）
  - 测试方法论和可重现性说明
- **用途**：性能评估和对比参考

## 📂 include/ 头文件目录

### 目录结构
```
include/zlcoro/
├── core/          # 协程核心类型
├── scheduler/     # 调度器
├── io/           # 异步 I/O
├── sync/         # 同步原语
└── utils/        # 工具类
```

### include/zlcoro/core/
- **Task.h**：异步任务协程类型
- **Generator.h**：惰性生成器协程类型
- **Promise.h**：Promise 对象实现
- **Awaiter.h**：Awaiter 接口实现

### include/zlcoro/scheduler/
- **Scheduler.h**：调度器基类
- **ThreadPoolScheduler.h**：线程池调度器
- **IOScheduler.h**：I/O 调度器
- **WorkStealingQueue.h**：工作窃取队列

### include/zlcoro/io/
- **AsyncSocket.h**：异步 Socket
- **Buffer.h**：动态缓冲区
- **EventLoop.h**：事件循环
- **Epoll.h**：Epoll 封装

### include/zlcoro/sync/
- **Mutex.h**：协程互斥锁
- **Channel.h**：通道（消息队列）
- **WaitGroup.h**：协程等待组
- **Semaphore.h**：信号量

### include/zlcoro/utils/
- **ObjectPool.h**：对象池
- **TimerWheel.h**：时间轮定时器
- **Result.h**：Result<T, E> 错误处理

## 📂 src/ 源文件

### 目录结构（与 include/ 对应）
```
src/
├── core/          # 协程实现
├── scheduler/     # 调度器实现
├── io/           # I/O 实现
├── sync/         # 同步原语实现
└── utils/        # 工具类实现
```

**说明**：每个 .cpp 文件对应 include/ 中的 .h 文件，实现具体逻辑

## 📂 tests/ 测试代码

### 目录结构
```
tests/
├── core_test/          # 协程核心测试
├── scheduler_test/     # 调度器测试
├── io_test/           # I/O 测试
```

**功能**：
- 单元测试和集成测试
- 使用 Google Test 框架
- 覆盖协程、调度器、I/O 的各种场景
- 包含并发压力测试和异常测试

## 📂 examples/ 示例代码

### examples/basic/
- **功能**：基础使用示例
- **内容**：
  - Task 基本用法
  - Generator 使用
  - 简单的协程组合

### examples/network/
- **功能**：网络编程示例
- **内容**：
  - Echo 服务器
  - HTTP 服务器
  - 并发连接处理

### examples/benchmark/
- **功能**：性能测试示例
- **内容**：
  - 协程创建/销毁基准测试
  - 调度器性能测试
  - I/O 吞吐量测试

## 📂 benchmarks/ 基准测试

**功能**：
- 详细的性能基准测试代码
- 使用 Google Benchmark 框架
- 生成 docs/BENCHMARKS.md 的测试数据

**包含**：
- coroutine_bench.cpp：协程性能测试
- scheduler_bench.cpp：调度器性能测试
- io_bench.cpp：I/O 性能测试

## 📂 cmake/ CMake 模块

### cmake/ZLCoroConfig.cmake.in
- **功能**：CMake 包配置模板
- **用途**：让其他项目能通过 `find_package(ZLCoro)` 引入本库
- **生成**：安装时自动生成 ZLCoroConfig.cmake
- **内容**：
  - 导出的目标（ZLCoro::zlcoro）
  - 头文件路径
  - 库文件路径

## 📂 .github/ CI/CD 配置

### .github/workflows/ci.yml
- **功能**：GitHub Actions 持续集成配置
- **包含**：
  - 多编译器测试（GCC 10/11, Clang 12/14）
  - 多 OS 测试（Ubuntu 20.04/22.04）
  - 自动运行测试套件
  - 代码覆盖率报告
  - 性能基准测试
- **触发条件**：
  - Push 到 main 分支
  - Pull Request 提交
- **徽章**：README.md 中的 build status 徽章

## 📂 build/ 构建输出（Git 忽略）

**功能**：CMake 构建输出目录
**内容**：
- 编译生成的目标文件（.o）
- 静态/动态库（.a, .so）
- 可执行文件（测试、示例）
- CMake 缓存文件

**说明**：该目录在 .gitignore 中，不提交到版本控制

## 🎯 项目组织概览

### 文档体系

**核心文档**：
- README.md - 项目入口和快速上手
- ROADMAP.md - 发展规划和技术演进
- docs/ARCHITECTURE.md - 架构设计和技术细节
- docs/BENCHMARKS.md - 性能测试和对比数据
- CONTRIBUTING.md - 贡献指南和开发规范

**代码组织**：
- include/zlcoro/ - 公共头文件（API 定义）
- src/ - 实现代码（C++20 协程）
- tests/ - 测试代码（质量保证）
- examples/ - 示例代码（使用参考）

**工程配置**：
- CMakeLists.txt - 构建系统
- .clang-format - 代码格式化
- .github/workflows/ci.yml - 持续集成
- .editorconfig - 编辑器配置

### 文件统计
```
文档文件：8 个（README, CONTRIBUTING, CHANGELOG, ROADMAP, LICENSE, PROJECT_STRUCTURE + 3 技术文档）
配置文件：5 个（CMakeLists.txt, .clang-format, .editorconfig, .gitignore, ci.yml）
头文件：约 15-20 个（core, scheduler, io, sync, utils）
源文件：约 15-20 个（对应头文件）
测试文件：约 10-15 个
示例文件：约 5-10 个
```

## � 使用指南

### 新用户快速开始
1. 阅读 **README.md** 了解项目概况
2. 查看 **examples/** 目录的示例代码
3. 参考 **docs/API.md** 学习 API 使用

### 深入了解项目
1. 阅读 **docs/ARCHITECTURE.md** 理解系统设计
2. 查看 **ROADMAP.md** 了解发展方向
3. 研究 **docs/BENCHMARKS.md** 了解性能特性

### 贡献代码
1. 阅读 **CONTRIBUTING.md** 了解开发流程
2. 配置开发环境（clang-format, editorconfig）
3. 运行测试确保代码质量
4. 提交 Pull Request

## 🔧 技术特色

### 设计理念
- **高性能**：协程切换 45ns，比传统线程快 33 倍
- **现代 C++**：充分利用 C++20 协程特性
- **工程化**：完整的构建、测试、CI/CD 体系
- **可扩展**：模块化设计，易于扩展新功能

### 核心技术
- 工作窃取调度器实现高效负载均衡
- Epoll 事件循环处理海量并发连接
- 无锁算法减少线程竞争
- 对象池和零拷贝优化内存性能

### 质量保证
- Google Test 单元测试框架
- AddressSanitizer 内存检测
- 多编译器交叉测试
- 持续集成自动化测试

## 🚀 未来规划

参考 **ROADMAP.md** 了解详细的发展计划，主要包括：

- **v0.2.0**：完善工作窃取调度器
- **v0.3.0**：异步 I/O 子系统
- **v0.4.0**：同步原语套件
- **v0.5.0**：性能优化（io_uring、对象池）
- **v1.0.0**：生产就绪版本

长期目标包括 NUMA 感知调度、分布式协调、跨平台支持等。

---

**最后更新**：2025-11-07  
**项目版本**：0.1.0  
**维护状态**：积极开发中
