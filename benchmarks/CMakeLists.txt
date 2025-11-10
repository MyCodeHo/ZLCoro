# Benchmarks for ZLCoro

# 查找 Google Benchmark
find_package(benchmark QUIET)

if(NOT benchmark_FOUND)
    message(STATUS "Google Benchmark not found, benchmarks will be skipped")
    return()
endif()

# 示例 benchmark (暂时为空)
# add_executable(task_benchmark task_benchmark.cpp)
# target_link_libraries(task_benchmark PRIVATE ZLCoro::zlcoro benchmark::benchmark)
