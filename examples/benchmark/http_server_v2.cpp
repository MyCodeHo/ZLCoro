/**
 * HTTP Echo 服务器 v2 - 用于 wrk 性能测试
 * 支持 epoll 和 io_uring 两种后端
 * 
 * io_uring 版本直接使用原生 API，避免中间层
 */

#include "zlcoro/runtime/epoll_per_core.hpp"
#include "zlcoro/runtime/io_uring_per_core.hpp"
#include "zlcoro/core/task.hpp"
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>
#include <atomic>
#include <csignal>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>

using namespace zlcoro;

std::atomic<bool> g_running{true};
std::atomic<uint64_t> g_total_connections{0};
std::atomic<uint64_t> g_total_requests{0};

void signal_handler(int) {
    g_running = false;
}

// HTTP 响应（Keep-Alive）
static const char HTTP_RESPONSE[] = 
    "HTTP/1.1 200 OK\r\n"
    "Content-Type: text/plain\r\n"
    "Content-Length: 13\r\n"
    "Connection: keep-alive\r\n"
    "\r\n"
    "Hello, World!";

static const size_t RESPONSE_LEN = sizeof(HTTP_RESPONSE) - 1;

// =============================================================================
// epoll 后端
// =============================================================================

Task<void> handle_epoll_connection(int fd, EpollPerCoreEventLoop& loop) {
    char buf[4096];
    
    // 设置非阻塞和 TCP_NODELAY
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
    int opt = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));
    
    while (g_running) {
        // 等待可读
        co_await EpollReadAwaiter{loop, fd};
        
        // 非阻塞读取
        ssize_t n = ::recv(fd, buf, sizeof(buf), MSG_DONTWAIT);
        if (n <= 0) {
            if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) {
                break;
            }
            continue;
        }
        
        // 检查是否有完整的 HTTP 请求
        if (memmem(buf, n, "\r\n\r\n", 4)) {
            g_total_requests.fetch_add(1, std::memory_order_relaxed);
            
            // 发送响应
            ssize_t written = ::send(fd, HTTP_RESPONSE, RESPONSE_LEN, MSG_NOSIGNAL);
            if (written != (ssize_t)RESPONSE_LEN) {
                // 如果没有完全写入，等待可写后重试
                if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                    co_await EpollWriteAwaiter{loop, fd};
                    ::send(fd, HTTP_RESPONSE, RESPONSE_LEN, MSG_NOSIGNAL);
                }
            }
        }
    }
    
    ::close(fd);
}

Task<void> epoll_accept_loop(int listen_fd, EpollPerCoreEventLoop& loop) {
    while (g_running) {
        co_await EpollReadAwaiter{loop, listen_fd};
        
        // 批量 accept
        while (g_running) {
            struct sockaddr_in addr{};
            socklen_t len = sizeof(addr);
            int client_fd = accept4(listen_fd, (struct sockaddr*)&addr, &len,
                                   SOCK_NONBLOCK | SOCK_CLOEXEC);
            if (client_fd < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                continue;
            }
            
            g_total_connections.fetch_add(1, std::memory_order_relaxed);
            
            // 启动连接处理协程
            auto task = handle_epoll_connection(client_fd, loop);
            auto h = task.handle();
            if (h && !h.done()) h.resume();
            task.release();
        }
    }
}

// 每核独立 listen socket 版本（利用 SO_REUSEPORT 内核负载均衡）
int create_listen_socket(uint16_t port) {
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (fd < 0) return -1;
    
    int on = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
    setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &on, sizeof(on));  // 关键：允许多 socket 绑定同一端口
    
    int qlen = 128;
    setsockopt(fd, SOL_TCP, TCP_FASTOPEN, &qlen, sizeof(qlen));
    
    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = INADDR_ANY;
    
    if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }
    
    if (listen(fd, 65535) < 0) {
        close(fd);
        return -1;
    }
    
    return fd;
}

void run_epoll_core(uint16_t port, int core_id) {
    // 每个核心创建独立的 listen socket
    int listen_fd = create_listen_socket(port);
    if (listen_fd < 0) {
        fprintf(stderr, "Core %d: 创建 listen socket 失败\n", core_id);
        return;
    }
    
    EpollPerCoreEventLoop loop;
    loop.bind_to_core(core_id);
    set_current_event_loop(&loop);
    
    auto task = epoll_accept_loop(listen_fd, loop);
    auto h = task.handle();
    if (h && !h.done()) h.resume();
    task.release();
    
    loop.run();
    
    close(listen_fd);  // 清理本核心的 listen socket
}

// =============================================================================
// io_uring 后端
// =============================================================================

#ifdef ZLCORO_HAS_IO_URING

Task<void> handle_iouring_connection(int fd, IoUringPerCoreEventLoop& loop) {
    char buf[4096];
    
    // 设置 TCP_NODELAY
    int opt = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));
    
    while (g_running) {
        // 先尝试非阻塞读取
        ssize_t n = ::recv(fd, buf, sizeof(buf), MSG_DONTWAIT);
        
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // 使用 io_uring 等待可读
                auto read_req = loop.prep_recv(fd, buf, sizeof(buf), 0);
                loop.submit();
                n = co_await make_awaiter(read_req);
            } else {
                break;  // 真正的错误
            }
        }
        
        if (n <= 0) break;
        
        // 检查是否有完整的 HTTP 请求
        if (memmem(buf, n, "\r\n\r\n", 4)) {
            g_total_requests.fetch_add(1, std::memory_order_relaxed);
            
            // 先尝试非阻塞发送
            ssize_t written = ::send(fd, HTTP_RESPONSE, RESPONSE_LEN, MSG_NOSIGNAL | MSG_DONTWAIT);
            
            if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                // 使用 io_uring 异步发送
                auto write_req = loop.prep_send(fd, HTTP_RESPONSE, RESPONSE_LEN, MSG_NOSIGNAL);
                loop.submit();
                written = co_await make_awaiter(write_req);
            }
            
            if (written != (ssize_t)RESPONSE_LEN) break;
        }
    }
    
    ::close(fd);
}

Task<void> iouring_accept_loop(int listen_fd, IoUringPerCoreEventLoop& loop) {
    while (g_running) {
        struct sockaddr_in addr{};
        socklen_t len = sizeof(addr);
        
        // 使用 io_uring 异步 accept
        auto accept_req = loop.prep_accept(listen_fd, (struct sockaddr*)&addr, &len);
        loop.submit();
        
        int client_fd = co_await make_awaiter(accept_req);
        
        if (client_fd < 0) {
            if (!g_running) break;
            continue;
        }
        
        g_total_connections.fetch_add(1, std::memory_order_relaxed);
        
        // 启动连接处理协程
        auto task = handle_iouring_connection(client_fd, loop);
        auto h = task.handle();
        if (h && !h.done()) h.resume();
        task.release();
    }
}

void run_iouring_core(uint16_t port, int core_id) {
    // 每个核心创建独立的 listen socket
    int listen_fd = create_listen_socket(port);
    if (listen_fd < 0) {
        fprintf(stderr, "Core %d: 创建 listen socket 失败\n", core_id);
        return;
    }
    
    IoUringPerCoreEventLoop loop(4096);  // 更大的队列
    loop.bind_to_core(core_id);
    set_current_event_loop(&loop);
    
    auto task = iouring_accept_loop(listen_fd, loop);
    auto h = task.handle();
    if (h && !h.done()) h.resume();
    task.release();
    
    loop.run();
    
    close(listen_fd);  // 清理本核心的 listen socket
}

#endif // ZLCORO_HAS_IO_URING

// =============================================================================
// main
// =============================================================================

void print_usage(const char* prog) {
    printf("用法: %s [选项]\n", prog);
    printf("选项:\n");
    printf("  -p <port>    监听端口 (默认: 8080)\n");
    printf("  -c <cores>   核心数 (默认: CPU核数)\n");
    printf("  -u           使用 io_uring 后端\n");
}

int main(int argc, char* argv[]) {
    uint16_t port = 8080;
    int num_cores = std::thread::hardware_concurrency();
    bool use_iouring = false;
    
    int opt;
    while ((opt = getopt(argc, argv, "p:c:uh")) != -1) {
        switch (opt) {
            case 'p': port = std::stoi(optarg); break;
            case 'c': num_cores = std::stoi(optarg); break;
            case 'u': use_iouring = true; break;
            case 'h':
            default:
                print_usage(argv[0]);
                return opt == 'h' ? 0 : 1;
        }
    }
    
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    signal(SIGPIPE, SIG_IGN);
    
    // 验证端口可用（快速失败）
    {
        int test_fd = create_listen_socket(port);
        if (test_fd < 0) {
            perror("无法绑定端口");
            return 1;
        }
        close(test_fd);
    }
    
    printf("===========================================\n");
    printf("HTTP Echo 服务器 v2 (SO_REUSEPORT 优化版)\n");
    printf("===========================================\n");
    printf("端口: %d\n", port);
    printf("核心数: %d\n", num_cores);
    printf("后端: %s\n", use_iouring ? "io_uring" : "epoll");
    printf("架构: 每核独立 listen socket (无惊群)\n");
    printf("===========================================\n");
    printf("测试命令:\n");
    printf("  wrk -t16 -c400 -d30s http://127.0.0.1:%d/\n", port);
    printf("===========================================\n\n");
    
    std::vector<std::thread> threads;
    threads.reserve(num_cores);
    
    if (use_iouring) {
#ifdef ZLCORO_HAS_IO_URING
        for (int i = 0; i < num_cores; ++i) {
            threads.emplace_back(run_iouring_core, port, i);
        }
#else
        fprintf(stderr, "io_uring 不可用\n");
        return 1;
#endif
    } else {
        for (int i = 0; i < num_cores; ++i) {
            threads.emplace_back(run_epoll_core, port, i);
        }
    }
    
    for (auto& t : threads) {
        t.join();
    }
    
    printf("\n===========================================\n");
    printf("服务器已停止\n");
    printf("总连接数: %lu\n", g_total_connections.load());
    printf("总请求数: %lu\n", g_total_requests.load());
    printf("===========================================\n");
    
    return 0;
}
