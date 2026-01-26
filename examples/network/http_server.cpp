/**
 * @file http_server.cpp
 * @brief HTTP 服务器示例
 * 
 * 本示例演示如何使用 ZLCoro 框架实现一个简单的 HTTP 服务器。
 * 
 * 功能：
 * - GET /           返回欢迎页面
 * - GET /hello      返回 "Hello, World!"
 * - GET /json       返回 JSON 数据
 * - GET /time       返回当前时间
 * - POST /echo      回显 POST 数据
 * - GET /status     返回服务器状态
 * 
 * 编译：
 *   g++ -std=c++20 -pthread http_server.cpp -o http_server
 * 
 * 运行：
 *   ./http_server [port]
 * 
 * 测试：
 *   curl http://localhost:8080/
 *   curl http://localhost:8080/json
 *   curl -X POST -d "Hello" http://localhost:8080/echo
 */

#include "zlcoro/net/http.hpp"
#include "zlcoro/io/event_loop.hpp"
#include "zlcoro/scheduler/async.hpp"

#include <iostream>
#include <chrono>
#include <ctime>
#include <csignal>
#include <atomic>

using namespace zlcoro;

// =============================================================================
// 全局变量
// =============================================================================

/// @brief 请求计数器
std::atomic<uint64_t> request_count{0};

/// @brief 服务器启动时间
std::chrono::steady_clock::time_point start_time;

/// @brief 服务器运行标志
std::atomic<bool> g_running{true};

// =============================================================================
// 工具函数
// =============================================================================

/// @brief 获取当前时间的字符串表示
std::string get_current_time() {
    auto now = std::chrono::system_clock::now();
    std::time_t time = std::chrono::system_clock::to_time_t(now);
    
    char buffer[64];
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", std::localtime(&time));
    
    return buffer;
}

/// @brief 获取服务器运行时间
std::string get_uptime() {
    auto now = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::seconds>(now - start_time);
    
    auto hours = std::chrono::duration_cast<std::chrono::hours>(duration);
    auto minutes = std::chrono::duration_cast<std::chrono::minutes>(duration % std::chrono::hours(1));
    auto seconds = duration % std::chrono::minutes(1);
    
    std::ostringstream oss;
    oss << hours.count() << "h " << minutes.count() << "m " << seconds.count() << "s";
    return oss.str();
}

// =============================================================================
// 请求处理器
// =============================================================================

/// @brief 首页处理器
Task<HttpResponse> handle_index([[maybe_unused]] const HttpRequest& req) {
    ++request_count;
    
    std::string html = R"html(
<!DOCTYPE html>
<html>
<head>
    <title>ZLCoro HTTP Server</title>
    <style>
        body { font-family: Arial, sans-serif; margin: 40px; background: #f5f5f5; }
        h1 { color: #333; }
        .container { background: white; padding: 20px; border-radius: 8px; box-shadow: 0 2px 4px rgba(0,0,0,0.1); }
        a { color: #0066cc; text-decoration: none; margin-right: 15px; }
        a:hover { text-decoration: underline; }
        .endpoints { margin-top: 20px; }
        .endpoint { margin: 10px 0; padding: 10px; background: #f9f9f9; border-radius: 4px; }
        code { background: #e8e8e8; padding: 2px 6px; border-radius: 3px; }
    </style>
</head>
<body>
    <div class="container">
        <h1>🚀 ZLCoro HTTP Server</h1>
        <p>Welcome to the ZLCoro HTTP server example!</p>
        <p>Current time: )html" + get_current_time() + R"html(</p>
        
        <div class="endpoints">
            <h2>Available Endpoints:</h2>
            <div class="endpoint">
                <strong>GET /hello</strong> - Returns "Hello, World!"
            </div>
            <div class="endpoint">
                <strong>GET /json</strong> - Returns JSON data
            </div>
            <div class="endpoint">
                <strong>GET /time</strong> - Returns current server time
            </div>
            <div class="endpoint">
                <strong>POST /echo</strong> - Echo back POST data
            </div>
            <div class="endpoint">
                <strong>GET /status</strong> - Server status information
            </div>
        </div>
        
        <h2>Test with curl:</h2>
        <p><code>curl http://localhost:8080/hello</code></p>
        <p><code>curl http://localhost:8080/json</code></p>
        <p><code>curl -X POST -d "Hello" http://localhost:8080/echo</code></p>
    </div>
</body>
</html>
)html";
    
    co_return HttpResponse::html(std::move(html));
}

/// @brief Hello 处理器
Task<HttpResponse> handle_hello([[maybe_unused]] const HttpRequest& req) {
    ++request_count;
    co_return HttpResponse::ok("Hello, World!\n");
}

/// @brief JSON 处理器
Task<HttpResponse> handle_json([[maybe_unused]] const HttpRequest& req) {
    ++request_count;
    
    std::ostringstream oss;
    oss << R"({
    "message": "Hello from ZLCoro",
    "version": "0.9.0",
    "time": ")" << get_current_time() << R"(",
    "request_count": )" << request_count.load() << R"(,
    "features": [
        "coroutines",
        "async I/O",
        "scheduler",
        "http server"
    ]
})";
    
    co_return HttpResponse::json(oss.str());
}

/// @brief 时间处理器
Task<HttpResponse> handle_time([[maybe_unused]] const HttpRequest& req) {
    ++request_count;
    
    std::ostringstream oss;
    oss << R"({
    "current_time": ")" << get_current_time() << R"(",
    "uptime": ")" << get_uptime() << R"("
})";
    
    co_return HttpResponse::json(oss.str());
}

/// @brief Echo 处理器
Task<HttpResponse> handle_echo(const HttpRequest& req) {
    ++request_count;
    
    std::ostringstream oss;
    oss << R"({
    "method": ")" << http::method_string(req.method()) << R"(",
    "path": ")" << req.path() << R"(",
    "content_type": ")" << req.content_type() << R"(",
    "content_length": )" << req.body().size() << R"(,
    "body": ")" << req.body() << R"("
})";
    
    co_return HttpResponse::json(oss.str());
}

/// @brief 状态处理器
Task<HttpResponse> handle_status([[maybe_unused]] const HttpRequest& req) {
    ++request_count;
    
    std::ostringstream oss;
    oss << R"({
    "status": "running",
    "uptime": ")" << get_uptime() << R"(",
    "request_count": )" << request_count.load() << R"(,
    "version": "0.9.0"
})";
    
    co_return HttpResponse::json(oss.str());
}

// =============================================================================
// 信号处理
// =============================================================================

void signal_handler(int sig) {
    if (sig == SIGINT || sig == SIGTERM) {
        std::cout << "\n[HTTP Server] 收到关闭信号...\n";
        g_running.store(false);
        EventLoop::instance().stop();
    }
}

// =============================================================================
// 服务器主函数
// =============================================================================

Task<void> run_server(uint16_t port) {
    HttpServer server;
    
    // 注册路由
    server.get("/", handle_index);
    server.get("/hello", handle_hello);
    server.get("/json", handle_json);
    server.get("/time", handle_time);
    server.post("/echo", handle_echo);
    server.get("/status", handle_status);
    
    // 设置自定义 404 处理器
    server.set_not_found_handler([](const HttpRequest& req) -> Task<HttpResponse> {
        std::string html = R"html(
<!DOCTYPE html>
<html>
<head>
    <title>404 Not Found</title>
    <style>
        body { font-family: Arial, sans-serif; margin: 40px; text-align: center; }
        h1 { font-size: 72px; margin: 0; color: #666; }
        p { color: #999; }
        a { color: #0066cc; }
    </style>
</head>
<body>
    <h1>404</h1>
    <p>The page <strong>)html" + req.path() + R"html(</strong> was not found.</p>
    <p><a href="/">Go to homepage</a></p>
</body>
</html>
)html";
        co_return HttpResponse::html(std::move(html)).set_status(http::StatusCode::NotFound);
    });
    
    std::cout << "HTTP Server listening on http://0.0.0.0:" << port << std::endl;
    std::cout << "Press Ctrl+C to stop\n" << std::endl;
    std::cout << "Try these commands:\n";
    std::cout << "  curl http://localhost:" << port << "/\n";
    std::cout << "  curl http://localhost:" << port << "/hello\n";
    std::cout << "  curl http://localhost:" << port << "/json\n";
    std::cout << "  curl -X POST -d \"Hello\" http://localhost:" << port << "/echo\n";
    std::cout << std::endl;
    
    co_await server.serve("0.0.0.0", port);
}

// =============================================================================
// 主函数
// =============================================================================

int main(int argc, char* argv[]) {
    // 记录启动时间
    start_time = std::chrono::steady_clock::now();
    
    // 解析端口参数
    uint16_t port = 8080;
    if (argc > 1) {
        port = static_cast<uint16_t>(std::stoi(argv[1]));
    }
    
    // 设置信号处理
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);
    
    std::cout << "======================================" << std::endl;
    std::cout << "   ZLCoro HTTP Server Example" << std::endl;
    std::cout << "======================================\n" << std::endl;
    
    try {
        // 在独立线程中运行 EventLoop
        std::thread event_loop_thread([]() {
            EventLoop::instance().run();
        });
        
        // 启动服务器协程
        auto server_future = async_run(run_server(port));
        
        // 等待服务器完成
        server_future.wait();
        
        // 停止事件循环并等待线程结束
        if (event_loop_thread.joinable()) {
            event_loop_thread.join();
        }
        
    } catch (const std::exception& e) {
        std::cerr << "[HTTP Server] 致命错误: " << e.what() << "\n";
        return 1;
    }
    
    std::cout << "\nServer stopped." << std::endl;
    
    return 0;
}
