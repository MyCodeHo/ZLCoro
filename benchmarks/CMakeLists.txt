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

# 添加到 CTest
add_test(NAME bench_coroutine COMMAND coroutine_bench)
add_test(NAME bench_scheduler COMMAND scheduler_bench)
add_test(NAME bench_io COMMAND io_bench)

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
