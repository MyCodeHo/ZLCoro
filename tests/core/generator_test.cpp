#include "zlcoro/core/generator.hpp"
#include <gtest/gtest.h>
#include <vector>
#include <string>

using namespace zlcoro;

// =============================================================================
// 基础功能测试
// =============================================================================

// 测试 1: 简单的整数序列
TEST(GeneratorTest, SimpleIntSequence) {
    auto gen = []() -> Generator<int> {
        co_yield 1;
        co_yield 2;
        co_yield 3;
    };

    std::vector<int> result;
    for (int x : gen()) {
        result.push_back(x);
    }

    EXPECT_EQ(result, (std::vector<int>{1, 2, 3}));
}

// 测试 2: 使用循环生成序列
TEST(GeneratorTest, Range) {
    auto range = [](int n) -> Generator<int> {
        for (int i = 0; i < n; ++i) {
            co_yield i;
        }
    };

    std::vector<int> result;
    for (int x : range(5)) {
        result.push_back(x);
    }

    EXPECT_EQ(result, (std::vector<int>{0, 1, 2, 3, 4}));
}

// 测试 3: 空的 Generator
TEST(GeneratorTest, EmptyGenerator) {
    auto empty = []() -> Generator<int> {
        co_return;  // 不 yield 任何值
    };

    std::vector<int> result;
    for (int x : empty()) {
        result.push_back(x);
    }

    EXPECT_TRUE(result.empty());
}

// 测试 4: 单个值
TEST(GeneratorTest, SingleValue) {
    auto single = []() -> Generator<int> {
        co_yield 42;
    };

    std::vector<int> result;
    for (int x : single()) {
        result.push_back(x);
    }

    EXPECT_EQ(result, (std::vector<int>{42}));
}

// =============================================================================
// 不同类型测试
// =============================================================================

// 测试 5: 字符串 Generator
TEST(GeneratorTest, StringGenerator) {
    auto strings = []() -> Generator<std::string> {
        co_yield "hello";
        co_yield "world";
        co_yield "generator";
    };

    std::vector<std::string> result;
    for (const auto& s : strings()) {
        result.push_back(s);
    }

    EXPECT_EQ(result, (std::vector<std::string>{"hello", "world", "generator"}));
}

// 测试 6: 自定义类型
struct Point {
    int x, y;
    bool operator==(const Point& other) const {
        return x == other.x && y == other.y;
    }
};

TEST(GeneratorTest, CustomType) {
    auto points = []() -> Generator<Point> {
        co_yield Point{0, 0};
        co_yield Point{1, 1};
        co_yield Point{2, 2};
    };

    std::vector<Point> result;
    for (const auto& p : points()) {
        result.push_back(p);
    }

    EXPECT_EQ(result.size(), 3);
    EXPECT_EQ(result[0], (Point{0, 0}));
    EXPECT_EQ(result[1], (Point{1, 1}));
    EXPECT_EQ(result[2], (Point{2, 2}));
}

// =============================================================================
// 惰性求值测试
// =============================================================================

// 测试 7: 验证惰性求值 (只在需要时才生成)
TEST(GeneratorTest, LazyEvaluation) {
    int call_count = 0;

    auto lazy = [&call_count](int n) -> Generator<int> {
        for (int i = 0; i < n; ++i) {
            ++call_count;  // 记录生成次数
            co_yield i;
        }
    };

    auto gen = lazy(10);
    EXPECT_EQ(call_count, 0);  // 创建 Generator 时不执行

    auto it = gen.begin();
    EXPECT_EQ(call_count, 1);  // 第一次迭代时才执行

    ++it;
    EXPECT_EQ(call_count, 2);  // 每次 ++ 才生成下一个值

    ++it;
    EXPECT_EQ(call_count, 3);
}

// 测试 8: 提前退出 (不消费所有值)
TEST(GeneratorTest, EarlyExit) {
    int yield_count = 0;

    auto counting = [&yield_count]() -> Generator<int> {
        for (int i = 0; i < 100; ++i) {
            ++yield_count;
            co_yield i;
        }
    };

    int sum = 0;
    for (int x : counting()) {
        sum += x;
        if (sum > 10) {
            break;  // 提前退出
        }
    }

    // 应该只生成了前几个值,而不是全部 100 个
    EXPECT_LT(yield_count, 100);
    EXPECT_LE(yield_count, 6);  // 0+1+2+3+4+5=15 > 10
}

// =============================================================================
// 复杂场景测试
// =============================================================================

// 测试 9: 斐波那契数列
TEST(GeneratorTest, Fibonacci) {
    auto fibonacci = [](int n) -> Generator<int> {
        int a = 0, b = 1;
        for (int i = 0; i < n; ++i) {
            co_yield a;
            int next = a + b;
            a = b;
            b = next;
        }
    };

    std::vector<int> result;
    for (int x : fibonacci(8)) {
        result.push_back(x);
    }

    EXPECT_EQ(result, (std::vector<int>{0, 1, 1, 2, 3, 5, 8, 13}));
}

// 测试 10: 条件生成
TEST(GeneratorTest, ConditionalYield) {
    auto even_numbers = [](int n) -> Generator<int> {
        for (int i = 0; i < n; ++i) {
            if (i % 2 == 0) {
                co_yield i;
            }
        }
    };

    std::vector<int> result;
    for (int x : even_numbers(10)) {
        result.push_back(x);
    }

    EXPECT_EQ(result, (std::vector<int>{0, 2, 4, 6, 8}));
}

// 测试 11: 嵌套循环
TEST(GeneratorTest, NestedLoops) {
    auto pairs = []() -> Generator<std::pair<int, int>> {
        for (int i = 0; i < 3; ++i) {
            for (int j = 0; j < 2; ++j) {
                co_yield std::make_pair(i, j);
            }
        }
    };

    std::vector<std::pair<int, int>> result;
    for (const auto& p : pairs()) {
        result.push_back(p);
    }

    EXPECT_EQ(result.size(), 6);
    EXPECT_EQ(result[0], std::make_pair(0, 0));
    EXPECT_EQ(result[1], std::make_pair(0, 1));
    EXPECT_EQ(result[2], std::make_pair(1, 0));
    EXPECT_EQ(result[5], std::make_pair(2, 1));
}

// =============================================================================
// 异常处理测试
// =============================================================================

// 测试 12: 生成过程中抛出异常
TEST(GeneratorTest, ExceptionDuringGeneration) {
    auto throwing = []() -> Generator<int> {
        co_yield 1;
        co_yield 2;
        throw std::runtime_error("error in generator");
        co_yield 3;  // 不会执行到这里
    };

    std::vector<int> result;
    EXPECT_THROW({
        for (int x : throwing()) {
            result.push_back(x);
        }
    }, std::runtime_error);

    // 应该收集到前两个值
    EXPECT_EQ(result, (std::vector<int>{1, 2}));
}

// 测试 13: 第一个 yield 之前就抛出异常
TEST(GeneratorTest, ExceptionBeforeFirstYield) {
    auto throwing = []() -> Generator<int> {
        throw std::runtime_error("error before yield");
        co_yield 1;  // 不会执行到这里
    };

    EXPECT_THROW({
        auto gen = throwing();
        for (int x : gen) {
            (void)x;
        }
    }, std::runtime_error);
}

// =============================================================================
// 移动语义测试
// =============================================================================

// 测试 14: 移动构造
TEST(GeneratorTest, MoveConstruction) {
    auto range = [](int n) -> Generator<int> {
        for (int i = 0; i < n; ++i) {
            co_yield i;
        }
    };

    auto gen1 = range(3);
    auto gen2 = std::move(gen1);  // 移动构造

    std::vector<int> result;
    for (int x : gen2) {
        result.push_back(x);
    }

    EXPECT_EQ(result, (std::vector<int>{0, 1, 2}));
}

// 测试 15: 移动赋值
TEST(GeneratorTest, MoveAssignment) {
    auto range = [](int n) -> Generator<int> {
        for (int i = 0; i < n; ++i) {
            co_yield i;
        }
    };

    auto gen1 = range(3);
    auto gen2 = range(5);
    
    gen2 = std::move(gen1);  // 移动赋值

    std::vector<int> result;
    for (int x : gen2) {
        result.push_back(x);
    }

    EXPECT_EQ(result, (std::vector<int>{0, 1, 2}));
}

// =============================================================================
// 迭代器测试
// =============================================================================

// 测试 16: 手动使用迭代器
TEST(GeneratorTest, ManualIteration) {
    auto range = [](int n) -> Generator<int> {
        for (int i = 0; i < n; ++i) {
            co_yield i;
        }
    };

    auto gen = range(5);
    auto it = gen.begin();
    auto end = gen.end();

    EXPECT_NE(it, end);
    EXPECT_EQ(*it, 0);

    ++it;
    EXPECT_NE(it, end);
    EXPECT_EQ(*it, 1);

    ++it;
    EXPECT_EQ(*it, 2);

    ++it;
    EXPECT_EQ(*it, 3);

    ++it;
    EXPECT_EQ(*it, 4);

    ++it;
    EXPECT_EQ(it, end);  // 迭代结束
}
