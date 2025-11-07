# 更新日志

本项目的所有重要变更都将记录在此文件中。

格式基于 [Keep a Changelog](https://keepachangelog.com/zh-CN/1.0.0/)，
版本号遵循 [语义化版本](https://semver.org/lang/zh-CN/)。

## [未发布]

### 新增
- 初始化项目结构
- 核心协程基础设施（Task<T>、Promise、Awaiter）
- 项目文档（架构设计、API 参考）
- CMake 构建系统
- 基础示例代码

### 进行中
- 工作窃取线程池调度器
- 基于 Epoll 的 I/O 调度器
- 异步 Socket 实现
- 同步原语实现

## [0.1.0] - 2025-11-07

### 新增
- 项目初始化
- 基础 Task<T> 实现
- 用于惰性序列的 Generator<T>
- 项目文档框架
- 构建系统配置
- 基于 Google Test 的测试基础设施

[未发布]: https://github.com/MyCodeHo/ZLCoro/compare/v0.1.0...HEAD
[0.1.0]: https://github.com/MyCodeHo/ZLCoro/releases/tag/v0.1.0
