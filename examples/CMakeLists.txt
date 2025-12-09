# Examples for ZLCoro

# 示例 1: 基础 Task 用法
add_executable(01_basic_task 01_basic_task.cpp)
target_link_libraries(01_basic_task PRIVATE ZLCoro::zlcoro)

# 示例 2: Generator 用法
add_executable(02_generator_example 02_generator_example.cpp)
target_link_libraries(02_generator_example PRIVATE ZLCoro::zlcoro)

# 示例 3: Scheduler 用法
add_executable(03_scheduler_example 03_scheduler_example.cpp)
target_link_libraries(03_scheduler_example PRIVATE 
    ZLCoro::zlcoro
    pthread  # 调度器需要 pthread
)

# 示例 4: 异步 I/O 用法
add_executable(04_async_io_example 04_async_io_example.cpp)
target_link_libraries(04_async_io_example PRIVATE 
    ZLCoro::zlcoro
    pthread
)

# 简单测试
add_executable(simple_io_test simple_io_test.cpp)
target_link_libraries(simple_io_test PRIVATE 
    ZLCoro::zlcoro
    pthread
)
