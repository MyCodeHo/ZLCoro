#pragma once

// =============================================================================
// ZLCoro Per-Core Runtime
// =============================================================================
//
// 每核心事件循环架构，为高性能网络服务器设计（目标：百万级 QPS）
//
// 设计原则：
// - 每个 CPU 核心运行一个独立的事件循环
// - 每核独立 Accept（SO_REUSEPORT，内核负载均衡）
// - 完全无锁设计，核心间无竞争
// - 每个网络连接由一个协程服务
// - 支持 epoll 和 io_uring 两种后端
//
// 架构图（每核独立 Accept 模式）：
//
//   ┌───────────────────────────────────────────────────────────────────┐
//   │                    客户端连接请求                                   │
//   └───────────────────────────┬───────────────────────────────────────┘
//                               │
//                               ▼
//   ┌───────────────────────────────────────────────────────────────────┐
//   │                Linux 内核（SO_REUSEPORT 负载均衡）                  │
//   │           自动将连接分发到不同核心的 listen socket                   │
//   └───────────────────────────┬───────────────────────────────────────┘
//                               │
//          ┌────────────────────┼────────────────────┐
//          ▼                    ▼                    ▼
//   ┌──────────────┐    ┌──────────────┐    ┌──────────────┐
//   │   Core 0     │    │   Core 1     │    │   Core N     │
//   │ ┌──────────┐ │    │ ┌──────────┐ │    │ ┌──────────┐ │
//   │ │listen_fd │ │    │ │listen_fd │ │    │ │listen_fd │ │
//   │ │(独立)    │ │    │ │(独立)    │ │    │ │(独立)    │ │
//   │ └──────────┘ │    │ └──────────┘ │    │ └──────────┘ │
//   │ ┌──────────┐ │    │ ┌──────────┐ │    │ ┌──────────┐ │
//   │ │EventLoop │ │    │ │EventLoop │ │    │ │EventLoop │ │
//   │ │(epoll/   │ │    │ │(epoll/   │ │    │ │(epoll/   │ │
//   │ │io_uring) │ │    │ │io_uring) │ │    │ │io_uring) │ │
//   │ └──────────┘ │    │ └──────────┘ │    │ └──────────┘ │
//   │ ┌──────────┐ │    │ ┌──────────┐ │    │ ┌──────────┐ │
//   │ │Coroutines│ │    │ │Coroutines│ │    │ │Coroutines│ │
//   │ │(per-conn)│ │    │ │(per-conn)│ │    │ │(per-conn)│ │
//   │ └──────────┘ │    │ └──────────┘ │    │ └──────────┘ │
//   └──────────────┘    └──────────────┘    └──────────────┘
//        独立运行            独立运行            独立运行
//        无共享状态          无共享状态          无共享状态
//
// 关键优化：
// 1. SO_REUSEPORT - 每核独立 listen socket，内核自动负载均衡
// 2. 无惊群效应 - 内核直接分派连接，只唤醒目标核心
// 3. 缓存友好 - Accept 和处理在同一核心，无跨核调度
//
// 使用示例：
//
//   // 创建多核服务器（每核独立事件循环）
//   OptimizedEpollKVServer server(8);  // 8 核
//   server.listen(8080);
//   
//   // 设置连接处理器（每个连接一个协程）
//   server.set_handler([](auto& conn) -> Task<void> {
//       char buf[1024];
//       while (true) {
//           ssize_t n = co_await conn.read(buf, sizeof(buf));
//           if (n <= 0) break;
//           co_await conn.write(buf, n);  // echo
//       }
//   });
//   
//   // 启动（每核独立 accept + 处理）
//   server.start();
//   server.join();
//
// =============================================================================

// io_uring 后端（Linux 5.1+）- 需要先包含以定义 ZLCORO_HAS_IO_URING 宏
#ifdef __linux__
#include <linux/version.h>
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 1, 0)
#define ZLCORO_HAS_IO_URING 1
#include "io_uring_per_core.hpp"
#endif
#endif

// 核心组件（按层次组织）
// 
// 基础层：
#include "per_core_event_loop.hpp"   // 事件循环基类
#include "epoll_per_core.hpp"        // epoll 后端实现
#include "per_core_runtime.hpp"      // 多核运行时管理

// 连接管理层：
#include "connection_manager.hpp"    // 基础连接管理（教学用）
#include "optimized_connection.hpp"  // 优化连接管理（生产用，带对象池）

// 服务器层：
#include "tcp_server.hpp"            // 通用 TCP 服务器
#include "kv_server.hpp"             // KV 服务器（基础版）
#include "optimized_kv_server.hpp"   // KV 服务器（优化版，推荐使用）

// =============================================================================
// 组件选择指南
// =============================================================================
//
// 场景                          推荐组件
// ---------------------------   ------------------------------------------
// 学习协程 I/O 原理             connection_manager.hpp + kv_server.hpp
// 生产环境高性能服务器          optimized_connection.hpp + optimized_kv_server.hpp
// 自定义协议服务器              tcp_server.hpp（PerCoreConnection API）
// 简单 HTTP 服务器              参考 examples/benchmark/http_server_v2.cpp
// =============================================================================
