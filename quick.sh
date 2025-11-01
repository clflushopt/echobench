#!/bin/bash

echo "--- IO_URING Benchmark Quick Test ---"
echo ""

if [ ! -f "./echobench" ] || [ ! -f "./loadgen" ]; then
  echo "Building ..."
  make clean && make
  if [ $? -ne 0 ]; then
    echo "Build failed!"
    exit 1
  fi
  echo "Build successful!"
  echo ""
fi

kernel_version=$(uname -r | cut -d. -f1,2)
echo "Kernel version: $(uname -r)"
if [[ $(echo "$kernel_version < 5.1" | bc -l) -eq 1 ]]; then
  echo "WARNING: Kernel version < 5.1, io_uring may not be available"
fi
echo ""

run_quick_test() {
  local mode=$1
  local port=9999

  echo "Testing $mode mode ..."

  ./echobench -m "$mode" -p $port > /tmp/test_server.log 2>&1 &
  local server_pid=$!

  sleep 2

  if ! kill -0 $server_pid 2>/dev/null; then
    echo "  ✗ FAILED: Server didn't start"
    cat /tmp/test_server.log
    return 1
  fi

  ./loadgen -s 127.0.0.1 -p $port -t 8 -c 10 -m 128 -d 3 > /tmp/test_client.log 2>&1
  local client_exit=$?

  local msg_rate=$(grep "Sent:" /tmp/test_client.log | grep -oP '\(\K[0-9.]+(?= msg/s)')

  kill -INT $server_pid 2>/dev/null
  wait $server_pid 2>/dev/null

  if [ $client_exit -eq 0 ] && [ -n "$msg_rate" ]; then
    echo "  ✓ SUCCESS: $msg_rate msg/s"
    return 0
  else
    echo "  ✗ FAILED"
    echo "Client log:"
    cat /tmp/test_client.log
    return 1
  fi
}

modes=("epoll")
uring_modes=()

if [ -e /proc/sys/kernel/io_uring_disabled ]; then
  uring_disabled=$(cat /proc/sys/kernel/io_uring_disabled)
  if [ "$uring_disabled" == "0" ]; then
    uring_modes+=("uring" "multishot")
  else
    echo "WARNING: io_uring is disabled on this system"
    echo "To enable: sudo sysctl -w kernel.io_uring_disabled=0"
    echo ""
  fi
else
  uring_modes+=("uring" "multishot")
fi

all_modes=("${modes[@]}" "${uring_modes[@]}")
success_count=0
total_count=${#all_modes[@]}

for mode in "${all_modes[@]}"; do
  if run_quick_test "$mode"; then
    success_count=$((success_count + 1))
  fi
  echo ""
  sleep 1
done

echo "================================================="
echo "Results: $success_count/$total_count tests passed"
echo "================================================="

if [ $success_count -eq $total_count ]; then
  echo ""
  echo "✓ All tests passed!"
  echo ""
  exit 0
else
  echo ""
  echo "✗ Some tests failed, check logs above."
  echo ""
  exit 1
fi
