# Tests for ZLCoro

# 查找 Google Test
find_package(GTest QUIET)

if(NOT GTest_FOUND)
    # 如果没有安装 GTest，使用 FetchContent 下载
    include(FetchContent)
    FetchContent_Declare(
        googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG v1.14.0
    )
    # 对于 Windows: 防止覆盖父项目的编译器/链接器设置
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)
endif()

# 启用测试
enable_testing()

# Core 测试
add_executable(task_test core/task_test.cpp)
target_link_libraries(task_test PRIVATE 
    ZLCoro::zlcoro
    GTest::gtest
    GTest::gtest_main
)

# 添加到 CTest
add_test(NAME TaskTest COMMAND task_test)

# Generator 测试
add_executable(generator_test core/generator_test.cpp)
target_link_libraries(generator_test PRIVATE 
    ZLCoro::zlcoro
    GTest::gtest
    GTest::gtest_main
)
add_test(NAME GeneratorTest COMMAND generator_test)

# Scheduler 测试
add_executable(scheduler_test scheduler/scheduler_test.cpp)
target_link_libraries(scheduler_test PRIVATE 
    ZLCoro::zlcoro
    GTest::gtest
    GTest::gtest_main
    pthread  # 线程池需要 pthread
)
add_test(NAME SchedulerTest COMMAND scheduler_test)

# I/O 测试
add_executable(io_test io/io_test.cpp)
target_link_libraries(io_test PRIVATE 
    ZLCoro::zlcoro
    GTest::gtest
    GTest::gtest_main
    pthread
)
add_test(NAME IOTest COMMAND io_test)

# Sync 测试
add_executable(sync_test sync/sync_test.cpp)
target_link_libraries(sync_test PRIVATE 
    ZLCoro::zlcoro
    GTest::gtest
    GTest::gtest_main
    pthread
)
add_test(NAME SyncTest COMMAND sync_test)

# Performance 测试（Phase 6: 性能优化）
add_executable(performance_test scheduler/performance_test.cpp)
target_link_libraries(performance_test PRIVATE 
    ZLCoro::zlcoro
    GTest::gtest
    GTest::gtest_main
    pthread
)
add_test(NAME PerformanceTest COMMAND performance_test)

# io_uring 测试（可选，需要 liburing）
find_library(LIBURING_LIBRARY uring)
if(LIBURING_LIBRARY)
    message(STATUS "Found liburing: ${LIBURING_LIBRARY}")
    
    add_executable(io_uring_test io/io_uring_test.cpp)
    target_link_libraries(io_uring_test PRIVATE 
        ZLCoro::zlcoro
        GTest::gtest
        GTest::gtest_main
        pthread
        ${LIBURING_LIBRARY}
    )
    add_test(NAME IoUringTest COMMAND io_uring_test)
else()
    message(STATUS "liburing not found, io_uring tests disabled")
    message(STATUS "  To enable: sudo apt install liburing-dev")
endif()

# Runtime 测试（Phase 7: 统一运行时）
if(LIBURING_LIBRARY)
    add_executable(runtime_test runtime/runtime_test.cpp)
    target_link_libraries(runtime_test PRIVATE 
        ZLCoro::zlcoro
        GTest::gtest
        GTest::gtest_main
        pthread
        ${LIBURING_LIBRARY}
    )
    add_test(NAME RuntimeTest COMMAND runtime_test)
else()
    # 无 io_uring 版本
    add_executable(runtime_test runtime/runtime_test.cpp)
    target_link_libraries(runtime_test PRIVATE 
        ZLCoro::zlcoro
        GTest::gtest
        GTest::gtest_main
        pthread
    )
    add_test(NAME RuntimeTest COMMAND runtime_test)
endif()
