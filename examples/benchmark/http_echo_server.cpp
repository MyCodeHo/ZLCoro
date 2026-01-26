/**
 * HTTP Echo 服务器 - 用于 wrk 性能测试
 * 支持 epoll 和 io_uring 两种后端
 */

#include "zlcoro/runtime/optimized_connection.hpp"
#include "zlcoro/runtime/per_core_runtime.hpp"
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

using namespace zlcoro;

std::atomic<bool> g_running{true};
std::atomic<uint64_t> g_connections{0};
std::atomic<uint64_t> g_requests{0};

void signal_handler(int) {
    g_running = false;
}

// HTTP 响应（Keep-Alive）
const char* HTTP_RESPONSE = 
    "HTTP/1.1 200 OK\r\n"
    "Content-Type: text/plain\r\n"
    "Content-Length: 13\r\n"
    "Connection: keep-alive\r\n"
    "\r\n"
    "Hello, World!";

const size_t RESPONSE_LEN = strlen(HTTP_RESPONSE);

// epoll 处理函数
Task<void> handle_epoll_connection(int fd, OptimizedEpollManager& mgr) {
    OptimizedEpollConnection conn(fd, mgr);
    
    char buf[4096];
    while (g_running) {
        // 读取 HTTP 请求
        ssize_t n = co_await conn.read(buf, sizeof(buf));
        if (n <= 0) break;
        
        // 简单解析 - 找到请求结束（\r\n\r\n）
        if (memmem(buf, n, "\r\n\r\n", 4)) {
            g_requests.fetch_add(1);
            
            // 发送响应
            ssize_t written = co_await conn.write(HTTP_RESPONSE, RESPONSE_LEN);
            if (written != (ssize_t)RESPONSE_LEN) break;
        }
    }
}

#ifdef ZLCORO_HAS_IO_URING

// io_uring 处理函数
Task<void> handle_iouring_connection(int fd, OptimizedIoUringManager& mgr) {
    OptimizedIoUringConnection conn(fd, mgr);
    
    char buf[4096];
    while (g_running) {
        ssize_t n = co_await conn.read(buf, sizeof(buf));
        if (n <= 0) break;
        
        if (memmem(buf, n, "\r\n\r\n", 4)) {
            g_requests.fetch_add(1);
            
            ssize_t written = co_await conn.write(HTTP_RESPONSE, RESPONSE_LEN);
            if (written != (ssize_t)RESPONSE_LEN) break;
        }
    }
}

#endif

// epoll 核心数据
struct EpollCoreData {
    std::unique_ptr<EpollPerCoreEventLoop> loop;
    std::unique_ptr<OptimizedEpollManager> mgr;
    std::thread thread;
};

// epoll accept 协程
Task<void> epoll_accept_loop(int listen_fd, EpollPerCoreEventLoop& loop, OptimizedEpollManager& mgr) {
    while (g_running) {
        co_await EpollReadAwaiter{loop, listen_fd};
        
        while (g_running) {
            struct sockaddr_in addr{};
            socklen_t len = sizeof(addr);
            int client_fd = accept4(listen_fd, (struct sockaddr*)&addr, &len,
                                   SOCK_NONBLOCK | SOCK_CLOEXEC);
            if (client_fd < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                continue;
            }
            
            int opt = 1;
            setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));
            
            g_connections++;
            
            auto task = handle_epoll_connection(client_fd, mgr);
            auto h = task.handle();
            if (h && !h.done()) h.resume();
            task.release();
        }
    }
}

void epoll_run_core(int listen_fd, EpollCoreData& data, int core_id) {
    data.loop->bind_to_core(core_id);
    set_current_event_loop(data.loop.get());
    
    auto task = epoll_accept_loop(listen_fd, *data.loop, *data.mgr);
    auto h = task.handle();
    if (h && !h.done()) h.resume();
    task.release();
    
    data.loop->run();
}

#ifdef ZLCORO_HAS_IO_URING

// io_uring 核心数据
struct IoUringCoreData {
    std::unique_ptr<IoUringPerCoreEventLoop> loop;
    std::unique_ptr<OptimizedIoUringManager> mgr;
    std::thread thread;
};

// io_uring accept 协程 - 使用非阻塞 accept
Task<void> iouring_accept_loop(int listen_fd, IoUringPerCoreEventLoop& loop, OptimizedIoUringManager& mgr) {
    // 将监听 socket 设为非阻塞
    int flags = fcntl(listen_fd, F_GETFL, 0);
    fcntl(listen_fd, F_SETFL, flags | O_NONBLOCK);
    
    while (g_running) {
        // 先尝试非阻塞 accept
        while (g_running) {
            struct sockaddr_in addr{};
            socklen_t len = sizeof(addr);
            int client_fd = accept4(listen_fd, (struct sockaddr*)&addr, &len, SOCK_NONBLOCK | SOCK_CLOEXEC);
            
            if (client_fd >= 0) {
                int opt = 1;
                setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));
                g_connections++;
                
                auto task = handle_iouring_connection(client_fd, mgr);
                auto h = task.handle();
                if (h && !h.done()) h.resume();
                task.release();
            } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // 没有更多连接，等待事件
                break;
            } else {
                // 其他错误，继续
                break;
            }
        }
        
        if (!g_running) break;
        
        // 等待新连接
        struct sockaddr_in addr{};
        socklen_t len = sizeof(addr);
        auto req = loop.prep_accept(listen_fd, (struct sockaddr*)&addr, &len);
        loop.submit();
        
        int client_fd = co_await make_awaiter(req);
        if (client_fd < 0) {
            if (!g_running) break;
            continue;
        }
        
        fcntl(client_fd, F_SETFL, fcntl(client_fd, F_GETFL, 0) | O_NONBLOCK);
        
        int opt = 1;
        setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));
        g_connections++;
        
        auto task = handle_iouring_connection(client_fd, mgr);
        auto h = task.handle();
        if (h && !h.done()) h.resume();
        task.release();
    }
}

void iouring_run_core(int listen_fd, IoUringCoreData& data, int core_id) {
    data.loop->bind_to_core(core_id);
    set_current_event_loop(data.loop.get());
    
    auto task = iouring_accept_loop(listen_fd, *data.loop, *data.mgr);
    auto h = task.handle();
    if (h && !h.done()) h.resume();
    task.release();
    
    data.loop->run();
}

#endif

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
    
    // 创建监听 socket
    int listen_fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (listen_fd < 0) {
        perror("socket");
        return 1;
    }
    
    int on = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEPORT, &on, sizeof(on));
    
    int qlen = 128;
    setsockopt(listen_fd, SOL_TCP, TCP_FASTOPEN, &qlen, sizeof(qlen));
    
    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = INADDR_ANY;
    
    if (bind(listen_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind");
        return 1;
    }
    
    if (listen(listen_fd, 65535) < 0) {
        perror("listen");
        return 1;
    }
    
    printf("===========================================\n");
    printf("HTTP Echo 服务器\n");
    printf("===========================================\n");
    printf("端口: %d\n", port);
    printf("核心数: %d\n", num_cores);
    printf("后端: %s\n", use_iouring ? "io_uring" : "epoll");
    printf("===========================================\n");
    printf("服务器启动，等待 wrk 测试...\n\n");
    printf("测试命令示例:\n");
    printf("  wrk -t8 -c400 -d30s http://127.0.0.1:%d/\n", port);
    printf("  wrk -t16 -c1000 -d30s http://127.0.0.1:%d/\n", port);
    printf("===========================================\n\n");
    
    if (use_iouring) {
#ifdef ZLCORO_HAS_IO_URING
        std::vector<IoUringCoreData> cores(num_cores);
        for (int i = 0; i < num_cores; ++i) {
            cores[i].loop = std::make_unique<IoUringPerCoreEventLoop>(4096);
            cores[i].mgr = std::make_unique<OptimizedIoUringManager>(*cores[i].loop);
            cores[i].thread = std::thread(iouring_run_core, listen_fd, std::ref(cores[i]), i);
        }
        
        for (auto& core : cores) {
            if (core.thread.joinable()) {
                core.thread.join();
            }
        }
#else
        fprintf(stderr, "io_uring 不可用\n");
        return 1;
#endif
    } else {
        std::vector<EpollCoreData> cores(num_cores);
        for (int i = 0; i < num_cores; ++i) {
            cores[i].loop = std::make_unique<EpollPerCoreEventLoop>();
            cores[i].mgr = std::make_unique<OptimizedEpollManager>(*cores[i].loop);
            cores[i].thread = std::thread(epoll_run_core, listen_fd, std::ref(cores[i]), i);
        }
        
        for (auto& core : cores) {
            if (core.thread.joinable()) {
                core.thread.join();
            }
        }
    }
    
    close(listen_fd);
    
    printf("\n===========================================\n");
    printf("服务器已停止\n");
    printf("总连接数: %lu\n", g_connections.load());
    printf("总请求数: %lu\n", g_requests.load());
    printf("===========================================\n");
    
    return 0;
}
