/**
 * 简化版高并发 Echo 客户端
 * 用于测试优化后服务器的性能
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

std::atomic<uint64_t> total_requests{0};
std::atomic<uint64_t> total_errors{0};
std::atomic<bool> running{true};

bool set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

// 流水线 echo 客户端
class PipelinedEchoClient {
public:
    PipelinedEchoClient(const std::string& host, uint16_t port, int pipeline_depth = 64)
        : host_(host), port_(port), pipeline_depth_(pipeline_depth), fd_(-1) {}
    
    ~PipelinedEchoClient() {
        if (fd_ >= 0) close(fd_);
    }
    
    bool connect() {
        fd_ = socket(AF_INET, SOCK_STREAM, 0);
        if (fd_ < 0) return false;
        
        int opt = 1;
        setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));
        
        int buf_size = 256 * 1024;
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
    
    // 运行流水线 echo 测试
    uint64_t run(uint64_t num_requests, size_t msg_size = 64) {
        if (fd_ < 0) return 0;
        
        // 准备消息
        std::string msg(msg_size, 'x');
        
        uint64_t sent = 0;
        uint64_t received = 0;
        uint64_t in_flight = 0;
        
        std::vector<char> send_buf;
        std::vector<char> recv_buf(256 * 1024);
        size_t recv_offset = 0;
        
        while (received < num_requests && running) {
            // 发送
            while (in_flight < (uint64_t)pipeline_depth_ && sent < num_requests) {
                size_t old_size = send_buf.size();
                send_buf.resize(old_size + msg_size);
                memcpy(send_buf.data() + old_size, msg.data(), msg_size);
                sent++;
                in_flight++;
            }
            
            if (!send_buf.empty()) {
                ssize_t n = send(fd_, send_buf.data(), send_buf.size(), MSG_NOSIGNAL);
                if (n > 0) {
                    send_buf.erase(send_buf.begin(), send_buf.begin() + n);
                } else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                    total_errors++;
                    break;
                }
            }
            
            // 接收
            ssize_t n = recv(fd_, recv_buf.data() + recv_offset, 
                           recv_buf.size() - recv_offset, MSG_DONTWAIT);
            if (n > 0) {
                recv_offset += n;
                
                // 计算收到多少完整消息
                size_t complete = recv_offset / msg_size;
                if (complete > 0) {
                    received += complete;
                    in_flight -= complete;
                    total_requests.fetch_add(complete);
                    
                    size_t consumed = complete * msg_size;
                    if (consumed < recv_offset) {
                        memmove(recv_buf.data(), recv_buf.data() + consumed, recv_offset - consumed);
                    }
                    recv_offset -= consumed;
                }
            } else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                total_errors++;
                break;
            }
            
            // 如果没有进展，短暂等待
            if (send_buf.empty() && n <= 0 && in_flight > 0) {
                struct pollfd pfd{fd_, POLLIN | POLLOUT, 0};
                poll(&pfd, 1, 1);
            }
        }
        
        return received;
    }
    
private:
    std::string host_;
    uint16_t port_;
    int pipeline_depth_;
    int fd_;
};

void worker_thread(const std::string& host, uint16_t port, 
                  int num_connections, uint64_t requests_per_conn,
                  int pipeline_depth, size_t msg_size) {
    for (int i = 0; i < num_connections && running; ++i) {
        PipelinedEchoClient client(host, port, pipeline_depth);
        if (client.connect()) {
            client.run(requests_per_conn, msg_size);
        } else {
            total_errors++;
        }
    }
}

void print_usage(const char* prog) {
    printf("用法: %s [选项]\n", prog);
    printf("选项:\n");
    printf("  -h <host>     服务器地址 (默认: 127.0.0.1)\n");
    printf("  -p <port>     服务器端口 (默认: 12399)\n");
    printf("  -t <threads>  线程数 (默认: CPU核数)\n");
    printf("  -c <conns>    每线程连接数 (默认: 50)\n");
    printf("  -n <requests> 每连接请求数 (默认: 10000)\n");
    printf("  -d <depth>    流水线深度 (默认: 64)\n");
    printf("  -s <size>     消息大小 (默认: 64)\n");
}

int main(int argc, char* argv[]) {
    std::string host = "127.0.0.1";
    uint16_t port = 12399;
    int num_threads = std::thread::hardware_concurrency();
    int conns_per_thread = 50;
    uint64_t requests_per_conn = 10000;
    int pipeline_depth = 64;
    size_t msg_size = 64;
    
    int opt;
    while ((opt = getopt(argc, argv, "h:p:t:c:n:d:s:")) != -1) {
        switch (opt) {
            case 'h': host = optarg; break;
            case 'p': port = std::stoi(optarg); break;
            case 't': num_threads = std::stoi(optarg); break;
            case 'c': conns_per_thread = std::stoi(optarg); break;
            case 'n': requests_per_conn = std::stoull(optarg); break;
            case 'd': pipeline_depth = std::stoi(optarg); break;
            case 's': msg_size = std::stoull(optarg); break;
            default:
                print_usage(argv[0]);
                return 1;
        }
    }
    
    int total_connections = num_threads * conns_per_thread;
    uint64_t total_reqs = (uint64_t)total_connections * requests_per_conn;
    
    printf("===========================================\n");
    printf("高并发 Echo 性能测试\n");
    printf("===========================================\n");
    printf("服务器: %s:%d\n", host.c_str(), port);
    printf("线程数: %d\n", num_threads);
    printf("每线程连接数: %d\n", conns_per_thread);
    printf("总连接数: %d\n", total_connections);
    printf("流水线深度: %d\n", pipeline_depth);
    printf("消息大小: %zu 字节\n", msg_size);
    printf("每连接请求数: %lu\n", requests_per_conn);
    printf("总请求数: %lu\n", total_reqs);
    printf("===========================================\n\n");
    
    auto start = std::chrono::high_resolution_clock::now();
    
    std::vector<std::thread> threads;
    for (int i = 0; i < num_threads; ++i) {
        threads.emplace_back(worker_thread, host, port, 
                            conns_per_thread, requests_per_conn,
                            pipeline_depth, msg_size);
    }
    
    for (auto& t : threads) {
        t.join();
    }
    
    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
    
    double seconds = duration.count() / 1000.0;
    uint64_t completed = total_requests.load();
    uint64_t errors = total_errors.load();
    double qps = completed / seconds;
    
    printf("===========================================\n");
    printf("测试结果\n");
    printf("===========================================\n");
    printf("完成请求数: %lu\n", completed);
    printf("错误数: %lu\n", errors);
    printf("耗时: %.2f 秒\n", seconds);
    printf("吞吐量: %.0f QPS\n", qps);
    if (qps >= 1000000) {
        printf("每秒请求: %.2f M\n", qps / 1000000);
    }
    printf("平均延迟: %.3f us\n", 1000000.0 / qps);
    printf("===========================================\n");
    
    return 0;
}
