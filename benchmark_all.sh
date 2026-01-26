#!/bin/bash
# 全方案性能测试脚本

set -e

PORT=8080
THREADS=8
CONNECTIONS=1000
DURATION=15s
BUILD_DIR="build/examples"

echo "========================================"
echo "ZLCoro 全方案性能对比测试"
echo "========================================"
echo "测试参数:"
echo "  服务器核心数: $THREADS"
echo "  wrk 线程数: $THREADS"
echo "  wrk 连接数: $CONNECTIONS"
echo "  测试时长: $DURATION"
echo "========================================"
echo ""

# 确保没有残留进程
pkill -9 http_server_v2 2>/dev/null || true
pkill -9 http_server 2>/dev/null || true
sleep 2

# 测试结果数组
declare -A RESULTS

# 测试函数
run_test() {
    local name=$1
    local server_cmd=$2
    local server_cores=$3
    
    echo "----------------------------------------"
    echo "测试: $name"
    echo "服务器命令: $server_cmd"
    echo "----------------------------------------"
    
    # 启动服务器
    eval "$server_cmd" &
    local server_pid=$!
    sleep 3
    
    # 检查服务器是否启动成功
    if ! kill -0 $server_pid 2>/dev/null; then
        echo "错误: 服务器启动失败"
        return 1
    fi
    
    # wrk 压测
    echo "开始 wrk 压测..."
    local result=$(wrk -t$THREADS -c$CONNECTIONS -d$DURATION --latency http://127.0.0.1:$PORT/ 2>&1)
    
    # 提取关键指标
    local qps=$(echo "$result" | grep "Requests/sec:" | awk '{print $2}')
    local p50=$(echo "$result" | grep "50%" | awk '{print $2}')
    local p99=$(echo "$result" | grep "99%" | awk '{print $2}')
    local avg=$(echo "$result" | grep "Latency" | head -1 | awk '{print $2}')
    
    # 停止服务器
    kill -TERM $server_pid 2>/dev/null || true
    sleep 2
    pkill -9 http_server_v2 2>/dev/null || true
    pkill -9 http_server 2>/dev/null || true
    sleep 1
    
    # 保存结果
    RESULTS["$name"]="$qps|$avg|$p50|$p99"
    
    echo "结果: QPS=$qps, 平均延迟=$avg, P50=$p50, P99=$p99"
    echo ""
}

# 1. epoll 单核单事件循环
run_test "epoll-单核" \
    "cd $BUILD_DIR && ./http_server_v2 0.0.0.0 $PORT 1 epoll" \
    1

# 2. epoll 多核 SO_REUSEPORT (当前版本)
run_test "epoll-多核-SO_REUSEPORT" \
    "cd $BUILD_DIR && ./http_server_v2 0.0.0.0 $PORT $THREADS epoll" \
    $THREADS

# 3. io_uring 单核
run_test "io_uring-单核" \
    "cd $BUILD_DIR && ./http_server_v2 0.0.0.0 $PORT 1 io_uring" \
    1

# 4. io_uring 多核 SO_REUSEPORT
run_test "io_uring-多核-SO_REUSEPORT" \
    "cd $BUILD_DIR && ./http_server_v2 0.0.0.0 $PORT $THREADS io_uring" \
    $THREADS

# 打印汇总表格
echo "========================================"
echo "测试结果汇总"
echo "========================================"
printf "%-30s %-15s %-12s %-12s %-12s\n" "方案" "QPS" "平均延迟" "P50延迟" "P99延迟"
echo "----------------------------------------"

for name in "epoll-单核" "epoll-多核-SO_REUSEPORT" "io_uring-单核" "io_uring-多核-SO_REUSEPORT"; do
    if [[ -n "${RESULTS[$name]}" ]]; then
        IFS='|' read -r qps avg p50 p99 <<< "${RESULTS[$name]}"
        printf "%-30s %-15s %-12s %-12s %-12s\n" "$name" "$qps" "$avg" "$p50" "$p99"
    fi
done

echo "========================================"
echo "测试完成"
echo "========================================"
