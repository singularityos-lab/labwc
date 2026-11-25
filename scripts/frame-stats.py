#!/usr/bin/env python3
import sys


def pct(v, p):
    if not v:
        return 0.0
    v = sorted(v)
    return v[min(len(v) - 1, int(round(p / 100 * (len(v) - 1))))]


def load(path):
    commits, presents = [], []
    for line in open(path):
        f = line.split()
        if not f:
            continue
        if f[0] == "C" and len(f) >= 5:
            commits.append((int(f[2]), int(f[3]), int(f[4])))
        elif f[0] == "P" and len(f) >= 6 and f[5] == "1":
            presents.append((int(f[2]), int(f[3]), int(f[4])))
    return commits, presents


def report(name, commits, presents, period_ms):
    render = [(e - s) / 1e6 for s, e, _ in commits]
    seqs = {seq for _, _, seq in commits}
    shown = sorted(p for p in presents if p[2] in seqs)
    intervals, missed = [], 0
    for (t0, r0, _), (t1, _, _) in zip(shown, shown[1:]):
        gap = (t1 - t0) / 1e6
        period = r0 / 1e6 if r0 > 0 else period_ms
        if gap > 4 * period:
            continue
        intervals.append(gap)
        if gap > 1.5 * period:
            missed += int(round(gap / period)) - 1
    secs = (commits[-1][0] - commits[0][0]) / 1e9 if len(commits) > 1 else 0
    print(f"{name:10s} frames {len(commits):5d}  render p50 {pct(render, 50):5.2f} p99 {pct(render, 99):5.2f} max {max(render or [0]):6.2f} ms"
          f"  present interval p50 {pct(intervals, 50):5.2f} p99 {pct(intervals, 99):5.2f} max {max(intervals or [0]):6.2f} ms  missed {missed}"
          + (f"  ({len(commits) / secs:.1f} fps over {secs:.0f}s)" if secs else ""))


def main():
    commits, presents = load(sys.argv[1])
    period_ms = 1000 / 60
    if len(sys.argv) > 2:
        marks = {}
        for line in open(sys.argv[2]):
            label, t = line.split()
            marks[label] = int(t)
        for label in [m[:-6] for m in marks if m.endswith("-start")]:
            start, end = marks[label + "-start"], marks.get(label + "-end")
            if end is None:
                continue
            report(label, [c for c in commits if start <= c[0] <= end], [p for p in presents if start <= p[0] <= end + 50_000_000], period_ms)
    else:
        report("all", commits, presents, period_ms)


if __name__ == "__main__":
    main()
