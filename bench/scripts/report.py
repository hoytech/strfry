#!/usr/bin/env python3
import sys, json, os, glob

def load_json(path, default=None):
    try:
        with open(path) as f:
            return json.load(f)
    except Exception:
        return default

def summarize_run(run_dir):
    sysinfo = load_json(os.path.join(run_dir, 'sysinfo.json'), {})
    client = load_json(os.path.join(run_dir, 'client.json'), {})
    # TODO: parse server.log for scan metrics and latencies when available
    return {
        'dir': os.path.basename(run_dir.rstrip('/')),
        'sys': sysinfo,
        'client': client,
    }

def main():
    if len(sys.argv) < 2:
        print("Usage: report.py <runs_dir...>", file=sys.stderr)
        sys.exit(1)

    runs = []
    for arg in sys.argv[1:]:
        paths = glob.glob(arg)
        for p in paths:
            if os.path.isdir(p):
                runs.append(summarize_run(p))

    # Emit a simple placeholder Markdown
    print("| Run | Kernel | CPU | Mem (KB) | Notes |")
    print("|:----|:-------|:----|--------:|:------|")
    for r in runs:
        sysinfo = r.get('sys', {})
        cpu_model = sysinfo.get('cpu', {}).get('Model name', '')
        print(f"| {r['dir']} | {sysinfo.get('kernel','')} | {cpu_model} | {sysinfo.get('memory_kb',0)} | - |")

if __name__ == '__main__':
    main()

