# ZLCoro

基于 C++20 协程的高性能异步编程框架

[![License](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
[![C++](https://img.shields.io/badge/C++-20-blue.svg)](https://en.cppreference.com/w/cpp/20)
[![Platform](https://img.shields.io/badge/platform-Linux-lightgrey.svg)](https://www.linux.org/)

## 项目简介

ZLCoro 是一个现代化的 C++ 协程框架，专为高并发服务器应用而设计。提供完整的异步运行时，包括高效调度器、异步 I/O 和同步原语。

## 核心特性

- **C++20 协程**：基于原生 C++20 协程支持构建
- **高性能**：工作窃取调度器，支持百万级并发协程
- **异步 I/O**：基于 Epoll 的事件循环，零拷贝优化
- **同步原语**：协程版 Mutex、Channel、WaitGroup、Semaphore
- **生产就绪**：完善的测试和性能基准

## 架构设计

```
┌─────────────────────────────────────┐
│         应用层                       │
├─────────────────────────────────────┤
│       同步原语层                     │
│  (Mutex/Channel/WaitGroup)          │
├─────────────────────────────────────┤
│       异步 I/O 层                    │
│  (Epoll/Socket/Buffer)              │
├─────────────────────────────────────┤
│        调度器层                      │
│  (ThreadPool/IOScheduler/Timer)     │
├─────────────────────────────────────┤
│      协程基础设施层                  │
│  (Task/Promise/Awaiter)             │
└─────────────────────────────────────┘
```

## 快速开始

### 环境要求

- GCC 10+ 或 Clang 12+
- CMake 3.20+
- Linux (推荐 Ubuntu 20.04+)

### 编译构建

```bash
mkdir build && cd build
cmake ..
make -j$(nproc)
```

### 运行测试

```bash
ctest --output-on-failure
```

### 示例代码

```cpp
#include "zlcoro/core/task.h"
#include <iostream>

using namespace zlcoro;

Task<int> compute() {
    co_return 42;
}

Task<void> main_task() {
    int result = co_await compute();
    std::cout << "Result: " << result << "\n";
}

int main() {
    auto task = main_task();
    task.resume();
    return 0;
}
```

## 项目结构

```
ZLCoro/
├── include/zlcoro/     # 公共头文件
│   ├── core/          # 协程原语
│   ├── scheduler/     # 调度器
│   ├── io/           # 异步 I/O
│   ├── sync/         # 同步原语
│   └── utils/        # 工具类
├── src/              # 实现代码
├── examples/         # 示例代码
├── tests/           # 单元测试
├── benchmarks/      # 性能基准测试
└── docs/            # 文档
```

## 文档

- [架构设计](docs/ARCHITECTURE.md)
- [API 参考](docs/API.md)
- [性能基准](docs/BENCHMARKS.md)

## 性能目标

- 协程创建：< 100ns
- 上下文切换：< 50ns
- 并发协程数：100 万+
- I/O 吞吐量：100 万+ QPS
- 内存开销：< 2KB/协程

## 开发路线

- [x] 核心协程基础设施
- [ ] 工作窃取调度器
- [ ] 基于 Epoll 的异步 I/O
- [ ] 同步原语
- [ ] 内存池优化
- [ ] 完善性能基准测试

## 参与贡献

欢迎贡献代码！请随时提交 issue 和 pull request。

## 开源协议

本项目采用 MIT 协议 - 详见 [LICENSE](LICENSE) 文件。

## 致谢

本项目受以下开源项目启发：
- [cppcoro](https://github.com/lewissbaker/cppcoro) - C++ 协程库
- [libco](https://github.com/Tencent/libco) - 腾讯协程库
- [Seastar](https://github.com/scylladb/seastar) - 高性能异步框架
