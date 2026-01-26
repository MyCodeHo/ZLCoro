#pragma once

// =============================================================================
// ZLCoro Per-Core Runtime
// =============================================================================
//
// 每核心事件循环架构，为高性能 KV 存储系统设计
//
// 特点：
// - 每个 CPU 核心运行一个独立的事件循环
// - 完全无锁设计，核心间无竞争
// - 每个网络连接由一个协程服务
// - 支持 epoll 和 io_uring 两种后端
//
// 架构图：
//
//   ┌─────────────────────────────────────────────────────────────────┐
//   │                        TcpServer                                │
//   │                   (Accept Dispatcher)                           │
//   └──────────────────────────┬──────────────────────────────────────┘
//                              │ 连接分发 (hash by fd)
//            ┌─────────────────┼─────────────────┐
//            ▼                 ▼                 ▼
//   ┌────────────────┐ ┌────────────────┐ ┌────────────────┐
//   │   Core 0       │ │   Core 1       │ │   Core N       │
//   │ ┌────────────┐ │ │ ┌────────────┐ │ │ ┌────────────┐ │
//   │ │ EventLoop  │ │ │ │ EventLoop  │ │ │ │ EventLoop  │ │
//   │ │ (epoll/    │ │ │ │ (epoll/    │ │ │ │ (epoll/    │ │
//   │ │  io_uring) │ │ │ │  io_uring) │ │ │ │  io_uring) │ │
//   │ └────────────┘ │ │ └────────────┘ │ │ └────────────┘ │
//   │                │ │                │ │                │
//   │ ┌────────────┐ │ │ ┌────────────┐ │ │ ┌────────────┐ │
//   │ │ Coroutines │ │ │ │ Coroutines │ │ │ │ Coroutines │ │
//   │ │ (per-conn) │ │ │ │ (per-conn) │ │ │ │ (per-conn) │ │
//   │ └────────────┘ │ │ └────────────┘ │ │ └────────────┘ │
//   └────────────────┘ └────────────────┘ └────────────────┘
//        独立运行           独立运行           独立运行
//        无共享状态         无共享状态         无共享状态
//
// 使用示例：
//
//   // 创建 io_uring 运行时
//   auto runtime = zlcoro::make_io_uring_runtime(4);  // 4 核
//   
//   // 创建 TCP 服务器
//   zlcoro::TcpServer server(*runtime);
//   server.listen(8080);
//   
//   // 设置连接处理器（每个连接一个协程）
//   server.set_handler([](zlcoro::TcpConnection& conn) -> zlcoro::Task<void> {
//       char buf[1024];
//       while (true) {
//           ssize_t n = co_await conn.read(buf, sizeof(buf));
//           if (n <= 0) break;
//           co_await conn.write(buf, n);  // echo
//       }
//   });
//   
//   // 启动服务器
//   runtime->start();
//   server.start_accept_all_cores();
//   runtime->wait();
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

// 核心组件
#include "per_core_event_loop.hpp"   // 事件循环基类
#include "epoll_per_core.hpp"        // epoll 后端
#include "per_core_runtime.hpp"      // 运行时管理器
#include "tcp_server.hpp"            // TCP 服务器/客户端
#include "connection_manager.hpp"    // 连接管理器（epoll/io_uring 协程模型）
#include "kv_server.hpp"             // KV 存储专用服务器
