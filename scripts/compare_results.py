#!/usr/bin/env python3

import sys
import json


def load_results(path: str) -> list:
    with open(path) as f:
        return json.load(f)


def extract_event_ids(results: list) -> dict:
    id_sets = {}
    for i, result in enumerate(results):
        filter_key = json.dumps(result.get("filter", {}))
        event_ids = set(result.get("event_ids", []))
        id_sets[filter_key] = event_ids
    return id_sets


def compare(baseline_results: list, patched_results: list) -> bool:
    baseline_ids = extract_event_ids(baseline_results)
    patched_ids = extract_event_ids(patched_results)
    
    all_match = True
    for filter_key in baseline_ids:
        baseline_set = baseline_ids.get(filter_key, set())
        patched_set = patched_ids.get(filter_key, set())
        filter_obj = json.loads(filter_key)
        
        if baseline_set != patched_set:
            all_match = False
            missing = baseline_set - patched_set
            extra = patched_set - baseline_set
            print(f"Mismatch: filter {filter_obj}")
            if missing:
                print(f"  Missing: {len(missing)}")
            if extra:
                print(f"  Extra: {len(extra)}")
    
    return all_match


def main():
    if len(sys.argv) != 3:
        return
    baseline = load_results(sys.argv[1])
    patched = load_results(sys.argv[2])
    sys.exit(0 if compare(baseline, patched) else 1)


if __name__ == "__main__":
    main()
