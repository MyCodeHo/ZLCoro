#include "zlcoro/io.hpp"
#include "zlcoro/scheduler/async.hpp"
#include <iostream>
#include <thread>

using namespace zlcoro;

// =============================================================================
// 示例 1: 异步文件读写
// =============================================================================

Task<void> example_file_io() {
    std::cout << "\n=== 示例 1: 异步文件读写 ===\n";
    
    std::string filename = "/tmp/zlcoro_example.txt";
    std::string content = "Hello from ZLCoro async file I/O!\n";
    
    // 写入文件
    std::cout << "写入文件: " << filename << "\n";
    co_await write_file(filename, content);
    
    // 读取文件
    std::cout << "读取文件: " << filename << "\n";
    std::string read_content = co_await read_file(filename);
    
    std::cout << "文件内容: " << read_content;
    
    // 追加内容
    std::cout << "追加内容到文件\n";
    co_await append_file(filename, "Appended line\n");
    
    // 再次读取
    read_content = co_await read_file(filename);
    std::cout << "更新后的内容:\n" << read_content;
}

// =============================================================================
// 示例 2: 多个文件并发操作
// =============================================================================

Task<void> write_multiple_files() {
    std::vector<std::future<void>> futures;
    
    for (int i = 0; i < 5; ++i) {
        auto task = [i]() -> Task<void> {
            std::string filename = "/tmp/zlcoro_file_" + std::to_string(i) + ".txt";
            std::string content = "File " + std::to_string(i) + " content\n";
            
            co_await write_file(filename, content);
            
            std::cout << "写入完成: " << filename << "\n";
        };
        
        futures.push_back(async_run(task()));
    }
    
    // 等待所有完成
    for (auto& future : futures) {
        future.get();
    }
    
    co_return;
}

Task<void> example_concurrent_files() {
    std::cout << "\n=== 示例 2: 并发文件操作 ===\n";
    co_await write_multiple_files();
}

// =============================================================================
// 示例 3: 大文件操作
// =============================================================================

Task<void> example_large_file() {
    std::cout << "\n=== 示例 3: 大文件操作 ===\n";
    
    std::string filename = "/tmp/zlcoro_large.txt";
    
    // 创建 10MB 的数据
    std::cout << "创建 10MB 数据...\n";
    std::string large_data(10 * 1024 * 1024, 'X');
    
    std::cout << "写入大文件...\n";
    co_await write_file(filename, large_data);
    
    std::cout << "读取大文件...\n";
    std::string read_data = co_await read_file(filename);
    
    std::cout << "验证数据...\n";
    if (read_data == large_data) {
        std::cout << "✓ 大文件读写成功！\n";
    } else {
        std::cout << "✗ 数据不匹配\n";
    }
}

// =============================================================================
// 示例 4: 文件复制
// =============================================================================

Task<void> copy_file(const std::string& src, const std::string& dst) {
    // 同步复制文件（在线程池中执行）
    AsyncFile src_file(src, AsyncFile::ReadOnly);
    AsyncFile dst_file(dst, AsyncFile::WriteOnly | AsyncFile::Create | AsyncFile::Truncate, 0644);
    
    // 分块复制
    const size_t chunk_size = 8192;
    while (true) {
        std::string chunk = src_file.read(chunk_size);
        if (chunk.empty()) {
            break;
        }
        
        dst_file.write(chunk);
    }
    
    dst_file.sync();
    co_return;
}

Task<void> example_file_copy() {
    std::cout << "\n=== 示例 4: 文件复制 ===\n";
    
    std::string src = "/tmp/zlcoro_source.txt";
    std::string dst = "/tmp/zlcoro_destination.txt";
    
    // 创建源文件
    std::string content = "This is the source file content.\n";
    content += "It has multiple lines.\n";
    content += "We will copy it to another file.\n";
    
    co_await write_file(src, content);
    std::cout << "创建源文件: " << src << "\n";
    
    // 复制
    std::cout << "复制到: " << dst << "\n";
    co_await copy_file(src, dst);
    
    // 验证
    std::string dst_content = co_await read_file(dst);
    if (dst_content == content) {
        std::cout << "✓ 文件复制成功！\n";
    } else {
        std::cout << "✗ 复制失败\n";
    }
}

// =============================================================================
// 示例 5: Echo 服务器（需要事件循环，此处展示结构）
// =============================================================================

Task<void> handle_client(AsyncSocket client) {
    std::cout << "新连接建立\n";
    
    try {
        while (true) {
            std::string data = co_await client.read();
            
            if (data.empty()) {
                std::cout << "客户端断开连接\n";
                break;
            }
            
            std::cout << "收到: " << data;
            
            // Echo 回去
            co_await client.write(data);
        }
    } catch (const std::exception& e) {
        std::cout << "处理客户端时出错: " << e.what() << "\n";
    }
}

Task<void> echo_server(int port) {
    AsyncSocket server;
    server.create();
    server.set_reuse_addr(true);
    server.bind("0.0.0.0", port);
    server.listen();
    
    std::cout << "Echo 服务器监听在端口 " << port << "\n";
    std::cout << "注意: 需要运行事件循环才能工作\n";
    
    // 注意：这需要事件循环支持
    // while (true) {
    //     AsyncSocket client = co_await server.accept();
    //     // 启动新协程处理客户端
    //     fire_and_forget(handle_client(std::move(client)));
    // }
    
    co_return;
}

Task<void> example_echo_server() {
    std::cout << "\n=== 示例 5: Echo 服务器（结构展示）===\n";
    std::cout << "完整的 Echo 服务器需要事件循环支持\n";
    std::cout << "当前使用线程池模式，适合文件 I/O\n";
    co_return;
}

// =============================================================================
// 主函数
// =============================================================================

int main() {
    std::cout << "ZLCoro 异步 I/O 示例\n";
    std::cout << "==================\n";
    
    try {
        // 示例 1: 基础文件读写
        {
            auto future = async_run(example_file_io());
            future.get();
        }
        
        // 示例 2: 并发文件操作
        {
            auto future = async_run(example_concurrent_files());
            future.get();
        }
        
        // 示例 3: 大文件操作
        {
            auto future = async_run(example_large_file());
            future.get();
        }
        
        // 示例 4: 文件复制
        {
            auto future = async_run(example_file_copy());
            future.get();
        }
        
        // 示例 5: Echo 服务器结构
        {
            auto future = async_run(example_echo_server());
            future.get();
        }
        
        std::cout << "\n所有示例完成！\n";
        
    } catch (const std::exception& e) {
        std::cerr << "错误: " << e.what() << "\n";
        return 1;
    }
    
    return 0;
}
