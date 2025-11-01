#!/bin/bash

# Benchmark configuration
PORT=9999
DURATION=30
MESSAGE_SIZES=(128 1024 4096)
CONNECTION_CONFIGS=(
    "1:100"    # 1 thread, 100 connections
    "4:50"     # 4 threads, 50 connections each (200 total)
    "8:25"     # 8 threads, 25 connections each (200 total)
)
MODES=("epoll" "uring" "multishot")

# Output directory
RESULTS_DIR="results_$(date +%Y%m%d_%H%M%S)"
mkdir -p "$RESULTS_DIR"

echo "=== IO_URING Echo Server Benchmark Suite ==="
echo "Results will be saved to: $RESULTS_DIR"
echo ""

# Check if programs are built
if [ ! -f "./echobench" ] || [ ! -f "./loadgen" ]; then
    echo "Building programs..."
    make clean && make
    if [ $? -ne 0 ]; then
        echo "Build failed!"
        exit 1
    fi
fi

# Function to run a single benchmark
run_benchmark() {
    local mode=$1
    local threads=$2
    local connections=$3
    local msg_size=$4

    local test_name="${mode}_t${threads}_c${connections}_m${msg_size}"
    local output_file="$RESULTS_DIR/${test_name}.txt"

    echo -n "Running: $test_name ... "

    # Start server
    ./echobench -m "$mode" -p $PORT > "$RESULTS_DIR/${test_name}_server.log" 2>&1 &
    local server_pid=$!

    # Wait for server to start
    sleep 2

    # Check if server is running
    if ! kill -0 $server_pid 2>/dev/null; then
        echo "FAILED (server didn't start)"
        return 1
    fi

    # Run load generator
    ./loadgen -s 127.0.0.1 -p $PORT \
                     -t $threads -c $connections \
                     -m $msg_size -d $DURATION > "$output_file" 2>&1

    local client_exit=$?

    # Stop server
    kill -INT $server_pid 2>/dev/null
    wait $server_pid 2>/dev/null

    if [ $client_exit -eq 0 ]; then
        echo "DONE"
        return 0
    else
        echo "FAILED"
        return 1
    fi
}

# Run all benchmark combinations
total_tests=0
passed_tests=0

for mode in "${MODES[@]}"; do
    for conn_config in "${CONNECTION_CONFIGS[@]}"; do
        threads=$(echo $conn_config | cut -d: -f1)
        connections=$(echo $conn_config | cut -d: -f2)

        for msg_size in "${MESSAGE_SIZES[@]}"; do
            total_tests=$((total_tests + 1))

            if run_benchmark "$mode" "$threads" "$connections" "$msg_size"; then
                passed_tests=$((passed_tests + 1))
            fi

            # Small delay between tests
            sleep 2
        done
    done
done

echo ""
echo "=== Benchmark Complete ==="
echo "Tests: $passed_tests/$total_tests passed"
echo "Results saved to: $RESULTS_DIR"

# Generate summary report
echo ""
echo "Generating summary report..."

SUMMARY_FILE="$RESULTS_DIR/SUMMARY.txt"

cat > "$SUMMARY_FILE" << EOF
=== IO_URING Echo Server Benchmark Summary ===
Date: $(date)
Duration per test: ${DURATION}s

Configuration:
- Modes tested: ${MODES[@]}
- Message sizes: ${MESSAGE_SIZES[@]} bytes
- Connection configs: ${CONNECTION_CONFIGS[@]}

EOF

echo "" >> "$SUMMARY_FILE"
echo "=== Results ===" >> "$SUMMARY_FILE"
echo "" >> "$SUMMARY_FILE"

# Extract key metrics from each test
printf "%-20s %-10s %-10s %-15s %-15s %-15s\n" \
       "Test" "Threads" "Conns" "Msg/s" "Throughput" "Errors" >> "$SUMMARY_FILE"
echo "--------------------------------------------------------------------------------" >> "$SUMMARY_FILE"

for result_file in "$RESULTS_DIR"/*.txt; do
    if [ "$result_file" == "$SUMMARY_FILE" ]; then
        continue
    fi

    filename=$(basename "$result_file" .txt)

    # Extract mode, threads, connections, and message size from filename
    mode=$(echo $filename | cut -d_ -f1)
    threads=$(echo $filename | cut -d_ -f2 | sed 's/t//')
    conns=$(echo $filename | cut -d_ -f3 | sed 's/c//')
    msgsize=$(echo $filename | cut -d_ -f4 | sed 's/m//')

    # Extract metrics
    msg_rate=$(grep "Sent:" "$result_file" | grep -oP '\(\K[0-9.]+(?= msg/s)')
    throughput=$(grep "Rate:" "$result_file" | grep "received" -A1 | tail -1 | grep -oP '[0-9.]+(?= MB/s)' | head -1)
    errors=$(grep "Errors:" "$result_file" | grep -oP '[0-9]+')

    if [ -n "$msg_rate" ] && [ -n "$throughput" ]; then
        printf "%-20s %-10s %-10s %-15s %-15s %-15s\n" \
               "$mode" "$threads" "$conns" "$msg_rate" "${throughput}MB/s" "$errors" >> "$SUMMARY_FILE"
    fi
done

echo "" >> "$SUMMARY_FILE"
cat "$SUMMARY_FILE"

# Generate comparison charts (simple text-based)
echo ""
echo "=== Performance Comparison ===" | tee -a "$SUMMARY_FILE"
echo "" | tee -a "$SUMMARY_FILE"

for msg_size in "${MESSAGE_SIZES[@]}"; do
    echo "Message Size: $msg_size bytes" | tee -a "$SUMMARY_FILE"

    for conn_config in "${CONNECTION_CONFIGS[@]}"; do
        threads=$(echo $conn_config | cut -d: -f1)
        connections=$(echo $conn_config | cut -d: -f2)
        total_conns=$((threads * connections))

        echo "  Config: ${threads}t x ${connections}c = ${total_conns} connections" | tee -a "$SUMMARY_FILE"

        for mode in "${MODES[@]}"; do
            result_file="$RESULTS_DIR/${mode}_t${threads}_c${connections}_m${msg_size}.txt"

            if [ -f "$result_file" ]; then
                msg_rate=$(grep "Sent:" "$result_file" | grep -oP '\(\K[0-9.]+(?= msg/s)')
                throughput=$(grep "Rate:" "$result_file" | grep "received" -A1 | tail -1 | grep -oP '[0-9.]+(?= MB/s)' | head -1)

                if [ -n "$msg_rate" ] && [ -n "$throughput" ]; then
                    printf "    %-12s: %10.2f msg/s, %8.2f MB/s\n" \
                           "$mode" "$msg_rate" "$throughput" | tee -a "$SUMMARY_FILE"
                fi
            fi
        done
        echo "" | tee -a "$SUMMARY_FILE"
    done
done

echo ""
echo "Full results and logs available in: $RESULTS_DIR"
