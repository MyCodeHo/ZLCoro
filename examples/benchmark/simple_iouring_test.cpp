/**
 * 简单的 io_uring HTTP echo 服务器 - 用于调试
 */

#include "zlcoro/runtime/io_uring_per_core.hpp"
#include "zlcoro/core/task.hpp"
#include <cstdio>
#include <cstring>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>

using namespace zlcoro;

bool g_running = true;

void signal_handler(int) {
    g_running = false;
}

const char* HTTP_RESPONSE = 
    "HTTP/1.1 200 OK\r\n"
    "Content-Type: text/plain\r\n"
    "Content-Length: 13\r\n"
    "Connection: keep-alive\r\n"
    "\r\n"
    "Hello, World!";

const size_t RESPONSE_LEN = strlen(HTTP_RESPONSE);

Task<void> handle_connection(int fd, IoUringPerCoreEventLoop& loop) {
    printf("处理连接 fd=%d\n", fd);
    char buf[4096];
    
    while (g_running) {
        // 读取请求
        auto req = loop.prep_recv(fd, buf, sizeof(buf), 0);
        loop.submit();
        int n = co_await make_awaiter(req);
        
        printf("读取 %d 字节\n", n);
        
        if (n <= 0) break;
        
        // 检查是否是 HTTP 请求
        if (memmem(buf, n, "\r\n\r\n", 4)) {
            // 发送响应
            auto wreq = loop.prep_send(fd, HTTP_RESPONSE, RESPONSE_LEN, MSG_NOSIGNAL);
            loop.submit();
            int written = co_await make_awaiter(wreq);
            
            printf("写入 %d 字节\n", written);
            
            if (written != (int)RESPONSE_LEN) break;
        }
    }
    
    printf("关闭连接 fd=%d\n", fd);
    close(fd);
}

Task<void> accept_loop(int listen_fd, IoUringPerCoreEventLoop& loop) {
    printf("开始 accept 循环\n");
    
    while (g_running) {
        struct sockaddr_in addr{};
        socklen_t len = sizeof(addr);
        
        printf("等待连接...\n");
        
        auto req = loop.prep_accept(listen_fd, (struct sockaddr*)&addr, &len);
        loop.submit();
        
        int client_fd = co_await make_awaiter(req);
        
        printf("accept 返回: %d\n", client_fd);
        
        if (client_fd < 0) {
            if (!g_running) break;
            printf("accept 错误: %d\n", client_fd);
            continue;
        }
        
        fcntl(client_fd, F_SETFL, fcntl(client_fd, F_GETFL, 0) | O_NONBLOCK);
        
        int opt = 1;
        setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));
        
        printf("启动连接处理协程\n");
        
        auto task = handle_connection(client_fd, loop);
        auto h = task.handle();
        if (h && !h.done()) {
            printf("恢复连接协程\n");
            h.resume();
        }
        task.release();
    }
}

int main() {
    signal(SIGINT, signal_handler);
    signal(SIGPIPE, SIG_IGN);
    
    // 创建监听 socket
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    int on = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEPORT, &on, sizeof(on));
    
    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(8082);
    addr.sin_addr.s_addr = INADDR_ANY;
    
    bind(listen_fd, (struct sockaddr*)&addr, sizeof(addr));
    listen(listen_fd, 128);
    
    printf("服务器监听在 8082\n");
    
    IoUringPerCoreEventLoop loop(1024);
    set_current_event_loop(&loop);
    
    auto task = accept_loop(listen_fd, loop);
    auto h = task.handle();
    if (h && !h.done()) {
        printf("启动 accept 协程\n");
        h.resume();
    }
    task.release();
    
    printf("运行事件循环\n");
    loop.run();
    
    close(listen_fd);
    printf("服务器停止\n");
    
    return 0;
}
