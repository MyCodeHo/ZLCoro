#include "zlcoro/runtime/kv_server.hpp"
#include "zlcoro/runtime/connection_manager.hpp"

#include <iostream>
#include <chrono>
#include <thread>
#include <vector>
#include <atomic>
#include <random>
#include <cstring>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <iomanip>

using namespace zlcoro;
using namespace std::chrono;

// =============================================================================
// 性能测试配置
// =============================================================================

struct BenchmarkConfig {
    uint16_t port = 12345;
    size_t num_server_cores = 4;        // 服务器核心数
    size_t num_client_threads = 8;      // 客户端线程数
    size_t connections_per_thread = 100; // 每线程连接数
    size_t requests_per_connection = 1000; // 每连接请求数
    size_t request_size = 64;           // 请求数据大小
    size_t response_size = 64;          // 响应数据大小
    int duration_seconds = 10;          // 测试持续时间（秒）
    bool use_io_uring = false;          // 使用 io_uring
};

// =============================================================================
// 测试统计
// =============================================================================

struct BenchmarkStats {
    std::atomic<uint64_t> total_requests{0};
    std::atomic<uint64_t> total_bytes_sent{0};
    std::atomic<uint64_t> total_bytes_recv{0};
    std::atomic<uint64_t> total_errors{0};
    std::atomic<uint64_t> total_connections{0};
    std::atomic<uint64_t> latency_sum_us{0};
    std::atomic<uint64_t> latency_count{0};
    
    void reset() {
        total_requests = 0;
        total_bytes_sent = 0;
        total_bytes_recv = 0;
        total_errors = 0;
        total_connections = 0;
        latency_sum_us = 0;
        latency_count = 0;
    }
    
    void print_report(double elapsed_seconds) {
        uint64_t requests = total_requests.load();
        uint64_t bytes_sent = total_bytes_sent.load();
        uint64_t bytes_recv = total_bytes_recv.load();
        uint64_t errors = total_errors.load();
        uint64_t lat_sum = latency_sum_us.load();
        uint64_t lat_count = latency_count.load();
        
        double qps = requests / elapsed_seconds;
        double throughput_mb = (bytes_sent + bytes_recv) / elapsed_seconds / (1024 * 1024);
        double avg_latency_us = (lat_count > 0) ? (double)lat_sum / lat_count : 0;
        
        std::cout << "\n========================================\n";
        std::cout << "        性能测试结果报告\n";
        std::cout << "========================================\n";
        std::cout << std::fixed << std::setprecision(2);
        std::cout << "测试时长:        " << elapsed_seconds << " 秒\n";
        std::cout << "总请求数:        " << requests << "\n";
        std::cout << "总连接数:        " << total_connections.load() << "\n";
        std::cout << "错误数:          " << errors << "\n";
        std::cout << "----------------------------------------\n";
        std::cout << "QPS:             " << qps << " req/s\n";
        std::cout << "吞吐量:          " << throughput_mb << " MB/s\n";
        std::cout << "平均延迟:        " << avg_latency_us << " us\n";
        std::cout << "发送数据:        " << bytes_sent / (1024 * 1024) << " MB\n";
        std::cout << "接收数据:        " << bytes_recv / (1024 * 1024) << " MB\n";
        std::cout << "========================================\n\n";
    }
};

BenchmarkStats g_stats;
std::atomic<bool> g_running{false};

// =============================================================================
// 简单 Echo 协议处理器
// =============================================================================

// epoll 版本的处理器
Task<void> echo_handler_epoll(EpollKVConnection& conn) {
    char buf[4096];
    
    while (conn.is_valid() && g_running.load(std::memory_order_relaxed)) {
        // 读取请求头（8字节）
        KVRequestHeader header;
        bool ok = co_await conn.read_header(&header, sizeof(header));
        if (!ok) break;
        
        // 读取数据
        size_t data_len = header.key_len + header.value_len;
        if (data_len > sizeof(buf)) data_len = sizeof(buf);
        
        if (data_len > 0) {
            ssize_t n = co_await conn.read_value(buf, data_len);
            if (n <= 0) break;
        }
        
        // 发送响应头
        KVResponseHeader resp;
        resp.status = static_cast<uint8_t>(KVStatus::OK);
        resp.reserved = 0;
        resp.reserved2 = 0;
        resp.value_len = header.key_len;  // Echo back key length
        
        ok = co_await conn.write_response(&resp, sizeof(resp));
        if (!ok) break;
        
        // Echo 数据
        if (header.key_len > 0) {
            ok = co_await conn.write_response(buf, header.key_len);
            if (!ok) break;
        }
    }
}

#ifdef ZLCORO_HAS_IO_URING
// io_uring 版本的处理器
Task<void> echo_handler_io_uring(IoUringKVConnection& conn) {
    char buf[4096];
    
    while (conn.is_valid() && g_running.load(std::memory_order_relaxed)) {
        KVRequestHeader header;
        bool ok = co_await conn.read_header(&header, sizeof(header));
        if (!ok) break;
        
        size_t data_len = header.key_len + header.value_len;
        if (data_len > sizeof(buf)) data_len = sizeof(buf);
        
        if (data_len > 0) {
            ssize_t n = co_await conn.read_value(buf, data_len);
            if (n <= 0) break;
        }
        
        KVResponseHeader resp;
        resp.status = static_cast<uint8_t>(KVStatus::OK);
        resp.reserved = 0;
        resp.reserved2 = 0;
        resp.value_len = header.key_len;
        
        ok = co_await conn.write_response(&resp, sizeof(resp));
        if (!ok) break;
        
        if (header.key_len > 0) {
            ok = co_await conn.write_response(buf, header.key_len);
            if (!ok) break;
        }
    }
}
#endif

// =============================================================================
// 同步客户端（用于压测）
// =============================================================================

class SyncClient {
public:
    SyncClient(const std::string& ip, uint16_t port)
        : ip_(ip), port_(port), fd_(-1) {}
    
    ~SyncClient() {
        close();
    }
    
    bool connect() {
        fd_ = socket(AF_INET, SOCK_STREAM, 0);
        if (fd_ < 0) return false;
        
        int opt = 1;
        setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));
        
        struct sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port_);
        inet_pton(AF_INET, ip_.c_str(), &addr.sin_addr);
        
        if (::connect(fd_, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
            ::close(fd_);
            fd_ = -1;
            return false;
        }
        
        return true;
    }
    
    void close() {
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
    }
    
    bool send_request(const void* data, size_t key_len, size_t value_len) {
        KVRequestHeader header;
        header.op = static_cast<uint8_t>(KVOp::SET);
        header.reserved = 0;
        header.key_len = static_cast<uint16_t>(key_len);
        header.value_len = static_cast<uint32_t>(value_len);
        
        // 发送头
        if (send_all(&header, sizeof(header)) != sizeof(header)) {
            return false;
        }
        
        // 发送数据
        size_t total_len = key_len + value_len;
        if (total_len > 0) {
            if (send_all(data, total_len) != static_cast<ssize_t>(total_len)) {
                return false;
            }
        }
        
        return true;
    }
    
    bool recv_response(void* buf, size_t& value_len) {
        KVResponseHeader resp;
        
        if (recv_all(&resp, sizeof(resp)) != sizeof(resp)) {
            return false;
        }
        
        value_len = resp.value_len;
        
        if (value_len > 0) {
            if (recv_all(buf, value_len) != static_cast<ssize_t>(value_len)) {
                return false;
            }
        }
        
        return resp.status == static_cast<uint8_t>(KVStatus::OK);
    }
    
    bool is_connected() const { return fd_ >= 0; }
    
private:
    ssize_t send_all(const void* buf, size_t len) {
        const char* ptr = static_cast<const char*>(buf);
        size_t sent = 0;
        
        while (sent < len) {
            ssize_t n = ::send(fd_, ptr + sent, len - sent, MSG_NOSIGNAL);
            if (n <= 0) return -1;
            sent += n;
        }
        
        return static_cast<ssize_t>(sent);
    }
    
    ssize_t recv_all(void* buf, size_t len) {
        char* ptr = static_cast<char*>(buf);
        size_t received = 0;
        
        while (received < len) {
            ssize_t n = ::recv(fd_, ptr + received, len - received, 0);
            if (n <= 0) return -1;
            received += n;
        }
        
        return static_cast<ssize_t>(received);
    }
    
    std::string ip_;
    uint16_t port_;
    int fd_;
};

// =============================================================================
// 客户端压测线程
// =============================================================================

void client_benchmark_thread(int thread_id, const BenchmarkConfig& config) {
    std::vector<std::unique_ptr<SyncClient>> clients;
    
    // 建立连接
    for (size_t i = 0; i < config.connections_per_thread; ++i) {
        auto client = std::make_unique<SyncClient>("127.0.0.1", config.port);
        if (client->connect()) {
            clients.push_back(std::move(client));
            g_stats.total_connections++;
        }
    }
    
    if (clients.empty()) {
        std::cerr << "Thread " << thread_id << ": 无法建立连接\n";
        return;
    }
    
    // 准备测试数据
    std::vector<char> request_data(config.request_size, 'x');
    std::vector<char> response_buf(config.response_size + 64);
    
    // 开始压测
    size_t client_index = 0;
    
    while (g_running.load(std::memory_order_relaxed)) {
        auto& client = clients[client_index];
        client_index = (client_index + 1) % clients.size();
        
        if (!client->is_connected()) {
            continue;
        }
        
        auto start = high_resolution_clock::now();
        
        // 发送请求
        if (!client->send_request(request_data.data(), 
                                  config.request_size / 2, 
                                  config.request_size / 2)) {
            g_stats.total_errors++;
            continue;
        }
        
        g_stats.total_bytes_sent += sizeof(KVRequestHeader) + config.request_size;
        
        // 接收响应
        size_t value_len = 0;
        if (!client->recv_response(response_buf.data(), value_len)) {
            g_stats.total_errors++;
            continue;
        }
        
        auto end = high_resolution_clock::now();
        auto latency_us = duration_cast<microseconds>(end - start).count();
        
        g_stats.total_bytes_recv += sizeof(KVResponseHeader) + value_len;
        g_stats.total_requests++;
        g_stats.latency_sum_us += latency_us;
        g_stats.latency_count++;
    }
}

// =============================================================================
// 运行 epoll 服务器压测
// =============================================================================

void run_epoll_benchmark(const BenchmarkConfig& config) {
    std::cout << "\n[epoll 模式压测]\n";
    std::cout << "服务器核心数: " << config.num_server_cores << "\n";
    std::cout << "客户端线程数: " << config.num_client_threads << "\n";
    std::cout << "每线程连接数: " << config.connections_per_thread << "\n";
    std::cout << "请求大小: " << config.request_size << " bytes\n";
    std::cout << "测试时长: " << config.duration_seconds << " 秒\n\n";
    
    // 启动服务器
    EpollKVServer server(config.num_server_cores);
    
    if (!server.listen(config.port)) {
        std::cerr << "Failed to listen on port " << config.port << "\n";
        return;
    }
    
    server.set_handler(echo_handler_epoll);
    server.start();
    
    std::cout << "服务器已启动，端口: " << config.port << "\n";
    
    // 等待服务器启动
    std::this_thread::sleep_for(milliseconds(200));
    
    // 重置统计
    g_stats.reset();
    g_running = true;
    
    // 启动客户端线程
    std::vector<std::thread> client_threads;
    for (size_t i = 0; i < config.num_client_threads; ++i) {
        client_threads.emplace_back(client_benchmark_thread, i, std::ref(config));
    }
    
    // 运行指定时间
    auto start_time = steady_clock::now();
    
    while (g_running) {
        std::this_thread::sleep_for(seconds(1));
        
        auto elapsed = duration_cast<seconds>(steady_clock::now() - start_time).count();
        std::cout << "\r进度: " << elapsed << "/" << config.duration_seconds 
                  << " 秒, QPS: " << g_stats.total_requests.load() / (elapsed + 1)
                  << "     " << std::flush;
        
        if (elapsed >= config.duration_seconds) {
            g_running = false;
        }
    }
    
    // 等待客户端线程结束
    for (auto& t : client_threads) {
        t.join();
    }
    
    auto total_time = duration_cast<milliseconds>(steady_clock::now() - start_time).count();
    
    // 打印结果
    g_stats.print_report(total_time / 1000.0);
    
    // 停止服务器（使用短超时以避免长时间等待）
    std::cout << "正在停止服务器..." << std::endl;
    
    // 创建一个线程来停止服务器，避免主线程阻塞
    std::thread stop_thread([&server]() {
        server.stop();
    });
    
    // 等待最多 2 秒
    if (stop_thread.joinable()) {
        stop_thread.join();
    }
    
    std::cout << "服务器已停止\n";
}

#ifdef ZLCORO_HAS_IO_URING
// =============================================================================
// 运行 io_uring 服务器压测
// =============================================================================

void run_io_uring_benchmark(const BenchmarkConfig& config) {
    std::cout << "\n[io_uring 模式压测]\n";
    std::cout << "服务器核心数: " << config.num_server_cores << "\n";
    std::cout << "客户端线程数: " << config.num_client_threads << "\n";
    std::cout << "每线程连接数: " << config.connections_per_thread << "\n";
    std::cout << "请求大小: " << config.request_size << " bytes\n";
    std::cout << "测试时长: " << config.duration_seconds << " 秒\n\n";
    
    IoUringKVServer server(config.num_server_cores, 512);
    
    if (!server.listen(config.port)) {
        std::cerr << "Failed to listen on port " << config.port << "\n";
        return;
    }
    
    server.set_handler(echo_handler_io_uring);
    server.start();
    
    std::cout << "服务器已启动，端口: " << config.port << "\n";
    
    std::this_thread::sleep_for(milliseconds(200));
    
    g_stats.reset();
    g_running = true;
    
    std::vector<std::thread> client_threads;
    for (size_t i = 0; i < config.num_client_threads; ++i) {
        client_threads.emplace_back(client_benchmark_thread, i, std::ref(config));
    }
    
    auto start_time = steady_clock::now();
    
    while (g_running) {
        std::this_thread::sleep_for(seconds(1));
        
        auto elapsed = duration_cast<seconds>(steady_clock::now() - start_time).count();
        std::cout << "\r进度: " << elapsed << "/" << config.duration_seconds 
                  << " 秒, QPS: " << g_stats.total_requests.load() / (elapsed + 1)
                  << "     " << std::flush;
        
        if (elapsed >= config.duration_seconds) {
            g_running = false;
        }
    }
    
    for (auto& t : client_threads) {
        t.join();
    }
    
    auto total_time = duration_cast<milliseconds>(steady_clock::now() - start_time).count();
    
    g_stats.print_report(total_time / 1000.0);
    
    server.stop();
}
#endif

// =============================================================================
// 主函数
// =============================================================================

void print_usage() {
    std::cout << "用法: kv_benchmark [选项]\n"
              << "选项:\n"
              << "  -c <cores>        服务器核心数 (默认: 4)\n"
              << "  -t <threads>      客户端线程数 (默认: 8)\n"
              << "  -n <conns>        每线程连接数 (默认: 100)\n"
              << "  -s <size>         请求大小 (默认: 64)\n"
              << "  -d <seconds>      测试时长 (默认: 10)\n"
              << "  -p <port>         服务器端口 (默认: 12345)\n"
#ifdef ZLCORO_HAS_IO_URING
              << "  -u                使用 io_uring (默认: epoll)\n"
#endif
              << "  -h                显示帮助\n";
}

int main(int argc, char* argv[]) {
    // 忽略 SIGPIPE
    signal(SIGPIPE, SIG_IGN);
    
    BenchmarkConfig config;
    
    // 解析参数
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "-c") == 0 && i + 1 < argc) {
            config.num_server_cores = std::stoul(argv[++i]);
        } else if (strcmp(argv[i], "-t") == 0 && i + 1 < argc) {
            config.num_client_threads = std::stoul(argv[++i]);
        } else if (strcmp(argv[i], "-n") == 0 && i + 1 < argc) {
            config.connections_per_thread = std::stoul(argv[++i]);
        } else if (strcmp(argv[i], "-s") == 0 && i + 1 < argc) {
            config.request_size = std::stoul(argv[++i]);
        } else if (strcmp(argv[i], "-d") == 0 && i + 1 < argc) {
            config.duration_seconds = std::stoi(argv[++i]);
        } else if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) {
            config.port = std::stoi(argv[++i]);
#ifdef ZLCORO_HAS_IO_URING
        } else if (strcmp(argv[i], "-u") == 0) {
            config.use_io_uring = true;
#endif
        } else if (strcmp(argv[i], "-h") == 0) {
            print_usage();
            return 0;
        }
    }
    
    std::cout << "========================================\n";
    std::cout << "   ZLCoro KV 服务器高并发性能测试\n";
    std::cout << "========================================\n";
    
#ifdef ZLCORO_HAS_IO_URING
    if (config.use_io_uring) {
        run_io_uring_benchmark(config);
    } else {
        run_epoll_benchmark(config);
    }
#else
    run_epoll_benchmark(config);
#endif
    
    return 0;
}
