#include "zlcoro/io/io_uring.hpp"
#include "zlcoro/core/task.hpp"
#include <gtest/gtest.h>
#include <fstream>
#include <thread>
#include <chrono>

using namespace zlcoro;

// =============================================================================
// io_uring 可用性测试
// =============================================================================

TEST(IoUringTest, Availability) {
#ifdef ZLCORO_HAS_IO_URING
    EXPECT_TRUE(io_uring_available());
    std::cout << "io_uring version: " << io_uring_version() << std::endl;
#else
    EXPECT_FALSE(io_uring_available());
    std::cout << "io_uring not available on this system" << std::endl;
    GTEST_SKIP() << "io_uring not available";
#endif
}

#ifdef ZLCORO_HAS_IO_URING

// =============================================================================
// IoUringPoller 测试
// =============================================================================

TEST(IoUringPollerTest, Construction) {
    EXPECT_NO_THROW({
        IoUringPoller poller(64);
        EXPECT_EQ(poller.queue_depth(), 64u);
        EXPECT_EQ(poller.pending_count(), 0u);
    });
}

TEST(IoUringPollerTest, DefaultQueueDepth) {
    IoUringPoller poller;
    EXPECT_EQ(poller.queue_depth(), 256u);
}

// =============================================================================
// IoUringFile 测试
// =============================================================================

class IoUringFileTest : public ::testing::Test {
protected:
    void SetUp() override {
        test_file_ = "/tmp/zlcoro_iouring_test_" + 
                     std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
        
        // 创建测试文件
        std::ofstream ofs(test_file_);
        ofs << test_content_;
        ofs.close();
    }

    void TearDown() override {
        std::remove(test_file_.c_str());
    }

    std::string test_file_;
    std::string test_content_ = "Hello, io_uring!\nThis is a test file.\n";
};

TEST_F(IoUringFileTest, ReadAll) {
    IoUringEventLoop loop;
    IoUringFile file(&loop.poller(), test_file_, IoUringFile::ReadOnly);
    
    std::string result;
    bool done = false;
    
    auto task = [&]() -> Task<void> {
        result = co_await file.read_all();
        done = true;
        co_return;
    };
    
    auto coro = task();
    coro.handle().resume();  // 启动协程
    
    // 运行事件循环
    while (!done && loop.poller().pending_count() > 0) {
        auto ready = loop.poller().poll(100);
        for (auto& h : ready) {
            if (h && !h.done()) {
                h.resume();
            }
        }
    }
    
    EXPECT_EQ(result, test_content_);
}

TEST_F(IoUringFileTest, WriteAndRead) {
    IoUringEventLoop loop;
    
    std::string write_content = "io_uring write test content\n";
    std::string read_result;
    bool done = false;
    
    auto task = [&]() -> Task<void> {
        // 写入文件
        {
            IoUringFile file(&loop.poller(), test_file_, 
                            IoUringFile::WriteOnly | IoUringFile::Truncate);
            co_await file.write_all_string(write_content);
            co_await file.fsync();
        }
        
        // 读取文件
        {
            IoUringFile file(&loop.poller(), test_file_, IoUringFile::ReadOnly);
            read_result = co_await file.read_all();
        }
        
        done = true;
        co_return;
    };
    
    auto coro = task();
    coro.handle().resume();
    
    while (!done && loop.poller().pending_count() > 0) {
        auto ready = loop.poller().poll(100);
        for (auto& h : ready) {
            if (h && !h.done()) {
                h.resume();
            }
        }
    }
    
    EXPECT_EQ(read_result, write_content);
}

TEST_F(IoUringFileTest, PartialRead) {
    IoUringEventLoop loop;
    IoUringFile file(&loop.poller(), test_file_, IoUringFile::ReadOnly);
    
    std::string result;
    bool done = false;
    
    auto task = [&]() -> Task<void> {
        result = co_await file.read_string(5, 0);  // 读取前 5 个字节
        done = true;
        co_return;
    };
    
    auto coro = task();
    coro.handle().resume();
    
    while (!done && loop.poller().pending_count() > 0) {
        auto ready = loop.poller().poll(100);
        for (auto& h : ready) {
            if (h && !h.done()) {
                h.resume();
            }
        }
    }
    
    EXPECT_EQ(result, "Hello");
}

// =============================================================================
// IoUringSocket 测试
// =============================================================================

TEST(IoUringSocketTest, CreateAndBind) {
    IoUringPoller poller;
    IoUringSocket socket(&poller);
    
    EXPECT_NO_THROW({
        socket.create();
        socket.set_reuse_addr();
        socket.bind("127.0.0.1", 0);  // 使用随机端口
        socket.listen(10);
    });
    
    EXPECT_TRUE(socket.is_valid());
}

TEST(IoUringSocketTest, EchoServer) {
    IoUringEventLoop loop;
    
    bool server_done = false;
    bool client_done = false;
    std::string received_data;
    std::string send_data = "Hello from client!";
    uint16_t server_port = 0;
    
    // 创建服务器 socket
    IoUringSocket server(&loop.poller());
    server.create();
    server.set_reuse_addr();
    server.bind("127.0.0.1", 0);
    server.listen(10);
    
    // 获取实际绑定的端口
    sockaddr_in addr{};
    socklen_t len = sizeof(addr);
    getsockname(server.fd(), reinterpret_cast<sockaddr*>(&addr), &len);
    server_port = ntohs(addr.sin_port);
    
    // 服务器任务
    auto server_task = [&]() -> Task<void> {
        auto client = co_await server.accept();
        received_data = co_await client.recv_string(1024);
        co_await client.send_string(received_data);  // Echo back
        server_done = true;
        co_return;
    };
    
    // 客户端任务
    auto client_task = [&]() -> Task<void> {
        // 等待服务器准备好
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        
        IoUringSocket client(&loop.poller());
        client.create();
        
        int ret = co_await client.connect("127.0.0.1", server_port);
        EXPECT_GE(ret, 0);
        
        co_await client.send_string(send_data);
        std::string response = co_await client.recv_string(1024);
        
        EXPECT_EQ(response, send_data);
        client_done = true;
        co_return;
    };
    
    auto s_coro = server_task();
    auto c_coro = client_task();
    
    s_coro.handle().resume();
    c_coro.handle().resume();
    
    // 运行事件循环
    int iterations = 0;
    while ((!server_done || !client_done) && iterations < 1000) {
        auto ready = loop.poller().poll(10);
        for (auto& h : ready) {
            if (h && !h.done()) {
                h.resume();
            }
        }
        ++iterations;
    }
    
    EXPECT_TRUE(server_done);
    EXPECT_TRUE(client_done);
    EXPECT_EQ(received_data, send_data);
}

// =============================================================================
// 性能基准测试
// =============================================================================

TEST(IoUringPerformanceTest, FileReadBenchmark) {
    // 创建一个较大的测试文件
    std::string test_file = "/tmp/zlcoro_iouring_benchmark_" + 
                            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    
    constexpr size_t file_size = 1024 * 1024;  // 1MB
    std::string large_content(file_size, 'A');
    
    {
        std::ofstream ofs(test_file);
        ofs << large_content;
    }
    
    // io_uring 读取
    IoUringEventLoop loop;
    
    auto start = std::chrono::high_resolution_clock::now();
    
    std::string result;
    bool done = false;
    
    auto task = [&]() -> Task<void> {
        IoUringFile file(&loop.poller(), test_file, IoUringFile::ReadOnly);
        result = co_await file.read_all();
        done = true;
        co_return;
    };
    
    auto coro = task();
    coro.handle().resume();
    
    while (!done && loop.poller().pending_count() > 0) {
        auto ready = loop.poller().poll(100);
        for (auto& h : ready) {
            if (h && !h.done()) {
                h.resume();
            }
        }
    }
    
    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);
    
    std::cout << "io_uring read " << file_size / 1024 << "KB: " 
              << duration.count() << " us" << std::endl;
    
    EXPECT_EQ(result.size(), file_size);
    
    // 清理
    std::remove(test_file.c_str());
}

TEST(IoUringPerformanceTest, MultipleSmallReads) {
    std::string test_file = "/tmp/zlcoro_iouring_small_reads_" + 
                            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    
    // 创建测试文件
    constexpr size_t file_size = 64 * 1024;  // 64KB
    std::string content(file_size, 'B');
    {
        std::ofstream ofs(test_file);
        ofs << content;
    }
    
    IoUringEventLoop loop;
    
    constexpr int num_reads = 100;
    constexpr size_t read_size = 1024;  // 每次读 1KB
    
    auto start = std::chrono::high_resolution_clock::now();
    
    int completed = 0;
    
    auto task = [&]() -> Task<void> {
        IoUringFile file(&loop.poller(), test_file, IoUringFile::ReadOnly);
        
        for (int i = 0; i < num_reads; ++i) {
            off_t offset = (i * read_size) % file_size;
            auto data = co_await file.read_string(read_size, offset);
            ++completed;
        }
        
        co_return;
    };
    
    auto coro = task();
    coro.handle().resume();
    
    while (completed < num_reads && loop.poller().pending_count() > 0) {
        auto ready = loop.poller().poll(100);
        for (auto& h : ready) {
            if (h && !h.done()) {
                h.resume();
            }
        }
    }
    
    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);
    
    std::cout << "io_uring " << num_reads << " small reads: " 
              << duration.count() << " us ("
              << duration.count() / num_reads << " us/read)" << std::endl;
    
    EXPECT_EQ(completed, num_reads);
    
    std::remove(test_file.c_str());
}

#endif // ZLCORO_HAS_IO_URING
