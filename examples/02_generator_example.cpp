#include "zlcoro/core/generator.hpp"
#include <iostream>
#include <string>
#include <vector>

using namespace zlcoro;

// =============================================================================
// 示例 1: 最简单的 Generator
// =============================================================================
Generator<int> simple_range() {
    co_yield 1;
    co_yield 2;
    co_yield 3;
}

void example1_simple() {
    std::cout << "\n=== 示例 1: 简单的 Generator ===\n";
    
    for (int x : simple_range()) {
        std::cout << x << " ";
    }
    std::cout << "\n";
    // 输出: 1 2 3
}

// =============================================================================
// 示例 2: 使用循环生成序列 (range 函数)
// =============================================================================
Generator<int> range(int start, int end) {
    for (int i = start; i < end; ++i) {
        co_yield i;
    }
}

void example2_range() {
    std::cout << "\n=== 示例 2: Range 函数 ===\n";
    
    std::cout << "range(0, 10): ";
    for (int x : range(0, 10)) {
        std::cout << x << " ";
    }
    std::cout << "\n";
    // 输出: 0 1 2 3 4 5 6 7 8 9
}

// =============================================================================
// 示例 3: 斐波那契数列
// =============================================================================
Generator<int> fibonacci(int n) {
    int a = 0, b = 1;
    for (int i = 0; i < n; ++i) {
        co_yield a;
        int next = a + b;
        a = b;
        b = next;
    }
}

void example3_fibonacci() {
    std::cout << "\n=== 示例 3: 斐波那契数列 ===\n";
    
    std::cout << "前 15 个斐波那契数: ";
    for (int x : fibonacci(15)) {
        std::cout << x << " ";
    }
    std::cout << "\n";
    // 输出: 0 1 1 2 3 5 8 13 21 34 55 89 144 233 377
}

// =============================================================================
// 示例 4: 条件过滤 (只生成偶数)
// =============================================================================
Generator<int> even_numbers(int n) {
    for (int i = 0; i < n; ++i) {
        if (i % 2 == 0) {
            co_yield i;
        }
    }
}

void example4_filter() {
    std::cout << "\n=== 示例 4: 过滤 (偶数) ===\n";
    
    std::cout << "0-20 中的偶数: ";
    for (int x : even_numbers(20)) {
        std::cout << x << " ";
    }
    std::cout << "\n";
    // 输出: 0 2 4 6 8 10 12 14 16 18
}

// =============================================================================
// 示例 5: 惰性求值 (无限序列)
// =============================================================================
// 注意: 这个 Generator 理论上可以无限生成数字
// 但我们可以在使用时提前退出
Generator<int> infinite_sequence() {
    int i = 0;
    while (true) {
        co_yield i++;
    }
}

void example5_lazy() {
    std::cout << "\n=== 示例 5: 惰性求值 (无限序列) ===\n";
    
    std::cout << "前 10 个数字: ";
    int count = 0;
    for (int x : infinite_sequence()) {
        std::cout << x << " ";
        if (++count >= 10) {
            break;  // 提前退出
        }
    }
    std::cout << "\n";
    // 输出: 0 1 2 3 4 5 6 7 8 9
}

// =============================================================================
// 示例 6: 字符串生成器
// =============================================================================
Generator<std::string> word_generator() {
    co_yield "Hello";
    co_yield "World";
    co_yield "from";
    co_yield "Generator";
}

void example6_strings() {
    std::cout << "\n=== 示例 6: 字符串生成器 ===\n";
    
    std::cout << "生成的单词: ";
    for (const auto& word : word_generator()) {
        std::cout << word << " ";
    }
    std::cout << "\n";
    // 输出: Hello World from Generator
}

// =============================================================================
// 示例 7: 生成排列组合
// =============================================================================
Generator<std::pair<int, int>> pairs(int n) {
    for (int i = 0; i < n; ++i) {
        for (int j = 0; j < n; ++j) {
            co_yield std::make_pair(i, j);
        }
    }
}

void example7_pairs() {
    std::cout << "\n=== 示例 7: 生成坐标对 ===\n";
    
    std::cout << "3x3 网格的坐标:\n";
    for (const auto& [x, y] : pairs(3)) {
        std::cout << "(" << x << "," << y << ") ";
        if (y == 2) {
            std::cout << "\n";  // 换行
        }
    }
}

// =============================================================================
// 示例 8: 使用 Generator 收集数据
// =============================================================================
Generator<int> squares(int n) {
    for (int i = 1; i <= n; ++i) {
        co_yield i * i;
    }
}

void example8_collect() {
    std::cout << "\n=== 示例 8: 收集数据到容器 ===\n";
    
    // 将 Generator 的输出收集到 vector
    std::vector<int> result;
    for (int x : squares(10)) {
        result.push_back(x);
    }
    
    std::cout << "前 10 个平方数: ";
    for (int x : result) {
        std::cout << x << " ";
    }
    std::cout << "\n";
    // 输出: 1 4 9 16 25 36 49 64 81 100
}

// =============================================================================
// 示例 9: 读取文件行 (模拟)
// =============================================================================
Generator<std::string> read_lines() {
    // 模拟从文件读取行
    // 实际应用中,这里可以真正读取文件
    co_yield "Line 1: First line";
    co_yield "Line 2: Second line";
    co_yield "Line 3: Third line";
    co_yield "Line 4: Fourth line";
}

void example9_file_lines() {
    std::cout << "\n=== 示例 9: 模拟读取文件行 ===\n";
    
    int line_number = 1;
    for (const auto& line : read_lines()) {
        std::cout << "[" << line_number++ << "] " << line << "\n";
    }
}

// =============================================================================
// 示例 10: 质数生成器 (埃拉托斯特尼筛法的简化版)
// =============================================================================
Generator<int> primes(int max) {
    if (max >= 2) co_yield 2;
    
    for (int candidate = 3; candidate <= max; candidate += 2) {
        bool is_prime = true;
        // 简单的质数检测
        for (int divisor = 3; divisor * divisor <= candidate; divisor += 2) {
            if (candidate % divisor == 0) {
                is_prime = false;
                break;
            }
        }
        if (is_prime) {
            co_yield candidate;
        }
    }
}

void example10_primes() {
    std::cout << "\n=== 示例 10: 质数生成器 ===\n";
    
    std::cout << "100 以内的质数: ";
    for (int p : primes(100)) {
        std::cout << p << " ";
    }
    std::cout << "\n";
}

// =============================================================================
// 主函数
// =============================================================================
int main() {
    std::cout << "==============================================\n";
    std::cout << "      Generator<T> 使用示例\n";
    std::cout << "==============================================\n";

    example1_simple();
    example2_range();
    example3_fibonacci();
    example4_filter();
    example5_lazy();
    example6_strings();
    example7_pairs();
    example8_collect();
    example9_file_lines();
    example10_primes();

    std::cout << "\n==============================================\n";
    std::cout << "所有示例运行完成!\n";
    std::cout << "==============================================\n";

    return 0;
}
