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
