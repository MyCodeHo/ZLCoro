/**
 * @file echo_server.cpp
 * @brief Echo 服务器示例 - 演示 ZLCoro TCP 网络编程
 * 
 * 本示例展示如何使用 ZLCoro 框架构建高性能 Echo 服务器：
 * - 使用 TcpListener 直接处理连接
 * - 使用 EventLoop 管理 I/O 事件
 * - 支持优雅关闭（Ctrl+C）
 * 
 * 测试方法：
 *   # 启动服务器
 *   ./echo_server
 *   
 *   # 使用 telnet 或 nc 测试
 *   telnet 127.0.0.1 8888
 *   nc 127.0.0.1 8888
 *   
 *   # 或使用 echo 命令测试
 *   echo "Hello, ZLCoro!" | nc 127.0.0.1 8888
 */

#include "zlcoro/zlcoro.hpp"
#include "zlcoro/net/tcp.hpp"
#include "zlcoro/io/event_loop.hpp"
#include "zlcoro/scheduler/async.hpp"

#include <iostream>
#include <string>
#include <csignal>
#include <atomic>
#include <vector>
#include <memory>
#include <sys/socket.h>

using namespace zlcoro;

// ============================================================================
// 全局状态
// ============================================================================

/// @brief 服务器运行标志
std::atomic<bool> g_running{true};

/// @brief 监听 socket（全局用于信号处理中关闭）
int g_listen_fd = -1;

// ============================================================================
// 信号处理
// ============================================================================

void signal_handler(int sig) {
    if (sig == SIGINT || sig == SIGTERM) {
        std::cout << "\n[Server] 收到关闭信号...\n";
        g_running.store(false, std::memory_order_release);
        
        // 关闭监听 socket 以中断 accept
        if (g_listen_fd >= 0) {
            shutdown(g_listen_fd, SHUT_RDWR);
        }
    }
}

// ============================================================================
// 连接处理
// ============================================================================

/**
 * @brief 处理单个客户端连接
 */
Task<void> handle_connection(TcpConnection conn) {
    try {
        auto [ip, port] = conn.peer_address();
        std::cout << "[Server] 新连接: " << ip << ":" << port << "\n";
        
        // 发送欢迎消息
        co_await conn.write_line("Welcome to ZLCoro Echo Server!");
        co_await conn.write_line("Type 'quit' to disconnect, 'shutdown' to stop server.");
        co_await conn.write_line("----------------------------------------");
        
        // Echo 循环
        while (conn.is_open() && g_running.load()) {
            std::string line = co_await conn.read_line();
            
            if (line.empty() && !conn.is_open()) {
                break;
            }
            
            if (line == "quit") {
                co_await conn.write_line("Goodbye!");
                // 退出服务器（与 shutdown 一致的收尾流程）
                g_running.store(false);
                if (g_listen_fd >= 0) {
                    shutdown(g_listen_fd, SHUT_RDWR);
                }
                break;
            }
            
            if (line == "shutdown") {
                co_await conn.write_line("Server shutting down...");
                g_running.store(false);
                // 唤醒 accept，使其尽快返回
                if (g_listen_fd >= 0) {
                    shutdown(g_listen_fd, SHUT_RDWR);
                }
                break;
            }
            
            co_await conn.write_line("Echo: " + line);
        }
        
        std::cout << "[Server] 连接关闭: " << ip << ":" << port << "\n";
        
    } catch (const std::exception& e) {
        // 连接异常，静默处理（可能是客户端断开）
    }
    
    co_return;
}

// ============================================================================
// 服务器主函数
// ============================================================================

/**
 * @brief 服务器 accept 循环
 */
Task<void> accept_loop(TcpListener& listener) {
    std::cout << "[Server] 开始接受连接...\n";

    // 跟踪所有连接协程，退出时等待它们收尾
    std::vector<std::future<void>> client_futures;

    while (g_running.load()) {
        try {
            auto conn = co_await listener.accept();

            if (!g_running.load()) {
                break;
            }

            // 使用 async_run 启动连接处理协程，future 用于生命周期管理
            client_futures.emplace_back(async_run(handle_connection(std::move(conn))));

        } catch (const std::exception& e) {
            if (g_running.load()) {
                std::cerr << "[Server] Accept 错误: " << e.what() << "\n";
            }
            break;
        }
    }

    // 等待所有连接协程结束，避免 EventLoop 停止时仍在运行导致崩溃
    for (auto& fut : client_futures) {
        try {
            fut.wait();
        } catch (...) {
            // 忽略单个连接错误，继续收尾
        }
    }

    std::cout << "[Server] Accept 循环结束\n";
    co_return;
}

// ============================================================================
// Main
// ============================================================================

int main(int argc, char* argv[]) {
    // 解析命令行参数
    std::string host = "0.0.0.0";
    uint16_t port = 8888;
    
    if (argc >= 2) {
        port = static_cast<uint16_t>(std::stoi(argv[1]));
    }
    if (argc >= 3) {
        host = argv[2];
    }
    
    // 设置信号处理
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);
    
    std::cout << "╔════════════════════════════════════════════╗\n";
    std::cout << "║     ZLCoro Echo Server v0.9.0              ║\n";
    std::cout << "╠════════════════════════════════════════════╣\n";
    std::cout << "║  监听地址: " << host << ":" << port << "                    ║\n";
    std::cout << "║  按 Ctrl+C 关闭服务器                       ║\n";
    std::cout << "╚════════════════════════════════════════════╝\n";
    
    try {
        // 创建监听器
        TcpListener listener;
        listener.listen(host, port);
        
        // 保存 listen fd 以便信号处理中关闭
        g_listen_fd = listener.socket().fd();
        
        // 在独立线程中运行 EventLoop
        std::thread event_loop_thread([]() {
            EventLoop::instance().run();
        });
        
        // 启动 accept 协程
        auto server_future = async_run(accept_loop(listener));
        
        // 等待服务器完成
        server_future.wait();
        
        // 停止事件循环并等待线程结束
        EventLoop::instance().stop();
        if (event_loop_thread.joinable()) {
            event_loop_thread.join();
        }
        
        // 清理
        listener.close();
        g_listen_fd = -1;
        
        std::cout << "[Server] 已关闭\n";
        
    } catch (const std::exception& e) {
        std::cerr << "[Server] 致命错误: " << e.what() << "\n";
        return 1;
    }
    
    return 0;
}
