# Benchmarks for ZLCoro
# 提供协程、调度器、I/O 性能基准测试

# ========================================
# 独立基准测试（不依赖 Google Benchmark）
# ========================================

# 协程性能基准测试
add_executable(coroutine_bench coroutine_bench.cpp)
target_link_libraries(coroutine_bench PRIVATE ZLCoro::zlcoro)
target_compile_features(coroutine_bench PRIVATE cxx_std_20)

# 调度器性能基准测试
add_executable(scheduler_bench scheduler_bench.cpp)
target_link_libraries(scheduler_bench PRIVATE ZLCoro::zlcoro)
target_compile_features(scheduler_bench PRIVATE cxx_std_20)

# I/O 性能基准测试
add_executable(io_bench io_bench.cpp)
target_link_libraries(io_bench PRIVATE ZLCoro::zlcoro)
target_compile_features(io_bench PRIVATE cxx_std_20)

# I/O 综合测试（网络+文件，所有场景）
add_executable(io_all_bench io_all_bench.cpp)
target_link_libraries(io_all_bench PRIVATE ZLCoro::zlcoro)
target_compile_features(io_all_bench PRIVATE cxx_std_20)

# I/O 对比测试（协程 vs 传统阻塞I/O）
add_executable(io_comparison_bench io_comparison_bench.cpp)
target_link_libraries(io_comparison_bench PRIVATE ZLCoro::zlcoro pthread)
target_compile_features(io_comparison_bench PRIVATE cxx_std_20)

# io_uring vs epoll 性能对比测试
add_executable(io_uring_bench io_uring_bench.cpp)
target_link_libraries(io_uring_bench PRIVATE ZLCoro::zlcoro pthread uring)
target_compile_features(io_uring_bench PRIVATE cxx_std_20)

# KV 服务器高并发性能测试
add_executable(kv_benchmark kv_benchmark.cpp)
target_link_libraries(kv_benchmark PRIVATE ZLCoro::zlcoro pthread)
target_compile_features(kv_benchmark PRIVATE cxx_std_20)

# 如果有 io_uring 支持，链接 uring 库
find_library(URING_LIB uring)
if(URING_LIB)
    target_link_libraries(kv_benchmark PRIVATE ${URING_LIB})
endif()

# 添加到 CTest
add_test(NAME bench_coroutine COMMAND coroutine_bench)
add_test(NAME bench_scheduler COMMAND scheduler_bench)
add_test(NAME bench_io COMMAND io_bench)
add_test(NAME bench_io_all COMMAND io_all_bench)
add_test(NAME bench_io_comparison COMMAND io_comparison_bench)

# ========================================
# Google Benchmark 集成（可选）
# ========================================

find_package(benchmark QUIET)

if(benchmark_FOUND)
    message(STATUS "Google Benchmark found, enabling advanced benchmarks")
    
    # 如果需要更详细的基准测试可以在这里添加
    # add_executable(task_benchmark task_benchmark.cpp)
    # target_link_libraries(task_benchmark PRIVATE ZLCoro::zlcoro benchmark::benchmark)
else()
    message(STATUS "Google Benchmark not found, using standalone benchmarks only")
endif()
