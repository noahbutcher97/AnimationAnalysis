"""Summarize retained host samples without inferring contact or universal speedups."""
import argparse
from collections import Counter, defaultdict
import hashlib
import json
import math
from pathlib import Path


def quantiles(values):
    ordered = sorted(values)
    if not ordered or any(not math.isfinite(x) or x < 0 for x in ordered):
        raise ValueError('Timing samples must be nonnegative and finite')
    return {f'p{p}': ordered[max(0, math.ceil(len(ordered) * p / 100) - 1)] for p in (50, 95, 99)}


def summarize(manifest, *, repetitions=3, samples=120):
    groups = defaultdict(list)
    for row in manifest['samples']:
        groups[row['mode'], row['repetition']].append(row)
    modes = sorted({mode for mode, _ in groups})
    if not modes:
        raise ValueError('No samples')
    result = []
    eligible = manifest.get('comparison_eligible', True)
    for mode in modes:
        if {rep for m, rep in groups if m == mode} != set(range(repetitions)):
            raise ValueError(f'Incomplete repetition coverage: {mode}')
        for repetition in range(repetitions):
            rows = groups[mode, repetition]
            if len(rows) != samples or sorted(row['sample'] for row in rows) != list(range(samples)):
                raise ValueError(f'Incomplete or duplicated sample coverage: {mode}/{repetition}')
            metrics = {}
            outcomes = Counter(row.get('terminal_status', 'completed') for row in rows)
            if any(status != 'completed' for status in outcomes):
                eligible = False
            for key in ('frame_wall_s', 'acquisition_wall_s', 'depth_readback_wall_s', 'completion_latency_s', 'decode_wall_s'):
                if any(key in row for row in rows):
                    measured = rows if key in ('frame_wall_s', 'acquisition_wall_s') else [
                        row for row in rows if row.get('terminal_status', 'completed') == 'completed']
                    if measured:
                        metrics[key.replace('_s', '_ms')] = quantiles([row[key] * 1000 for row in measured])
            item = dict(mode=mode, repetition=repetition, sample_count=len(rows), outcomes=dict(outcomes), metrics=metrics)
            for key in ('pending_requests', 'reserved_bytes', 'peak_bytes', 'rejected', 'cancelled', 'failed', 'timeouts'):
                if any(key in row for row in rows):
                    item[key + '_max'] = max(row[key] for row in rows)
            result.append(item)
    return dict(status='measurements_recorded', comparison_eligible=eligible, percentile_method='nearest_rank', runs=result,
                policy={key: value for key, value in manifest.items() if key != 'samples'},
                limits='Workstation and declared fixture only. Readback samples exclude encoding and file export; frame times include engine scheduling. No universal speedup inferred.')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('input', type=Path)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--repetitions', type=int, default=3)
    parser.add_argument('--samples', type=int, default=120)
    args = parser.parse_args()
    raw = args.input.read_bytes()
    result = summarize(json.loads(raw.decode('utf-8-sig')), repetitions=args.repetitions, samples=args.samples)
    result['input_sha256'] = hashlib.sha256(raw).hexdigest()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
