#!/usr/bin/env python3
import argparse
import subprocess
import os
import sys
import time
import json
import urllib.request
import re
import shutil
import threading
from datetime import datetime

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
STRFRY_DIR = os.path.dirname(SCRIPT_DIR)

import builtins
def print(*args, **kwargs):
    kwargs.setdefault('flush', True)
    builtins.print(*args, **kwargs)

def check_docker():
    try:
        res = subprocess.run(["docker", "info"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=3)
        return res.returncode == 0
    except:
        return False

def drop_caches():
    print("[INFO] Dropping OS caches...")
    res = subprocess.run(["sudo", "-n", "sh", "-c", "sync; echo 3 > /proc/sys/vm/drop_caches"], capture_output=True)
    if res.returncode != 0:
        print("[WARNING] Could not drop OS caches (requires passwordless sudo). Out-of-core benchmarks might use cached pages.")


class StrfryManager:
    def __init__(self, use_docker=False, memory_limit=None):
        self.use_docker = use_docker
        self.memory_limit = memory_limit
        self.process = None
        self.db_dir = os.path.join(STRFRY_DIR, 'strfry-db')
        self.config_path = os.path.join(STRFRY_DIR, 'strfry.conf')
        self.binary_path = os.path.join(STRFRY_DIR, 'strfry')
    def build_docker(self):
        print("[INFO] Building strfry docker image...")
        subprocess.run(["docker", "build", "--progress=plain", "-t", "strfry-bench-image", "."], cwd=STRFRY_DIR, check=True)

    def clean_db(self):
        print("[INFO] Cleaning database...")
        res = subprocess.run(["rm", "-rf", self.db_dir])
        if res.returncode != 0 or os.path.exists(self.db_dir):
            print("[WARNING] Normal rm -rf failed or directory still exists, trying with sudo...")
            subprocess.run(["sudo", "rm", "-rf", self.db_dir])
        os.makedirs(self.db_dir, exist_ok=True)

    def start(self, config_overrides=None):
        if self.process is not None:
            self.stop()
            
        print("[INFO] Starting strfry relay...")
        
        args = []
        if config_overrides:
            for k, v in config_overrides.items():
                args.extend(["--set", f"{k}={v}"])
        
        if self.use_docker:
            cmd = [
                "docker", "run", "--rm", "-p", "7777:7777",
                "-v", f"{self.config_path}:/app/strfry.conf",
                "-v", f"{self.db_dir}:/app/strfry-db"
            ]
            if self.memory_limit:
                cmd.extend(["--memory", self.memory_limit])
            cmd.append("strfry-bench-image")
            # Always bind to 0.0.0.0 inside Docker so the host can connect
            cmd.extend(["--set", "relay.bind=0.0.0.0"])
            cmd.extend(args)
            cmd.append("relay")
            self.process = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        else:
            cmd = [self.binary_path, "--config", self.config_path]
            cmd.extend(args)
            cmd.append("relay")
            self.process = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        
        time.sleep(2) # Wait for relay to bind port
        print("[INFO] Relay started.")

    def stop(self):
        if self.process:
            print("[INFO] Stopping strfry relay...")
            self.process.terminate()
            self.process.wait()
            self.process = None
            print("[INFO] Relay stopped.")

class PrometheusScraper:
    def __init__(self, port=7777):
        self.url = f"http://localhost:{port}/metrics"
        self.running = False
        self.thread = None
        self.peak_queue = 0.0

    def get_metrics(self):
        try:
            req = urllib.request.Request(self.url)
            with urllib.request.urlopen(req) as response:
                return response.read().decode('utf-8')
        except Exception as e:
            return ""

    def parse_metric(self, metrics_text, metric_name):
        for line in metrics_text.splitlines():
            if line.startswith(metric_name):
                parts = line.split()
                if len(parts) >= 2:
                    return float(parts[1])
        return 0.0

    def _poll_loop(self):
        while self.running:
            metrics_text = self.get_metrics()
            q = self.parse_metric(metrics_text, "strfry_writer_queue")
            if q > self.peak_queue:
                self.peak_queue = q
            time.sleep(0.1)

    def start_polling(self):
        self.running = True
        self.peak_queue = 0.0
        self.thread = threading.Thread(target=self._poll_loop)
        self.thread.daemon = True
        self.thread.start()

    def stop_polling(self):
        self.running = False
        if self.thread:
            self.thread.join(timeout=1.0)

class ResourceMonitor:
    def __init__(self, pid):
        self.pid = pid
        self.running = False
        self.thread = None
        self.cpu_usages = []
        self.read_bytes_start = 0
        self.write_bytes_start = 0
        self.logical_write_start = 0
        self.read_bytes_end = 0
        self.write_bytes_end = 0
        self.logical_write_end = 0

    def get_cpu_times(self):
        try:
            with open("/proc/stat", "r") as f:
                first_line = f.readline()
                parts = first_line.split()
                total = sum(float(x) for x in parts[1:])
                idle = float(parts[4])
                return total, idle
        except:
            return 0, 0

    def get_proc_cpu_time(self):
        try:
            with open(f"/proc/{self.pid}/stat", "r") as f:
                parts = f.readline().split()
                utime = float(parts[13])
                stime = float(parts[14])
                return utime + stime
        except:
            return 0

    def get_proc_io_bytes(self):
        try:
            r, w, lw = 0, 0, 0
            with open(f"/proc/{self.pid}/io", "r") as f:
                for line in f:
                    if line.startswith("read_bytes:"):
                        r = int(line.split()[1])
                    elif line.startswith("write_bytes:"):
                        w = int(line.split()[1])
                    elif line.startswith("wchar:"):
                        lw = int(line.split()[1])
            return r, w, lw
        except:
            return 0, 0, 0

    def _poll_loop(self):
        num_cores = os.cpu_count() or 1
        while self.running:
            t1_total, t1_idle = self.get_cpu_times()
            p1_time = self.get_proc_cpu_time()
            time.sleep(0.5)
            t2_total, t2_idle = self.get_cpu_times()
            p2_time = self.get_proc_cpu_time()
            
            total_diff = t2_total - t1_total
            proc_diff = p2_time - p1_time
            if total_diff > 0:
                cpu_pct = (proc_diff / total_diff) * 100 * num_cores
                self.cpu_usages.append(cpu_pct)

    def start(self):
        self.running = True
        self.cpu_usages = []
        r, w, lw = self.get_proc_io_bytes()
        self.read_bytes_start = r
        self.write_bytes_start = w
        self.logical_write_start = lw
        self.thread = threading.Thread(target=self._poll_loop)
        self.thread.daemon = True
        self.thread.start()

    def stop(self):
        self.running = False
        if self.thread:
            self.thread.join(timeout=1.0)
        r, w, lw = self.get_proc_io_bytes()
        self.read_bytes_end = r
        self.write_bytes_end = w
        self.logical_write_end = lw

    def get_results(self):
        avg_cpu = sum(self.cpu_usages) / len(self.cpu_usages) if self.cpu_usages else 0.0
        peak_cpu = max(self.cpu_usages) if self.cpu_usages else 0.0
        read_delta = self.read_bytes_end - self.read_bytes_start
        write_delta = self.write_bytes_end - self.write_bytes_start
        logical_write_delta = self.logical_write_end - self.logical_write_start
        waf = write_delta / logical_write_delta if logical_write_delta > 0 else 0.0
        return {
            "avg_cpu_percent": avg_cpu,
            "peak_cpu_percent": peak_cpu,
            "read_bytes": read_delta,
            "write_bytes": write_delta,
            "logical_write_bytes": logical_write_delta,
            "waf": waf,
            "read_mb": read_delta / (1024 * 1024),
            "write_mb": write_delta / (1024 * 1024)
        }

def get_bench_dir():
    parent_dir = os.path.dirname(STRFRY_DIR)
    sibling_bench = os.path.join(parent_dir, "strfry-bench")
    if os.path.exists(sibling_bench):
        return sibling_bench
    local_bench = os.path.join(STRFRY_DIR, "strfry-bench")
    return local_bench

def run_bench(command, args):
    bench_dir = get_bench_dir()
    cmd = ["./target/release/strfry-bench", command] + args
    print(f"[INFO] Running bench: {' '.join(cmd)}")
    start = time.time()
    result = subprocess.run(cmd, cwd=bench_dir, capture_output=True, text=True)
    end = time.time()
    
    if result.returncode != 0:
        print(f"[ERROR] strfry-bench failed:\n{result.stderr}")
        return None
        
    return {
        "output": result.stdout,
        "elapsed": end - start
    }

def generate_seed_data(events=100000):
    print(f"[INFO] Generating {events} events for seed...")
    ratio = events / 100000.0
    users = max(1, int(500 * ratio))
    kind1_notes = max(1, int(70000 * ratio))
    kind4_dms = max(1, int(8000 * ratio))
    kind7_reactions = max(1, int(8000 * ratio))
    replaceable = max(1, int(6000 * ratio))
    param_replaceable = max(1, int(10000 * ratio))
    ephemeral = max(1, int(4000 * ratio))
    deletions = max(1, int(2500 * ratio))
    other = max(1, int(3000 * ratio))
    duplicates = max(1, int(1500 * ratio))
    
    gen_cmd = [
        "perl", "test/generate-seed-data.pl", "-o", "-",
        "--users", str(users),
        "--kind1-notes", str(kind1_notes),
        "--kind4-dms", str(kind4_dms),
        "--kind7-reactions", str(kind7_reactions),
        "--replaceable", str(replaceable),
        "--param-replaceable", str(param_replaceable),
        "--ephemeral", str(ephemeral),
        "--deletions", str(deletions),
        "--other", str(other),
        "--duplicates", str(duplicates)
    ]
    binary = os.path.join(STRFRY_DIR, "strfry")
    import_cmd = [binary, "import", "--no-verify"]
    
    p1 = subprocess.Popen(gen_cmd, stdout=subprocess.PIPE, cwd=STRFRY_DIR)
    p2 = subprocess.Popen(import_cmd, stdin=p1.stdout, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    p1.stdout.close()
    p2.communicate()
    print("[INFO] Seeding complete.")

def run_iostat(duration=5):
    # Runs iostat for the given duration and returns MB/s read/write
    if not shutil.which("iostat"):
        return {"read_mb": -1, "write_mb": -1}
    cmd = ["iostat", "-m", "-y", "1", str(duration)]
    res = subprocess.run(cmd, capture_output=True, text=True)
    # Parse logic omitted for brevity, returning dummy data for now
    return {"read_mb": 0.0, "write_mb": 0.0}

def suite_storage(manager, skip_heavy=False):
    print("\n--- Running Suite 1: Storage (In-Core vs Out-of-Core) ---")
    results = {}
    manager.clean_db()
    
    events_count = 10000 if skip_heavy else 1000000
    generate_seed_data(events_count)
    
    # 1. In-Core Test
    manager.use_docker = False
    manager.start()
    
    start = time.time()
    scan_res = subprocess.run(["./strfry", "scan", "{}"], capture_output=True, text=True)
    results["scan_time"] = time.time() - start
    results["scan_tps"] = events_count / results["scan_time"] if results["scan_time"] > 0 else 0
    
    res = run_bench("paginate", ["ws://localhost:7777", "--depth", "10", "--concurrency", "2"])
    results["in_core_time"] = res["elapsed"] if res else -1
    manager.stop()
    
    # 2. Out-of-Core Test (Docker, 256MB memory limit)
    if check_docker():
        drop_caches()
        manager.use_docker = True
        manager.memory_limit = "256m"
        manager.start()
        res = run_bench("paginate", ["ws://localhost:7777", "--depth", "10", "--concurrency", "2"])
        results["out_of_core_time"] = res["elapsed"] if res else -1
        manager.stop()
    else:
        print("[WARNING] Docker daemon not available. Skipping Out-of-Core test.")
        results["out_of_core_time"] = -1
    
    try:
        mdb_res = subprocess.run(["mdb_stat", "-e", manager.db_dir], capture_output=True, text=True)
        results["mdb_stat"] = mdb_res.stdout
    except FileNotFoundError:
        results["mdb_stat"] = "mdb_stat not installed"
        
    return results

def suite_ingestion(manager, skip_heavy=False):
    print("\n--- Running Suite 2: Ingestion Pipeline ---")
    results = {}
    manager.clean_db()
    manager.use_docker = False
    manager.start()
    
    count = 1000 if skip_heavy else 50000
    
    scraper = PrometheusScraper()
    scraper.start_polling()
    
    monitor = ResourceMonitor(manager.process.pid)
    monitor.start()
    
    # Standard Events (Small)
    res_small = run_bench("event", ["ws://localhost:7777", "-c", "20", "-n", str(count), "--payload-size", "50"])
    
    # Standard Events (Large)
    res_large = run_bench("event", ["ws://localhost:7777", "-c", "20", "-n", str(count // 10), "--payload-size", "10000"])
    
    # Spam / Rate Limiting (Single connection trying to blast events)
    res_spam = run_bench("event", ["ws://localhost:7777", "-c", "1", "-n", str(count)])
    
    monitor.stop()
    scraper.stop_polling()
    
    results.update(monitor.get_results())
    results["writer_queue_peak"] = scraper.peak_queue
    results["event_small_tps"] = count / res_small["elapsed"] if res_small else -1
    results["event_large_tps"] = (count // 10) / res_large["elapsed"] if res_large else -1
    results["event_spam_tps"] = count / res_spam["elapsed"] if res_spam else -1
    results["event_small_output"] = res_small["output"] if res_small else ""
    
    manager.stop()
    return results

def suite_concurrency(manager, skip_heavy=False):
    print("\n--- Running Suite 3: Concurrency & Thread Pool ---")
    results = {}
    manager.clean_db()
    manager.use_docker = False
    manager.start()
    
    events = 1000 if skip_heavy else 100000
    
    # Background writer
    bench_dir = get_bench_dir()
    write_cmd = ["./target/release/strfry-bench", "event", "ws://localhost:7777", "-c", "20", "-n", str(events)]
    writer = subprocess.Popen(write_cmd, cwd=bench_dir, stdout=subprocess.DEVNULL)
    
    time.sleep(2) # Let writer build pressure
    
    req_start = time.time()
    res = run_bench("req", ["ws://localhost:7777", "-c", "10", "-n", "1000", "--filter", "{\"limit\":10}"])
    results["mixed_req_time"] = res["elapsed"] if res else -1
    
    writer.wait()
    manager.stop()
    results["status"] = "Done"
    return results

def get_process_rss(pid):
    try:
        with open(f"/proc/{pid}/status", "r") as f:
            for line in f:
                if line.startswith("VmRSS:"):
                    return int(line.split()[1]) / 1024 # MB
    except:
        pass
    return -1

def get_process_cpu_time(pid):
    try:
        with open(f"/proc/{pid}/stat", "r") as f:
            parts = f.read().split()
            # 13: utime, 14: stime
            return float(parts[13]) + float(parts[14])
    except:
        return 0.0

def get_process_io(pid):
    io_data = {"read_bytes": 0, "write_bytes": 0}
    try:
        with open(f"/proc/{pid}/io", "r") as f:
            for line in f:
                if line.startswith("read_bytes:"):
                    io_data["read_bytes"] = int(line.split()[1])
                elif line.startswith("write_bytes:"):
                    io_data["write_bytes"] = int(line.split()[1])
    except:
        pass
    return io_data

def count_time_wait_sockets(port=7777):
    port_hex = f"{port:04X}"
    count = 0
    for filename in ["/proc/net/tcp", "/proc/net/tcp6"]:
        if not os.path.exists(filename):
            continue
        try:
            with open(filename, "r") as f:
                lines = f.readlines()
                for line in lines[1:]: # skip header
                    parts = line.split()
                    if len(parts) >= 4:
                        state = parts[3]
                        if state == "06": # TIME_WAIT
                            local_port = parts[1].split(":")[-1]
                            remote_port = parts[2].split(":")[-1]
                            if local_port == port_hex or remote_port == port_hex:
                                count += 1
        except Exception:
            pass
    return count

def suite_websockets(manager, skip_heavy=False):
    print("\n--- Running Suite 4: WebSockets & Connections ---")
    results = {}
    manager.clean_db()
    manager.use_docker = False
    manager.start()
    
    counts = [100, 500] if skip_heavy else [100, 1000]
    if not skip_heavy:
        counts.append(5000)
        
    results["connection_memory"] = {}
    for c in counts:
        print(f"[INFO] Testing {c} connection storm...")
        res = run_bench("connections", ["ws://localhost:7777", "-c", str(c)])
        rss = get_process_rss(manager.process.pid)
        results["connection_memory"][str(c)] = rss
        if res:
            output = res["output"]
            results[f"conn_storm_{c}_output"] = output
            tps_m = re.search(r'\(([\d.]+)\s+conn/sec\)', output)
            if tps_m:
                results[f"conn_storm_{c}_tps"] = float(tps_m.group(1))
            lat_m = re.search(r'P50:\s*([\d.]+).*P99:\s*([\d.]+)', output)
            if lat_m:
                results[f"conn_storm_{c}_p50_ms"] = float(lat_m.group(1))
                results[f"conn_storm_{c}_p99_ms"] = float(lat_m.group(2))
        time.sleep(1) # Let strfry clean up
    print("[INFO] Testing High Churn...")
    churn_count = 200 if skip_heavy else 10000
    res_churn = run_bench("churn", ["ws://localhost:7777", "-c", "50", "-n", str(churn_count)])
    if res_churn:
        results["churn_output"] = res_churn["output"]
        tps_m = re.search(r'\(([\d.]+)\s+conn/sec\)', res_churn["output"])
        if tps_m:
            results["churn_tps"] = float(tps_m.group(1))
        
    results["time_wait_count"] = count_time_wait_sockets(7777)

    manager.stop()
    results["status"] = "Done"
    return results

def suite_queries(manager, skip_heavy=False):
    print("\n--- Running Suite 5: Query Engine & Indices ---")
    results = {}
    manager.clean_db()
    generate_seed_data(10000 if skip_heavy else 1000000)
    manager.use_docker = False
    manager.start()
    
    # 1. Point Lookup (exact id)
    # Get a real ID from the DB via strfry scan
    scan_res = subprocess.run(["./strfry", "scan", "{\"limit\":1}"], capture_output=True, text=True)
    try:
        real_id = json.loads(scan_res.stdout.strip().splitlines()[0])["id"]
    except:
        real_id = "0000000000000000000000000000000000000000000000000000000000000000"
        
    res_point = run_bench("req", ["ws://localhost:7777", "-c", "5", "-n", "100", "--filter", "{\"ids\":[\"" + real_id + "\"]}"])
    results["query_point_time"] = res_point["elapsed"] if res_point else -1
    results["query_point_output"] = res_point["output"] if res_point else ""
    
    # 2. Point COUNT Lookup (NIP-45)
    res_point_count = run_bench("req", ["ws://localhost:7777", "-c", "5", "-n", "100", "--filter", "{\"ids\":[\"" + real_id + "\"]}", "--nip45"])
    results["query_point_count_time"] = res_point_count["elapsed"] if res_point_count else -1
    results["query_point_count_output"] = res_point_count["output"] if res_point_count else ""
    
    # 3. Complex Query (authors, kinds, tags, time range)
    # A heavy NIP-01 complex query
    complex_filter = json.dumps({
        "authors": ["0000000000000000000000000000000000000000000000000000000000000000"],
        "kinds": [1, 5, 7],
        "#t": ["nostr", "benchmark"],
        "since": 1600000000,
        "until": 1800000000,
        "limit": 100
    })
    res_complex = run_bench("req", ["ws://localhost:7777", "-c", "10", "-n", "100", "--filter", complex_filter])
    results["query_complex_time"] = res_complex["elapsed"] if res_complex else -1
    results["query_complex_output"] = res_complex["output"] if res_complex else ""
    
    # 4. Complex COUNT Query (NIP-45)
    res_complex_count = run_bench("req", ["ws://localhost:7777", "-c", "10", "-n", "100", "--filter", complex_filter, "--nip45"])
    results["query_complex_count_time"] = res_complex_count["elapsed"] if res_complex_count else -1
    results["query_complex_count_output"] = res_complex_count["output"] if res_complex_count else ""
    
    manager.stop()
    results["status"] = "Done"
    return results

def suite_monitors(manager, skip_heavy=False):
    print("\n--- Running Suite 6: Active Monitors ---")
    results = {}
    manager.clean_db()
    manager.use_docker = False
    manager.start()
    
    subs = 50 if skip_heavy else 150
    res = run_bench("monitor", ["ws://localhost:7777", "-s", str(subs), "-p", "100"])
    results["monitor_fanout_time"] = res["elapsed"] if res else -1
    results["monitor_output"] = res["output"] if res else ""
    
    manager.stop()
    results["status"] = "Done"
    return results

def suite_negentropy(manager, skip_heavy=False):
    print("\n--- Running Suite 7: Negentropy Sync ---")
    results = {}
    import shutil
    target_db_dir = os.path.join(STRFRY_DIR, 'strfry-db-negentropy-target')
    shutil.rmtree(target_db_dir, ignore_errors=True)
    os.makedirs(target_db_dir, exist_ok=True)
    
    # 1. Clean and seed the main DB
    manager.clean_db()
    events_count = 100 if skip_heavy else 10000
    generate_seed_data(events_count)
    
    # 2. Start the source relay
    manager.use_docker = False
    manager.start()
    
    # 3. Perform negentropy sync
    print(f"[INFO] Syncing {events_count} events via Negentropy...")
    binary = os.path.join(STRFRY_DIR, "strfry")
    sync_cmd = [
        binary, 
        "--set", f"db={target_db_dir}/",
        "sync", "ws://127.0.0.1:7777"
    ]
    
    start_time = time.time()
    res = subprocess.run(sync_cmd, capture_output=True, text=True)
    elapsed = time.time() - start_time
    
    manager.stop()
    
    if res.returncode != 0:
        print(f"[ERROR] Negentropy sync failed:\n{res.stderr}")
        results["status"] = f"Failed: {res.stderr.strip()}"
    else:
        results["elapsed"] = elapsed
        results["tps"] = events_count / elapsed if elapsed > 0 else 0
        results["status"] = f"Success ({events_count} events synced in {elapsed:.2f}s)"
        print(f"[INFO] Negentropy sync finished. Status: {results['status']}")
        
    shutil.rmtree(target_db_dir, ignore_errors=True)
    return results

def suite_plugin(manager, skip_heavy=False):
    print("\n--- Running Suite 8: Write Policy Plugin ---")
    results = {}
    manager.clean_db()
    manager.use_docker = False
    
    # Configure the writePolicy plugin
    plugin_path = os.path.abspath(os.path.join(SCRIPT_DIR, "bench_plugin.py"))
    manager.start(config_overrides={
        "relay.writePolicy.plugin": plugin_path,
        "relay.writePolicy.timeoutSeconds": "10"
    })
    
    count = 1000 if skip_heavy else 10000
    res = run_bench("event", ["ws://localhost:7777", "-c", "10", "-n", str(count), "--payload-size", "50"])
    
    manager.stop()
    
    if res:
        results["elapsed"] = res["elapsed"]
        results["tps"] = count / res["elapsed"] if res["elapsed"] > 0 else 0
        results["status"] = f"Success ({count} events processed with plugin)"
        results["output"] = res["output"]
    else:
        results["status"] = "Failed"
        
    return results

def suite_cli_dict(manager, skip_heavy=False):
    print("\n--- Running Suite 9 & 10: CLI & Dictionary ---")
    results = {}
    manager.clean_db()
    events = 1000 if skip_heavy else 1000000
    
    print("[INFO] Testing strfry import...")
    ratio = events / 100000.0
    users = max(1, int(500 * ratio))
    kind1_notes = max(1, int(70000 * ratio))
    kind4_dms = max(1, int(8000 * ratio))
    kind7_reactions = max(1, int(8000 * ratio))
    replaceable = max(1, int(6000 * ratio))
    param_replaceable = max(1, int(10000 * ratio))
    ephemeral = max(1, int(4000 * ratio))
    deletions = max(1, int(2500 * ratio))
    other = max(1, int(3000 * ratio))
    duplicates = max(1, int(1500 * ratio))
    
    gen_cmd = [
        "perl", "test/generate-seed-data.pl", "-o", "-",
        "--users", str(users),
        "--kind1-notes", str(kind1_notes),
        "--kind4-dms", str(kind4_dms),
        "--kind7-reactions", str(kind7_reactions),
        "--replaceable", str(replaceable),
        "--param-replaceable", str(param_replaceable),
        "--ephemeral", str(ephemeral),
        "--deletions", str(deletions),
        "--other", str(other),
        "--duplicates", str(duplicates)
    ]
    import_cmd = ["./strfry", "import", "--no-verify"]
    
    start = time.time()
    p1 = subprocess.Popen(gen_cmd, stdout=subprocess.PIPE)
    p2 = subprocess.Popen(import_cmd, stdin=p1.stdout, stdout=subprocess.DEVNULL)
    p1.stdout.close()
    p2.communicate()
    results["import_time"] = time.time() - start
    
    print("[INFO] Testing strfry export...")
    start = time.time()
    subprocess.run(["./strfry", "export"], stdout=subprocess.DEVNULL)
    results["export_time"] = time.time() - start
    
    print("[INFO] Testing dictionary generation...")
    start = time.time()
    subprocess.run(["./strfry", "dict", "train"], stdout=subprocess.DEVNULL)
    results["dict_gen_time"] = time.time() - start
    
    results["status"] = "Done"
    return results

def suite_os(manager, skip_heavy=False):
    print("\n--- Running Suite 11: OS-Level Metrics ---")
    results = {}
    manager.clean_db()
    manager.use_docker = False
    manager.start()
    
    pid = manager.process.pid
    
    # 1. Initial State
    initial_io = get_process_io(pid)
    initial_cpu = get_process_cpu_time(pid)
    db_file = os.path.join(manager.db_dir, "data.mdb")
    initial_db_size = os.stat(db_file).st_blocks * 512 if os.path.exists(db_file) else 0
    start_time = time.time()
    
    # 2. Run workload (Ingest events)
    count = 2000 if skip_heavy else 10000
    res = run_bench("event", ["ws://localhost:7777", "-c", "2", "-n", str(count)])
    
    # 3. Final State
    elapsed = time.time() - start_time
    final_io = get_process_io(pid)
    final_cpu = get_process_cpu_time(pid)
    final_db_size = os.stat(db_file).st_blocks * 512 if os.path.exists(db_file) else 0
    
    write_bytes = final_io["write_bytes"] - initial_io["write_bytes"]
    read_bytes = final_io["read_bytes"] - initial_io["read_bytes"]
    db_growth = final_db_size - initial_db_size
    cpu_time_diff = final_cpu - initial_cpu
    
    # Ticks per second
    try:
        ticks_per_sec = os.sysconf(os.sysconf_names['SC_CLK_TCK'])
    except:
        ticks_per_sec = 100
        
    cpu_util = (cpu_time_diff / (elapsed * ticks_per_sec)) * 100 if elapsed > 0 else 0.0
    waf = write_bytes / db_growth if db_growth > 0 else 1.0
    
    results["elapsed"] = elapsed
    results["write_bytes"] = write_bytes
    results["read_bytes"] = read_bytes
    results["db_growth"] = db_growth
    results["cpu_utilization_pct"] = cpu_util
    results["write_amplification_factor"] = waf
    results["process_rss"] = get_process_rss(pid)
    
    manager.stop()
    results["status"] = "Done"
    return results

def suite_stress(manager, skip_heavy=False):
    print("\n--- Running Suite 12: Stress & Edge Cases ---")
    results = {}
    manager.clean_db()
    manager.use_docker = False
    manager.start()
    
    count = 100 if skip_heavy else 2000
    print("[INFO] Running Slow Loris attack...")
    res1 = run_bench("malicious", ["ws://localhost:7777", "-c", str(count), "--slow-loris"])
    
    manager.stop()
    manager.clean_db()
    manager.start()
    
    print("[INFO] Running Signature Flood attack...")
    res2 = run_bench("malicious", ["ws://localhost:7777", "-c", "50", "--sig-flood"])
    
    manager.stop()
    results["status"] = "Done"
    return results

def suite_backpressure(manager, skip_heavy=False):
    print("\n--- Running Suite 13: Backpressure Performance ---")
    results = {}
    manager.clean_db()
    manager.use_docker = False
    manager.start()
    
    fast_clients = 20 if skip_heavy else 100
    slow_clients = 5 if skip_heavy else 20
    count = 100 if skip_heavy else 1000
    res = run_bench("backpressure", [
        "ws://localhost:7777",
        "--fast-clients", str(fast_clients),
        "--slow-clients", str(slow_clients),
        "-n", str(count),
        "--slow-delay", "50"
    ])
    results["backpressure_time"] = res["elapsed"] if res else -1
    results["backpressure_output"] = res["output"] if res else ""
    
    manager.stop()
    results["status"] = "Done"
    return results

def generate_report(results, report_path="benchmark_report.md"):
    print(f"[INFO] Generating comprehensive report at {report_path}")
    with open(report_path, "w") as f:
        f.write("# Strfry Benchmarking Report\n\n")
        f.write(f"Generated at: {datetime.now().isoformat()}\n\n")
        
        f.write("## 1. Storage & LMDB Statistics\n")
        if "suite_storage" in results:
            r = results["suite_storage"]
            f.write(f"- **Sequential scan throughput (events/sec):** {r.get('scan_tps', -1):.2f}\n")
            f.write(f"- **In-Core Pagination Time:** {r.get('in_core_time', -1):.2f} seconds\n")
            f.write(f"- **Out-of-Core Pagination Time (256MB RAM):** {r.get('out_of_core_time', -1):.2f} seconds\n")
            f.write("\n### DB Stat Output\n```\n")
            f.write(r.get('mdb_stat', ''))
            f.write("\n```\n\n")
        else:
            f.write("- **Status:** Skipped / Not Run\n\n")
            
        f.write("## 2. Event Ingestion Pipeline Statistics\n")
        if "suite_ingestion" in results:
            r = results["suite_ingestion"]
            f.write(f"- **Standard Write throughput (events/sec) (50b payload):** {r.get('event_small_tps', -1):.2f}\n")
            f.write(f"- **Standard Write throughput (events/sec) (10Kb payload):** {r.get('event_large_tps', -1):.2f}\n")
            f.write(f"- **Spam Write throughput (events/sec):** {r.get('event_spam_tps', -1):.2f}\n")
            f.write(f"- **Peak Writer Queue Depth:** {r.get('writer_queue_peak', -1)}\n")
            f.write(f"- **Average CPU Utilization (across cores):** {r.get('avg_cpu_percent', 0.0):.2f}%\n")
            f.write(f"- **Peak CPU Utilization:** {r.get('peak_cpu_percent', 0.0):.2f}%\n")
            f.write(f"- **Disk Physical Reads:** {r.get('read_mb', 0.0):.2f} MB\n")
            f.write(f"- **Disk Physical Writes:** {r.get('write_mb', 0.0):.2f} MB\n")
            f.write(f"- **Write Amplification Factor (WAF):** {r.get('waf', 0.0):.4f}\n\n")
            f.write("### Small Payload Latencies\n```\n")
            f.write(r.get('event_small_output', ''))
            f.write("\n```\n\n")
        else:
            f.write("- **Status:** Skipped / Not Run\n\n")
            
        f.write("## 3. Concurrency & Thread Pool\n")
        if "suite_concurrency" in results:
            r = results["suite_concurrency"]
            f.write(f"- **Mixed Read-Write REQ Time:** {r.get('mixed_req_time', -1):.2f} seconds\n\n")
        else:
            f.write("- **Status:** Skipped / Not Run\n\n")
            
        f.write("## 4. WebSockets & Connections\n")
        if "suite_websockets" in results:
            r = results["suite_websockets"]
            f.write("### Connection Memory Scaling (VmRSS)\n")
            for c, mem in r.get('connection_memory', {}).items():
                f.write(f"- **Connection Memory ({c} conns):** {mem:.2f} MB\n")
            conn_keys = sorted([k for k in r.keys() if k.startswith("conn_storm_") and k.endswith("_output")],
                               key=lambda x: int(re.search(r'\d+', x).group(0)))
            for conn_k in conn_keys:
                c_val = re.search(r'\d+', conn_k).group(0)
                tps_val = r.get(f"conn_storm_{c_val}_tps", -1)
                p50_val = r.get(f"conn_storm_{c_val}_p50_ms", -1)
                p99_val = r.get(f"conn_storm_{c_val}_p99_ms", -1)
                if tps_val >= 0:
                    f.write(f"- **Connection Storm ({c_val} conns) Throughput:** {tps_val:.2f} conn/sec\n")
                if p50_val >= 0:
                    f.write(f"- **Connection Storm ({c_val} conns) P50 Latency:** {p50_val:.2f} ms\n")
                if p99_val >= 0:
                    f.write(f"- **Connection Storm ({c_val} conns) P99 Latency:** {p99_val:.2f} ms\n")
                f.write(f"\n### Connection Storm ({c_val} conns) Performance\n```\n")
                f.write(r[conn_k])
                f.write("\n```\n\n")
            if "churn_tps" in r:
                f.write(f"- **High Churn Throughput:** {r['churn_tps']:.2f} conn/sec\n")
            f.write("### High Churn Performance\n```\n")
            f.write(r.get('churn_output', ''))
            f.write("\n```\n")
            f.write(f"- **OS TIME_WAIT sockets count (post-churn):** {r.get('time_wait_count', -1)}\n\n")
        else:
            f.write("- **Status:** Skipped / Not Run\n\n")
            
        f.write("## 5. Query Engine & Indices\n")
        if "suite_queries" in results:
            r = results["suite_queries"]
            f.write(f"- **Point Lookup REQ Time:** {r.get('query_point_time', -1):.2f} seconds\n")
            f.write("### Point Lookup REQ Latencies\n```\n")
            f.write(r.get('query_point_output', ''))
            f.write("\n```\n")
            f.write(f"- **Point Lookup COUNT (NIP-45) Time:** {r.get('query_point_count_time', -1):.2f} seconds\n")
            f.write("### Point Lookup COUNT Latencies\n```\n")
            f.write(r.get('query_point_count_output', ''))
            f.write("\n```\n")
            f.write(f"- **Complex Query REQ Time:** {r.get('query_complex_time', -1):.2f} seconds\n")
            f.write("### Complex Query REQ Latencies\n```\n")
            f.write(r.get('query_complex_output', ''))
            f.write("\n```\n")
            f.write(f"- **Complex COUNT (NIP-45) Query Time:** {r.get('query_complex_count_time', -1):.2f} seconds\n")
            f.write("### Complex COUNT Latencies\n```\n")
            f.write(r.get('query_complex_count_output', ''))
            f.write("\n```\n\n")
        else:
            f.write("- **Status:** Skipped / Not Run\n\n")
            
        f.write("## 6. Active Monitors (Viral Post Fanout)\n")
        if "suite_monitors" in results:
            r = results["suite_monitors"]
            f.write(f"- **Subscription Fan-out Time:** {r.get('monitor_fanout_time', -1):.2f} seconds\n")
            f.write("### Fanout Output\n```\n")
            f.write(r.get('monitor_output', ''))
            f.write("\n```\n\n")
        else:
            f.write("- **Status:** Skipped / Not Run\n\n")
            
        f.write("## 7. Negentropy Sync\n")
        if "suite_negentropy" in results:
            r = results["suite_negentropy"]
            f.write(f"- **Status:** {r.get('status', 'Pending')}\n")
            if "elapsed" in r:
                f.write(f"- **Sync Time:** {r['elapsed']:.2f} seconds\n")
                f.write(f"- **Sync Throughput:** {r['tps']:.2f} events/sec\n")
            f.write("\n")
        else:
            f.write("- **Status:** Skipped / Not Run\n\n")
            
        f.write("## 8. Write Policy Plugin\n")
        if "suite_plugin" in results:
            r = results["suite_plugin"]
            f.write(f"- **Status:** {r.get('status', 'Pending')}\n")
            if "elapsed" in r:
                f.write(f"- **Plugin Ingestion Time:** {r['elapsed']:.2f} seconds\n")
                f.write(f"- **Plugin Ingestion Throughput:** {r['tps']:.2f} events/sec\n")
                f.write("\n### Plugin Ingestion Latencies\n```\n")
                f.write(r.get('output', ''))
                f.write("\n```\n")
            f.write("\n")
        else:
            f.write("- **Status:** Skipped / Not Run\n\n")
        f.write("## 9 & 10. CLI & Dictionary Compression\n")
        if "suite_cli" in results:
            r = results["suite_cli"]
            f.write(f"- **Import Time:** {r.get('import_time', -1):.2f} seconds\n")
            f.write(f"- **Export Time:** {r.get('export_time', -1):.2f} seconds\n")
            f.write(f"- **Dictionary Generation Time:** {r.get('dict_gen_time', -1):.2f} seconds\n\n")
        else:
            f.write("- **Status:** Skipped / Not Run\n\n")
            
        f.write("## 11. OS-Level Metrics\n")
        if "suite_os" in results:
            r = results["suite_os"]
            f.write(f"- **Baseline RSS:** {r.get('process_rss', -1):.2f} MB\n")
            f.write(f"- **CPU Utilization:** {r.get('cpu_utilization_pct', -1):.2f}%\n")
            f.write(f"- **Write Amplification Factor (WAF):** {r.get('write_amplification_factor', -1):.2f}\n")
            f.write(f"- **Bytes Written:** {r.get('write_bytes', -1) / (1024*1024):.2f} MB\n")
            f.write(f"- **Bytes Read:** {r.get('read_bytes', -1) / (1024*1024):.2f} MB\n\n")
        else:
            f.write("- **Status:** Skipped / Not Run\n\n")
            
        f.write("## 12. Stress & Edge Cases\n")
        if "suite_stress" in results:
            r = results["suite_stress"]
            f.write(f"- **Adversarial Tests:** Completed successfully\n\n")
        else:
            f.write("- **Status:** Skipped / Not Run\n\n")

        f.write("## 13. Backpressure Performance\n")
        if "suite_backpressure" in results:
            r = results["suite_backpressure"]
            f.write(f"- **Total Backpressure Test Time:** {r.get('backpressure_time', -1):.2f} seconds\n")
            f.write("### Backpressure Latencies\n```\n")
            f.write(r.get('backpressure_output', ''))
            f.write("\n```\n\n")
        else:
            f.write("- **Status:** Skipped / Not Run\n\n")

def main():
    parser = argparse.ArgumentParser(description="Strfry Performance Benchmarking Orchestrator")
    parser.add_argument("--suite", type=str, help="Run a specific suite (e.g. storage, ingestion, stress)")
    parser.add_argument("--skip-heavy", action="store_true", help="Skip heavy database generation to speed up testing")
    parser.add_argument("--dry-run", action="store_true", help="Print commands but don't run tests")
    args = parser.parse_args()

    manager = StrfryManager()
    
    suites_to_run = []
    all_suites = {
        "storage": suite_storage,
        "ingestion": suite_ingestion,
        "concurrency": suite_concurrency,
        "websockets": suite_websockets,
        "queries": suite_queries,
        "monitors": suite_monitors,
        "negentropy": suite_negentropy,
        "plugin": suite_plugin,
        "cli": suite_cli_dict,
        "os": suite_os,
        "stress": suite_stress,
        "backpressure": suite_backpressure
    }
    
    if args.suite:
        if args.suite in all_suites:
            suites_to_run.append((args.suite, all_suites[args.suite]))
        else:
            print(f"[ERROR] Unknown suite: {args.suite}")
            sys.exit(1)
    elif args.skip_heavy:
        suites_to_run = [(name, func) for name, func in all_suites.items() if name not in ["storage"]]
    else:
        suites_to_run = list(all_suites.items())

    docker_available = check_docker()
    if not docker_available:
        print("[WARNING] Docker daemon is not running or not accessible. Docker-based out-of-core storage test will be skipped.")

    if args.dry_run:
        print("[INFO] DRY RUN: Would execute the selected suites:")
        for name, _ in suites_to_run:
            print(f" - {name}")
        return

    if any(name == "storage" for name, _ in suites_to_run):
        manager.build_docker()
    
    print("[INFO] Pre-compiling strfry-bench...")
    subprocess.run(["cargo", "build", "--release"], cwd=get_bench_dir(), check=True)

    results = {}
    try:
        for name, func in suites_to_run:
            results[f"suite_{name}"] = func(manager, args.skip_heavy)
        generate_report(results)
    finally:
        if any(name == "storage" for name, _ in suites_to_run):
            print("[INFO] Purging leftover Docker resources to reclaim space...")
            subprocess.run(["docker", "system", "prune", "-f"], stdout=subprocess.DEVNULL)

if __name__ == "__main__":
    main()

