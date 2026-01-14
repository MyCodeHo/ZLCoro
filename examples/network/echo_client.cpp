/**
 * @file echo_client.cpp
 * @brief Echo 客户端示例 - 演示 ZLCoro TCP 客户端编程
 * 
 * 本示例展示如何使用 ZLCoro 框架构建 TCP 客户端：
 * - 使用 AsyncSocket 连接服务器
 * - 使用 TcpConnection 进行读写
 * - 使用 EventLoop 处理异步 I/O
 * 
 * 使用方法：
 *   ./echo_client [host] [port]
 *   ./echo_client 127.0.0.1 8888
 */

#include "zlcoro/zlcoro.hpp"
#include "zlcoro/net/tcp.hpp"
#include "zlcoro/io/event_loop.hpp"

#include <iostream>
#include <string>
#include <thread>
#include <atomic>

using namespace zlcoro;

// ============================================================================
// 全局状态
// ============================================================================

std::atomic<bool> g_done{false};

// ============================================================================
// 客户端主逻辑
// ============================================================================

/**
 * @brief 运行 Echo 客户端
 */
Task<void> run_client(const std::string& host, uint16_t port) {
    std::cout << "[Client] 连接到 " << host << ":" << port << "...\n";
    
    try {
        // 创建 socket 并连接
        AsyncSocket socket;
        socket.create();
        
        co_await socket.connect(host, port);
        
        std::cout << "[Client] 已连接!\n";
        std::cout << "[Client] 输入消息，按 Enter 发送。输入 'quit' 退出。\n";
        std::cout << "----------------------------------------\n";
        
        // 包装为 TcpConnection
        TcpConnection conn(std::move(socket));
        
        // 读取服务器欢迎消息
        for (int i = 0; i < 3; ++i) {
            std::string welcome = co_await conn.read_line();
            std::cout << "[Server] " << welcome << "\n";
        }
        
        // 交互循环
        std::string input;
        while (conn.is_open()) {
            std::cout << "> ";
            std::cout.flush();
            
            if (!std::getline(std::cin, input)) {
                break;
            }
            
            if (input.empty()) {
                continue;
            }
            
            // 发送消息
            co_await conn.write_line(input);
            
            // 读取响应
            std::string response = co_await conn.read_line();
            std::cout << "[Server] " << response << "\n";
            
            if (input == "quit" || input == "shutdown") {
                break;
            }
        }
        
        std::cout << "[Client] 断开连接\n";
        
    } catch (const std::exception& e) {
        std::cerr << "[Client] 错误: " << e.what() << "\n";
    }
    
    g_done.store(true);
    EventLoop::instance().stop();
    co_return;
}

// ============================================================================
// Main
// ============================================================================

int main(int argc, char* argv[]) {
    // 解析命令行参数
    std::string host = "127.0.0.1";
    uint16_t port = 8888;
    
    if (argc >= 2) {
        host = argv[1];
    }
    if (argc >= 3) {
        port = static_cast<uint16_t>(std::stoi(argv[2]));
    }
    
    try {
        std::cout << "╔════════════════════════════════════════════╗\n";
        std::cout << "║     ZLCoro Echo Client v0.9.0              ║\n";
        std::cout << "╚════════════════════════════════════════════╝\n";
        
        // 启动客户端协程（必须保持 client_task 存活！）
        auto client_task = run_client(host, port);
        auto handle = client_task.handle();
        if (handle && !handle.done()) {
            handle.resume();
        }
        
        // 运行事件循环（阻塞直到协程完成调用 stop()）
        // 注意：client_task 必须在这里保持存活，否则协程帧会被销毁
        EventLoop::instance().run();
        
    } catch (const std::exception& e) {
        std::cerr << "[Client] 致命错误: " << e.what() << "\n";
        return 1;
    }
    
    return 0;
}
