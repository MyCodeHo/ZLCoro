/**
 * 多核心优化 Echo 服务器
 */

#include "zlcoro/runtime/optimized_connection.hpp"
#include "zlcoro/runtime/per_core_runtime.hpp"
#include <cstdio>
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

struct CoreData {
    std::unique_ptr<EpollPerCoreEventLoop> loop;
    std::unique_ptr<OptimizedEpollManager> mgr;
    std::thread thread;
};

// 连接处理协程
Task<void> handle_connection(int fd, OptimizedEpollManager& mgr) {
    OptimizedEpollConnection conn(fd, mgr);
    
    char buf[4096];
    while (g_running) {
        ssize_t n = co_await conn.read(buf, sizeof(buf));
        if (n <= 0) break;
        
        g_requests.fetch_add(1);
        
        ssize_t written = co_await conn.write(buf, n);
        if (written != n) break;
    }
}

// Accept 协程
Task<void> accept_loop(int listen_fd, EpollPerCoreEventLoop& loop, OptimizedEpollManager& mgr) {
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
            
            // 设置 TCP_NODELAY
            int opt = 1;
            setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));
            
            g_connections++;
            
            // 启动处理协程
            auto task = handle_connection(client_fd, mgr);
            auto h = task.handle();
            if (h && !h.done()) {
                h.resume();
            }
            task.release();
        }
    }
}

void run_core(int listen_fd, CoreData& data, int core_id) {
    data.loop->bind_to_core(core_id);
    set_current_event_loop(data.loop.get());
    
    // 启动 accept 协程
    auto task = accept_loop(listen_fd, *data.loop, *data.mgr);
    auto h = task.handle();
    if (h && !h.done()) {
        h.resume();
    }
    task.release();
    
    // 运行事件循环
    data.loop->run();
}

void print_usage(const char* prog) {
    printf("用法: %s [选项]\n", prog);
    printf("选项:\n");
    printf("  -p <port>    监听端口 (默认: 12399)\n");
    printf("  -c <cores>   核心数 (默认: CPU核数)\n");
}

int main(int argc, char* argv[]) {
    uint16_t port = 12399;
    int num_cores = std::thread::hardware_concurrency();
    
    int opt;
    while ((opt = getopt(argc, argv, "p:c:h")) != -1) {
        switch (opt) {
            case 'p': port = std::stoi(optarg); break;
            case 'c': num_cores = std::stoi(optarg); break;
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
    
    // TCP Fast Open
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
    
    // 大 backlog
    if (listen(listen_fd, 65535) < 0) {
        perror("listen");
        return 1;
    }
    
    printf("===========================================\n");
    printf("多核心优化 Echo 服务器\n");
    printf("===========================================\n");
    printf("端口: %d\n", port);
    printf("核心数: %d\n", num_cores);
    printf("===========================================\n");
    printf("服务器启动在 :%d\n", port);
    
    // 初始化每个核心
    std::vector<CoreData> cores(num_cores);
    for (int i = 0; i < num_cores; ++i) {
        cores[i].loop = std::make_unique<EpollPerCoreEventLoop>();
        cores[i].mgr = std::make_unique<OptimizedEpollManager>(*cores[i].loop);
        cores[i].thread = std::thread(run_core, listen_fd, std::ref(cores[i]), i);
    }
    
    // 等待线程结束
    for (auto& core : cores) {
        if (core.thread.joinable()) {
            core.thread.join();
        }
    }
    
    close(listen_fd);
    
    printf("\n===========================================\n");
    printf("统计: 连接=%lu, 请求=%lu\n", g_connections.load(), g_requests.load());
    printf("===========================================\n");
    
    return 0;
}
