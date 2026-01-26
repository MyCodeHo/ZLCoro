/**
 * 多核共享 listen_fd 服务器（演示惊群效应）
 * 编译: g++ -std=c++20 -O2 -I include -pthread create_shared_fd_server.cpp -o shared_fd_server -luring
 */
#include <iostream>
#include <thread>
#include <chrono>
#include <vector>
#include <atomic>
#include <csignal>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/epoll.h>
#include <cstring>

std::atomic<bool> g_running{true};
std::atomic<uint64_t> g_total_requests{0};
uint32_t g_cpu_iters = 0;
uint32_t g_io_wait_us = 0;

void signal_handler(int) {
    g_running = false;
}

static const char HTTP_RESPONSE[] = 
    "HTTP/1.1 200 OK\r\n"
    "Content-Type: text/plain\r\n"
    "Content-Length: 13\r\n"
    "Connection: keep-alive\r\n"
    "\r\n"
    "Hello, World!";

static inline void simulate_cpu_work(uint32_t iters) {
    if (iters == 0) return;
    volatile uint64_t x = 0x9e3779b97f4a7c15ULL;
    for (uint32_t i = 0; i < iters; ++i) {
        x ^= (x << 7) + (x >> 3) + i;
    }
}

static inline void simulate_io_wait(uint32_t us) {
    if (us == 0) return;
    std::this_thread::sleep_for(std::chrono::microseconds(us));
}

void worker_thread(int listen_fd, int core_id) {
    // 绑定到指定核心
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(core_id, &cpuset);
    pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
    
    // 创建 epoll
    int epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    
    // 注册 listen_fd（所有线程共享同一个 listen_fd - 惊群！）
    struct epoll_event ev{};
    ev.events = EPOLLIN | EPOLLET;
    ev.data.fd = listen_fd;
    epoll_ctl(epoll_fd, EPOLL_CTL_ADD, listen_fd, &ev);
    
    std::vector<int> client_fds;
    client_fds.reserve(1000);
    
    struct epoll_event events[128];
    char buf[4096];
    
    while (g_running) {
        int n = epoll_wait(epoll_fd, events, 128, 100);
        
        for (int i = 0; i < n; ++i) {
            int fd = events[i].data.fd;
            
            if (fd == listen_fd) {
                // accept 新连接
                while (true) {
                    struct sockaddr_in addr{};
                    socklen_t len = sizeof(addr);
                    int client_fd = accept4(listen_fd, (struct sockaddr*)&addr, &len,
                                           SOCK_NONBLOCK | SOCK_CLOEXEC);
                    if (client_fd < 0) break;
                    
                    int opt = 1;
                    setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));
                    
                    struct epoll_event cev{};
                    cev.events = EPOLLIN | EPOLLET;
                    cev.data.fd = client_fd;
                    epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client_fd, &cev);
                    client_fds.push_back(client_fd);
                }
            } else {
                // 处理客户端请求
                while (true) {
                    ssize_t nr = recv(fd, buf, sizeof(buf), MSG_DONTWAIT);
                    if (nr <= 0) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                        epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, nullptr);
                        close(fd);
                        break;
                    }
                    
                    g_total_requests++;

                    simulate_cpu_work(g_cpu_iters);
                    simulate_io_wait(g_io_wait_us);
                    
                    ssize_t nw = send(fd, HTTP_RESPONSE, sizeof(HTTP_RESPONSE) - 1, MSG_NOSIGNAL);
                    if (nw < 0) {
                        epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, nullptr);
                        close(fd);
                        break;
                    }
                }
            }
        }
    }
    
    for (int fd : client_fds) {
        close(fd);
    }
    close(epoll_fd);
}

int main(int argc, char* argv[]) {
    uint16_t port = 8080;
    int num_cores = 8;
    
    if (argc > 1) port = std::stoi(argv[1]);
    if (argc > 2) num_cores = std::stoi(argv[2]);
    if (argc > 3) g_cpu_iters = static_cast<uint32_t>(std::stoul(argv[3]));
    if (argc > 4) g_io_wait_us = static_cast<uint32_t>(std::stoul(argv[4]));
    
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    signal(SIGPIPE, SIG_IGN);
    
    // 创建共享的 listen socket（不使用 SO_REUSEPORT）
    int listen_fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    int on = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
    // 注意：故意不设置 SO_REUSEPORT，导致惊群效应
    
    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = INADDR_ANY;
    
    if (bind(listen_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind");
        return 1;
    }
    
    if (listen(listen_fd, 8192) < 0) {
        perror("listen");
        return 1;
    }
    
    std::cout << "========================================\n";
    std::cout << "多核共享 listen_fd 服务器（惊群版本）\n";
    std::cout << "========================================\n";
    std::cout << "端口: " << port << "\n";
    std::cout << "核心数: " << num_cores << "\n";
    std::cout << "架构: 所有核心共享 listen socket (有惊群)\n";
    std::cout << "CPU 负载: " << g_cpu_iters << " iters/req\n";
    std::cout << "I/O 等待: " << g_io_wait_us << " us/req\n";
    std::cout << "========================================\n\n";
    
    std::vector<std::thread> threads;
    for (int i = 0; i < num_cores; ++i) {
        threads.emplace_back(worker_thread, listen_fd, i);
    }
    
    for (auto& t : threads) {
        t.join();
    }
    
    close(listen_fd);
    
    std::cout << "\n总请求数: " << g_total_requests.load() << "\n";
    
    return 0;
}
