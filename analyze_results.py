#!/usr/bin/env python3
"""
Analyze and compare io_uring benchmark results
"""

import os
import sys
import re
import argparse
from collections import defaultdict

def parse_result_file(filepath):
    """Extract key metrics from a result file."""
    metrics = {}

    try:
        with open(filepath, 'r') as f:
            content = f.read()

            # Extract message rate (sent)
            match = re.search(r'Sent:\s+\d+\s+\(([\d.]+)\s+msg/s\)', content)
            if match:
                metrics['msg_rate'] = float(match.group(1))

            # Extract throughput (received, MB/s)
            match = re.search(r'Throughput \(received\):.*?Rate:\s+([\d.]+)\s+MB/s', content, re.DOTALL)
            if match:
                metrics['throughput_mb'] = float(match.group(1))

            # Extract throughput (received, Mb/s)
            match = re.search(r'Throughput \(received\):.*?Rate:.*?\(([\d.]+)\s+Mb/s\)', content, re.DOTALL)
            if match:
                metrics['throughput_mbit'] = float(match.group(1))

            # Extract errors
            match = re.search(r'Errors:\s+(\d+)', content)
            if match:
                metrics['errors'] = int(match.group(1))

            # Extract elapsed time
            match = re.search(r'Elapsed time:\s+([\d.]+)\s+seconds', content)
            if match:
                metrics['elapsed'] = float(match.group(1))

    except Exception as e:
        print(f"Error parsing {filepath}: {e}", file=sys.stderr)

    return metrics

def parse_filename(filename):
    """Extract test parameters from filename."""
    # Expected format: mode_tTHREADS_cCONNS_mMSGSIZE.txt
    match = re.match(r'(\w+)_t(\d+)_c(\d+)_m(\d+)\.txt', filename)
    if match:
        return {
            'mode': match.group(1),
            'threads': int(match.group(2)),
            'conns': int(match.group(3)),
            'msgsize': int(match.group(4)),
            'total_conns': int(match.group(2)) * int(match.group(3))
        }
    return None

def format_bar(value, max_value, width=40):
    """Create a simple text-based bar chart."""
    if max_value == 0:
        return ""
    bar_length = int((value / max_value) * width)
    return "█" * bar_length + "░" * (width - bar_length)

def analyze_results(results_dir):
    """Analyze all results in a directory."""

    # Collect all results
    results = defaultdict(lambda: defaultdict(dict))

    for filename in os.listdir(results_dir):
        if not filename.endswith('.txt'):
            continue
        if filename == 'SUMMARY.txt':
            continue

        filepath = os.path.join(results_dir, filename)
        params = parse_filename(filename)

        if not params:
            continue

        metrics = parse_result_file(filepath)

        if not metrics:
            continue

        # Store by msgsize -> config -> mode
        config_key = f"{params['threads']}t×{params['conns']}c"
        results[params['msgsize']][config_key][params['mode']] = {
            **params,
            **metrics
        }

    return results

def print_comparison(results):
    """Print a formatted comparison of results."""

    print("\n" + "="*80)
    print("BENCHMARK COMPARISON")
    print("="*80)

    for msgsize in sorted(results.keys()):
        print(f"\n{'='*80}")
        print(f"MESSAGE SIZE: {msgsize} bytes")
        print(f"{'='*80}\n")

        for config in sorted(results[msgsize].keys()):
            modes_data = results[msgsize][config]

            if not modes_data:
                continue

            print(f"Configuration: {config}")
            print("-" * 80)

            # Find max values for normalization
            max_msg_rate = max((data['msg_rate'] for data in modes_data.values()
                              if 'msg_rate' in data), default=0)
            max_throughput = max((data['throughput_mb'] for data in modes_data.values()
                                if 'throughput_mb' in data), default=0)

            # Print results for each mode
            for mode in ['epoll', 'uring', 'multishot']:
                if mode not in modes_data:
                    continue

                data = modes_data[mode]
                msg_rate = data.get('msg_rate', 0)
                throughput = data.get('throughput_mb', 0)
                errors = data.get('errors', 0)

                print(f"\n  {mode.upper():12s}")
                print(f"    Message Rate:  {msg_rate:>12,.2f} msg/s")
                print(f"                   {format_bar(msg_rate, max_msg_rate)}")
                print(f"    Throughput:    {throughput:>12,.2f} MB/s")
                print(f"                   {format_bar(throughput, max_throughput)}")
                print(f"    Errors:        {errors:>12,}")

                # Calculate improvement over epoll
                if mode != 'epoll' and 'epoll' in modes_data:
                    epoll_rate = modes_data['epoll'].get('msg_rate', 0)
                    if epoll_rate > 0:
                        improvement = ((msg_rate - epoll_rate) / epoll_rate) * 100
                        print(f"    vs epoll:      {improvement:>+12.1f}%")

            print()

    print("="*80)

def print_summary_table(results):
    """Print a summary table of all results."""

    print("\n" + "="*100)
    print("SUMMARY TABLE")
    print("="*100)
    print()

    # Header
    print(f"{'Mode':<12} {'Config':<12} {'MsgSize':<10} {'Msg/s':<15} {'MB/s':<12} {'Errors':<10}")
    print("-" * 100)

    # Sort and print all results
    all_results = []
    for msgsize in sorted(results.keys()):
        for config in sorted(results[msgsize].keys()):
            for mode in ['epoll', 'uring', 'multishot']:
                if mode in results[msgsize][config]:
                    data = results[msgsize][config][mode]
                    all_results.append((
                        mode,
                        config,
                        msgsize,
                        data.get('msg_rate', 0),
                        data.get('throughput_mb', 0),
                        data.get('errors', 0)
                    ))

    for mode, config, msgsize, msg_rate, throughput, errors in sorted(all_results):
        print(f"{mode:<12} {config:<12} {msgsize:<10} {msg_rate:>12,.2f}  {throughput:>9,.2f}  {errors:>9,}")

    print("="*100)

def print_best_results(results):
    """Print the best performing configuration for each mode."""

    print("\n" + "="*80)
    print("BEST RESULTS BY MODE")
    print("="*80)
    print()

    best = {}

    for msgsize in results.keys():
        for config in results[msgsize].keys():
            for mode, data in results[msgsize][config].items():
                msg_rate = data.get('msg_rate', 0)
                if mode not in best or msg_rate > best[mode]['msg_rate']:
                    best[mode] = {
                        'msgsize': msgsize,
                        'config': config,
                        'msg_rate': msg_rate,
                        'throughput': data.get('throughput_mb', 0)
                    }

    for mode in ['epoll', 'uring', 'multishot']:
        if mode in best:
            data = best[mode]
            print(f"{mode.upper()}")
            print(f"  Best: {data['msg_rate']:,.2f} msg/s, {data['throughput']:.2f} MB/s")
            print(f"  Config: {data['config']}, Message size: {data['msgsize']} bytes")
            print()

    print("="*80)

def main():
    parser = argparse.ArgumentParser(description='Analyze io_uring benchmark results')
    parser.add_argument('results_dir', help='Directory containing benchmark results')
    parser.add_argument('--summary-only', action='store_true',
                       help='Only show summary table')
    parser.add_argument('--best-only', action='store_true',
                       help='Only show best results')

    args = parser.parse_args()

    if not os.path.isdir(args.results_dir):
        print(f"Error: {args.results_dir} is not a directory", file=sys.stderr)
        return 1

    results = analyze_results(args.results_dir)

    if not results:
        print("No valid results found", file=sys.stderr)
        return 1

    if args.summary_only:
        print_summary_table(results)
    elif args.best_only:
        print_best_results(results)
    else:
        print_comparison(results)
        print_summary_table(results)
        print_best_results(results)

    return 0

if __name__ == '__main__':
    sys.exit(main())
