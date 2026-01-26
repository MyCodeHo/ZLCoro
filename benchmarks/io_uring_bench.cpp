#include "zlcoro/io/io_uring.hpp"
#include "zlcoro/io/async_file.hpp"
#include "zlcoro/io/async_socket.hpp"
#include "zlcoro/scheduler/async.hpp"
#include <iostream>
#include <iomanip>
#include <chrono>
#include <vector>
#include <thread>
#include <atomic>
#include <fstream>
#include <cstring>

using namespace std::chrono;
using namespace zlcoro;

// =============================================================================
// 工具函数
// =============================================================================

class Timer {
public:
    Timer() : start_(steady_clock::now()) {}
    
    double elapsed_us() const {
        return duration_cast<microseconds>(steady_clock::now() - start_).count();
    }
    
    double elapsed_ms() const {
        return duration_cast<milliseconds>(steady_clock::now() - start_).count();
    }
    
    double elapsed_s() const {
        return duration_cast<duration<double>>(steady_clock::now() - start_).count();
    }
    
private:
    steady_clock::time_point start_;
};

void print_result(const std::string& name, double throughput_mbs, double latency_us) {
    std::cout << std::left << std::setw(40) << name 
              << std::right << std::setw(10) << std::fixed << std::setprecision(2) 
              << throughput_mbs << " MB/s"
              << std::setw(12) << std::setprecision(1) << latency_us << " μs\n";
}

// =============================================================================
// 文件 I/O 性能测试
// =============================================================================

#ifdef ZLCORO_HAS_IO_URING

void benchmark_iouring_file_read(const std::string& path, size_t file_size, int iterations) {
    IoUringEventLoop loop;
    
    double total_time = 0;
    size_t total_bytes = 0;
    
    for (int i = 0; i < iterations; ++i) {
        IoUringFile file(&loop.poller(), path, IoUringFile::ReadOnly);
        
        std::string result;
        bool done = false;
        
        Timer timer;
        
        auto task = [&]() -> Task<void> {
            result = co_await file.read_all();
            done = true;
        };
        
        auto coro = task();
        coro.handle().resume();
        
        while (!done && loop.poller().pending_count() > 0) {
            auto ready = loop.poller().poll(100);
            for (auto& h : ready) {
                if (h && !h.done()) h.resume();
            }
        }
        
        total_time += timer.elapsed_us();
        total_bytes += result.size();
    }
    
    double avg_time_us = total_time / iterations;
    double throughput = (total_bytes / iterations) / (avg_time_us / 1e6) / (1024 * 1024);
    print_result("io_uring 文件读取", throughput, avg_time_us);
}

void benchmark_iouring_file_write(const std::string& path, size_t file_size, int iterations) {
    IoUringEventLoop loop;
    std::string data(file_size, 'x');
    
    double total_time = 0;
    
    for (int i = 0; i < iterations; ++i) {
        IoUringFile file(&loop.poller(), path, 
                         IoUringFile::WriteOnly | IoUringFile::Create | IoUringFile::Truncate);
        
        bool done = false;
        
        Timer timer;
        
        auto task = [&]() -> Task<void> {
            co_await file.write_all_string(data);
            co_await file.fsync();
            done = true;
        };
        
        auto coro = task();
        coro.handle().resume();
        
        while (!done && loop.poller().pending_count() > 0) {
            auto ready = loop.poller().poll(100);
            for (auto& h : ready) {
                if (h && !h.done()) h.resume();
            }
        }
        
        total_time += timer.elapsed_us();
    }
    
    double avg_time_us = total_time / iterations;
    double throughput = file_size / (avg_time_us / 1e6) / (1024 * 1024);
    print_result("io_uring 文件写入 (fsync)", throughput, avg_time_us);
}

#endif // ZLCORO_HAS_IO_URING

// epoll + 线程池版本（当前 AsyncFile 实现）
void benchmark_epoll_file_read(const std::string& path, size_t file_size, int iterations) {
    // 启动 EventLoop
    auto& loop = EventLoop::instance();
    std::thread loop_thread([&loop]() { loop.run(); });
    
    double total_time = 0;
    size_t total_bytes = 0;
    
    for (int i = 0; i < iterations; ++i) {
        Timer timer;
        
        auto fut = async_run(read_file(path));
        auto result = fut.get();
        
        total_time += timer.elapsed_us();
        total_bytes += result.size();
    }
    
    loop.stop();
    loop_thread.join();
    loop.reset();
    
    double avg_time_us = total_time / iterations;
    double throughput = (total_bytes / iterations) / (avg_time_us / 1e6) / (1024 * 1024);
    print_result("epoll + 线程池 文件读取", throughput, avg_time_us);
}

void benchmark_epoll_file_write(const std::string& path, size_t file_size, int iterations) {
    std::string data(file_size, 'x');
    
    auto& loop = EventLoop::instance();
    std::thread loop_thread([&loop]() { loop.run(); });
    
    double total_time = 0;
    
    for (int i = 0; i < iterations; ++i) {
        Timer timer;
        
        auto fut = async_run(write_file(path, data));
        fut.wait();
        
        total_time += timer.elapsed_us();
    }
    
    loop.stop();
    loop_thread.join();
    loop.reset();
    
    double avg_time_us = total_time / iterations;
    double throughput = file_size / (avg_time_us / 1e6) / (1024 * 1024);
    print_result("epoll + 线程池 文件写入", throughput, avg_time_us);
}

// =============================================================================
// 批量小文件操作测试（暂时禁用，需要修复生命周期问题）
// =============================================================================

#ifdef ZLCORO_HAS_IO_URING

// void benchmark_iouring_batch_reads(...) { ... }

#endif

// =============================================================================
// 网络 I/O 性能测试 (暂时禁用)
// =============================================================================
// 注意：网络测试需要更复杂的协程生命周期管理
// 当前 lambda 协程存在 use-after-free 问题
// 需要使用 shared_ptr<Task<>> 或协程池来管理
// =============================================================================

#ifdef ZLCORO_HAS_IO_URING

// 网络测试暂时禁用，后续使用 Runtime 统一管理
/*
void benchmark_iouring_network_echo(uint16_t port, int clients, int messages, size_t payload_size) {
    // ... 原有代码 ...
}
*/

#endif

// =============================================================================
// 主函数
// =============================================================================

int main() {
    std::cout << "\n╔════════════════════════════════════════════════════════════════╗\n";
    std::cout << "║         ZLCoro io_uring vs epoll 性能对比测试                  ║\n";
    std::cout << "╚════════════════════════════════════════════════════════════════╝\n\n";

#ifndef ZLCORO_HAS_IO_URING
    std::cout << "⚠️  io_uring 不可用，仅测试 epoll 方案\n\n";
#else
    std::cout << "✓ io_uring 可用: " << io_uring_version() << "\n\n";
#endif

    const std::string test_file = "/tmp/zlcoro_uring_bench.dat";
    const size_t small_file = 4 * 1024;       // 4KB
    const size_t medium_file = 64 * 1024;     // 64KB
    const size_t large_file = 1024 * 1024;    // 1MB
    const int iterations = 20;

    // 准备测试文件
    {
        std::ofstream ofs(test_file);
        ofs << std::string(large_file, 'x');
    }

    // ========== 文件 I/O 测试 ==========
    
    std::cout << "╔════════════════════════════════════════════════════════════════╗\n";
    std::cout << "║                    1. 文件读取性能测试                         ║\n";
    std::cout << "╚════════════════════════════════════════════════════════════════╝\n\n";
    
    std::cout << std::left << std::setw(40) << "测试项" 
              << std::right << std::setw(15) << "吞吐量"
              << std::setw(12) << "延迟\n";
    std::cout << std::string(67, '-') << "\n";

#ifdef ZLCORO_HAS_IO_URING
    benchmark_iouring_file_read(test_file, large_file, iterations);
#endif
    benchmark_epoll_file_read(test_file, large_file, iterations);
    
    std::cout << "\n";

    // ========== 文件写入测试 ==========
    
    std::cout << "╔════════════════════════════════════════════════════════════════╗\n";
    std::cout << "║                    2. 文件写入性能测试                         ║\n";
    std::cout << "╚════════════════════════════════════════════════════════════════╝\n\n";
    
    std::cout << std::left << std::setw(40) << "测试项" 
              << std::right << std::setw(15) << "吞吐量"
              << std::setw(12) << "延迟\n";
    std::cout << std::string(67, '-') << "\n";

#ifdef ZLCORO_HAS_IO_URING
    benchmark_iouring_file_write(test_file, large_file, iterations);
#endif
    benchmark_epoll_file_write(test_file, large_file, iterations);
    
    std::cout << "\n";

#ifdef ZLCORO_HAS_IO_URING
    // 批量文件操作测试暂时禁用
    /*
    std::cout << "╔════════════════════════════════════════════════════════════════╗\n";
    std::cout << "║                    3. 批量文件操作测试                         ║\n";
    std::cout << "╚════════════════════════════════════════════════════════════════╝\n\n";
    
    std::cout << std::left << std::setw(40) << "测试项" 
              << std::right << std::setw(15) << "吞吐量"
              << std::setw(12) << "延迟/文件\n";
    std::cout << std::string(67, '-') << "\n";

    benchmark_iouring_batch_reads("/tmp/zlcoro_batch", 100, 4096);  // 100个4KB文件
    
    std::cout << "\n";
    */

    // ========== 网络 I/O 测试 (暂时禁用) ==========
    /*
    std::cout << "╔════════════════════════════════════════════════════════════════╗\n";
    std::cout << "║                    3. 网络 I/O 性能测试                        ║\n";
    std::cout << "╚════════════════════════════════════════════════════════════════╝\n\n";

    benchmark_iouring_network_echo(18080, 4, 100, 512);
    benchmark_iouring_network_echo(18081, 8, 100, 512);
    benchmark_iouring_network_echo(18082, 16, 50, 512);
    */
    
#endif

    // 清理
    std::remove(test_file.c_str());

    // ========== 总结 ==========
    
    std::cout << "╔════════════════════════════════════════════════════════════════╗\n";
    std::cout << "║                         性能总结                               ║\n";
    std::cout << "╚════════════════════════════════════════════════════════════════╝\n\n";

#ifdef ZLCORO_HAS_IO_URING
    std::cout << "io_uring 优势:\n";
    std::cout << "  ✓ 真正的异步 I/O（内核直接执行）\n";
    std::cout << "  ✓ 零拷贝 SQ/CQ 环形缓冲区\n";
    std::cout << "  ✓ 批量提交减少系统调用\n";
    std::cout << "  ✓ 统一文件和网络 I/O 接口\n\n";
    
    std::cout << "epoll + 线程池 优势:\n";
    std::cout << "  ✓ 兼容性好（Linux 2.6+）\n";
    std::cout << "  ✓ 简单易用\n";
    std::cout << "  ✓ 调试方便\n\n";
#endif

    std::cout << "✓ 性能测试完成！\n\n";
    return 0;
}
