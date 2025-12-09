#include "zlcoro/scheduler/async.hpp"
#include "zlcoro/io/async_file.hpp"
#include <iostream>

using namespace zlcoro;

Task<void> simple_test() {
    std::cout << "Starting simple test...\n";
    
    std::string filename = "/tmp/test.txt";
    std::string content = "Hello, World!\n";
    
    std::cout << "Writing file...\n";
    co_await write_file(filename, content);
    
    std::cout << "Reading file...\n";
    std::string read_content = co_await read_file(filename);
    
    std::cout << "Content: " << read_content;
    std::cout << "Test complete!\n";
}

int main() {
    std::cout << "Simple File I/O Test\n";
    
    try {
        auto future = async_run(simple_test());
        future.get();
        
        std::cout << "Success!\n";
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }
    
    return 0;
}
