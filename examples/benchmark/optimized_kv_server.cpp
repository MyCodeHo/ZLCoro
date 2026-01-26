/**
 * 优化版 KV 服务器 - 目标百万 QPS
 */

#include "zlcoro/runtime/optimized_kv_server.hpp"
#include <unordered_map>
#include <string>
#include <mutex>
#include <cstdio>
#include <csignal>
#include <unistd.h>

using namespace zlcoro;

// 线程本地 KV 存储 - 避免锁竞争
thread_local std::unordered_map<std::string, std::string> local_kv_store;

std::atomic<bool> g_running{true};

void signal_handler(int) {
    g_running = false;
}

// 请求响应结构
struct alignas(8) KVRequest {
    uint8_t op;
    uint8_t reserved;
    uint16_t key_len;
    uint32_t value_len;
};

struct alignas(8) KVResponse {
    uint8_t status;
    uint8_t reserved;
    uint16_t reserved2;
    uint32_t value_len;
};

// Epoll 处理器
Task<void> handle_epoll_connection(OptimizedEpollConnection& conn) {
    char buf[8192];
    
    while (g_running) {
        // 读取请求头
        ssize_t n = co_await conn.read(buf, sizeof(KVRequest));
        if (n <= 0) break;
        
        KVRequest* req = reinterpret_cast<KVRequest*>(buf);
        uint16_t key_len = req->key_len;
        uint32_t value_len = req->value_len;
        
        // 读取 key
        if (key_len > 0) {
            n = co_await conn.read(buf + sizeof(KVRequest), key_len);
            if (n <= 0) break;
        }
        std::string key(buf + sizeof(KVRequest), key_len);
        
        // 准备响应
        KVResponse resp{};
        std::string result_value;
        
        if (req->op == 0) {  // GET
            auto it = local_kv_store.find(key);
            if (it != local_kv_store.end()) {
                resp.status = 0;
                result_value = it->second;
                resp.value_len = static_cast<uint32_t>(result_value.size());
            } else {
                resp.status = 1;  // NOT_FOUND
            }
        } else {  // SET
            // 读取 value
            std::string value;
            if (value_len > 0) {
                value.resize(value_len);
                n = co_await conn.read(value.data(), value_len);
                if (n <= 0) break;
            }
            local_kv_store[key] = std::move(value);
            resp.status = 0;
        }
        
        // 发送响应头
        n = co_await conn.write(&resp, sizeof(resp));
        if (n <= 0) break;
        
        // 发送值（如果有）
        if (resp.value_len > 0) {
            n = co_await conn.write(result_value.data(), result_value.size());
            if (n <= 0) break;
        }
    }
}

#ifdef ZLCORO_HAS_IO_URING

// io_uring 处理器
Task<void> handle_iouring_connection(OptimizedIoUringConnection& conn) {
    char buf[8192];
    
    while (g_running) {
        ssize_t n = co_await conn.read(buf, sizeof(KVRequest));
        if (n <= 0) break;
        
        KVRequest* req = reinterpret_cast<KVRequest*>(buf);
        uint16_t key_len = req->key_len;
        uint32_t value_len = req->value_len;
        
        if (key_len > 0) {
            n = co_await conn.read(buf + sizeof(KVRequest), key_len);
            if (n <= 0) break;
        }
        std::string key(buf + sizeof(KVRequest), key_len);
        
        KVResponse resp{};
        std::string result_value;
        
        if (req->op == 0) {
            auto it = local_kv_store.find(key);
            if (it != local_kv_store.end()) {
                resp.status = 0;
                result_value = it->second;
                resp.value_len = static_cast<uint32_t>(result_value.size());
            } else {
                resp.status = 1;
            }
        } else {
            std::string value;
            if (value_len > 0) {
                value.resize(value_len);
                n = co_await conn.read(value.data(), value_len);
                if (n <= 0) break;
            }
            local_kv_store[key] = std::move(value);
            resp.status = 0;
        }
        
        n = co_await conn.write(&resp, sizeof(resp));
        if (n <= 0) break;
        
        if (resp.value_len > 0) {
            n = co_await conn.write(result_value.data(), result_value.size());
            if (n <= 0) break;
        }
    }
}

#endif

void print_usage(const char* prog) {
    printf("用法: %s [选项]\n", prog);
    printf("选项:\n");
    printf("  -p <port>    监听端口 (默认: 12345)\n");
    printf("  -c <cores>   核心数 (默认: CPU核数)\n");
    printf("  -u           使用 io_uring 后端\n");
    printf("  -h           显示帮助\n");
}

int main(int argc, char* argv[]) {
    uint16_t port = 12345;
    int num_cores = std::thread::hardware_concurrency();
    bool use_iouring = false;
    
    int opt;
    while ((opt = getopt(argc, argv, "p:c:uh")) != -1) {
        switch (opt) {
            case 'p': port = std::stoi(optarg); break;
            case 'c': num_cores = std::stoi(optarg); break;
            case 'u': use_iouring = true; break;
            case 'h':
            default:
                print_usage(argv[0]);
                return opt == 'h' ? 0 : 1;
        }
    }
    
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    signal(SIGPIPE, SIG_IGN);
    
    printf("===========================================\n");
    printf("优化版 KV 服务器\n");
    printf("===========================================\n");
    printf("端口: %d\n", port);
    printf("核心数: %d\n", num_cores);
    printf("后端: %s\n", use_iouring ? "io_uring" : "epoll");
    printf("===========================================\n");
    
    if (use_iouring) {
#ifdef ZLCORO_HAS_IO_URING
        OptimizedIoUringKVServer server(num_cores, 4096);
        if (!server.listen(port)) {
            fprintf(stderr, "监听失败\n");
            return 1;
        }
        server.set_handler(handle_iouring_connection);
        printf("服务器启动在 :%d (io_uring, %zu 核心)\n", port, server.num_cores());
        server.start();
        server.join();
#else
        fprintf(stderr, "io_uring 不可用\n");
        return 1;
#endif
    } else {
        OptimizedEpollKVServer server(num_cores);
        if (!server.listen(port)) {
            fprintf(stderr, "监听失败\n");
            return 1;
        }
        server.set_handler(handle_epoll_connection);
        printf("服务器启动在 :%d (epoll, %zu 核心)\n", port, server.num_cores());
        server.start();
        server.join();
    }
    
    printf("\n服务器已停止\n");
    return 0;
}
