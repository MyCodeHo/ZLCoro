/**
 * @file http.hpp
 * @brief 简单的 HTTP/1.1 服务器实现
 * 
 * 本文件提供了一个轻量级的 HTTP 服务器框架：
 * - HttpRequest: HTTP 请求解析
 * - HttpResponse: HTTP 响应构建
 * - HttpServer: HTTP 服务器
 * 
 * 特性：
 * - 支持 GET/POST/PUT/DELETE 等常用方法
 * - 支持请求头和请求体解析
 * - 支持路由注册
 * - 基于 TcpServer 构建
 * 
 * 使用示例：
 * @code
 * HttpServer server;
 * 
 * server.get("/", [](const HttpRequest& req) -> Task<HttpResponse> {
 *     co_return HttpResponse::ok("Hello, World!");
 * });
 * 
 * server.get("/json", [](const HttpRequest& req) -> Task<HttpResponse> {
 *     co_return HttpResponse::json(R"({"message": "Hello"})");
 * });
 * 
 * co_await server.serve("0.0.0.0", 8080);
 * @endcode
 */

#pragma once

#include "zlcoro/core/task.hpp"
#include "zlcoro/net/tcp.hpp"
#include "zlcoro/sync/cancellation.hpp"

#include <string>
#include <string_view>
#include <map>
#include <vector>
#include <functional>
#include <sstream>
#include <algorithm>
#include <cctype>

namespace zlcoro {

// =============================================================================
// HTTP 常量定义
// =============================================================================

namespace http {

/// @brief HTTP 方法枚举
enum class Method {
    GET,
    POST,
    PUT,
    DELETE_,  // DELETE 是保留字
    HEAD,
    OPTIONS,
    PATCH,
    UNKNOWN
};

/// @brief HTTP 状态码
enum class StatusCode {
    OK = 200,
    Created = 201,
    NoContent = 204,
    MovedPermanently = 301,
    Found = 302,
    NotModified = 304,
    BadRequest = 400,
    Unauthorized = 401,
    Forbidden = 403,
    NotFound = 404,
    MethodNotAllowed = 405,
    InternalServerError = 500,
    NotImplemented = 501,
    BadGateway = 502,
    ServiceUnavailable = 503
};

/// @brief 获取状态码对应的描述文本
inline std::string_view status_text(StatusCode code) {
    switch (code) {
        case StatusCode::OK: return "OK";
        case StatusCode::Created: return "Created";
        case StatusCode::NoContent: return "No Content";
        case StatusCode::MovedPermanently: return "Moved Permanently";
        case StatusCode::Found: return "Found";
        case StatusCode::NotModified: return "Not Modified";
        case StatusCode::BadRequest: return "Bad Request";
        case StatusCode::Unauthorized: return "Unauthorized";
        case StatusCode::Forbidden: return "Forbidden";
        case StatusCode::NotFound: return "Not Found";
        case StatusCode::MethodNotAllowed: return "Method Not Allowed";
        case StatusCode::InternalServerError: return "Internal Server Error";
        case StatusCode::NotImplemented: return "Not Implemented";
        case StatusCode::BadGateway: return "Bad Gateway";
        case StatusCode::ServiceUnavailable: return "Service Unavailable";
        default: return "Unknown";
    }
}

/// @brief 将字符串转换为 HTTP 方法
inline Method parse_method(std::string_view method) {
    if (method == "GET") return Method::GET;
    if (method == "POST") return Method::POST;
    if (method == "PUT") return Method::PUT;
    if (method == "DELETE") return Method::DELETE_;
    if (method == "HEAD") return Method::HEAD;
    if (method == "OPTIONS") return Method::OPTIONS;
    if (method == "PATCH") return Method::PATCH;
    return Method::UNKNOWN;
}

/// @brief 将 HTTP 方法转换为字符串
inline std::string_view method_string(Method method) {
    switch (method) {
        case Method::GET: return "GET";
        case Method::POST: return "POST";
        case Method::PUT: return "PUT";
        case Method::DELETE_: return "DELETE";
        case Method::HEAD: return "HEAD";
        case Method::OPTIONS: return "OPTIONS";
        case Method::PATCH: return "PATCH";
        default: return "UNKNOWN";
    }
}

/// @brief 大小写不敏感的字符串比较器
struct CaseInsensitiveCompare {
    bool operator()(const std::string& a, const std::string& b) const {
        return std::lexicographical_compare(
            a.begin(), a.end(),
            b.begin(), b.end(),
            [](char c1, char c2) {
                return std::tolower(static_cast<unsigned char>(c1)) < 
                       std::tolower(static_cast<unsigned char>(c2));
            });
    }
};

} // namespace http

// =============================================================================
// HttpRequest - HTTP 请求
// =============================================================================

/**
 * @brief HTTP 请求类
 * @details 封装 HTTP 请求的所有信息：方法、路径、头部、请求体
 */
class HttpRequest {
public:
    /// @brief 最大请求体大小（默认 10MB）
    static constexpr size_t MAX_BODY_SIZE = 10 * 1024 * 1024;
    
    /// @brief 请求头使用大小写不敏感比较
    using Headers = std::map<std::string, std::string, http::CaseInsensitiveCompare>;
    
    HttpRequest() = default;
    
    // =========================================================================
    // 请求属性访问
    // =========================================================================
    
    /// @brief 获取 HTTP 方法
    http::Method method() const noexcept { return method_; }
    
    /// @brief 获取请求路径
    const std::string& path() const noexcept { return path_; }
    
    /// @brief 获取查询字符串（不含 ?）
    const std::string& query_string() const noexcept { return query_string_; }
    
    /// @brief 获取 HTTP 版本（如 "HTTP/1.1"）
    const std::string& version() const noexcept { return version_; }
    
    /// @brief 获取所有请求头
    const Headers& headers() const noexcept { return headers_; }
    
    /// @brief 获取请求体
    const std::string& body() const noexcept { return body_; }
    
    /// @brief 获取指定请求头（大小写不敏感）
    std::string header(const std::string& name) const {
        auto it = headers_.find(name);
        return it != headers_.end() ? it->second : "";
    }
    
    /// @brief 获取 Content-Length（带错误处理）
    size_t content_length() const {
        auto cl = header("Content-Length");
        if (cl.empty()) return 0;
        try {
            return std::stoull(cl);
        } catch (const std::exception&) {
            return 0;  // 无效的 Content-Length 值，返回 0
        }
    }
    
    /// @brief 获取 Content-Type
    std::string content_type() const {
        return header("Content-Type");
    }
    
    // =========================================================================
    // 请求解析
    // =========================================================================
    
    /**
     * @brief 从 TcpConnection 解析 HTTP 请求
     * @param conn TCP 连接
     * @return 解析后的 HttpRequest
     */
    static Task<HttpRequest> parse(TcpConnection& conn) {
        HttpRequest req;
        
        // 读取请求行
        std::string request_line = co_await conn.read_line();
        if (request_line.empty()) {
            throw std::runtime_error("Empty request line");
        }
        
        // 解析请求行: METHOD PATH VERSION
        std::istringstream line_stream(request_line);
        std::string method_str, full_path;
        
        line_stream >> method_str >> full_path >> req.version_;
        
        // 验证 HTTP 版本
        if (req.version_.find("HTTP/") != 0) {
            throw std::runtime_error("Invalid HTTP version: " + req.version_);
        }
        
        req.method_ = http::parse_method(method_str);
        
        // 验证请求方法
        if (req.method_ == http::Method::UNKNOWN) {
            throw std::runtime_error("Unknown HTTP method: " + method_str);
        }
        
        // 分离路径和查询字符串
        auto query_pos = full_path.find('?');
        if (query_pos != std::string::npos) {
            req.path_ = full_path.substr(0, query_pos);
            req.query_string_ = full_path.substr(query_pos + 1);
        } else {
            req.path_ = full_path;
        }
        
        // 读取请求头
        while (true) {
            std::string header_line = co_await conn.read_line();
            
            // 空行表示头部结束
            if (header_line.empty()) {
                break;
            }
            
            // 解析头部: Name: Value
            auto colon_pos = header_line.find(':');
            if (colon_pos != std::string::npos) {
                std::string name = header_line.substr(0, colon_pos);
                std::string value = header_line.substr(colon_pos + 1);
                
                // 去除前导空格
                while (!value.empty() && value[0] == ' ') {
                    value.erase(0, 1);
                }
                
                req.headers_[std::move(name)] = std::move(value);
            }
        }
        
        // 读取请求体（如果有）
        size_t content_length = req.content_length();
        if (content_length > 0) {
            // 验证请求体大小不超过限制
            if (content_length > MAX_BODY_SIZE) {
                throw std::runtime_error(
                    "Request body too large: " + std::to_string(content_length) + 
                    " bytes (max: " + std::to_string(MAX_BODY_SIZE) + ")");
            }
            
            auto body_data = co_await conn.read_exact(content_length);
            req.body_.assign(body_data.begin(), body_data.end());
        }
        
        co_return req;
    }

private:
    // =========================================================================
    // 数据成员
    // =========================================================================
    
    /// @brief HTTP 方法
    http::Method method_ = http::Method::GET;
    
    /// @brief 请求路径（不含查询字符串）
    std::string path_;
    
    /// @brief 查询字符串（不含 ?）
    std::string query_string_;
    
    /// @brief HTTP 版本
    std::string version_ = "HTTP/1.1";
    
    /// @brief 请求头（名称 -> 值）
    Headers headers_;
    
    /// @brief 请求体
    std::string body_;
};

// =============================================================================
// HttpResponse - HTTP 响应
// =============================================================================

/**
 * @brief HTTP 响应类
 * @details 封装 HTTP 响应的所有信息：状态码、头部、响应体
 */
class HttpResponse {
public:
    using Headers = std::map<std::string, std::string>;
    
    HttpResponse() = default;
    
    explicit HttpResponse(http::StatusCode status)
        : status_(status) {}
    
    HttpResponse(http::StatusCode status, std::string body)
        : status_(status), body_(std::move(body)) {}
    
    // =========================================================================
    // 工厂方法
    // =========================================================================
    
    /// @brief 创建 200 OK 响应
    static HttpResponse ok(std::string body = "") {
        HttpResponse resp(http::StatusCode::OK, std::move(body));
        resp.set_header("Content-Type", "text/plain; charset=utf-8");
        return resp;
    }
    
    /// @brief 创建 HTML 响应
    static HttpResponse html(std::string body) {
        HttpResponse resp(http::StatusCode::OK, std::move(body));
        resp.set_header("Content-Type", "text/html; charset=utf-8");
        return resp;
    }
    
    /// @brief 创建 JSON 响应
    static HttpResponse json(std::string body) {
        HttpResponse resp(http::StatusCode::OK, std::move(body));
        resp.set_header("Content-Type", "application/json; charset=utf-8");
        return resp;
    }
    
    /// @brief 创建 404 Not Found 响应
    static HttpResponse not_found(std::string body = "Not Found") {
        HttpResponse resp(http::StatusCode::NotFound, std::move(body));
        resp.set_header("Content-Type", "text/plain; charset=utf-8");
        return resp;
    }
    
    /// @brief 创建 400 Bad Request 响应
    static HttpResponse bad_request(std::string body = "Bad Request") {
        HttpResponse resp(http::StatusCode::BadRequest, std::move(body));
        resp.set_header("Content-Type", "text/plain; charset=utf-8");
        return resp;
    }
    
    /// @brief 创建 500 Internal Server Error 响应
    static HttpResponse internal_error(std::string body = "Internal Server Error") {
        HttpResponse resp(http::StatusCode::InternalServerError, std::move(body));
        resp.set_header("Content-Type", "text/plain; charset=utf-8");
        return resp;
    }
    
    /// @brief 创建重定向响应
    static HttpResponse redirect(const std::string& location, 
                                 http::StatusCode code = http::StatusCode::Found) {
        HttpResponse resp(code);
        resp.set_header("Location", location);
        return resp;
    }
    
    // =========================================================================
    // 属性访问和设置
    // =========================================================================
    
    /// @brief 获取状态码
    http::StatusCode status() const noexcept { return status_; }
    
    /// @brief 设置状态码
    HttpResponse& set_status(http::StatusCode status) {
        status_ = status;
        return *this;
    }
    
    /// @brief 获取响应体
    const std::string& body() const noexcept { return body_; }
    
    /// @brief 设置响应体
    HttpResponse& set_body(std::string body) {
        body_ = std::move(body);
        return *this;
    }
    
    /// @brief 获取所有响应头
    const Headers& headers() const noexcept { return headers_; }
    
    /// @brief 设置响应头
    HttpResponse& set_header(const std::string& name, const std::string& value) {
        headers_[name] = value;
        return *this;
    }
    
    // =========================================================================
    // 序列化
    // =========================================================================
    
    /**
     * @brief 将响应序列化为 HTTP 格式
     * @return HTTP 响应字符串
     */
    std::string serialize() const {
        std::ostringstream oss;
        
        // 状态行
        oss << "HTTP/1.1 " << static_cast<int>(status_) 
            << " " << http::status_text(status_) << "\r\n";
        
        // 响应头
        for (const auto& [name, value] : headers_) {
            oss << name << ": " << value << "\r\n";
        }
        
        // Content-Length（如果未设置）
        if (headers_.find("Content-Length") == headers_.end()) {
            oss << "Content-Length: " << body_.size() << "\r\n";
        }
        
        // 默认关闭连接（简化实现）
        if (headers_.find("Connection") == headers_.end()) {
            oss << "Connection: close\r\n";
        }
        
        // 空行
        oss << "\r\n";
        
        // 响应体
        oss << body_;
        
        return oss.str();
    }
    
    /**
     * @brief 将响应发送到 TCP 连接
     * @param conn TCP 连接
     */
    Task<void> send(TcpConnection& conn) const {
        std::string data = serialize();
        co_await conn.write(data);
    }

private:
    // =========================================================================
    // 数据成员
    // =========================================================================
    
    /// @brief HTTP 状态码
    http::StatusCode status_ = http::StatusCode::OK;
    
    /// @brief 响应头
    Headers headers_;
    
    /// @brief 响应体
    std::string body_;
};

// =============================================================================
// HttpServer - HTTP 服务器
// =============================================================================

/**
 * @brief HTTP 服务器类
 * @details 基于 TcpServer 构建的 HTTP 服务器，支持路由注册
 */
class HttpServer {
public:
    /// @brief 请求处理器类型
    using Handler = std::function<Task<HttpResponse>(const HttpRequest&)>;
    
    /// @brief 路由键类型（方法 + 路径）
    struct RouteKey {
        http::Method method;
        std::string path;
        
        bool operator<(const RouteKey& other) const {
            if (method != other.method) {
                return method < other.method;
            }
            return path < other.path;
        }
    };
    
    HttpServer() = default;
    
    // =========================================================================
    // 路由注册
    // =========================================================================
    
    /// @brief 注册 GET 路由
    HttpServer& get(const std::string& path, Handler handler) {
        routes_[{http::Method::GET, path}] = std::move(handler);
        return *this;
    }
    
    /// @brief 注册 POST 路由
    HttpServer& post(const std::string& path, Handler handler) {
        routes_[{http::Method::POST, path}] = std::move(handler);
        return *this;
    }
    
    /// @brief 注册 PUT 路由
    HttpServer& put(const std::string& path, Handler handler) {
        routes_[{http::Method::PUT, path}] = std::move(handler);
        return *this;
    }
    
    /// @brief 注册 DELETE 路由
    HttpServer& del(const std::string& path, Handler handler) {
        routes_[{http::Method::DELETE_, path}] = std::move(handler);
        return *this;
    }
    
    /// @brief 注册任意方法的路由
    HttpServer& route(http::Method method, const std::string& path, Handler handler) {
        routes_[{method, path}] = std::move(handler);
        return *this;
    }
    
    /// @brief 设置 404 处理器
    HttpServer& set_not_found_handler(Handler handler) {
        not_found_handler_ = std::move(handler);
        return *this;
    }
    
    /// @brief 设置错误处理器
    HttpServer& set_error_handler(
        std::function<Task<HttpResponse>(const HttpRequest&, const std::exception&)> handler) {
        error_handler_ = std::move(handler);
        return *this;
    }
    
    // =========================================================================
    // 服务器启动
    // =========================================================================
    
    /**
     * @brief 启动 HTTP 服务器
     * @param host 监听地址
     * @param port 监听端口
     * @param token 取消令牌
     */
    Task<void> serve(const std::string& host, uint16_t port,
                     CancellationToken token = CancellationToken::none()) {
        TcpServer tcp_server;
        
        // 设置连接处理器
        tcp_server.on_connection([this](TcpConnection conn) -> Task<void> {
            co_await handle_connection(std::move(conn));
        });
        
        // 设置 spawn 函数（如果有）
        if (spawn_fn_) {
            tcp_server.set_spawn(spawn_fn_);
        }
        
        co_await tcp_server.serve(host, port, token);
    }
    
    /// @brief 设置 spawn 函数
    void set_spawn(std::function<void(Task<void>)> spawn_fn) {
        spawn_fn_ = std::move(spawn_fn);
    }

private:
    /**
     * @brief 处理单个 HTTP 连接
     * @param conn TCP 连接
     */
    Task<void> handle_connection(TcpConnection conn) {
        try {
            // 解析请求
            HttpRequest req = co_await HttpRequest::parse(conn);
            
            // 查找路由
            HttpResponse resp;
            std::exception_ptr handler_exception;
            
            auto it = routes_.find({req.method(), req.path()});
            if (it != routes_.end()) {
                try {
                    resp = co_await it->second(req);
                } catch (...) {
                    // 保存异常，不在 catch 块中使用 co_await
                    handler_exception = std::current_exception();
                }
                
                // 在 catch 块外处理异常
                if (handler_exception) {
                    try {
                        std::rethrow_exception(handler_exception);
                    } catch (const std::exception& e) {
                        if (error_handler_) {
                            resp = co_await error_handler_(req, e);
                        } else {
                            resp = HttpResponse::internal_error(
                                std::string("Handler error: ") + e.what());
                        }
                    }
                }
            } else {
                if (not_found_handler_) {
                    resp = co_await not_found_handler_(req);
                } else {
                    resp = HttpResponse::not_found(
                        "404 Not Found: " + req.path());
                }
            }
            
            // 发送响应
            co_await resp.send(conn);
            
        } catch (const std::exception& e) {
            // 解析错误或连接错误，静默忽略
            // 实际应用中应该记录日志
        }
        
        co_return;
    }

private:
    // =========================================================================
    // 数据成员
    // =========================================================================
    
    /// @brief 路由表（路由键 -> 处理器）
    std::map<RouteKey, Handler> routes_;
    
    /// @brief 404 处理器
    Handler not_found_handler_;
    
    /// @brief 错误处理器
    std::function<Task<HttpResponse>(const HttpRequest&, const std::exception&)> error_handler_;
    
    /// @brief spawn 函数
    std::function<void(Task<void>)> spawn_fn_;
};

} // namespace zlcoro
