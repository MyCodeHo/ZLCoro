#!/bin/bash
set -e
PORT=8080
THREADS=8
CONNECTIONS=1000
DURATION=15s
BUILD_DIR="/home/zpw/study/MyProject/ZLCoro/build/examples"
RESULTS_FILE="heavy_bench_results.txt"

function cleanup() {
  pkill -9 http_server_v2 2>/dev/null || true
  pkill -9 -f shared_fd_server 2>/dev/null || true
  pkill -9 -f shared_fd 2>/dev/null || true
  sleep 2
}

function run_wrk() {
  wrk -t${THREADS} -c${CONNECTIONS} -d${DURATION} --latency http://127.0.0.1:${PORT}/ 2>&1
}

function extract_metrics() {
  local output="$1"
  local qps=$(echo "$output" | grep "Requests/sec:" | awk '{print $2}')
  local avg=$(echo "$output" | grep "Latency" | head -1 | awk '{print $2}')
  local p50=$(echo "$output" | grep "50%" | awk '{print $2}')
  local p99=$(echo "$output" | grep "99%" | awk '{print $2}')
  echo "$qps|$avg|$p50|$p99"
}

function run_case() {
  local scenario="$1"
  local name="$2"
  local cmd="$3"

  echo "[$scenario] $name" | tee -a "$RESULTS_FILE"
  cleanup
  eval "$cmd" &
  local pid=$!
  sleep 3

  local output=$(run_wrk)
  echo "$output" >> "$RESULTS_FILE"
  local metrics=$(extract_metrics "$output")
  echo "METRICS=$metrics" | tee -a "$RESULTS_FILE"
  kill -TERM $pid 2>/dev/null || true
  cleanup
  echo "" | tee -a "$RESULTS_FILE"
}

: > "$RESULTS_FILE"

# 场景 A: CPU 重负载
CPU_ITERS=20000
IO_WAIT=0
SCENARIO="CPU重负载"
run_case "$SCENARIO" "epoll-单核" "cd $BUILD_DIR && ./http_server_v2 -p $PORT -c 1 -x $CPU_ITERS -w $IO_WAIT"
run_case "$SCENARIO" "epoll-多核-SO_REUSEPORT" "cd $BUILD_DIR && ./http_server_v2 -p $PORT -c $THREADS -x $CPU_ITERS -w $IO_WAIT"
run_case "$SCENARIO" "epoll-多核-惊群" "./shared_fd_server $PORT $THREADS $CPU_ITERS $IO_WAIT"
run_case "$SCENARIO" "io_uring-单核" "cd $BUILD_DIR && ./http_server_v2 -u -p $PORT -c 1 -x $CPU_ITERS -w $IO_WAIT"
run_case "$SCENARIO" "io_uring-多核-SO_REUSEPORT" "cd $BUILD_DIR && ./http_server_v2 -u -p $PORT -c $THREADS -x $CPU_ITERS -w $IO_WAIT"

# 场景 B: I/O 等待重负载
CPU_ITERS=0
IO_WAIT=200
SCENARIO="IO等待重负载"
run_case "$SCENARIO" "epoll-单核" "cd $BUILD_DIR && ./http_server_v2 -p $PORT -c 1 -x $CPU_ITERS -w $IO_WAIT"
run_case "$SCENARIO" "epoll-多核-SO_REUSEPORT" "cd $BUILD_DIR && ./http_server_v2 -p $PORT -c $THREADS -x $CPU_ITERS -w $IO_WAIT"
run_case "$SCENARIO" "epoll-多核-惊群" "./shared_fd_server $PORT $THREADS $CPU_ITERS $IO_WAIT"
run_case "$SCENARIO" "io_uring-单核" "cd $BUILD_DIR && ./http_server_v2 -u -p $PORT -c 1 -x $CPU_ITERS -w $IO_WAIT"
run_case "$SCENARIO" "io_uring-多核-SO_REUSEPORT" "cd $BUILD_DIR && ./http_server_v2 -u -p $PORT -c $THREADS -x $CPU_ITERS -w $IO_WAIT"

# 场景 C: CPU+I/O 混合重负载
CPU_ITERS=10000
IO_WAIT=100
SCENARIO="混合重负载"
run_case "$SCENARIO" "epoll-单核" "cd $BUILD_DIR && ./http_server_v2 -p $PORT -c 1 -x $CPU_ITERS -w $IO_WAIT"
run_case "$SCENARIO" "epoll-多核-SO_REUSEPORT" "cd $BUILD_DIR && ./http_server_v2 -p $PORT -c $THREADS -x $CPU_ITERS -w $IO_WAIT"
run_case "$SCENARIO" "epoll-多核-惊群" "./shared_fd_server $PORT $THREADS $CPU_ITERS $IO_WAIT"
run_case "$SCENARIO" "io_uring-单核" "cd $BUILD_DIR && ./http_server_v2 -u -p $PORT -c 1 -x $CPU_ITERS -w $IO_WAIT"
run_case "$SCENARIO" "io_uring-多核-SO_REUSEPORT" "cd $BUILD_DIR && ./http_server_v2 -u -p $PORT -c $THREADS -x $CPU_ITERS -w $IO_WAIT"

echo "DONE" | tee -a "$RESULTS_FILE"
