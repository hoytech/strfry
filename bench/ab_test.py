#!/usr/bin/env python3
import sys
import os
import subprocess
import shutil
import time
import json
import re
import argparse
from datetime import datetime

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
STRFRY_DIR = os.path.dirname(SCRIPT_DIR)

def parse_markdown_metrics(filepath):
    metrics = {}
    if not os.path.exists(filepath):
        return metrics
    with open(filepath, "r") as f:
        for line in f:
            m = re.match(r'-\s+\*\*(.*?)(?::\*\*|\*\*[:]*)\s*(.*)', line.strip())
            if m:
                name = m.group(1).strip()
                val_str = m.group(2).strip()
                num_m = re.search(r'[-+]?\d*\.\d+|\d+', val_str)
                if num_m:
                    metrics[name] = float(num_m.group(0))
    return metrics

def run_deterministic_benchmark(out_json_path):
    print(f"[INFO] Running deterministic benchmark to {out_json_path}...")
    
    # 1. Compile allocator tracker
    tracker_src = os.path.join(STRFRY_DIR, "bench", "alloc_tracker.c")
    tracker_so = os.path.join(STRFRY_DIR, "bench", "alloc_tracker.so")
    print(f"[INFO] Compiling allocation tracker...")
    subprocess.run(["gcc", "-shared", "-fPIC", "-o", tracker_so, tracker_src, "-ldl"], check=True)
    
    # 2. Generate test seed data (always deterministic)
    seed_data_path = os.path.join(STRFRY_DIR, "bench", "test_seed.jsonl")
    if not os.path.exists(seed_data_path):
        print(f"[INFO] Generating deterministic test seed data...")
        subprocess.run([
            "perl", "test/generate-seed-data.pl",
            "--seed", "1337",
            "--output", seed_data_path,
            "--users", "10",
            "--kind1-notes", "100",
            "--kind0-profiles", "10",
            "--kind3-contacts", "5",
            "--kind4-dms", "10",
            "--kind7-reactions", "10",
            "--replaceable", "10",
            "--param-replaceable", "10",
            "--ephemeral", "10",
            "--deletions", "10",
            "--other", "10",
            "--duplicates", "5"
        ], check=True)

    # 3. Clean db
    db_det = os.path.join(STRFRY_DIR, "strfry-db-det")
    shutil.rmtree(db_det, ignore_errors=True)
    os.makedirs(db_det, exist_ok=True)
    
    # 4. Setup files for output
    allocs_file = os.path.join(STRFRY_DIR, "bench", "allocs.txt")
    perf_file = os.path.join(STRFRY_DIR, "bench", "perf.txt")
    if os.path.exists(allocs_file):
        os.remove(allocs_file)
    if os.path.exists(perf_file):
        os.remove(perf_file)
        
    # 5. Run perf stat & alloc tracker
    env = os.environ.copy()
    env["LD_PRELOAD"] = tracker_so
    env["ALLOC_TRACKER_OUT"] = allocs_file
    
    binary = os.path.join(STRFRY_DIR, "strfry")
    perf_cmd = [
        "perf", "stat", "-x,", "-o", perf_file,
        "-e", "cpu_atom/instructions/u,cpu_core/instructions/u",
        binary, "--set", "db=strfry-db-det/", "import", "--no-verify"
    ]
    
    with open(seed_data_path, "r") as stdin_f:
        subprocess.run(perf_cmd, stdin=stdin_f, env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        
    # 6. Parse allocations
    total_allocs = 0
    total_bytes = 0
    if os.path.exists(allocs_file):
        with open(allocs_file, "r") as f:
            for line in f:
                if line.startswith("ALLOCS:"):
                    total_allocs += int(line.split()[1])
                elif line.startswith("BYTES:"):
                    total_bytes += int(line.split()[1])
                    
    # 7. Parse instructions
    total_instructions = 0
    if os.path.exists(perf_file):
        with open(perf_file, "r") as f:
            for line in f:
                parts = line.strip().split(",")
                if len(parts) >= 3 and "instructions" in parts[2]:
                    val = parts[0]
                    if val != "<not counted>" and val != "":
                        try:
                            total_instructions += int(val)
                        except ValueError:
                            pass
                            
    shutil.rmtree(db_det, ignore_errors=True)
    
    # Save results
    results = {
        "instructions": total_instructions,
        "allocations": total_allocs,
        "allocated_bytes": total_bytes
    }
    with open(out_json_path, "w") as f:
        json.dump(results, f, indent=2)
        
    print(f"[INFO] Deterministic metrics: {results}")
    return results

def main():
    parser = argparse.ArgumentParser(description="Strfry Local Relative A/B Benchmarking Script")
    parser.add_argument("--base", type=str, default="master", help="Base branch/commit to compare against (default: master)")
    parser.add_argument("--skip-heavy", action="store_true", help="Skip 1M event heavy database benchmark")
    parser.add_argument("--full", action="store_true", help="Run all benchmark suites including 1M event storage test (recommended before opening a PR)")
    args = parser.parse_args()

    base_branch = args.base
    
    # 1. Save original branch & check status
    res = subprocess.run(["git", "rev-parse", "--abbrev-ref", "HEAD"], capture_output=True, text=True, check=True)
    current_branch = res.stdout.strip()
    
    res = subprocess.run(["git", "status", "--porcelain"], capture_output=True, text=True, check=True)
    has_changes = len(res.stdout.strip()) > 0
    
    stashed = False
    
    print(f"[INFO] Current branch is {current_branch}")
    print(f"[INFO] Comparing against base branch {base_branch}")
    
    temp_run_stats_py = os.path.join(STRFRY_DIR, "bench", "run_stats_temp.py")
    temp_alloc_tracker_c = os.path.join(STRFRY_DIR, "bench", "alloc_tracker_temp.c")
    temp_bench_plugin_py = os.path.join(STRFRY_DIR, "bench", "bench_plugin_temp.py")
    
    alloc_tracker_c = os.path.join(STRFRY_DIR, "bench", "alloc_tracker.c")
    bench_plugin_py = os.path.join(STRFRY_DIR, "bench", "bench_plugin.py")
    try:
        # Copy current run_stats.py to a temp path so we run the same runner on both branches
        run_stats_py = os.path.join(STRFRY_DIR, "bench", "run_stats.py")
        shutil.copy(run_stats_py, temp_run_stats_py)
        shutil.copy(alloc_tracker_c, temp_alloc_tracker_c)
        shutil.copy(bench_plugin_py, temp_bench_plugin_py)
        # 2. Run PR branch (new changes)
        print(f"\n=== BENCHMARKING NEW CHANGES (Branch: {current_branch}) ===")
        # Re-build just in case
        print("[INFO] Rebuilding strfry on current branch...")
        subprocess.run(["make", "-j4"], check=True)
        
        # Run stats
        report_new = os.path.join(STRFRY_DIR, "benchmark_report_new.md")
        skip_flag = [] if args.full else (["--skip-heavy"] if args.skip_heavy else [])
        
        subprocess.run(["python3", temp_run_stats_py] + skip_flag, check=True)
        if os.path.exists("benchmark_report.md"):
            shutil.copy("benchmark_report.md", report_new)
            
        det_new = os.path.join(STRFRY_DIR, "bench", "det_new.json")
        run_deterministic_benchmark(det_new)
        
        # Stash changes if any
        if has_changes:
            print("[INFO] Stashing uncommitted changes...")
            subprocess.run(["git", "stash", "push", "-m", "ab_test_auto_stash"], check=True)
            stashed = True
            
        # 3. Checkout base branch
        print(f"\n=== CHECKING OUT BASE BRANCH ({base_branch}) ===")
        subprocess.run(["git", "checkout", base_branch], check=True)
        
        # Restore needed files if they were deleted by checkout
        if not os.path.exists(alloc_tracker_c):
            shutil.copy(temp_alloc_tracker_c, alloc_tracker_c)
        if not os.path.exists(bench_plugin_py):
            shutil.copy(temp_bench_plugin_py, bench_plugin_py)
        print("[INFO] Building strfry on base branch...")
        subprocess.run(["make", "-j4"], check=True)
        
        report_old = os.path.join(STRFRY_DIR, "benchmark_report_old.md")
        subprocess.run(["python3", temp_run_stats_py] + skip_flag, check=True)
        if os.path.exists("benchmark_report.md"):
            shutil.copy("benchmark_report.md", report_old)
            
        det_old = os.path.join(STRFRY_DIR, "bench", "det_old.json")
        run_deterministic_benchmark(det_old)
        
    finally:
        # Clean up temp files
        for temp_file in [temp_run_stats_py, temp_alloc_tracker_c, temp_bench_plugin_py]:
            if os.path.exists(temp_file):
                try:
                    os.remove(temp_file)
                except:
                    pass
        # Remove restored files on base branch so they don't block checkout
        for restored_file in [alloc_tracker_c, bench_plugin_py]:
            if os.path.exists(restored_file):
                try:
                    os.remove(restored_file)
                except:
                    pass
        # 4. Checkout back to current branch
        print(f"\n=== RESTORING ORIGINAL STATE (Branch: {current_branch}) ===")
        subprocess.run(["git", "checkout", current_branch], stderr=subprocess.DEVNULL)
        print("[INFO] Rebuilding strfry...")
        subprocess.run(["make", "-j4"], stderr=subprocess.DEVNULL)
        
        if stashed:
            print("[INFO] Restoring stashed changes...")
            subprocess.run(["git", "stash", "pop"], stderr=subprocess.DEVNULL)
            
    # 5. Parse and compare metrics
    new_metrics = parse_markdown_metrics(os.path.join(STRFRY_DIR, "benchmark_report_new.md"))
    old_metrics = parse_markdown_metrics(os.path.join(STRFRY_DIR, "benchmark_report_old.md"))
    
    det_new_data = {}
    det_old_data = {}
    det_new_path = os.path.join(STRFRY_DIR, "bench", "det_new.json")
    det_old_path = os.path.join(STRFRY_DIR, "bench", "det_old.json")
    if os.path.exists(det_new_path):
        with open(det_new_path, "r") as f:
            det_new_data = json.load(f)
    if os.path.exists(det_old_path):
        with open(det_old_path, "r") as f:
            det_old_data = json.load(f)
            
    # Combine metrics
    all_keys = sorted(list(set(new_metrics.keys()) | set(old_metrics.keys())))
    
    comparison_path = os.path.join(STRFRY_DIR, "benchmark_comparison.md")
    print(f"\n[INFO] Generating comparison report at {comparison_path}")
    
    with open(comparison_path, "w") as f:
        f.write("# Strfry A/B Benchmark Comparison Report\n\n")
        f.write(f"Generated at: {datetime.now().isoformat()}\n")
        f.write(f"- **Base Branch:** {base_branch}\n")
        f.write(f"- **PR/Current Branch:** {current_branch}\n\n")
        
        f.write("## 1. Deterministic Metrics (Hardware Agnostic)\n")
        f.write("| Metric | Base | PR | Delta (%) | Status |\n")
        f.write("| :--- | :--- | :--- | :--- | :--- |\n")
        
        det_metrics = [
            ("instructions", "Instructions Retired"),
            ("allocations", "Heap Allocations Count"),
            ("allocated_bytes", "Total Bytes Allocated")
        ]
        
        for key, label in det_metrics:
            base_val = det_old_data.get(key, 0)
            pr_val = det_new_data.get(key, 0)
            if base_val > 0:
                delta = ((pr_val - base_val) / base_val) * 100
                delta_str = f"{delta:+.2f}%"
                status = "🟢 Improved" if pr_val < base_val else ("🔴 Regressed" if pr_val > base_val else "⚪ No Change")
            else:
                delta_str = "N/A"
                status = "Unknown"
            
            f.write(f"| {label} | {base_val:,} | {pr_val:,} | {delta_str} | {status} |\n")
            
        f.write("\n")
        
        f.write("## 2. Standard Benchmark Suites (Wall-clock & Resource Metrics)\n")
        f.write("| Suite Metric | Base | PR | Delta (%) | Status |\n")
        f.write("| :--- | :--- | :--- | :--- | :--- |\n")
        
        for key in all_keys:
            if key.lower() in ["status", "status:"]:
                continue
            base_val = old_metrics.get(key, -1)
            pr_val = new_metrics.get(key, -1)
            
            if base_val >= 0 and pr_val >= 0:
                if base_val == 0:
                    if pr_val == 0:
                        delta = 0.0
                        delta_str = "0.00%"
                    else:
                        delta = 100.0
                        delta_str = "N/A"
                else:
                    delta = ((pr_val - base_val) / base_val) * 100
                    delta_str = f"{delta:+.2f}%"
                
                # Check if lower is better or higher is better
                lower_better = any(x in key.lower() for x in ["time", "ms", "rss", "bytes", "amplification", "depth", "memory", "sockets"])
                
                if abs(delta) < 1.0:
                    status = "⚪ Stable"
                elif pr_val < base_val:
                    status = "🟢 Faster/Lighter" if lower_better else "🔴 Slower/Lower"
                else:
                    status = "🔴 Slower/Heavier" if lower_better else "🟢 Faster/Higher"
            else:
                delta_str = "N/A"
                status = "Unknown"
                
            f.write(f"| {key} | {base_val:.2f} | {pr_val:.2f} | {delta_str} | {status} |\n")
            
    print(f"[INFO] Comparison complete. Comparison saved to {comparison_path}.")

if __name__ == "__main__":
    main()
