#include "zlcoro/io.hpp"
#include "zlcoro/scheduler/async.hpp"
#include <gtest/gtest.h>
#include <fstream>
#include <filesystem>

using namespace zlcoro;

// =============================================================================
// AsyncFile 测试
// =============================================================================

TEST(AsyncFileTest, ReadWrite) {
    auto test_file = "/tmp/zlcoro_test_file.txt";
    std::string content = "Hello, AsyncFile!";
    
    // 写入
    auto write_task = [&]() -> Task<void> {
        co_await write_file(test_file, content);
    };
    
    auto future = async_run(write_task());
    future.get();
    
    // 读取
    auto read_task = [&]() -> Task<std::string> {
        co_return co_await read_file(test_file);
    };
    
    auto read_future = async_run(read_task());
    std::string read_content = read_future.get();
    
    EXPECT_EQ(read_content, content);
    
    // 清理
    std::filesystem::remove(test_file);
}

TEST(AsyncFileTest, Append) {
    auto test_file = "/tmp/zlcoro_test_append.txt";
    
    auto task = [&]() -> Task<void> {
        co_await write_file(test_file, "Line 1\n");
        co_await append_file(test_file, "Line 2\n");
        co_await append_file(test_file, "Line 3\n");
    };
    
    auto future = async_run(task());
    future.get();
    
    // 验证
    std::ifstream file(test_file);
    std::string line;
    std::vector<std::string> lines;
    while (std::getline(file, line)) {
        lines.push_back(line);
    }
    
    ASSERT_EQ(lines.size(), 3);
    EXPECT_EQ(lines[0], "Line 1");
    EXPECT_EQ(lines[1], "Line 2");
    EXPECT_EQ(lines[2], "Line 3");
    
    // 清理
    std::filesystem::remove(test_file);
}

TEST(AsyncFileTest, LargeFile) {
    auto test_file = "/tmp/zlcoro_test_large.txt";
    
    // 创建 1MB 的数据
    std::string large_content(1024 * 1024, 'X');
    
    // 先写入
    auto write_task = [&]() -> Task<void> {
        co_return co_await write_file(test_file, large_content);
    };
    auto write_future = async_run(write_task());
    write_future.get();
    
    // 再读取
    auto read_task = [&]() -> Task<std::string> {
        AsyncFile file(test_file, AsyncFile::ReadOnly);
        co_return file.read_all();
    };
    auto read_future = async_run(read_task());
    std::string read_content = read_future.get();
    
    EXPECT_EQ(read_content.size(), large_content.size());
    EXPECT_EQ(read_content, large_content);
    
    // 清理
    std::filesystem::remove(test_file);
}

// =============================================================================
// EpollPoller 基础测试
// =============================================================================

TEST(EpollPollerTest, CreateAndDestroy) {
    EXPECT_NO_THROW({
        EpollPoller poller;
        EXPECT_GT(poller.fd(), 0);
    });
}

TEST(EpollPollerTest, AddRemove) {
    EpollPoller poller;
    
    // 创建一个管道用于测试
    int pipefd[2];
    ASSERT_EQ(pipe(pipefd), 0);
    
    // 创建一个简单的协程句柄（使用 noop_coroutine）
    auto coro = std::noop_coroutine();
    
    EXPECT_NO_THROW(poller.add(pipefd[0], EpollPoller::Read, coro));
    EXPECT_TRUE(poller.has(pipefd[0]));
    
    // 移除
    EXPECT_NO_THROW(poller.remove(pipefd[0]));
    EXPECT_FALSE(poller.has(pipefd[0]));
    
    // 清理
    close(pipefd[0]);
    close(pipefd[1]);
}

// =============================================================================
// 集成测试（需要事件循环）
// =============================================================================

// 注意：完整的 Socket 测试需要运行事件循环，这里只做基础测试

TEST(AsyncSocketTest, CreateAndClose) {
    EXPECT_NO_THROW({
        AsyncSocket socket;
        socket.create();
        EXPECT_TRUE(socket.is_open());
        socket.close();
        EXPECT_FALSE(socket.is_open());
    });
}

TEST(AsyncSocketTest, BindAndListen) {
    AsyncSocket socket;
    socket.create();
    socket.set_reuse_addr(true);
    
    EXPECT_NO_THROW(socket.bind("127.0.0.1", 12345));
    EXPECT_NO_THROW(socket.listen());
    
    socket.close();
}
