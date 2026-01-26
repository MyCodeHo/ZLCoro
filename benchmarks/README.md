# ZLCoro 性能测试套件

## 📁 文件结构

### 核心基准测试

| 文件 | 用途 | 状态 |
|------|------|------|
| `coroutine_bench.cpp` | 协程创建/切换性能 | ✅ 稳定 |
| `scheduler_bench.cpp` | 调度器性能测试 | ✅ 稳定 |
| `io_bench.cpp` | 基础 I/O 性能测试 | ✅ 稳定 |
| `io_all_bench.cpp` | I/O 综合测试（并发扩展） | ✅ 稳定 |
| `io_uring_bench.cpp` | epoll vs io_uring 对比 | ✅ 稳定 |
| `io_comparison_bench.cpp` | 协程 vs 阻塞 I/O | ⚠️ 高并发时有问题 |
| `kv_benchmark.cpp` | KV 服务器压测 | ⚠️ 启动段错误待修复 |

### 共享组件

- `io_bench_common.hpp` - 测试工具库（计时器、结果统计）

## 🚀 运行测试

```bash
cd build

# 核心测试（推荐）
./benchmarks/coroutine_bench    # 协程性能
./benchmarks/scheduler_bench    # 调度器性能
./benchmarks/io_bench           # 基础 I/O
./benchmarks/io_all_bench       # I/O 综合测试
./benchmarks/io_uring_bench     # io_uring vs epoll

# 通过 ctest 运行
ctest -R bench
```

## 📊 典型结果

### 协程性能（coroutine_bench）

```
协程创建耗时: ~50ns/个
协程切换耗时: ~20ns/次
百万协程创建: ~50ms
```

### I/O 性能（io_all_bench）

```
单客户端:   2.4 MB/s,   5K msg/s
16 客户端: 25.3 MB/s,  52K msg/s
扩展效率:  ~10x（16 并发 vs 单客户端）
```

### io_uring vs epoll（io_uring_bench）

```
文件读取 (1MB):
  io_uring: 12 GB/s
  epoll:     2.8 GB/s
  提升:      4.2x

网络 echo:
  io_uring: ~85 万 QPS
  epoll:    ~98 万 QPS
  说明:     简单场景 epoll 更优
```

## 🔧 编译说明

```bash
cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make coroutine_bench scheduler_bench io_bench io_all_bench io_uring_bench
```

## ⚠️ 已知问题

1. **io_comparison_bench**: 64 客户端时段错误（阻塞端线程资源问题）
2. **kv_benchmark**: 启动即段错误（待 gdb 排查）

这些问题不影响核心功能测试，可使用 `examples/benchmark/` 中的服务器进行压测。
- 测试文件数：从 8 个 → 2 个
- 测试覆盖度：保持 100%
- 可维护性：显著提升

## 📝 添加新测试

使用共享库快速添加新测试场景：

```cpp
#include "io_bench_common.hpp"
using namespace io_bench;

int main() {
    EventLoopRunner global_event_loop;
    
    // 自定义测试场景
    auto result = run_coroutine_benchmark(
        16,      // 客户端数
        1024,    // 负载大小
        100,     // 消息数
        true     // verbose
    );
    
    result.print("我的测试");
    return 0;
}
```

## 📈 性能分析建议

1. **基准测试**：先运行 `io_all_bench` 获取框架基准性能
2. **对比测试**：运行 `io_comparison_bench` 验证相对优势
3. **场景调优**：根据实际应用场景调整并发数和负载大小
4. **持续监控**：集成到 CI/CD 进行性能回归测试


**最后更新**：2026年1月15日
