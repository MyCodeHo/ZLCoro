#pragma once

#include "zlcoro/core/task.hpp"
#include "zlcoro/sync/cancellation.hpp"

#ifdef ZLCORO_HAS_IO_URING
#include "zlcoro/io/io_uring_socket.hpp"
#include "zlcoro/io/io_uring_poller.hpp"
#else
#include "zlcoro/io/async_socket.hpp"
#endif

#include <string>
#include <vector>
#include <deque>
#include <memory>
#include <functional>
#include <optional>
#include <cstring>

namespace zlcoro {

// =============================================================================
// TcpConnection - TCP 连接抽象
// =============================================================================
// 
// 封装 TCP 连接的读写操作，提供：
// - 缓冲读取（按行、按字节数）
// - 缓冲写入
// - 连接状态管理
// 
// 使用示例:
//   Task<void> handle_client(TcpConnection conn) {
//       auto line = co_await conn.read_line();
//       co_await conn.write("Echo: " + line);
//   }
// =============================================================================

class TcpConnection {
public:
    static constexpr size_t DEFAULT_BUFFER_SIZE = 8192;
    static constexpr size_t MAX_BUFFER_SIZE = 1024 * 1024;  // 1MB 最大缓冲区
    static constexpr size_t SHRINK_THRESHOLD = 65536;       // 64KB 收缩阈值

#ifdef ZLCORO_HAS_IO_URING
    using SocketType = IoUringSocket;
#else
    using SocketType = AsyncSocket;
#endif

    // 从已有 socket 构造
    explicit TcpConnection(SocketType socket)
        : socket_(std::move(socket))
        , read_buffer_(DEFAULT_BUFFER_SIZE)
        , read_pos_(0)
        , read_end_(0)
        , closed_(false) {}

    // 禁止拷贝
    TcpConnection(const TcpConnection&) = delete;
    TcpConnection& operator=(const TcpConnection&) = delete;

    // 允许移动
    TcpConnection(TcpConnection&& other) noexcept
        : socket_(std::move(other.socket_))
        , read_buffer_(std::move(other.read_buffer_))
        , read_pos_(other.read_pos_)
        , read_end_(other.read_end_)
        , write_buffer_(std::move(other.write_buffer_))
        , closed_(other.closed_) {
        // 重置源对象状态，防止悬挂引用
        other.read_pos_ = 0;
        other.read_end_ = 0;
        other.closed_ = true;
    }

    TcpConnection& operator=(TcpConnection&& other) noexcept {
        if (this != &other) {
            // 先关闭当前连接
            close_sync();
            
            socket_ = std::move(other.socket_);
            read_buffer_ = std::move(other.read_buffer_);
            read_pos_ = other.read_pos_;
            read_end_ = other.read_end_;
            write_buffer_ = std::move(other.write_buffer_);
            closed_ = other.closed_;
            
            // 重置源对象状态
            other.read_pos_ = 0;
            other.read_end_ = 0;
            other.closed_ = true;
        }
        return *this;
    }

    ~TcpConnection() {
        close_sync();
    }

    // =========================================================================
    // 读取操作
    // =========================================================================

    // 读取指定字节数
    Task<std::vector<char>> read_bytes(size_t count) {
        std::vector<char> result;
        result.reserve(count);

        while (result.size() < count) {
            // 先从缓冲区读
            size_t buffered = read_end_ - read_pos_;
            if (buffered > 0) {
                size_t to_copy = std::min(buffered, count - result.size());
                result.insert(result.end(), 
                             read_buffer_.begin() + read_pos_,
                             read_buffer_.begin() + read_pos_ + to_copy);
                read_pos_ += to_copy;
                continue;
            }

            // 缓冲区空，从 socket 读取
            if (co_await fill_buffer() == 0) {
                break;  // EOF
            }
        }

        co_return result;
    }

    // 读取一行（以 \n 或 \r\n 结尾）
    Task<std::string> read_line() {
        std::string line;

        while (true) {
            // 在缓冲区中查找换行符
            for (size_t i = read_pos_; i < read_end_; ++i) {
                if (read_buffer_[i] == '\n') {
                    // 找到换行符
                    size_t line_end = i;
                    
                    // 处理 \r\n
                    if (line_end > read_pos_ && read_buffer_[line_end - 1] == '\r') {
                        line.append(read_buffer_.begin() + read_pos_,
                                   read_buffer_.begin() + line_end - 1);
                    } else {
                        line.append(read_buffer_.begin() + read_pos_,
                                   read_buffer_.begin() + line_end);
                    }
                    
                    read_pos_ = i + 1;
                    co_return line;
                }
            }

            // 没找到换行符，把当前缓冲区内容加入 line
            line.append(read_buffer_.begin() + read_pos_,
                       read_buffer_.begin() + read_end_);
            read_pos_ = read_end_;

            // 读取更多数据
            if (co_await fill_buffer() == 0) {
                // EOF，返回当前积累的内容
                co_return line;
            }
        }
    }

    // 读取所有可用数据（非阻塞）
    Task<std::string> read_available() {
        std::string result;

        // 先返回缓冲区中的数据
        if (read_pos_ < read_end_) {
            result.append(read_buffer_.begin() + read_pos_,
                         read_buffer_.begin() + read_end_);
            read_pos_ = read_end_;
        }

        // 尝试读取更多（如果 socket 有数据）
#ifdef ZLCORO_HAS_IO_URING
        ssize_t n = co_await socket_.recv(read_buffer_.data(), read_buffer_.size(), 0);
#else
        ssize_t n = co_await socket_.recv(read_buffer_.data(), read_buffer_.size());
#endif
        if (n > 0) {
            result.append(read_buffer_.data(), n);
        }

        co_return result;
    }

    // 精确读取 n 字节（不足则抛异常）
    Task<std::vector<char>> read_exact(size_t count) {
        auto data = co_await read_bytes(count);
        if (data.size() != count) {
            throw std::runtime_error("Connection closed before reading enough data");
        }
        co_return data;
    }

    // =========================================================================
    // 写入操作
    // =========================================================================

    // 写入数据
    Task<void> write(const void* data, size_t len) {
#ifdef ZLCORO_HAS_IO_URING
        const char* ptr = static_cast<const char*>(data);
        size_t written = 0;
        
        while (written < len) {
            ssize_t n = co_await socket_.send(ptr + written, len - written, 0);
            if (n < 0) {
                throw std::runtime_error(
                    std::string("write failed: ") + strerror(-n));
            }
            if (n == 0) {
                throw std::runtime_error("Connection closed during write");
            }
            written += n;
        }
#else
        co_await socket_.send(data, len);
#endif
    }

    // 写入字符串
    Task<void> write(const std::string& data) {
        co_await write(data.data(), data.size());
    }

    // 写入字符串视图
    Task<void> write(std::string_view data) {
        co_await write(data.data(), data.size());
    }

    // 写入并添加换行符
    Task<void> write_line(const std::string& line) {
        co_await write(line + "\r\n");
    }

    // =========================================================================
    // 连接管理
    // =========================================================================

    // 检查连接是否打开
    bool is_open() const noexcept {
        return !closed_ && socket_.is_valid();
    }

    // 同步关闭
    void close_sync() {
        if (!closed_) {
            closed_ = true;
            socket_.close_sync();
        }
    }

    // 异步关闭
    Task<void> close() {
#ifdef ZLCORO_HAS_IO_URING
        if (!closed_) {
            closed_ = true;
            co_await socket_.close_async();
        }
#else
        close_sync();
#endif
        co_return;
    }

    // 获取底层 socket（高级用法）
    SocketType& socket() noexcept {
        return socket_;
    }

    const SocketType& socket() const noexcept {
        return socket_;
    }

    // 获取对端地址
    std::pair<std::string, uint16_t> peer_address() const {
        return socket_.get_peer_address();
    }

private:
    // 填充读缓冲区
    Task<size_t> fill_buffer() {
        // 如果缓冲区已空，重置位置并考虑收缩
        if (read_pos_ == read_end_) {
            read_pos_ = 0;
            read_end_ = 0;
            
            // 如果缓冲区太大，收缩到默认大小
            if (read_buffer_.size() > SHRINK_THRESHOLD) {
                read_buffer_.resize(DEFAULT_BUFFER_SIZE);
                read_buffer_.shrink_to_fit();
            }
        }

        // 如果缓冲区已满，需要移动数据或扩展
        if (read_end_ == read_buffer_.size()) {
            if (read_pos_ > 0) {
                // 移动未读数据到缓冲区开头
                std::memmove(read_buffer_.data(),
                            read_buffer_.data() + read_pos_,
                            read_end_ - read_pos_);
                read_end_ -= read_pos_;
                read_pos_ = 0;
            } else {
                // 缓冲区真的满了，需要扩展（但有上限）
                size_t new_size = read_buffer_.size() * 2;
                if (new_size > MAX_BUFFER_SIZE) {
                    throw std::runtime_error("Read buffer overflow: line too long");
                }
                read_buffer_.resize(new_size);
            }
        }

#ifdef ZLCORO_HAS_IO_URING
        ssize_t n = co_await socket_.recv(
            read_buffer_.data() + read_end_,
            read_buffer_.size() - read_end_,
            0);
#else
        ssize_t n = co_await socket_.recv(
            read_buffer_.data() + read_end_,
            read_buffer_.size() - read_end_);
#endif

        if (n > 0) {
            read_end_ += n;
        } else if (n < 0) {
            throw std::runtime_error(
                std::string("recv failed: ") + strerror(-n));
        }

        co_return (n > 0) ? static_cast<size_t>(n) : 0;
    }
    
    // 重置缓冲区到默认大小
    void reset_buffer() {
        read_buffer_.clear();
        read_buffer_.resize(DEFAULT_BUFFER_SIZE);
        read_buffer_.shrink_to_fit();
        read_pos_ = 0;
        read_end_ = 0;
    }

private:
    // =========================================================================
    // 数据成员
    // =========================================================================
    
    /// @brief 底层 Socket
    /// @details 根据编译选项，可能是 IoUringSocket 或 AsyncSocket。
    ///          负责实际的网络 I/O 操作。
    /// @ownership TcpConnection 独占所有权
    SocketType socket_;
    
    /// @brief 读缓冲区
    /// @details 从 socket 读取的数据先存入此缓冲区，
    ///          然后由 read_bytes/read_line 等方法消费。
    ///          大小动态调整，有上限（MAX_BUFFER_SIZE）防止内存溢出。
    std::vector<char> read_buffer_;
    
    /// @brief 缓冲区读取位置
    /// @details 指向 read_buffer_ 中下一个待读取字节的位置。
    ///          范围：[0, read_end_]
    size_t read_pos_;
    
    /// @brief 缓冲区数据结束位置
    /// @details 指向 read_buffer_ 中有效数据的末尾（不含）。
    ///          [read_pos_, read_end_) 是未读数据区间。
    size_t read_end_;
    
    /// @brief 写缓冲区（预留，当前未使用）
    /// @details 可用于实现批量写入或 writev 优化。
    std::vector<char> write_buffer_;
    
    /// @brief 连接关闭标志
    /// @details true 表示连接已关闭，后续操作会失败或被忽略。
    bool closed_;
};

// =============================================================================
// TcpListener - TCP 监听器
// =============================================================================
// 
// 封装 TCP 监听和连接接受逻辑：
// - 配置监听参数
// - 异步接受连接
// - 支持取消
// =============================================================================

class TcpListener {
public:
#ifdef ZLCORO_HAS_IO_URING
    using SocketType = IoUringSocket;
#else
    using SocketType = AsyncSocket;
#endif

    TcpListener() = default;

#ifdef ZLCORO_HAS_IO_URING
    explicit TcpListener(IoUringPoller* poller) : poller_(poller) {}
#endif

    // 禁止拷贝
    TcpListener(const TcpListener&) = delete;
    TcpListener& operator=(const TcpListener&) = delete;

    // 允许移动
    TcpListener(TcpListener&&) = default;
    TcpListener& operator=(TcpListener&&) = default;

    // 绑定并监听
    void listen(const std::string& host, uint16_t port, int backlog = 128) {
        socket_.create();
        socket_.set_reuse_addr(true);
        socket_.set_reuse_port(true);
        socket_.bind(host, port);
        socket_.listen(backlog);
        
#ifdef ZLCORO_HAS_IO_URING
        if (poller_) {
            socket_.set_poller(poller_);
        }
#endif

        host_ = host;
        port_ = port;
    }

    // 异步接受连接
    Task<TcpConnection> accept() {
#ifdef ZLCORO_HAS_IO_URING
        auto client_socket = co_await socket_.accept();
        co_return TcpConnection(std::move(client_socket));
#else
        auto client_socket = co_await socket_.accept();
        co_return TcpConnection(std::move(client_socket));
#endif
    }

    // 获取监听地址
    std::string host() const { return host_; }
    uint16_t port() const { return port_; }

    // 获取底层 socket
    SocketType& socket() noexcept { return socket_; }
    const SocketType& socket() const noexcept { return socket_; }

    // 关闭监听器
    void close() {
        socket_.close_sync();
    }

#ifdef ZLCORO_HAS_IO_URING
    void set_poller(IoUringPoller* poller) {
        poller_ = poller;
        socket_.set_poller(poller);
    }
#endif

private:
    // =========================================================================
    // 数据成员
    // =========================================================================
    
    /// @brief 监听 Socket
    /// @details 绑定到指定地址端口，等待客户端连接。
    SocketType socket_;
    
    /// @brief 监听主机地址
    /// @details 在 listen() 中设置，用于日志和状态查询。
    std::string host_;
    
    /// @brief 监听端口号
    /// @details 在 listen() 中设置。
    uint16_t port_ = 0;
    
#ifdef ZLCORO_HAS_IO_URING
    /// @brief io_uring 轮询器指针（可选）
    /// @details 如果使用 io_uring 后端，需要设置此指针。
    IoUringPoller* poller_ = nullptr;
#endif
};

// =============================================================================
// TcpServer - TCP 服务器框架
// =============================================================================
// 
// 高层抽象的 TCP 服务器，支持：
// - 连接处理回调
// - 优雅关闭
// - 并发连接管理
// 
// 使用示例:
//   TcpServer server;
//   server.on_connection([](TcpConnection conn) -> Task<void> {
//       auto line = co_await conn.read_line();
//       co_await conn.write("Echo: " + line);
//   });
//   co_await server.serve("0.0.0.0", 8080);
// =============================================================================

class TcpServer {
public:
    using ConnectionHandler = std::function<Task<void>(TcpConnection)>;
    // 用于并发执行的 spawn 函数类型
    using SpawnFunction = std::function<void(Task<void>)>;

    TcpServer() = default;

#ifdef ZLCORO_HAS_IO_URING
    explicit TcpServer(IoUringPoller* poller) : listener_(poller) {}
#endif

    // 设置连接处理器
    void on_connection(ConnectionHandler handler) {
        handler_ = std::move(handler);
    }
    
    // 设置 spawn 函数，用于并发执行连接处理
    // 如果设置了 spawn 函数，每个连接将在独立的协程中处理
    void set_spawn(SpawnFunction spawn_fn) {
        spawn_fn_ = std::move(spawn_fn);
    }

    // 启动服务器
    Task<void> serve(const std::string& host, uint16_t port,
                     CancellationToken token = CancellationToken::none()) {
        listener_.listen(host, port);

        // 注册取消回调：关闭监听 socket 以中断 accept()
        token.on_cancel([this]() {
            listener_.close();
        });
        
        // 存储活跃任务（确保协程生命周期）
        std::vector<std::unique_ptr<Task<void>>> active_tasks;

        while (!token.is_cancelled()) {
            try {
                auto conn = co_await listener_.accept();
                
                // 再次检查取消状态（可能在 accept 期间被取消）
                if (token.is_cancelled()) {
                    break;
                }
                
                if (handler_) {
                    if (spawn_fn_) {
                        // 使用 spawn 函数并发处理连接
                        // 将连接移动到 shared_ptr 以延长生命周期
                        auto conn_ptr = std::make_shared<TcpConnection>(std::move(conn));
                        auto handler = handler_;  // 复制 handler
                        
                        // 创建任务并存储以保持生命周期
                        auto task = std::make_unique<Task<void>>(
                            [conn_ptr, handler]() -> Task<void> {
                                try {
                                    co_await handler(std::move(*conn_ptr));
                                } catch (const std::exception& e) {
                                    // 记录错误但不影响其他连接
                                }
                            }());
                        
                        auto handle = task->handle();
                        if (handle && !handle.done()) {
                            handle.resume();
                        }
                        active_tasks.push_back(std::move(task));
                        
                        // 清理已完成的任务
                        active_tasks.erase(
                            std::remove_if(active_tasks.begin(), active_tasks.end(),
                                [](const std::unique_ptr<Task<void>>& t) {
                                    return t->handle().done();
                                }),
                            active_tasks.end());
                    } else {
                        // 串行处理（原来的行为）
                        try {
                            co_await handler_(std::move(conn));
                        } catch (const std::exception& e) {
                            // 记录错误但继续接受新连接
                        }
                    }
                }
            } catch (const std::exception& e) {
                if (token.is_cancelled()) {
                    break;
                }
                // 其他错误，继续尝试
            }
        }
    }

    // 关闭服务器
    void close() {
        listener_.close();
    }

    // 获取监听器
    TcpListener& listener() noexcept { return listener_; }
    const TcpListener& listener() const noexcept { return listener_; }

#ifdef ZLCORO_HAS_IO_URING
    void set_poller(IoUringPoller* poller) {
        listener_.set_poller(poller);
    }
#endif

private:
    // =========================================================================
    // 数据成员
    // =========================================================================
    
    /// @brief TCP 监听器
    /// @details 封装底层监听 socket，负责接受新连接。
    TcpListener listener_;
    
    /// @brief 连接处理回调
    /// @details 每个新连接到来时调用，返回处理该连接的协程。
    ///          通过 on_connection() 设置。
    ConnectionHandler handler_;
    
    /// @brief 协程 spawn 函数（可选）
    /// @details 如果设置了此函数，每个连接会在独立协程中并发处理。
    ///          如果未设置，连接会串行处理（一个接一个）。
    ///          通过 set_spawn() 设置。
    SpawnFunction spawn_fn_;
};

// =============================================================================
// TcpClient - TCP 客户端
// =============================================================================

class TcpClient {
public:
#ifdef ZLCORO_HAS_IO_URING
    using SocketType = IoUringSocket;
    
    explicit TcpClient(IoUringPoller* poller) : poller_(poller) {}
#else
    using SocketType = AsyncSocket;
    
    TcpClient() = default;
#endif

    // 连接到服务器
    Task<TcpConnection> connect(const std::string& host, uint16_t port) {
        SocketType socket;
        socket.create();
        
#ifdef ZLCORO_HAS_IO_URING
        socket.set_poller(poller_);
        int result = co_await socket.connect(host, port);
        if (result < 0) {
            throw std::runtime_error(
                std::string("connect failed: ") + strerror(-result));
        }
#else
        co_await socket.connect(host, port);
#endif

        co_return TcpConnection(std::move(socket));
    }

private:
    // =========================================================================
    // 数据成员
    // =========================================================================
    
#ifdef ZLCORO_HAS_IO_URING
    /// @brief io_uring 轮询器指针
    /// @details 用于异步连接操作。必须在构造时提供（io_uring 后端）。
    IoUringPoller* poller_;
#endif
};

} // namespace zlcoro
