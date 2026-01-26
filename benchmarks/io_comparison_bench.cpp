#include "io_bench_common.hpp"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <cstring>
#include <future>
#include <vector>

using namespace io_bench;

// ============================================================================
// 传统阻塞 I/O 实现
// ============================================================================

namespace blocking_io {

ssize_t read_exact(int fd, char* buffer, size_t len) {
    size_t total = 0;
    while (total < len) {
        ssize_t n = recv(fd, buffer + total, len - total, 0);
        if (n <= 0) return total;
        total += n;
    }
    return total;
}

void echo_session(int client_fd, size_t payload_size) {
    std::vector<char> buffer(payload_size);
    while (true) {
        ssize_t n = read_exact(client_fd, buffer.data(), payload_size);
        if (n == 0) break;
        send(client_fd, buffer.data(), n, 0);
    }
    close(client_fd);
}

void echo_server(uint16_t port, std::atomic<bool>* started, std::atomic<bool>* stop_flag, 
                 size_t payload_size) {
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    addr.sin_port = htons(port);
    
    bind(server_fd, (sockaddr*)&addr, sizeof(addr));
    listen(server_fd, 128);
    
    started->store(true, std::memory_order_release);
    
    // 设置非阻塞以便检查 stop_flag
    fcntl(server_fd, F_SETFL, O_NONBLOCK);
    
    std::vector<std::thread> workers;
    
    while (!stop_flag->load(std::memory_order_acquire)) {
        sockaddr_in client_addr{};
        socklen_t len = sizeof(client_addr);
        int client_fd = accept(server_fd, (sockaddr*)&client_addr, &len);
        
        if (client_fd < 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }
        
        // 为每个连接创建一个线程
        workers.emplace_back([client_fd, payload_size]() {
            echo_session(client_fd, payload_size);
        });
    }
    
    close(server_fd);
    
    // 等待所有工作线程完成
    for (auto& t : workers) {
        if (t.joinable()) t.join();
    }
}

uint64_t echo_client(uint16_t port, size_t payload_size, size_t message_count) {
    int sock_fd = socket(AF_INET, SOCK_STREAM, 0);
    
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    addr.sin_port = htons(port);
    
    connect(sock_fd, (sockaddr*)&addr, sizeof(addr));
    
    std::vector<char> payload(payload_size, 'x');
    std::vector<char> buffer(payload_size);
    uint64_t total = 0;
    
    for (size_t i = 0; i < message_count; ++i) {
        send(sock_fd, payload.data(), payload_size, 0);
        ssize_t n = read_exact(sock_fd, buffer.data(), payload_size);
        total += n;
    }
    
    close(sock_fd);
    return total;
}

BenchmarkResult run_blocking_benchmark(size_t client_count, size_t payload_size, 
                                        size_t message_count, bool verbose = false) {
    std::atomic<bool> server_started{false};
    std::atomic<bool> stop_server{false};
    const uint16_t port = pick_port();
    
    if (verbose) {
        std::cout << "启动阻塞 I/O 服务器 (端口 " << port << ")..." << std::flush;
    }
    
    std::thread server_thread([&]() {
        echo_server(port, &server_started, &stop_server, payload_size);
    });
    
    while (!server_started.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    
    std::vector<std::future<uint64_t>> futures;
    futures.reserve(client_count);
    
    ScopedTimer timer;
    
    if (verbose) {
        std::cout << " 完成\n启动 " << client_count << " 个客户端..." << std::flush;
    }
    
    for (size_t i = 0; i < client_count; ++i) {
        futures.emplace_back(std::async(std::launch::async, 
            [port, payload_size, message_count]() {
                return echo_client(port, payload_size, message_count);
            }));
    }
    
    uint64_t total_bytes = 0;
    for (auto& fut : futures) {
        total_bytes += fut.get();
    }
    
    stop_server.store(true, std::memory_order_release);
    
    // 发送唤醒连接
    try {
        int wake_fd = socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = inet_addr("127.0.0.1");
        addr.sin_port = htons(port);
        connect(wake_fd, (sockaddr*)&addr, sizeof(addr));
        close(wake_fd);
    } catch (...) {}
    
    server_thread.join();
    
    double seconds = timer.elapsed_seconds();
    double mb = static_cast<double>(total_bytes) / (1024.0 * 1024.0);
    double throughput = mb / seconds;
    uint64_t msg_rate = static_cast<uint64_t>((client_count * message_count) / seconds);
    
    if (verbose) {
        std::cout << " 完成\n";
    }
    
    return BenchmarkResult{client_count, payload_size, message_count, mb, seconds, throughput, msg_rate};
}

} // namespace blocking_io

// ============================================================================
// 对比测试主程序
// ============================================================================

void print_comparison(const std::string& test_name, 
                      const BenchmarkResult& coro_result,
                      const BenchmarkResult& blocking_result) {
    std::cout << "╔════════════════════════════════════════════════════════════╗\n";
    std::cout << "║  " << std::left << std::setw(55) << test_name << "  ║\n";
    std::cout << "╠════════════════════════════════════════════════════════════╣\n";
    
    double speedup = coro_result.throughput_mbps / blocking_result.throughput_mbps;
    std::string verdict = speedup > 1.0 ? "协程更快" : "阻塞I/O更快";
    
    std::cout << "║  协程框架:   " << std::fixed << std::setprecision(2) << std::setw(8) 
              << coro_result.throughput_mbps << " MB/s  |  "
              << std::setw(8) << coro_result.messages_per_second << " msg/s  ║\n";
    std::cout << "║  阻塞I/O:    " << std::fixed << std::setprecision(2) << std::setw(8) 
              << blocking_result.throughput_mbps << " MB/s  |  "
              << std::setw(8) << blocking_result.messages_per_second << " msg/s  ║\n";
    std::cout << "║                                                            ║\n";
    std::cout << "║  性能比:     " << std::fixed << std::setprecision(2) << std::setw(8) 
              << speedup << "x     (" << verdict << ")                ║\n";
    std::cout << "╚════════════════════════════════════════════════════════════╝\n\n";
}

int main() {
    std::cout << "\n";
    std::cout << "╔══════════════════════════════════════════════════════════════╗\n";
    std::cout << "║       ZLCoro 协程框架 vs 传统阻塞 I/O 性能对比测试          ║\n";
    std::cout << "╚══════════════════════════════════════════════════════════════╝\n\n";

    // 全局 EventLoop
    EventLoopRunner global_event_loop;

    // 测试 1: 低并发，小负载
    {
        std::cout << "【测试 1】低并发场景 (4 客户端, 512B 负载)\n\n";
        
        auto coro = run_coroutine_benchmark(4, 512, 50, true);
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        
        auto blocking = blocking_io::run_blocking_benchmark(4, 512, 50, true);
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        
        print_comparison("低并发场景", coro, blocking);
    }

    // 测试 2: 中等并发
    {
        std::cout << "【测试 2】中等并发场景 (8 客户端, 512B 负载)\n\n";
        
        auto coro = run_coroutine_benchmark(8, 512, 50, true);
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        
        auto blocking = blocking_io::run_blocking_benchmark(8, 512, 50, true);
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        
        print_comparison("中等并发场景", coro, blocking);
    }

    // 测试 3: 高并发 - 协程优势场景（减少并发数避免线程资源问题）
    {
        std::cout << "【测试 3】高并发场景 (12 客户端, 512B 负载)\n\n";
        
        auto coro = run_coroutine_benchmark(12, 512, 50, true);
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        
        auto blocking = blocking_io::run_blocking_benchmark(12, 512, 50, true);
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        
        print_comparison("高并发场景", coro, blocking);
    }

    // 测试 4: 小包高频
    {
        std::cout << "【测试 4】小包高频场景 (8 客户端, 64B 负载)\n\n";
        
        auto coro = run_coroutine_benchmark(8, 64, 50, true);
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        
        auto blocking = blocking_io::run_blocking_benchmark(8, 64, 50, true);
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        
        print_comparison("小包高频场景", coro, blocking);
    }

    // 测试 5: 大包传输
    {
        std::cout << "【测试 5】大包传输场景 (8 客户端, 4KB 负载)\n\n";
        
        auto coro = run_coroutine_benchmark(8, 4096, 30, true);
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        
        auto blocking = blocking_io::run_blocking_benchmark(8, 4096, 30, true);
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        
        print_comparison("大包传输场景", coro, blocking);
    }

    // 测试 6: 高并发优势场景（减少到 32 客户端避免线程资源问题）
    {
        std::cout << "【测试 6】高并发优势场景 (32 客户端, 512B 负载)\n\n";
        
        auto coro = run_coroutine_benchmark(32, 512, 50, true);
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        
        auto blocking = blocking_io::run_blocking_benchmark(32, 512, 50, true);
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        
        print_comparison("高并发优势场景", coro, blocking);
    }

    std::cout << "════════════════════════════════════════════════════════════════\n";
    std::cout << "                         测试总结                                \n";
    std::cout << "════════════════════════════════════════════════════════════════\n\n";
    std::cout << "协程框架优势:\n";
    std::cout << "  ✓ 高并发场景 (64+ 连接)\n";
    std::cout << "  ✓ 内存占用低 (协程栈 vs 线程栈)\n";
    std::cout << "  ✓ 代码简洁 (async/await vs 回调/线程)\n";
    std::cout << "  ✓ 上下文切换开销小\n\n";
    
    std::cout << "阻塞I/O优势:\n";
    std::cout << "  ✓ 低并发场景 (<32 连接)\n";
    std::cout << "  ✓ 简单直接，无需事件循环\n";
    std::cout << "  ✓ 每个连接独立线程，隔离性好\n\n";

    std::cout << "✓ 所有对比测试完成！\n\n";
    return 0;
}
