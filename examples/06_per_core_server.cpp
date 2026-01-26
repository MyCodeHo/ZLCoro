/**
 * @file 06_per_core_server.cpp
 * @brief 每核心事件循环示例 - 高性能 Echo 服务器
 * 
 * 演示：
 * - 每个 CPU 核心运行独立的事件循环
 * - 每个网络连接由一个协程服务
 * - 支持 epoll 和 io_uring 两种后端
 * - 完全无锁设计
 * 
 * 编译：
 *   g++ -std=c++20 -O2 -o 06_per_core_server 06_per_core_server.cpp -luring -pthread
 * 
 * 测试：
 *   # 启动服务器
 *   ./06_per_core_server [epoll|io_uring] [port] [num_cores]
 *   
 *   # 客户端测试
 *   echo "Hello" | nc localhost 8080
 *   
 *   # 压力测试
 *   wrk -t4 -c100 -d10s http://localhost:8080/
 */

#include <iostream>
#include <cstring>
#include <csignal>

#include "zlcoro/runtime/per_core.hpp"

using namespace zlcoro;

// 全局运行时指针（用于信号处理）
std::unique_ptr<PerCoreRuntime> g_runtime;

// 信号处理
void signal_handler(int sig) {
    std::cout << "\nReceived signal " << sig << ", stopping...\n";
    if (g_runtime) {
        g_runtime->stop();
    }
}

// Echo 连接处理器
Task<void> echo_handler(PerCoreConnection& conn) {
    std::cout << "[Core " << conn.loop().core_id() 
              << "] New connection fd=" << conn.fd() << "\n";
    
    char buffer[4096];
    size_t total_bytes = 0;
    
    while (!conn.is_closed()) {
        // 读取数据
        ssize_t n = co_await conn.read(buffer, sizeof(buffer));
        
        if (n <= 0) {
            if (n < 0) {
                std::cerr << "[Core " << conn.loop().core_id() 
                          << "] Read error: " << strerror(errno) << "\n";
            }
            break;
        }
        
        total_bytes += n;
        
        // 回显数据
        ssize_t written = co_await conn.write(buffer, n);
        if (written < 0) {
            std::cerr << "[Core " << conn.loop().core_id() 
                      << "] Write error: " << strerror(errno) << "\n";
            break;
        }
    }
    
    std::cout << "[Core " << conn.loop().core_id() 
              << "] Connection closed, total bytes: " << total_bytes << "\n";
}

// HTTP 连接处理器（简单示例）
Task<void> http_handler(PerCoreConnection& conn) {
    char buffer[4096];
    
    // 读取请求
    ssize_t n = co_await conn.read(buffer, sizeof(buffer) - 1);
    if (n <= 0) {
        co_return;
    }
    buffer[n] = '\0';
    
    // 构造响应
    const char* body = "Hello from ZLCoro Per-Core Server!\n";
    char response[512];
    int len = snprintf(response, sizeof(response),
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/plain\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n"
        "\r\n"
        "%s",
        strlen(body), body);
    
    // 发送响应
    co_await conn.write(response, len);
}

void print_usage(const char* program) {
    std::cout << "Usage: " << program << " [backend] [port] [num_cores]\n"
              << "  backend:   'epoll' or 'io_uring' (default: io_uring)\n"
              << "  port:      listening port (default: 8080)\n"
              << "  num_cores: number of cores to use (default: auto)\n"
              << "\nExamples:\n"
              << "  " << program << "                    # io_uring, port 8080, all cores\n"
              << "  " << program << " epoll 9000 4       # epoll, port 9000, 4 cores\n"
              << "  " << program << " io_uring 8080 8    # io_uring, port 8080, 8 cores\n";
}

int main(int argc, char* argv[]) {
    // 解析参数
    std::string backend = "io_uring";
    uint16_t port = 8080;
    size_t num_cores = 0;  // 0 = 自动检测
    
    if (argc >= 2) {
        if (std::string(argv[1]) == "-h" || std::string(argv[1]) == "--help") {
            print_usage(argv[0]);
            return 0;
        }
        backend = argv[1];
    }
    if (argc >= 3) {
        port = static_cast<uint16_t>(std::stoi(argv[2]));
    }
    if (argc >= 4) {
        num_cores = std::stoul(argv[3]);
    }
    
    // 设置信号处理
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    signal(SIGPIPE, SIG_IGN);
    
    try {
        // 创建运行时
        std::cout << "Creating " << backend << " runtime with " 
                  << (num_cores == 0 ? std::thread::hardware_concurrency() : num_cores)
                  << " cores...\n";
        
#ifdef ZLCORO_HAS_IO_URING
        if (backend == "io_uring") {
            g_runtime = make_io_uring_runtime(num_cores);
        } else
#endif
        {
            g_runtime = make_epoll_runtime(num_cores);
        }
        
        // 创建 TCP 服务器
        PerCoreTcpServer server(*g_runtime);
        
        if (!server.listen(port)) {
            std::cerr << "Failed to listen on port " << port << ": " 
                      << strerror(errno) << "\n";
            return 1;
        }
        
        std::cout << "Listening on port " << port << "...\n";
        
        // 设置连接处理器
        server.set_handler(echo_handler);
        // server.set_handler(http_handler);  // 使用 HTTP 处理器
        
        // 启动运行时
        g_runtime->start();
        
        // 在所有核心上接受连接
        server.start_accept_all_cores();
        
        std::cout << "Server started. Press Ctrl+C to stop.\n";
        std::cout << "Backend: " << backend << "\n";
        std::cout << "Cores: " << g_runtime->num_cores() << "\n";
        std::cout << "Architecture: Per-Core EventLoop (lock-free)\n";
        
        // 等待结束
        g_runtime->wait();
        
        std::cout << "Server stopped.\n";
        
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }
    
    return 0;
}
