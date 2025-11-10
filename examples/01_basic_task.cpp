// ============================================================================
// 示例 1: Task 基础用法
// ============================================================================
// 本示例展示如何创建和使用最基本的 Task 协程

#include "zlcoro/core/task.hpp"
#include <functional>
#include <iostream>

using namespace zlcoro;

// ----------------------------------------------------------------------------
// 示例 1: 最简单的协程 - 返回一个整数
// ----------------------------------------------------------------------------
Task<int> simple_computation() {
    std::cout << "开始计算...\n";
    co_return 42;  // co_return 表示协程返回
}

// ----------------------------------------------------------------------------
// 示例 2: void 返回类型的协程
// ----------------------------------------------------------------------------
Task<void> print_message() {
    std::cout << "这是一个 void 协程\n";
    co_return;  // void 协程使用 co_return;
}

// ----------------------------------------------------------------------------
// 示例 3: 协程链式调用 - co_await 其他协程
// ----------------------------------------------------------------------------
Task<int> get_number() {
    std::cout << "获取数字...\n";
    co_return 10;
}

Task<int> double_number() {
    std::cout << "准备获取并加倍数字...\n";
    
    // co_await 会挂起当前协程，等待 get_number() 完成
    int num = co_await get_number();
    
    std::cout << "收到数字: " << num << "，开始加倍...\n";
    co_return num * 2;
}

// ----------------------------------------------------------------------------
// 示例 4: 多个 co_await - 顺序执行多个异步操作
// ----------------------------------------------------------------------------
Task<int> fetch_user_age() {
    std::cout << "从数据库获取用户年龄...\n";
    co_return 25;
}

Task<std::string> fetch_user_name() {
    std::cout << "从数据库获取用户名...\n";
    co_return "张三";
}

Task<void> print_user_info() {
    std::cout << "\n=== 开始获取用户信息 ===\n";
    
    // 顺序等待多个协程
    int age = co_await fetch_user_age();
    std::string name = co_await fetch_user_name();
    
    std::cout << "用户信息: " << name << ", " << age << " 岁\n";
    std::cout << "=== 完成 ===\n";
}

// ----------------------------------------------------------------------------
// 示例 5: 异常处理
// ----------------------------------------------------------------------------
Task<int> may_throw(bool should_throw) {
    if (should_throw) {
        throw std::runtime_error("发生错误！");
    }
    co_return 100;
}

Task<int> handle_error() {
    std::cout << "\n=== 测试异常处理 ===\n";
    
    try {
        // 尝试调用可能抛出异常的协程
        int result = co_await may_throw(false);
        std::cout << "成功获取结果: " << result << "\n";
        
        // 这次会抛出异常
        result = co_await may_throw(true);
        std::cout << "不会执行到这里\n";
        
    } catch (const std::exception& e) {
        std::cout << "捕获异常: " << e.what() << "\n";
        co_return -1;  // 返回错误码
    }
    
    co_return 0;
}

// ----------------------------------------------------------------------------
// 示例 6: 协程组合 - 构建复杂的异步流程
// ----------------------------------------------------------------------------
Task<int> step1() {
    std::cout << "  步骤 1: 初始化...\n";
    co_return 1;
}

Task<int> step2(int prev) {
    std::cout << "  步骤 2: 处理数据 (输入: " << prev << ")...\n";
    co_return prev + 10;
}

Task<int> step3(int prev) {
    std::cout << "  步骤 3: 最终计算 (输入: " << prev << ")...\n";
    co_return prev * 2;
}

Task<int> complex_workflow() {
    std::cout << "\n=== 执行复杂工作流 ===\n";
    
    int result1 = co_await step1();
    int result2 = co_await step2(result1);
    int result3 = co_await step3(result2);
    
    std::cout << "工作流完成，最终结果: " << result3 << "\n";
    co_return result3;
}

// ----------------------------------------------------------------------------
// 示例 7: 递归协程 - 计算斐波那契数
// ----------------------------------------------------------------------------
// 注意：需要使用 std::function 来支持递归 lambda
std::function<Task<int>(int)> fibonacci;

void init_fibonacci() {
    fibonacci = [](int n) -> Task<int> {
        std::cout << "计算 fib(" << n << ")\n";
        
        if (n <= 1) {
            co_return n;
        }
        
        // 递归调用自己
        int a = co_await fibonacci(n - 1);
        int b = co_await fibonacci(n - 2);
        
        co_return a + b;
    };
}

// ----------------------------------------------------------------------------
// 主函数 - 运行所有示例
// ----------------------------------------------------------------------------
int main() {
    std::cout << "========================================\n";
    std::cout << "ZLCoro Task 基础示例\n";
    std::cout << "========================================\n\n";

    // 示例 1: 简单计算
    {
        std::cout << "--- 示例 1: 简单协程 ---\n";
        auto task = simple_computation();
        int result = task.sync_wait();  // sync_wait() 阻塞等待协程完成
        std::cout << "结果: " << result << "\n\n";
    }

    // 示例 2: void 协程
    {
        std::cout << "--- 示例 2: void 协程 ---\n";
        auto task = print_message();
        task.sync_wait();
        std::cout << "\n";
    }

    // 示例 3: 协程链式调用
    {
        std::cout << "--- 示例 3: 链式调用 ---\n";
        auto task = double_number();
        int result = task.sync_wait();
        std::cout << "最终结果: " << result << "\n\n";
    }

    // 示例 4: 多个 co_await
    {
        std::cout << "--- 示例 4: 多个 co_await ---\n";
        auto task = print_user_info();
        task.sync_wait();
        std::cout << "\n";
    }

    // 示例 5: 异常处理
    {
        std::cout << "--- 示例 5: 异常处理 ---\n";
        auto task = handle_error();
        int result = task.sync_wait();
        std::cout << "错误码: " << result << "\n\n";
    }

    // 示例 6: 复杂工作流
    {
        std::cout << "--- 示例 6: 复杂工作流 ---\n";
        auto task = complex_workflow();
        int result = task.sync_wait();
        std::cout << "\n";
    }

    // 示例 7: 递归协程
    {
        std::cout << "--- 示例 7: 递归协程 (斐波那契) ---\n";
        init_fibonacci();
        auto task = fibonacci(6);
        int result = task.sync_wait();
        std::cout << "fib(6) = " << result << "\n\n";
    }

    std::cout << "========================================\n";
    std::cout << "所有示例运行完毕！\n";
    std::cout << "========================================\n";

    return 0;
}
