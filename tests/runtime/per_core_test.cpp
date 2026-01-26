/**
 * @file per_core_test.cpp
 * @brief Per-Core 架构单元测试
 */

#include <gtest/gtest.h>
#include <thread>
#include <chrono>
#include <atomic>

#include "zlcoro/runtime/per_core.hpp"

using namespace zlcoro;

// 测试 EpollPerCoreEventLoop 基本功能
TEST(PerCoreTest, EpollEventLoopBasic) {
    EpollPerCoreEventLoop loop;
    
    // 测试初始状态
    EXPECT_FALSE(loop.is_running());
    EXPECT_EQ(loop.core_id(), -1);
    
    // 测试后端类型
    EXPECT_EQ(loop.backend(), PerCoreEventLoop::Backend::Epoll);
}

// 测试 IoUringPerCoreEventLoop 基本功能
#ifdef ZLCORO_HAS_IO_URING
TEST(PerCoreTest, IoUringEventLoopBasic) {
    IoUringPerCoreEventLoop loop;
    
    // 测试初始状态
    EXPECT_FALSE(loop.is_running());
    EXPECT_EQ(loop.core_id(), -1);
    
    // 测试后端类型
    EXPECT_EQ(loop.backend(), PerCoreEventLoop::Backend::IoUring);
    
    // 测试 io_uring ring
    EXPECT_NE(loop.ring(), nullptr);
    EXPECT_GT(loop.ring_fd(), 0);
}
#endif

// 测试 PerCoreRuntime 创建（epoll）
TEST(PerCoreTest, RuntimeCreationEpoll) {
    auto runtime = make_epoll_runtime(2);
    
    EXPECT_EQ(runtime->num_cores(), 2);
    EXPECT_EQ(runtime->backend(), PerCoreEventLoop::Backend::Epoll);
    EXPECT_FALSE(runtime->is_running());
}

// 测试 PerCoreRuntime 创建（io_uring）
#ifdef ZLCORO_HAS_IO_URING
TEST(PerCoreTest, RuntimeCreationIoUring) {
    auto runtime = make_io_uring_runtime(2);
    
    EXPECT_EQ(runtime->num_cores(), 2);
    EXPECT_EQ(runtime->backend(), PerCoreEventLoop::Backend::IoUring);
    EXPECT_FALSE(runtime->is_running());
}
#endif

// 测试 PerCoreRuntime 启动和停止
TEST(PerCoreTest, RuntimeStartStop) {
    auto runtime = make_epoll_runtime(2);
    
    runtime->start();
    EXPECT_TRUE(runtime->is_running());
    
    // 短暂等待线程启动
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    
    runtime->stop();
    EXPECT_FALSE(runtime->is_running());
}

// 测试核心选择
TEST(PerCoreTest, CoreSelection) {
    auto runtime = make_epoll_runtime(4);
    
    // 轮询选择
    EXPECT_EQ(runtime->next_core_round_robin() % 4, 0);
    EXPECT_EQ(runtime->next_core_round_robin() % 4, 1);
    EXPECT_EQ(runtime->next_core_round_robin() % 4, 2);
    EXPECT_EQ(runtime->next_core_round_robin() % 4, 3);
    
    // 基于 fd 选择（确定性）
    EXPECT_EQ(runtime->select_core_by_fd(100), 100 % 4);
    EXPECT_EQ(runtime->select_core_by_fd(100), 100 % 4);  // 相同结果
}

// 测试定时器
TEST(PerCoreTest, Timer) {
    EpollPerCoreEventLoop loop;
    
    std::atomic<bool> fired{false};
    auto id = loop.add_timer(10, [&fired]() {
        fired = true;
    });
    
    // 运行一次事件循环
    loop.run_once();
    
    // 等待定时器触发
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    loop.run_once();
    
    EXPECT_TRUE(fired);
}

// 测试取消定时器
TEST(PerCoreTest, TimerCancel) {
    EpollPerCoreEventLoop loop;
    
    std::atomic<bool> fired{false};
    auto id = loop.add_timer(100, [&fired]() {
        fired = true;
    });
    
    // 取消定时器
    loop.cancel_timer(id);
    
    // 等待并运行
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    loop.run_once();
    
    EXPECT_FALSE(fired);
}

// 测试 post 任务
TEST(PerCoreTest, PostTask) {
    EpollPerCoreEventLoop loop;
    
    std::atomic<int> counter{0};
    
    loop.post([&counter]() { counter++; });
    loop.post([&counter]() { counter++; });
    loop.post([&counter]() { counter++; });
    
    loop.run_once();
    
    EXPECT_EQ(counter, 3);
}

// 测试 thread_local 事件循环
TEST(PerCoreTest, CurrentEventLoop) {
    EXPECT_EQ(get_current_event_loop(), nullptr);
    
    EpollPerCoreEventLoop loop;
    set_current_event_loop(&loop);
    
    EXPECT_EQ(get_current_event_loop(), &loop);
    
    set_current_event_loop(nullptr);
    EXPECT_EQ(get_current_event_loop(), nullptr);
}

int main(int argc, char **argv) {
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
