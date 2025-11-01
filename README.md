# IO_URING Echo Server Benchmark Suite

A benchmark suite to compare the performance of different I/O mechanisms on Linux.

- Traditional `epoll()` (edge-triggered)
- `io_uring` single-shot operations
- `io_uring` multishot operations

## Requirements

- Linux kernel 5.19+ (for multishot support, 5.1+ for basic io_uring)
- liburing development libraries
- GCC compiler
- pthread support

### Installing Dependencies

**Ubuntu/Debian:**
```bash
sudo apt-get update
sudo apt-get install build-essential liburing-dev
```

**Fedora/RHEL:**
```bash
sudo dnf install gcc make liburing-devel
```

**Arch Linux:**
```bash
sudo pacman -S base-devel liburing
```

### Check Kernel Version
```bash
uname -r  # Should be 5.19+ for full multishot support
```

## Building

```bash
make clean
make
```

This will build two executables:

- `echobench` - The echo server with three modes
- `loadgen`   - The load testing client

## Quick Start

### 1. Start a Server

**Epoll mode:**
```bash
./echobench -m epoll -p 9999
```

**io_uring mode (single-shot):**
```bash
./echobench -m uring -p 9999
```

**io_uring multishot mode:**
```bash
./echobench -m multishot -p 9999
```

### 2. Run Load Generator

In another terminal:
```bash
./loadgen -s 127.0.0.1 -p 9999 -t 4 -c 50 -m 1024 -d 30
```

This will:
- Connect to server at 127.0.0.1:9999
- Use 4 threads with 50 connections each (200 total)
- Send 1024-byte messages
- Run for 30 seconds

## Automated Benchmarking

Run the complete benchmark suite:

```bash
chmod +x run_benchmark.sh
./run_benchmark.sh
```

This will:
1. Test all three modes (epoll, uring, multishot)
2. Try different connection patterns (1, 4, 8 threads)
3. Test multiple message sizes (128, 1024, 4096 bytes)
4. Generate a comprehensive summary report

Results will be saved in a timestamped directory: `results_YYYYMMDD_HHMMSS/`

## Server Usage

```
./echobench [-m mode] [-p port]
  -m mode: epoll, uring, multishot (default: epoll)
  -p port: port number (default: 9999)
```

**Output:**
```
EPOLL server listening on port 9999
[10.5s] Connections: 200 active, 200 total | Messages: 1523847 (145080 msg/s) |
Throughput: 1123.17 Mb/s (140.40 MB/s) | Total: 1472.11 MB
```

## Load Generator Usage

```
./loadgen [options]
  -s server_ip   Server IP address (default: 127.0.0.1)
  -p port        Server port (default: 9999)
  -c connections Number of connections per thread (default: 100)
  -t threads     Number of threads (default: 1)
  -m size        Message size in bytes (default: 1024)
  -d duration    Duration in seconds (default: 30)
```

**Example Output:**
```
=== Echo Server Benchmark ===
Server: 127.0.0.1:9999
Threads: 4
Connections per thread: 50
Total connections: 200
Message size: 1024 bytes
Duration: 30 seconds
=============================

Thread 0: Connected, starting benchmark...
Thread 1: Connected, starting benchmark...
Thread 2: Connected, starting benchmark...
Thread 3: Connected, starting benchmark...
...

=== Results ===
Elapsed time: 30.02 seconds

Messages:
  Sent:     1523847 (50762.45 msg/s)
  Received: 1523847 (50762.45 msg/s)
  Errors:   0

Throughput (sent):
  Bytes:    1560547328 (1488.23 MB)
  Rate:     49.58 MB/s (396.66 Mb/s)

Throughput (received):
  Bytes:    1560547328 (1488.23 MB)
  Rate:     49.58 MB/s (396.66 Mb/s)
```

## Benchmark Examples

### Test 1: High Message Rate (Small Messages)
```bash
# Server
./echobench -m multishot -p 9999

# Client (8 threads × 25 connections, 128-byte messages)
./loadgen -s 127.0.0.1 -p 9999 -t 8 -c 25 -m 128 -d 60
```

### Test 2: High Throughput (Large Messages)
```bash
# Server
./echobench -m uring -p 9999

# Client (4 threads × 50 connections, 4KB messages)
./loadgen -s 127.0.0.1 -p 9999 -t 4 -c 50 -m 4096 -d 60
```

### Test 3: Connection Scalability
```bash
# Server
./echobench -m multishot -p 9999

# Client (1 thread, 1000 connections, 1KB messages)
./loadgen -s 127.0.0.1 -p 9999 -t 1 -c 1000 -m 1024 -d 60
```

## Performance Tuning

SEE [**TUNING.md**](TUNING.md)

## F.A.Q

### "Address already in use"

Wait for TIME_WAIT to clear, or use different port:
```bash
./echobench -m epoll -p 10000
```

### "Too many open files"

Increase ulimit:
```bash
ulimit -n 100000
```

### "Cannot allocate memory"

Reduce connection count or increase system limits:
```bash
sudo sysctl -w vm.max_map_count=262144
```

### io_uring not available

Check kernel version:
```bash
uname -r  # Need 5.1+ for basic, 5.19+ for multishot
```

### Low performance

- Check CPU usage (`top`, `htop`)
- Monitor network with `iftop` or `nethogs`
- Ensure no other services using same port
- Try different message sizes and connection counts

## Further Reading

- [io_uring documentation](https://kernel.dk/io_uring.pdf)
- [liburing repository](https://github.com/axboe/liburing)
- [Efficient IO with io_uring](https://kernel.dk/io_uring-whatsnew.pdf)
- [io_uring by example](https://unixism.net/loti/)

## License

This benchmark suite is provided as-is for educational and testing purposes.

## Contributing

Feel free to:
- Add new benchmark scenarios
- Improve measurement accuracy
- Add visualization tools (we have scripts for `gnuplot` see `plot.py`)
- Port to other platforms

## Known Limitations

- Simplified error handling
- No SSL/TLS support
- Single-process server (no multi-process)
- No thread pinning or other topology setting.
- Processing messages allocates via `malloc`.

## Future Enhancements

- [ ] Add latency measurements (min/avg/max/p99)
- [ ] CPU usage monitoring
- [ ] Memory usage tracking
- [✓] Graphical output (plots)
- [ ] Support for UDP
- [ ] Zero-copy everywhere.
- [ ] Better allocations.
