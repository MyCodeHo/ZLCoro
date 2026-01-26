/**
 * 简化版测试 - 排查 optimized_connection 问题
 */

#include "zlcoro/runtime/optimized_connection.hpp"
#include "zlcoro/runtime/per_core_runtime.hpp"
#include <cstdio>
#include <thread>
#include <atomic>
#include <csignal>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

using namespace zlcoro;

std::atomic<bool> running{true};
std::atomic<int> conn_count{0};
std::atomic<int> req_count{0};

void signal_handler(int) {
    running = false;
}

Task<void> handle_client(int fd, OptimizedEpollManager& mgr) {
    OptimizedEpollConnection conn(fd, mgr);
    
    char buf[1024];
    while (running) {
        ssize_t n = co_await conn.read(buf, sizeof(buf));
        if (n <= 0) {
            printf("连接 %d 读取结束: n=%zd\n", fd, n);
            break;
        }
        
        req_count++;
        
        // 回显
        ssize_t w = co_await conn.write(buf, n);
        if (w != n) {
            printf("连接 %d 写入失败: w=%zd, n=%zd\n", fd, w, n);
            break;
        }
    }
    printf("连接 %d 处理完成, ops=%lu\n", fd, conn.total_ops());
}

int main() {
    signal(SIGINT, signal_handler);
    signal(SIGPIPE, SIG_IGN);
    
    // 创建监听 socket
    int listen_fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (listen_fd < 0) {
        perror("socket");
        return 1;
    }
    
    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));
    
    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(12399);
    addr.sin_addr.s_addr = INADDR_ANY;
    
    if (bind(listen_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind");
        return 1;
    }
    
    if (listen(listen_fd, 128) < 0) {
        perror("listen");
        return 1;
    }
    
    printf("简化测试服务器启动在端口 12399\n");
    
    // 单线程事件循环
    EpollPerCoreEventLoop loop;
    OptimizedEpollManager mgr(loop);
    
    set_current_event_loop(&loop);
    
    // Accept 协程
    auto accept_coro = [&]() -> Task<void> {
        while (running) {
            co_await EpollReadAwaiter{loop, listen_fd};
            
            while (true) {
                int client_fd = accept4(listen_fd, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
                if (client_fd < 0) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                    continue;
                }
                
                conn_count++;
                printf("接受连接 %d (总数: %d)\n", client_fd, conn_count.load());
                
                // 启动处理协程
                auto task = handle_client(client_fd, mgr);
                auto h = task.handle();
                if (h && !h.done()) {
                    h.resume();
                }
                task.release();
            }
        }
    };
    
    auto task = accept_coro();
    auto h = task.handle();
    if (h && !h.done()) {
        h.resume();
    }
    task.release();
    
    printf("开始事件循环...\n");
    
    // 事件循环
    loop.run();
    
    printf("\n统计: 连接=%d, 请求=%d\n", conn_count.load(), req_count.load());
    return 0;
}
