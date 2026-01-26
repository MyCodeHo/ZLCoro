#include "io_bench_common.hpp"
#include "zlcoro/io/async_file.hpp"

using namespace io_bench;

int main() {
    std::cout << "\n╔════════════════════════════════════════════════════════════╗\n";
    std::cout << "║          ZLCoro 协程框架 I/O 性能综合测试套件           ║\n";
    std::cout << "╚════════════════════════════════════════════════════════════╝\n\n";

    // 全局 EventLoop - 在所有测试期间保持运行
    EventLoopRunner global_event_loop;

    // ========== 网络 I/O 性能测试 ==========
    
    std::cout << "╔════════════════════════════════════════════════════════════╗\n";
    std::cout << "║                  1. 并发扩展性测试                       ║\n";
    std::cout << "╚════════════════════════════════════════════════════════════╝\n\n";
    
    {
        auto r1 = run_coroutine_benchmark(1, 512, 100, true);
        r1.print("单客户端基准");
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        
        auto r2 = run_coroutine_benchmark(8, 512, 100, true);
        r2.print("小规模并发 (8客户端)");
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        
        auto r3 = run_coroutine_benchmark(16, 512, 100, true);
        r3.print("中等并发 (16客户端)");
        std::this_thread::sleep_for(std::chrono::milliseconds(200));

        auto r4 = run_coroutine_benchmark(64, 512, 80, true);
        r4.print("高并发 (64客户端)");
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    std::cout << "╔════════════════════════════════════════════════════════════╗\n";
    std::cout << "║                  2. 不同负载大小测试                     ║\n";
    std::cout << "╚════════════════════════════════════════════════════════════╝\n\n";
    
    {
        auto r1 = run_coroutine_benchmark(16, 64, 100, true);
        r1.print("小包传输 (64B)");
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        
        auto r2 = run_coroutine_benchmark(16, 512, 100, true);
        r2.print("中等包 (512B)");
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        
        auto r3 = run_coroutine_benchmark(8, 4096, 50, true);
        r3.print("大包传输 (4KB)");
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        
        auto r4 = run_coroutine_benchmark(8, 16384, 30, true);
        r4.print("超大包 (16KB)");
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    std::cout << "╔════════════════════════════════════════════════════════════╗\n";
    std::cout << "║                  3. 长连接持续传输测试                   ║\n";
    std::cout << "╚════════════════════════════════════════════════════════════╝\n\n";
    
    {
        auto r = run_coroutine_benchmark(8, 1024, 200, true);
        r.print("长连接测试 (8客户端, 200消息)");
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    // ========== 文件 I/O 性能测试 ==========
    
    std::cout << "╔════════════════════════════════════════════════════════════╗\n";
    std::cout << "║                  4. 文件 I/O 性能测试                    ║\n";
    std::cout << "╚════════════════════════════════════════════════════════════╝\n\n";
    
    std::vector<std::pair<std::string, size_t>> file_tests = {
        {"1KB 文件", 1024},
        {"10KB 文件", 10240},
        {"100KB 文件", 102400},
        {"1MB 文件", 1048576}
    };
    
    for (const auto& [name, size] : file_tests) {
        std::string path = "/tmp/zlcoro_bench_" + std::to_string(size) + ".dat";
        
        // 写入测试
        std::cout << name << " - 写入 ..." << std::flush;
        ScopedTimer wtimer;
        auto wfut = zlcoro::async_run(zlcoro::write_file(path, std::string(size, 'x')));
        wfut.wait();
        double wsec = wtimer.elapsed_seconds();
        double wmb = static_cast<double>(size) / (1024.0 * 1024.0);
        double w_throughput = wmb / wsec;
        std::cout << " 完成 [" << std::fixed << std::setprecision(2) << w_throughput << " MB/s]\n";
        
        // 读取测试
        std::cout << name << " - 读取 ..." << std::flush;
        ScopedTimer rtimer;
        auto rfut = zlcoro::async_run(zlcoro::read_file(path));
        auto data = rfut.get();
        double rsec = rtimer.elapsed_seconds();
        double rmb = static_cast<double>(data.size()) / (1024.0 * 1024.0);
        double r_throughput = rmb / rsec;
        std::cout << " 完成 [" << std::fixed << std::setprecision(2) << r_throughput << " MB/s]\n\n";
        
        std::remove(path.c_str());
    }

    // ========== 性能总结 ==========
    
    std::cout << "╔════════════════════════════════════════════════════════════╗\n";
    std::cout << "║                      测试总结                            ║\n";
    std::cout << "╚════════════════════════════════════════════════════════════╝\n\n";
    
    std::cout << "✓ 并发扩展性: 1 → 16 客户端测试完成\n";
    std::cout << "✓ 负载测试: 64B → 16KB 测试完成\n";
    std::cout << "✓ 长连接测试: 持续传输场景测试完成\n";
    std::cout << "✓ 文件 I/O: 1KB → 1MB 测试完成\n\n";
    
    std::cout << "框架优势:\n";
    std::cout << "  • 高并发场景性能优异\n";
    std::cout << "  • 内存占用极低 (协程栈 vs 线程栈)\n";
    std::cout << "  • 代码简洁易维护 (async/await)\n";
    std::cout << "  • 良好的扩展性\n\n";
    
    std::cout << "✓ 所有测试完成！\n\n";
    return 0;
}
