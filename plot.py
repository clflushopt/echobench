#!/usr/bin/env python3
"""
Generate gnuplot performance graphs from io_uring benchmark results
"""

import os
import sys
import re
import argparse
import subprocess
from collections import defaultdict

def parse_result_file(filepath):
    """Extract key metrics from a result file."""
    metrics = {}

    try:
        with open(filepath, 'r') as f:
            content = f.read()

            match = re.search(r'Sent:\s+\d+\s+\(([\d.]+)\s+msg/s\)', content)
            if match:
                metrics['msg_rate'] = float(match.group(1))

            match = re.search(r'Throughput \(received\):.*?Rate:\s+([\d.]+)\s+MB/s', content, re.DOTALL)
            if match:
                metrics['throughput_mb'] = float(match.group(1))

            match = re.search(r'Throughput \(received\):.*?Rate:.*?\(([\d.]+)\s+Mb/s\)', content, re.DOTALL)
            if match:
                metrics['throughput_mbit'] = float(match.group(1))

            match = re.search(r'Errors:\s+(\d+)', content)
            if match:
                metrics['errors'] = int(match.group(1))

    except Exception as e:
        print(f"Error parsing {filepath}: {e}", file=sys.stderr)

    return metrics

def parse_filename(filename):
    """Extract test parameters from filename."""
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

def analyze_results(results_dir):
    """Analyze all results in a directory."""
    results = defaultdict(lambda: defaultdict(dict))

    for filename in os.listdir(results_dir):
        if not filename.endswith('.txt') or filename == 'SUMMARY.txt':
            continue

        filepath = os.path.join(results_dir, filename)
        params = parse_filename(filename)
        if not params:
            continue

        metrics = parse_result_file(filepath)
        if not metrics:
            continue

        config_key = f"{params['threads']}t×{params['conns']}c"
        results[params['msgsize']][config_key][params['mode']] = {
            **params,
            **metrics
        }

    return results

def check_gnuplot():
    """Check if gnuplot is available."""
    try:
        result = subprocess.run(['gnuplot', '--version'],
                              capture_output=True, check=True, text=True)
        version = result.stdout.strip()
        return True, version
    except (subprocess.CalledProcessError, FileNotFoundError):
        return False, None

def generate_data_files(results, output_dir):
    """Generate data files for gnuplot."""

    os.makedirs(output_dir, exist_ok=True)
    data_files = {}

    # 1. Message rate by config (for each message size)
    for msgsize in sorted(results.keys()):
        filename = os.path.join(output_dir, f'msgrate_{msgsize}.dat')
        data_files[f'msgrate_{msgsize}'] = filename

        with open(filename, 'w') as f:
            f.write("# Config Epoll Uring Multishot\n")
            for config in sorted(results[msgsize].keys()):
                modes_data = results[msgsize][config]
                epoll = modes_data.get('epoll', {}).get('msg_rate', 0)
                uring = modes_data.get('uring', {}).get('msg_rate', 0)
                multishot = modes_data.get('multishot', {}).get('msg_rate', 0)
                f.write(f'"{config}" {epoll} {uring} {multishot}\n')

    # 2. Throughput by config (for each message size)
    for msgsize in sorted(results.keys()):
        filename = os.path.join(output_dir, f'throughput_{msgsize}.dat')
        data_files[f'throughput_{msgsize}'] = filename

        with open(filename, 'w') as f:
            f.write("# Config Epoll Uring Multishot\n")
            for config in sorted(results[msgsize].keys()):
                modes_data = results[msgsize][config]
                epoll = modes_data.get('epoll', {}).get('throughput_mb', 0)
                uring = modes_data.get('uring', {}).get('throughput_mb', 0)
                multishot = modes_data.get('multishot', {}).get('throughput_mb', 0)
                f.write(f'"{config}" {epoll} {uring} {multishot}\n')

    # 3. Message size scaling (for each config)
    configs = set()
    for msgsize in results.keys():
        configs.update(results[msgsize].keys())

    for config in sorted(configs):
        config_clean = config.replace("×", "x")
        filename = os.path.join(output_dir, f'msgsize_scaling_{config_clean}.dat')
        data_files[f'msgsize_{config}'] = filename

        with open(filename, 'w') as f:
            f.write("# MsgSize Epoll Uring Multishot\n")
            for msgsize in sorted(results.keys()):
                if config in results[msgsize]:
                    modes_data = results[msgsize][config]
                    epoll = modes_data.get('epoll', {}).get('msg_rate', 0)
                    uring = modes_data.get('uring', {}).get('msg_rate', 0)
                    multishot = modes_data.get('multishot', {}).get('msg_rate', 0)
                    if epoll or uring or multishot:  # Only include if we have data
                        f.write(f'{msgsize} {epoll} {uring} {multishot}\n')

    # 4. Speedup vs epoll
    for msgsize in sorted(results.keys()):
        filename = os.path.join(output_dir, f'speedup_{msgsize}.dat')
        data_files[f'speedup_{msgsize}'] = filename

        with open(filename, 'w') as f:
            f.write("# Config Uring_Speedup Multishot_Speedup\n")
            for config in sorted(results[msgsize].keys()):
                modes_data = results[msgsize][config]
                epoll_rate = modes_data.get('epoll', {}).get('msg_rate', 0)
                uring_rate = modes_data.get('uring', {}).get('msg_rate', 0)
                multishot_rate = modes_data.get('multishot', {}).get('msg_rate', 0)

                uring_speedup = uring_rate / epoll_rate if epoll_rate > 0 else 0
                multishot_speedup = multishot_rate / epoll_rate if epoll_rate > 0 else 0

                f.write(f'"{config}" {uring_speedup:.2f} {multishot_speedup:.2f}\n')

    return data_files

def generate_gnuplot_scripts(results, output_dir, data_files):
    """Generate gnuplot scripts."""

    scripts = []

    # 1. Message rate comparison charts
    for msgsize in sorted(results.keys()):
        script_file = os.path.join(output_dir, f'plot_msgrate_{msgsize}.gnu')
        png_file = os.path.join(output_dir, f'msgrate_{msgsize}.png')
        data_file = data_files[f'msgrate_{msgsize}']

        with open(script_file, 'w') as f:
            f.write(f'''set terminal pngcairo size 1400,900 enhanced font 'Arial,12'
set output '{png_file}'
set title 'Message Rate Comparison - {msgsize} byte messages' font 'Arial,18'
set xlabel 'Configuration (threads × connections)' font 'Arial,14'
set ylabel 'Messages per second' font 'Arial,14'
set style data histogram
set style histogram cluster gap 1
set style fill solid 0.8 border -1
set boxwidth 0.9
set xtic rotate by -45 scale 0 font 'Arial,11'
set grid ytics linetype 0 linewidth 1
set key top left font 'Arial,12'
set format y "%.0s%c"
set yrange [0:*]

plot '{data_file}' using 2:xtic(1) title 'epoll' linecolor rgb '#e74c3c', \\
     '' using 3 title 'uring' linecolor rgb '#3498db', \\
     '' using 4 title 'multishot' linecolor rgb '#2ecc71'
''')
        scripts.append((script_file, png_file))

    # 2. Throughput comparison charts
    for msgsize in sorted(results.keys()):
        script_file = os.path.join(output_dir, f'plot_throughput_{msgsize}.gnu')
        png_file = os.path.join(output_dir, f'throughput_{msgsize}.png')
        data_file = data_files[f'throughput_{msgsize}']

        with open(script_file, 'w') as f:
            f.write(f'''set terminal pngcairo size 1400,900 enhanced font 'Arial,12'
set output '{png_file}'
set title 'Throughput Comparison - {msgsize} byte messages' font 'Arial,18'
set xlabel 'Configuration (threads × connections)' font 'Arial,14'
set ylabel 'Throughput (MB/s)' font 'Arial,14'
set style data histogram
set style histogram cluster gap 1
set style fill solid 0.8 border -1
set boxwidth 0.9
set xtic rotate by -45 scale 0 font 'Arial,11'
set grid ytics linetype 0 linewidth 1
set key top left font 'Arial,12'
set yrange [0:*]

plot '{data_file}' using 2:xtic(1) title 'epoll' linecolor rgb '#e74c3c', \\
     '' using 3 title 'uring' linecolor rgb '#3498db', \\
     '' using 4 title 'multishot' linecolor rgb '#2ecc71'
''')
        scripts.append((script_file, png_file))

    # 3. Message size scaling charts
    configs = set()
    for msgsize in results.keys():
        configs.update(results[msgsize].keys())

    for config in sorted(configs):
        config_clean = config.replace("×", "x")
        if f'msgsize_{config}' not in data_files:
            continue

        script_file = os.path.join(output_dir, f'plot_msgsize_scaling_{config_clean}.gnu')
        png_file = os.path.join(output_dir, f'msgsize_scaling_{config_clean}.png')
        data_file = data_files[f'msgsize_{config}']

        with open(script_file, 'w') as f:
            f.write(f'''set terminal pngcairo size 1400,900 enhanced font 'Arial,12'
set output '{png_file}'
set title 'Message Size Scaling - {config}' font 'Arial,18'
set xlabel 'Message Size (bytes)' font 'Arial,14'
set ylabel 'Messages per second' font 'Arial,14'
set logscale x
set grid xtics ytics linetype 0 linewidth 1
set key top right font 'Arial,12'
set style line 1 lc rgb '#e74c3c' lt 1 lw 3 pt 7 ps 1.5
set style line 2 lc rgb '#3498db' lt 1 lw 3 pt 9 ps 1.5
set style line 3 lc rgb '#2ecc71' lt 1 lw 3 pt 11 ps 1.5
set format y "%.0s%c"

plot '{data_file}' using 1:2 with linespoints ls 1 title 'epoll', \\
     '' using 1:3 with linespoints ls 2 title 'uring', \\
     '' using 1:4 with linespoints ls 3 title 'multishot'
''')
        scripts.append((script_file, png_file))

    # 4. Speedup comparison
    for msgsize in sorted(results.keys()):
        script_file = os.path.join(output_dir, f'plot_speedup_{msgsize}.gnu')
        png_file = os.path.join(output_dir, f'speedup_{msgsize}.png')
        data_file = data_files[f'speedup_{msgsize}']

        with open(script_file, 'w') as f:
            f.write(f'''set terminal pngcairo size 1400,900 enhanced font 'Arial,12'
set output '{png_file}'
set title 'Speedup vs epoll Baseline - {msgsize} byte messages' font 'Arial,18'
set xlabel 'Configuration (threads × connections)' font 'Arial,14'
set ylabel 'Speedup (×)' font 'Arial,14'
set style data histogram
set style histogram cluster gap 1
set style fill solid 0.8 border -1
set boxwidth 0.9
set xtic rotate by -45 scale 0 font 'Arial,11'
set grid ytics linetype 0 linewidth 1
set key top left font 'Arial,12'
set yrange [0:*]

# Baseline reference line at 1.0x
set arrow from graph 0, first 1 to graph 1, first 1 nohead lc rgb 'gray' lw 2 dt 2

plot '{data_file}' using 2:xtic(1) title 'uring' linecolor rgb '#3498db', \\
     '' using 3 title 'multishot' linecolor rgb '#2ecc71'
''')
        scripts.append((script_file, png_file))

    return scripts

def run_gnuplot(scripts, verbose=True):
    """Execute gnuplot scripts."""

    generated = []
    failed = []

    for script_file, png_file in scripts:
        try:
            subprocess.run(['gnuplot', script_file],
                         capture_output=True,
                         text=True,
                         check=True)

            if os.path.exists(png_file):
                generated.append(png_file)
                if verbose:
                    size = os.path.getsize(png_file)
                    print(f"  ✓ {os.path.basename(png_file):<40} ({size:>8,} bytes)")
            else:
                failed.append(script_file)
                if verbose:
                    print(f"  ✗ {os.path.basename(png_file):<40} (not created)")

        except subprocess.CalledProcessError as e:
            failed.append(script_file)
            if verbose:
                print(f"  ✗ {os.path.basename(png_file):<40} (gnuplot error)")
                if e.stderr:
                    print(f"     {e.stderr.strip()}")

    return generated, failed

def create_index_html(generated_files, output_dir):
    """Create an HTML index to view all graphs."""

    index_file = os.path.join(output_dir, 'index.html')

    # Organize files by type
    msgrate_files = sorted([f for f in generated_files if 'msgrate_' in f])
    throughput_files = sorted([f for f in generated_files if 'throughput_' in f])
    scaling_files = sorted([f for f in generated_files if 'msgsize_scaling' in f])
    speedup_files = sorted([f for f in generated_files if 'speedup_' in f])

    with open(index_file, 'w') as f:
        f.write('''<!DOCTYPE html>
<html>
<head>
    <title>io_uring Benchmark Results</title>
    <style>
        body {
            font-family: Arial, sans-serif;
            margin: 40px;
            background: #f5f5f5;
        }
        h1 { color: #333; }
        h2 {
            color: #555;
            margin-top: 40px;
            border-bottom: 2px solid #3498db;
            padding-bottom: 10px;
        }
        .graph {
            margin: 20px 0;
            background: white;
            padding: 20px;
            border-radius: 8px;
            box-shadow: 0 2px 4px rgba(0,0,0,0.1);
        }
        .graph img {
            max-width: 100%;
            height: auto;
            border: 1px solid #ddd;
        }
        .graph h3 {
            margin-top: 0;
            color: #2c3e50;
        }
        .toc {
            background: white;
            padding: 20px;
            border-radius: 8px;
            margin-bottom: 30px;
            box-shadow: 0 2px 4px rgba(0,0,0,0.1);
        }
        .toc a {
            display: block;
            padding: 5px 0;
            color: #3498db;
            text-decoration: none;
        }
        .toc a:hover {
            text-decoration: underline;
        }
    </style>
</head>
<body>
    <h1>io_uring Benchmark Results</h1>

    <div class="toc">
        <h3>Table of Contents</h3>
        <a href="#msgrate">Message Rate Comparisons</a>
        <a href="#throughput">Throughput Comparisons</a>
        <a href="#scaling">Message Size Scaling</a>
        <a href="#speedup">Speedup Analysis</a>
    </div>
''')

        if msgrate_files:
            f.write('    <h2 id="msgrate">Message Rate Comparisons</h2>\n')
            for filepath in msgrate_files:
                basename = os.path.basename(filepath)
                msgsize = basename.replace('msgrate_', '').replace('.png', '')
                f.write(f'    <div class="graph">\n')
                f.write(f'        <h3>{msgsize} byte messages</h3>\n')
                f.write(f'        <img src="{basename}" alt="{basename}">\n')
                f.write(f'    </div>\n')

        if throughput_files:
            f.write('    <h2 id="throughput">Throughput Comparisons</h2>\n')
            for filepath in throughput_files:
                basename = os.path.basename(filepath)
                msgsize = basename.replace('throughput_', '').replace('.png', '')
                f.write(f'    <div class="graph">\n')
                f.write(f'        <h3>{msgsize} byte messages</h3>\n')
                f.write(f'        <img src="{basename}" alt="{basename}">\n')
                f.write(f'    </div>\n')

        if scaling_files:
            f.write('    <h2 id="scaling">Message Size Scaling</h2>\n')
            for filepath in scaling_files:
                basename = os.path.basename(filepath)
                config = basename.replace('msgsize_scaling_', '').replace('.png', '')
                f.write(f'    <div class="graph">\n')
                f.write(f'        <h3>Configuration: {config}</h3>\n')
                f.write(f'        <img src="{basename}" alt="{basename}">\n')
                f.write(f'    </div>\n')

        if speedup_files:
            f.write('    <h2 id="speedup">Speedup Analysis (vs epoll)</h2>\n')
            for filepath in speedup_files:
                basename = os.path.basename(filepath)
                msgsize = basename.replace('speedup_', '').replace('.png', '')
                f.write(f'    <div class="graph">\n')
                f.write(f'        <h3>{msgsize} byte messages</h3>\n')
                f.write(f'        <img src="{basename}" alt="{basename}">\n')
                f.write(f'    </div>\n')

        f.write('''</body>
</html>
''')

    return index_file

def main():
    parser = argparse.ArgumentParser(
        description='Generate gnuplot graphs from io_uring benchmark results',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog='''
Examples:
  %(prog)s results/
  %(prog)s results/ -o my_graphs/
  %(prog)s results/ --no-html
        '''
    )
    parser.add_argument('results_dir',
                       help='Directory containing benchmark result files')
    parser.add_argument('-o', '--output',
                       default='graphs',
                       help='Output directory for graphs (default: graphs/)')
    parser.add_argument('--no-html',
                       action='store_true',
                       help='Skip HTML index generation')
    parser.add_argument('-q', '--quiet',
                       action='store_true',
                       help='Quiet mode (less output)')

    args = parser.parse_args()

    # Check if results directory exists
    if not os.path.isdir(args.results_dir):
        print(f"Error: '{args.results_dir}' is not a directory", file=sys.stderr)
        return 1

    # Check gnuplot
    has_gnuplot, version = check_gnuplot()
    if not has_gnuplot:
        print("Error: gnuplot is not installed or not in PATH", file=sys.stderr)
        print("\nInstall gnuplot:", file=sys.stderr)
        print("  Debian/Ubuntu: sudo apt install gnuplot", file=sys.stderr)
        print("  macOS:         brew install gnuplot", file=sys.stderr)
        print("  Fedora/RHEL:   sudo dnf install gnuplot", file=sys.stderr)
        return 1

    if not args.quiet:
        print(f"Using gnuplot: {version}")
        print()

    # Analyze results
    results = analyze_results(args.results_dir)
    if not results:
        print(f"Error: No valid benchmark results found in '{args.results_dir}'", file=sys.stderr)
        return 1

    if not args.quiet:
        total_tests = sum(len(configs) for configs in results.values())
        print(f"Found {len(results)} message sizes, {total_tests} configurations")
        print(f"Generating graphs in '{args.output}/'")
        print()

    # Generate data files
    data_files = generate_data_files(results, args.output)
    if not args.quiet:
        print(f"Created {len(data_files)} data files")

    # Generate scripts
    scripts = generate_gnuplot_scripts(results, args.output, data_files)
    if not args.quiet:
        print(f"Created {len(scripts)} gnuplot scripts")
        print()
        print("Generating graphs:")

    # Run gnuplot
    generated, failed = run_gnuplot(scripts, verbose=not args.quiet)

    if not args.quiet:
        print()
        print(f"Successfully generated {len(generated)} graphs")
        if failed:
            print(f"Failed to generate {len(failed)} graphs")

    # Create HTML index
    if not args.no_html and generated:
        index_file = create_index_html(generated, args.output)
        if not args.quiet:
            print()
            print(f"Created HTML index: {index_file}")
            print(f"View in browser:    file://{os.path.abspath(index_file)}")

    if not args.quiet:
        print()
        print("=" * 80)
        print("Done!")

    return 0 if not failed else 1

if __name__ == '__main__':
    sys.exit(main())
