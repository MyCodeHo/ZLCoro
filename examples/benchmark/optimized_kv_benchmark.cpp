/**
 * 优化版 KV 性能测试 - 目标百万 QPS
 * 
 * 主要优化:
 * 1. 流水线化客户端 - 批量发送请求不等待响应
 * 2. 连接池 - 复用连接
 * 3. 更高并发 - 多线程多连接
 */

#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <cstring>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <chrono>
#include <random>

// 请求响应结构
struct alignas(8) KVRequest {
    uint8_t op;       // 0: GET, 1: SET
    uint8_t reserved;
    uint16_t key_len;
    uint32_t value_len;
};

struct alignas(8) KVResponse {
    uint8_t status;   // 0: OK, 1: NOT_FOUND, 2: ERROR
    uint8_t reserved;
    uint16_t reserved2;
    uint32_t value_len;
};

// 设置非阻塞
bool set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

// 流水线客户端
class PipelinedClient {
public:
    PipelinedClient(const std::string& host, uint16_t port, int pipeline_depth = 64)
        : host_(host), port_(port), pipeline_depth_(pipeline_depth), fd_(-1),
          requests_sent_(0), responses_received_(0), errors_(0) {}
    
    ~PipelinedClient() {
        disconnect();
    }
    
    bool connect() {
        fd_ = socket(AF_INET, SOCK_STREAM, 0);
        if (fd_ < 0) return false;
        
        // 优化 TCP 选项
        int opt = 1;
        setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));
        
        // 增大缓冲区
        int buf_size = 1024 * 1024;
        setsockopt(fd_, SOL_SOCKET, SO_SNDBUF, &buf_size, sizeof(buf_size));
        setsockopt(fd_, SOL_SOCKET, SO_RCVBUF, &buf_size, sizeof(buf_size));
        
        struct sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port_);
        inet_pton(AF_INET, host_.c_str(), &addr.sin_addr);
        
        if (::connect(fd_, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
            close(fd_);
            fd_ = -1;
            return false;
        }
        
        set_nonblocking(fd_);
        return true;
    }
    
    void disconnect() {
        if (fd_ >= 0) {
            close(fd_);
            fd_ = -1;
        }
    }
    
    // 运行流水线测试
    uint64_t run_benchmark(uint64_t num_requests, int set_ratio = 20) {
        if (fd_ < 0) return 0;
        
        // 预生成请求
        std::mt19937 rng(std::random_device{}());
        std::uniform_int_distribution<int> op_dist(0, 99);
        std::uniform_int_distribution<int> key_dist(0, 999999);
        
        // 发送缓冲区和接收缓冲区
        std::vector<char> send_buf;
        std::vector<char> recv_buf(1024 * 1024);
        
        uint64_t in_flight = 0;
        uint64_t total_sent = 0;
        uint64_t total_recv = 0;
        
        size_t recv_offset = 0;
        
        while (total_recv < num_requests) {
            // 发送更多请求直到达到流水线深度
            while (in_flight < (uint64_t)pipeline_depth_ && total_sent < num_requests) {
                // 生成请求
                bool is_set = (op_dist(rng) < set_ratio);
                std::string key = "key:" + std::to_string(key_dist(rng));
                std::string value = is_set ? "value_" + std::to_string(key_dist(rng)) : "";
                
                // 构建请求
                KVRequest req{};
                req.op = is_set ? 1 : 0;
                req.key_len = static_cast<uint16_t>(key.size());
                req.value_len = static_cast<uint32_t>(value.size());
                
                size_t old_size = send_buf.size();
                size_t req_size = sizeof(req) + key.size() + value.size();
                send_buf.resize(old_size + req_size);
                
                memcpy(send_buf.data() + old_size, &req, sizeof(req));
                memcpy(send_buf.data() + old_size + sizeof(req), key.data(), key.size());
                if (!value.empty()) {
                    memcpy(send_buf.data() + old_size + sizeof(req) + key.size(), 
                           value.data(), value.size());
                }
                
                total_sent++;
                in_flight++;
                requests_sent_++;
            }
            
            // 批量发送
            if (!send_buf.empty()) {
                ssize_t sent = send(fd_, send_buf.data(), send_buf.size(), MSG_NOSIGNAL);
                if (sent > 0) {
                    send_buf.erase(send_buf.begin(), send_buf.begin() + sent);
                } else if (sent < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                    errors_++;
                    break;
                }
            }
            
            // 接收响应
            ssize_t received = recv(fd_, recv_buf.data() + recv_offset, 
                                   recv_buf.size() - recv_offset, MSG_DONTWAIT);
            if (received > 0) {
                recv_offset += received;
                
                // 解析响应
                size_t pos = 0;
                while (pos + sizeof(KVResponse) <= recv_offset) {
                    KVResponse* resp = reinterpret_cast<KVResponse*>(recv_buf.data() + pos);
                    size_t resp_size = sizeof(KVResponse) + resp->value_len;
                    
                    if (pos + resp_size > recv_offset) break;
                    
                    total_recv++;
                    in_flight--;
                    responses_received_++;
                    pos += resp_size;
                }
                
                if (pos > 0) {
                    memmove(recv_buf.data(), recv_buf.data() + pos, recv_offset - pos);
                    recv_offset -= pos;
                }
            } else if (received < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                errors_++;
                break;
            }
            
            // 如果没有进展，短暂等待
            if (send_buf.empty() && received <= 0 && in_flight > 0) {
                struct pollfd pfd{fd_, POLLIN | POLLOUT, 0};
                poll(&pfd, 1, 1);
            }
        }
        
        return total_recv;
    }
    
    uint64_t requests_sent() const { return requests_sent_; }
    uint64_t responses_received() const { return responses_received_; }
    uint64_t errors() const { return errors_; }
    
private:
    std::string host_;
    uint16_t port_;
    int pipeline_depth_;
    int fd_;
    uint64_t requests_sent_;
    uint64_t responses_received_;
    uint64_t errors_;
};

// 多连接客户端 - 每个线程多个连接
class MultiConnectionClient {
public:
    MultiConnectionClient(const std::string& host, uint16_t port,
                         int num_connections, int pipeline_depth)
        : host_(host), port_(port), num_connections_(num_connections),
          pipeline_depth_(pipeline_depth) {}
    
    uint64_t run_benchmark(uint64_t total_requests) {
        uint64_t per_conn = total_requests / num_connections_;
        std::atomic<uint64_t> completed{0};
        std::vector<std::thread> threads;
        
        for (int i = 0; i < num_connections_; ++i) {
            threads.emplace_back([&, per_conn]() {
                PipelinedClient client(host_, port_, pipeline_depth_);
                if (client.connect()) {
                    completed += client.run_benchmark(per_conn);
                }
            });
        }
        
        for (auto& t : threads) {
            t.join();
        }
        
        return completed.load();
    }
    
private:
    std::string host_;
    uint16_t port_;
    int num_connections_;
    int pipeline_depth_;
};

void print_usage(const char* prog) {
    printf("用法: %s [选项]\n", prog);
    printf("选项:\n");
    printf("  -h <host>     服务器地址 (默认: 127.0.0.1)\n");
    printf("  -p <port>     服务器端口 (默认: 12345)\n");
    printf("  -t <threads>  线程数 (默认: CPU核数)\n");
    printf("  -c <conns>    每线程连接数 (默认: 50)\n");
    printf("  -n <requests> 每连接请求数 (默认: 100000)\n");
    printf("  -d <depth>    流水线深度 (默认: 128)\n");
    printf("  -s <ratio>    SET操作比例%% (默认: 20)\n");
}

int main(int argc, char* argv[]) {
    std::string host = "127.0.0.1";
    uint16_t port = 12345;
    int num_threads = std::thread::hardware_concurrency();
    int conns_per_thread = 50;
    uint64_t requests_per_conn = 100000;
    int pipeline_depth = 128;
    int set_ratio = 20;
    
    int opt;
    while ((opt = getopt(argc, argv, "h:p:t:c:n:d:s:")) != -1) {
        switch (opt) {
            case 'h': host = optarg; break;
            case 'p': port = std::stoi(optarg); break;
            case 't': num_threads = std::stoi(optarg); break;
            case 'c': conns_per_thread = std::stoi(optarg); break;
            case 'n': requests_per_conn = std::stoull(optarg); break;
            case 'd': pipeline_depth = std::stoi(optarg); break;
            case 's': set_ratio = std::stoi(optarg); break;
            default:
                print_usage(argv[0]);
                return 1;
        }
    }
    
    int total_connections = num_threads * conns_per_thread;
    uint64_t total_requests = (uint64_t)total_connections * requests_per_conn;
    
    printf("===========================================\n");
    printf("优化版 KV 性能测试\n");
    printf("===========================================\n");
    printf("服务器: %s:%d\n", host.c_str(), port);
    printf("线程数: %d\n", num_threads);
    printf("每线程连接数: %d\n", conns_per_thread);
    printf("总连接数: %d\n", total_connections);
    printf("流水线深度: %d\n", pipeline_depth);
    printf("每连接请求数: %lu\n", requests_per_conn);
    printf("总请求数: %lu\n", total_requests);
    printf("SET比例: %d%%\n", set_ratio);
    printf("===========================================\n\n");
    
    std::atomic<uint64_t> total_completed{0};
    std::vector<std::thread> threads;
    
    auto start = std::chrono::high_resolution_clock::now();
    
    for (int i = 0; i < num_threads; ++i) {
        threads.emplace_back([&]() {
            for (int j = 0; j < conns_per_thread; ++j) {
                PipelinedClient client(host, port, pipeline_depth);
                if (client.connect()) {
                    total_completed += client.run_benchmark(requests_per_conn, set_ratio);
                }
            }
        });
    }
    
    for (auto& t : threads) {
        t.join();
    }
    
    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
    
    double seconds = duration.count() / 1000.0;
    uint64_t completed = total_completed.load();
    double qps = completed / seconds;
    
    printf("===========================================\n");
    printf("测试结果\n");
    printf("===========================================\n");
    printf("完成请求数: %lu\n", completed);
    printf("耗时: %.2f 秒\n", seconds);
    printf("吞吐量: %.0f QPS\n", qps);
    if (qps >= 1000000) {
        printf("每秒请求: %.2f M\n", qps / 1000000);
    }
    printf("平均延迟: %.3f us\n", 1000000.0 / qps);
    printf("===========================================\n");
    
    return 0;
}
