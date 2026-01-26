#pragma once

#include "zlcoro/io/async_socket.hpp"
#include "zlcoro/scheduler/async.hpp"
#include <atomic>
#include <chrono>
#include <iostream>
#include <string>
#include <thread>
#include <vector>
#include <iomanip>

namespace io_bench {

using namespace std::chrono;
using zlcoro::AsyncSocket;
using zlcoro::EventLoop;
using zlcoro::Task;

// ============================================================================
// EventLoop 管理
// ============================================================================

struct EventLoopRunner {
    EventLoopRunner() : loop_(EventLoop::instance()), thread_([this] { loop_.run(); }) {}
    ~EventLoopRunner() {
        loop_.stop();
        if (thread_.joinable()) {
            thread_.join();
        }
        loop_.reset();
    }
    EventLoop& loop_;
    std::thread thread_;
};

// ============================================================================
// 计时工具
// ============================================================================

class ScopedTimer {
public:
    ScopedTimer() : start_(steady_clock::now()) {}
    double elapsed_seconds() const {
        return duration_cast<duration<double>>(steady_clock::now() - start_).count();
    }
private:
    steady_clock::time_point start_;
};

// ============================================================================
// 端口分配
// ============================================================================

inline uint16_t pick_port() {
    static std::atomic<uint16_t> base_port{25080};
    return base_port.fetch_add(1, std::memory_order_relaxed);
}

// ============================================================================
// 协程版本 Echo 实现
// ============================================================================

inline Task<size_t> read_exact(AsyncSocket& socket, std::string& buffer, size_t len) {
    buffer.clear();
    buffer.reserve(len);
    while (buffer.size() < len) {
        auto chunk = co_await socket.read(len - buffer.size());
        if (chunk.empty()) {
            co_return buffer.size();
        }
        buffer.append(chunk);
    }
    co_return buffer.size();
}

inline Task<void> echo_session(AsyncSocket client_socket, size_t payload_size) {
    std::string buffer;
    buffer.reserve(payload_size);
    while (true) {
        size_t n = co_await read_exact(client_socket, buffer, payload_size);
        if (n == 0) break;
        co_await client_socket.write(buffer);
    }
    co_return;
}

inline Task<void> echo_server(uint16_t port, std::atomic<bool>* started, 
                               std::atomic<bool>* stop_flag, size_t payload_size) {
    AsyncSocket listener;
    listener.create();
    listener.set_reuse_addr();
    listener.bind("127.0.0.1", port);
    listener.listen();
    started->store(true, std::memory_order_release);
    
    while (!stop_flag->load(std::memory_order_acquire)) {
        try {
            auto client = co_await listener.accept();
            zlcoro::fire_and_forget(echo_session(std::move(client), payload_size));
        } catch (...) { 
            break; 
        }
    }
    
    listener.close();
    co_return;
}

inline Task<uint64_t> echo_client(uint16_t port, size_t payload_size, size_t message_count) {
    AsyncSocket socket;
    std::string payload(payload_size, 'x');
    std::string buffer;
    uint64_t total = 0;
    
    co_await socket.connect("127.0.0.1", port);
    
    for (size_t i = 0; i < message_count; ++i) {
        co_await socket.write(payload);
        size_t n = co_await read_exact(socket, buffer, payload_size);
        total += n;
    }
    
    socket.close();
    co_return total;
}

// ============================================================================
// 测试结果数据结构
// ============================================================================

struct BenchmarkResult {
    size_t clients;
    size_t payload_size;
    size_t message_count;
    double total_mb;
    double elapsed_seconds;
    double throughput_mbps;
    uint64_t messages_per_second;
    
    void print(const std::string& title) const {
        std::cout << title << "\n";
        std::cout << "  并发数: " << clients << "\n";
        std::cout << "  负载: " << payload_size << " bytes\n";
        std::cout << "  消息数: " << message_count << " × " << clients << " = " << (message_count * clients) << "\n";
        std::cout << "  总数据: " << std::fixed << std::setprecision(2) << total_mb << " MB\n";
        std::cout << "  耗时: " << std::fixed << std::setprecision(4) << elapsed_seconds << " 秒\n";
        std::cout << "  吞吐量: " << std::fixed << std::setprecision(2) << throughput_mbps << " MB/s\n";
        std::cout << "  消息速率: " << messages_per_second << " msg/s\n";
        std::cout << std::endl;
    }
};

// ============================================================================
// 协程测试运行器
// ============================================================================

inline BenchmarkResult run_coroutine_benchmark(size_t client_count, size_t payload_size, 
                                                size_t message_count, bool verbose = false) {
    std::atomic<bool> server_started{false};
    std::atomic<bool> stop_server{false};
    const uint16_t port = pick_port();
    
    if (verbose) {
        std::cout << "启动协程 echo 服务器 (端口 " << port << ")..." << std::flush;
    }
    
    auto server_future = zlcoro::async_run(echo_server(port, &server_started, &stop_server, payload_size));
    
    while (!server_started.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    
    std::vector<std::future<uint64_t>> futures;
    futures.reserve(client_count);
    
    ScopedTimer timer;
    
    if (verbose) {
        std::cout << " 完成\n启动 " << client_count << " 个客户端..." << std::flush;
    }
    
    for (size_t i = 0; i < client_count; ++i) {
        futures.emplace_back(zlcoro::async_run(echo_client(port, payload_size, message_count)));
    }
    
    uint64_t total_bytes = 0;
    for (auto& fut : futures) {
        total_bytes += fut.get();
    }
    
    stop_server.store(true, std::memory_order_release);
    
    // 使用同步短连接唤醒 accept，避免额外协程持有栈捕获
    try {
        int wake_fd = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = inet_addr("127.0.0.1");
        addr.sin_port = htons(port);
        ::connect(wake_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
        ::close(wake_fd);
    } catch (...) {}
    
    server_future.wait();
    
    double seconds = timer.elapsed_seconds();
    double mb = static_cast<double>(total_bytes) / (1024.0 * 1024.0);
    double throughput = mb / seconds;
    uint64_t msg_rate = static_cast<uint64_t>((client_count * message_count) / seconds);
    
    if (verbose) {
        std::cout << " 完成\n";
    }
    
    return BenchmarkResult{client_count, payload_size, message_count, mb, seconds, throughput, msg_rate};
}

} // namespace io_bench
