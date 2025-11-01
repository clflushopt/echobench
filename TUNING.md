# System Tuning Guide for io_uring Benchmarks

This guide helps you configure your system for optimal benchmark performance.

## Kernel Parameters

Use with absolute care, changing kernel settings might be disallowed or might damage your system :).

### Check Current Settings
```bash
# Check io_uring status
cat /proc/sys/kernel/io_uring_disabled

# Check file descriptor limits
ulimit -n

# Check network settings
sysctl net.core.somaxconn
sysctl net.ipv4.tcp_max_syn_backlog
```

### Essential Tuning

#### 1. Enable io_uring (if disabled)

```bash
# Check if disabled
cat /proc/sys/kernel/io_uring_disabled

# Enable for current session
sudo sysctl -w kernel.io_uring_disabled=0

# Enable permanently
echo "kernel.io_uring_disabled=0" | sudo tee -a /etc/sysctl.conf
sudo sysctl -p
```

#### 2. Increase File Descriptor Limits

```bash
# For current session
ulimit -n 100000

# Permanent (add to /etc/security/limits.conf)
echo "* soft nofile 100000" | sudo tee -a /etc/security/limits.conf
echo "* hard nofile 100000" | sudo tee -a /etc/security/limits.conf

# For systemd services, edit /etc/systemd/system.conf:
# DefaultLimitNOFILE=100000
```

#### 3. TCP/Network Tuning
```bash
# Increase connection queue sizes
sudo sysctl -w net.core.somaxconn=4096
sudo sysctl -w net.ipv4.tcp_max_syn_backlog=4096

# Increase port range
sudo sysctl -w net.ipv4.ip_local_port_range="1024 65535"

# For local testing: enable faster TIME_WAIT recycling
sudo sysctl -w net.ipv4.tcp_tw_reuse=1

# Make permanent (add to /etc/sysctl.conf)
cat <<EOF | sudo tee -a /etc/sysctl.conf
net.core.somaxconn=4096
net.ipv4.tcp_max_syn_backlog=4096
net.ipv4.ip_local_port_range=1024 65535
net.ipv4.tcp_tw_reuse=1
EOF

sudo sysctl -p
```

## Performance Tuning Script

If you're feeling YOLO; create and run this script before benchmarking:

```bash
#!/bin/bash
# tune_system.sh

echo "Tuning system for io_uring benchmarks..."

# Enable io_uring
if [ -e /proc/sys/kernel/io_uring_disabled ]; then
    sudo sysctl -w kernel.io_uring_disabled=0
    echo "✓ io_uring enabled"
fi

# Set file descriptor limits
ulimit -n 100000
echo "✓ File descriptor limit: $(ulimit -n)"

# Network tuning
sudo sysctl -w net.core.somaxconn=4096 > /dev/null
sudo sysctl -w net.ipv4.tcp_max_syn_backlog=4096 > /dev/null
sudo sysctl -w net.ipv4.ip_local_port_range="1024 65535" > /dev/null
sudo sysctl -w net.ipv4.tcp_tw_reuse=1 > /dev/null
echo "✓ Network parameters tuned"

# Display current settings
echo ""
echo "Current settings:"
echo "  io_uring: $(cat /proc/sys/kernel/io_uring_disabled)"
echo "  nofile: $(ulimit -n)"
echo "  somaxconn: $(sysctl -n net.core.somaxconn)"
echo "  tcp_max_syn_backlog: $(sysctl -n net.ipv4.tcp_max_syn_backlog)"
echo ""
echo "System ready for benchmarking!"
```

## CPU Configuration

Again, use with absolute care, ideally on a baremetal test box and not your workstation.

### Disable CPU Frequency Scaling (for consistent results)
```bash
# Check current governor
cat /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor

# Set to performance mode
echo performance | sudo tee /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor

# Or use cpupower tool
sudo cpupower frequency-set -g performance
```

### CPU Affinity

For single-threaded benchmarks, pin to specific CPU:
```bash
# Run on CPU 0
taskset -c 0 ./echo_benchmark -m multishot &
taskset -c 1 ./load_generator -t 1 -c 100
```

## Memory Configuration

### Increase Memory Map Limits

```bash
# Check current limit
sysctl vm.max_map_count

# Increase for io_uring
sudo sysctl -w vm.max_map_count=262144

# Make permanent
echo "vm.max_map_count=262144" | sudo tee -a /etc/sysctl.conf
```

### Huge Pages (optional, not tested)

```bash
# Check current huge pages
cat /proc/meminfo | grep Huge

# Configure huge pages
sudo sysctl -w vm.nr_hugepages=1024

# Make permanent
echo "vm.nr_hugepages=1024" | sudo tee -a /etc/sysctl.conf
```

## Monitoring During Benchmarks

### CPU Usage

```bash
# Real-time monitoring
top -H

# Or use htop
htop

# Record CPU usage
sar -u 1 60 > cpu_usage.log
```

### Network Statistics

```bash
# Monitor connections
watch -n1 'ss -s'

# Monitor network traffic
iftop -i lo  # for localhost
nethogs

# Record network stats
sar -n DEV 1 60 > network_stats.log
```

### System Calls

```bash
# Trace syscalls (will slow down performance)
strace -c -p $(pgrep echo_benchmark)

# Count syscalls
perf stat -e 'syscalls:*' -p $(pgrep echo_benchmark)
```

### io_uring Statistics
```bash
# If available, check io_uring stats
cat /proc/*/io_uring_stats
```

## F.A.Q

### "Cannot allocate memory"
```bash
# Increase limits
sudo sysctl -w vm.max_map_count=262144
sudo sysctl -w vm.overcommit_memory=1
```

### "Too many open files"
```bash
# Increase limit
ulimit -n 100000

# Check system-wide limit
cat /proc/sys/fs/file-max

# Increase system-wide
sudo sysctl -w fs.file-max=2097152
```

### "Address already in use"
```bash
# Check what's using the port
sudo netstat -tlnp | grep 9999
sudo ss -tlnp | grep 9999

# Kill process using port
sudo fuser -k 9999/tcp

# Or wait for TIME_WAIT (about 60 seconds)
# Or use a different port
```

### io_uring Permission Denied
```bash
# Check if disabled
cat /proc/sys/kernel/io_uring_disabled

# Some systems disable for unprivileged users
# May need to run with sudo or adjust permissions
```

## Configuration Checklist

Before running benchmarks, verify:

- [ ] io_uring enabled (`/proc/sys/kernel/io_uring_disabled == 0`)
- [ ] File descriptors: `ulimit -n >= 100000`
- [ ] somaxconn: `>= 4096`
- [ ] Port range: `1024-65535`
- [ ] CPU governor: `performance`
- [ ] No other heavy processes running
- [ ] Sufficient RAM available
- [ ] Network buffers tuned

## Reset to Defaults

To restore default settings:

```bash
#!/bin/bash
# reset_tuning.sh

# Reset CPU governor
echo powersave | sudo tee /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor

# Reset network settings
sudo sysctl -w net.core.somaxconn=128
sudo sysctl -w net.ipv4.tcp_max_syn_backlog=512
sudo sysctl -w net.ipv4.tcp_tw_reuse=0
sudo sysctl -w net.ipv4.ip_local_port_range="32768 60999"

# Reset file descriptor limit
ulimit -n 1024

echo "System settings restored to defaults"
```

## Best Practices

1. **Run benchmarks on an isolated baremetal box**.
2. **Multiple runs**: Run each benchmark 3-5 times and average
3. **Warm-up**: Discard first run to avoid cold-start effects
4. **Consistent load**: Ensure load generator doesn't bottleneck
5. **Monitor resources**: Watch for CPU/memory saturation
6. **Document config**: Record all settings for reproducibility
