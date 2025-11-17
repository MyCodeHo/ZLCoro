#include "zlcoro/core/task.hpp"
#include <gtest/gtest.h>
#include <string>

using namespace zlcoro;

// ============================================================================
// 基础测试：测试 Task 的基本功能
// ============================================================================

// 测试返回 int 的简单协程
TEST(TaskTest, SimpleIntReturn) {
    auto simple_task = []() -> Task<int> {
        co_return 42;
    };

    auto task = simple_task();
    int result = task.sync_wait();
    
    EXPECT_EQ(result, 42);
}

// 测试返回 void 的协程
TEST(TaskTest, VoidReturn) {
    bool executed = false;
    
    auto void_task = [&executed]() -> Task<void> {
        executed = true;
        co_return;
    };

    auto task = void_task();
    task.sync_wait();
    
    EXPECT_TRUE(executed);
}

// 测试返回字符串的协程
TEST(TaskTest, StringReturn) {
    auto string_task = []() -> Task<std::string> {
        co_return "Hello, ZLCoro!";
    };

    auto task = string_task();
    std::string result = task.sync_wait();
    
    EXPECT_EQ(result, "Hello, ZLCoro!");
}

// 测试返回引用的协程
TEST(TaskTest, ReferenceReturn) {
    static int value = 100;
    
    auto ref_task = []() -> Task<int&> {
        co_return value;
    };

    auto task = ref_task();
    int& result = task.sync_wait();
    
    EXPECT_EQ(result, 100);
    EXPECT_EQ(&result, &value);  // 确保是同一个对象
    
    result = 200;
    EXPECT_EQ(value, 200);  // 修改引用应该影响原对象
}

// ============================================================================
// 协程链式调用测试：co_await 另一个 Task
// ============================================================================

// 测试 co_await 单个 Task
TEST(TaskTest, AwaitTask) {
    auto inner_task = []() -> Task<int> {
        co_return 10;
    };

    auto outer_task = [&inner_task]() -> Task<int> {
        int value = co_await inner_task();
        co_return value * 2;
    };

    auto task = outer_task();
    int result = task.sync_wait();
    
    EXPECT_EQ(result, 20);
}

// 测试多层嵌套的 co_await
TEST(TaskTest, MultiLevelAwait) {
    auto level1 = []() -> Task<int> {
        co_return 1;
    };

    auto level2 = [&level1]() -> Task<int> {
        int v1 = co_await level1();
        co_return v1 + 10;
    };

    auto level3 = [&level2]() -> Task<int> {
        int v2 = co_await level2();
        co_return v2 + 100;
    };

    auto task = level3();
    int result = task.sync_wait();
    
    EXPECT_EQ(result, 111);  // 1 + 10 + 100
}

// 测试在同一个协程中 await 多个 Task
TEST(TaskTest, MultipleAwait) {
    auto task1 = []() -> Task<int> {
        co_return 10;
    };

    auto task2 = []() -> Task<int> {
        co_return 20;
    };

    auto main_task = [&task1, &task2]() -> Task<int> {
        int v1 = co_await task1();
        int v2 = co_await task2();
        co_return v1 + v2;
    };

    auto task = main_task();
    int result = task.sync_wait();
    
    EXPECT_EQ(result, 30);
}

// ============================================================================
// 异常处理测试
// ============================================================================

// 测试协程中抛出的异常能被正确捕获
TEST(TaskTest, ExceptionHandling) {
    auto throwing_task = []() -> Task<int> {
        throw std::runtime_error("Test exception");
        co_return 42;  // 永远不会执行
    };

    auto task = throwing_task();
    
    EXPECT_THROW({
        task.sync_wait();
    }, std::runtime_error);
}

// 测试异常在 co_await 时传播
TEST(TaskTest, ExceptionPropagation) {
    auto inner_task = []() -> Task<int> {
        throw std::runtime_error("Inner exception");
        co_return 10;
    };

    auto outer_task = [&inner_task]() -> Task<int> {
        int value = co_await inner_task();  // 这里会抛出异常
        co_return value * 2;  // 永远不会执行
    };

    auto task = outer_task();
    
    EXPECT_THROW({
        task.sync_wait();
    }, std::runtime_error);
}

// 测试在协程中捕获异常
TEST(TaskTest, ExceptionCatch) {
    auto throwing_task = []() -> Task<int> {
        throw std::runtime_error("Test exception");
        co_return 10;
    };

    auto catching_task = [&throwing_task]() -> Task<int> {
        try {
            co_return co_await throwing_task();
        } catch (const std::runtime_error&) {
            co_return -1;  // 捕获异常后返回默认值
        }
    };

    auto task = catching_task();
    int result = task.sync_wait();
    
    EXPECT_EQ(result, -1);
}

// ============================================================================
// 移动语义测试
// ============================================================================

// 测试 Task 的移动构造
TEST(TaskTest, MoveConstruction) {
    auto simple_task = []() -> Task<int> {
        co_return 42;
    };

    auto task1 = simple_task();
    EXPECT_TRUE(task1.valid());
    
    auto task2 = std::move(task1);
    EXPECT_FALSE(task1.valid());  // 移动后原对象无效
    EXPECT_TRUE(task2.valid());
    
    int result = task2.sync_wait();
    EXPECT_EQ(result, 42);
}

// 测试 Task 的移动赋值
TEST(TaskTest, MoveAssignment) {
    auto task_factory = [](int value) -> Task<int> {
        co_return value;
    };

    auto task1 = task_factory(10);
    auto task2 = task_factory(20);
    
    task1 = std::move(task2);
    
    int result = task1.sync_wait();
    EXPECT_EQ(result, 20);
}

// ============================================================================
// 复杂场景测试
// ============================================================================

// 测试返回复杂对象（验证移动语义）
TEST(TaskTest, ComplexObjectReturn) {
    struct LargeObject {
        std::string data;
        int value;
        
        LargeObject(std::string d, int v) 
            : data(std::move(d)), value(v) {}
    };

    auto complex_task = []() -> Task<LargeObject> {
        co_return LargeObject("large data", 999);
    };

    auto task = complex_task();
    auto result = task.sync_wait();
    
    EXPECT_EQ(result.data, "large data");
    EXPECT_EQ(result.value, 999);
}

// 测试条件分支
TEST(TaskTest, ConditionalBranch) {
    auto branching_task = [](bool condition) -> Task<int> {
        if (condition) {
            co_return 1;
        } else {
            co_return 2;
        }
    };

    auto task1 = branching_task(true);
    auto task2 = branching_task(false);
    
    EXPECT_EQ(task1.sync_wait(), 1);
    EXPECT_EQ(task2.sync_wait(), 2);
}

// 测试递归协程
TEST(TaskTest, RecursiveCoroutine) {
    // 计算阶乘的递归协程
    std::function<Task<int>(int)> factorial;
    factorial = [&factorial](int n) -> Task<int> {
        if (n <= 1) {
            co_return 1;
        }
        int prev = co_await factorial(n - 1);
        co_return n * prev;
    };

    auto task = factorial(5);
    int result = task.sync_wait();
    
    EXPECT_EQ(result, 120);  // 5! = 120
}

// 主函数
int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
