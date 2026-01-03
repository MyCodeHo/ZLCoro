#pragma once

// =============================================================================
// ZLCoro - C++20 协程框架
// =============================================================================
// 
// 高性能异步编程框架，提供：
// - Task<T>: 异步任务
// - Generator<T>: 惰性生成器
// - Runtime: 统一运行时
// - 网络: TcpServer/TcpConnection
// - 同步原语: Mutex/RWLock/Channel/Semaphore/WaitGroup
// - 定时器: Timer/Interval
// 
// 快速开始:
//   #include <zlcoro/zlcoro.hpp>
//   
//   Task<void> hello() {
//       co_await Timer::sleep(std::chrono::seconds(1));
//       std::cout << "Hello, ZLCoro!\n";
//   }
//   
//   int main() {
//       Runtime runtime;
//       runtime.block_on(hello());
//       return 0;
//   }
// =============================================================================

// 核心
#include "zlcoro/core/task.hpp"
#include "zlcoro/core/generator.hpp"

// 运行时
#include "zlcoro/runtime/runtime.hpp"

// 调度器
#include "zlcoro/scheduler/scheduler.hpp"
#include "zlcoro/scheduler/work_stealing_scheduler.hpp"
#include "zlcoro/scheduler/async.hpp"

// 网络
#include "zlcoro/net/tcp.hpp"

// 同步原语
#include "zlcoro/sync/mutex.hpp"
#include "zlcoro/sync/rwlock.hpp"
#include "zlcoro/sync/channel.hpp"
#include "zlcoro/sync/semaphore.hpp"
#include "zlcoro/sync/wait_group.hpp"
#include "zlcoro/sync/cancellation.hpp"
#include "zlcoro/sync/timer.hpp"

// I/O
#include "zlcoro/io.hpp"

// 工具
#include "zlcoro/utils/memory_pool.hpp"

namespace zlcoro {

// 版本信息
constexpr int VERSION_MAJOR = 0;
constexpr int VERSION_MINOR = 8;
constexpr int VERSION_PATCH = 0;
constexpr const char* VERSION_STRING = "0.8.0";

} // namespace zlcoro
