# Examples for ZLCoro
# 提供协程框架的各种使用示例

# ========================================
# 基础示例
# ========================================

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

# 示例 5: 同步原语
add_executable(05_sync_primitives 05_sync_primitives.cpp)
target_link_libraries(05_sync_primitives PRIVATE 
    ZLCoro::zlcoro
    pthread
)

# ========================================
# 网络示例
# ========================================

# Echo 服务器示例
add_executable(echo_server network/echo_server.cpp)
target_link_libraries(echo_server PRIVATE 
    ZLCoro::zlcoro
    pthread
)

# Echo 客户端示例
add_executable(echo_client network/echo_client.cpp)
target_link_libraries(echo_client PRIVATE 
    ZLCoro::zlcoro
    pthread
)

# HTTP 服务器示例
add_executable(http_server network/http_server.cpp)
target_link_libraries(http_server PRIVATE 
    ZLCoro::zlcoro
    pthread
)
