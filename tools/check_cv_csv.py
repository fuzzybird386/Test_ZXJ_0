"""Verify time-ordered CV points exported by the unchanged APP, then split cycles.

The APP writes cycle=1 for an entire acquisition; never trust that column as
the physical cycle number. No sorting, interpolation or invented samples.
"""
import argparse
import csv
import io
import json
from pathlib import Path


def expected_legs(start, vertex1, vertex2, end, step, cycles):
    potential = start
    points = [(potential, 1, 'initial')]
    segments = []
    for cycle in range(1, cycles + 1):
        for name, target in (('leg1', vertex1), ('leg2', vertex2)):
            begin = len(points) - 1
            direction = 'increasing' if target > potential else 'decreasing' if target < potential else 'flat'
            while potential != target:
                delta = min(abs(target - potential), step)
                potential += delta if target > potential else -delta
                points.append((potential, cycle, name))
            segments.append({'cycle': cycle, 'leg': name, 'direction': direction,
                             'first_index': begin, 'last_index': len(points) - 1})
    if potential != end:
        while potential != end:
            delta = min(abs(end - potential), step)
            potential += delta if end > potential else -delta
            points.append((potential, cycles, 'final'))
    return points, segments


def analyze(path, defaults):
    text = path.read_text(encoding='utf-8-sig')
    params = {}
    for line in text.splitlines():
        if line.startswith('# params='):
            params = dict(item.split('=', 1) for item in line[9:].split(',') if '=' in item)
    names = ('startPotential', 'vertexPotential', 'vertex2Potential', 'endPotential', 'stepPotential', 'cycles')
    values = [int(params.get(key, default)) for key, default in zip(names, defaults)]
    if values[4] <= 0 or not 1 <= values[5] <= 65535:
        raise ValueError('Invalid step or cycle count')
    rows = list(csv.DictReader(io.StringIO('\n'.join(line for line in text.splitlines() if not line.startswith('#')))))
    expected, segments = expected_legs(*values)
    if len(rows) != len(expected):
        raise ValueError(f'Incomplete or unexpected data: got {len(rows)} points, expected {len(expected)}. '
                         'Do not infer cycle count from the APP Scan 1 label.')
    for i, (row, (mv, _, _)) in enumerate(zip(rows, expected)):
        # APP exports Float values via truncation: allow 1mV representation loss.
        if abs(float(row['potential_mV']) - mv) > 1.01:
            raise ValueError(f'Order/gap mismatch at point {i}: got {row["potential_mV"]}mV, expected {mv}mV')
        float(row['current_nA'])  # verify the current column, do not alter sign
    return rows, expected, segments


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('csv', type=Path, help='CV CSV saved/exported from original APP')
    parser.add_argument('--out', type=Path, help='New directory for split CSVs and order report')
    parser.add_argument('--start', type=int, default=-600)
    parser.add_argument('--vertex1', type=int, default=1000)
    parser.add_argument('--vertex2', type=int, default=-600)
    parser.add_argument('--end', type=int, default=-600)
    parser.add_argument('--step', type=int, default=5)
    parser.add_argument('--cycles', type=int, default=4)
    args = parser.parse_args()
    defaults = (args.start, args.vertex1, args.vertex2, args.end, args.step, args.cycles)
    try:
        rows, expected, segments = analyze(args.csv, defaults)
    except (ValueError, KeyError, OSError) as error:
        parser.exit(1, f'FAIL: {error}\n')
    report = {'source': str(args.csv.resolve()), 'points': len(rows), 'order_matches': True,
              'cycles': max(p[1] for p in expected), 'legs': segments,
              'note': 'Verifies point count and commanded-potential pattern only. APP CSV has no BLE seq/timestamps: identical-cycle permutations or a symmetric whole-trace reversal cannot be detected. Does not prove electrode voltage or peak chemistry.'}
    if args.out:
        args.out.mkdir(parents=True, exist_ok=False)
        for cycle in range(1, report['cycles'] + 1):
            legs = [leg for leg in segments if leg['cycle'] == cycle]
            # Share the actual boundary sample between adjacent cycle files.
            begin, end = legs[0]['first_index'], legs[-1]['last_index']
            with (args.out / f'cycle_{cycle}.csv').open('w', newline='', encoding='utf-8') as f:
                writer = csv.writer(f)
                writer.writerow(['sample_index', 'potential_mV', 'current_nA', 'physical_cycle'])
                for i in range(begin, end + 1):
                    writer.writerow([i, rows[i]['potential_mV'], rows[i]['current_nA'], cycle])
        (args.out / 'report.json').write_text(json.dumps(report, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    print(json.dumps(report, ensure_ascii=False, indent=2))


if __name__ == '__main__':
    main()
