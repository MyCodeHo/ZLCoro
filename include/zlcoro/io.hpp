#pragma once

// 异步 I/O 模块

// Epoll 后端
#include "io/epoll_poller.hpp"
#include "io/event_loop.hpp"
#include "io/async_file.hpp"
#include "io/async_socket.hpp"

// io_uring 后端 (Linux 5.1+)
#include "io/io_uring.hpp"
#include "io/io_uring_poller.hpp"
#include "io/io_uring_file.hpp"
#include "io/io_uring_socket.hpp"
#include "io/wal_writer.hpp"

// 工具
#include "utils/aligned_buffer.hpp"
