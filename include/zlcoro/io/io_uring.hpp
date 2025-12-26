#pragma once

// =============================================================================
// io_uring.hpp - io_uring 异步 I/O 统一头文件
// =============================================================================
//
// io_uring 是 Linux 5.1+ 引入的高性能异步 I/O 接口。
// 相比 epoll + 线程池方案，io_uring 具有以下优势:
// 
// 1. 真正的异步: 内核直接执行 I/O，无需线程池模拟
// 2. 零拷贝: 使用共享内存的 SQ/CQ 环形缓冲区
// 3. 批量提交: 多个请求可以一次系统调用提交
// 4. 统一接口: 支持文件、网络、定时器等多种操作
//
// 使用示例:
//
//   #include "zlcoro/io/io_uring.hpp"
//
//   #ifdef ZLCORO_HAS_IO_URING
//
//   IoUringEventLoop loop;
//   
//   // 文件操作
//   IoUringFile file(&loop.poller(), "test.txt", IoUringFile::ReadOnly);
//   auto content = co_await file.read_all();
//   
//   // Socket 操作
//   IoUringSocket server(&loop.poller());
//   server.create();
//   server.bind("0.0.0.0", 8080);
//   server.listen();
//   auto client = co_await server.accept();
//   
//   #endif
//
// 编译要求:
// - Linux 5.1+ 内核
// - liburing 库 (apt install liburing-dev)
// - 链接时添加 -luring
//
// =============================================================================

#include "io_uring_poller.hpp"
#include "io_uring_file.hpp"
#include "io_uring_socket.hpp"

#ifdef ZLCORO_HAS_IO_URING

namespace zlcoro {

// 检查 io_uring 是否可用
inline bool io_uring_available() noexcept {
    return true;
}

// 获取 io_uring 版本信息（如果 liburing 支持）
inline const char* io_uring_version() noexcept {
    return "io_uring (Linux 5.1+)";
}

} // namespace zlcoro

#else

namespace zlcoro {

// io_uring 不可用
inline bool io_uring_available() noexcept {
    return false;
}

inline const char* io_uring_version() noexcept {
    return "io_uring not available (requires Linux 5.1+)";
}

} // namespace zlcoro

#endif // ZLCORO_HAS_IO_URING
