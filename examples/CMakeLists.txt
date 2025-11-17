# Examples for ZLCoro

# 示例 1: 基础 Task 用法
add_executable(01_basic_task 01_basic_task.cpp)
target_link_libraries(01_basic_task PRIVATE ZLCoro::zlcoro)

# 示例 2: Generator 用法
add_executable(02_generator_example 02_generator_example.cpp)
target_link_libraries(02_generator_example PRIVATE ZLCoro::zlcoro)
